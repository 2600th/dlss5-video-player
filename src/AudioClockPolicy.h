#pragma once

// Whether the audio clock is still advancing, and therefore still a clock.
//
// The player runs on an audio master clock: Position() returns the audio
// position whenever there is one and falls back to a steady clock otherwise.
// Nothing noticed when the audio position stopped moving but remained
// available - the reader thread ending on pipe EOF, the ffmpeg child dying, or
// waveOutWrite failing after a device change all leave the queued buffers to
// drain and waveOutGetPosition frozen at its last value, while hasAudioData
// stays set until Stop(). The presentation gate only shows a frame once the
// clock reaches its due time, so a frozen clock stops video for the rest of
// the file with no error and no log line.
namespace audio_clock {

// Wall time a playing clock may stand still before it stops being trusted.
// waveOutGetPosition quantizes to the shared-mode engine period - about 10 ms,
// which is longer than a frame interval at 119.88 fps - so the window has to be
// far wider than one frame. Half of playback_cadence::kReanchorSeconds keeps a
// genuine stall well inside the point at which the player would rather seek
// than walk, while leaving an underrun room to recover.
inline constexpr double kStallSeconds = 0.5;

struct StallState {
    double lastPosition = 0.0;
    double lastMovedAt = 0.0;
    bool have = false;
};

// True while `position` may be used as the master clock. `wallSeconds` is any
// monotonic clock and `playing` is false whenever the clock is not expected to
// advance, which is the paused case: a paused clock standing still is correct,
// not stalled.
//
// Recovery is deliberate rather than latched. The player already switches
// between the audio and steady clocks whenever audio starts or stops, so an
// underrun that resolves returns to the audio clock the same way, and one
// hiccup does not demote audio for the rest of a film.
inline bool Usable(StallState& state, double position, double wallSeconds, bool playing)
{
    if (!state.have || position != state.lastPosition) {
        state.have = true;
        state.lastPosition = position;
        state.lastMovedAt = wallSeconds;
        return true;
    }
    // Not expected to advance, so standing still says nothing. Carrying the
    // window forward means a long pause does not arrive at the resume already
    // counted as a stall.
    if (!playing) {
        state.lastMovedAt = wallSeconds;
        return true;
    }
    return wallSeconds - state.lastMovedAt < kStallSeconds;
}

inline void Reset(StallState& state) { state = {}; }

} // namespace audio_clock
