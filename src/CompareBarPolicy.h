#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

// The compare bar: one row at the top of the control strip that holds what used to sit
// behind Video > Compare - the mode, the Mix, the zoom and the swap - while a
// comparison is possible. Layout and hit testing only; main.cpp paints it and routes
// the clicks. Measured in DIPs and scaled here, like LayoutToolbar.
//
// The labels are measured, not assumed. The first version gave every mode a fixed
// 84 dip on the belief that "Side by side" is 68 dip wide in Segoe UI 14; it is 78, so
// at the default window (1440 px client at 175%) "Differen..." and "Side by ..." were
// clipped and Swap ran off the right edge. main.cpp measures each label with the font
// it paints with, and the bar steps down through fixed arrangements until one fits:
// full labels, short labels with icon toggles, a mode menu, and a tight row with no
// Mix caption. The last one fits the player's minimum window at every DPI.
namespace compare_bar {

inline constexpr int kBarHeightDip = 44;
inline constexpr int kItemHeightDip = 32;
inline constexpr int kGutterDip = 16;
inline constexpr int kGroupGapDip = 16;
inline constexpr int kTightGapDip = 8;
inline constexpr int kLabelInsetDip = 10;     // either side of a segment's text
inline constexpr int kMinSegmentDip = 40;     // a segment is never a thinner target than this
inline constexpr int kIconLabelGapDip = 6;
inline constexpr int kChevronDip = 16;        // the mode menu's drop-down mark
inline constexpr int kMixTrackDip = 140;
inline constexpr int kCompactMixTrackDip = 88;
inline constexpr int kTightMixTrackDip = 56;
inline constexpr int kMixValueDip = 46;
inline constexpr int kTightMixValueDip = 40;
inline constexpr int kZoomButtonDip = 32;
inline constexpr int kZoomValueDip = 44;
inline constexpr int kIconToggleDip = 36;
inline constexpr int kHintMinDip = 120;

enum class Part { None, Mode, ModeMenu, MixTrack, ZoomOut, ZoomIn, Swap, Loupe };

// How a segment shows its content; main.cpp draws it this way.
enum class Face { Label, ShortLabel, Icon, IconLabel };

// The arrangements, widest first. Short swaps the mode names for their short forms
// and the Swap and Loupe captions for icons; Menu folds the modes into one button
// that opens a menu; Tight also drops the Mix caption and narrows the track.
enum class Tier { Full, Short, Menu, Tight };

struct Item {
    Part part = Part::None;
    int index = 0;  // the mode's position for Part::Mode
    RECT bounds{};
    Face face = Face::Label;
};

// Pixel widths of the text the bar can show, measured by the caller in the font the
// bar paints with. `icon` is the width of one Tabler glyph, 0 when the icon font is
// unavailable: the toggles then keep their words at every tier.
struct Metrics {
    std::vector<int> modeLabels;       // one per mode, in bar order
    std::vector<int> modeShortLabels;  // the same modes' short names
    int swapLabel = 0;
    int loupeLabel = 0;
    int mixLabel = 0;
    int icon = 0;
};

struct Layout {
    RECT bar{};
    std::vector<Item> items;
    RECT mixLabel{};  // empty in the Tight tier
    RECT mixTrack{};
    RECT mixValue{};
    RECT zoomValue{};
    RECT hint{};  // empty when there is no room left for it
    Tier tier = Tier::Full;
    bool compact = false;  // anything but Full
    bool fits = true;      // false only when even Tight overflowed and items were dropped
};

inline int Scale(int dip, UINT dpi)
{
    return MulDiv(dip, static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi), USER_DEFAULT_SCREEN_DPI);
}

// Widths from a per-character estimate, for callers with no DC: 7 dip a character
// is Segoe UI 14's average over these labels, rounded up.
inline Metrics EstimatedMetrics(size_t modeCount, UINT dpi)
{
    static constexpr std::array full{6, 8, 5, 4, 10, 12, 5}, brief{6, 4, 5, 4, 4, 4, 5};
    Metrics metrics;
    for (size_t index = 0; index < modeCount; ++index) {
        metrics.modeLabels.push_back(Scale(7 * full[index % full.size()], dpi));
        metrics.modeShortLabels.push_back(Scale(7 * brief[index % brief.size()], dpi));
    }
    metrics.swapLabel = Scale(28, dpi);
    metrics.loupeLabel = Scale(35, dpi);
    metrics.mixLabel = Scale(21, dpi);
    metrics.icon = Scale(17, dpi);
    return metrics;
}

namespace detail {

inline int LabelAt(const std::vector<int>& widths, size_t index)
{
    return index < widths.size() ? std::max(0, widths[index]) : 0;
}

inline int Widest(const std::vector<int>& widths)
{
    int widest = 0;
    for (const int width : widths) widest = std::max(widest, width);
    return widest;
}

struct Plan {
    std::vector<int> modes;  // segment widths, or one entry for the menu button
    Face modeFace = Face::Label;
    int mixLabel = 0, track = 0, mixValue = 0;
    Face toggleFace = Face::Label;
    int swap = 0, loupe = 0;
    int gap = 0;
};

inline Plan PlanFor(Tier tier, const Metrics& metrics, size_t modeCount, UINT dpi)
{
    const auto D = [dpi](int dip) { return Scale(dip, dpi); };
    const int inset = D(kLabelInsetDip), minimum = D(kMinSegmentDip);
    Plan plan;
    plan.gap = D(tier == Tier::Tight ? kTightGapDip : kGroupGapDip);
    if (tier == Tier::Full || tier == Tier::Short) {
        plan.modeFace = tier == Tier::Full ? Face::Label : Face::ShortLabel;
        const auto& labels = tier == Tier::Full ? metrics.modeLabels : metrics.modeShortLabels;
        for (size_t index = 0; index < modeCount; ++index)
            plan.modes.push_back(std::max(minimum, LabelAt(labels, index) + 2 * inset));
    } else if (modeCount > 0) {
        // One button, as wide as the widest name it can show plus the chevron, so it
        // does not change width as the mode changes under it.
        plan.modeFace = tier == Tier::Menu ? Face::Label : Face::ShortLabel;
        const int widest = Widest(tier == Tier::Menu ? metrics.modeLabels : metrics.modeShortLabels);
        plan.modes.push_back(std::max(minimum, widest + 2 * inset + D(kIconLabelGapDip) + D(kChevronDip)));
    }
    plan.mixLabel = tier == Tier::Tight ? 0 : metrics.mixLabel + D(kIconLabelGapDip) + D(4);
    plan.track = D(tier == Tier::Full ? kMixTrackDip : tier == Tier::Tight ? kTightMixTrackDip : kCompactMixTrackDip);
    plan.mixValue = D(tier == Tier::Tight ? kTightMixValueDip : kMixValueDip);
    const bool icons = metrics.icon > 0;
    if (tier == Tier::Full) {
        plan.toggleFace = icons ? Face::IconLabel : Face::Label;
        const int extra = icons ? metrics.icon + D(kIconLabelGapDip) : 0;
        plan.swap = std::max(minimum, metrics.swapLabel + extra + 2 * inset);
        plan.loupe = std::max(minimum, metrics.loupeLabel + extra + 2 * inset);
    } else if (icons) {
        plan.toggleFace = Face::Icon;
        plan.swap = plan.loupe = std::max(D(kIconToggleDip), metrics.icon + 2 * D(8));
    } else {
        plan.swap = std::max(minimum, metrics.swapLabel + 2 * inset);
        plan.loupe = std::max(minimum, metrics.loupeLabel + 2 * inset);
    }
    return plan;
}

inline int Width(const Plan& plan, bool loupe, UINT dpi)
{
    int width = 0;
    for (const int mode : plan.modes) width += mode;
    width += plan.gap + plan.mixLabel + plan.track + plan.mixValue;
    width += plan.gap + 2 * Scale(kZoomButtonDip, dpi) + Scale(kZoomValueDip, dpi);
    width += plan.gap + plan.swap + (loupe ? plan.loupe : 0);
    return width;
}

} // namespace detail

// The tier a bar of this width gets: the first, widest first, whose row fits.
inline Tier ChooseTier(int clientWidth, UINT dpi, const Metrics& metrics, size_t modeCount, bool loupe)
{
    const int available = std::max(0, clientWidth - 2 * Scale(kGutterDip, dpi));
    for (const Tier tier : {Tier::Full, Tier::Short, Tier::Menu})
        if (detail::Width(detail::PlanFor(tier, metrics, modeCount, dpi), loupe, dpi) <= available) return tier;
    return Tier::Tight;
}

// `top` is the bar's top edge in client pixels. The loupe toggle is optional so a build
// without it lays out the same bar it always did.
inline Layout LayoutBar(int clientWidth, int top, UINT dpi, const Metrics& metrics, bool loupe)
{
    Layout layout;
    const size_t modeCount = metrics.modeLabels.size();
    const int height = Scale(kBarHeightDip, dpi);
    layout.bar = RECT{0, top, std::max(0, clientWidth), top + height};
    const int itemHeight = Scale(kItemHeightDip, dpi);
    const int itemTop = top + (height - itemHeight) / 2;
    const int gutter = Scale(kGutterDip, dpi);
    layout.tier = ChooseTier(clientWidth, dpi, metrics, modeCount, loupe);
    layout.compact = layout.tier != Tier::Full;
    const detail::Plan plan = detail::PlanFor(layout.tier, metrics, modeCount, dpi);
    const int right = std::max(gutter, clientWidth - gutter);
    int x = gutter;
    const auto take = [&](int width) {
        const RECT r{x, itemTop, x + width, itemTop + itemHeight};
        x += width;
        return r;
    };
    // Past the right edge an item is dropped rather than drawn half off the window;
    // only a window narrower than the player allows gets here, and every one of these
    // is also on the keyboard and in Video > Compare.
    const auto push = [&](Part part, int index, RECT bounds, Face face) {
        if (bounds.right > right) { layout.fits = false; return; }
        layout.items.push_back(Item{part, index, bounds, face});
    };
    if (layout.tier == Tier::Full || layout.tier == Tier::Short) {
        for (size_t index = 0; index < plan.modes.size(); ++index) push(Part::Mode, int(index), take(plan.modes[index]), plan.modeFace);
    } else if (!plan.modes.empty()) {
        push(Part::ModeMenu, 0, take(plan.modes.front()), plan.modeFace);
    }
    x += plan.gap;
    if (plan.mixLabel > 0) layout.mixLabel = take(plan.mixLabel);
    layout.mixTrack = take(plan.track);
    push(Part::MixTrack, 0, layout.mixTrack, Face::Label);
    layout.mixValue = take(plan.mixValue);
    if (layout.mixValue.right > right) layout.mixValue = RECT{};
    x += plan.gap;
    push(Part::ZoomOut, 0, take(Scale(kZoomButtonDip, dpi)), Face::Label);
    layout.zoomValue = take(Scale(kZoomValueDip, dpi));
    if (layout.zoomValue.right > right) layout.zoomValue = RECT{};
    push(Part::ZoomIn, 0, take(Scale(kZoomButtonDip, dpi)), Face::Label);
    x += plan.gap;
    push(Part::Swap, 0, take(plan.swap), plan.toggleFace);
    if (loupe) push(Part::Loupe, 0, take(plan.loupe), plan.toggleFace);
    // Whatever is left says how to peek, and is simply dropped when it would not fit.
    const int hintLeft = x + plan.gap, hintRight = clientWidth - gutter;
    if (hintRight - hintLeft >= Scale(kHintMinDip, dpi)) layout.hint = RECT{hintLeft, itemTop, hintRight, itemTop + itemHeight};
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
