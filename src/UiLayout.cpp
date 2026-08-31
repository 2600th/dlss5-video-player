#include "UiLayout.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace {

constexpr int kToolbarCompactWidthDip = 44;
constexpr int kToolbarSmallestWidthDip = 36;
constexpr int kToolbarTopOffsetDip = 96;

struct ToolbarDefinition {
    ToolbarAction action;
    int widthDip;
    int group;
    bool requiredAtNarrowWidths;
};

constexpr std::array kToolbarDefinitions{
    ToolbarDefinition{ToolbarAction::Open, 68, 0, true},
    ToolbarDefinition{ToolbarAction::Back10, 44, 0, false},
    ToolbarDefinition{ToolbarAction::PlayPause, 62, 0, true},
    ToolbarDefinition{ToolbarAction::Stop, 48, 0, false},
    ToolbarDefinition{ToolbarAction::Forward10, 44, 0, false},
    ToolbarDefinition{ToolbarAction::Mute, 54, 0, true},
    ToolbarDefinition{ToolbarAction::ToggleDlss, 82, 1, true},
    ToolbarDefinition{ToolbarAction::Aspect, 72, 1, false},
    ToolbarDefinition{ToolbarAction::Adjustments, 66, 1, false},
    ToolbarDefinition{ToolbarAction::DebugView, 72, 2, false},
    ToolbarDefinition{ToolbarAction::Fullscreen, 54, 2, true},
};

int DipToPixels(int dip, UINT dpi)
{
    return MulDiv(dip, static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi),
                  USER_DEFAULT_SCREEN_DPI);
}

int GapPixels(const ToolbarDefinition& previous, const ToolbarDefinition& current, UINT dpi)
{
    return DipToPixels(previous.group == current.group ? kToolbarSpacingDip : kToolbarGroupGapDip, dpi);
}

int LayoutWidth(std::span<const ToolbarDefinition* const> definitions, int itemWidthDip, UINT dpi)
{
    int width = 0;
    for (size_t index = 0; index < definitions.size(); ++index) {
        if (index != 0) width += GapPixels(*definitions[index - 1], *definitions[index], dpi);
        const int widthDip = itemWidthDip == 0 ? definitions[index]->widthDip : itemWidthDip;
        width += DipToPixels(widthDip, dpi);
    }
    return width;
}

} // namespace

std::vector<ToolbarItem> LayoutToolbar(int clientWidth, int clientHeight, UINT dpi)
{
    if (clientWidth <= 0 || clientHeight <= 0) return {};

    std::array<const ToolbarDefinition*, kToolbarDefinitions.size()> all{};
    std::array<const ToolbarDefinition*, 5> required{};
    size_t requiredCount = 0;
    for (size_t index = 0; index < kToolbarDefinitions.size(); ++index) {
        all[index] = &kToolbarDefinitions[index];
        if (kToolbarDefinitions[index].requiredAtNarrowWidths) {
            required[requiredCount++] = &kToolbarDefinitions[index];
        }
    }

    const int gutter = DipToPixels(kToolbarOuterGutterDip, dpi);
    const int availableWidth = std::max(0, clientWidth - 2 * gutter);
    std::span<const ToolbarDefinition* const> selected{all};
    bool compact = false;
    int itemWidthDip = 0;

    if (LayoutWidth(selected, 0, dpi) > availableWidth) {
        compact = true;
        itemWidthDip = kToolbarCompactWidthDip;
        if (LayoutWidth(selected, itemWidthDip, dpi) > availableWidth) {
            selected = std::span<const ToolbarDefinition* const>{required.data(), requiredCount};
            if (LayoutWidth(selected, itemWidthDip, dpi) > availableWidth) {
                itemWidthDip = kToolbarSmallestWidthDip;
            }
        }
    }

    const int itemHeight = DipToPixels(kToolbarMinHitHeightDip, dpi);
    const int top = std::max(0, clientHeight - DipToPixels(kToolbarTopOffsetDip, dpi));
    int left = gutter;
    std::vector<ToolbarItem> items;
    items.reserve(selected.size());
    for (size_t index = 0; index < selected.size(); ++index) {
        if (index != 0) left += GapPixels(*selected[index - 1], *selected[index], dpi);
        const int widthDip = itemWidthDip == 0 ? selected[index]->widthDip : itemWidthDip;
        const int width = DipToPixels(widthDip, dpi);
        items.push_back(ToolbarItem{
            selected[index]->action,
            RECT{left, top, left + width, top + itemHeight},
            compact,
        });
        left += width;
    }
    return items;
}

ToolbarAction HitTestToolbar(std::span<const ToolbarItem> items, POINT point)
{
    for (const auto& item : items) {
        if (point.x >= item.bounds.left && point.x < item.bounds.right &&
            point.y >= item.bounds.top && point.y < item.bounds.bottom) {
            return item.action;
        }
    }
    return ToolbarAction::None;
}
