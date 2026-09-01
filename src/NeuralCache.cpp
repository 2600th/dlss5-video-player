#include "NeuralCache.h"

#include <windows.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <fstream>
#include <map>
#include <system_error>
#include <vector>

namespace {

constexpr uint32_t kSchema = 1;
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

std::optional<std::string> Sha256Bytes(std::string_view bytes)
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

bool CommonManifestFieldsValid(const NeuralCacheManifest& manifest)
{
    return manifest.schema == kSchema &&
           manifest.width >= kMinDimension && manifest.width <= kMaxWidth &&
           manifest.height >= kMinDimension && manifest.height <= kMaxHeight &&
           manifest.frameCount > 0 && manifest.duration100ns > 0 &&
           !manifest.encoder.empty() && !manifest.upscaling;
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

} // namespace

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

std::string BuildNeuralCacheKey(const NeuralCacheIdentity& identity)
{
    std::string canonical;
    AppendField(canonical, "schema", "1");
    AppendField(canonical, "source", identity.sourceDigest);
    AppendField(canonical, "width", std::to_string(identity.width));
    AppendField(canonical, "height", std::to_string(identity.height));
    AppendField(canonical, "application", identity.applicationVersion);
    AppendField(canonical, "gpu", identity.gpuPath);
    AppendField(canonical, "runtime", identity.runtimeDigest);
    AppendField(canonical, "quality", identity.quality);
    AppendField(canonical, "upscaling", identity.upscaling ? "1" : "0");
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
        const auto digest = Sha256File(moduleDirectory / relative, stop);
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
    return "{\"schema\":" + std::to_string(manifest.schema) +
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
        ",\"observedFeature18Evaluations\":" +
            std::to_string(manifest.observedFeature18Evaluations) +
        ",\"feature18Created\":" + (manifest.feature18Created ? "true" : "false") +
        ",\"upscaling\":" + (manifest.upscaling ? "true" : "false") + "}\n";
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
        !ReadIntegerField(cursor, "observedFeature18Evaluations",
                          manifest.observedFeature18Evaluations) ||
        !ReadBoolField(cursor, "feature18Created", manifest.feature18Created) ||
        !ReadBoolField(cursor, "upscaling", manifest.upscaling, false) ||
        !cursor.Expect('}') || !cursor.Finished()) return std::nullopt;

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
    return IsHexDigest(manifest.sourceDigest) && IsHexDigest(manifest.neuralDigest) &&
           IsHexDigest(manifest.runtimeDigest) && manifest.feature18Created &&
           manifest.nativeEvaluations >= manifest.frameCount &&
           manifest.observedFeature18Evaluations > 0;
}

std::optional<std::filesystem::path> NeuralCacheManager::DefaultRoot()
{
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                    &localAppData)) || !localAppData) return std::nullopt;
    std::filesystem::path result = std::filesystem::path(localAppData) /
        L"DLSSVideoPlayer" / L"NeuralCache" / L"v1";
    CoTaskMemFree(localAppData);
    return result;
}

NeuralCacheManager::NeuralCacheManager(std::filesystem::path root)
{
    if (root.empty()) {
        const auto resolved = DefaultRoot();
        if (!resolved) return;
        root = *resolved;
    }
    std::error_code error;
    root_ = CanonicalOrAbsolute(root, error);
    if (error || root_.empty() || root_ == root_.root_path() ||
        root_.parent_path().empty()) return;
    std::filesystem::create_directories(root_ / L"sources", error);
    if (error) return;
    std::filesystem::create_directories(root_ / L"renders", error);
    if (error) return;
    std::filesystem::create_directories(root_ / L"staging", error);
    if (error) return;
    valid_ = true;
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
    if (!valid_ || !ValidKey(key)) return std::nullopt;
    const uint64_t nonce = ++g_stagingNonce;
    const std::wstring prefix = kind == NeuralCacheEntryKind::Source ? L"source-" : L"render-";
    const std::filesystem::path directory = root_ / L"staging" /
        (prefix + std::wstring(key.begin(), key.end()) + L"-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(nonce));
    std::error_code error;
    if (!std::filesystem::create_directories(directory, error) || error || !OwnsPath(directory))
        return std::nullopt;
    return directory;
}

std::optional<std::filesystem::path> NeuralCacheManager::BeginSourceStaging(std::string_view key)
{
    return BeginStaging(NeuralCacheEntryKind::Source, key);
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
                                 NeuralCacheManifest manifest)
{
    if (!valid_ || !ValidKey(key) || !OwnsPath(staging) ||
        staging.parent_path().filename() != L"staging") return false;
    const auto payload = staging /
        (kind == NeuralCacheEntryKind::Source ? L"source.mkv" : L"neural.mkv");
    const auto digest = Sha256File(payload);
    if (!digest) return false;
    manifest.kind = kind;
    manifest.state = NeuralCacheState::Complete;
    manifest.schema = kSchema;
    if (kind == NeuralCacheEntryKind::Source) {
        manifest.sourceDigest = *digest;
        manifest.neuralDigest.clear();
        manifest.runtimeDigest.clear();
        manifest.nativeEvaluations = 0;
        manifest.observedFeature18Evaluations = 0;
        manifest.feature18Created = false;
    } else {
        manifest.neuralDigest = *digest;
    }
    if (!IsReusableNeuralCacheManifest(manifest)) return false;
    const auto manifestPath = staging / L"manifest.json";
    {
        std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) return false;
        const std::string serialized = SerializeNeuralCacheManifest(manifest);
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        if (!output.good()) return false;
    }
    {
        std::ifstream input(manifestPath, std::ios::binary);
        const std::string serialized{std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>()};
        const auto reparsed = ParseNeuralCacheManifest(serialized);
        if (!reparsed || *reparsed != manifest || !IsReusableNeuralCacheManifest(*reparsed))
            return false;
    }

    const auto destination = root_ /
        (kind == NeuralCacheEntryKind::Source ? L"sources" : L"renders") /
        std::wstring(key.begin(), key.end());
    if (Lookup(kind, key)) {
        std::error_code cleanupError;
        std::filesystem::remove_all(staging, cleanupError);
        return !cleanupError;
    }
    std::error_code existsError;
    if (std::filesystem::exists(destination, existsError)) {
        if (existsError) return false;
        const auto quarantine = root_ / L"staging" /
            (L"invalid-" + std::wstring(key.begin(), key.end()) + L"-" +
             std::to_wstring(++g_stagingNonce));
        if (!MoveFileExW(destination.c_str(), quarantine.c_str(), MOVEFILE_WRITE_THROUGH))
            return false;
    }
    if (!MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
        return false;
    return Lookup(kind, key).has_value();
}

bool NeuralCacheManager::PromoteSource(std::string_view key,
                                       const std::filesystem::path& staging,
                                       NeuralCacheManifest manifest)
{
    return Promote(NeuralCacheEntryKind::Source, key, staging, std::move(manifest));
}

bool NeuralCacheManager::PromoteRender(std::string_view key,
                                       const std::filesystem::path& staging,
                                       NeuralCacheManifest manifest)
{
    return Promote(NeuralCacheEntryKind::Render, key, staging, std::move(manifest));
}

bool NeuralCacheManager::MarkInvalid(const std::filesystem::path& staging)
{
    if (!OwnsPath(staging) || staging.parent_path().filename() != L"staging") return false;
    const auto destination = root_ / L"staging" /
        (L"invalid-" + std::to_wstring(++g_stagingNonce));
    return MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
}

uintmax_t NeuralCacheManager::SizeBytes() const
{
    if (!valid_) return 0;
    uintmax_t total = 0;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(
             root_, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && !error) total += iterator->file_size(error);
        if (error) return 0;
    }
    return error ? 0 : total;
}

bool NeuralCacheManager::Clear()
{
    if (!valid_) return false;
    for (const auto name : {L"sources", L"renders", L"staging"}) {
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
