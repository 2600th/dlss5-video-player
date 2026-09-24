#pragma once

#include <algorithm>
#include <cstdint>

#include "NeuralRenderTypes.h"

// Where a seek is allowed to land. PlayerApp reads the decoder and the cached
// pair it is playing; every decision about the target is made here, where it
// can be tested without a decoder, a renderer or a render.
namespace seek_policy {

struct Timeline {
    // The container's duration; 0 when it is unknown (a stream still arriving).
    double durationSeconds{};
    double frameRate{};
    // LastFramePts for this source: the timestamp of its last whole frame, or
    // 0 when the rate or the duration is invalid.
    int64_t lastFramePts100ns{};
    // A synchronized cache pair is on screen.
    bool cachedPlayback{};
    // That pair belongs to an active (live) session rather than a finished
    // cache entry.
    bool liveSession{};
    // The range the cached entry was rendered for; Whole() for a full render.
    NeuralRenderRange cachedRange{};
};

// A cached range entry only holds [start,end); seeking outside it would
// desynchronize the pair, so the timeline is clamped to the last range frame.
inline double Clamp(double seconds, const Timeline& timeline)
{
    double low = 0.0, high = timeline.durationSeconds;
    // Seeking to the container end has no frame to decode: the restarted
    // decoder returns nothing and the seek pays for a second restart.
    if (timeline.lastFramePts100ns > 0) high = std::min(high, double(timeline.lastFramePts100ns) * 1e-7);
    // A cached entry can only serve its own range. An active session is
    // different now: its coverage is a set of regions with holes between
    // them, and the original plays in the holes, so every seek target in the
    // source is legal. Clamping to the newest rendered frame is what made a
    // seek back to an earlier rendered region impossible to even express -
    // the target was pulled forward to the clamp before anything could
    // answer whether it was rendered.
    if (timeline.cachedPlayback && !timeline.liveSession && !timeline.cachedRange.Whole()) {
        low = double(timeline.cachedRange.start100ns) * 1e-7;
        high = std::max(low, double(timeline.cachedRange.end100ns) * 1e-7 -
                                 1.0 / std::max(1.0, timeline.frameRate));
    }
    if (high > 0) return std::clamp(seconds, low, high);
    return std::max(low, seconds);
}

} // namespace seek_policy
