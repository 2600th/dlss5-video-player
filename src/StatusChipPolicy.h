#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

// The numbers a viewer watches while playing - how far the render has got,
// the frame rate and the dropped count - as three chips at the right end of
// the status row, instead of the tail of one ellipsised line.
//
// That line was measured at about 120 characters on a 1920x1080 window and
// was still cut ("...Frame Generat..." in docs/media/neural-playback.jpg); the
// frame rate and the dropped count sat after the feature states, so they were
// exactly the part that went. A chip has a fixed place and a fixed width, so a
// notice arriving in the line can no longer push them off, and a value that
// changes does not make its neighbours jump.
//
// Pure: text, layout and the flash are decided here and tested without a
// window; main.cpp only feeds numbers in and paints the result.
namespace status_chips {

enum class Chip : size_t { Render, Fps, Dropped };
inline constexpr size_t kChipCount = 3;

// Fixed widths, each the widest text its chip can print in Segoe UI at the
// 14 px the status row draws with, plus kChipPaddingDip on either side:
//   "Render 99% · ETA 99:59:59"   174 px
//   "144 / 144 fps"                92 px
//   "Dropped 99999"               107 px
// A dropped count past five digits is a broken session rather than a number
// anyone reads, and ellipsises instead of widening the chip.
inline constexpr int kRenderChipWidthDip = 196;
inline constexpr int kFpsChipWidthDip = 106;
inline constexpr int kDroppedChipWidthDip = 121;
inline constexpr int kChipPaddingDip = 7;
inline constexpr int kChipGapDip = 6;
inline constexpr int kChipHeightDip = 21;
// The status line keeps at least this much before a chip is given up: a line
// squeezed to nothing hides the notices that outrank every chip.
inline constexpr int kMinimumTextWidthDip = 160;
// Long enough to catch the eye at the edge of vision, short enough that a
// chip that changes twice in a second reads as two flashes.
inline constexpr std::chrono::milliseconds kFlashDuration{900};

inline int Dip(int value, UINT dpi)
{
    return MulDiv(value, static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi),
                  USER_DEFAULT_SCREEN_DPI);
}

// "0:42", "3:10", "1:02:03": the time-remaining form, which has no leading
// zero hour because an ETA is read, not aligned.
inline std::wstring EtaText(double seconds)
{
    const long long total = std::max<long long>(0, std::llround(seconds));
    const long long hours = total / 3600, minutes = (total / 60) % 60, rest = total % 60;
    wchar_t text[32]{};
    if (hours > 0) swprintf_s(text, L"%lld:%02lld:%02lld", hours, minutes, rest);
    else swprintf_s(text, L"%lld:%02lld", minutes, rest);
    return text;
}

// Rounded, never the raw double: "58.4" implies a precision a rate sampled
// over 0.75 s does not have.
inline std::wstring FpsText(double renderedFps, double sourceFps)
{
    return std::to_wstring(std::lround(std::max(0.0, renderedFps))) + L" / " +
           std::to_wstring(std::lround(std::max(0.0, sourceFps))) + L" fps";
}

inline std::wstring DroppedText(uint64_t dropped)
{
    return L"Dropped " + std::to_wstring(dropped);
}

inline std::wstring RenderText(double fraction, std::optional<double> etaSeconds)
{
    const long long percent = std::llround(std::clamp(fraction, 0.0, 1.0) * 100.0);
    // 99.6% is not rendered; only a render that is done may say 100.
    const long long shown = fraction < 1.0 ? std::min<long long>(percent, 99) : percent;
    std::wstring text = L"Render " + std::to_wstring(shown) + L"%";
    if (etaSeconds && *etaSeconds > 0.0 && fraction < 1.0) text += L" · ETA " + EtaText(*etaSeconds);
    return text;
}

// Seconds until the rest of a range is rendered, at the pace the render is
// actually managing (video seconds per wall-clock second). Nothing when the
// pace is not known yet: an ETA made up from nothing is worse than none.
inline std::optional<double> SecondsToFullCoverage(double remainingVideoSeconds,
                                                   double videoSecondsPerSecond)
{
    if (!(remainingVideoSeconds > 0.0)) return 0.0;
    if (!(videoSecondsPerSecond > 0.0) || !std::isfinite(videoSecondsPerSecond)) return std::nullopt;
    return remainingVideoSeconds / videoSecondsPerSecond;
}

// What a chip flashes on. Not every repaint of its text: the frame rate
// wobbles by a frame every sample and the render percentage ticks once a
// second, and a chip that flashes all the time says nothing. So each chip
// flashes on the fact it reports changing:
//   Render  - a render starting, and every tenth of the range completed;
//   Fps     - the render starting or ceasing to keep up with the source;
//   Dropped - every newly dropped frame.
// 0 is "nothing to report" and never flashes, so a source reload that resets
// the counters, or a rate not measured yet, is silent.
inline uint64_t RenderFlashKey(double fraction)
{
    return 1u + static_cast<uint64_t>(std::floor(std::clamp(fraction, 0.0, 1.0) * 10.0));
}

inline uint64_t FpsFlashKey(double renderedFps, double sourceFps)
{
    if (!(renderedFps > 0.0) || !(sourceFps > 0.0)) return 0;
    // Two percent is inside run-to-run noise; the same tolerance the live
    // forecast uses before it calls a render short.
    return renderedFps >= sourceFps * 0.98 ? 1u : 2u;
}

struct Content {
    bool visible{};
    std::wstring text;
    uint64_t flashKey{};
    // Drawn in the secondary text colour: a value that is fine, like no
    // dropped frames, should not compete with one that is not.
    bool quiet{};

    friend bool operator==(const Content&, const Content&) = default;
};

using Snapshot = std::array<Content, kChipCount>;

inline Content& At(Snapshot& snapshot, Chip chip) { return snapshot[static_cast<size_t>(chip)]; }
inline const Content& At(const Snapshot& snapshot, Chip chip) { return snapshot[static_cast<size_t>(chip)]; }

struct RenderProgress {
    bool active{};
    double fraction{};
    std::optional<double> etaSeconds;
};

inline Snapshot Build(bool mediaLoaded, RenderProgress render, double renderedFps, double sourceFps,
                      uint64_t dropped)
{
    Snapshot snapshot{};
    if (!mediaLoaded) return snapshot;
    if (render.active) {
        At(snapshot, Chip::Render) = Content{true, RenderText(render.fraction, render.etaSeconds),
                                             RenderFlashKey(render.fraction), false};
    }
    // A still image has no rate; the chip would only ever say "0 / 0 fps".
    if (sourceFps > 0.0) {
        At(snapshot, Chip::Fps) = Content{true, FpsText(renderedFps, sourceFps),
                                          FpsFlashKey(renderedFps, sourceFps), false};
        At(snapshot, Chip::Dropped) = Content{true, DroppedText(dropped), dropped, dropped == 0};
    }
    return snapshot;
}

inline int WidthDip(Chip chip)
{
    switch (chip) {
    case Chip::Render: return kRenderChipWidthDip;
    case Chip::Fps: return kFpsChipWidthDip;
    case Chip::Dropped: return kDroppedChipWidthDip;
    }
    return kFpsChipWidthDip;
}

struct RowLayout {
    RECT text{};
    // Empty for a chip that is not shown.
    std::array<RECT, kChipCount> chips{};
};

// Chips sit at the right end of `row` in a fixed order, Render then Fps then
// Dropped, so each keeps its place whatever the others do; a chip that is not
// shown gives its room to the line. When the row is too narrow for every chip
// and the line's minimum, chips go from the left - the render chip first,
// because the line still names the render - never from the middle.
inline RowLayout LayoutRow(RECT row, UINT dpi, const std::array<bool, kChipCount>& visible)
{
    RowLayout layout{};
    layout.text = row;
    const int height = std::min<int>(Dip(kChipHeightDip, dpi), std::max<LONG>(0, row.bottom - row.top));
    const LONG top = row.top + (row.bottom - row.top - height) / 2;
    const LONG floor = row.left + Dip(kMinimumTextWidthDip, dpi);
    LONG right = row.right;
    bool placed = false;
    for (size_t index = kChipCount; index-- > 0;) {
        if (!visible[index]) continue;
        const int width = Dip(WidthDip(static_cast<Chip>(index)), dpi);
        const LONG left = right - width;
        if (left - Dip(kChipGapDip, dpi) < floor) break;
        layout.chips[index] = RECT{left, top, right, top + height};
        right = left - Dip(kChipGapDip, dpi);
        placed = true;
    }
    if (placed) layout.text.right = std::max(row.left, right);
    return layout;
}

// When each chip's reported fact last changed, and how much flash is left.
class Flash {
public:
    using Clock = std::chrono::steady_clock;

    // Records the snapshot; a chip whose flash key moved to a non-zero value
    // starts a flash. Returns whether any did.
    bool Observe(const Snapshot& snapshot, Clock::time_point now)
    {
        bool started = false;
        for (size_t index = 0; index < kChipCount; ++index) {
            const uint64_t key = snapshot[index].visible ? snapshot[index].flashKey : 0;
            if (key != keys_[index] && key != 0) {
                changed_[index] = now;
                started = true;
            }
            keys_[index] = key;
        }
        return started;
    }

    // 1 at the change, falling to 0 over kFlashDuration. Without motion it
    // holds at full for the duration and then stops: a highlight that says the
    // same thing without animating.
    double Level(Chip chip, Clock::time_point now, bool motion = true) const
    {
        const auto changed = changed_[static_cast<size_t>(chip)];
        if (!changed) return 0.0;
        const double elapsed = std::chrono::duration<double>(now - *changed).count();
        const double duration = std::chrono::duration<double>(kFlashDuration).count();
        if (elapsed < 0.0 || elapsed >= duration) return 0.0;
        return motion ? 1.0 - elapsed / duration : 1.0;
    }

    bool Animating(Clock::time_point now) const
    {
        for (size_t index = 0; index < kChipCount; ++index)
            if (Level(static_cast<Chip>(index), now) > 0.0) return true;
        return false;
    }

private:
    std::array<uint64_t, kChipCount> keys_{};
    std::array<std::optional<Clock::time_point>, kChipCount> changed_{};
};

} // namespace status_chips
