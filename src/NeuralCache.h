#pragma once

#include "CacheEvictionPolicy.h"
#include "MediaPipeline.h"
#include "NeuralRenderTypes.h"

#include <cstdint>
#include <filesystem>
#include <functional>
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
    // The two terms that stop a render from crossing the runtime it was made
    // on. `driverVersion` closes a driver change: `gpuPath` is a generation
    // label, so every Ada card on every driver shared one value and a render
    // produced on one driver was served - and validated - on any later one.
    // `modelStoreDigest` closes a model-store change: the pass resolves its
    // weights out of the registered NGX core directory and
    // %ProgramData%\NVIDIA\NGX\models, neither of which `runtimeDigest`
    // covers, so a driver update or a model refresh used to leave the key
    // identical (see ResolveNeuralModelStore). Both are appended to the
    // canonical form only when set, so source entries - which have neither -
    // keep their keys.
    std::string driverVersion;
    std::string modelStoreDigest;
};

// The environment terms of the key a render was published under, recorded
// beside it so eviction can tell an entry nothing will ask for again from one
// that is merely old (cache_eviction::IdentityRetired). The key is a one-way
// hash, so without these a driver update left every orphan indistinguishable
// from a live render. `runtimeDigest` is already a manifest field and is not
// repeated here. Empty on sources, on legacy schema-3 entries, and on every
// render published before the field existed - all of which stay reusable.
struct NeuralCacheEnvironment {
    std::string application;   // NeuralCacheIdentity::applicationVersion
    std::string installation;  // NeuralCacheInstallation() of the publisher
    std::string driver;        // NeuralCacheIdentity::driverVersion
    std::string modelStore;    // NeuralCacheIdentity::modelStoreDigest

    bool Recorded() const
    {
        return !application.empty() || !installation.empty() || !driver.empty() ||
               !modelStore.empty();
    }
    friend bool operator==(const NeuralCacheEnvironment&, const NeuralCacheEnvironment&) = default;
};

// The named mutex every player instance takes around the operations that
// restructure one cache root (Evict, Clear, Promote, and a lookup's last-use
// mark): Local\DLSSVideoPlayer.Cache.<first 16 hex of SHA-256 of the root,
// lower case, forward slashes>. Pass the manager's Root(), already canonical.
std::wstring NeuralCacheRootLockName(const std::filesystem::path& root);

// This installation, as the environment records it: the module directory in
// lower case with forward slashes, so two spellings of one directory compare
// equal. Empty when the module path cannot be read.
std::string NeuralCacheInstallation();
// The environment terms of `identity`, plus this installation.
NeuralCacheEnvironment NeuralCacheEnvironmentFor(const NeuralCacheIdentity& identity);

struct NeuralCacheManifest {
    // Schema 5 carries schema 4's field list. The bump retires every schema-4
    // entry, because those were written under a render identity that named
    // neither the driver nor the model store: nothing recorded which weights
    // produced them, so they cannot be matched against today's key and are
    // refused by the schema gate rather than by a missing field.
    uint32_t schema{5};
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
    // Schema 4 onwards. Parsed schema-3 entries keep these defaults: whole-source
    // range, default guides, no job identity and no receipt.
    int64_t rangeStart100ns{};
    int64_t rangeEnd100ns{};
    std::string guides;
    uint64_t jobId{};
    uint32_t historyResets{};
    // Required on current-schema renders, which authenticate receipt.json
    // beside the payload; empty on sources and on legacy schema-3 entries.
    std::string receiptDigest;
    // Optional, schema 5, renders only: written after receiptDigest when
    // recorded and absent otherwise, so every manifest written before it
    // still parses - and those entries stay reusable.
    NeuralCacheEnvironment environment;

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
// The pipeline term of a render key: what the renderer and the encoder promise
// about the pixels, as against the source, the geometry and the settings.
// `gpuSourceConversion` belongs here because it changes what the model is shown -
// NV12 converted on the GPU instead of BGRA delivered by ffmpeg - while the
// capture-side encoder switches only change how the result is written, which is
// why they stay out. The term is appended, never substituted, so every key
// published before it existed keeps the key it was published under.
// Canonical `quality` term for a render identity: everything about the
// pipeline that changes the bytes a cache hit hands back.
//
// `gpuSourceConversion` changes what the model is shown. The other two change
// what is written from what it produced: `nvencPreset` picks the NVENC preset
// (measured 0.12 VMAF on ordinary content, 0.53 on noise-heavy, between p5 and
// p7), and `gpuColorConversion` picks between a GPU 2x2 box chroma downsample
// and ffmpeg's CPU conversion - two different filters over the neural output.
// A render made under one must never be served for the other, or the setting
// is silently inert on every range already rendered.
//
// Each term is appended only when it differs from the shipped default, so the
// defaults canonicalize to exactly the term every field render was published
// under and adding them retires no cache entry. No parameter defaults here on
// purpose: a caller that forgets one must not silently get the shipped value.
std::string NeuralRenderPipelineIdentity(bool gpuSourceConversion, uint32_t nvencPreset,
                                         bool gpuColorConversion);

// The shipped encoder defaults, so a caller that means "unchanged" says so by
// name rather than by repeating the literals.
inline constexpr uint32_t kDefaultNvencPreset = 5;
inline constexpr bool kDefaultGpuColorConversion = false;

// The capture-quality switches of this player's quality work, as one key term
// appended after every other pipeline term (the temporal one included), in a fixed
// order, each spelled out only when it leaves its default - so the defaults add
// nothing and no entry published under them is retired.
//
// `captureDither` is the ordered dither at the 8-bit capture store
// (DitherPolicy.h): the same picture, different bytes in every frame. The term names
// the map as well as the switch: a different map or amplitude is a different set of
// bytes and gets a new version, never this term.
//
// `quality` is the rung of the ladder (EncoderQuality). A 10-bit rung captures P010
// and has no 8-bit store, so the dither term is dropped there: flipping an inert
// switch must not re-render anything. The High term carries its CQ. Standard has a
// term too, although it is the default: lifting NVENC's bitrate ceiling changed its
// bytes, so the renders written under the ceiling must not be served for it.
//
// `sourceDeband` is the deband pre-pass on the decoded source (DebandPolicy.h),
// which changes what the model is shown; the term spells out its parameters and
// its fixed pattern, so a change to either is a new term.
//
// `suppliedExposure` replaces the feature's AutoExposure with a smoothed meter
// (ExposurePolicy.h), which changes what the model produces.
struct CaptureQualityTerms {
    bool captureDither{false};
    EncoderQuality quality{EncoderQuality::Standard};
    bool sourceDeband{false};
    bool suppliedExposure{false};
};
inline constexpr bool kDefaultCaptureDither = false;
inline constexpr EncoderQuality kDefaultCacheQuality = EncoderQuality::Standard;
inline constexpr bool kDefaultSourceDeband = false;
inline constexpr bool kDefaultSuppliedExposure = false;
std::string CaptureQualityIdentityTerm(const CaptureQualityTerms& terms);

// The encoder switches a rung makes inert, as the key should see them: a 10-bit rung
// captures P010 whatever the colour conversion says, and Lossless (FFV1) has no NVENC
// preset. Keyed at their defaults there, so flipping one re-renders nothing.
constexpr bool KeyedGpuColorConversion(bool gpuColorConversion, EncoderQuality quality)
{
    return EncoderQualityIsTenBit(quality) ? kDefaultGpuColorConversion : gpuColorConversion;
}
constexpr uint32_t KeyedNvencPreset(uint32_t nvencPreset, EncoderQuality quality)
{
    return quality == EncoderQuality::Lossless ? kDefaultNvencPreset : nvencPreset;
}
std::optional<std::string> BuildRuntimeDigest(
    const std::filesystem::path& moduleDirectory,
    std::span<const std::wstring_view> relativeFiles,
    std::stop_token stop = {});
std::string SerializeNeuralCacheManifest(const NeuralCacheManifest& manifest);
std::optional<NeuralCacheManifest> ParseNeuralCacheManifest(std::string_view bytes);
bool IsReusableNeuralCacheManifest(const NeuralCacheManifest& manifest);

// Why a promotion did not publish, and what it published when it did. The
// publish gate reported one verdict for every reason, so a rename that lost to
// an antivirus scan of the freshly written entry was indistinguishable from a
// digest mismatch, and a finished render was discarded with nothing to tell the
// two apart.
struct NeuralCachePromotion {
    enum class Stage {
        Published, Rejected, PayloadDigest, ManifestRejected, SidecarDigest,
        ManifestWrite, ManifestReread, ExistingEntry, StagingCleanup, Move, Reopen
    };
    Stage stage = Stage::Published;
    // Win32 error and attempts from the rename that publishes the entry.
    unsigned long win32Error = 0;
    unsigned attempts = 0;
    // Set when `stage` is Published: the entry as promotion verified it, so
    // the caller does not re-read a payload that was hashed a moment ago.
    std::optional<NeuralCacheEntry> entry;
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

    // Authenticates the entry - manifest, sidecars and the payload's digest -
    // and returns it. `stop` abandons the payload hash, which on a multi-GB
    // source is seconds of reading a cancelled job used to sit out. A payload
    // this process published is not re-read while it is provably the file
    // promotion hashed (same path, size, write and change times, file id).
    std::optional<NeuralCacheEntry> LookupSource(std::string_view key,
                                                 std::stop_token stop = {}) const;
    std::optional<NeuralCacheEntry> LookupRender(std::string_view key,
                                                 std::stop_token stop = {}) const;
    // Where an entry's payload would be, without opening or hashing anything.
    // `LookupSource` authenticates the copy by hashing it, which a caller polling
    // "is there one?" cannot afford; this lets such a caller memoise the verdict
    // against the file's own size and write time instead of re-asking.
    std::optional<std::filesystem::path> SourcePayloadPath(std::string_view key) const;
    // An entry's manifest and payload path as they are on disk, for a picture or
    // a label and never for playback: nothing is hashed, the entry is not marked
    // used (so showing it does not reorder eviction), no lock is taken and the
    // payload may be missing. Static, so a thread can read a root the owner
    // resolved without building a manager, which prepares directories and
    // sweeps staging. Anything that plays the entry goes through Lookup*.
    static std::optional<NeuralCacheEntry> Peek(const std::filesystem::path& root,
                                                NeuralCacheEntryKind kind, std::string_view key);
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

    // What one eviction pass did. Reported rather than silent: this deletes
    // renders, and a user who finds one gone deserves to find out why from
    // the log.
    struct EvictionReport {
        size_t unreachableRemoved{};   // manifests this build can never serve
        size_t retiredRemoved{};       // recorded key environment nothing can rebuild
        size_t leastRecentlyUsedRemoved{};
        uintmax_t freedBytes{};
        bool freeSpaceFloorMet{};
        size_t failures{};             // entries the filesystem refused, or in use
        // Planned but left: looked up since the plan was made, or another
        // instance held the cache root past the wait.
        size_t deferred{};
    };

    // Removes render entries that can never be served again, and - only when
    // the volume has less than `freeFloorBytes` free - the least recently used
    // reusable entries until it does. `activeKeys` are never touched.
    // `current` is this process's key environment; when given, an entry whose
    // recorded one is retired by it (cache_eviction::IdentityRetired) counts as
    // unreachable. Without it only manifests the reuse gate refuses are.
    //
    // The cache had no eviction at all, and its key deliberately retires
    // entries wholesale: one driver update changes every key, so 40 GB of
    // renders becomes unreachable at once and the only remedy on offer was
    // Clear(), which destroys the new renders too. See CacheEvictionPolicy.h
    // for why the trigger is free space rather than a size cap.
    EvictionReport Evict(std::span<const std::string> activeKeys = {},
                         uintmax_t freeFloorBytes = 0,
                         const cache_eviction::Identity* current = nullptr);
    uintmax_t SizeBytes() const;
    // Empties the cache except what another RUNNING instance owns: its
    // live/pid<N> session and its staging entries. Published entries go by
    // rename, so one that another process has open is left whole. False when
    // anything was left, or when another instance held the cache root past a
    // short wait - the UI thread calls this and must not hang on it.
    bool Clear();
    // Removes live/pid<N> session directories whose process is gone. Returns
    // the number removed.
    size_t SweepLiveSessions();
    // Reaps staging/ entries nothing will ever finish: directories set aside
    // as invalid, and partial payloads whose owning process is gone.
    // Quarantined entries - published ones that failed authentication - are
    // kept for cache_eviction::kQuarantineRetentionSeconds first, so they can be
    // inspected. Bounded per call, file by file, and run by the constructor,
    // so a litter of them is worked off oldest first across constructions.
    // Returns the number removed.
    size_t SweepStaging();
    // Called after each publishing rename that failed on a transient sharing
    // error, before the retry, with the attempt number that failed. The
    // promotion test releases its scanner's handle from here, so the retry
    // it then observes is the one that publishes.
    void ObservePublishRetries(std::function<void(unsigned)> observer)
    {
        publishRetryObserver_ = std::move(observer);
    }

private:
    static std::optional<std::filesystem::path> DefaultRoot();
    static bool ValidKey(std::string_view key);
    bool OwnsPath(const std::filesystem::path& path) const;
    std::optional<std::filesystem::path> BeginStaging(NeuralCacheEntryKind kind,
                                                      std::string_view key);
    std::optional<NeuralCacheEntry> Lookup(NeuralCacheEntryKind kind,
                                           std::string_view key,
                                           std::stop_token stop = {}) const;
    bool Promote(NeuralCacheEntryKind kind, std::string_view key,
                 const std::filesystem::path& staging, NeuralCacheManifest manifest,
                 NeuralCachePromotion* diagnostic);
    bool Remove(NeuralCacheEntryKind kind, std::string_view key);

    std::filesystem::path root_;
    bool valid_{false};
    NeuralCacheFailure failure_;
    std::function<void(unsigned)> publishRetryObserver_;
};
