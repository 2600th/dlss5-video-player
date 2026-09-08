#pragma once

#include "OfflineNeuralRenderer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

enum class GpuGeneration {
    Rtx40Ada,
    Rtx50Blackwell,
    OtherNvidia,
    Unsupported,
};

struct DetectedGpu {
    GpuGeneration generation{GpuGeneration::Unsupported};
    std::wstring description;
    uint32_t vendorId{};
    uint32_t deviceId{};
    uint64_t dedicatedVideoMemoryBytes{};
    // User-mode driver version reported by DXGI ("32.0.15.6164"); empty when
    // the adapter does not report one.
    std::wstring driverVersion;
};

struct NeuralRenderDefaults {
    uint32_t width{};
    uint32_t height{};
};

struct RuntimeArguments {
    bool ok{false};
    bool safeMode{false};
    bool addonBootstrapRestarted{false};
    std::vector<std::wstring> userArguments;
    std::wstring error;
};

enum class BootstrapAction {
    Continue,
    Relaunch,
    Fail,
};

enum class NeuralRuntimeLayout {
    Absent,
    Complete,
    Incomplete,
};

enum class SafeModeRestartOutcome {
    Cancelled,
    LaunchFailed,
    CloseCurrent,
};

enum class NeuralPlaybackState {
    Idle,
    Acquiring,
    Rendering,
    Validating,
    Ready,
    OriginalOnly,
    Cancelling,
    Failed,
    // Render job suspended by the user; resumes to Rendering.
    Paused,
    // Job retrying the same frame / relaunching the worker; resolves to
    // Rendering, Failed, RetryExhausted or Cancelling.
    Recovering,
    // Bounded retries spent. Terminal like Failed.
    RetryExhausted,
};

const wchar_t* NeuralPlaybackStateName(NeuralPlaybackState state) noexcept;

// Lifecycle state a job failure lands in: RetryExhausted stays distinct so
// the UI can say the retries were spent, Cancelled is the user's choice and
// offers the original, every other failure is Failed.
NeuralPlaybackState StateForFailure(NeuralRenderFailure failure) noexcept;

struct NeuralPlaybackLifecycle {
    NeuralPlaybackState state{NeuralPlaybackState::Idle};
    uint64_t generation{};
    uint64_t Begin();
    bool Accept(uint64_t candidateGeneration) const;
    bool Transition(NeuralPlaybackState next);
    void Invalidate();
};

enum class NeuralOpenAction { UseCache, StartJob, OriginalOnly };
NeuralOpenAction DecideNeuralOpen(bool runtimeComplete, bool safeMode, bool cacheValid);
bool CanPublishNeuralCompletion(bool renderOk, bool probeOk, bool manifestValid);
void ExecuteNeuralReplacementSequence(const std::function<void()>& requestStop,
                                      const std::function<void()>& joinWorker,
                                      const std::function<void()>& replace);

GpuGeneration ClassifyGpu(uint32_t vendorId, std::wstring_view description);
bool NeuralAddonDesired(GpuGeneration gpu, bool safeMode);
NeuralRenderDefaults ResolveNeuralRenderDefaults(
    bool neuralAddonConfigured,
    bool outputExplicit,
    uint32_t requestedWidth,
    uint32_t requestedHeight);
NeuralRuntimeLayout ClassifyNeuralRuntimeLayout(
    bool hasReShadeConfig,
    bool hasReShadeProxy,
    bool hasNeuralAddon,
    bool hasNeuralRuntime);
DetectedGpu DetectHighPerformanceGpu();
BootstrapAction DecideBootstrap(
    bool desiredEnabled,
    bool configEnabled,
    bool alreadyRestarted,
    bool updateSucceeded);
BootstrapAction DecideBootstrapFromObservedUpdate(
    bool desiredEnabled,
    bool previousEnabled,
    bool currentEnabled,
    bool alreadyRestarted,
    bool updateSucceeded);
RuntimeArguments ParseRuntimeArguments(int argc, const wchar_t* const* argv);
std::vector<std::wstring> BuildBootstrapRelaunchArguments(
    const std::vector<std::wstring>& userArguments);
std::vector<std::wstring> BuildSafeModeRestartArguments(
    const std::vector<std::wstring>& userArguments);
SafeModeRestartOutcome ExecuteAdvancedSafeModeRestart(
    bool confirmed,
    const std::vector<std::wstring>& userArguments,
    const std::function<bool(const std::vector<std::wstring>&)>& launch);
std::wstring BuildWindowsCommandLine(
    std::wstring_view executable,
    const std::vector<std::wstring>& arguments);
