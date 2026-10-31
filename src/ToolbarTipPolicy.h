#pragma once

#include <algorithm>
#include <string>
#include <string_view>

#include "AppMenu.h"
#include "UiLayout.h"

// What each toolbar control says on hover, and how soon. Every control gets a
// tip, and a control with a key names it on the tip's first line, in the
// "Name (Key)" form the compare bar's tips already use - hovering is where a
// mouse user learns the keyboard.
//
// A tip that names a key is only as good as the key. The Fullscreen tip said
// "Escape or F leaves" for as long as F has been "Preview this frame", which
// starts a neural render; so each tip is tied to the command it runs and names
// that command's keys from the key table (KeyBindings.h), the one the menus
// print too.
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
    case ToolbarAction::NeuralMenu: return L"toolbar.tip.neural_menu";
    case ToolbarAction::Upscaler: return L"toolbar.tip.upscaler";
    case ToolbarAction::Export: return L"toolbar.tip.export";
    case ToolbarAction::None: break;
    }
    return nullptr;
}

// The command a control runs, whose keys its tip names; 0 for a control with
// no key of its own. The key is not written into the tip's text: WithKeys adds
// it from the live key table, so a key rebound in input.conf is the key the tip
// names, and a translation never carries a key it could get wrong.
inline UINT Command(ToolbarAction action)
{
    switch (action) {
    case ToolbarAction::Open: return app_menu::IDM_OPEN;
    case ToolbarAction::OpenYouTube: return app_menu::IDM_OPEN_YOUTUBE;
    case ToolbarAction::Back10: return app_menu::IDM_BACK10;
    case ToolbarAction::PlayPause: return app_menu::IDM_PLAY;
    case ToolbarAction::Stop: return app_menu::IDM_STOP;
    case ToolbarAction::Forward10: return app_menu::IDM_FWD10;
    case ToolbarAction::Mute: return app_menu::IDM_MUTE;
    case ToolbarAction::ToggleNeuralRendering: return app_menu::IDM_NEURAL_RENDERING;
    case ToolbarAction::Aspect: return app_menu::IDM_ASPECT_TOGGLE;
    // Picture opens the panel on its render zone, as Ctrl+N does.
    case ToolbarAction::Adjustments: return app_menu::IDM_NEURAL_SETTINGS;
    case ToolbarAction::Export: return app_menu::IDM_EXPORT_STAGES;
    case ToolbarAction::Fullscreen: return app_menu::IDM_FULLSCREEN;
    default: return 0;
    }
}

// "Fullscreen\nEsc leaves." with "F11" -> "Fullscreen (F11)\nEsc leaves.": the
// keys in the "Name (Key)" form the compare bar's tips use, at the end of the
// first line. No keys, no parentheses.
inline std::wstring WithKeys(std::wstring_view tip, std::wstring_view keys)
{
    const size_t end = std::min(tip.size(), tip.find(L'\n'));
    std::wstring text(tip.substr(0, end));
    if (!keys.empty()) text.append(L" (").append(keys).append(L")");
    return text.append(tip.substr(end));
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
