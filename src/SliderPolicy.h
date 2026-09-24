#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>

// One slider for the whole player: the volume on the strip, the Mix on the
// compare bar and every trackbar in the settings dialogs are drawn from this
// geometry, so a slider looks and answers the same wherever it is. A thin
// track, the part up to the value filled, a round knob that grows while the
// pointer is on it or dragging it, and - where there is room that is not the
// picture - a value bubble over the knob while it is dragged.
namespace slider {

inline constexpr int kTrackDip = 4;
inline constexpr int kKnobRestDip = 6;
inline constexpr int kKnobHotDip = 8;
inline constexpr int kBubbleWidthDip = 46;
inline constexpr int kBubbleHeightDip = 20;
inline constexpr int kBubbleGapDip = 3;
// The bubble stays up this long after release, so the value can be read
// once the hand has let go; then it fades out.
inline constexpr unsigned kBubbleLingerMs = 400;

inline int Dip(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96); }

struct Geometry {
    RECT track{};     // the whole rail
    RECT fill{};      // from the origin to the value
    POINT knob{};     // the knob's centre
    int knobRadius{};
    RECT bubble{};    // above the knob, the same width wherever it goes
};

// `area` is the slider's hit rectangle; the rail runs its full width on its
// vertical centre, with the knob's rest radius kept inside at both ends so a
// knob at 0 or 1 is not cut in half. `origin` is where the fill starts (0 for
// a level, 0.5 for a Mix whose middle is "the render untouched"). `hot` is
// 0 at rest and 1 hovered or dragged. `bubbleBounds` keeps the bubble inside
// what it may cover.
inline Geometry Layout(RECT area, double value, double origin, double hot, UINT dpi,
                       RECT bubbleBounds = RECT{LONG_MIN / 2, LONG_MIN / 2, LONG_MAX / 2, LONG_MAX / 2})
{
    Geometry g;
    const int inset = Dip(kKnobRestDip, dpi);
    const LONG left = area.left + inset, right = std::max<LONG>(left + 1, area.right - inset);
    const LONG mid = (area.top + area.bottom) / 2;
    const int thick = std::max(2, Dip(kTrackDip, dpi));
    g.track = RECT{left, mid - thick / 2, right, mid - thick / 2 + thick};
    const auto xAt = [&](double fraction) {
        return left + static_cast<LONG>(std::lround(std::clamp(fraction, 0.0, 1.0) * double(right - left)));
    };
    const LONG x = xAt(value), o = xAt(origin);
    g.fill = RECT{std::min(x, o), g.track.top, std::max(x, o), g.track.bottom};
    g.knob = POINT{x, mid};
    const double h = std::clamp(hot, 0.0, 1.0);
    g.knobRadius = static_cast<int>(std::lround(Dip(kKnobRestDip, dpi) + (Dip(kKnobHotDip, dpi) - Dip(kKnobRestDip, dpi)) * h));
    const int bw = Dip(kBubbleWidthDip, dpi), bh = Dip(kBubbleHeightDip, dpi);
    LONG bl = x - bw / 2;
    bl = std::clamp<LONG>(bl, bubbleBounds.left, std::max<LONG>(bubbleBounds.left, bubbleBounds.right - bw));
    LONG bb = mid - Dip(kKnobHotDip, dpi) - Dip(kBubbleGapDip, dpi);
    bb = std::max<LONG>(bb, bubbleBounds.top + bh);
    g.bubble = RECT{bl, bb - bh, bl + bw, bb};
    return g;
}

// The value under a pointer at `x`, on the same rail Layout draws.
inline double ValueFromX(RECT area, int x, UINT dpi)
{
    const int inset = Dip(kKnobRestDip, dpi);
    const LONG left = area.left + inset, right = std::max<LONG>(left + 1, area.right - inset);
    return std::clamp(double(x - left) / double(right - left), 0.0, 1.0);
}

} // namespace slider
