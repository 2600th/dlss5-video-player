#pragma once

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <utility>

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
// The "render complete" glow along the timeline's coverage lane: a sweep
// across what was rendered, then a settle. Longer than a hover because it
// announces something that took minutes, and it happens once per render.
inline constexpr std::chrono::milliseconds kCompleteGlow{900};
// The compare bar's orange mark moving from the old mode to the new one.
inline constexpr std::chrono::milliseconds kMarkSlide{160};
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

// A horizontal span moving from one place to another: the compare bar's mark
// sliding from the mode it left to the mode it chose, so the eye follows the
// change instead of finding the mark again. Eases out; without motion there
// is nothing to follow and it is simply at the end.
class Slide {
public:
    void Start(LONG fromLeft, LONG fromRight, LONG toLeft, LONG toRight, Clock::time_point now, bool motion)
    {
        from_ = {fromLeft, fromRight};
        to_ = {toLeft, toRight};
        started_ = motion ? std::optional<Clock::time_point>(now) : std::nullopt;
    }
    [[nodiscard]] bool Animating(Clock::time_point now) const
    {
        return started_ && now - *started_ < kMarkSlide;
    }
    // The span now: left, right.
    [[nodiscard]] std::pair<LONG, LONG> At(Clock::time_point now) const
    {
        if (!Animating(now)) return to_;
        const double t = EaseOut(std::chrono::duration<double>(now - *started_).count() /
                                 std::chrono::duration<double>(kMarkSlide).count());
        const auto lerp = [t](LONG a, LONG b) { return a + static_cast<LONG>(std::lround((b - a) * t)); };
        return {lerp(from_.first, to_.first), lerp(from_.second, to_.second)};
    }

private:
    std::pair<LONG, LONG> from_{}, to_{};
    std::optional<Clock::time_point> started_;
};

// A toast: a short confirmation that rises from the strip, holds, and goes.
// In 160 ms (ease-out, rising 8 dip), held 2.4 s, out 140 ms (ease-in). A new
// toast replaces the one on screen in place: it restarts the hold without
// rising again, so a quick run of confirmations does not bounce. Without
// motion it is simply there for the hold and then gone.
inline constexpr std::chrono::milliseconds kToastIn{160};
inline constexpr std::chrono::milliseconds kToastHold{2400};
inline constexpr std::chrono::milliseconds kToastOut{140};
inline constexpr int kToastRiseDip = 8;

class Toast {
public:
    struct Frame {
        bool visible{};
        double alpha{};   // 0..1
        double rise{};    // 0 = settled, 1 = kToastRiseDip below where it settles
    };
    void Show(Clock::time_point now)
    {
        // Already up: keep it where it is and hold again from now.
        const Frame current = At(now);
        shownAt_ = current.visible && current.alpha >= 1.0 ? now - kToastIn : now;
    }
    void Hide() { shownAt_.reset(); }
    [[nodiscard]] Frame At(Clock::time_point now, bool motion = true) const
    {
        if (!shownAt_) return {};
        const auto t = now - *shownAt_;
        if (t < std::chrono::steady_clock::duration::zero()) return {};
        if (t >= kToastIn + kToastHold + kToastOut) return {};
        if (!motion) return t < kToastIn + kToastHold ? Frame{true, 1.0, 0.0} : Frame{};
        const auto seconds = [](auto d) { return std::chrono::duration<double>(d).count(); };
        if (t < kToastIn) {
            const double e = EaseOut(seconds(t) / seconds(kToastIn));
            return {true, e, 1.0 - e};
        }
        if (t < kToastIn + kToastHold) return {true, 1.0, 0.0};
        return {true, 1.0 - EaseIn(seconds(t - kToastIn - kToastHold) / seconds(kToastOut)), 0.0};
    }
    // When the next change is due: soon while it moves, at the end of the hold
    // while it sits still, nothing once it is gone.
    [[nodiscard]] std::optional<std::chrono::milliseconds> NextChange(Clock::time_point now, bool motion = true) const
    {
        if (!shownAt_) return std::nullopt;
        const auto t = std::chrono::duration_cast<std::chrono::milliseconds>(now - *shownAt_);
        const auto end = kToastIn + kToastHold + kToastOut;
        if (t >= end) return std::nullopt;
        if (!motion) {
            if (t < kToastIn + kToastHold) return kToastIn + kToastHold - t;
            return std::nullopt;
        }
        if (t >= kToastIn && t < kToastIn + kToastHold) return kToastIn + kToastHold - t;
        return std::chrono::milliseconds(kFrameMs);
    }

private:
    std::optional<Clock::time_point> shownAt_;
};

// The once-per-render glow. `Sweep` is where the highlight has got to across
// the rendered span (0 = its left edge, 1 = past its right edge); `Level` is
// how lit the whole lane is. The lane lights with the sweep and settles after
// it. Without motion there is no sweep: the lane holds lit for the same time
// and then goes out, the static equivalent of the same announcement.
class Glow {
public:
    void Start(Clock::time_point now) { started_ = now; }
    void Cancel() { started_.reset(); }

    [[nodiscard]] double Level(Clock::time_point now, bool motion) const
    {
        const auto t = Progress(now);
        if (!t) return 0.0;
        if (!motion) return 1.0;
        // Rise over the first fifth, hold while the sweep crosses, then ease out.
        if (*t < 0.2) return EaseOut(*t / 0.2);
        if (*t < 0.6) return 1.0;
        return 1.0 - EaseIn((*t - 0.6) / 0.4);
    }
    [[nodiscard]] std::optional<double> Sweep(Clock::time_point now, bool motion) const
    {
        const auto t = Progress(now);
        if (!t || !motion || *t >= 0.7) return std::nullopt;
        return EaseOut(*t / 0.7);
    }
    [[nodiscard]] bool Animating(Clock::time_point now) const { return Progress(now).has_value(); }

private:
    [[nodiscard]] std::optional<double> Progress(Clock::time_point now) const
    {
        if (!started_) return std::nullopt;
        const double t = std::chrono::duration<double>(now - *started_).count() /
                         std::chrono::duration<double>(kCompleteGlow).count();
        if (t < 0.0 || t >= 1.0) return std::nullopt;
        return t;
    }
    std::optional<Clock::time_point> started_;
};

// Whether a live session just finished, as opposed to having been finished
// when it was opened. A source whose render is already whole attaches a
// session with no holes; glowing then would announce a render nobody watched
// happen. So only a session seen with holes earns the moment, once.
class CompletionLatch {
public:
    // Returns true exactly once per session: on the first observation that it
    // is finished after one where it was not.
    bool Observe(bool sessionActive, bool finished)
    {
        if (!sessionActive) {
            sawHoles_ = false;
            return false;
        }
        if (!finished) {
            sawHoles_ = true;
            return false;
        }
        const bool fire = sawHoles_;
        sawHoles_ = false;
        return fire;
    }

private:
    bool sawHoles_{false};
};

} // namespace chrome_motion
