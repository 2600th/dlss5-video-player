#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

enum class NeuralCacheEntryKind {
    Source,
    Render,
};

enum class NeuralCacheState {
    Staging,
    Complete,
    Invalid,
};

struct NeuralCacheIdentity {
    std::string sourceDigest;
    uint32_t width{};
    uint32_t height{};
    std::string applicationVersion;
    std::string gpuPath;
    std::string runtimeDigest;
    std::string quality;
    bool upscaling{};
};

struct NeuralCacheManifest {
    uint32_t schema{3};
    NeuralCacheEntryKind kind{NeuralCacheEntryKind::Render};
    NeuralCacheState state{NeuralCacheState::Staging};
    std::string sourceDigest;
    std::string neuralDigest;
    std::string runtimeDigest;
    std::string encoder;
    uint32_t width{};
    uint32_t height{};
    uint64_t frameCount{};
    int64_t duration100ns{};
    uint64_t nativeEvaluations{};
    uint64_t verifiedNeuralFrames{};
    uint64_t observedFeature18Evaluations{};
    bool feature18Created{};
    bool feature18ArmedBeforeCapture{};
    bool upscaling{};

    friend bool operator==(const NeuralCacheManifest&, const NeuralCacheManifest&) = default;
};

struct NeuralCacheEntry {
    std::filesystem::path directory;
    std::filesystem::path payloadPath;
    NeuralCacheManifest manifest;
};

std::optional<std::string> Sha256File(const std::filesystem::path& path,
                                      std::stop_token stop = {});
std::string BuildNeuralCacheKey(const NeuralCacheIdentity& identity);
std::optional<std::string> BuildRuntimeDigest(
    const std::filesystem::path& moduleDirectory,
    std::span<const std::wstring_view> relativeFiles,
    std::stop_token stop = {});
std::string SerializeNeuralCacheManifest(const NeuralCacheManifest& manifest);
std::optional<NeuralCacheManifest> ParseNeuralCacheManifest(std::string_view bytes);
bool IsReusableNeuralCacheManifest(const NeuralCacheManifest& manifest);

class NeuralCacheManager {
public:
    explicit NeuralCacheManager(std::filesystem::path root = {});

    bool Valid() const { return valid_; }
    const std::filesystem::path& Root() const { return root_; }

    std::optional<NeuralCacheEntry> LookupSource(std::string_view key) const;
    std::optional<NeuralCacheEntry> LookupRender(std::string_view key) const;
    std::optional<std::filesystem::path> BeginSourceStaging(std::string_view key);
    std::optional<std::filesystem::path> BeginRenderStaging(std::string_view key);
    bool PromoteSource(std::string_view key, const std::filesystem::path& staging,
                       NeuralCacheManifest manifest);
    bool PromoteRender(std::string_view key, const std::filesystem::path& staging,
                       NeuralCacheManifest manifest);
    bool MarkInvalid(const std::filesystem::path& staging);
    bool Quarantine(const NeuralCacheEntry& entry);
    uintmax_t SizeBytes() const;
    bool Clear();

private:
    static std::optional<std::filesystem::path> DefaultRoot();
    static bool ValidKey(std::string_view key);
    bool OwnsPath(const std::filesystem::path& path) const;
    std::optional<std::filesystem::path> BeginStaging(NeuralCacheEntryKind kind,
                                                      std::string_view key);
    std::optional<NeuralCacheEntry> Lookup(NeuralCacheEntryKind kind,
                                           std::string_view key) const;
    bool Promote(NeuralCacheEntryKind kind, std::string_view key,
                 const std::filesystem::path& staging, NeuralCacheManifest manifest);

    std::filesystem::path root_;
    bool valid_{false};
};
