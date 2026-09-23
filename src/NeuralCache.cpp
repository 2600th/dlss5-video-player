#include "NeuralCache.h"
#include "CacheEvictionPolicy.h"
#include "NarrowText.h"
#include "PlatformPaths.h"
#include "GuideControls.h"
#include "JsonEscape.h"
#include "Utf8Text.h"
#include "LiveSessionPolicy.h"
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
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
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
            // What JsonEscape writes for the other control characters, and
            // nothing else: the writer never produces any other \u escape.
            case 'u': {
                if (bytes_.size() - position_ < 4 || bytes_.substr(position_, 2) != "00")
                    return std::nullopt;
                int value = 0;
                for (const char digit : bytes_.substr(position_ + 2, 2)) {
                    const int nibble = digit >= '0' && digit <= '9' ? digit - '0'
                                     : digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 : -1;
                    if (nibble < 0) return std::nullopt;
                    value = value * 16 + nibble;
                }
                if (value >= 0x20) return std::nullopt;
                result.push_back(static_cast<char>(value));
                position_ += 4;
                break;
            }
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

// A recorded environment is all or nothing, and only a render has one: a
// source key carries none of these terms. The installation may be empty - a
// publisher that could not read its own module path still records the rest,
// and eviction then never treats the entry as this installation's.
bool EnvironmentValid(const NeuralCacheManifest& manifest)
{
    const NeuralCacheEnvironment& environment = manifest.environment;
    if (!environment.Recorded()) return true;
    return manifest.schema == kSchema && manifest.kind == NeuralCacheEntryKind::Render &&
           !environment.application.empty() && !environment.driver.empty() &&
           IsHexDigest(environment.modelStore);
}

bool CommonManifestFieldsValid(const NeuralCacheManifest& manifest)
{
    if (manifest.schema != kSchema && manifest.schema != kLegacySchema) return false;
    if (manifest.schema == kLegacySchema &&
        (manifest.rangeStart100ns != 0 || manifest.rangeEnd100ns != 0 ||
         !manifest.guides.empty() || manifest.jobId != 0 || manifest.historyResets != 0 ||
         !manifest.receiptDigest.empty())) return false;
    if (!EnvironmentValid(manifest)) return false;
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

// One line per refusal. The only field report of an uncreatable cache carried
// the player's status string and nothing else, so the path, the cause, the
// filesystem error and the ownership verdict all belong on it.
void LogCacheFailure(std::string_view what, const NeuralCacheFailure& failure)
{
    LOG(what << ": cause=" << NeuralCacheFailureCauseName(failure.cause)
             << " path=" << utf8_text::FromWide(failure.path.native())
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

// The player is not long-path aware, so a staging directory - a 64-character
// key plus pid and nonce - and the sidecars written into it fail with error=3
// once the root passes roughly 140 characters. The root itself was accepted,
// so a deep portable folder or a long custom root looked usable and then
// failed every render at staging. This creates the deepest shape the cache
// writes (an "invalid-" name, one character longer than "render-" and reaped
// by any sweep if this process dies here, holding a file named past the
// longest sidecar to leave room for the nonce to grow) and refuses the root
// when the system will not, so the default falls back to LocalAppData and an
// explicit root is refused up front. Probing rather than counting follows
// whatever path limit the process actually has.
bool DeepestStagingPathFits(const std::filesystem::path& root, NeuralCacheFailure& failure)
{
    const auto directory = root / L"staging" /
        (L"invalid-" + std::wstring(64, L'0') + L"-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(++g_stagingNonce));
    DWORD error = ERROR_SUCCESS;
    if (!CreateDirectoryW(directory.c_str(), nullptr)) {
        error = GetLastError();
    } else {
        const HANDLE file = CreateFileW((directory / L"neural-settings.ini~probe").c_str(),
            GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE) error = GetLastError();
        else CloseHandle(file);
        RemoveDirectoryW(directory.c_str());
    }
    if (error == ERROR_SUCCESS) return true;
    failure.error.assign(static_cast<int>(error), std::system_category());
    failure.path = directory;
    return false;
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
    if (!DeepestStagingPathFits(*writableRoot, failure)) return std::nullopt;
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

// `patient` retries the sharing errors a scanner causes. Eviction and Clear
// pass false: an entry something has open is one they must leave whole, and a
// single refused rename is exactly how they find that out.
bool MoveToInvalidDirectory(const std::filesystem::path& root,
                            const std::filesystem::path& source,
                            std::wstring_view prefix,
                            std::filesystem::path* moved = nullptr,
                            bool patient = true)
{
    for(size_t attempt=0;attempt<128;++attempt){
        const auto destination=root/L"staging"/
            (std::wstring(prefix)+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+
             std::to_wstring(++g_stagingNonce));
        DWORD error=ERROR_SUCCESS;
        const bool renamed=patient?RenameDirectory(source,destination,&error)
                                  :MoveFileExW(source.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
        if(!patient&&!renamed)error=GetLastError();
        if(renamed){if(moved)*moved=destination;return true;}
        if(error!=ERROR_ALREADY_EXISTS&&error!=ERROR_FILE_EXISTS)return false;
    }
    return false;
}

// Removes a published entry without ever leaving half of one. remove_all
// deletes file by file, so an entry whose payload another process was playing
// lost its manifest and receipt, kept the payload it could not delete, and
// was left as a directory lookup refuses and nothing ever cleans up. A rename
// is all or nothing: while anything inside is open the directory cannot move,
// and the entry stays whole for whoever has it open. Once moved it is
// unreachable, and what cannot be deleted now is an invalid-* staging
// directory the next sweep reaps.
bool RetireEntryDirectory(const std::filesystem::path& root,
                          const std::filesystem::path& directory,
                          std::wstring_view prefix)
{
    std::filesystem::path moved;
    if (!MoveToInvalidDirectory(root, directory, prefix, &moved, false)) return false;
    std::error_code error;
    std::filesystem::remove_all(moved, error);
    return true;
}

// Serializes the operations that restructure a cache root - Evict, Clear,
// Promote - across every player instance that shares it, and the last-use mark
// a lookup leaves. The cache is per root, not per process: a second instance
// running Clear() deleted the first one's live segments, and one instance's
// eviction could remove the entry another was opening.
//
// Named after the root rather than stored in it, so there is nothing on disk
// to go stale when a process dies: Windows abandons the mutex, and the next
// waiter owns it (WAIT_ABANDONED) with nothing to recover - every operation
// under it leaves the root consistent at each step. `Local\` scopes it to the
// logon session, which is what shares a LocalAppData root.
//
// Every wait is bounded. The UI thread asks for this (Clear, and the source
// lookups the toolbar makes), and it must never hang on another instance; a
// caller that does not get the lock in time skips or degrades, and says so.
class CacheRootLock {
public:
    CacheRootLock(const std::filesystem::path& root, DWORD waitMs)
    {
        mutex_ = CreateMutexW(nullptr, FALSE, NeuralCacheRootLockName(root).c_str());
        if (!mutex_) return;
        const DWORD wait = WaitForSingleObject(mutex_, waitMs);
        held_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }
    ~CacheRootLock()
    {
        if (held_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }
    CacheRootLock(const CacheRootLock&) = delete;
    CacheRootLock& operator=(const CacheRootLock&) = delete;
    bool Held() const { return held_; }

private:
    HANDLE mutex_{};
    bool held_{};
};

// The waits. A lookup holds the lock only for the mark it leaves, and Clear
// runs on the UI thread, so both are short enough not to be felt. Eviction is
// on its own thread and waits per entry. Promotion is on a render's worker at
// the end of minutes of GPU time, so it waits long - and then publishes
// anyway: the only holder that could take that long is a Clear, which the
// user asked for, and throwing a finished render away is the worse outcome.
constexpr DWORD kLookupLockWaitMs = 100;
constexpr DWORD kClearLockWaitMs = 250;
constexpr DWORD kEvictLockWaitMs = 2000;
constexpr DWORD kPromoteLockWaitMs = 30000;

// Last use is the entry directory's own write time. Nothing recorded a use, so
// "least recently used" was really "oldest written": a film watched every day
// was evicted before one rendered last week and never opened. Stamping the
// directory costs one handle and writes no file into the entry. Never allowed
// to fail a lookup: a mark that cannot be written leaves the entry as old as
// it was.
void MarkEntryUsed(const std::filesystem::path& directory)
{
    const HANDLE handle = CreateFileW(directory.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    SetFileTime(handle, nullptr, nullptr, &now);
    CloseHandle(handle);
}

std::optional<int64_t> DirectoryWriteTime(const std::filesystem::path& directory)
{
    std::error_code error;
    const auto written = std::filesystem::last_write_time(directory, error);
    if (error) return std::nullopt;
    return written.time_since_epoch().count();
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

// remove_all is one call, so the sweep's budget was checked only between
// directories: a partial render of several gigabytes and hundreds of segment
// files was deleted in one go, on the UI thread, whatever the budget said.
// This deletes file by file and stops at the deadline; the directory keeps its
// name, so the next sweep finishes it. Directory symlinks and junctions are
// removed as links, never followed.
bool RemoveTreeBefore(const std::filesystem::path& root,
                      std::chrono::steady_clock::time_point deadline)
{
    namespace fs = std::filesystem;
    std::error_code error;
    std::vector<fs::path> directories;
    for (fs::recursive_directory_iterator iterator(root, fs::directory_options::none, error), end;
         !error && iterator != end; iterator.increment(error)) {
        std::error_code local;
        if (fs::is_directory(iterator->symlink_status(local)) && !local) {
            directories.push_back(iterator->path());
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        fs::remove(iterator->path(), local);
    }
    for (auto directory = directories.rbegin(); directory != directories.rend(); ++directory) {
        std::error_code local;
        fs::remove(*directory, local);
    }
    std::error_code local;
    fs::remove(root, local);
    return !fs::exists(root, local) && !local;
}

// Written into a quarantined entry when it is set aside. It says what put it
// there and when, for whoever inspects it, and it moves the directory's write
// time to the moment of quarantine, which is what the retention is measured
// from (a rename does not change a directory's own times).
void NoteQuarantine(const std::filesystem::path& directory, std::wstring_view reason)
{
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char stamp[32]{};
    std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear, now.wMonth,
                  now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::ofstream note(directory / L"quarantine.txt", std::ios::binary | std::ios::trunc);
    note << "reason=" << utf8_text::FromWide(reason) << "\npid=" << GetCurrentProcessId() << "\ntime=" << stamp
         << "\n";
}

// Writes `bytes` and flushes them to the device before returning. The
// manifest used to reach the disk whenever the cache manager got to it, and
// the rename that publishes the entry is write-through: a power cut between
// the two left a published entry whose manifest was empty.
bool WriteFileDurably(const std::filesystem::path& path, std::string_view bytes)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                              nullptr) && written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

// The payload and the sidecars were written by other processes (ffmpeg, the
// helper) or by ofstream, none of which flushes. Best effort: a file that
// cannot be opened for the flush is still published, and the log says so.
bool FlushToDevice(const std::filesystem::path& path)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool flushed = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return flushed;
}

// Payloads this process published, and the digest promotion computed for each.
//
// Lookup hashed the whole payload on every call - a multi-gigabyte render read
// end to end each time the same process asked about the entry it had just
// written and hashed a moment before. The digest is reused only while the file
// is provably the one that was hashed: the same path, size, last-write and
// change times and file id, under the newest promotion of that path in this
// process. Every rewrite moves the change time, which SetFileTime cannot
// restore; a replaced file has a new id. The record dies with the process,
// and anything that removes or sets aside an entry drops it first.
struct PayloadStamp {
    uint64_t size{};
    int64_t lastWrite{};
    int64_t change{};
    uint64_t volume{};
    std::array<uint8_t, 16> id{};
    bool operator==(const PayloadStamp&) const = default;
};

std::optional<PayloadStamp> StampPayload(const std::filesystem::path& path)
{
    const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    FILE_ID_INFO identity{};
    const bool ok =
        GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) &&
        GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) &&
        GetFileInformationByHandleEx(file, FileIdInfo, &identity, sizeof(identity));
    CloseHandle(file);
    if (!ok) return std::nullopt;
    PayloadStamp stamp;
    stamp.size = static_cast<uint64_t>(standard.EndOfFile.QuadPart);
    stamp.lastWrite = basic.LastWriteTime.QuadPart;
    stamp.change = basic.ChangeTime.QuadPart;
    stamp.volume = identity.VolumeSerialNumber;
    std::copy(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier),
              stamp.id.begin());
    return stamp;
}

std::wstring PayloadMemoPath(const std::filesystem::path& path)
{
    std::wstring normalized = path.lexically_normal().generic_wstring();
    std::ranges::transform(normalized, normalized.begin(), towlower);
    return normalized;
}

struct PublishedPayload {
    std::wstring path;
    PayloadStamp stamp;
    uint64_t promotionSequence{};
    std::string digest;
};

std::mutex g_publishedMutex;
std::vector<PublishedPayload> g_published;
uint64_t g_promotionSequence{};
constexpr size_t kPublishedPayloads = 64;

void RememberPublishedPayload(const std::filesystem::path& path, const std::string& digest)
{
    const auto stamp = StampPayload(path);
    if (!stamp) return;
    const std::wstring key = PayloadMemoPath(path);
    std::lock_guard lock(g_publishedMutex);
    std::erase_if(g_published, [&](const PublishedPayload& entry) { return entry.path == key; });
    if (g_published.size() >= kPublishedPayloads) g_published.erase(g_published.begin());
    g_published.push_back({key, *stamp, ++g_promotionSequence, digest});
}

std::optional<std::string> PublishedPayloadDigest(const std::filesystem::path& path)
{
    const std::wstring key = PayloadMemoPath(path);
    {
        std::lock_guard lock(g_publishedMutex);
        if (std::ranges::none_of(g_published, [&](const PublishedPayload& entry) {
                return entry.path == key;
            })) return std::nullopt;
    }
    const auto stamp = StampPayload(path);
    if (!stamp) return std::nullopt;
    std::lock_guard lock(g_publishedMutex);
    const PublishedPayload* newest = nullptr;
    for (const PublishedPayload& entry : g_published)
        if (entry.path == key && (!newest || entry.promotionSequence > newest->promotionSequence))
            newest = &entry;
    if (!newest || newest->stamp != *stamp) return std::nullopt;
    return newest->digest;
}

// Drops every record at or under `directory`.
void ForgetPublishedPayloads(const std::filesystem::path& directory)
{
    std::wstring prefix = PayloadMemoPath(directory);
    if (!prefix.empty() && prefix.back() != L'/') prefix.push_back(L'/');
    std::lock_guard lock(g_publishedMutex);
    std::erase_if(g_published, [&](const PublishedPayload& entry) {
        return entry.path.starts_with(prefix);
    });
}

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
    // Read through the OS rather than ifstream's own buffering, with
    // FILE_FLAG_SEQUENTIAL_SCAN so the cache manager trims behind us instead of
    // retaining a multi-GB source in the standby list. This hashes whole media
    // files - the code's own measurements are 40-80 ms on a 60 MB render and
    // seconds on a multi-GB source - so the read pattern is the cost, and
    // ifstream was layering a second copy under a 1 MiB buffer to do it.
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    Sha256Hasher hasher;
    std::vector<uint8_t> buffer(4 * 1024 * 1024);
    for (;;) {
        if (stop.stop_requested()) { CloseHandle(file); return std::nullopt; }
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            CloseHandle(file);
            return std::nullopt;
        }
        if (read == 0) break;
        if (!hasher.Update(std::span{buffer.data(), static_cast<size_t>(read)})) {
            CloseHandle(file);
            return std::nullopt;
        }
    }
    CloseHandle(file);
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

std::string CaptureQualityIdentityTerm(const CaptureQualityTerms& terms)
{
    std::string term;
    const bool tenBit = EncoderQualityIsTenBit(terms.quality);
    if (terms.captureDither != kDefaultCaptureDither && !tenBit) term += "|dither-bayer8-v1";
    if (terms.quality == EncoderQuality::High)
        term += "|high-main10-cq" + std::to_string(kHighRungCq) + "-v1";
    else if (terms.quality == EncoderQuality::Lossless)
        term += "|lossless-ffv1-10bit-v1";
    return term;
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

std::wstring NeuralCacheRootLockName(const std::filesystem::path& root)
{
    std::wstring canonical = root.lexically_normal().generic_wstring();
    std::ranges::transform(canonical, canonical.begin(), towlower);
    while (canonical.size() > 1 && canonical.back() == L'/') canonical.pop_back();
    const std::string digest = HashBytes(utf8_text::FromWide(canonical)).value_or(std::string(64, '0'));
    return L"Local\\DLSSVideoPlayer.Cache." + std::wstring(digest.begin(), digest.begin() + 16);
}

std::string NeuralCacheInstallation()
{
    const auto directory = platform_paths::ModuleDirectory();
    if (!directory) return {};
    std::wstring normalized = directory->lexically_normal().generic_wstring();
    std::ranges::transform(normalized, normalized.begin(), towlower);
    while (normalized.size() > 1 && normalized.back() == L'/') normalized.pop_back();
    return utf8_text::FromWide(normalized);
}

NeuralCacheEnvironment NeuralCacheEnvironmentFor(const NeuralCacheIdentity& identity)
{
    // Incomplete terms record nothing rather than something the manifest gate
    // would refuse: an undetected driver must not turn a finished render into
    // an unpublishable one. Such an entry is kept like any legacy entry.
    if (identity.applicationVersion.empty() || identity.driverVersion.empty() ||
        !IsHexDigest(identity.modelStoreDigest)) return {};
    return {identity.applicationVersion, NeuralCacheInstallation(), identity.driverVersion,
            identity.modelStoreDigest};
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
        const auto utf8 = utf8_text::FromWideStrict(relative);
        if (!utf8 || utf8->empty()) return std::nullopt;
        canonical += *utf8;
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
        ",\"receiptDigest\":\"" + JsonEscape(manifest.receiptDigest) + "\"";
    // Absent rather than empty when nothing was recorded, so a manifest without
    // it is byte-for-byte what every earlier schema-5 writer produced.
    if (const NeuralCacheEnvironment& environment = manifest.environment; environment.Recorded())
        json += ",\"environment\":{\"application\":\"" + JsonEscape(environment.application) +
            "\",\"installation\":\"" + JsonEscape(environment.installation) +
            "\",\"driver\":\"" + JsonEscape(environment.driver) +
            "\",\"models\":\"" + JsonEscape(environment.modelStore) + "\"}";
    return json + "}\n";
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
        // The one optional extension: the key environment, fixed and ordered
        // like everything before it. An empty object is not something this
        // code writes, so it is refused rather than read as "not recorded".
        if (cursor.Expect(',')) {
            NeuralCacheEnvironment& environment = manifest.environment;
            if (!cursor.Key("environment") || !cursor.Expect('{') ||
                !ReadStringField(cursor, "application", environment.application) ||
                !ReadStringField(cursor, "installation", environment.installation) ||
                !ReadStringField(cursor, "driver", environment.driver) ||
                !ReadStringField(cursor, "models", environment.modelStore, false) ||
                !cursor.Expect('}') || !environment.Recorded())
                return std::nullopt;
        }
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
        // to the live process whose pid it carries; a quarantined entry waits
        // out its retention (cache_eviction::ReapStagingEntry).
        const auto kind = cache_eviction::ClassifyStagingEntry(name);
        std::error_code timeError;
        const auto written = iterator->last_write_time(timeError);
        if (timeError) continue;
        const int64_t ageSeconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::filesystem::file_time_type::clock::now() - written).count();
        const bool ownerAlive = kind == cache_eviction::StagingEntry::Partial && ProcessAlive(pid);
        if (!cache_eviction::ReapStagingEntry(kind, ownerAlive, ageSeconds)) continue;
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
        if (RemoveTreeBefore(candidate.path, started + kSweepBudget)) ++removed;
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

std::optional<NeuralCacheEntry> NeuralCacheManager::Peek(const std::filesystem::path& root,
                                                         NeuralCacheEntryKind kind, std::string_view key)
{
    // The key is 64 hex digits, so it cannot climb out of the bucket.
    if (root.empty() || !ValidKey(key)) return std::nullopt;
    const std::filesystem::path directory = root /
        (kind == NeuralCacheEntryKind::Source ? L"sources" : L"renders") /
        std::wstring(key.begin(), key.end());
    std::ifstream input(directory / L"manifest.json", std::ios::binary);
    if (!input.is_open()) return std::nullopt;
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    auto manifest = ParseNeuralCacheManifest(bytes);
    if (!manifest || manifest->kind != kind) return std::nullopt;
    return NeuralCacheEntry{directory,
                            directory / (kind == NeuralCacheEntryKind::Source ? L"source.mkv" : L"neural.mkv"),
                            std::move(*manifest)};
}

std::optional<std::filesystem::path> NeuralCacheManager::BeginRenderStaging(std::string_view key)
{
    return BeginStaging(NeuralCacheEntryKind::Render, key);
}

std::optional<NeuralCacheEntry> NeuralCacheManager::Lookup(
    NeuralCacheEntryKind kind, std::string_view key, std::stop_token stop) const
{
    if (!valid_ || !ValidKey(key)) return std::nullopt;
    const std::filesystem::path directory = root_ /
        (kind == NeuralCacheEntryKind::Source ? L"sources" : L"renders") /
        std::wstring(key.begin(), key.end());
    if (!OwnsPath(directory)) return std::nullopt;
    {
        // Marked before it is read, and under the root's lock: an eviction
        // that planned to remove this entry re-reads the mark under the same
        // lock and leaves it, so an entry being opened - the startup file's,
        // whose key nobody knows until its source is hashed - is never deleted
        // from under the lookup. Without the lock in time the mark is still
        // written; the hash below then refuses anything removed mid-read.
        const CacheRootLock lock(root_, kLookupLockWaitMs);
        if (!lock.Held()) LOG("Neural cache lookup did not get the cache lock in time; reading unlocked.");
        MarkEntryUsed(directory);
    }
    const auto manifestPath = directory / L"manifest.json";
    std::ifstream input(manifestPath, std::ios::binary);
    if (!input.is_open()) return std::nullopt;
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    const auto manifest = ParseNeuralCacheManifest(bytes);
    if (!manifest || manifest->kind != kind || !IsReusableNeuralCacheManifest(*manifest))
        return std::nullopt;
    if (!manifest->settingsDigest.empty() &&
        Sha256File(directory / L"neural-settings.ini", stop) != manifest->settingsDigest)
        return std::nullopt;
    // A reusable render always has a receipt digest, so the empty case below is
    // only ever reached by sources and legacy schema-3 entries, which have no
    // receipt to authenticate.
    if (!manifest->receiptDigest.empty() &&
        Sha256File(directory / L"receipt.json", stop) != manifest->receiptDigest)
        return std::nullopt;
    const auto payload = directory /
        (kind == NeuralCacheEntryKind::Source ? L"source.mkv" : L"neural.mkv");
    auto digest = PublishedPayloadDigest(payload);
    if (!digest) digest = Sha256File(payload, stop);
    if (!digest) return std::nullopt;
    const std::string& expected = kind == NeuralCacheEntryKind::Source
        ? manifest->sourceDigest : manifest->neuralDigest;
    if (*digest != expected) return std::nullopt;
    return NeuralCacheEntry{directory, payload, *manifest};
}

std::optional<NeuralCacheEntry> NeuralCacheManager::LookupSource(std::string_view key,
                                                                 std::stop_token stop) const
{
    return Lookup(NeuralCacheEntryKind::Source, key, std::move(stop));
}

std::optional<NeuralCacheEntry> NeuralCacheManager::LookupRender(std::string_view key,
                                                                 std::stop_token stop) const
{
    return Lookup(NeuralCacheEntryKind::Render, key, std::move(stop));
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
    // Flushed before it is hashed, and stamped around the hash: the stamp is
    // what lets a later lookup in this process reuse this digest, so it has to
    // describe exactly the bytes the digest was taken over.
    if (!FlushToDevice(payload)) LOG("Neural cache payload could not be flushed before publishing.");
    const auto hashedStamp = StampPayload(payload);
    const auto digest = Sha256File(payload);
    if (!digest) return fail(NeuralCachePromotion::Stage::PayloadDigest);
    const bool hashedUnchanged = hashedStamp && StampPayload(payload) == hashedStamp;
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
        manifest.environment = {};
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
    // The sidecars and the manifest reach the device before the rename that
    // publishes them does; the rename itself is write-through.
    for (const auto sidecar : {L"neural-settings.ini", L"receipt.json"}) {
        std::error_code sidecarError;
        if (std::filesystem::is_regular_file(staging / sidecar, sidecarError) &&
            !FlushToDevice(staging / sidecar))
            LOG("Neural cache sidecar could not be flushed before publishing.");
    }
    const auto manifestPath = staging / L"manifest.json";
    if (!WriteFileDurably(manifestPath, SerializeNeuralCacheManifest(manifest)))
        return fail(NeuralCachePromotion::Stage::ManifestWrite);
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
    // From the existing-entry check to the rename, nothing else may restructure
    // the root: another instance's eviction or Clear, or a second promotion of
    // the same key setting this one's entry aside as "existing".
    const CacheRootLock lock(root_, kPromoteLockWaitMs);
    if (!lock.Held())
        LOG("Neural cache promotion waited " << kPromoteLockWaitMs
            << " ms for another instance's cache operation; publishing without the lock.");
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
        // An entry lookup refused - tampered or damaged - is set aside for
        // inspection rather than deleted (see SweepStaging).
        std::filesystem::path setAside;
        if (!MoveToInvalidDirectory(root_, destination, L"invalid-existing", &setAside))
            return fail(NeuralCachePromotion::Stage::ExistingEntry);
        ForgetPublishedPayloads(destination);
        NoteQuarantine(setAside, L"existing entry failed authentication; replaced by a promotion");
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
    // A rename moves the file, not its bytes or its times; if the stamp still
    // matches the one taken around the hash, the digest describes this file.
    if (hashedUnchanged && StampPayload(destination / payloadName) == hashedStamp)
        RememberPublishedPayload(destination / payloadName, *digest);
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
    ForgetPublishedPayloads(entry.directory);
    std::filesystem::path setAside;
    if (!MoveToInvalidDirectory(root_, entry.directory, L"invalid-cache", &setAside)) return false;
    NoteQuarantine(setAside, L"published entry failed validation on reuse");
    return true;
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
    ForgetPublishedPayloads(directory);
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
    std::span<const std::string> activeKeys, uintmax_t freeFloorBytes,
    const cache_eviction::Identity* current)
{
    EvictionReport report;
    if (!valid_) return report;
    const auto renders = root_ / L"renders";
    std::error_code error;
    if (!std::filesystem::is_directory(renders, error)) return report;

    std::vector<cache_eviction::Entry> entries;
    std::vector<std::string> retired;
    std::vector<std::pair<std::string, std::optional<int64_t>>> marks;
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
        // A well-formed entry whose recorded key environment nothing sharing
        // this root can rebuild is as unreachable as a retired schema. One that
        // recorded nothing - everything published before the field existed -
        // is never judged here and waits for the pressure pass instead.
        if (entry.reusable && current && manifest->environment.Recorded()) {
            const NeuralCacheEnvironment& environment = manifest->environment;
            const cache_eviction::Identity recorded{environment.application,
                environment.installation, manifest->runtimeDigest, environment.driver,
                environment.modelStore};
            if (cache_eviction::IdentityRetired(recorded, *current)) {
                entry.reusable = false;
                retired.push_back(*key);
            }
        }

        // Last use is the newest of the entry's own mark - which every lookup
        // stamps (MarkEntryUsed) - and its files' write times, which stand in
        // for an entry nothing has looked up since it was written. The mark is
        // also remembered as planned: an entry whose mark moves before its turn
        // comes was looked up in the meantime, and is left.
        const auto mark = DirectoryWriteTime(child.path());
        if (mark) entry.lastUsed = *mark;
        marks.emplace_back(*key, mark);
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
        const auto directory = renders / std::wstring(key.begin(), key.end());
        // One entry at a time under the root's lock, so a lookup in any
        // instance is either before this - and has moved the mark - or after
        // it, and finds nothing. The lock is never held across the walk above.
        const CacheRootLock lock(root_, kEvictLockWaitMs);
        if (!lock.Held()) { ++report.deferred; continue; }
        const auto planned = std::ranges::find(marks, key, &decltype(marks)::value_type::first);
        if (planned == marks.end() || DirectoryWriteTime(directory) != planned->second) {
            ++report.deferred;
            continue;
        }
        const DWORD attributes = GetFileAttributesW(directory.c_str());
        if (!OwnsPath(directory) || attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            !RetireEntryDirectory(root_, directory, L"invalid-evicted")) {
            ++report.failures;
            continue;
        }
        ForgetPublishedPayloads(directory);
        if (matched != entries.end()) report.freedBytes += matched->bytes;
        if (std::ranges::find(retired, key) != retired.end()) ++report.retiredRemoved;
        else if (unreachable) ++report.unreachableRemoved;
        else ++report.leastRecentlyUsedRemoved;
    }
    if (!plan.evict.empty() || report.failures)
        LOG("Cache eviction: removed " << report.unreachableRemoved
            << " unreachable, " << report.retiredRemoved
            << " retired by a changed driver, model store, version or runtime, and "
            << report.leastRecentlyUsedRemoved
            << " least-recently-used render(s), freeing " << report.freedBytes
            << " bytes; " << report.failures << " could not be removed (in use or refused), "
            << report.deferred << " were left because they were looked up since, or another"
               " instance held the cache."
            << (plan.floorMet ? "" : " The free-space floor was still not met."));
    return report;
}

bool NeuralCacheManager::Clear()
{
    if (!valid_) return false;
    // Another instance evicting, promoting or clearing: skip rather than
    // block the UI thread that asked, and say so.
    const CacheRootLock lock(root_, kClearLockWaitMs);
    if (!lock.Held()) {
        LOG("Neural cache clear skipped: another player instance is using the cache.");
        return false;
    }
    // frame-generation holds the player's converted videos. Leaving it out made
    // the Clear prompt lie: SizeBytes recurses the whole root, so the dialog
    // offered to free bytes it then kept, and generated files accumulated with
    // no surface in the player able to delete them.
    //
    // `live` is here for exactly the same reason and was missed the first time
    // round. It holds each session's published segments; SizeBytes counts them
    // and Clear did not remove them, so the dialog over-promised again by
    // however much live rendering the user had done.
    //
    // Emptied child by child rather than removed whole. remove_all over live/
    // and staging/ deleted what another RUNNING instance was writing - its
    // session's segments and its unfinished renders - which is the
    // cross-instance deletion SessionDirectory was introduced to stop. Those
    // stay, by the same owner check the staging sweep uses; everything of this
    // process's and of processes that are gone goes. A published entry is
    // retired by rename, so one another instance is playing is left whole and
    // reported rather than half deleted.
    bool complete = true;
    const DWORD self = GetCurrentProcessId();
    for (const auto name : {L"sources", L"renders", L"staging", L"frame-generation", L"live"}) {
        const std::wstring bucket = name;
        const auto target = root_ / name;
        if (!OwnsPath(target)) return false;
        std::error_code error;
        std::filesystem::create_directories(target, error);
        if (error) return false;
        std::vector<std::filesystem::path> children;
        for (std::filesystem::directory_iterator iterator(target, error), end;
             !error && iterator != end; iterator.increment(error))
            children.push_back(iterator->path());
        if (error) return false;
        for (const auto& child : children) {
            const std::wstring childName = child.filename().wstring();
            DWORD owner = 0;
            if (bucket == L"staging" && ParseStagingOwner(childName, owner) && owner != self &&
                ProcessAlive(owner)) continue;
            uint32_t session = 0;
            if (bucket == L"live" && live_session::ParseSessionOwner(childName, session) &&
                session != self && ProcessAlive(session)) continue;
            if (bucket == L"sources" || bucket == L"renders") {
                if (!RetireEntryDirectory(root_, child, L"invalid-cleared")) complete = false;
                continue;
            }
            std::error_code removeError;
            std::filesystem::remove_all(child, removeError);
            if (removeError) complete = false;
        }
    }
    ForgetPublishedPayloads(root_);
    if (!complete) LOG("Neural cache clear left entries another process still has open.");
    return complete;
}

size_t NeuralCacheManager::SweepLiveSessions()
{
    if (!valid_) return 0;
    // A crash during a live session left its live/pid<N> behind - gigabytes of
    // segments - and only a directory named after the CURRENT process was ever
    // removed, so each crash added another one for good. Windows does not
    // reuse a pid while its process lives, so a directory whose owner is gone
    // belongs to nobody. An owner that is alive, or that this process may not
    // query, keeps its directory.
    size_t removed = 0;
    std::error_code error;
    std::vector<std::filesystem::path> dead;
    for (std::filesystem::directory_iterator iterator(root_ / L"live", error), end;
         !error && iterator != end; iterator.increment(error)) {
        uint32_t owner = 0;
        if (!live_session::ParseSessionOwner(iterator->path().filename().wstring(), owner)) continue;
        if (ProcessAlive(owner)) continue;
        dead.push_back(iterator->path());
    }
    for (const auto& directory : dead) {
        if (!OwnsPath(directory)) continue;
        std::error_code removeError;
        std::filesystem::remove_all(directory, removeError);
        if (!removeError) ++removed;
    }
    if (removed || !dead.empty())
        LOG("Live session directories swept: removed=" << removed << " of " << dead.size()
            << " left by processes that are gone.");
    return removed;
}
