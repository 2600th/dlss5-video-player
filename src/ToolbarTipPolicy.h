#pragma once

#include <string_view>

#include "UiLayout.h"

// What each toolbar control says on hover, and how soon. Every control gets a
// tip, and a control with a key names it on the tip's first line, in the
// "Name (Key)" form the compare bar's tips already use - hovering is where a
// mouse user learns the keyboard.
//
// A tip that names a key is only as good as the key. The Fullscreen tip said
// "Escape or F leaves" for as long as F has been "Preview this frame", which
// starts a neural render; so each tip is tied to the menu row that owns the
// same command, and PolicyTests checks the key in one against the other.
namespace toolbar_tips {

// Long enough that sweeping the cursor across the bar raises nothing; short
// enough that resting on a control feels answered. Moving from one control to
// the next while a tip is up shows the next at once (the reshow delay), the
// way a menu bar behaves once it is open.
inline constexpr unsigned kInitialDelayMs = 500;
inline constexpr unsigned kReshowDelayMs = 30;
inline constexpr unsigned kAutoPopMs = 30000;

inline const wchar_t* TipKey(ToolbarAction action)
{
    switch (action) {
    case ToolbarAction::Open: return L"toolbar.tip.open";
    case ToolbarAction::OpenYouTube: return L"toolbar.tip.youtube";
    case ToolbarAction::Back10: return L"toolbar.tip.back10";
    case ToolbarAction::PlayPause: return L"toolbar.tip.playpause";
    case ToolbarAction::Stop: return L"toolbar.tip.stop";
    case ToolbarAction::Forward10: return L"toolbar.tip.forward10";
    case ToolbarAction::Mute: return L"toolbar.tip.mute";
    case ToolbarAction::ToggleNeuralRendering: return L"toolbar.tip.neural";
    case ToolbarAction::ToggleUpscaling: return L"toolbar.tip.upscaling";
    case ToolbarAction::ToggleFrameGeneration: return L"toolbar.tip.framegen";
    case ToolbarAction::Aspect: return L"toolbar.tip.aspect";
    case ToolbarAction::Adjustments: return L"toolbar.tip.color";
    case ToolbarAction::DebugView: return L"toolbar.tip.debug";
    case ToolbarAction::Fullscreen: return L"toolbar.tip.fullscreen";
    case ToolbarAction::None: break;
    }
    return nullptr;
}

// The menu row that runs the same command, whose accelerator the tip must
// name. Null for a control with no key of its own.
inline const wchar_t* MenuKey(ToolbarAction action)
{
    switch (action) {
    case ToolbarAction::Open: return L"menu.open";
    case ToolbarAction::OpenYouTube: return L"menu.open_youtube";
    case ToolbarAction::Back10: return L"menu.back10";
    case ToolbarAction::PlayPause: return L"menu.playpause";
    case ToolbarAction::Stop: return L"menu.stop";
    case ToolbarAction::Forward10: return L"menu.forward10";
    case ToolbarAction::Mute: return L"menu.mute";
    case ToolbarAction::ToggleNeuralRendering: return L"menu.neural_rendering";
    case ToolbarAction::Aspect: return L"menu.aspectfit";
    case ToolbarAction::Adjustments: return L"menu.adjustments";
    case ToolbarAction::DebugView: return L"menu.final";
    case ToolbarAction::Fullscreen: return L"menu.fullscreen";
    default: return nullptr;
    }
}

// "Play / Pause\tSpace   (Overlay: Ctrl+Alt+Space)" -> "Space": the text
// after the tab, up to a run of spaces that starts a note.
inline std::wstring_view MenuAccelerator(std::wstring_view label)
{
    const size_t tab = label.find(L'\t');
    if (tab == std::wstring_view::npos) return {};
    std::wstring_view accelerator = label.substr(tab + 1);
    const size_t note = accelerator.find(L"  ");
    return note == std::wstring_view::npos ? accelerator : accelerator.substr(0, note);
}

// "Fullscreen (F11)\nEsc or F11 leaves." -> "F11": the parenthesised key at
// the end of the first line, or nothing.
inline std::wstring_view TipShortcut(std::wstring_view tip)
{
    const std::wstring_view first = tip.substr(0, tip.find(L'\n'));
    if (first.empty() || first.back() != L')') return {};
    const size_t open = first.rfind(L'(');
    if (open == std::wstring_view::npos) return {};
    return first.substr(open + 1, first.size() - open - 2);
}

} // namespace toolbar_tips
