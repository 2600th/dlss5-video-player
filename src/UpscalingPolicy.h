#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

struct UpscalingSize { uint32_t width{}, height{}; bool grows{}; };

// A target is a bounding box, not a reason to discard source detail.
inline UpscalingSize UpscalingTarget(uint32_t width, uint32_t height, uint32_t targetHeight) {
    if (!width || !height || (targetHeight != 1440 && targetHeight != 2160))
        return {width,height,false};
    const uint32_t targetWidth = targetHeight == 2160 ? 3840 : 2560;
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
