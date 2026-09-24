#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>

// How big the player's first window is. It used to be a fixed 1440x880 of
// client in physical pixels, which at 175% is 823x503 dip: every pill on the
// bar dropped to an icon and lost its state words ("On", "Preparing cache"),
// and the video had a third of a 4K screen. It is now a share of the monitor's
// work area, shaped for a 16:9 picture above the chrome, so the same window
// grows with the display instead of shrinking with its scaling.
namespace initial_window {

// Big enough to be the thing on screen, small enough to read as a window and
// not a maximised one, with room left to see the desktop around it.
inline constexpr double kWorkAreaShare = 0.8;
inline constexpr double kPictureAspect = 16.0 / 9.0;

struct Input {
    SIZE work{};            // the monitor's work area
    SIZE nonClient{};       // frame, caption and menu: window size minus client
    int chrome{};           // client pixels below the picture (strip and compare bar)
    SIZE minimumClient{};   // the smallest client the player lays out in
    // The client width at which the bar keeps every pill's words
    // (FullPillToolbarClientWidth). A work area whose height binds the
    // picture first still gets this width if 80% of it allows: the pills
    // stating On / Off / Preparing cache matter more than a picture without
    // side bars, and the picture keeps its aspect inside the wider client.
    int preferredClientWidth{};
};

// The client size: the largest 16:9 picture that, with the chrome under it
// and the frame around it, fits in kWorkAreaShare of the work area, widened
// to the preferred width where that share allows. Never below the minimum,
// and never past the work area itself even then.
inline SIZE ClientSize(const Input& in)
{
    const double maxClientW = in.work.cx * kWorkAreaShare - in.nonClient.cx;
    const double maxClientH = in.work.cy * kWorkAreaShare - in.nonClient.cy;
    double width = std::min(maxClientW, (maxClientH - in.chrome) * kPictureAspect);
    double height = width / kPictureAspect + in.chrome;
    width = std::max(width, std::min<double>(in.preferredClientWidth, maxClientW));
    width = std::max<double>(width, in.minimumClient.cx);
    height = std::max<double>(height, in.minimumClient.cy);
    width = std::min<double>(width, in.work.cx - in.nonClient.cx);
    height = std::min<double>(height, in.work.cy - in.nonClient.cy);
    return SIZE{std::max<LONG>(1, static_cast<LONG>(std::lround(width))),
                std::max<LONG>(1, static_cast<LONG>(std::lround(height)))};
}

} // namespace initial_window
