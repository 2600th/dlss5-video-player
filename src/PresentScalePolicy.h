#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// How the player's last scaling step is done. The swapchain used to stay at the
// output's size with DXGI_SCALING_STRETCH, so DWM resampled every frame to the
// window with a bilinear filter: a 3840-wide frame in the default 1440-wide window
// was decimated about 2.7x with no prefilter, and aliased, in a player whose whole
// subject is fine detail. The backbuffers now follow the window and the present
// pass does the scaling (PSPresentScaled in D3D12Renderer.cpp).
namespace present_scale {

struct Target {
    uint32_t width{};
    uint32_t height{};
    // True when the output is resampled into a backbuffer of another size.
    bool scaled{};
};

// A renderer that does not follow its window - the offline carrier, whose hidden
// window's size means nothing - draws at the output's size through PSPresent
// exactly as it always did. A window that is the output's size draws through
// PSPresent too unless `compose` asks for the compositor - and the player's
// renderer always asks (D3D12Renderer::CurrentPresentTarget), because the
// compositor is also what dithers the window's 8-bit store (DitherPolicy.h). So in
// practice PSPresent draws only the offline carrier's hidden window and the cache
// capture. At 1:1 the scaled pass takes one bilinear tap at each texel's centre, so
// the picture under the dither and any marks is the one PSPresent would have drawn.
inline Target Choose(bool followsWindow, bool scaledPresentAvailable,
                     uint32_t backbufferW, uint32_t backbufferH,
                     uint32_t outputW, uint32_t outputH, bool compose = false)
{
    const bool scaled = followsWindow && scaledPresentAvailable && backbufferW && backbufferH &&
                        (compose || backbufferW != outputW || backbufferH != outputH);
    return scaled ? Target{backbufferW, backbufferH, true} : Target{outputW, outputH, false};
}

// Bilinear taps per axis the scaled present averages over one screen pixel whose
// footprint covers `texelsPerPixel` texels: one when magnifying, then one more per
// texel of footprint, capped at eight. Mirrors SampleFootprint in the shader; the
// 0.01 slack keeps an exact 2:1 at two taps rather than letting float error make
// it three.
inline uint32_t FootprintTaps(float texelsPerPixel)
{
    const float taps = std::ceil(texelsPerPixel - 0.01f);
    return static_cast<uint32_t>(std::clamp(taps, 1.0f, 8.0f));
}

} // namespace present_scale
