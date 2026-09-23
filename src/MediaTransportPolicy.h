#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "UiResources.h"

// What the player tells Windows' media controls and its taskbar thumbnail,
// and what it does when either is pressed. The WinRT and shell calls live in
// main.cpp; every decision they carry out is made here, where it can be
// tested without a session, a taskbar or a flyout.
namespace media_transport {

// ---- System Media Transport Controls --------------------------------------

// Windows.Media.SystemMediaTransportControlsButton, by value: Play, Pause and
// Stop are the three this player enables. Named here rather than taken from
// windows.media.h so the policy compiles without the WinRT headers.
inline constexpr int kSmtcPlay = 0;
inline constexpr int kSmtcPause = 1;
inline constexpr int kSmtcStop = 2;

enum class Action { None, TogglePause, Stop };

// The flyout's Play and Pause are requests for a state, not a toggle: a Play
// that arrives while playing (the flyout and the player can disagree for a
// moment) must not pause. `playing` counts a press already queued behind a
// buffering session, the same fact the toolbar's Play/Pause label reads.
inline Action ActionForButton(int button, bool loaded, bool playing)
{
    if (!loaded) return Action::None;
    switch (button) {
    case kSmtcPlay: return playing ? Action::None : Action::TogglePause;
    case kSmtcPause: return playing ? Action::TogglePause : Action::None;
    case kSmtcStop: return Action::Stop;
    default: return Action::None;
    }
}

// Windows.Media.MediaPlaybackStatus, by value.
enum class Status : int { Closed = 0, Changing = 1, Stopped = 2, Playing = 3, Paused = 4 };

inline Status StatusFor(bool loaded, bool playing)
{
    if (!loaded) return Status::Closed;
    return playing ? Status::Playing : Status::Paused;
}

// The flyout's seek bar extrapolates between updates by itself, so the
// position is only pushed when it has to be: the first time, when playback
// starts or stops, when the playhead jumps (a seek), and otherwise every five
// seconds to correct drift. Pushing it every frame would be a cross-process
// WinRT call per frame for a bar almost nobody is looking at.
struct TimelinePush {
    bool pushed{};
    bool playing{};
    double position{};
    std::chrono::steady_clock::time_point at{};
};

inline bool ShouldPushTimeline(const TimelinePush& last, double position, bool playing,
                               std::chrono::steady_clock::time_point now)
{
    if (!last.pushed || last.playing != playing) return true;
    const double elapsed = std::chrono::duration<double>(now - last.at).count();
    const double expected = last.position + (playing ? elapsed : 0.0);
    if (std::abs(position - expected) > 1.0) return true;
    return elapsed >= 5.0;
}

// ---- Taskbar thumbnail buttons ----------------------------------------------

// The three buttons under the taskbar thumbnail, in order. The set is fixed
// when it is created - Windows allows at most seven and never a new one - so
// a control that does not apply is disabled rather than removed.
struct ThumbButton {
    UINT command{};
    UiIcon icon{};
    const wchar_t* tipKey{};
    bool enabled{};

    friend bool operator==(const ThumbButton&, const ThumbButton&) = default;
};

using ThumbBar = std::array<ThumbButton, 3>;

struct ThumbState {
    bool loaded{};
    bool playing{};
    bool neuralAvailable{};
    bool neuralOn{};
    bool compareAvailable{};
    bool comparing{};
};

// `playCommand`, `neuralCommand` and `compareCommand` are the menu commands
// the buttons send, so a thumbnail press takes exactly the path the menu does.
inline ThumbBar ThumbButtonsFor(const ThumbState& state, UINT playCommand, UINT neuralCommand, UINT compareCommand)
{
    return ThumbBar{
        ThumbButton{playCommand, state.playing ? UiIcon::Pause : UiIcon::Play,
                    state.playing ? L"thumb.pause" : L"thumb.play", state.loaded},
        ThumbButton{neuralCommand, UiIcon::Sparkles,
                    state.neuralOn ? L"thumb.neural_off" : L"thumb.neural_on",
                    state.loaded && state.neuralAvailable},
        ThumbButton{compareCommand, UiIcon::Compare,
                    state.comparing ? L"thumb.compare_off" : L"thumb.compare_on",
                    state.loaded && state.compareAvailable},
    };
}

} // namespace media_transport
