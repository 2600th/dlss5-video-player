#pragma once

#include <windows.h>

#include <span>
#include <optional>
#include <vector>

enum class ToolbarAction {
    Open, Back10, PlayPause, Stop, Forward10, Mute,
    ToggleDlss, Aspect, Adjustments, DebugView, Fullscreen, None
};

struct ToolbarItem {
    ToolbarAction action{ToolbarAction::None};
    RECT bounds{};
    bool compact{false};
};

struct ToolbarAvailability {
    bool mediaLoaded{false};
    bool seeking{false};
    bool rendererReady{false};
};

inline constexpr int kToolbarSpacingDip = 4;
inline constexpr int kToolbarMinHitHeightDip = 36;
inline constexpr int kToolbarCornerRadiusDip = 8;
inline constexpr int kToolbarOuterGutterDip = 16;
inline constexpr int kToolbarGroupGapDip = 12;

std::vector<ToolbarItem> LayoutToolbar(int clientWidth, int clientHeight, UINT dpi);
ToolbarAction HitTestToolbar(std::span<const ToolbarItem> items, POINT point);
int MinimumToolbarClientWidth(UINT dpi);
std::optional<RECT> LayoutVolumeSlider(int clientWidth, int clientHeight, UINT dpi,
                                      std::span<const ToolbarItem> toolbarItems);
bool IsToolbarActionEnabled(ToolbarAction action, ToolbarAvailability availability);
ToolbarAction NextFocusableToolbarAction(std::span<const ToolbarItem> items,
                                         ToolbarAction current, bool reverse,
                                         ToolbarAvailability availability);
std::vector<RECT> HoverDirtyRectangles(std::span<const ToolbarItem> items,
                                       ToolbarAction oldAction,
                                       ToolbarAction newAction);
