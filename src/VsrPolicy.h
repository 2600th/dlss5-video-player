#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// RTX Video Super Resolution (P2.8): NVIDIA's video super resolution, trained on
// compressed video, run live on the original the player already decodes and shown
// as a comparison view beside DLSS 5. It is presentation only - the window
// compositor reads it, and nothing that reaches the cache capture, an export or a
// cache key does - so it needs no render and no cache. The engine itself is
// VsrEngine; this is what can be decided without a GPU: the quality it runs at,
// whether it can run and what to say when it cannot, and how large a frame it makes.
namespace vsr_policy {

// NGX's own numbers (NVSDK_NGX_VSR_QualityLevel): 0 is bicubic and 1 to 4 the
// network at rising cost. The ladder offers the four network levels; bicubic is a
// scaler the compositor already is. High is the default: on an RTX 4080 SUPER a
// 1440p frame measured 5.6 ms there against 7.8 ms at Ultra, and a 1080p frame
// 3.2 ms against 4.3 ms (the spike's table in the p28 report), so High holds a
// 1440p 30 fps clip with room left in every frame.
enum class Quality : int { Low = 1, Medium = 2, High = 3, Ultra = 4 };
inline constexpr Quality kDefaultQuality = Quality::High;
inline constexpr std::array<Quality, 4> kQualities{Quality::Low, Quality::Medium, Quality::High, Quality::Ultra};

// [Comparison] VsrQuality: anything but a known level reads as the default.
inline Quality LoadQuality(int saved)
{
    for (const Quality quality : kQualities)
        if (static_cast<int>(quality) == saved) return quality;
    return kDefaultQuality;
}

inline size_t QualityIndex(Quality quality)
{
    for (size_t index = 0; index < kQualities.size(); ++index)
        if (kQualities[index] == quality) return index;
    return QualityIndex(kDefaultQuality);
}

// Why the view can or cannot be shown, in the order the conditions are met: a
// build without the SDK has nothing to ask, a GPU with no NGX session has no one
// to ask, and so on down to a feature that was offered and then refused.
enum class Reason {
    Ready,
    NotBuilt,        // built without -DRTX_VIDEO_SDK: the engine is compiled out
    NoSession,       // NGX never started on this device (no RTX GPU, or no NVIDIA driver)
    QueryFailed,     // NGX answered nothing about its features
    NeedsDriver,     // VSR.NeedsUpdatedDriver
    MissingRuntime,  // VSR.Available could not be read: nvngx_vsr.dll is not beside the player
    Unsupported,     // VSR.Available = 0: this GPU or driver does not offer it
    CreateFailed,    // offered, then CreateFeature refused
};

// What the runtime said, as VsrEngine read it from NGX's capability parameters.
// The guide names the three parameters and their meaning (RTX Video SDK
// Programming Guide 1.1.0, 3.2.2.2 and 6.3): an unreadable VSR.Available means
// the feature DLL was not found, and a zero one an unsupported GPU or driver.
struct Capabilities {
    bool built = false;
    bool session = false;
    bool queried = false;
    bool availableRead = false;
    bool available = false;
    bool needsUpdatedDriver = false;
    uint32_t minDriverMajor = 0;
    uint32_t minDriverMinor = 0;
    bool createAttempted = false;
    bool createSucceeded = false;
};

inline Reason Decide(const Capabilities& caps)
{
    if (!caps.built) return Reason::NotBuilt;
    if (!caps.session) return Reason::NoSession;
    if (!caps.queried) return Reason::QueryFailed;
    // Asked first: an old driver also reports the feature unavailable, and "update
    // the driver" is the answer that fixes it.
    if (caps.needsUpdatedDriver) return Reason::NeedsDriver;
    if (!caps.availableRead) return Reason::MissingRuntime;
    if (!caps.available) return Reason::Unsupported;
    if (caps.createAttempted && !caps.createSucceeded) return Reason::CreateFailed;
    return Reason::Ready;
}

// The Localization.h key that says it; NeedsDriver's text takes the minimum
// driver version as two numbers, CreateFailed's the NGX result in hex.
inline const wchar_t* ReasonKey(Reason reason)
{
    switch (reason) {
    case Reason::Ready: return L"vsr.reason.ready";
    case Reason::NotBuilt: return L"vsr.reason.not_built";
    case Reason::NoSession: return L"vsr.reason.no_session";
    case Reason::QueryFailed: return L"vsr.reason.query_failed";
    case Reason::NeedsDriver: return L"vsr.reason.needs_driver";
    case Reason::MissingRuntime: return L"vsr.reason.missing_runtime";
    case Reason::Unsupported: return L"vsr.reason.unsupported";
    case Reason::CreateFailed: return L"vsr.reason.create_failed";
    }
    return L"vsr.reason.unsupported";
}

struct Size {
    uint32_t width = 0;
    uint32_t height = 0;
    friend bool operator==(const Size&, const Size&) = default;
};

// D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, spelled out so this header needs no D3D.
inline constexpr uint32_t kMaxDimension = 16384;

// The frame VSR writes. Where the window shows the picture larger than the source,
// VSR does the upscale to the size the picture is shown at (the fitted size in the
// render target, which is what every other member is drawn into), so the
// compositor draws it texel for pixel. Where it does not, VSR runs at 1x - the
// source's own size, where it is a compression clean-up - and the compositor
// minifies it as it minifies every other member. Its cost follows the INPUT: on the
// spike a 1080p source took 3.0 ms at High whether it wrote 1080p, 1440p or 2160p.
// The runtime accepted every factor the spike asked for, 1x to 6x; the only bound
// kept is D3D12's, with the aspect held.
inline Size OutputSize(uint32_t sourceW, uint32_t sourceH, uint32_t targetW, uint32_t targetH)
{
    if (!sourceW || !sourceH) return {};
    if (!targetW || !targetH) return {sourceW, sourceH};
    double scale = std::min(double(targetW) / double(sourceW), double(targetH) / double(sourceH));
    if (scale <= 1.0) return {sourceW, sourceH};
    scale = std::min({scale, double(kMaxDimension) / double(sourceW), double(kMaxDimension) / double(sourceH)});
    const auto axis = [scale](uint32_t source) {
        return static_cast<uint32_t>(std::clamp<long long>(std::llround(double(source) * scale), 1, kMaxDimension));
    };
    return {std::max(sourceW, axis(sourceW)), std::max(sourceH, axis(sourceH))};
}

} // namespace vsr_policy
