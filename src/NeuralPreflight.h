#pragma once

// Feature-18 runtime preflight. The helper probes the locked runtime before a
// render and returns a JSON receipt (schema 3) naming GPU, driver, every
// runtime module (size, version, hash), the ReShade/RenoDX/NR banner
// versions, the effective RenoDX settings line, every feature-18
// creation/evaluation observation and the classified diagnosis of a refusal.
// Schema 2 separated feature18.createResult - the feature's own NGX result,
// parsed out of the log - from feature18.carrierCreateResult, the Super
// Resolution carrier's, which reports success on machines where feature 18
// was refused. Schema 3 adds modelStore, which records whether the render's
// model-store identity term was digested from the resolved model roots or fell
// back to the driver version, so that fallback is on the receipt rather than
// silent.

#include "HexText.h"
#include "JsonEscape.h"
#include "RuntimePolicy.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

struct NeuralRuntimeBanner {
    std::string reshadeVersion;   // "6.8.0.2155"
    std::string addonVersion;     // "0.2026.828.517"
    std::string addonApiVersion;  // "18"
    std::string renodxVersion;    // "4.7"
    std::string renodxBuild;      // "Sep  2 2026 01:15:39"
    std::string dlssnrRuntime;    // "310.8.0"
    std::string activeSettings;   // "upscaling=OFF intensity=1.000000 ..."

    friend bool operator==(const NeuralRuntimeBanner&, const NeuralRuntimeBanner&) = default;
};

// Parses the proxy banner lines written when the add-on loads (offset 0 of
// ReShade.log). Missing lines leave the corresponding field empty.
NeuralRuntimeBanner ParseNeuralRuntimeBanner(std::string_view reshadeLog);

// One feature-18 creation/evaluation observation from the log, in order.
struct Feature18Observation {
    std::string line;
    bool failure{};
};
std::vector<Feature18Observation> CollectFeature18Observations(std::string_view reshadeLogSegment);

// The real feature-18 NGX result, which the runtime reports only as text in
// the log: the last "feature 18 create failed with 0x<hex>" code observed,
// nothing when every create succeeded. The carrier (Super Resolution) feature
// has its own result and routinely reports success while feature 18 refuses.
std::optional<uint32_t> ParseFeature18CreateResult(std::span<const Feature18Observation> observations);

// The receipt's `"observations":[...]` member, bounded. The metadata pipe
// refuses any frame over 64 KiB (neural_worker_protocol::kMaximumPayloadBytes),
// and a probe that fails every frame logs an observation per frame, so the
// whole list reached the player as "malformed metadata" instead of the
// diagnosis it carried. The first and last kReportedObservationsPerEnd are
// kept - how it started failing and what it settled into - each line cut to
// kReportedObservationLineBytes on a UTF-8 boundary and marked "...", and an
// `"observationsOmitted":N` member follows when any were dropped. A list that
// fits is written exactly as before. Diagnosis still reads the full list.
inline constexpr size_t kReportedObservationsPerEnd = 8;
inline constexpr size_t kReportedObservationLineBytes = 256;
std::string ReportedFeature18ObservationsJson(std::span<const Feature18Observation> observations);

// Why feature 18 is unavailable, in the order that decides what the user is
// told. A driver below the floor outranks the platform refusal it causes,
// because updating the driver is the action that fixes it.
enum class NeuralPreflightCause {
    None,
    DriverBelowFloor,
    PlatformRefusal,
    ArchitectureUnsupported,
    OutOfVideoMemory,
    EvidenceIncomplete,
    ProbeFailed,
};

// Receipt names, index-aligned with NeuralPreflightCause: the probe writes
// them, the parent scans them back out of the receipt.
inline constexpr std::array<std::string_view, 7> kNeuralPreflightCauseNames{
    "none", "driverBelowFloor", "platformRefusal", "architectureUnsupported", "outOfVideoMemory",
    "evidenceIncomplete", "probeFailed"};

struct NeuralPreflightDiagnosis {
    NeuralPreflightCause cause{NeuralPreflightCause::None};
    std::optional<uint32_t> ngxResult;
    std::wstring detail;
};

// Classifies a finished probe. `created`/`evidenceValid` are the probe's own
// verdicts and `probeFailure` the sentence it produced, if any.
NeuralPreflightDiagnosis DiagnoseNeuralPreflight(const DetectedGpu& gpu,
                                                 std::span<const Feature18Observation> observations,
                                                 bool created,
                                                 bool evidenceValid,
                                                 std::wstring_view probeFailure);
const char* NeuralPreflightCauseName(NeuralPreflightCause cause) noexcept;

struct RuntimeModuleReceipt {
    std::wstring name;
    bool present{};
    uint64_t sizeBytes{};
    std::wstring fileVersion;
    std::string sha256;
};
std::vector<RuntimeModuleReceipt> DescribeRuntimeModules(const std::filesystem::path& directory,
                                                         std::span<const std::wstring_view> names);

// JsonEscape (JsonEscape.h) over the UTF-8 of `text`.
std::string JsonEscapeWide(std::wstring_view text);
std::string BuildPreflightFailureJson(std::wstring_view detail);

// The thirteen runtime files whose hashes form the render identity: the twelve
// vendor modules the embedded lock pins byte for byte, plus NeuralWorker.exe,
// which stages the guides and classifies the cuts. The worker was absent from
// this set, so a worker rebuilt with different guide or cut logic left the
// runtime digest unchanged and only an application-version bump retired the
// renders it had produced. It ships inside neural-runtime/ beside the vendor
// modules, so it is located exactly as they are, and a missing one still
// leaves BuildRuntimeDigest without a digest - which the render path reports
// as an incomplete runtime.
std::span<const std::wstring_view> LockedRuntimeFileNames();

// The twelve of those that packaging/runtime-lock.json pins and
// VerifyRuntimeLock checks. The worker is built from this repository and every
// build changes it, so it is hashed into the identity and never pinned.
std::span<const std::wstring_view> LockPinnedRuntimeFileNames();

// Where the model-store term of the render identity came from.
enum class NeuralModelStoreSource {
    ModelContents,  // at least one resolved root was enumerated and digested
    DriverVersion,  // no root could be read, so the driver version stood in
};
inline constexpr std::array<std::string_view, 2> kNeuralModelStoreSourceNames{"modelContents",
                                                                              "driverVersion"};
const char* NeuralModelStoreSourceName(NeuralModelStoreSource source) noexcept;

// One directory the pass resolves weights out of. A driver-store root holds the
// whole display driver, so only its top-level nvngx* modules belong in the
// identity; the ProgramData model store is walked except for the feature
// directories the neural pass never evaluates (frame generation, ray
// reconstruction and the like).
struct NeuralModelRoot {
    std::filesystem::path directory;
    bool recursive{};
    std::wstring_view namePrefix;  // empty accepts every name
};

// The roots feature 18 actually evaluates out of, in digest order: the NGX
// core directory the display driver registers - the nv_dispsi.inf_...
// directory under the driver store that NGX resolves through
// NGXGetPathUsingQAI - and %ProgramData%\NVIDIA\NGX\models. Neither is inside
// neural-runtime/, so LockedRuntimeFileNames() cannot cover them: a driver
// update or a model refresh replaced them with the runtime digest unchanged.
// A root this machine does not have is simply absent from the result.
std::vector<NeuralModelRoot> RegisteredNeuralModelRoots();

struct NeuralModelStore {
    std::string digest;  // 64 hex characters; always usable as an identity term
    NeuralModelStoreSource source{NeuralModelStoreSource::DriverVersion};
    std::vector<std::wstring> enumeratedRoots;  // the roots the listing covers
    uint32_t files{};                           // files in the listing
    uint32_t contentHashedFiles{};              // of those, hashed byte for byte
    uint64_t bytes{};                           // bytes the listing accounts for
    // Why a registered root is missing from the listing, or why the driver
    // version stood in for every one of them. Empty only when each registered
    // root was enumerated whole.
    std::wstring fallbackDetail;
    // What made this digest depend on the moment it was taken rather than on
    // the store: a registered root that could not be enumerated whole (a stop
    // included), and files the listing could not size or, within the content
    // bound, could not hash. Either one yields a digest no render was keyed
    // under, which is harmless for a lookup - it misses - and fatal for
    // eviction, which would read every entry as retired by it.
    uint32_t unavailableRoots{};
    uint32_t unreadableFiles{};
};

// True when `store` describes the store rather than a failed read of it, so
// eviction may compare recorded digests against it. A machine with no NGX
// root registered at all is settled: its driver-version fallback is
// deterministic.
bool NeuralModelStoreSettled(const NeuralModelStore& store);

// Digests the given roots: every file's relative name, and its content hash
// when it is small enough to afford one or its size and write time when it is
// not. Feature directories and selector sections for features the neural
// pass never evaluates are left out of a recursive root. Files a root
// cannot enumerate leave the root out of the listing and named in
// fallbackDetail; when no root can be read at all the digest covers
// `driverVersion` instead and the source says so.
NeuralModelStore DigestNeuralModelStore(std::span<const NeuralModelRoot> roots,
                                        std::wstring_view driverVersion,
                                        std::stop_token stop = {});
// The same over RegisteredNeuralModelRoots(): what the render identity carries.
NeuralModelStore ResolveNeuralModelStore(std::wstring_view driverVersion,
                                         std::stop_token stop = {});
// The receipt's modelStore object.
std::string NeuralModelStoreJson(const NeuralModelStore& store);
