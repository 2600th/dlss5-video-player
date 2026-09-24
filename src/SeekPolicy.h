#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

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

// Whether playback resumes after a requested seek. A second request before the
// tick has run the first keeps the first one's intent: the first request
// already paused playback, so reading "playing" again would turn every step of
// a drag after the first into a pause.
inline bool ResumeAfterRequest(bool seekPending, bool pendingResume, bool playing)
{
    return seekPending ? pendingResume : playing;
}

// A restarted decoder that returns no frame at the target - it lies in the
// last frame's tail - is asked again a frame and a half before the end.
// nullopt when there is no earlier target to try.
inline std::optional<double> RetryTarget(double target, double durationSeconds, double frameRate)
{
    const double frameDuration = 1.0 / std::max(1.0, frameRate);
    if (!(durationSeconds > 0.0 && target > 0.0)) return std::nullopt;
    const double safe = std::max(0.0, std::min(target, durationSeconds - frameDuration * 1.5));
    if (safe < target) return safe;
    return std::nullopt;
}

// A plain file plays on after a seek only when a frame follows the one it
// showed; at the end of the file it stays paused on that frame. A cached pair
// reads its next frame on the next tick, so it plays on whenever it was asked to.
inline bool PlaysAfterSeek(bool resumeAfter, bool cachedPlayback, bool haveNext)
{
    return cachedPlayback ? resumeAfter : resumeAfter && haveNext;
}

// A drag preview would respawn the audio helper on every step; the release
// restarts it once.
inline bool RestartsAudioAfterSeek(bool dragSeek) { return !dragSeek; }

} // namespace seek_policy
