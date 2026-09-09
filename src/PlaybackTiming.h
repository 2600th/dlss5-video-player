#pragma once

#include <algorithm>

namespace playback_timing {

inline double TimelinePosition(bool dragging,
                               double previewPosition,
                               bool seekPending,
                               double pendingPosition,
                               double lastPresentedPosition,
                               double /*playbackClockPosition*/)
{
    if (dragging) return previewPosition;
    if (seekPending) return pendingPosition;
    return lastPresentedPosition;
}

inline double PausePosition(double lastPresentedPosition,
                            double /*playbackClockPosition*/)
{
    return lastPresentedPosition;
}

inline double LateFrameThreshold(double frameDuration)
{
    return std::max(0.0, frameDuration) * 1.5;
}

// Rendering cost is linear in pixel count. Measured on the reference GPU
// (RTX 5090, driver 616.64) by timing segment arrivals, which excludes job
// startup: 35.3 ms/frame at 1920x1080, 65.5 at 2560x1440, 142.3 at 3840x2160,
// i.e. 17.1, 17.8 and 17.2 ms per megapixel. A session can only follow live
// playback while that per-frame cost fits inside one frame interval, so a 4K30
// source renders at about 0.2x realtime and would spend its life buffering.
inline constexpr double kNeuralMillisecondsPerMegapixel = 17.2;

struct LiveRenderForecast {
    double msPerFrame;      // predicted render cost of one frame
    double renderFps;       // frames the renderer can produce per second
    double sourceFps;       // frames playback consumes per second
    double realtimeRatio;   // renderFps / sourceFps; 1.0 means it exactly keeps up
    bool keepsUp;
};

inline LiveRenderForecast ForecastLiveRender(uint32_t width, uint32_t height, double sourceFps,
                                             double msPerMegapixel = kNeuralMillisecondsPerMegapixel)
{
    LiveRenderForecast forecast{};
    forecast.sourceFps = sourceFps;
    const double megapixels = (double(width) * double(height)) / 1'000'000.0;
    if (!(megapixels > 0.0) || !(sourceFps > 0.0) || !(msPerMegapixel > 0.0)) {
        // Nothing measurable to forecast: never block the user on a guess.
        forecast.keepsUp = true;
        return forecast;
    }
    forecast.msPerFrame = megapixels * msPerMegapixel;
    forecast.renderFps = 1000.0 / forecast.msPerFrame;
    forecast.realtimeRatio = forecast.renderFps / sourceFps;
    // A couple of percent either way is inside run-to-run noise; only warn when
    // the shortfall is real.
    forecast.keepsUp = forecast.realtimeRatio >= 0.98;
    return forecast;
}

} // namespace playback_timing
