#pragma once

// Feature-18 runtime preflight. The helper probes the locked runtime before a
// render and returns a JSON receipt (schema 2) naming GPU, driver, every
// runtime module (size, version, hash), the ReShade/RenoDX/NR banner
// versions, the effective RenoDX settings line, every feature-18
// creation/evaluation observation and the classified diagnosis of a refusal.
// Schema 2 separates feature18.createResult - the feature's own NGX result,
// parsed out of the log - from feature18.carrierCreateResult, the Super
// Resolution carrier's, which reports success on machines where feature 18
// was refused.

#include "RuntimePolicy.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
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

std::string JsonEscape(std::string_view text);
std::string JsonEscapeWide(std::wstring_view text);
std::string BuildPreflightFailureJson(std::wstring_view detail);

// Lowercase 0x-prefixed eight-digit form of an NGX result code. Shared with the
// probe, which reports the same codes in its receipt JSON.
std::string HexResultText(uint32_t value);

// The twelve runtime files whose hashes form the render identity.
std::span<const std::wstring_view> LockedRuntimeFileNames();
