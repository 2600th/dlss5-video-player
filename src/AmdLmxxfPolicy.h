#pragma once

#include "TemporalStabilityPolicy.h"

#include <cstdint>
#include <string_view>

namespace amd_lmxxf {

inline constexpr uint32_t kAmdVendorId = 0x1002;
inline constexpr std::wstring_view kRuntimeDirectoryName = L"amd-neural-runtime";
inline constexpr std::wstring_view kRuntimeDllName = L"LmxxfNrRuntime.dll";
inline constexpr std::wstring_view kAssetsDirectoryName = L"native-game-tiled-assets";
inline constexpr std::wstring_view kLockFileName = L"amd-runtime-lock.json";

// The rung an AMD render takes when nothing asked for one.
//
// The runtime relights every frame on its own and this backend has no history
// for it to fuse with - its own temporal model needs guides, and the estimator
// that would produce them is the expensive half this backend does not have - so
// what a viewer sees between frames is the model's own frame-to-frame jitter.
// The host-side accumulation (TemporalStabilityPolicy.h) is what takes that out,
// which makes Off here not a neutral setting but the arm that flickers: measured
// on a still source, frame-to-frame Y PSNR is 49.1 dB at the model's own output
// against the source's own 96.6 dB, and Low takes the added jitter down 28.5%
// for 0.1 dB of distance from the source
// (docs/AMD-TEMPORAL-ACCUMULATION-zh.md).
//
// Low rather than Medium or High because this is a default and not a choice: the
// rungs trade steadiness for detail monotonically, Low is the one that moves the
// picture least, and a viewer who wants more can ask for it.
inline constexpr TemporalStability kAmdDefaultStability = TemporalStability::Low;

enum class GpuFamily
{
    Unsupported,
    Rdna4,
};

enum class KernelTarget
{
    Unsupported,
    Gfx1200,
    Gfx1201,
};

constexpr std::string_view GpuFamilyName(GpuFamily family) noexcept
{
    switch (family) {
        case GpuFamily::Unsupported: return "unsupported";
        case GpuFamily::Rdna4: return "amd-rdna4";
    }
    return "unsupported";
}

constexpr std::string_view KernelTargetName(KernelTarget target) noexcept
{
    switch (target) {
        case KernelTarget::Unsupported: return "unsupported";
        case KernelTarget::Gfx1200: return "gfx1200";
        case KernelTarget::Gfx1201: return "gfx1201";
    }
    return "unsupported";
}

constexpr GpuFamily ClassifyGpu(uint32_t vendorId, std::wstring_view description) noexcept
{
    if (vendorId != kAmdVendorId) return GpuFamily::Unsupported;
    if (description.find(L"RX 9070") != std::wstring_view::npos ||
        description.find(L"RX 9060") != std::wstring_view::npos ||
        description.find(L"Radeon AI PRO R9700") != std::wstring_view::npos)
        return GpuFamily::Rdna4;
    return GpuFamily::Unsupported;
}

constexpr KernelTarget SelectKernelTarget(GpuFamily family,
                                          std::wstring_view description) noexcept
{
    if (family != GpuFamily::Rdna4) return KernelTarget::Unsupported;
    if (description.find(L"RX 9060") != std::wstring_view::npos)
        return KernelTarget::Gfx1200;
    if (description.find(L"RX 9070") != std::wstring_view::npos ||
        description.find(L"Radeon AI PRO R9700") != std::wstring_view::npos)
        return KernelTarget::Gfx1201;
    return KernelTarget::Unsupported;
}

struct RuntimeLayout
{
    bool runtimeDll{};
    bool assetsDirectory{};
    bool shaders{};
    bool modules{};
    bool weights{};
    bool lockFile{};

    constexpr bool Complete() const noexcept
    {
        return runtimeDll && assetsDirectory && shaders && modules && weights && lockFile;
    }
    constexpr bool Absent() const noexcept
    {
        return !runtimeDll && !assetsDirectory && !shaders && !modules && !weights && !lockFile;
    }
};

enum class LayoutState
{
    Absent,
    Complete,
    Incomplete,
};

constexpr LayoutState ClassifyLayout(const RuntimeLayout& layout) noexcept
{
    if (layout.Absent()) return LayoutState::Absent;
    return layout.Complete() ? LayoutState::Complete : LayoutState::Incomplete;
}

} // namespace amd_lmxxf
