#pragma once

// Feature-18 runtime preflight. The helper probes the locked runtime before a
// render and returns a JSON receipt naming GPU, driver, every runtime module
// (size, version, hash), the ReShade/RenoDX/NR banner versions, the effective
// RenoDX settings line and every feature-18 creation/evaluation observation.

#include "NeuralWorkerProtocol.h"
#include "OfflineNeuralRenderer.h"
#include "RuntimePolicy.h"

#include <windows.h>

#include <cstdint>
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

// The twelve runtime files whose hashes form the render identity.
std::span<const std::wstring_view> LockedRuntimeFileNames();

// Worker side. Primes feature 18 on a synthetic sequence, reads the runtime
// evidence and returns the receipt. Never writes to the cache.
neural_worker_protocol::PreflightPayload RunNeuralPreflightProbe(
    HWND renderWindow, const std::filesystem::path& moduleDirectory, const DetectedGpu& gpu);
