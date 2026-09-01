#include "YouTubeResolver.h"

#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cwctype>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr size_t kMaximumInputCharacters = 2048;
constexpr size_t kMaximumOutputBytes = 16 * 1024;
constexpr size_t kMaximumCapturedBytes = 64 * 1024;

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) reset(other.release());
        return *this;
    }

    HANDLE get() const { return handle_; }
    HANDLE release()
    {
        const HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }
    void reset(HANDLE value = nullptr)
    {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = value;
    }
    explicit operator bool() const
    {
        return handle_ && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_{nullptr};
};

std::filesystem::path module_directory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(std::move(path)).parent_path();
}

std::filesystem::path final_normalized_path(HANDLE handle)
{
    DWORD required = GetFinalPathNameByHandleW(
        handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0) return {};
    std::wstring value(required, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(
        handle, value.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written >= required) return {};
    value.resize(written);
    return std::filesystem::path(std::move(value)).lexically_normal();
}

bool same_path_case_insensitive(const std::filesystem::path& left,
                                const std::filesystem::path& right)
{
    const std::wstring leftValue = left.lexically_normal().wstring();
    const std::wstring rightValue = right.lexically_normal().wstring();
    if (leftValue.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        rightValue.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(
               leftValue.data(), static_cast<int>(leftValue.size()),
               rightValue.data(), static_cast<int>(rightValue.size()), TRUE) == CSTR_EQUAL;
}

struct VerifiedHelpers {
    UniqueHandle directory;
    UniqueHandle ytDlp;
    UniqueHandle deno;
    UniqueHandle cacheDirectory;
    std::filesystem::path directoryPath;
    std::filesystem::path ytDlpPath;
    std::filesystem::path cacheDirectoryPath;
};

bool create_verified_package_cache(const std::filesystem::path& packageDirectory,
                                   UniqueHandle& heldHandle,
                                   std::filesystem::path& canonicalPath)
{
    if (!packageDirectory.is_absolute()) return false;
    const std::filesystem::path requestedPath = packageDirectory / L"youtube-helper-cache";
    if (!CreateDirectoryW(requestedPath.c_str(), nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) return false;
    }

    UniqueHandle candidate(CreateFileW(
        requestedPath.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!candidate || GetFileType(candidate.get()) != FILE_TYPE_DISK) return false;

    FILE_ATTRIBUTE_TAG_INFO tagInfo{};
    if (!GetFileInformationByHandleEx(candidate.get(), FileAttributeTagInfo,
                                      &tagInfo, sizeof(tagInfo)) ||
        (tagInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        tagInfo.ReparseTag != 0) {
        return false;
    }

    canonicalPath = final_normalized_path(candidate.get());
    if (canonicalPath.empty() ||
        !same_path_case_insensitive(canonicalPath.parent_path(), packageDirectory) ||
        !same_path_case_insensitive(canonicalPath.filename(), L"youtube-helper-cache")) {
        return false;
    }
    heldHandle = std::move(candidate);
    return true;
}

bool environment_entry_is_named(std::wstring_view entry, std::wstring_view name)
{
    const size_t equals = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
    if (equals == std::wstring_view::npos || equals != name.size()) return false;
    return CompareStringOrdinal(entry.data(), static_cast<int>(equals), name.data(),
                                static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
}

std::optional<std::vector<wchar_t>> child_environment_with_package_cache(
    const std::filesystem::path& cacheDirectory)
{
    if (!cacheDirectory.is_absolute() || cacheDirectory.wstring().find(L'\0') != std::wstring::npos) {
        return std::nullopt;
    }
    LPWCH rawEnvironment = GetEnvironmentStringsW();
    if (!rawEnvironment) return std::nullopt;
    const auto freeEnvironment = [](LPWCH value) {
        if (value) FreeEnvironmentStringsW(value);
    };
    const std::unique_ptr<wchar_t, decltype(freeEnvironment)> environment(
        rawEnvironment, freeEnvironment);

    std::vector<std::wstring> entries;
    for (const wchar_t* cursor = rawEnvironment; *cursor != L'\0';) {
        const std::wstring_view entry(cursor);
        if (!environment_entry_is_named(entry, L"DENO_DIR")) entries.emplace_back(entry);
        cursor += entry.size() + 1;
    }
    entries.emplace_back(L"DENO_DIR=" + cacheDirectory.wstring());
    std::sort(entries.begin(), entries.end(), [](const std::wstring& left,
                                                  const std::wstring& right) {
        const int insensitive = CompareStringOrdinal(
            left.data(), static_cast<int>(left.size()), right.data(),
            static_cast<int>(right.size()), TRUE);
        if (insensitive != CSTR_EQUAL) return insensitive == CSTR_LESS_THAN;
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                    static_cast<int>(right.size()), FALSE) == CSTR_LESS_THAN;
    });

    size_t characters = 1;
    for (const std::wstring& entry : entries) {
        if (entry.find(L'\0') != std::wstring::npos ||
            entry.size() > std::numeric_limits<size_t>::max() - characters - 1) {
            return std::nullopt;
        }
        characters += entry.size() + 1;
    }
    std::vector<wchar_t> result;
    result.reserve(characters);
    for (const std::wstring& entry : entries) {
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

bool open_verified_helper(const std::filesystem::path& requestedPath,
                          const std::filesystem::path& canonicalDirectory,
                          UniqueHandle& heldHandle,
                          std::filesystem::path& canonicalPath)
{
    UniqueHandle candidate(CreateFileW(
        requestedPath.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES | FILE_EXECUTE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!candidate) return false;
    if (GetFileType(candidate.get()) != FILE_TYPE_DISK) return false;

    FILE_ATTRIBUTE_TAG_INFO tagInfo{};
    if (!GetFileInformationByHandleEx(candidate.get(), FileAttributeTagInfo,
                                      &tagInfo, sizeof(tagInfo)) ||
        (tagInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        tagInfo.ReparseTag != 0) {
        return false;
    }

    canonicalPath = final_normalized_path(candidate.get());
    if (canonicalPath.empty() ||
        !same_path_case_insensitive(canonicalPath.parent_path(), canonicalDirectory)) {
        return false;
    }
    heldHandle = std::move(candidate);
    return true;
}

bool verify_beside_app_helpers(const std::filesystem::path& requestedDirectory,
                               VerifiedHelpers& verified)
{
    if (!requestedDirectory.is_absolute()) return false;
    UniqueHandle directory(CreateFileW(
        requestedDirectory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!directory || GetFileType(directory.get()) != FILE_TYPE_DISK) return false;
    FILE_ATTRIBUTE_TAG_INFO directoryInfo{};
    if (!GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo,
                                      &directoryInfo, sizeof(directoryInfo)) ||
        (directoryInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }
    const std::filesystem::path canonicalDirectory =
        final_normalized_path(directory.get());
    if (canonicalDirectory.empty()) return false;

    UniqueHandle ytDlp;
    UniqueHandle deno;
    std::filesystem::path canonicalYtDlp;
    std::filesystem::path canonicalDeno;
    if (!open_verified_helper(requestedDirectory / L"yt-dlp.exe", canonicalDirectory,
                              ytDlp, canonicalYtDlp) ||
        !open_verified_helper(requestedDirectory / L"deno.exe", canonicalDirectory,
                              deno, canonicalDeno)) {
        return false;
    }

    UniqueHandle cacheDirectory;
    std::filesystem::path canonicalCacheDirectory;
    if (!create_verified_package_cache(canonicalDirectory, cacheDirectory,
                                       canonicalCacheDirectory)) {
        return false;
    }

    verified.directory = std::move(directory);
    verified.ytDlp = std::move(ytDlp);
    verified.deno = std::move(deno);
    verified.cacheDirectory = std::move(cacheDirectory);
    verified.directoryPath = canonicalDirectory;
    verified.ytDlpPath = canonicalYtDlp;
    verified.cacheDirectoryPath = canonicalCacheDirectory;
    return true;
}

ResolveResult resolver_error(ResolveError error, std::wstring detail)
{
    ResolveResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

struct CrackedUrl {
    INTERNET_SCHEME scheme{0};
    std::wstring host;
    std::wstring path;
    std::wstring extra;
    bool hasUserInfo{false};
};

bool equals_case_insensitive(std::wstring_view left, std::wstring_view right)
{
    if (left.size() != right.size()) return false;
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool has_dot_bound_suffix(std::wstring_view host, std::wstring_view suffix)
{
    if (equals_case_insensitive(host, suffix)) return true;
    if (host.size() <= suffix.size()) return false;
    const size_t suffixOffset = host.size() - suffix.size();
    return host[suffixOffset - 1] == L'.' &&
           equals_case_insensitive(host.substr(suffixOffset), suffix);
}

bool crack_url(std::wstring_view value, CrackedUrl& result)
{
    if (value.empty() || value.size() > static_cast<size_t>(std::numeric_limits<DWORD>::max())) {
        return false;
    }

    std::wstring host(value.size() + 1, L'\0');
    std::wstring path(value.size() + 1, L'\0');
    std::wstring extra(value.size() + 1, L'\0');
    std::wstring user(value.size() + 1, L'\0');
    std::wstring password(value.size() + 1, L'\0');

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.lpszHostName = host.data();
    components.dwHostNameLength = static_cast<DWORD>(host.size());
    components.lpszUrlPath = path.data();
    components.dwUrlPathLength = static_cast<DWORD>(path.size());
    components.lpszExtraInfo = extra.data();
    components.dwExtraInfoLength = static_cast<DWORD>(extra.size());
    components.lpszUserName = user.data();
    components.dwUserNameLength = static_cast<DWORD>(user.size());
    components.lpszPassword = password.data();
    components.dwPasswordLength = static_cast<DWORD>(password.size());

    if (!WinHttpCrackUrl(value.data(), static_cast<DWORD>(value.size()), 0, &components)) {
        return false;
    }

    host.resize(components.dwHostNameLength);
    path.resize(components.dwUrlPathLength);
    extra.resize(components.dwExtraInfoLength);
    result.scheme = components.nScheme;
    result.host = std::move(host);
    result.path = std::move(path);
    result.extra = std::move(extra);
    result.hasUserInfo = components.dwUserNameLength != 0 || components.dwPasswordLength != 0;
    return true;
}

bool is_video_id(std::wstring_view value)
{
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](wchar_t character) {
        return (character >= L'a' && character <= L'z') ||
               (character >= L'A' && character <= L'Z') ||
               (character >= L'0' && character <= L'9') ||
               character == L'-' || character == L'_';
    });
}

std::wstring_view query_part(std::wstring_view extra)
{
    if (extra.empty() || extra.front() != L'?') return {};
    extra.remove_prefix(1);
    const size_t fragment = extra.find(L'#');
    if (fragment != std::wstring_view::npos) extra = extra.substr(0, fragment);
    return extra;
}

bool query_has_video_id(std::wstring_view query)
{
    bool foundVideoId = false;
    while (!query.empty()) {
        const size_t separator = query.find(L'&');
        const std::wstring_view item = query.substr(0, separator);
        const size_t equals = item.find(L'=');
        const std::wstring_view name = item.substr(0, equals);
        if (name.find(L'%') != std::wstring_view::npos) return false;
        if (equals_case_insensitive(name, L"v")) {
            if (name != L"v" || equals == std::wstring_view::npos || foundVideoId ||
                !is_video_id(item.substr(equals + 1))) {
                return false;
            }
            foundVideoId = true;
        }
        if (separator == std::wstring_view::npos) break;
        query.remove_prefix(separator + 1);
    }
    return foundVideoId;
}

bool has_forbidden_input_character(std::wstring_view value)
{
    return std::any_of(value.begin(), value.end(), [](wchar_t character) {
        return character <= 0x1f || character == 0x7f || character == L'\'' ||
               character == L'"' || character == L'\\' || std::iswspace(character) != 0;
    });
}

std::wstring utf8_to_wide(std::string_view value)
{
    if (value.empty() || value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring converted(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), converted.data(), count) != count) {
        return {};
    }
    return converted;
}

ResolveResult invalid_output()
{
    ResolveResult result;
    result.error = ResolveError::InvalidOutput;
    result.detail = L"Resolver returned invalid output.";
    return result;
}

} // namespace

bool IsSupportedYouTubeUrl(std::wstring_view value)
{
    if (value.empty() || value.size() > kMaximumInputCharacters ||
        has_forbidden_input_character(value)) {
        return false;
    }

    CrackedUrl url;
    if (!crack_url(value, url) || url.scheme != INTERNET_SCHEME_HTTPS ||
        url.hasUserInfo || !has_dot_bound_suffix(url.host, L"youtube.com") &&
                                !equals_case_insensitive(url.host, L"youtu.be")) {
        return false;
    }

    if (equals_case_insensitive(url.host, L"youtu.be")) {
        if (url.path.size() <= 1 || url.path.front() != L'/') return false;
        return is_video_id(std::wstring_view(url.path).substr(1));
    }

    if (url.path == L"/watch") return query_has_video_id(query_part(url.extra));
    constexpr std::wstring_view shortsPrefix = L"/shorts/";
    if (url.path.starts_with(shortsPrefix)) {
        return is_video_id(std::wstring_view(url.path).substr(shortsPrefix.size()));
    }
    return false;
}

std::wstring_view YouTubeResolveErrorMessageKey(ResolveError error)
{
    switch (error) {
    case ResolveError::InvalidUrl: return L"youtube.error.invalid";
    case ResolveError::HelperMissing: return L"youtube.error.helper_missing";
    case ResolveError::StartFailed: return L"youtube.error.start_failed";
    case ResolveError::TimedOut: return L"youtube.error.timeout";
    case ResolveError::Cancelled: return L"youtube.error.cancelled";
    case ResolveError::OutputTooLarge:
    case ResolveError::ExtractionFailed:
    case ResolveError::InvalidOutput:
        return L"youtube.error.extraction";
    case ResolveError::None:
        return L"youtube.error.extraction";
    }
    return L"youtube.error.extraction";
}

ResolveResult ParseResolverOutput(std::string_view stdoutBytes, DWORD exitCode)
{
    if (exitCode != 0) {
        ResolveResult result;
        result.error = ResolveError::ExtractionFailed;
        result.detail = L"Could not extract a playable YouTube stream.";
        return result;
    }

    if (stdoutBytes.empty() || stdoutBytes.size() > kMaximumOutputBytes) {
        return invalid_output();
    }
    if (stdoutBytes.ends_with("\r\n")) {
        stdoutBytes.remove_suffix(2);
    } else if (stdoutBytes.ends_with("\n")) {
        stdoutBytes.remove_suffix(1);
    }
    if (stdoutBytes.empty() ||
        stdoutBytes.find_first_of("\r\n") != std::string_view::npos) {
        return invalid_output();
    }

    std::wstring mediaUrl = utf8_to_wide(stdoutBytes);
    if (mediaUrl.empty() || has_forbidden_input_character(mediaUrl)) return invalid_output();

    CrackedUrl url;
    if (!crack_url(mediaUrl, url) || url.scheme != INTERNET_SCHEME_HTTPS ||
        url.hasUserInfo || !has_dot_bound_suffix(url.host, L"googlevideo.com")) {
        return invalid_output();
    }

    ResolveResult result;
    result.ok = true;
    result.mediaUrl = std::move(mediaUrl);
    return result;
}

namespace {

std::wstring quote_windows_argument(std::wstring_view argument)
{
    if (!argument.empty() &&
        argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }

    std::wstring quoted(1, L'\"');
    size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::vector<std::wstring> build_youtube_resolver_arguments(
    const std::filesystem::path& helperDirectory,
    std::wstring_view youtubeUrl)
{
    return {
        L"--no-config",
        L"--no-cache-dir",
        L"--no-playlist",
        L"--no-warnings",
        L"--js-runtimes",
        L"deno:" + (helperDirectory / L"deno.exe").wstring(),
        L"--extractor-args",
        L"youtube:player_client=android",
        L"-f",
        L"b[ext=mp4]/b",
        L"--get-url",
        std::wstring(youtubeUrl),
    };
}

} // namespace

#ifdef YOUTUBE_RESOLVER_TESTING
std::wstring QuoteWindowsArgument(std::wstring_view argument)
{
    return quote_windows_argument(argument);
}

std::vector<std::wstring> BuildYouTubeResolverArguments(
    const std::filesystem::path& helperDirectory,
    std::wstring_view youtubeUrl)
{
    return build_youtube_resolver_arguments(helperDirectory, youtubeUrl);
}
#endif

YouTubeResolver::YouTubeResolver()
    : helperDirectory_(module_directory())
{
}

#ifdef YOUTUBE_RESOLVER_TESTING
YouTubeResolver::YouTubeResolver(Settings settings)
    : helperDirectory_(std::move(settings.helperDirectory)),
      deadline_(settings.deadline),
      pollInterval_(settings.pollInterval),
      shutdownWait_(settings.shutdownWait),
      failureStage_(settings.failureStage)
{
    pollInterval_ = std::clamp(
        pollInterval_, std::chrono::milliseconds{1},
        std::chrono::milliseconds{50});
    shutdownWait_ = std::clamp(
        shutdownWait_, std::chrono::milliseconds{1},
        std::chrono::milliseconds{2000});
}
#endif

YouTubeResolver::~YouTubeResolver()
{
    Cancel();
    const std::scoped_lock operationLock(resolveMutex_);
}

void YouTubeResolver::Cancel()
{
    const std::scoped_lock stateLock(stateMutex_);
    if (!resolving_) return;
    cancelRequested_ = true;
    if (activeJob_) TerminateJobObject(activeJob_, ERROR_CANCELLED);
}

ResolveResult YouTubeResolver::Resolve(std::wstring_view youtubeUrl, std::stop_token stop)
{
    const std::unique_lock operationLock(resolveMutex_);
    {
        const std::scoped_lock stateLock(stateMutex_);
        resolving_ = true;
        cancelRequested_ = false;
    }

    HANDLE job = nullptr;
    const auto finishState = [this, &job](void*) {
        const std::scoped_lock stateLock(stateMutex_);
        if (activeJob_ == job) activeJob_ = nullptr;
        if (job) CloseHandle(job);
        resolving_ = false;
        cancelRequested_ = false;
    };
    const std::unique_ptr<void, decltype(finishState)> stateGuard(this, finishState);

    if (!IsSupportedYouTubeUrl(youtubeUrl)) {
        return resolver_error(ResolveError::InvalidUrl,
                              L"Enter a supported YouTube video URL.");
    }
    if (stop.stop_requested()) {
        return resolver_error(ResolveError::Cancelled,
                              L"YouTube resolution was cancelled.");
    }

    VerifiedHelpers verifiedHelpers;
    if (!verify_beside_app_helpers(helperDirectory_, verifiedHelpers)) {
        return resolver_error(ResolveError::HelperMissing,
                              L"YouTube helper files are missing beside the app.");
    }

    SECURITY_ATTRIBUTES pipeSecurity{sizeof(pipeSecurity), nullptr, TRUE};
    HANDLE rawReadPipe = nullptr;
    HANDLE rawWritePipe = nullptr;
    if (!CreatePipe(&rawReadPipe, &rawWritePipe, &pipeSecurity, 0)) {
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }
    UniqueHandle readPipe(rawReadPipe);
    UniqueHandle writePipe(rawWritePipe);
#ifdef YOUTUBE_RESOLVER_TESTING
    if (failureStage_ == FailureStage::PipeHandlesOwned) {
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }
#endif
    if (!SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0)) {
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }

    job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
    jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &jobLimits, sizeof(jobLimits))) {
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    if (attributeBytes == 0) {
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }
    std::vector<std::byte> attributeStorage(attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes)) {
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }
    const auto deleteAttributes = [](LPPROC_THREAD_ATTRIBUTE_LIST value) {
        if (value) DeleteProcThreadAttributeList(value);
    };
    const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>,
                          decltype(deleteAttributes)>
        attributes(attributeList, deleteAttributes);
    HANDLE inheritedOutput = writePipe.get();
    if (!UpdateProcThreadAttribute(
            attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            &inheritedOutput, sizeof(inheritedOutput), nullptr, nullptr)) {
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }

    const std::vector<std::wstring> arguments =
        build_youtube_resolver_arguments(verifiedHelpers.directoryPath, youtubeUrl);
    auto childEnvironment =
        child_environment_with_package_cache(verifiedHelpers.cacheDirectoryPath);
    if (!childEnvironment) {
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }
    std::wstring commandLine = quote_windows_argument(verifiedHelpers.ytDlpPath.wstring());
    for (const std::wstring& argument : arguments) {
        commandLine.push_back(L' ');
        commandLine.append(quote_windows_argument(argument));
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullptr;
    startup.StartupInfo.hStdOutput = writePipe.get();
    startup.StartupInfo.hStdError = writePipe.get();
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION rawProcess{};
    const DWORD creationFlags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                                EXTENDED_STARTUPINFO_PRESENT;
    if (!CreateProcessW(verifiedHelpers.ytDlpPath.c_str(), commandLine.data(), nullptr, nullptr,
                        TRUE, creationFlags, childEnvironment->data(),
                        verifiedHelpers.directoryPath.c_str(),
                        &startup.StartupInfo, &rawProcess)) {
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }
    UniqueHandle process(rawProcess.hProcess);
    UniqueHandle processThread(rawProcess.hThread);
    writePipe.reset();

    bool assignedToJob = false;
#ifdef YOUTUBE_RESOLVER_TESTING
    if (failureStage_ != FailureStage::JobAssignment)
#endif
    {
        assignedToJob = AssignProcessToJobObject(job, process.get()) != FALSE;
    }
    if (!assignedToJob) {
        TerminateProcess(process.get(), ERROR_PROCESS_ABORTED);
        WaitForSingleObject(process.get(), static_cast<DWORD>(shutdownWait_.count()));
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }

    bool cancelledBeforeResume = false;
    {
        const std::scoped_lock stateLock(stateMutex_);
        activeJob_ = job;
        cancelledBeforeResume = cancelRequested_;
    }
    if (cancelledBeforeResume || stop.stop_requested()) {
        TerminateJobObject(job, ERROR_CANCELLED);
        WaitForSingleObject(process.get(), static_cast<DWORD>(shutdownWait_.count()));
        return resolver_error(ResolveError::Cancelled,
                              L"YouTube resolution was cancelled.");
    }
    DWORD resumeResult = static_cast<DWORD>(-1);
#ifdef YOUTUBE_RESOLVER_TESTING
    if (failureStage_ != FailureStage::Resume)
#endif
    {
        resumeResult = ResumeThread(processThread.get());
    }
    if (resumeResult == static_cast<DWORD>(-1)) {
        TerminateJobObject(job, ERROR_PROCESS_ABORTED);
        WaitForSingleObject(process.get(), static_cast<DWORD>(shutdownWait_.count()));
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }
    processThread.reset();

    enum class Completion {
        Running,
        Exited,
        Cancelled,
        TimedOut,
        Overflow,
        PipeFailed,
    };
    Completion completion = Completion::Running;
    std::string output;
    output.reserve(kMaximumOutputBytes);
    const auto deadline = std::chrono::steady_clock::now() + deadline_;

    const auto cancellationRequested = [this, &stop] {
        if (stop.stop_requested()) return true;
        const std::scoped_lock stateLock(stateMutex_);
        return cancelRequested_;
    };
    const auto drainAvailable = [&] {
#ifdef YOUTUBE_RESOLVER_TESTING
        if (failureStage_ == FailureStage::PipeRead) return Completion::PipeFailed;
#endif
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(readPipe.get(), nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) return Completion::Running;
                return Completion::PipeFailed;
            }
            if (available == 0) return Completion::Running;
            char buffer[4096];
            const DWORD wanted = std::min<DWORD>(available, sizeof(buffer));
            DWORD received = 0;
            if (!ReadFile(readPipe.get(), buffer, wanted, &received, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) return Completion::Running;
                return Completion::PipeFailed;
            }
            if (received == 0) return Completion::Running;
            if (output.size() + received > kMaximumCapturedBytes) {
                return Completion::Overflow;
            }
            output.append(buffer, received);
        }
    };

    while (completion == Completion::Running) {
        if (cancellationRequested()) {
            completion = Completion::Cancelled;
            break;
        }
        const Completion drainResult = drainAvailable();
        if (drainResult != Completion::Running) {
            completion = drainResult;
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            completion = Completion::TimedOut;
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        const auto waitDuration = std::min(pollInterval_, remaining);
        const DWORD wait = WaitForSingleObject(
            process.get(), static_cast<DWORD>(std::max<int64_t>(1, waitDuration.count())));
        if (wait == WAIT_OBJECT_0) {
            if (cancellationRequested()) completion = Completion::Cancelled;
            else {
                const Completion finalDrain = drainAvailable();
                completion = finalDrain == Completion::Running ? Completion::Exited
                                                                : finalDrain;
            }
        } else if (wait == WAIT_FAILED) {
            completion = Completion::PipeFailed;
        }
    }

    if (completion == Completion::Cancelled || completion == Completion::TimedOut ||
        completion == Completion::Overflow || completion == Completion::PipeFailed) {
        TerminateJobObject(job, ERROR_PROCESS_ABORTED);
        WaitForSingleObject(process.get(), static_cast<DWORD>(shutdownWait_.count()));
    }

    if (completion == Completion::Cancelled) {
        return resolver_error(ResolveError::Cancelled,
                              L"YouTube resolution was cancelled.");
    }
    if (completion == Completion::TimedOut) {
        return resolver_error(ResolveError::TimedOut,
                              L"YouTube resolution timed out.");
    }
    if (completion == Completion::Overflow) {
        return resolver_error(ResolveError::OutputTooLarge,
                              L"YouTube resolver output exceeded the safety limit.");
    }
    if (completion == Completion::PipeFailed) {
        return resolver_error(ResolveError::ExtractionFailed,
                              L"Could not extract a playable YouTube stream.");
    }

    DWORD exitCode = ERROR_PROCESS_ABORTED;
    if (!GetExitCodeProcess(process.get(), &exitCode)) {
        return resolver_error(ResolveError::ExtractionFailed,
                              L"Could not extract a playable YouTube stream.");
    }
    return ParseResolverOutput(output, exitCode);
}
