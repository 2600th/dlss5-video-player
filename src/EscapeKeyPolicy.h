#pragma once

// What one press of Esc does, in the order the player asks. Fullscreen comes
// before every job: Esc leaves fullscreen in every other player, and a viewer
// who pressed it to get their desktop back stopped a live render instead - a
// render that had cost a 17-22 s cold start. In a window Esc keeps its old
// meaning, the longest-running thing is stopped first.
namespace escape_key {

enum class Action {
    None,
    CloseShortcutSheet,
    LeaveFullscreen,
    CancelFrameGeneration,
    StopLiveSession,
    CancelNeuralJob,
    CancelYouTube,
};

struct State {
    bool shortcutSheetOpen{};
    bool fullscreen{};
    bool frameGenerationCancellable{};
    bool liveSession{};
    bool neuralJob{};
    bool resolvingYouTube{};
};

inline Action Resolve(const State& state)
{
    // The sheet is on top of everything, fullscreen included: Esc puts away
    // what the viewer is looking at.
    if (state.shortcutSheetOpen) return Action::CloseShortcutSheet;
    if (state.fullscreen) return Action::LeaveFullscreen;
    if (state.frameGenerationCancellable) return Action::CancelFrameGeneration;
    if (state.liveSession) return Action::StopLiveSession;
    if (state.neuralJob) return Action::CancelNeuralJob;
    if (state.resolvingYouTube) return Action::CancelYouTube;
    return Action::None;
}

} // namespace escape_key
