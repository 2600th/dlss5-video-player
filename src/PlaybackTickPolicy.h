#pragma once

#include <algorithm>
#include <cstdint>

#include "PlaybackTiming.h"

// What the playback loop does, given the clock, pause, seek and read-ahead
// state. PlayerApp::TickOnce, Position and SetPaused carry the decisions out
// against the real decoder, audio and renderer; they are made here so they can
// be tested with fake inputs. That is what P0.2 and P1.4 lacked: both were
// bugs in what the loop did with the audio clock, and neither could be driven
// without a device.
namespace playback_tick {

// ---- The playback clock ----------------------------------------------------

struct ClockState {
    bool loaded{};
    bool playing{};
    // The last presented (or seeked-to) position; what a pause shows.
    double currentSeconds{};
    // Where the steady clock was anchored at the last seek or resume.
    double playStartSeconds{};
    // The container's duration; 0 when it is unknown.
    double durationSeconds{};
};

// The position playback is at. `audioSeconds()` is the audio clock, negative
// when there is none (no audio stream, or no endpoint); `wallSinceStart()` is
// the steady clock's seconds since the anchor.
//
// A paused player answers with the frame it presented and never reads the
// audio clock: reading it is not free of consequences (audio_clock::Present
// carries its continuity forward on every read), and a stale reading is what
// P0.2 restarted audio from - 50 s away from a paused seek. A stall no longer
// reaches this function as a negative reading: audio_clock::Present carries
// the clock through it (P1.4), so the steady-clock fallback below is only for
// media that has no audio clock at all.
template<class AudioSeconds, class WallSeconds>
double Position(const ClockState& state, AudioSeconds&& audioSeconds, WallSeconds&& wallSinceStart)
{
    if (!state.loaded) return 0;
    if (!state.playing) return state.currentSeconds;
    const double audio = audioSeconds();
    const double duration = state.durationSeconds;
    if (audio >= 0.0) return duration > 0 ? std::clamp(audio, 0.0, duration) : audio;
    const double steady = state.playStartSeconds + wallSinceStart();
    return duration > 0 ? std::clamp(steady, 0.0, duration) : std::max(0.0, steady);
}

// ---- One tick ----------------------------------------------------------------

struct TickState {
    bool loaded{};
    bool playing{};
    // A seek transaction is running (PerformSeek) ...
    bool seeking{};
    // ... or one has been requested and runs at the top of the next tick.
    bool seekPending{};
    // A synchronized cache pair, or a live network stream, is what plays.
    bool cachedPlayback{};
    bool networkPlayback{};
    // A decoded frame is queued for its due time.
    bool haveNext{};
};

// A requested seek runs before anything else in the tick, and the tick ends
// with it: seeks are transactional and performed from Tick, never from a
// mouse message.
inline bool PerformsPendingSeek(const TickState& state) { return state.seekPending; }

// A paused frame is presented when something invalidated it - a window resize
// or repaint, or a renderer setting the present pass reads - and not
// otherwise. It used to be re-presented at 60 Hz whether or not anything had
// changed: a full-screen draw and a Present per 16 ms of every pause, for an
// image the compositor already holds.
inline bool PresentsPausedFrame(const TickState& state, bool haveRenderer, bool presentPending,
                                bool presentationStale)
{
    return state.loaded && !state.playing && !state.seeking && haveRenderer && (presentPending || presentationStale);
}

// The two read-aheads that refill an empty queue before the frame is timed.
// Only a cached pair and a network stream read here; a local file reads the
// frame after the one it presents, at the end of the tick.
inline bool ReadsCachedAhead(const TickState& state)
{
    return state.loaded && state.cachedPlayback && state.playing && !state.haveNext && !state.seeking;
}
inline bool ReadsNetworkAhead(const TickState& state)
{
    return state.loaded && state.playing && state.networkPlayback && !state.haveNext && !state.seeking;
}

// Whether the tick goes on to time the queued frame at all.
inline bool AdvancesFrame(const TickState& state)
{
    return state.loaded && state.playing && state.haveNext && !state.seeking;
}

// A frame this late is dropped rather than presented (never a cached pair's:
// see PlaybackCadence.h). Written as the negation of "on time" so a clock
// that is not a number drops, as the loop always did.
inline bool IsLate(double now, double due, double frameDuration)
{
    return !(now - due <= playback_timing::LateFrameThreshold(frameDuration));
}

// A frame more than a millisecond early waits for a later tick.
inline bool IsEarly(double now, double due) { return now + 0.001 < due; }

// The queue ran dry. A local file or a cached pair has reached its end (or a
// decode stopped, which says so itself), so playback stops; a network stream
// is only waiting for data and stays in the playing state.
inline bool EndsPlaybackWhenQueueEmpty(bool networkPlayback) { return !networkPlayback; }

// How long the main pump may block before the next tick: a paused, settled
// player has nothing due for a while; anything else ticks at once.
inline uint32_t SleepMs(const TickState& state)
{
    return (state.loaded && !state.playing && !state.seekPending && !state.seeking) ? 8u : 0u;
}

// Play on a local file with nothing queued - it played to its end, or its
// decode stopped - restarts through a seek (status_note::PlayRestartSeconds
// says where) instead of resuming a clock with no frame behind it. A cached
// pair and a stream refill their own queues.
inline bool ResumeRestartsWithSeek(const TickState& state, double durationSeconds)
{
    return !state.cachedPlayback && !state.networkPlayback && !state.haveNext && durationSeconds > 0;
}

} // namespace playback_tick
