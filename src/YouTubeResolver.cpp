#include "YouTubeResolver.h"
#include "PlatformPaths.h"
#include "HardErrorSuppression.h"
#include "KillOnCloseJob.h"
#include "NarrowText.h"
#include "Utf8Text.h"
#include "Log.h"

#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cwctype>
#include <limits>
#include <memory>
#include <optional>
#include <charconv>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr size_t kMaximumInputCharacters = 2048;
constexpr size_t kMaximumOutputBytes = 16 * 1024;
constexpr size_t kMaximumCapturedBytes = 64 * 1024;
// How much of yt-dlp's stderr is kept for the log. The rest is still read, so
// a chatty helper never blocks on a full pipe, but only counted.
constexpr size_t kMaximumStderrBytes = 4 * 1024;
constexpr double kMaximumDurationSeconds = 30.0 * 24.0 * 60.0 * 60.0;

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
    return platform_paths::ModuleDirectory().value_or(std::filesystem::path{});
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

// Why a helper could not be used. Five causes shared one message and no log
// line, so "YouTube helper files are missing beside the app" was shown for a
// read-only install whose files were all present and for a junction attack
// being correctly refused. A bug report could not distinguish them, and
// neither could the person writing it.
enum class HelperFailure {
    None,
    DirectoryNotAbsolute,
    DirectoryUnopenable,
    DirectoryNotADirectory,
    DirectoryNotCanonical,
    HelperAbsent,
    HelperUnreadable,
    HelperNotAFile,
    HelperIsReparsePoint,
    HelperOutsideDirectory,
    CacheUnavailable,
};

const char* describe_helper_failure(HelperFailure failure)
{
    switch (failure) {
        case HelperFailure::None: return "none";
        case HelperFailure::DirectoryNotAbsolute: return "the helper directory is a relative path";
        case HelperFailure::DirectoryUnopenable: return "the helper directory could not be opened";
        case HelperFailure::DirectoryNotADirectory: return "the helper directory is not a directory";
        case HelperFailure::DirectoryNotCanonical: return "the helper directory has no canonical path";
        case HelperFailure::HelperAbsent: return "a helper file is not there";
        case HelperFailure::HelperUnreadable: return "a helper file could not be opened";
        case HelperFailure::HelperNotAFile: return "a helper name is a directory, not a file";
        case HelperFailure::HelperIsReparsePoint: return "a helper name is a reparse point";
        case HelperFailure::HelperOutsideDirectory: return "a helper resolves outside the package";
        case HelperFailure::CacheUnavailable: return "the package-local helper cache could not be created";
    }
    return "unknown";
}

bool open_verified_helper(const std::filesystem::path& requestedPath,
                          const std::filesystem::path& canonicalDirectory,
                          UniqueHandle& heldHandle,
                          std::filesystem::path& canonicalPath,
                          HelperFailure& failure)
{
    UniqueHandle candidate(CreateFileW(
        requestedPath.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES | FILE_EXECUTE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!candidate) {
        const DWORD reason = GetLastError();
        // Not there and cannot be read are different problems with different
        // remedies: one is a broken install, the other is usually antivirus
        // or an ACL.
        failure = (reason == ERROR_FILE_NOT_FOUND || reason == ERROR_PATH_NOT_FOUND)
                      ? HelperFailure::HelperAbsent : HelperFailure::HelperUnreadable;
        return false;
    }
    if (GetFileType(candidate.get()) != FILE_TYPE_DISK) {
        failure = HelperFailure::HelperNotAFile;
        return false;
    }

    FILE_ATTRIBUTE_TAG_INFO tagInfo{};
    if (!GetFileInformationByHandleEx(candidate.get(), FileAttributeTagInfo,
                                      &tagInfo, sizeof(tagInfo))) {
        failure = HelperFailure::HelperUnreadable;
        return false;
    }
    if ((tagInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        failure = HelperFailure::HelperNotAFile;
        return false;
    }
    if ((tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || tagInfo.ReparseTag != 0) {
        failure = HelperFailure::HelperIsReparsePoint;
        return false;
    }

    canonicalPath = final_normalized_path(candidate.get());
    if (canonicalPath.empty() ||
        !same_path_case_insensitive(canonicalPath.parent_path(), canonicalDirectory)) {
        failure = HelperFailure::HelperOutsideDirectory;
        return false;
    }
    heldHandle = std::move(candidate);
    return true;
}

bool verify_beside_app_helpers(const std::filesystem::path& requestedDirectory,
                               VerifiedHelpers& verified,
                               HelperFailure& failure,
                               std::wstring& failedHelper)
{
    failure = HelperFailure::None;
    failedHelper.clear();
    if (!requestedDirectory.is_absolute()) {
        failure = HelperFailure::DirectoryNotAbsolute;
        return false;
    }
    UniqueHandle directory(CreateFileW(
        requestedDirectory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!directory || GetFileType(directory.get()) != FILE_TYPE_DISK) {
        failure = HelperFailure::DirectoryUnopenable;
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO directoryInfo{};
    if (!GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo,
                                      &directoryInfo, sizeof(directoryInfo)) ||
        (directoryInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        failure = HelperFailure::DirectoryNotADirectory;
        return false;
    }
    const std::filesystem::path canonicalDirectory =
        final_normalized_path(directory.get());
    if (canonicalDirectory.empty()) {
        failure = HelperFailure::DirectoryNotCanonical;
        return false;
    }

    UniqueHandle ytDlp;
    UniqueHandle deno;
    std::filesystem::path canonicalYtDlp;
    std::filesystem::path canonicalDeno;
    if (!open_verified_helper(requestedDirectory / L"yt-dlp.exe", canonicalDirectory,
                              ytDlp, canonicalYtDlp, failure)) {
        failedHelper = L"yt-dlp.exe";
        return false;
    }
    if (!open_verified_helper(requestedDirectory / L"deno.exe", canonicalDirectory,
                              deno, canonicalDeno, failure)) {
        failedHelper = L"deno.exe";
        return false;
    }

    UniqueHandle cacheDirectory;
    std::filesystem::path canonicalCacheDirectory;
    if (!create_verified_package_cache(canonicalDirectory, cacheDirectory,
                                       canonicalCacheDirectory)) {
        failure = HelperFailure::CacheUnavailable;
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

const char* describe_resolve_error(ResolveError error)
{
    switch (error) {
        case ResolveError::None: return "none";
        case ResolveError::InvalidUrl: return "invalid-url";
        case ResolveError::HelperMissing: return "helper-missing";
        case ResolveError::StartFailed: return "start-failed";
        case ResolveError::TimedOut: return "timed-out";
        case ResolveError::Cancelled: return "cancelled";
        case ResolveError::OutputTooLarge: return "output-too-large";
        case ResolveError::ExtractionFailed: return "extraction-failed";
        case ResolveError::InvalidOutput: return "invalid-output";
    }
    return "unknown";
}

// Every refusal in this module goes through here, which is why the log line
// lives here rather than at twenty-five call sites. The module is the one
// most exposed to upstream breakage - yt-dlp and YouTube both change without
// notice - and it used to produce no diagnostic at all: not one LOG line in
// 1,143 lines, so a failure left nothing for a bug report to carry.
ResolveResult resolver_error(ResolveError error, std::wstring detail)
{
    ResolveResult result;
    result.error = error;
    result.detail = std::move(detail);
    // Cancellation is the user's own doing and happens on every abandoned
    // paste; logging it would bury the failures that matter.
    if (error != ResolveError::Cancelled)
        LOG("YouTube: " << describe_resolve_error(error) << " - "
            << narrow_text::LossyAscii(result.detail));
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

// A YouTube video id is exactly eleven characters of the URL-safe base64
// alphabet; the length is part of the format, not a limit.
constexpr size_t kVideoIdLength = 11;

bool is_video_id(std::wstring_view value)
{
    if (value.size() != kVideoIdLength) return false;
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

std::wstring_view unique_query_value(std::wstring_view query,std::wstring_view wanted)
{
    std::wstring_view found;
    while(!query.empty()){
        const size_t separator=query.find(L'&');
        const std::wstring_view item=query.substr(0,separator);
        const size_t equals=item.find(L'=');
        if(equals!=std::wstring_view::npos&&
           equals_case_insensitive(item.substr(0,equals),wanted)){
            if(!found.empty())return {};
            found=item.substr(equals+1);
        }
        if(separator==std::wstring_view::npos)break;
        query.remove_prefix(separator+1);
    }
    return found;
}

std::string ascii_value(std::wstring_view value)
{
    std::string result;result.reserve(value.size());
    for(const wchar_t character:value){
        if(character<0x20||character>0x7e)return {};
        result.push_back(static_cast<char>(character));
    }
    return result;
}

std::string stream_itag(std::wstring_view value)
{
    CrackedUrl url;
    if(!crack_url(value,url)||url.scheme!=INTERNET_SCHEME_HTTPS||url.hasUserInfo||
       !has_dot_bound_suffix(url.host,L"googlevideo.com"))return {};
    auto itag=unique_query_value(query_part(url.extra),L"itag");
    if(url.path.starts_with(L"/api/manifest/hls_playlist/")){
        // YouTube HLS puts its stable format ID in the path, alongside expiring
        // signature fields that must never enter the cache identity.
        constexpr std::wstring_view marker=L"/itag/";
        const size_t start=url.path.find(marker);
        if(start==std::wstring::npos||url.path.find(marker,start+1)!=std::wstring::npos)return {};
        itag=std::wstring_view(url.path).substr(start+marker.size());
        itag=itag.substr(0,itag.find(L'/'));
    }
    if(itag.empty()||itag.size()>8||!std::all_of(itag.begin(),itag.end(),[](wchar_t c){
        return c>=L'0'&&c<=L'9';
    }))return {};
    return ascii_value(itag);
}

bool has_forbidden_input_character(std::wstring_view value)
{
    return std::any_of(value.begin(), value.end(), [](wchar_t character) {
        return character <= 0x1f || character == 0x7f || character == L'\'' ||
               character == L'"' || character == L'\\' || std::iswspace(character) != 0;
    });
}

ResolveResult invalid_output()
{
    ResolveResult result;
    result.error = ResolveError::InvalidOutput;
    result.detail = L"Resolver returned invalid output.";
    return result;
}

// yt-dlp --print writes NA for every field it could not fill, and the stream
// metrics are diagnostics rather than playback inputs, so anything unusable
// reads as zero instead of rejecting output that carries a playable stream URL.
double optional_metric(std::string_view value)
{
    double parsed = 0.0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        !std::isfinite(parsed) || parsed < 0.0) {
        return 0.0;
    }
    return parsed;
}

// Heights and age gates print as integers, but parse through the float path so a
// float-formatted value still reads, and bound the value before the cast.
int optional_metric_count(std::string_view value)
{
    return static_cast<int>(std::min(optional_metric(value), 1000000.0));
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

std::string CanonicalYouTubeVideoId(std::wstring_view value)
{
    if(!IsSupportedYouTubeUrl(value))return {};
    CrackedUrl url;if(!crack_url(value,url))return {};
    std::wstring_view id;
    if(equals_case_insensitive(url.host,L"youtu.be"))id=std::wstring_view(url.path).substr(1);
    else if(url.path==L"/watch")id=unique_query_value(query_part(url.extra),L"v");
    else{
        constexpr std::wstring_view prefix=L"/shorts/";
        if(url.path.starts_with(prefix))id=std::wstring_view(url.path).substr(prefix.size());
    }
    return is_video_id(id)?ascii_value(id):std::string{};
}

std::string StableYouTubeStreamIdentity(std::wstring_view mediaUrl,
                                        std::wstring_view audioUrl)
{
    std::string video=stream_itag(mediaUrl),audio=stream_itag(audioUrl);
    if(video.empty()||audio.empty())return {};
    return "video-itag="+video+"|audio-itag="+audio;
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

std::string SummarizeResolverStderr(std::string_view captured, size_t totalBytes)
{
    std::string summary;
    summary.reserve(captured.size());
    bool lineBreak = false;
    for (const char raw : captured) {
        const auto character = static_cast<unsigned char>(raw);
        if (character == '\r' || character == '\n') {
            lineBreak = !summary.empty();
            continue;
        }
        if (lineBreak) summary += " | ";
        lineBreak = false;
        summary.push_back(character >= 0x20 && character < 0x7F ? static_cast<char>(character) : '?');
    }
    if (!summary.empty() && totalBytes > captured.size()) {
        summary += " ... (" + std::to_string(totalBytes - captured.size()) + " more bytes)";
    }
    return summary;
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
    if (stdoutBytes.empty()) return invalid_output();

    double durationSeconds = 0.0;
    int64_t availableAtUnixSeconds = 0;
    int selectedHeight = 0;
    double videoKbps = 0.0;
    int ageLimit = 0;
    if (stdoutBytes.starts_with("duration=")) {
        // yt-dlp --print emits metadata before the --get-url stream lines.
        // Legacy URL-only parsing stays available, but Resolve requires metadata.
        const size_t metadataEnd = stdoutBytes.find('\n');
        if (metadataEnd == std::string_view::npos) return invalid_output();
        std::string_view metadata = stdoutBytes.substr(0, metadataEnd);
        if (metadata.ends_with('\r')) metadata.remove_suffix(1);
        // The stream metrics ride at the end of the same printed line, so peel
        // them off before the live fields below parse what is left.
        constexpr std::string_view heightMarker = ";selected_height=";
        const size_t metricsStart = metadata.find(heightMarker);
        if (metricsStart != std::string_view::npos) {
            constexpr std::string_view bitrateMarker = ";video_kbps=";
            constexpr std::string_view ageMarker = ";age_limit=";
            std::string_view metrics = metadata.substr(metricsStart + heightMarker.size());
            metadata = metadata.substr(0, metricsStart);
            std::string_view age;
            if (const size_t ageStart = metrics.find(ageMarker); ageStart != std::string_view::npos) {
                age = metrics.substr(ageStart + ageMarker.size());
                metrics = metrics.substr(0, ageStart);
            }
            std::string_view bitrate;
            if (const size_t bitrateStart = metrics.find(bitrateMarker);
                bitrateStart != std::string_view::npos) {
                bitrate = metrics.substr(bitrateStart + bitrateMarker.size());
                metrics = metrics.substr(0, bitrateStart);
            }
            // What survives both peels is the height value on its own.
            selectedHeight = optional_metric_count(metrics);
            videoKbps = optional_metric(bitrate);
            ageLimit = optional_metric_count(age);
        }
        constexpr std::string_view statusMarker = ";live_status=";
        const size_t statusStart = metadata.find(statusMarker);
        if (statusStart == std::string_view::npos) return invalid_output();
        const std::string_view duration = metadata.substr(9, statusStart - 9);
        std::string_view status = metadata.substr(statusStart + statusMarker.size());
        constexpr std::string_view videoAvailabilityMarker = ";video_available_at=";
        constexpr std::string_view audioAvailabilityMarker = ";audio_available_at=";
        const size_t availabilityStart = status.find(videoAvailabilityMarker);
        if (availabilityStart != std::string_view::npos) {
            const auto availability = status.substr(availabilityStart + videoAvailabilityMarker.size());
            status = status.substr(0, availabilityStart);
            const auto audioStart = availability.find(audioAvailabilityMarker);
            if (audioStart == std::string_view::npos) return invalid_output();
            for (const auto value : {availability.substr(0, audioStart),
                                     availability.substr(audioStart + audioAvailabilityMarker.size())}) {
                double timestamp = 0.0;
                const auto parsedTimestamp = std::from_chars(value.data(), value.data() + value.size(), timestamp);
                // Bound the epoch before any clock arithmetic (through year 9999).
                if (parsedTimestamp.ec != std::errc{} || parsedTimestamp.ptr != value.data() + value.size() ||
                    !std::isfinite(timestamp) || timestamp < 0.0 || timestamp > 253402300799.0)
                    return invalid_output();
                // A millisecond ad skip offset can produce a fractional epoch.
                // Round upward so the integer-second wait never opens it early.
                availableAtUnixSeconds = std::max(availableAtUnixSeconds,
                    static_cast<int64_t>(std::ceil(timestamp)));
            }
        }
        const auto parsed = std::from_chars(duration.data(), duration.data() + duration.size(),
                                            durationSeconds);
        if (parsed.ec != std::errc{} || parsed.ptr != duration.data() + duration.size() ||
            !std::isfinite(durationSeconds) || durationSeconds <= 0.0 ||
            durationSeconds > kMaximumDurationSeconds ||
            (status != "not_live" && status != "was_live")) {
            return invalid_output();
        }
        stdoutBytes.remove_prefix(metadataEnd + 1);
        if (stdoutBytes.empty()) return invalid_output();
    }

    const size_t separator = stdoutBytes.find('\n');
    if (separator != std::string_view::npos &&
        stdoutBytes.find('\n', separator + 1) != std::string_view::npos) {
        return invalid_output();
    }
    std::string_view videoBytes = separator == std::string_view::npos
                                      ? stdoutBytes
                                      : stdoutBytes.substr(0, separator);
    std::string_view audioBytes = separator == std::string_view::npos
                                      ? videoBytes
                                      : stdoutBytes.substr(separator + 1);
    if (separator != std::string_view::npos && videoBytes.ends_with('\r')) {
        videoBytes.remove_suffix(1);
    }
    if (videoBytes.empty() || audioBytes.empty() ||
        videoBytes.find_first_of("\r\n") != std::string_view::npos ||
        audioBytes.find_first_of("\r\n") != std::string_view::npos) {
        return invalid_output();
    }

    std::wstring mediaUrl = utf8_text::ToWideStrict(videoBytes).value_or(std::wstring{});
    std::wstring audioUrl = utf8_text::ToWideStrict(audioBytes).value_or(std::wstring{});
    const auto trustedStreamUrl = [](const std::wstring& value) {
        if (value.empty() || has_forbidden_input_character(value)) return false;
        CrackedUrl url;
        return crack_url(value, url) && url.scheme == INTERNET_SCHEME_HTTPS &&
               !url.hasUserInfo && has_dot_bound_suffix(url.host, L"googlevideo.com");
    };
    if (!trustedStreamUrl(mediaUrl) || !trustedStreamUrl(audioUrl)) {
        return invalid_output();
    }

    ResolveResult result;
    result.ok = true;
    result.mediaUrl = std::move(mediaUrl);
    result.audioUrl = std::move(audioUrl);
    result.durationSeconds = durationSeconds;
    result.availableAtUnixSeconds = availableAtUnixSeconds;
    result.selectedHeight = selectedHeight;
    result.videoKbps = videoKbps;
    result.ageLimit = ageLimit;
    return result;
}

std::wstring_view YouTubeFormatSelector(YouTubeSourceQuality quality)
{
    switch (quality) {
    case YouTubeSourceQuality::Auto:
        return L"bv*[height<=1440]+ba/b[height<=1440]/bv*[height<=2160]+ba/b[height<=2160]";
    case YouTubeSourceQuality::P2160:
        return L"bv*[height=2160]+ba/b[height=2160]";
    case YouTubeSourceQuality::P1440:
        return L"bv*[height=1440]+ba/b[height=1440]";
    case YouTubeSourceQuality::P1080:
        return L"bv*[height=1080]+ba/b[height=1080]";
    }
    return L"bv*[height<=1440]+ba/b[height<=1440]/bv*[height<=2160]+ba/b[height<=2160]";
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
    std::wstring_view youtubeUrl,
    YouTubeSourceQuality quality)
{
    return {
        L"--no-config",
        L"--no-cache-dir",
        L"--no-plugin-dirs",
        L"--no-playlist",
        L"--no-warnings",
        L"--js-runtimes",
        L"deno:" + (helperDirectory / L"deno.exe").wstring(),
        L"-f",
        std::wstring(YouTubeFormatSelector(quality)),
        // Auto takes the tallest rung up to 1440p and then the highest advertised
        // video bitrate inside it, ahead of codec, container, frame rate and
        // extractor preferences. The former height=1080 first rung pinned the
        // lowest-bitrate copy YouTube publishes: one trailer measures 3899 kbps
        // at 1080p against 7854 kbps at 1440p and 20764 kbps at 2160p. The cap is
        // 1440 and not 2160 because 1440p roughly doubles the bitrate for about
        // 1.8x the render cost, while 4K measures 42 ms per frame, 0.78x real
        // time, and costs 4x the bandwidth, VRAM and cache footprint. The
        // explicit 2160p menu entry stays the way to ask for that trade.
        L"--format-sort-force",
        L"-S",
        L"height,vbr,abr",
        L"--get-url",
        L"--print",
        L"duration=%(duration)s;live_status=%(live_status)s;video_available_at=%(requested_formats.0.available_at,available_at|0)s;audio_available_at=%(requested_formats.1.available_at|0)s;selected_height=%(height)s;video_kbps=%(requested_formats.0.vbr,vbr,tbr|0)s;age_limit=%(age_limit)s",
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
    std::wstring_view youtubeUrl,
    YouTubeSourceQuality quality)
{
    return build_youtube_resolver_arguments(helperDirectory, youtubeUrl, quality);
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
    return Resolve(youtubeUrl, YouTubeSourceQuality::Auto, stop);
}

ResolveResult YouTubeResolver::Resolve(std::wstring_view youtubeUrl,
                                       YouTubeSourceQuality quality,
                                       std::stop_token stop)
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
    HelperFailure helperFailure = HelperFailure::None;
    std::wstring failedHelper;
    if (!verify_beside_app_helpers(helperDirectory_, verifiedHelpers, helperFailure, failedHelper)) {
        // One opaque string used to cover all of these. A read-only install
        // whose files are all present was told they were missing; a junction
        // attack being refused was told the same thing.
        std::wstring detail;
        switch (helperFailure) {
            case HelperFailure::HelperAbsent:
                detail = failedHelper + L" is not beside the app. Reinstall the complete package, "
                                        L"or run tools/fetch_youtube_helpers.ps1.";
                break;
            case HelperFailure::HelperUnreadable:
                detail = failedHelper + L" is there but could not be opened. Antivirus or file "
                                        L"permissions are the usual cause.";
                break;
            case HelperFailure::HelperNotAFile:
                detail = failedHelper + L" is a directory, not a program. The install is damaged.";
                break;
            case HelperFailure::HelperIsReparsePoint:
                detail = failedHelper + L" is a link rather than a file, so it was refused. "
                                        L"Replace it with the real program.";
                break;
            case HelperFailure::HelperOutsideDirectory:
                detail = failedHelper + L" resolves to somewhere outside the app's folder, so it "
                                        L"was refused.";
                break;
            case HelperFailure::CacheUnavailable:
                detail = L"The YouTube helpers are present, but their cache folder beside the app "
                         L"could not be created. A read-only install folder is the usual cause.";
                break;
            case HelperFailure::DirectoryNotAbsolute:
            case HelperFailure::DirectoryUnopenable:
            case HelperFailure::DirectoryNotADirectory:
            case HelperFailure::DirectoryNotCanonical:
                detail = L"The app's own folder could not be read, so the YouTube helpers beside "
                         L"it could not be checked.";
                break;
            case HelperFailure::None:
                detail = L"YouTube helper files are missing beside the app.";
                break;
        }
        LOG("YouTube: refusing to resolve - " << describe_helper_failure(helperFailure)
            << (failedHelper.empty() ? std::string{} : " (" + narrow_text::LossyAscii(failedHelper) + ")")
            << ", winerr=" << GetLastError() << '.');
        return resolver_error(ResolveError::HelperMissing, detail);
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
    // stderr gets a pipe of its own. It used to share stdout's, and the parser
    // accepts only the metadata and URL lines, so a single Python warning
    // turned a good run into "invalid output" - and was never seen.
    HANDLE rawErrorRead = nullptr;
    HANDLE rawErrorWrite = nullptr;
    if (!CreatePipe(&rawErrorRead, &rawErrorWrite, &pipeSecurity, 0)) {
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }
    UniqueHandle errorReadPipe(rawErrorRead);
    UniqueHandle errorWritePipe(rawErrorWrite);
#ifdef YOUTUBE_RESOLVER_TESTING
    if (failureStage_ == FailureStage::PipeHandlesOwned) {
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }
#endif
    if (!SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(errorReadPipe.get(), HANDLE_FLAG_INHERIT, 0)) {
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }

    job = CreateKillOnCloseJob();
    if (!job) {
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
    HANDLE inheritedOutputs[] = {writePipe.get(), errorWritePipe.get()};
    if (!UpdateProcThreadAttribute(
            attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedOutputs, sizeof(inheritedOutputs), nullptr, nullptr)) {
        return resolver_error(ResolveError::StartFailed,
                              L"Could not start the YouTube resolver.");
    }

    const std::vector<std::wstring> arguments =
        build_youtube_resolver_arguments(verifiedHelpers.directoryPath, youtubeUrl, quality);
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
    startup.StartupInfo.hStdError = errorWritePipe.get();
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION rawProcess{};
    const DWORD creationFlags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                                EXTENDED_STARTUPINFO_PRESENT;
    // A truncated helper would otherwise stop this thread on a system modal.
    const ScopedHardErrorSuppression noHardErrorDialog;
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
    errorWritePipe.reset();

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
    std::string errors;
    size_t errorBytes = 0;
    // Everything waiting in `pipe`, appended to `into` up to `keep` bytes.
    // Past it, stdout is an overflow; stderr is only counted.
    const auto drainPipe = [](HANDLE pipe, std::string& into, size_t keep, bool overflowFails,
                              size_t& total) {
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) return Completion::Running;
                return Completion::PipeFailed;
            }
            if (available == 0) return Completion::Running;
            char buffer[4096];
            const DWORD wanted = std::min<DWORD>(available, sizeof(buffer));
            DWORD received = 0;
            if (!ReadFile(pipe, buffer, wanted, &received, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) return Completion::Running;
                return Completion::PipeFailed;
            }
            if (received == 0) return Completion::Running;
            total += received;
            if (into.size() + received > keep) {
                if (overflowFails) return Completion::Overflow;
                into.append(buffer, std::min<size_t>(received, keep - into.size()));
                continue;
            }
            into.append(buffer, received);
        }
    };
    const auto drainAvailable = [&] {
#ifdef YOUTUBE_RESOLVER_TESTING
        if (failureStage_ == FailureStage::PipeRead) return Completion::PipeFailed;
#endif
        size_t outputBytes = 0;
        const Completion drained =
            drainPipe(readPipe.get(), output, kMaximumCapturedBytes, true, outputBytes);
        if (drained != Completion::Running) return drained;
        // A failure to read the diagnostics is not a failure to resolve.
        drainPipe(errorReadPipe.get(), errors, kMaximumStderrBytes, false, errorBytes);
        return Completion::Running;
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
    // One bounded line per resolve, whatever the outcome: a warning on a run
    // that worked is still worth having the day the next one does not.
    const std::string stderrSummary = SummarizeResolverStderr(errors, errorBytes);
    if (!stderrSummary.empty()) LOG("YouTube: yt-dlp stderr: " << stderrSummary);
#ifdef YOUTUBE_RESOLVER_TESTING
    lastStderrSummary_ = stderrSummary;
#endif

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
    ResolveResult result = ParseResolverOutput(output, exitCode);
    if (result.ok && result.durationSeconds <= 0.0) {
        return resolver_error(ResolveError::InvalidOutput,
                              L"Only fixed-duration YouTube videos are supported.");
    }
    // yt-dlp's native downloader honors the selected formats' available_at;
    // --get-url does not. Opening these URLs early can return HTTP 403 even
    // though the same public streams work as soon as their waiting period ends.
    while (result.ok) {
        if (cancellationRequested())
            return resolver_error(ResolveError::Cancelled, L"YouTube resolution was cancelled.");
        const auto steadyNow = std::chrono::steady_clock::now();
        if (steadyNow >= deadline)
            return resolver_error(ResolveError::TimedOut, L"YouTube resolution timed out.");
        const auto unixNow = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (result.availableAtUnixSeconds <= unixNow) break;
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
            pollInterval_, deadline - steadyNow));
    }
    return result;
}
