#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// The compare bar: one row at the top of the control strip that holds what used to sit
// behind Video > Compare - the mode, the Mix, the zoom and the swap - while a
// comparison is possible. Layout and hit testing only; main.cpp paints it and routes
// the clicks. Measured in DIPs and scaled here, like LayoutToolbar.
namespace compare_bar {

inline constexpr int kBarHeightDip = 44;
inline constexpr int kItemHeightDip = 32;
inline constexpr int kGutterDip = 16;
inline constexpr int kGroupGapDip = 16;
// "Side by side" is the widest mode label, 68 dip in Segoe UI 14 plus two 8 dip insets.
inline constexpr int kModeWidthDip = 84;
inline constexpr int kCompactModeWidthDip = 64;
inline constexpr int kMixLabelDip = 30;
inline constexpr int kMixTrackDip = 140;
inline constexpr int kCompactMixTrackDip = 88;
inline constexpr int kMixValueDip = 46;
inline constexpr int kZoomButtonDip = 32;
inline constexpr int kZoomValueDip = 44;
inline constexpr int kToggleDip = 64;

enum class Part { None, Mode, MixTrack, ZoomOut, ZoomIn, Swap, Loupe };

struct Item {
    Part part = Part::None;
    int index = 0;  // the mode's position for Part::Mode
    RECT bounds{};
};

struct Layout {
    RECT bar{};
    std::vector<Item> items;
    RECT mixLabel{};
    RECT mixTrack{};
    RECT mixValue{};
    RECT zoomValue{};
    RECT hint{};  // empty when there is no room left for it
    bool compact = false;
};

inline int Scale(int dip, UINT dpi)
{
    return MulDiv(dip, static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi), USER_DEFAULT_SCREEN_DPI);
}

// `top` is the bar's top edge in client pixels. The loupe toggle is optional so a build
// without it lays out the same bar it always did.
inline Layout LayoutBar(int clientWidth, int top, UINT dpi, size_t modeCount, bool loupe)
{
    Layout layout;
    const int height = Scale(kBarHeightDip, dpi);
    layout.bar = RECT{0, top, std::max(0, clientWidth), top + height};
    const int itemHeight = Scale(kItemHeightDip, dpi);
    const int itemTop = top + (height - itemHeight) / 2;
    const int gutter = Scale(kGutterDip, dpi), gap = Scale(kGroupGapDip, dpi);
    const auto fixedWidth = [&](int modeDip, int trackDip) {
        return int(modeCount) * Scale(modeDip, dpi) + gap +
               Scale(kMixLabelDip, dpi) + Scale(trackDip, dpi) + Scale(kMixValueDip, dpi) + gap +
               2 * Scale(kZoomButtonDip, dpi) + Scale(kZoomValueDip, dpi) + gap +
               Scale(kToggleDip, dpi) + (loupe ? Scale(kToggleDip, dpi) : 0);
    };
    const int available = std::max(0, clientWidth - 2 * gutter);
    layout.compact = fixedWidth(kModeWidthDip, kMixTrackDip) > available;
    const int modeWidth = Scale(layout.compact ? kCompactModeWidthDip : kModeWidthDip, dpi);
    const int trackWidth = Scale(layout.compact ? kCompactMixTrackDip : kMixTrackDip, dpi);
    int x = gutter;
    const auto take = [&](int width) {
        const RECT r{x, itemTop, x + width, itemTop + itemHeight};
        x += width;
        return r;
    };
    for (size_t index = 0; index < modeCount; ++index)
        layout.items.push_back(Item{Part::Mode, int(index), take(modeWidth)});
    x += gap;
    layout.mixLabel = take(Scale(kMixLabelDip, dpi));
    layout.mixTrack = take(trackWidth);
    layout.items.push_back(Item{Part::MixTrack, 0, layout.mixTrack});
    layout.mixValue = take(Scale(kMixValueDip, dpi));
    x += gap;
    layout.items.push_back(Item{Part::ZoomOut, 0, take(Scale(kZoomButtonDip, dpi))});
    layout.zoomValue = take(Scale(kZoomValueDip, dpi));
    layout.items.push_back(Item{Part::ZoomIn, 0, take(Scale(kZoomButtonDip, dpi))});
    x += gap;
    layout.items.push_back(Item{Part::Swap, 0, take(Scale(kToggleDip, dpi))});
    if (loupe) layout.items.push_back(Item{Part::Loupe, 0, take(Scale(kToggleDip, dpi))});
    // Whatever is left says how to peek, and is simply dropped when it would not fit.
    const int hintLeft = x + gap, hintRight = clientWidth - gutter;
    if (hintRight - hintLeft >= Scale(120, dpi)) layout.hint = RECT{hintLeft, itemTop, hintRight, itemTop + itemHeight};
    return layout;
}

inline const Item* HitTest(const Layout& layout, POINT point)
{
    for (const Item& item : layout.items) {
        // The track takes the whole bar height, so a thin slider is not a thin target.
        RECT bounds = item.bounds;
        if (item.part == Part::MixTrack) { bounds.top = layout.bar.top; bounds.bottom = layout.bar.bottom; }
        if (point.x >= bounds.left && point.x < bounds.right && point.y >= bounds.top && point.y < bounds.bottom)
            return &item;
    }
    return nullptr;
}

// The Mix track spans 0..200%, with 100% - the neural frame untouched - at its centre.
// A click lands on the nearest 5%, and within 3% of 100 it snaps there, so the one
// value that means "what the model produced" is easy to hit and to come back to.
inline float MixFromX(RECT track, int x)
{
    const int width = std::max<LONG>(1, track.right - track.left);
    const float fraction = std::clamp(float(x - track.left) / float(width), 0.0f, 1.0f);
    float mix = std::round(fraction * 2.0f * 20.0f) / 20.0f;
    if (std::abs(fraction * 2.0f - 1.0f) <= 0.03f) mix = 1.0f;
    return std::clamp(mix, 0.0f, 2.0f);
}

inline int XFromMix(RECT track, float mix)
{
    const int width = std::max<LONG>(0, track.right - track.left);
    return track.left + int(std::lround(double(width) * std::clamp(mix, 0.0f, 2.0f) / 2.0));
}

} // namespace compare_bar
