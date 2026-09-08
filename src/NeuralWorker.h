#pragma once

#include "OfflineNeuralRenderer.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// Starts the isolated neural-cache helper and accepts only a complete, validated
// helper result. The caller retains ownership of cache staging/promotion.
// Worker crashes and device removal are retried from frame zero at most
// `crashRelaunchLimit` times; exhaustion reports NeuralRenderFailure::RetryExhausted.
inline constexpr uint32_t kDefaultCrashRelaunchLimit = 1;
NeuralRenderResult RunNeuralWorker(
    const std::filesystem::path& executable,
    const NeuralRenderRequest& request,
    OfflineNeuralRenderer::ProgressCallback progress = {},
    std::stop_token stop = {},
    uint32_t crashRelaunchLimit = kDefaultCrashRelaunchLimit);

// Short Feature-18 probe run in the same isolated helper before a render. The
// JSON receipt names GPU, driver, runtime/consumer versions and every feature
// creation result observed; `ok` is false when the probe could not arm the
// neural contract. Never renders or touches the cache.
struct NeuralPreflightResult {
    bool ok{};
    bool cancelled{};
    std::string json;
    std::wstring detail;
};
NeuralPreflightResult RunNeuralPreflight(
    const std::filesystem::path& executable,
    std::stop_token stop = {});

// Serializes every use of the shared neural-runtime directory. The parent
// rewrites ReShade.ini with the render's neural settings per job and the
// helper's proxy owns ReShade.log, so two concurrent renders would swap each
// other's settings and evidence under cache keys that claim otherwise. Hold
// this from the settings write until the helper has exited.
class NeuralRuntimeLease {
public:
    explicit NeuralRuntimeLease(const std::filesystem::path& runtimeDirectory,
                                std::chrono::milliseconds wait = std::chrono::seconds{5});
    ~NeuralRuntimeLease();
    NeuralRuntimeLease(const NeuralRuntimeLease&) = delete;
    NeuralRuntimeLease& operator=(const NeuralRuntimeLease&) = delete;

    bool Held() const noexcept { return held_; }
    // Session-scoped name derived from the directory; stable across processes.
    static std::wstring MutexName(const std::filesystem::path& runtimeDirectory);

private:
    void* mutex_{};
    bool held_{};
};

namespace neural_worker_detail {

// The helper exits completely after updating the proxy's startup settings.
// Only its hook-free parent may launch the replacement, at most once.
inline constexpr unsigned long kConfigurationChangedExitCode = 75;

struct WorkerArguments {
    HANDLE metadata{};
    bool preflight{};
    bool configurationRestarted{};
    NeuralRenderRequest request;
};

// Exact argument envelope accepted by the helper executable. Shared with
// NeuralWorkerMain so the launcher and the parser cannot drift apart.
std::vector<std::wstring> BuildWorkerArguments(const NeuralRenderRequest& request, HANDLE metadata,
                                               HANDLE pauseEvent, bool configurationRestarted);
std::vector<std::wstring> BuildPreflightArguments(HANDLE metadata, bool configurationRestarted);
std::optional<WorkerArguments> ParseWorkerArguments(std::span<const std::wstring_view> arguments);

} // namespace neural_worker_detail
