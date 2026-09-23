#pragma once

#include <algorithm>
#include <cmath>

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
// Recovery is deliberate rather than latched: an underrun that resolves
// returns to the audio clock (through Present's slew, below), and one hiccup
// does not demote audio for the rest of a film.
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

// What the player is handed while the audio clock is not a clock, and on the
// way back to it.
//
// A stall used to hand the caller -1, and the caller fell back to its own
// steady clock anchored at the last seek or resume rather than at the audio;
// when audio moved again the clock snapped straight back to it. After a 1 s
// YouTube underrun video ran ahead on the steady clock and then stood still
// for a second while the audio caught up with it.
//
// Now the position carries on from the last one handed out, at wall-clock
// rate, and when audio resumes the clock slews back to it at no more than
// kMaxSlewRate - a speed change nobody sees - instead of jumping. A gap too
// wide to close that way is closed in one step, as it was before.
struct Continuity {
    double presented = 0.0;  // the last position handed out
    double at = 0.0;         // wall time it was handed out at
    bool have = false;
    // Stalled, or slewing back after one. Off, the audio position is handed
    // through untouched, so a healthy clock behaves exactly as it always did.
    bool bridging = false;
};

// 5 %: well inside the rate error a viewer notices in video, and it walks a
// half-second gap - the least a declared stall can leave - shut in ten
// seconds of playback.
inline constexpr double kMaxSlewRate = 0.05;

// Past this the gap is closed in one step. It is playback_cadence's
// kReanchorSeconds, the lateness at which the player would rather seek than
// walk: slewing a longer gap would leave sound and picture visibly apart for
// longer than a stall that size is worth.
inline constexpr double kSnapSeconds = 1.0;

// `usable` is Usable()'s answer for `audioPosition`, which is ignored when it
// is false. `playing` is false while paused, when the clock stands still.
inline double Present(Continuity& state, double audioPosition, bool usable, double wallSeconds,
                      bool playing)
{
    const double elapsed = state.have && playing ? std::max(0.0, wallSeconds - state.at) : 0.0;
    state.at = wallSeconds;
    if (!state.have) {
        state.have = true;
        state.bridging = !usable;
        state.presented = audioPosition;
        return state.presented;
    }
    // Carried forward from the last position handed out, not from the last
    // one audio reported: the stall window has already held video on that
    // one for kStallSeconds, and starting from it again keeps the clock
    // continuous rather than jumping it forward by the window.
    if (!usable) {
        state.bridging = true;
        state.presented += elapsed;
        return state.presented;
    }
    if (!state.bridging) {
        state.presented = audioPosition;
        return state.presented;
    }
    const double predicted = state.presented + elapsed;
    const double error = audioPosition - predicted;
    const double step = kMaxSlewRate * elapsed;
    if (std::abs(error) >= kSnapSeconds || std::abs(error) <= step) {
        state.bridging = false;
        state.presented = audioPosition;
        return state.presented;
    }
    state.presented = predicted + (error > 0.0 ? step : -step);
    return state.presented;
}

// The player stops reading the clock while it is paused, so neither window
// sees the pause go by: without this, the first read after a long pause
// counted the whole pause as a stall, and as time for the carried clock to
// advance. Called on both edges, it restarts both windows from now.
inline void PauseChanged(StallState& stall, Continuity& continuity, double wallSeconds)
{
    stall.lastMovedAt = wallSeconds;
    continuity.at = wallSeconds;
}

inline void Reset(Continuity& state) { state = {}; }

} // namespace audio_clock
