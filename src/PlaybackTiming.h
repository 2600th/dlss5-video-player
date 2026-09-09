#pragma once

#include <algorithm>

namespace playback_timing {

// Both of these deliberately ignore the playback clock: what the timeline and a
// pause show is the frame the player last presented, never where the clock has
// drifted to. The clock argument used to be passed and discarded, so callers
// computed a position for nothing.
inline double TimelinePosition(bool dragging,
                               double previewPosition,
                               bool seekPending,
                               double pendingPosition,
                               double lastPresentedPosition)
{
    if (dragging) return previewPosition;
    if (seekPending) return pendingPosition;
    return lastPresentedPosition;
}

inline double PausePosition(double lastPresentedPosition)
{
    return lastPresentedPosition;
}

inline double LateFrameThreshold(double frameDuration)
{
    return std::max(0.0, frameDuration) * 1.5;
}

// Rendering cost per frame is a fixed part plus a part proportional to pixel
// count. Measured on the reference GPU (RTX 5090, driver 616.64) by timing
// segment arrivals, which excludes job startup: 12.50 ms/frame at 1920x1080,
// 16.60 at 2560x1440 and 28.07 at 3840x2160. A least-squares fit of those three
// points is 7.35 ms + 2.50 ms per megapixel and reproduces them to within
// 0.06 ms. The fixed part is the guide pass, the DLSS evaluate and the capture's
// fence wait; the proportional part is the readback and the pixel work.
//
// A session can only follow live playback while that cost fits inside one frame
// interval. At these rates 4K30 keeps up with about 19% to spare and 4K60 does
// not, which is a very different conclusion from the same measurement before the
// decoder stopped answering every 4 MiB chunk with a sleep.
inline constexpr double kNeuralFrameFixedMs = 7.35;
inline constexpr double kNeuralMillisecondsPerMegapixel = 2.50;

struct LiveRenderForecast {
    double msPerFrame;      // predicted render cost of one frame
    double renderFps;       // frames the renderer can produce per second
    double sourceFps;       // frames playback consumes per second
    double realtimeRatio;   // renderFps / sourceFps; 1.0 means it exactly keeps up
    bool keepsUp;
};

inline LiveRenderForecast ForecastLiveRender(uint32_t width, uint32_t height, double sourceFps,
                                             double msPerMegapixel = kNeuralMillisecondsPerMegapixel,
                                             double fixedMs = kNeuralFrameFixedMs)
{
    LiveRenderForecast forecast{};
    forecast.sourceFps = sourceFps;
    const double megapixels = (double(width) * double(height)) / 1'000'000.0;
    if (!(megapixels > 0.0) || !(sourceFps > 0.0) || !(msPerMegapixel > 0.0)) {
        // Nothing measurable to forecast: never block the user on a guess.
        forecast.keepsUp = true;
        return forecast;
    }
    forecast.msPerFrame = fixedMs + megapixels * msPerMegapixel;
    forecast.renderFps = 1000.0 / forecast.msPerFrame;
    forecast.realtimeRatio = forecast.renderFps / sourceFps;
    // A couple of percent either way is inside run-to-run noise; only warn when
    // the shortfall is real.
    forecast.keepsUp = forecast.realtimeRatio >= 0.98;
    return forecast;
}

} // namespace playback_timing
