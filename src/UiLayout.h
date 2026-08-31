#pragma once

#include <windows.h>

#include <span>
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

inline constexpr int kToolbarSpacingDip = 4;
inline constexpr int kToolbarMinHitHeightDip = 36;
inline constexpr int kToolbarCornerRadiusDip = 8;
inline constexpr int kToolbarOuterGutterDip = 16;
inline constexpr int kToolbarGroupGapDip = 12;

std::vector<ToolbarItem> LayoutToolbar(int clientWidth, int clientHeight, UINT dpi);
ToolbarAction HitTestToolbar(std::span<const ToolbarItem> items, POINT point);
