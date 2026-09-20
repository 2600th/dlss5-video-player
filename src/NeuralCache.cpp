#include "NeuralCache.h"
#include "CacheEvictionPolicy.h"
#include "NarrowText.h"
#include "PlatformPaths.h"
#include "GuideControls.h"
#include "Log.h"

#include <windows.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <system_error>
#include <vector>

namespace {

// Schema 5 has schema 4's field list; the number retires the entries written
// before the render identity carried a driver and a model-store term. The
// gate is the same one schema 2 and 3 pass through: ParseNeuralCacheManifest
// accepts kSchema and kLegacySchema and refuses everything else, so a
// schema-4 manifest on disk is rejected for its schema rather than for a
// field it happens to be missing.
constexpr uint32_t kSchema = 5;
constexpr uint32_t kLegacySchema = 3;
constexpr uint32_t kMinDimension = 64;
constexpr uint32_t kMaxWidth = 7680;
constexpr uint32_t kMaxHeight = 4320;

class Sha256Hasher {
public:
    Sha256Hasher()
    {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return;
        DWORD copied = 0;
        DWORD objectSize = 0;
        if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                              &copied, 0) < 0 || objectSize == 0) return;
        object_.resize(objectSize);
        if (BCryptCreateHash(algorithm_, &hash_, object_.data(), objectSize,
                             nullptr, 0, 0) < 0) return;
        valid_ = true;
    }

    ~Sha256Hasher()
    {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }

    bool Update(std::span<const uint8_t> bytes)
    {
        if (!valid_) return false;
        if (bytes.empty()) return true;
        return BCryptHashData(hash_, const_cast<PUCHAR>(bytes.data()),
                              static_cast<ULONG>(bytes.size()), 0) >= 0;
    }

    std::optional<std::string> Finish()
    {
        if (!valid_ || finished_) return std::nullopt;
        std::array<uint8_t, 32> digest{};
        if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            return std::nullopt;
        finished_ = true;
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.resize(digest.size() * 2);
        for (size_t index = 0; index < digest.size(); ++index) {
            result[index * 2] = hex[digest[index] >> 4];
            result[index * 2 + 1] = hex[digest[index] & 0x0f];
        }
        return result;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
    std::vector<uint8_t> object_;
    bool valid_{false};
    bool finished_{false};
};

std::optional<std::string> HashBytes(std::string_view bytes)
{
    Sha256Hasher hasher;
    if (!hasher.Update(std::span{
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()})) return std::nullopt;
    return hasher.Finish();
}

bool IsHexDigest(std::string_view value)
{
    return value.size() == 64 && std::ranges::all_of(value, [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

void AppendField(std::string& output, std::string_view name, std::string_view value)
{
    output.append(name);
    output.push_back('=');
    output.append(std::to_string(value.size()));
    output.push_back(':');
    output.append(value);
    output.push_back('\n');
}

std::string JsonEscape(std::string_view value)
{
    std::string result;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20) return {};
            result.push_back(static_cast<char>(character));
            break;
        }
    }
    return result;
}

std::string_view KindName(NeuralCacheEntryKind kind)
{
    return kind == NeuralCacheEntryKind::Source ? "source" : "render";
}

std::string_view StateName(NeuralCacheState state)
{
    switch (state) {
    case NeuralCacheState::Staging: return "staging";
    case NeuralCacheState::Complete: return "complete";
    case NeuralCacheState::Invalid: return "invalid";
    }
    return "invalid";
}

class JsonCursor {
public:
    explicit JsonCursor(std::string_view bytes) : bytes_(bytes) {}

    bool Expect(char character)
    {
        SkipSpace();
        if (position_ >= bytes_.size() || bytes_[position_] != character) return false;
        ++position_;
        return true;
    }

    bool Key(std::string_view expected)
    {
        const auto value = String();
        return value && *value == expected && Expect(':');
    }

    std::optional<std::string> String()
    {
        SkipSpace();
        if (position_ >= bytes_.size() || bytes_[position_++] != '"') return std::nullopt;
        std::string result;
        while (position_ < bytes_.size()) {
            const char character = bytes_[position_++];
            if (character == '"') return result;
            if (static_cast<unsigned char>(character) < 0x20) return std::nullopt;
            if (character != '\\') {
                result.push_back(character);
                continue;
            }
            if (position_ >= bytes_.size()) return std::nullopt;
            const char escaped = bytes_[position_++];
            switch (escaped) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    template <typename Integer>
    std::optional<Integer> IntegerValue()
    {
        SkipSpace();
        const size_t start = position_;
        if (position_ < bytes_.size() && bytes_[position_] == '-') ++position_;
        while (position_ < bytes_.size() && bytes_[position_] >= '0' && bytes_[position_] <= '9')
            ++position_;
        if (position_ == start || (position_ == start + 1 && bytes_[start] == '-'))
            return std::nullopt;
        Integer value{};
        const auto result = std::from_chars(bytes_.data() + start, bytes_.data() + position_, value);
        if (result.ec != std::errc{} || result.ptr != bytes_.data() + position_) return std::nullopt;
        return value;
    }

    std::optional<bool> Bool()
    {
        SkipSpace();
        if (bytes_.substr(position_, 4) == "true") { position_ += 4; return true; }
        if (bytes_.substr(position_, 5) == "false") { position_ += 5; return false; }
        return std::nullopt;
    }

    bool Finished()
    {
        SkipSpace();
        return position_ == bytes_.size();
    }

private:
    void SkipSpace()
    {
        while (position_ < bytes_.size() &&
               (bytes_[position_] == ' ' || bytes_[position_] == '\t' ||
                bytes_[position_] == '\r' || bytes_[position_] == '\n')) ++position_;
    }

    std::string_view bytes_;
    size_t position_{};
};

template <typename Integer>
bool ReadIntegerField(JsonCursor& cursor, std::string_view key, Integer& value, bool comma = true)
{
    const auto parsed = cursor.Key(key) ? cursor.IntegerValue<Integer>() : std::nullopt;
    if (!parsed) return false;
    value = *parsed;
    return !comma || cursor.Expect(',');
}

bool ReadStringField(JsonCursor& cursor, std::string_view key, std::string& value, bool comma = true)
{
    const auto parsed = cursor.Key(key) ? cursor.String() : std::nullopt;
    if (!parsed) return false;
    value = *parsed;
    return !comma || cursor.Expect(',');
}

bool ReadBoolField(JsonCursor& cursor, std::string_view key, bool& value, bool comma = true)
{
    const auto parsed = cursor.Key(key) ? cursor.Bool() : std::nullopt;
    if (!parsed) return false;
    value = *parsed;
    return !comma || cursor.Expect(',');
}

bool RangeFieldsValid(int64_t start100ns, int64_t end100ns)
{
    return start100ns >= 0 && end100ns >= 0 && (end100ns == 0 || end100ns > start100ns);
}

bool CommonManifestFieldsValid(const NeuralCacheManifest& manifest)
{
    if (manifest.schema != kSchema && manifest.schema != kLegacySchema) return false;
    if (manifest.schema == kLegacySchema &&
        (manifest.rangeStart100ns != 0 || manifest.rangeEnd100ns != 0 ||
         !manifest.guides.empty() || manifest.jobId != 0 || manifest.historyResets != 0 ||
         !manifest.receiptDigest.empty())) return false;
    return manifest.width >= kMinDimension && manifest.width <= kMaxWidth &&
           manifest.height >= kMinDimension && manifest.height <= kMaxHeight &&
           manifest.frameCount > 0 && manifest.duration100ns > 0 &&
           !manifest.encoder.empty() && !manifest.upscaling &&
           RangeFieldsValid(manifest.rangeStart100ns, manifest.rangeEnd100ns) &&
           (manifest.guides.empty() || ParseGuideControls(manifest.guides).has_value()) &&
           (manifest.settingsDigest.empty() || IsHexDigest(manifest.settingsDigest)) &&
           (manifest.receiptDigest.empty() || IsHexDigest(manifest.receiptDigest));
}

bool IsStrictDescendant(const std::filesystem::path& root,
                        const std::filesystem::path& candidate)
{
    auto rootIterator = root.begin();
    auto candidateIterator = candidate.begin();
    for (; rootIterator != root.end(); ++rootIterator, ++candidateIterator) {
        if (candidateIterator == candidate.end() ||
            _wcsicmp(rootIterator->c_str(), candidateIterator->c_str()) != 0) return false;
    }
    return candidateIterator != candidate.end();
}

std::filesystem::path CanonicalOrAbsolute(const std::filesystem::path& path,
                                          std::error_code& error)
{
    auto value = std::filesystem::weakly_canonical(path, error);
    if (!error) return value;
    error.clear();
    value = std::filesystem::absolute(path, error).lexically_normal();
    return value;
}

std::atomic<uint64_t> g_stagingNonce{0};

std::string Utf8(std::wstring_view text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(std::max(length, 0)), '\0');
    if (length > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        utf8.data(), length, nullptr, nullptr);
    return utf8;
}

// One line per refusal. The only field report of an uncreatable cache carried
// the player's status string and nothing else, so the path, the cause, the
// filesystem error and the ownership verdict all belong on it.
void LogCacheFailure(std::string_view what, const NeuralCacheFailure& failure)
{
    LOG(what << ": cause=" << NeuralCacheFailureCauseName(failure.cause)
             << " path=" << Utf8(failure.path.native())
             << " error=" << failure.error.value() << " ownershipRejected="
             << (failure.cause == NeuralCacheFailure::Cause::OutsideRoot ? 1 : 0));
}

std::optional<std::filesystem::path> ResolveWritableRoot(const std::filesystem::path& root,
                                                         std::error_code& error)
{
    // Windows can merge an existing LocalAppData directory with package-private
    // writes. A directory handle may report the read-side path; a newly created
    // file identifies the actual writable parent without weakening ownership.
    const auto probe = root / (L".cache-path-" + std::to_wstring(GetCurrentProcessId()) +
                               L"-" + std::to_wstring(++g_stagingNonce));
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error.assign(static_cast<int>(GetLastError()), std::system_category());
        return std::nullopt;
    }
    std::wstring physical(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(file, physical.data(),
        static_cast<DWORD>(physical.size()), FILE_NAME_NORMALIZED);
    // Read before the close, which overwrites the thread's last error.
    const DWORD nameError = length ? (length >= physical.size() ? ERROR_BUFFER_OVERFLOW
                                                                : ERROR_SUCCESS)
                                   : GetLastError();
    CloseHandle(file); // Deletes only this unique probe, including on failure.
    if (nameError != ERROR_SUCCESS) {
        error.assign(static_cast<int>(nameError), std::system_category());
        return std::nullopt;
    }
    physical.resize(length);
    auto resolved = std::filesystem::canonical(std::filesystem::path(physical).parent_path(), error);
    if (error || resolved == resolved.root_path()) return std::nullopt;
    return resolved;
}

std::optional<std::filesystem::path> PrepareWritableRoot(const std::filesystem::path& root,
                                                         NeuralCacheFailure& failure)
{
    failure = {NeuralCacheFailure::Cause::NoWritableRoot, {}, root};
    std::error_code error;
    auto resolved = CanonicalOrAbsolute(root, error);
    if (error || resolved.empty() || resolved == resolved.root_path() ||
        resolved.parent_path().empty()) { failure.error = error; return std::nullopt; }
    failure.path = resolved;
    // create_directories reports false for a directory that is already there,
    // which a portable installation ships; only the error decides here.
    std::filesystem::create_directories(resolved, error);
    if (error) { failure.error = error; return std::nullopt; }
    const auto writableRoot = ResolveWritableRoot(resolved, error);
    if (!writableRoot) { failure.error = error; return std::nullopt; }
    for (const auto directory : {L"sources", L"renders", L"staging", L"frame-generation", L"live"}) {
        std::filesystem::create_directories(*writableRoot / directory, error);
        if (error) {
            failure.error = error;
            failure.path = *writableRoot / directory;
            return std::nullopt;
        }
    }
    failure = {};
    return writableRoot;
}

// A directory cannot be renamed while any file inside it is open, and the file
// this one just finished writing is a few hundred megabytes that an antivirus
// scanner opens the instant it is closed. Publishing a render is a rename, so
// one attempt throws a finished render away for a condition that clears itself
// in well under a second. Only the sharing errors are retried; a wrong path or
// a missing directory still fails immediately.
constexpr unsigned kRenameAttempts = 24;
constexpr DWORD kRenameDelayMs = 125;

bool TransientRenameError(DWORD error)
{
    return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED ||
           error == ERROR_LOCK_VIOLATION || error == ERROR_USER_MAPPED_FILE;
}

bool RenameDirectory(const std::filesystem::path& from, const std::filesystem::path& to,
                     DWORD* lastError = nullptr, unsigned* attempts = nullptr,
                     const std::function<void(unsigned)>& retrying = {})
{
    for (unsigned attempt = 1; attempt <= kRenameAttempts; ++attempt) {
        if (attempts) *attempts = attempt;
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        const DWORD error = GetLastError();
        if (lastError) *lastError = error;
        if (!TransientRenameError(error) || attempt == kRenameAttempts) return false;
        if (retrying) retrying(attempt);
        Sleep(kRenameDelayMs);
    }
    return false;
}

bool MoveToInvalidDirectory(const std::filesystem::path& root,
                            const std::filesystem::path& source,
                            std::wstring_view prefix)
{
    for(size_t attempt=0;attempt<128;++attempt){
        const auto destination=root/L"staging"/
            (std::wstring(prefix)+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+
             std::to_wstring(++g_stagingNonce));
        DWORD error=ERROR_SUCCESS;
        if(RenameDirectory(source,destination,&error))return true;
        if(error!=ERROR_ALREADY_EXISTS&&error!=ERROR_FILE_EXISTS)return false;
    }
    return false;
}

// The owner of a staging directory, from the "<prefix>-<pid>-<nonce>" name
// BeginStaging and MoveToInvalidDirectory write. Anything else in staging/ was
// not put there by this code and is left alone.
bool ParseStagingOwner(const std::wstring& name, DWORD& pid)
{
    const size_t nonce = name.rfind(L'-');
    if (nonce == std::wstring::npos || nonce == 0) return false;
    const size_t owner = name.rfind(L'-', nonce - 1);
    if (owner == std::wstring::npos || owner + 1 == nonce) return false;
    uint64_t value = 0;
    for (size_t index = owner + 1; index < nonce; ++index) {
        const wchar_t digit = name[index];
        if (digit < L'0' || digit > L'9') return false;
        value = value * 10 + static_cast<uint64_t>(digit - L'0');
        if (value > MAXDWORD) return false;
    }
    pid = static_cast<DWORD>(value);
    return true;
}

// Conservative: a process this one may not open is alive, and a reused pid
// keeps the directory of the process that had it. Only a pid the kernel does
// not know, or one whose process has exited, frees an entry.
bool ProcessAlive(DWORD pid)
{
    if (pid == GetCurrentProcessId()) return true;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
    DWORD code = 0;
    const bool alive = !GetExitCodeProcess(process, &code) || code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

// A sweep runs on every manager construction, some of which are on the UI
// thread, so it stops after this many removals or this long, whichever comes
// first. What it leaves behind is oldest-first the next construction's.
constexpr size_t kSweepRemovals = 8;
constexpr std::chrono::milliseconds kSweepBudget{100};

} // namespace

const char* NeuralCachePromotionStageName(NeuralCachePromotion::Stage stage)
{
    switch (stage) {
    case NeuralCachePromotion::Stage::Published: return "published";
    case NeuralCachePromotion::Stage::Rejected: return "rejected-request";
    case NeuralCachePromotion::Stage::PayloadDigest: return "payload-digest";
    case NeuralCachePromotion::Stage::ManifestRejected: return "manifest-rejected";
    case NeuralCachePromotion::Stage::SidecarDigest: return "sidecar-digest";
    case NeuralCachePromotion::Stage::ManifestWrite: return "manifest-write";
    case NeuralCachePromotion::Stage::ManifestReread: return "manifest-reread";
    case NeuralCachePromotion::Stage::ExistingEntry: return "existing-entry";
    case NeuralCachePromotion::Stage::StagingCleanup: return "staging-cleanup";
    case NeuralCachePromotion::Stage::Move: return "rename";
    case NeuralCachePromotion::Stage::Reopen: return "reopen";
    }
    return "unknown";
}

const char* NeuralCacheFailureCauseName(NeuralCacheFailure::Cause cause)
{
    switch (cause) {
    case NeuralCacheFailure::Cause::None: return "none";
    case NeuralCacheFailure::Cause::NoWritableRoot: return "no-writable-root";
    case NeuralCacheFailure::Cause::InvalidKey: return "invalid-key";
    case NeuralCacheFailure::Cause::CreateFailed: return "create-failed";
    case NeuralCacheFailure::Cause::AlreadyExists: return "already-exists";
    case NeuralCacheFailure::Cause::OutsideRoot: return "outside-root";
    }
    return "unknown";
}

std::optional<std::string> Sha256Bytes(std::string_view bytes)
{
    return HashBytes(bytes);
}

std::optional<std::string> Sha256File(const std::filesystem::path& path, std::stop_token stop)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return std::nullopt;
    Sha256Hasher hasher;
    std::vector<uint8_t> buffer(1024 * 1024);
    while (input) {
        if (stop.stop_requested()) return std::nullopt;
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && !hasher.Update(std::span{buffer.data(), static_cast<size_t>(count)}))
            return std::nullopt;
    }
    if (!input.eof()) return std::nullopt;
    return hasher.Finish();
}

std::optional<std::string> Sha256FileCached(const std::filesystem::path& path, std::stop_token stop)
{
    struct Key {
        std::wstring path;
        uintmax_t size{};
        int64_t writeTime{};
        bool operator==(const Key&) const = default;
    };
    static std::mutex mutex;
    static std::vector<std::pair<Key, std::string>> memo;

    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return Sha256File(path, stop);
    const auto written = std::filesystem::last_write_time(path, error);
    if (error) return Sha256File(path, stop);
    const Key key{path.wstring(), size, written.time_since_epoch().count()};
    {
        std::lock_guard lock(mutex);
        for (const auto& [candidate, digest] : memo)
            if (candidate == key) return digest;
    }
    auto digest = Sha256File(path, stop);
    if (!digest) return digest;
    std::lock_guard lock(mutex);
    // The locked runtime is thirteen files and the model store adds a listing
    // of its own, all of which the render identity hashes once per request; a
    // cap below that set would clear the memo mid-pass and re-read every byte
    // on the next request in the same process.
    if (memo.size() >= 256) memo.clear();
    memo.emplace_back(key, *digest);
    return digest;
}

std::string NeuralRenderPipelineIdentity(bool gpuSourceConversion, uint32_t nvencPreset,
                                         bool gpuColorConversion)
{
    std::string pipeline =
        "DLAA|strict-timeline-v3|armed-inline-interception-v3|bt709-export-v1";
    if (gpuSourceConversion) pipeline += "|nv12-source-v1";
    // Appended only when they leave the shipped default, so a default render
    // keeps the exact term it was published under and no field cache is lost.
    // The preset is spelled out rather than bucketed as "non-default": p1 and
    // p7 are different encodes and must not share an entry.
    if (nvencPreset != kDefaultNvencPreset)
        pipeline += "|nvenc-p" + std::to_string(nvencPreset);
    if (gpuColorConversion != kDefaultGpuColorConversion) pipeline += "|nv12-output-v1";
    return pipeline;
}

std::string BuildNeuralCacheKey(const NeuralCacheIdentity& identity)
{
    std::string canonical;
    // Key schema 2: the correspondence-failure mask guide was deleted, so a
    // schema-1 entry was produced by a pipeline that still bound an R8 bias
    // mask to NGX and whose guide term had a third field. Those entries must
    // never be mistaken for matches, including the default-guides ones whose
    // key carried no guide term at all.
    AppendField(canonical, "schema", "2");
    AppendField(canonical, "source", identity.sourceDigest);
    AppendField(canonical, "width", std::to_string(identity.width));
    AppendField(canonical, "height", std::to_string(identity.height));
    AppendField(canonical, "application", identity.applicationVersion);
    AppendField(canonical, "gpu", identity.gpuPath);
    // The driver and model-store terms, like every term below them, are
    // appended only when set, so a source identity - which carries none of
    // them - keeps the key it was published under. `driver` closes the driver
    // crossing: gpuPath is a generation label, so without it a render made on
    // one driver was served, and validated, on every later one. `models`
    // closes the weight crossing: runtimeDigest covers the staged runtime
    // directory only, never the driver-store NGX core or the ProgramData model
    // store the pass resolves its weights out of.
    if (!identity.driverVersion.empty())
        AppendField(canonical, "driver", identity.driverVersion);
    AppendField(canonical, "runtime", identity.runtimeDigest);
    if (!identity.modelStoreDigest.empty())
        AppendField(canonical, "models", identity.modelStoreDigest);
    AppendField(canonical, "quality", identity.quality);
    AppendField(canonical, "upscaling", identity.upscaling ? "1" : "0");
    if (!identity.settingsDigest.empty())
        AppendField(canonical, "settings", identity.settingsDigest);
    if (!identity.range.Whole())
        AppendField(canonical, "range", std::to_string(identity.range.start100ns) + "-" +
                                        std::to_string(identity.range.end100ns));
    if (!identity.guides.empty())
        AppendField(canonical, "guides", identity.guides);
    return Sha256Bytes(canonical).value_or(std::string{});
}

std::optional<std::string> BuildRuntimeDigest(
    const std::filesystem::path& moduleDirectory,
    std::span<const std::wstring_view> relativeFiles,
    std::stop_token stop)
{
    std::vector<std::wstring> sorted;
    sorted.reserve(relativeFiles.size());
    for (const auto relative : relativeFiles) {
        std::filesystem::path path(relative);
        if (path.empty() || path.is_absolute() || relative.find(L"..") != std::wstring_view::npos)
            return std::nullopt;
        std::wstring normalized = path.generic_wstring();
        std::ranges::transform(normalized, normalized.begin(), towlower);
        sorted.push_back(std::move(normalized));
    }
    std::ranges::sort(sorted);
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
        return std::nullopt;
    std::string canonical;
    for (const auto& relative : sorted) {
        if (stop.stop_requested()) return std::nullopt;
        const auto digest = Sha256FileCached(moduleDirectory / relative, stop);
        if (!digest) return std::nullopt;
        const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, relative.data(),
                                                 static_cast<int>(relative.size()), nullptr, 0,
                                                 nullptr, nullptr);
        if (required <= 0) return std::nullopt;
        std::string utf8(static_cast<size_t>(required), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, relative.data(),
                                static_cast<int>(relative.size()), utf8.data(), required,
                                nullptr, nullptr) != required) return std::nullopt;
        canonical += utf8;
        canonical.push_back('\0');
        canonical += *digest;
        canonical.push_back('\n');
    }
    return Sha256Bytes(canonical);
}

std::string SerializeNeuralCacheManifest(const NeuralCacheManifest& manifest)
{
    std::string json = "{\"schema\":" + std::to_string(manifest.schema) +
        ",\"kind\":\"" + std::string(KindName(manifest.kind)) +
        "\",\"state\":\"" + std::string(StateName(manifest.state)) +
        "\",\"sourceDigest\":\"" + JsonEscape(manifest.sourceDigest) +
        "\",\"neuralDigest\":\"" + JsonEscape(manifest.neuralDigest) +
        "\",\"runtimeDigest\":\"" + JsonEscape(manifest.runtimeDigest) +
        "\",\"encoder\":\"" + JsonEscape(manifest.encoder) +
        "\",\"width\":" + std::to_string(manifest.width) +
        ",\"height\":" + std::to_string(manifest.height) +
        ",\"frameCount\":" + std::to_string(manifest.frameCount) +
        ",\"duration100ns\":" + std::to_string(manifest.duration100ns) +
        ",\"nativeEvaluations\":" + std::to_string(manifest.nativeEvaluations) +
        ",\"verifiedNeuralFrames\":" + std::to_string(manifest.verifiedNeuralFrames) +
        ",\"observedFeature18Evaluations\":" +
            std::to_string(manifest.observedFeature18Evaluations) +
        ",\"feature18Created\":" + (manifest.feature18Created ? "true" : "false") +
        ",\"feature18ArmedBeforeCapture\":" +
            (manifest.feature18ArmedBeforeCapture ? "true" : "false") +
        ",\"upscaling\":" + (manifest.upscaling ? "true" : "false");
    if (manifest.schema == kLegacySchema) {
        // Schema 3 keeps its single optional extension byte-for-byte.
        if (!manifest.settingsDigest.empty())
            json += ",\"settingsDigest\":\"" + JsonEscape(manifest.settingsDigest) + "\"";
        return json + "}\n";
    }
    json += ",\"settingsDigest\":\"" + JsonEscape(manifest.settingsDigest) +
        "\",\"rangeStart100ns\":" + std::to_string(manifest.rangeStart100ns) +
        ",\"rangeEnd100ns\":" + std::to_string(manifest.rangeEnd100ns) +
        ",\"guides\":\"" + JsonEscape(manifest.guides) +
        "\",\"jobId\":" + std::to_string(manifest.jobId) +
        ",\"historyResets\":" + std::to_string(manifest.historyResets) +
        ",\"receiptDigest\":\"" + JsonEscape(manifest.receiptDigest) + "\"}\n";
    return json;
}

std::optional<NeuralCacheManifest> ParseNeuralCacheManifest(std::string_view bytes)
{
    JsonCursor cursor(bytes);
    NeuralCacheManifest manifest;
    std::string kind;
    std::string state;
    if (!cursor.Expect('{') ||
        !ReadIntegerField(cursor, "schema", manifest.schema) ||
        !ReadStringField(cursor, "kind", kind) ||
        !ReadStringField(cursor, "state", state) ||
        !ReadStringField(cursor, "sourceDigest", manifest.sourceDigest) ||
        !ReadStringField(cursor, "neuralDigest", manifest.neuralDigest) ||
        !ReadStringField(cursor, "runtimeDigest", manifest.runtimeDigest) ||
        !ReadStringField(cursor, "encoder", manifest.encoder) ||
        !ReadIntegerField(cursor, "width", manifest.width) ||
        !ReadIntegerField(cursor, "height", manifest.height) ||
        !ReadIntegerField(cursor, "frameCount", manifest.frameCount) ||
        !ReadIntegerField(cursor, "duration100ns", manifest.duration100ns) ||
        !ReadIntegerField(cursor, "nativeEvaluations", manifest.nativeEvaluations) ||
        !ReadIntegerField(cursor, "verifiedNeuralFrames", manifest.verifiedNeuralFrames) ||
        !ReadIntegerField(cursor, "observedFeature18Evaluations",
                          manifest.observedFeature18Evaluations) ||
        !ReadBoolField(cursor, "feature18Created", manifest.feature18Created) ||
        !ReadBoolField(cursor, "feature18ArmedBeforeCapture",
                       manifest.feature18ArmedBeforeCapture) ||
        !ReadBoolField(cursor, "upscaling", manifest.upscaling, false)) return std::nullopt;
    if (manifest.schema == kLegacySchema) {
        // Schema 3's only optional extension. Reject duplicate/unknown fields.
        if (cursor.Expect(',') &&
            !ReadStringField(cursor, "settingsDigest", manifest.settingsDigest, false))
            return std::nullopt;
    } else if (manifest.schema == kSchema) {
        // Schema 5 is fixed and ordered; every field is required.
        if (!cursor.Expect(',') ||
            !ReadStringField(cursor, "settingsDigest", manifest.settingsDigest) ||
            !ReadIntegerField(cursor, "rangeStart100ns", manifest.rangeStart100ns) ||
            !ReadIntegerField(cursor, "rangeEnd100ns", manifest.rangeEnd100ns) ||
            !ReadStringField(cursor, "guides", manifest.guides) ||
            !ReadIntegerField(cursor, "jobId", manifest.jobId) ||
            !ReadIntegerField(cursor, "historyResets", manifest.historyResets) ||
            !ReadStringField(cursor, "receiptDigest", manifest.receiptDigest, false))
            return std::nullopt;
    } else {
        // Schema 4 included, whose field list schema 5 keeps: those entries are
        // refused for their schema rather than for a field they are missing,
        // because the identity they were keyed under named neither the driver
        // nor the model store the render was produced against.
        return std::nullopt;
    }
    if (!cursor.Expect('}') || !cursor.Finished()) return std::nullopt;

    if (kind == "source") manifest.kind = NeuralCacheEntryKind::Source;
    else if (kind == "render") manifest.kind = NeuralCacheEntryKind::Render;
    else return std::nullopt;
    if (state == "staging") manifest.state = NeuralCacheState::Staging;
    else if (state == "complete") manifest.state = NeuralCacheState::Complete;
    else if (state == "invalid") manifest.state = NeuralCacheState::Invalid;
    else return std::nullopt;
    if (!CommonManifestFieldsValid(manifest)) return std::nullopt;
    return manifest;
}

bool IsReusableNeuralCacheManifest(const NeuralCacheManifest& manifest)
{
    if (manifest.state != NeuralCacheState::Complete || !CommonManifestFieldsValid(manifest))
        return false;
    if (manifest.kind == NeuralCacheEntryKind::Source) {
        return IsHexDigest(manifest.sourceDigest) && manifest.neuralDigest.empty() &&
               !manifest.feature18Created;
    }
    // A render entry is handed back as verified neural output out of a
    // directory the user can write to, and receipt.json is the only thing that
    // vouches for how it was produced; accepting an entry without one serves a
    // render on the strength of a manifest that merely claims to be verified.
    // The digest was allowed to be empty because it arrived with the rest of
    // the schema-4 field list, which schema 5 keeps, beside fields that really
    // are optional - but no release ever wrote such a render without it: the
    // render path builds the receipt, stages receipt.json and fails the render
    // when it cannot, so an empty digest is a shape this player has never
    // produced. Legacy schema-3 entries predate receipts and are required to
    // carry no digest at all (CommonManifestFieldsValid); sources never carry
    // one either.
    // nativeEvaluations is the backend's own count of evaluations while the
    // frames were captured - a second witness to frameCount, not a copy of it.
    // Resubmits (the receipt gate, frame retries) evaluate a frame more than
    // once, so it may exceed the frame count; it may never fall short of it.
    return IsHexDigest(manifest.sourceDigest) && IsHexDigest(manifest.neuralDigest) &&
           IsHexDigest(manifest.runtimeDigest) && manifest.feature18Created &&
           manifest.feature18ArmedBeforeCapture &&
           (manifest.schema == kLegacySchema || IsHexDigest(manifest.receiptDigest)) &&
           manifest.nativeEvaluations >= manifest.frameCount &&
           manifest.verifiedNeuralFrames == manifest.frameCount &&
           manifest.observedFeature18Evaluations > 0;
}

std::optional<std::filesystem::path> NeuralCacheManager::DefaultRoot()
{
    const auto directory = platform_paths::ModuleDirectory();
    if (!directory) return std::nullopt;
    return *directory / L"cache" / L"v1";
}

std::optional<std::filesystem::path> NeuralCacheManager::LegacyDefaultRoot()
{
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                    &localAppData)) || !localAppData) return std::nullopt;
    std::filesystem::path result = std::filesystem::path(localAppData) /
        L"DLSSVideoPlayer" / L"NeuralCache" / L"v1";
    CoTaskMemFree(localAppData);
    return result;
}

std::optional<std::filesystem::path> NeuralCacheManager::ResolvedLegacyDefaultRoot()
{
    const auto legacy = LegacyDefaultRoot();
    if (!legacy) return std::nullopt;
    std::error_code error;
    if (!std::filesystem::is_directory(*legacy, error) || error) return std::nullopt;
    return ResolveWritableRoot(*legacy, error);
}

NeuralCacheManager::NeuralCacheManager(std::filesystem::path root)
{
    std::optional<std::filesystem::path> writableRoot;
    if (!root.empty()) {
        writableRoot = PrepareWritableRoot(root, failure_);
    } else {
        // An install directory the user cannot write to is the whole point of
        // the fallback, so the portable attempt only decides whether
        // LocalAppData is tried; its verdict is reported when that fails too.
        NeuralCacheFailure portableFailure;
        if (const auto portable = DefaultRoot())
            writableRoot = PrepareWritableRoot(*portable, portableFailure);
        if (!writableRoot) {
            if (const auto fallback = LegacyDefaultRoot())
                writableRoot = PrepareWritableRoot(*fallback, failure_);
            else failure_ = portableFailure;
        }
    }
    if (!writableRoot) {
        LogCacheFailure("Neural cache root unavailable", failure_);
        return;
    }
    root_ = *writableRoot;
    valid_ = true;
    SweepStaging();
}

size_t NeuralCacheManager::SweepStaging()
{
    if (!valid_) return 0;
    const auto started = std::chrono::steady_clock::now();
    struct Candidate {
        std::filesystem::path path;
        std::filesystem::file_time_type written;
    };
    std::vector<Candidate> candidates;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(root_ / L"staging", error), end;
         !error && iterator != end; iterator.increment(error)) {
        const std::wstring name = iterator->path().filename().wstring();
        DWORD pid = 0;
        if (!ParseStagingOwner(name, pid)) continue;
        // An entry set aside as invalid has nothing left to reference it, so it
        // goes whoever made it; a partial payload still being written belongs
        // to the live process whose pid it carries.
        if (!name.starts_with(L"invalid") && ProcessAlive(pid)) continue;
        std::error_code timeError;
        const auto written = iterator->last_write_time(timeError);
        if (timeError) continue;
        candidates.push_back({iterator->path(), written});
    }
    if (candidates.empty()) return 0;
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.written < b.written; });
    size_t removed = 0;
    for (const Candidate& candidate : candidates) {
        if (removed >= kSweepRemovals ||
            std::chrono::steady_clock::now() - started >= kSweepBudget) break;
        if (!OwnsPath(candidate.path)) continue;
        std::error_code removeError;
        std::filesystem::remove_all(candidate.path, removeError);
        if (!removeError) ++removed;
    }
    const double elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    LOG("Neural cache staging swept: removed=" << removed
        << " remaining=" << (candidates.size() - removed) << " ms=" << elapsedMs);
    return removed;
}

bool NeuralCacheManager::ValidKey(std::string_view key)
{
    return IsHexDigest(key);
}

bool NeuralCacheManager::OwnsPath(const std::filesystem::path& path) const
{
    if (!valid_) return false;
    std::error_code rootError;
    std::error_code pathError;
    const auto canonicalRoot = CanonicalOrAbsolute(root_, rootError);
    const auto canonicalPath = CanonicalOrAbsolute(path, pathError);
    return !rootError && !pathError && IsStrictDescendant(canonicalRoot, canonicalPath);
}

std::optional<std::filesystem::path> NeuralCacheManager::BeginStaging(
    NeuralCacheEntryKind kind, std::string_view key)
{
    // An invalid manager already holds the reason it never became one.
    if (!valid_) return std::nullopt;
    const std::filesystem::path staging = root_ / L"staging";
    if (!ValidKey(key)) {
        failure_ = {NeuralCacheFailure::Cause::InvalidKey, {}, staging};
    } else {
        const uint64_t nonce = ++g_stagingNonce;
        const std::wstring prefix = kind == NeuralCacheEntryKind::Source ? L"source-" : L"render-";
        const std::filesystem::path directory = staging /
            (prefix + std::wstring(key.begin(), key.end()) + L"-" +
             std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(nonce));
        std::error_code error;
        const bool created = std::filesystem::create_directories(directory, error);
        // The name carries a per-process nonce, so a directory that is already
        // there is an anomaly rather than a race and is still refused; the
        // error is what separates it from a directory nothing may create.
        if (error) failure_ = {NeuralCacheFailure::Cause::CreateFailed, error, directory};
        else if (!created) failure_ = {NeuralCacheFailure::Cause::AlreadyExists, {}, directory};
        else if (!OwnsPath(directory)) failure_ = {NeuralCacheFailure::Cause::OutsideRoot, {}, directory};
        else {
            failure_ = {};
            return directory;
        }
    }
    LogCacheFailure(std::string("Neural ") + std::string(KindName(kind)) + " staging refused",
                    failure_);
    return std::nullopt;
}

std::optional<std::filesystem::path> NeuralCacheManager::BeginSourceStaging(std::string_view key)
{
    return BeginStaging(NeuralCacheEntryKind::Source, key);
}

std::optional<std::filesystem::path> NeuralCacheManager::SourcePayloadPath(std::string_view key) const
{
    if (!valid_ || !ValidKey(key)) return std::nullopt;
    const std::filesystem::path directory = root_ / L"sources" /
        std::wstring(key.begin(), key.end());
    if (!OwnsPath(directory)) return std::nullopt;
    return directory / L"source.mkv";
}

std::optional<std::filesystem::path> NeuralCacheManager::BeginRenderStaging(std::string_view key)
{
    return BeginStaging(NeuralCacheEntryKind::Render, key);
}

std::optional<NeuralCacheEntry> NeuralCacheManager::Lookup(
    NeuralCacheEntryKind kind, std::string_view key) const
{
    if (!valid_ || !ValidKey(key)) return std::nullopt;
    const std::filesystem::path directory = root_ /
        (kind == NeuralCacheEntryKind::Source ? L"sources" : L"renders") /
        std::wstring(key.begin(), key.end());
    if (!OwnsPath(directory)) return std::nullopt;
    const auto manifestPath = directory / L"manifest.json";
    std::ifstream input(manifestPath, std::ios::binary);
    if (!input.is_open()) return std::nullopt;
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    const auto manifest = ParseNeuralCacheManifest(bytes);
    if (!manifest || manifest->kind != kind || !IsReusableNeuralCacheManifest(*manifest))
        return std::nullopt;
    if (!manifest->settingsDigest.empty() &&
        Sha256File(directory / L"neural-settings.ini") != manifest->settingsDigest)
        return std::nullopt;
    // A reusable render always has a receipt digest, so the empty case below is
    // only ever reached by sources and legacy schema-3 entries, which have no
    // receipt to authenticate.
    if (!manifest->receiptDigest.empty() &&
        Sha256File(directory / L"receipt.json") != manifest->receiptDigest)
        return std::nullopt;
    const auto payload = directory /
        (kind == NeuralCacheEntryKind::Source ? L"source.mkv" : L"neural.mkv");
    const auto digest = Sha256File(payload);
    if (!digest) return std::nullopt;
    const std::string& expected = kind == NeuralCacheEntryKind::Source
        ? manifest->sourceDigest : manifest->neuralDigest;
    if (*digest != expected) return std::nullopt;
    return NeuralCacheEntry{directory, payload, *manifest};
}

std::optional<NeuralCacheEntry> NeuralCacheManager::LookupSource(std::string_view key) const
{
    return Lookup(NeuralCacheEntryKind::Source, key);
}

std::optional<NeuralCacheEntry> NeuralCacheManager::LookupRender(std::string_view key) const
{
    return Lookup(NeuralCacheEntryKind::Render, key);
}

bool NeuralCacheManager::Promote(NeuralCacheEntryKind kind, std::string_view key,
                                 const std::filesystem::path& staging,
                                 NeuralCacheManifest manifest,
                                 NeuralCachePromotion* diagnostic)
{
    NeuralCachePromotion report{};
    const auto fail = [&](NeuralCachePromotion::Stage stage) {
        report.stage = stage;
        if (diagnostic) *diagnostic = report;
        return false;
    };
    if (!valid_ || !ValidKey(key) || !OwnsPath(staging) ||
        staging.parent_path().filename() != L"staging")
        return fail(NeuralCachePromotion::Stage::Rejected);
    const auto payload = staging /
        (kind == NeuralCacheEntryKind::Source ? L"source.mkv" : L"neural.mkv");
    const auto digest = Sha256File(payload);
    if (!digest) return fail(NeuralCachePromotion::Stage::PayloadDigest);
    manifest.kind = kind;
    manifest.state = NeuralCacheState::Complete;
    manifest.schema = kSchema;
    if (kind == NeuralCacheEntryKind::Source) {
        manifest.sourceDigest = *digest;
        manifest.neuralDigest.clear();
        manifest.runtimeDigest.clear();
        manifest.settingsDigest.clear();
        manifest.nativeEvaluations = 0;
        manifest.verifiedNeuralFrames = 0;
        manifest.observedFeature18Evaluations = 0;
        manifest.feature18Created = false;
        manifest.feature18ArmedBeforeCapture = false;
        manifest.rangeStart100ns = 0;
        manifest.rangeEnd100ns = 0;
        manifest.guides.clear();
        manifest.jobId = 0;
        manifest.historyResets = 0;
        manifest.receiptDigest.clear();
    } else {
        manifest.neuralDigest = *digest;
    }
    if (!IsReusableNeuralCacheManifest(manifest))
        return fail(NeuralCachePromotion::Stage::ManifestRejected);
    if (!manifest.settingsDigest.empty() &&
        Sha256File(staging / L"neural-settings.ini") != manifest.settingsDigest)
        return fail(NeuralCachePromotion::Stage::SidecarDigest);
    if (!manifest.receiptDigest.empty() &&
        Sha256File(staging / L"receipt.json") != manifest.receiptDigest)
        return fail(NeuralCachePromotion::Stage::SidecarDigest);
    const auto manifestPath = staging / L"manifest.json";
    {
        std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) return fail(NeuralCachePromotion::Stage::ManifestWrite);
        const std::string serialized = SerializeNeuralCacheManifest(manifest);
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        if (!output.good()) return fail(NeuralCachePromotion::Stage::ManifestWrite);
    }
    {
        std::ifstream input(manifestPath, std::ios::binary);
        const std::string serialized{std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>()};
        const auto reparsed = ParseNeuralCacheManifest(serialized);
        if (!reparsed || *reparsed != manifest || !IsReusableNeuralCacheManifest(*reparsed))
            return fail(NeuralCachePromotion::Stage::ManifestReread);
    }

    const auto destination = root_ /
        (kind == NeuralCacheEntryKind::Source ? L"sources" : L"renders") /
        std::wstring(key.begin(), key.end());
    if (auto existing = Lookup(kind, key)) {
        std::error_code cleanupError;
        std::filesystem::remove_all(staging, cleanupError);
        if (cleanupError) return fail(NeuralCachePromotion::Stage::StagingCleanup);
        report.entry = std::move(existing);
        if (diagnostic) *diagnostic = std::move(report);
        return true;
    }
    std::error_code existsError;
    if (std::filesystem::exists(destination, existsError)) {
        if (existsError) return fail(NeuralCachePromotion::Stage::ExistingEntry);
        if (!MoveToInvalidDirectory(root_, destination, L"invalid-existing"))
            return fail(NeuralCachePromotion::Stage::ExistingEntry);
    }
    if (!RenameDirectory(staging, destination, &report.win32Error, &report.attempts,
                         publishRetryObserver_))
        return fail(NeuralCachePromotion::Stage::Move);
    // The payload was hashed and the manifest reread in staging a moment ago;
    // a rename moves the directory's contents byte for byte, so the reopen
    // only has to confirm they arrived. Hashing a multi-hundred-megabyte
    // payload a second time here proved nothing the first pass had not.
    const auto payloadName = payload.filename();
    {
        std::ifstream input(destination / L"manifest.json", std::ios::binary);
        const std::string serialized{std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>()};
        const auto reopened = ParseNeuralCacheManifest(serialized);
        std::error_code payloadError;
        if (!reopened || *reopened != manifest ||
            !std::filesystem::is_regular_file(destination / payloadName, payloadError) ||
            payloadError)
            return fail(NeuralCachePromotion::Stage::Reopen);
    }
    report.entry = NeuralCacheEntry{destination, destination / payloadName, std::move(manifest)};
    if (diagnostic) *diagnostic = std::move(report);
    return true;
}

bool NeuralCacheManager::PromoteSource(std::string_view key,
                                       const std::filesystem::path& staging,
                                       NeuralCacheManifest manifest,
                                       NeuralCachePromotion* diagnostic)
{
    return Promote(NeuralCacheEntryKind::Source, key, staging, std::move(manifest), diagnostic);
}

bool NeuralCacheManager::PromoteRender(std::string_view key,
                                       const std::filesystem::path& staging,
                                       NeuralCacheManifest manifest,
                                       NeuralCachePromotion* diagnostic)
{
    return Promote(NeuralCacheEntryKind::Render, key, staging, std::move(manifest), diagnostic);
}

bool NeuralCacheManager::MarkInvalid(const std::filesystem::path& staging)
{
    if (!OwnsPath(staging) || staging.parent_path().filename() != L"staging") return false;
    return MoveToInvalidDirectory(root_,staging,L"invalid");
}

bool NeuralCacheManager::Quarantine(const NeuralCacheEntry& entry)
{
    if (!valid_ || !OwnsPath(entry.directory)) return false;
    const auto parent = entry.directory.parent_path().filename();
    if (parent != L"sources" && parent != L"renders") return false;
    return MoveToInvalidDirectory(root_,entry.directory,L"invalid-cache");
}

bool NeuralCacheManager::Remove(NeuralCacheEntryKind kind, std::string_view key)
{
    if (!valid_ || !ValidKey(key)) return false;
    const auto directory = root_ /
        (kind == NeuralCacheEntryKind::Source ? L"sources" : L"renders") /
        std::wstring(key.begin(), key.end());
    if (!OwnsPath(directory)) return false;
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return !error;
}

bool NeuralCacheManager::RemoveSource(std::string_view key)
{
    return Remove(NeuralCacheEntryKind::Source, key);
}

bool NeuralCacheManager::RemoveRender(std::string_view key)
{
    return Remove(NeuralCacheEntryKind::Render, key);
}

uintmax_t NeuralCacheManager::SizeBytes() const
{
    if (!valid_) return 0;
    uintmax_t total = 0;
    size_t unreadable = 0;
    std::error_code walkError;
    for (std::filesystem::recursive_directory_iterator iterator(
             root_, std::filesystem::directory_options::skip_permission_denied, walkError), end;
         !walkError && iterator != end; iterator.increment(walkError)) {
        // Per entry, and never fatal. One shared error_code that was never
        // cleared used to make a single unmeasurable file report the WHOLE
        // cache as zero bytes - so the Clear prompt offered to free 0 MiB of a
        // 40 GB cache. A file that vanishes mid-walk, which the staging sweep
        // can cause, was enough.
        std::error_code entryError;
        if (iterator->is_regular_file(entryError) && !entryError) {
            const uintmax_t bytes = iterator->file_size(entryError);
            if (entryError) ++unreadable;
            else total += bytes;
        } else if (entryError) {
            ++unreadable;
        }
    }
    if (unreadable)
        LOG("Cache size: " << unreadable << " entr" << (unreadable == 1 ? "y" : "ies")
            << " could not be measured and are not counted in " << total << " bytes.");
    return total;
}

NeuralCacheManager::EvictionReport NeuralCacheManager::Evict(
    std::span<const std::string> activeKeys, uintmax_t freeFloorBytes)
{
    EvictionReport report;
    if (!valid_) return report;
    const auto renders = root_ / L"renders";
    std::error_code error;
    if (!std::filesystem::is_directory(renders, error)) return report;

    std::vector<cache_eviction::Entry> entries;
    for (const auto& child : std::filesystem::directory_iterator(
             renders, std::filesystem::directory_options::skip_permission_denied, error)) {
        std::error_code entryError;
        if (!child.is_directory(entryError) || entryError) continue;
        // A key is a hex digest, so anything that is not plain ASCII is not
        // one of ours and is left alone rather than narrowed into something
        // that might collide with one.
        const auto key = narrow_text::StrictAscii(child.path().filename().wstring());
        if (!key || !ValidKey(*key)) continue;

        cache_eviction::Entry entry;
        entry.key = *key;
        entry.active = std::ranges::find(activeKeys, *key) != activeKeys.end();

        // Unparsable or unreadable manifests count as unreachable: lookup
        // refuses them too, so they are occupying space for nothing.
        std::string manifestBytes;
        if (std::ifstream input(child.path() / L"manifest.json", std::ios::binary); input)
            manifestBytes.assign(std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>());
        const auto manifest = ParseNeuralCacheManifest(manifestBytes);
        entry.reusable = manifest && IsReusableNeuralCacheManifest(*manifest);

        // Last use is the newest timestamp in the entry, so serving a render
        // keeps it alive only if something touches it. Nothing does today, so
        // in practice this orders by when the entry was written - which is
        // still a far better answer than an arbitrary one.
        for (std::filesystem::recursive_directory_iterator file(
                 child.path(), std::filesystem::directory_options::skip_permission_denied,
                 entryError), end;
             !entryError && file != end; file.increment(entryError)) {
            std::error_code fileError;
            if (!file->is_regular_file(fileError) || fileError) continue;
            const uintmax_t bytes = file->file_size(fileError);
            if (!fileError) entry.bytes += bytes;
            const auto written = file->last_write_time(fileError);
            if (!fileError)
                entry.lastUsed = std::max(entry.lastUsed, written.time_since_epoch().count());
        }
        entries.push_back(std::move(entry));
    }

    uintmax_t freeBytes = 0;
    if (freeFloorBytes) {
        const auto space = std::filesystem::space(root_, error);
        freeBytes = error ? freeFloorBytes : space.available;   // unknown: assume no pressure
        if (error) LOG("Cache eviction could not read free space; skipping the pressure pass.");
    }

    const auto plan = cache_eviction::PlanEviction(entries, freeBytes, freeFloorBytes);
    report.freeSpaceFloorMet = plan.floorMet;
    for (const std::string& key : plan.evict) {
        const auto matched = std::ranges::find(entries, key, &cache_eviction::Entry::key);
        const bool unreachable = matched != entries.end() && !matched->reusable;
        if (!RemoveRender(key)) { ++report.failures; continue; }
        if (matched != entries.end()) report.freedBytes += matched->bytes;
        if (unreachable) ++report.unreachableRemoved;
        else ++report.leastRecentlyUsedRemoved;
    }
    if (!plan.evict.empty() || report.failures)
        LOG("Cache eviction: removed " << report.unreachableRemoved
            << " unreachable and " << report.leastRecentlyUsedRemoved
            << " least-recently-used render(s), freeing " << report.freedBytes
            << " bytes; " << report.failures << " could not be removed."
            << (plan.floorMet ? "" : " The free-space floor was still not met."));
    return report;
}

bool NeuralCacheManager::Clear()
{
    if (!valid_) return false;
    // frame-generation holds the player's converted videos. Leaving it out made
    // the Clear prompt lie: SizeBytes recurses the whole root, so the dialog
    // offered to free bytes it then kept, and generated files accumulated with
    // no surface in the player able to delete them.
    //
    // `live` is here for exactly the same reason and was missed the first time
    // round. It holds each session's published segments; SizeBytes counts them
    // and Clear did not remove them, so the dialog over-promised again by
    // however much live rendering the user had done.
    for (const auto name : {L"sources", L"renders", L"staging", L"frame-generation", L"live"}) {
        const auto target = root_ / name;
        if (!OwnsPath(target)) return false;
        std::error_code error;
        std::filesystem::remove_all(target, error);
        if (error) return false;
        std::filesystem::create_directories(target, error);
        if (error) return false;
    }
    return true;
}
