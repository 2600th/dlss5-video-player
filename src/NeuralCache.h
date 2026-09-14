#pragma once

#include "NeuralRenderTypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>

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
    std::string settingsDigest;
    // Half-open [start,end) render window; a whole-source render keeps the
    // legacy key. Non-default guide controls append their canonical form.
    NeuralRenderRange range{};
    std::string guides;
};

struct NeuralCacheManifest {
    uint32_t schema{4};
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
    // Empty for legacy schema-3 entries; present renders also authenticate
    // neural-settings.ini alongside the encoded payload.
    std::string settingsDigest;
    // Schema 4. Parsed schema-3 entries keep these defaults: whole-source
    // range, default guides, no job identity and no receipt.
    int64_t rangeStart100ns{};
    int64_t rangeEnd100ns{};
    std::string guides;
    uint64_t jobId{};
    uint32_t historyResets{};
    // Required on schema-4 renders, which authenticate receipt.json beside the
    // payload; empty on sources and on legacy schema-3 entries.
    std::string receiptDigest;

    friend bool operator==(const NeuralCacheManifest&, const NeuralCacheManifest&) = default;
};

struct NeuralCacheEntry {
    std::filesystem::path directory;
    std::filesystem::path payloadPath;
    NeuralCacheManifest manifest;
};

std::optional<std::string> Sha256File(const std::filesystem::path& path,
                                      std::stop_token stop = {});
// Same digest, memoised per process on (path, size, last write time). Only for
// installation files that do not change while the player runs - the locked
// runtime set is hashed three times per session otherwise (226 MB each pass,
// ~0.5 s). Never use it for user content: a file rewritten with an identical
// size and timestamp would keep the stale digest.
std::optional<std::string> Sha256FileCached(const std::filesystem::path& path,
                                            std::stop_token stop = {});
std::optional<std::string> Sha256Bytes(std::string_view bytes);
std::string BuildNeuralCacheKey(const NeuralCacheIdentity& identity);
std::optional<std::string> BuildRuntimeDigest(
    const std::filesystem::path& moduleDirectory,
    std::span<const std::wstring_view> relativeFiles,
    std::stop_token stop = {});
std::string SerializeNeuralCacheManifest(const NeuralCacheManifest& manifest);
std::optional<NeuralCacheManifest> ParseNeuralCacheManifest(std::string_view bytes);
bool IsReusableNeuralCacheManifest(const NeuralCacheManifest& manifest);

// Why a promotion did not publish. The publish gate reported one verdict for
// every reason, so a rename that lost to an antivirus scan of the freshly
// written entry was indistinguishable from a digest mismatch, and a finished
// render was discarded with nothing to tell the two apart.
struct NeuralCachePromotion {
    enum class Stage {
        Published, Rejected, PayloadDigest, ManifestRejected, SidecarDigest,
        ManifestWrite, ManifestReread, ExistingEntry, StagingCleanup, Move, Reopen
    };
    Stage stage = Stage::Published;
    // Win32 error and attempts from the rename that publishes the entry.
    unsigned long win32Error = 0;
    unsigned attempts = 0;
};
const char* NeuralCachePromotionStageName(NeuralCachePromotion::Stage stage);

// Why a staging directory, or the cache root that holds it, could not be
// created. Every one of these answered std::nullopt, so an install directory
// the user cannot write to was indistinguishable from a rejected key, and the
// one field report of it carried neither the path that was attempted nor the
// error the filesystem gave for it.
struct NeuralCacheFailure {
    enum class Cause {
        None, NoWritableRoot, InvalidKey, CreateFailed, AlreadyExists, OutsideRoot
    };
    Cause cause = Cause::None;
    std::error_code error;
    // The directory the attempt was made on; for a key the manager refused
    // before touching the disk, the staging directory it would have gone in.
    std::filesystem::path path;
};
const char* NeuralCacheFailureCauseName(NeuralCacheFailure::Cause cause);

class NeuralCacheManager {
public:
    // An empty root prefers <executable directory>/cache/v1, with LocalAppData
    // as the fallback. Explicit roots are used without silently falling back.
    explicit NeuralCacheManager(std::filesystem::path root = {});

    // Identifies the previously persisted automatic root without creating it.
    static std::optional<std::filesystem::path> LegacyDefaultRoot();
    // Resolves package-virtualized writes using a delete-on-close probe in an
    // existing legacy directory. Never creates a legacy cache during migration.
    static std::optional<std::filesystem::path> ResolvedLegacyDefaultRoot();

    bool Valid() const { return valid_; }
    const std::filesystem::path& Root() const { return root_; }
    // Why the last staging attempt returned nothing. A manager that never
    // became valid keeps the constructor's verdict, because no attempt can get
    // past it, and a successful attempt clears the record.
    const NeuralCacheFailure& LastFailure() const { return failure_; }

    std::optional<NeuralCacheEntry> LookupSource(std::string_view key) const;
    std::optional<NeuralCacheEntry> LookupRender(std::string_view key) const;
    std::optional<std::filesystem::path> BeginSourceStaging(std::string_view key);
    std::optional<std::filesystem::path> BeginRenderStaging(std::string_view key);
    bool PromoteSource(std::string_view key, const std::filesystem::path& staging,
                       NeuralCacheManifest manifest,
                       NeuralCachePromotion* diagnostic = nullptr);
    bool PromoteRender(std::string_view key, const std::filesystem::path& staging,
                       NeuralCacheManifest manifest,
                       NeuralCachePromotion* diagnostic = nullptr);
    bool MarkInvalid(const std::filesystem::path& staging);
    bool Quarantine(const NeuralCacheEntry& entry);
    // Removes only this exact owned cache entry. Missing entries succeed.
    // Callers must first release playback/jobs referencing the entry.
    bool RemoveSource(std::string_view key);
    bool RemoveRender(std::string_view key);
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
                 const std::filesystem::path& staging, NeuralCacheManifest manifest,
                 NeuralCachePromotion* diagnostic);
    bool Remove(NeuralCacheEntryKind kind, std::string_view key);

    std::filesystem::path root_;
    bool valid_{false};
    NeuralCacheFailure failure_;
};
