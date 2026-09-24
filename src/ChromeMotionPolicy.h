#pragma once

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

// The player chrome's motion, as numbers. DESIGN.md "Player" is the spec this
// implements: motion says where something came from or went, it is short
// (120-220 ms), it eases out when something arrives and in when it leaves,
// and it never bounces. Every animation here has a static equivalent for
// Windows' "Show animations in Windows" off (SPI_GETCLIENTAREAANIMATION),
// which the player reads as `motion`.
//
// Nothing here draws or owns a timer. main.cpp repaints only the rectangle an
// animation is in, on a timer that runs only while Animating() says so, so an
// idle chrome costs nothing and playback never pays for a hover.
namespace chrome_motion {

using Clock = std::chrono::steady_clock;

// Hover arrives quickly and leaves a little slower: the cursor landing on a
// control is the thing being answered, and a tint that snaps off as the
// cursor passes over a row of pills reads as flicker.
inline constexpr std::chrono::milliseconds kHoverIn{120};
inline constexpr std::chrono::milliseconds kHoverOut{180};
// One repaint per display frame at 60 Hz is all a GDI tint can use.
inline constexpr unsigned kFrameMs = 16;

inline double Clamp01(double value) { return std::clamp(value, 0.0, 1.0); }

// Cubic ease-out and ease-in. No overshoot anywhere: DESIGN.md bans bounce.
inline double EaseOut(double t)
{
    const double u = 1.0 - Clamp01(t);
    return 1.0 - u * u * u;
}
inline double EaseIn(double t)
{
    const double v = Clamp01(t);
    return v * v * v;
}

inline COLORREF Mix(COLORREF from, COLORREF to, double amount)
{
    const double a = Clamp01(amount);
    const auto channel = [a](int x, int y) {
        return static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(x + (y - x) * a)), 0, 255));
    };
    return RGB(channel(GetRValue(from), GetRValue(to)), channel(GetGValue(from), GetGValue(to)),
               channel(GetBValue(from), GetBValue(to)));
}

// A level between 0 and 1 that eases toward on or off. A change of mind
// halfway (the cursor leaving before the tint arrived) starts from where the
// level is, not from the end it was heading to, so it never jumps.
class Fade {
public:
    void Set(bool on, Clock::time_point now, bool motion)
    {
        if (on == on_ && started_) return;
        from_ = motion ? Level(now) : (on ? 0.0 : 1.0);
        on_ = on;
        motion_ = motion;
        started_ = now;
    }
    // Snaps to a state without animating, for a layout change that moved the
    // control out from under the cursor.
    void Reset(bool on = false)
    {
        on_ = on;
        from_ = on ? 1.0 : 0.0;
        started_.reset();
    }
    [[nodiscard]] double Level(Clock::time_point now) const
    {
        const double target = on_ ? 1.0 : 0.0;
        if (!started_ || !motion_) return target;
        const double duration = std::chrono::duration<double>(on_ ? kHoverIn : kHoverOut).count();
        const double t = std::chrono::duration<double>(now - *started_).count() / duration;
        if (t >= 1.0) return target;
        if (t <= 0.0) return from_;
        return from_ + (target - from_) * (on_ ? EaseOut(t) : EaseIn(t));
    }
    [[nodiscard]] bool Animating(Clock::time_point now) const
    {
        if (!started_ || !motion_) return false;
        return now - *started_ < (on_ ? kHoverIn : kHoverOut);
    }
    [[nodiscard]] bool On() const { return on_; }

private:
    bool on_{false};
    bool motion_{true};
    double from_{0.0};
    std::optional<Clock::time_point> started_;
};

} // namespace chrome_motion
