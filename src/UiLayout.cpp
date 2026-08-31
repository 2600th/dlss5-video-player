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

constexpr size_t RequiredToolbarItemCount()
{
    size_t count = 0;
    for (const auto& definition : kToolbarDefinitions) {
        if (definition.requiredAtNarrowWidths) ++count;
    }
    return count;
}

constexpr size_t kRequiredToolbarItemCount = RequiredToolbarItemCount();

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

std::array<const ToolbarDefinition*, kRequiredToolbarItemCount> RequiredToolbarDefinitions()
{
    std::array<const ToolbarDefinition*, kRequiredToolbarItemCount> required{};
    size_t requiredCount = 0;
    for (const auto& definition : kToolbarDefinitions) {
        if (definition.requiredAtNarrowWidths) required[requiredCount++] = &definition;
    }
    return required;
}

} // namespace

std::vector<ToolbarItem> LayoutToolbar(int clientWidth, int clientHeight, UINT dpi)
{
    if (clientWidth <= 0 || clientHeight <= 0) return {};

    std::array<const ToolbarDefinition*, kToolbarDefinitions.size()> all{};
    const auto required = RequiredToolbarDefinitions();
    for (size_t index = 0; index < kToolbarDefinitions.size(); ++index) {
        all[index] = &kToolbarDefinitions[index];
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
            selected = std::span<const ToolbarDefinition* const>{required};
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

int MinimumToolbarClientWidth(UINT dpi)
{
    const auto required = RequiredToolbarDefinitions();
    const std::span<const ToolbarDefinition* const> selected{required};
    return 2 * DipToPixels(kToolbarOuterGutterDip, dpi) +
           LayoutWidth(selected, kToolbarSmallestWidthDip, dpi);
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

std::optional<RECT> LayoutVolumeSlider(int clientWidth, int clientHeight, UINT dpi,
                                      std::span<const ToolbarItem> toolbarItems)
{
    if (clientWidth <= 0 || clientHeight <= 0) return std::nullopt;
    const RECT candidate{
        clientWidth - DipToPixels(185, dpi),
        clientHeight - DipToPixels(69, dpi),
        clientWidth - DipToPixels(95, dpi),
        clientHeight - DipToPixels(61, dpi),
    };
    if (candidate.left < 0 || candidate.top < 0 || candidate.right <= candidate.left ||
        candidate.bottom <= candidate.top) {
        return std::nullopt;
    }
    for (const auto& item : toolbarItems) {
        if (candidate.left < item.bounds.right && candidate.right > item.bounds.left &&
            candidate.top < item.bounds.bottom && candidate.bottom > item.bounds.top) {
            return std::nullopt;
        }
    }
    return candidate;
}

bool IsToolbarActionEnabled(ToolbarAction action, ToolbarAvailability availability)
{
    switch (action) {
    case ToolbarAction::Open:
    case ToolbarAction::Fullscreen:
        return true;
    case ToolbarAction::Back10:
    case ToolbarAction::PlayPause:
    case ToolbarAction::Stop:
    case ToolbarAction::Forward10:
    case ToolbarAction::Mute:
    case ToolbarAction::Aspect:
        return availability.mediaLoaded && !availability.seeking;
    case ToolbarAction::ToggleDlss:
        return availability.mediaLoaded && !availability.seeking && availability.rendererReady;
    case ToolbarAction::Adjustments:
    case ToolbarAction::DebugView:
        return availability.mediaLoaded && availability.rendererReady;
    case ToolbarAction::None:
        return false;
    }
    return false;
}

ToolbarAction NextFocusableToolbarAction(std::span<const ToolbarItem> items,
                                         ToolbarAction current, bool reverse,
                                         ToolbarAvailability availability)
{
    if (items.empty()) return ToolbarAction::None;
    int currentIndex = -1;
    for (size_t index = 0; index < items.size(); ++index) {
        if (items[index].action == current) {
            currentIndex = static_cast<int>(index);
            break;
        }
    }
    for (size_t offset = 0; offset < items.size(); ++offset) {
        currentIndex = reverse
            ? (currentIndex <= 0 ? static_cast<int>(items.size()) - 1 : currentIndex - 1)
            : (currentIndex + 1) % static_cast<int>(items.size());
        const ToolbarAction candidate = items[static_cast<size_t>(currentIndex)].action;
        if (IsToolbarActionEnabled(candidate, availability)) return candidate;
    }
    return ToolbarAction::None;
}

std::vector<RECT> HoverDirtyRectangles(std::span<const ToolbarItem> items,
                                       ToolbarAction oldAction,
                                       ToolbarAction newAction)
{
    if (oldAction == newAction) return {};

    std::vector<RECT> dirty;
    dirty.reserve(2);
    const auto appendBounds = [&](ToolbarAction action) {
        if (action == ToolbarAction::None) return;
        for (const auto& item : items) {
            if (item.action == action) {
                dirty.push_back(item.bounds);
                return;
            }
        }
    };
    appendBounds(oldAction);
    appendBounds(newAction);
    return dirty;
}

ToolbarAction ResolveToolbarHover(std::span<const ToolbarItem> items,
                                  POINT point,
                                  ToolbarAvailability availability)
{
    const ToolbarAction action = HitTestToolbar(items, point);
    return IsToolbarActionEnabled(action, availability) ? action : ToolbarAction::None;
}

ToolbarAction ResolveToolbarHoverForCursor(std::span<const ToolbarItem> items,
                                           std::optional<POINT> clientPoint,
                                           ToolbarAvailability availability)
{
    return clientPoint ? ResolveToolbarHover(items, *clientPoint, availability)
                       : ToolbarAction::None;
}

std::optional<PaintBufferLayout> LayoutPaintBuffer(RECT clientBounds, RECT paintBounds)
{
    const RECT clipped{
        std::max(clientBounds.left, paintBounds.left),
        std::max(clientBounds.top, paintBounds.top),
        std::min(clientBounds.right, paintBounds.right),
        std::min(clientBounds.bottom, paintBounds.bottom),
    };
    if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) return std::nullopt;
    return PaintBufferLayout{
        clipped,
        static_cast<int>(clipped.right - clipped.left),
        static_cast<int>(clipped.bottom - clipped.top),
        POINT{-clipped.left, -clipped.top},
    };
}
