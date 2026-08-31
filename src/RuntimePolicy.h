#pragma once

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

enum class SafeModeRestartOutcome {
    Cancelled,
    LaunchFailed,
    CloseCurrent,
};

GpuGeneration ClassifyGpu(uint32_t vendorId, std::wstring_view description);
bool NeuralAddonDesired(GpuGeneration gpu, bool safeMode);
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
