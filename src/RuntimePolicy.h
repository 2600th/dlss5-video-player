#pragma once

#include <cstdint>
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

enum class BootstrapAction {
    Continue,
    Relaunch,
    Fail,
};

GpuGeneration ClassifyGpu(uint32_t vendorId, std::wstring_view description);
bool NeuralAddonDesired(GpuGeneration gpu, bool safeMode);
DetectedGpu DetectHighPerformanceGpu();
BootstrapAction DecideBootstrap(
    bool desiredEnabled,
    bool configEnabled,
    bool alreadyRestarted,
    bool updateSucceeded);
std::wstring BuildWindowsCommandLine(
    std::wstring_view executable,
    const std::vector<std::wstring>& arguments);
