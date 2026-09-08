#include "RangeSelection.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

constexpr double kTicksPerSecond = 10000000.0;

bool ValidFps(double fps) noexcept { return std::isfinite(fps) && fps > 0.0; }

// Non-drop timecode counts ceil(fps) frames per labelled second.
uint32_t NominalFrameRate(double fps) noexcept
{
    const double nominal = std::ceil(fps - 1e-6);
    if (!(nominal >= 1.0)) return 1;
    if (nominal > 100000.0) return 100000;
    return static_cast<uint32_t>(nominal);
}

// Largest frame index whose timestamp is <= pts.
uint64_t FrameIndexAtOrBefore(int64_t pts100ns, double fps)
{
    if (pts100ns <= 0) return 0;
    // FramePts saturates at INT64_MAX; keep the search bound strictly below it.
    pts100ns = std::min(pts100ns, std::numeric_limits<int64_t>::max() - 1);
    uint64_t index = FrameIndexNearest(pts100ns, fps);
    while (index > 0 && FramePts(index, fps) > pts100ns) --index;
    while (FramePts(index + 1, fps) <= pts100ns) ++index;
    return index;
}

// Smallest frame index whose timestamp is >= pts.
uint64_t FrameIndexAtOrAfter(int64_t pts100ns, double fps)
{
    if (pts100ns <= 0) return 0;
    uint64_t index = FrameIndexNearest(pts100ns, fps);
    while (index > 0 && FramePts(index - 1, fps) >= pts100ns) --index;
    while (FramePts(index, fps) < pts100ns) ++index;
    return index;
}

// Number of frames starting before the source ends; FramePts(count) is the
// first grid boundary at or beyond the container duration.
uint64_t SourceFrameCount(int64_t duration100ns, double fps)
{
    return FrameIndexAtOrBefore(duration100ns - 1, fps) + 1;
}

NeuralRenderRange GridRange(uint64_t startIndex, uint64_t endIndex, uint64_t frameCount, double fps)
{
    if (startIndex == 0 && endIndex >= frameCount) return {};
    return {FramePts(startIndex, fps), FramePts(endIndex, fps)};
}

bool ParseDigits(std::wstring_view text, size_t minDigits, size_t maxDigits, uint64_t& out)
{
    if (text.size() < minDigits || text.size() > maxDigits) return false;
    uint64_t value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') return false;
        value = value * 10 + static_cast<uint64_t>(character - L'0');
    }
    out = value;
    return true;
}

// "ss" or "ss.fffffff" -> 100ns ticks. integerDigits bounds the whole-second field.
bool ParseSecondsField(std::wstring_view text, size_t integerDigits, bool boundedByMinute, int64_t& out100ns)
{
    const size_t point = text.find(L'.');
    uint64_t seconds = 0;
    if (!ParseDigits(text.substr(0, point), 1, integerDigits, seconds)) return false;
    if (boundedByMinute && seconds >= 60) return false;
    uint64_t fraction = 0;
    if (point != std::wstring_view::npos) {
        const auto digits = text.substr(point + 1);
        if (!ParseDigits(digits, 1, 7, fraction)) return false;
        for (size_t scale = digits.size(); scale < 7; ++scale) fraction *= 10;
    }
    out100ns = static_cast<int64_t>(seconds * 10000000ull + fraction);
    return true;
}

} // namespace

int64_t FramePts(uint64_t index, double fps)
{
    if (!ValidFps(fps)) return 0;
    const double ticks = (static_cast<double>(index) / fps) * kTicksPerSecond;
    if (!(ticks < 9.2e18)) return std::numeric_limits<int64_t>::max();
    return static_cast<int64_t>(ticks);
}

uint64_t FrameIndexNearest(int64_t pts100ns, double fps)
{
    if (!ValidFps(fps) || pts100ns <= 0) return 0;
    const double index = std::round(static_cast<double>(pts100ns) * fps / kTicksPerSecond);
    if (!(index < 1.8e19)) return std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(index);
}

std::optional<NeuralRenderRange> RangeFromMarkers(const RangeMarkers& markers, int64_t sourceDuration100ns)
{
    if (!markers.in100ns || !markers.out100ns || sourceDuration100ns <= 0) return std::nullopt;
    const int64_t start = *markers.in100ns;
    const int64_t end = std::min(*markers.out100ns, sourceDuration100ns);
    if (start < 0 || start >= end) return std::nullopt;
    if (start == 0 && end == sourceDuration100ns) return NeuralRenderRange{};
    return NeuralRenderRange{start, end};
}

NeuralRenderRange SingleFrameRange(int64_t at100ns, double fps, int64_t sourceDuration100ns)
{
    if (!ValidFps(fps) || sourceDuration100ns <= 0) return {};
    const uint64_t frameCount = SourceFrameCount(sourceDuration100ns, fps);
    const uint64_t start = std::min(FrameIndexAtOrBefore(at100ns, fps), frameCount - 1);
    return GridRange(start, start + 1, frameCount, fps);
}

NeuralRenderRange ClipPreviewRange(int64_t at100ns, double fps, int64_t sourceDuration100ns, double seconds)
{
    if (!ValidFps(fps) || sourceDuration100ns <= 0) return {};
    const uint64_t frameCount = SourceFrameCount(sourceDuration100ns, fps);
    const uint64_t start = std::min(FrameIndexAtOrBefore(at100ns, fps), frameCount - 1);
    uint64_t frames = 1;
    if (std::isfinite(seconds) && seconds > 0.0) {
        const double requested = std::round(seconds * fps);
        frames = requested < 1.0 ? 1 : requested > 1e15 ? uint64_t{1000000000000000} : static_cast<uint64_t>(requested);
    }
    return GridRange(start, std::min(start + frames, frameCount), frameCount, fps);
}

std::optional<int64_t> ParseTimecode(std::wstring_view text, double fps)
{
    if (!ValidFps(fps)) return std::nullopt;
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.remove_suffix(1);
    if (text.empty()) return std::nullopt;

    if (text.front() == L'f' || text.front() == L'F') {
        uint64_t frames = 0;
        if (!ParseDigits(text.substr(1), 1, 12, frames)) return std::nullopt;
        return FramePts(frames, fps);
    }

    std::wstring_view fields[4];
    size_t count = 0;
    for (size_t position = 0;;) {
        const size_t separator = text.find(L':', position);
        if (count == 4) return std::nullopt;
        fields[count++] = text.substr(position, separator == std::wstring_view::npos ? separator : separator - position);
        if (separator == std::wstring_view::npos) break;
        position = separator + 1;
    }

    uint64_t hours = 0, minutes = 0;
    if (count == 4) {
        const uint32_t nominal = NominalFrameRate(fps);
        uint64_t seconds = 0, frames = 0;
        if (!ParseDigits(fields[0], 1, 6, hours) || !ParseDigits(fields[1], 1, 2, minutes) ||
            !ParseDigits(fields[2], 1, 2, seconds) || !ParseDigits(fields[3], 1, 6, frames) ||
            minutes >= 60 || seconds >= 60 || frames >= nominal)
            return std::nullopt;
        return FramePts(((hours * 60 + minutes) * 60 + seconds) * nominal + frames, fps);
    }

    if (count == 3 && !ParseDigits(fields[0], 1, 6, hours)) return std::nullopt;
    if (count >= 2) {
        // "mm:ss" allows minutes beyond 59; "h:mm:ss" bounds them.
        if (!ParseDigits(fields[count - 2], 1, count == 3 ? 2 : 7, minutes)) return std::nullopt;
        if (count == 3 && minutes >= 60) return std::nullopt;
    }
    int64_t seconds100ns = 0;
    if (!ParseSecondsField(fields[count - 1], count == 1 ? 9 : 2, count > 1, seconds100ns)) return std::nullopt;
    const int64_t total = static_cast<int64_t>((hours * 60 + minutes) * 60 * 10000000ull) + seconds100ns;
    return FramePts(FrameIndexNearest(total, fps), fps);
}

std::wstring FormatTimecode(int64_t pts100ns, double fps, bool withFrames)
{
    if (pts100ns < 0) pts100ns = 0;
    wchar_t buffer[64]{};
    if (withFrames && ValidFps(fps)) {
        const uint32_t nominal = NominalFrameRate(fps);
        const uint64_t index = FrameIndexNearest(pts100ns, fps);
        const uint64_t totalSeconds = index / nominal;
        swprintf_s(buffer, nominal > 100 ? L"%02llu:%02llu:%02llu:%03llu" : L"%02llu:%02llu:%02llu:%02llu",
                   static_cast<unsigned long long>(totalSeconds / 3600),
                   static_cast<unsigned long long>(totalSeconds / 60 % 60),
                   static_cast<unsigned long long>(totalSeconds % 60),
                   static_cast<unsigned long long>(index % nominal));
        return buffer;
    }
    const uint64_t totalMilliseconds = (static_cast<uint64_t>(pts100ns) + 5000) / 10000;
    const uint64_t totalSeconds = totalMilliseconds / 1000;
    swprintf_s(buffer, L"%llu:%02llu:%02llu.%03llu",
               static_cast<unsigned long long>(totalSeconds / 3600),
               static_cast<unsigned long long>(totalSeconds / 60 % 60),
               static_cast<unsigned long long>(totalSeconds % 60),
               static_cast<unsigned long long>(totalMilliseconds % 1000));
    return buffer;
}

uint32_t PrerollFramesFor(const NeuralRenderRange& range, double fps)
{
    if (range.Whole() || !ValidFps(fps) || range.start100ns <= 0) return 0;
    const uint64_t available = FrameIndexAtOrAfter(range.start100ns, fps);
    return static_cast<uint32_t>(std::min<uint64_t>(available, kDefaultPrerollFrames));
}
