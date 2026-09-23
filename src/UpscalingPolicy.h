#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct UpscalingSize { uint32_t width{}, height{}; bool grows{}; };

// The output bounding boxes the player offers, smallest first. They are 16:9
// boxes, not a forced aspect: UpscalingTarget fits the source inside one.
inline constexpr uint32_t kUpscaleRungHeights[]{1080, 1440, 2160};

// Width of a rung, or 0 when `targetHeight` names none.
inline constexpr uint32_t UpscaleRungWidth(uint32_t targetHeight) noexcept {
    switch (targetHeight) {
        case 1080: return 1920;
        case 1440: return 2560;
        case 2160: return 3840;
        default: return 0;
    }
}

// Largest rung the display can actually scan out, or 0 when the panel is
// shorter than the smallest one. Above the panel's native height the surplus is
// scaled away at present time while the DLSS evaluate is still charged per
// output pixel: a 2160p target on a 1080p panel costs four times the pixels for
// the same picture. The list ends at 2160, which is the ceiling the target menu
// already offered when it was a fixed pick.
inline constexpr uint32_t AutoUpscaleTargetHeight(uint32_t displayHeight) noexcept {
    uint32_t chosen = 0;
    for (const uint32_t rung : kUpscaleRungHeights)
        if (displayHeight >= rung) chosen = rung;
    return chosen;
}

// A target is a bounding box, not a reason to discard source detail.
inline UpscalingSize UpscalingTarget(uint32_t width, uint32_t height, uint32_t targetHeight) {
    const uint32_t targetWidth = UpscaleRungWidth(targetHeight);
    if (!width || !height || !targetWidth)
        return {width,height,false};
    const double scale = std::min(double(targetWidth)/width,double(targetHeight)/height);
    if (scale <= 1.0) return {width,height,false};
    return {std::max(width, uint32_t(std::lround(width*scale)) & ~1u),
            std::max(height,uint32_t(std::lround(height*scale)) & ~1u),true};
}

inline bool SourceFitsDLSSRange(uint32_t w,uint32_t h,uint32_t ow,uint32_t oh,
                               uint32_t minW,uint32_t minH,uint32_t maxW,uint32_t maxH) {
    return w && h && minW && minH && maxW >= minW && maxH >= minH &&
           w < ow && h < oh && w >= minW && h >= minH && w <= maxW && h <= maxH;
}

// The largest output that still admits `w x h` as a DLSS input, derived from the
// minimum a larger output advertised. `minW`/`minH` come straight from
// NGX_DLSS_GET_OPTIMAL_SETTINGS, so the shrink is the runtime's own arithmetic
// rather than an assumed ratio. One scale factor keeps the source aspect, which
// DLSS requires of the render size, and a result at or below the source is
// refused because it would no longer be an upscale.
inline UpscalingSize AdmissibleDLSSOutput(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh,
                                          uint32_t minW, uint32_t minH) {
    if (!w || !h || !ow || !oh || !minW || !minH) return {ow,oh,false};
    if (w >= minW && h >= minH) return {ow,oh,false};
    const double scale = std::min(double(w)/minW, double(h)/minH);
    if (!(scale > 0.0) || scale >= 1.0) return {ow,oh,false};
    const uint32_t reducedW = uint32_t(std::lround(ow*scale)) & ~1u;
    const uint32_t reducedH = uint32_t(std::lround(oh*scale)) & ~1u;
    if (reducedW <= w || reducedH <= h || reducedW >= ow || reducedH >= oh)
        return {ow,oh,false};
    return {reducedW,reducedH,true};
}

// ---- Processing scale ----------------------------------------------------
//
// The resolution the neural model runs at, as a percentage of the source. At
// 100 - the default, and the recommended rung - the model sees the decoded
// frame and nothing about the render changes. Below it the frame is
// area-reduced before the model and DLSS Super Resolution brings the result
// back to the source size, with the add-on's NRPreUpscale=1 putting the model
// on the reduced input rather than on the restored output: that ordering is
// the whole saving, since after the upscale the model would run at 100% again.
// It is the community Neural Upstream arrangement, which NVIDIA's own order
// (the model last, on the full frame) is the reference against; so it is a
// rung the user picks, labelled with what it measured, never a default.
//
// No rung above 100. A model run at 150% would need the Super Resolution
// output reduced back to the source size after it, and the capture path has
// no downscale: the renderer reads back exactly the frame DLSS wrote. An
// export that wants the model on more pixels than the source has them already
// - Super Resolution plus neural rendering runs it on the upscaled frame.
//
// Measured 2026-09-23, RTX 4080 SUPER, driver 610.47, RenoDX 6.5.3: a whole
// 8 s 3840x2160 30 fps testsrc2 render per rung through the real helper,
// `NeuralRangeRenderSmoke <ffmpeg> <worker> <out> --processing-scale-cost
// 3840x2160 8`, three runs, median, helper start included:
//
//   100%  model 3840x2160  14.8 fps  (14.2 / 14.8 / 15.3)  GPU 18.8 ms/frame
//    75%  model 2880x1620  17.3 fps  (17.2 / 17.3 / 17.4)  GPU 21.3 ms/frame
//    50%  model 1920x1080  22.0 fps  (21.9 / 22.0 / 22.7)  GPU 11.3 ms/frame
//
// The GPU column is the carrier's Evaluate, which now holds the model AND the
// Super Resolution restore: at 75% the upscale to 4K costs more than the
// model saves, and the render still gets faster because at this size the job
// is bound by decode, readback and encode rather than by that one call. At
// 1920x1080 the three rungs rendered 21.4 / 23.4 / 26.1 fps, a single run,
// mostly the helper's start-up. So what a rung buys depends on the GPU and the
// source, which is why the menu prints what was measured and where.
inline constexpr uint32_t kProcessingScaleRungs[]{100, 75, 50};
inline constexpr uint32_t kDefaultProcessingScale = 100;

inline constexpr bool IsProcessingScaleRung(uint32_t percent) noexcept {
    for (const uint32_t rung : kProcessingScaleRungs)
        if (rung == percent) return true;
    return false;
}

// `reduced` is false when the model sees the source as decoded.
struct ProcessingInput { uint32_t width{}, height{}; bool reduced{}; };

// The model's input size at `percent` of a source: even in both dimensions,
// because the job's encoder choice and the renderer's NV12 paths both want
// even sizes, and never zero. At 100 (or any value that is not a rung) it is
// the source size exactly, odd dimensions included.
inline ProcessingInput ProcessingSize(uint32_t width, uint32_t height, uint32_t percent) {
    if (!width || !height || percent >= 100 || !IsProcessingScaleRung(percent))
        return {width, height, false};
    const uint32_t scaledWidth = std::max(2u, uint32_t(std::lround(double(width) * percent / 100.0)) & ~1u);
    const uint32_t scaledHeight = std::max(2u, uint32_t(std::lround(double(height) * percent / 100.0)) & ~1u);
    if (scaledWidth >= width || scaledHeight >= height) return {width, height, false};
    return {scaledWidth, scaledHeight, true};
}

// The cache-key term a rung adds to the pipeline identity. Empty at 100, so a
// default render keeps the exact key it was published under; a reduced rung
// is a different picture and must never be served for another.
inline std::string ProcessingScaleIdentityTerm(uint32_t percent) {
    if (percent == kDefaultProcessingScale) return {};
    return "|processing-scale-" + std::to_string(percent) + "-v1";
}

// The add-on's NRPreUpscale a render at `percent` needs, given the value its
// ini holds now (empty when the key is absent), or nothing to write. A reduced
// rung needs the model ahead of the upscale (1). At 100 the player writes
// nothing unless a reduced render left 1 behind, which it puts back to the
// add-on's default (0) - so a default render's settings, and with them its
// cache key, stay exactly what they were before the rungs existed.
inline std::optional<std::string_view> PreUpscaleOverride(uint32_t percent, std::string_view current) {
    if (percent != kDefaultProcessingScale) return std::string_view("1");
    if (!current.empty() && current != "0") return std::string_view("0");
    return std::nullopt;
}
