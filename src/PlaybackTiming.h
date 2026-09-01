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

} // namespace playback_timing
