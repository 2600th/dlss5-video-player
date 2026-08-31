#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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

GpuGeneration ClassifyGpu(uint32_t vendorId, std::wstring_view description);
bool NeuralAddonDesired(GpuGeneration gpu, bool safeMode);
DetectedGpu DetectHighPerformanceGpu();
