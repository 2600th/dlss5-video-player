#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The timeline as a render map: what the hover preview says and shows, where
// the chapters are, and which stretch is being rendered right now. Pure and
// windowless, like the chip policy beside it, so main.cpp only measures,
// launches and paints.
namespace timeline {

// ---- Chapters --------------------------------------------------------------

struct Chapter {
    double startSeconds{};
    double endSeconds{};
    std::wstring title;

    friend bool operator==(const Chapter&, const Chapter&) = default;
};

// ffprobe's own arguments for the question, next to the parser that reads the
// answer so the two cannot drift. `default=noprint_wrappers=1` prints one
// key=value per line and a chapter always starts with its start_time; a title,
// when there is one, is a TAG:title line inside it. CSV was the obvious choice
// and the wrong one: a chapter title is free text and may hold the separator.
inline std::vector<std::wstring> ChapterProbeArguments(const std::filesystem::path& media)
{
    return {L"-v", L"error", L"-show_entries", L"chapter=start_time,end_time:chapter_tags=title",
            L"-of", L"default=noprint_wrappers=1", L"-i", media.wstring()};
}

inline std::wstring Utf8ToWideText(std::string_view text)
{
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

// Chapters sorted by start, with anything unreadable dropped rather than
// guessed at: a marker at the wrong place is worse than a missing one. A title
// is trimmed to one line, since it is drawn into one.
inline std::vector<Chapter> ParseFfprobeChapters(std::string_view output)
{
    std::vector<Chapter> chapters;
    std::optional<Chapter> current;
    bool startValid = false;
    const auto finish = [&] {
        if (current && startValid) chapters.push_back(std::move(*current));
        current.reset();
        startValid = false;
    };
    const auto parseSeconds = [](std::string_view value, double& seconds) {
        std::string copy(value);
        char* end = nullptr;
        const double parsed = std::strtod(copy.c_str(), &end);
        if (end == copy.c_str() || !std::isfinite(parsed) || parsed < 0.0) return false;
        seconds = parsed;
        return true;
    };
    size_t at = 0;
    while (at <= output.size()) {
        const size_t newline = output.find('\n', at);
        std::string_view line = output.substr(at, newline == std::string_view::npos ? std::string_view::npos : newline - at);
        at = newline == std::string_view::npos ? output.size() + 1 : newline + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, equals), value = line.substr(equals + 1);
        if (key == "start_time") {
            finish();
            current = Chapter{};
            startValid = parseSeconds(value, current->startSeconds);
        } else if (current && key == "end_time") {
            double end = 0.0;
            if (parseSeconds(value, end)) current->endSeconds = end;
        } else if (current && key == "TAG:title") {
            current->title = Utf8ToWideText(value);
            for (wchar_t& character : current->title)
                if (character < L' ') character = L' ';
        }
    }
    finish();
    std::stable_sort(chapters.begin(), chapters.end(),
                     [](const Chapter& a, const Chapter& b) { return a.startSeconds < b.startSeconds; });
    return chapters;
}

// The chapter a moment falls in: the last one that starts at or before it.
inline const Chapter* ChapterAt(const std::vector<Chapter>& chapters, double seconds)
{
    const Chapter* found = nullptr;
    for (const Chapter& chapter : chapters) {
        if (chapter.startSeconds > seconds) break;
        found = &chapter;
    }
    return found;
}

// ---- Hover text --------------------------------------------------------------

// "0:42", "1:02:03" - read, not aligned, so no leading zero hour.
inline std::wstring ClockText(double seconds)
{
    const long long total = std::max<long long>(0, static_cast<long long>(std::floor(seconds)));
    const long long hours = total / 3600, minutes = (total / 60) % 60, rest = total % 60;
    wchar_t text[32]{};
    if (hours > 0) swprintf_s(text, L"%lld:%02lld:%02lld", hours, minutes, rest);
    else swprintf_s(text, L"%lld:%02lld", minutes, rest);
    return text;
}

struct HoverFacts {
    double seconds{};
    bool durationKnown{true};
    // Whether the moment under the cursor has neural frames. Nothing when no
    // render exists for this source, so an ordinary video is not told it is
    // "not rendered" on every hover.
    std::optional<bool> rendered;
    // Until everything the running session set out to render is rendered.
    std::optional<double> secondsToFullCoverage;
    std::wstring chapter;
};

// One line: where you are, which chapter, and what the render map says there.
inline std::wstring HoverText(const HoverFacts& facts)
{
    if (!facts.durationKnown) return L"Length unknown · Left and Right seek 10 s";
    std::wstring text = ClockText(facts.seconds);
    if (!facts.chapter.empty()) text += L" · " + facts.chapter;
    if (facts.rendered) text += *facts.rendered ? L" · Rendered" : L" · Not rendered yet";
    if (facts.secondsToFullCoverage && *facts.secondsToFullCoverage > 0.0)
        text += L" · All rendered in " + ClockText(std::ceil(*facts.secondsToFullCoverage));
    return text;
}

// ---- Rendering now --------------------------------------------------------

// The hatched stretch at the render head: from the head to the end of the
// hole the job is filling, but never longer than `maxWidth` - the job renders
// a few frames at a time, and hatching a whole twenty-minute hole would say
// "being rendered" about minutes nobody will reach for a while - and never
// thinner than `minWidth`, so a head at the very end of its hole still shows.
// Nothing when there is no head on the track.
inline std::optional<std::pair<LONG, LONG>> RenderingNowSpan(LONG trackLeft, LONG trackRight,
                                                             LONG headX, LONG targetEndX,
                                                             LONG minWidth, LONG maxWidth)
{
    if (trackRight <= trackLeft || headX < trackLeft || headX >= trackRight) return std::nullopt;
    LONG right = std::min({targetEndX, headX + maxWidth, trackRight});
    if (right - headX < minWidth) right = std::min(trackRight, headX + minWidth);
    if (right <= headX) return std::nullopt;
    return std::pair<LONG, LONG>{headX, right};
}

// ---- Hover thumbnails -----------------------------------------------------

// Thumbnails are cached per bucket, not per pixel: 200 buckets over the video,
// but never finer than a second, which is as close as a keyframe seek lands
// anyway. A cursor resting anywhere in one bucket reuses one decode.
inline constexpr int kThumbnailBuckets = 200;

inline int64_t ThumbnailBucket(double seconds, double duration)
{
    if (!(duration > 0.0)) return 0;
    const double width = std::max(1.0, duration / kThumbnailBuckets);
    return static_cast<int64_t>(std::floor(std::clamp(seconds, 0.0, duration) / width));
}

inline double ThumbnailBucketSeconds(int64_t bucket, double duration)
{
    if (!(duration > 0.0)) return 0.0;
    const double width = std::max(1.0, duration / kThumbnailBuckets);
    return std::min(duration, (static_cast<double>(bucket) + 0.5) * width);
}

// Even dimensions at the display aspect: 4:2:0 sources scale cleanly to them.
inline SIZE ThumbnailSize(double displayAspect, int width)
{
    const double aspect = std::isfinite(displayAspect) && displayAspect > 0.2 ? displayAspect : 16.0 / 9.0;
    const int evenWidth = std::max(2, width & ~1);
    const int height = std::max(2, static_cast<int>(std::lround(evenWidth / aspect)) & ~1);
    return SIZE{evenWidth, height};
}

// One frame, the keyframe at or before the moment, scaled and handed back as
// raw BGRA on stdout. -noaccurate_seek is what makes it cheap: an accurate
// seek decodes every frame from that keyframe to the moment, which on a
// long-GOP stream is seconds per hover.
inline std::vector<std::wstring> ThumbnailArguments(const std::filesystem::path& media, double seconds,
                                                    SIZE size)
{
    wchar_t at[32]{};
    swprintf_s(at, L"%.3f", std::max(0.0, seconds));
    return {L"-v", L"error", L"-nostdin", L"-noaccurate_seek", L"-ss", at, L"-i", media.wstring(),
            L"-map", L"0:v:0", L"-frames:v", L"1",
            L"-vf", L"scale=" + std::to_wstring(size.cx) + L":" + std::to_wstring(size.cy) + L":flags=bilinear",
            L"-f", L"rawvideo", L"-pix_fmt", L"bgra", L"pipe:1"};
}

// Most-recently-used first; the oldest goes when a new key would pass the
// capacity. Small on purpose: a thumbnail is ~60 KB and a hover sweep visits
// dozens, of which only the last few are wanted again.
template <typename Value>
class LruCache {
public:
    explicit LruCache(size_t capacity) : capacity_(std::max<size_t>(1, capacity)) {}

    const Value* Find(int64_t key)
    {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->first != key) continue;
            entries_.splice(entries_.begin(), entries_, it);
            return &entries_.front().second;
        }
        return nullptr;
    }

    void Insert(int64_t key, Value value)
    {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->first == key) { entries_.erase(it); break; }
        }
        entries_.emplace_front(key, std::move(value));
        while (entries_.size() > capacity_) entries_.pop_back();
    }

    void Clear() { entries_.clear(); }
    size_t Size() const { return entries_.size(); }

private:
    size_t capacity_;
    std::list<std::pair<int64_t, Value>> entries_;
};

// ---- The preview popup ----------------------------------------------------

struct PreviewLayout {
    RECT window{};      // screen coordinates
    RECT thumbnail{};   // client coordinates; empty without a thumbnail
    RECT text{};        // client coordinates
};

// Centred over the cursor, standing on the track with a gap, and kept inside
// the monitor's work area; it never covers the track it describes.
inline PreviewLayout LayoutPreview(POINT cursorOnTrackTop, SIZE thumbnail, SIZE text, int padding, int gap,
                                   RECT workArea)
{
    const int width = std::max<int>(thumbnail.cx, text.cx) + 2 * padding;
    const int height = (thumbnail.cy > 0 ? thumbnail.cy + padding : 0) + text.cy + 2 * padding;
    LONG left = cursorOnTrackTop.x - width / 2;
    left = std::clamp<LONG>(left, workArea.left, std::max<LONG>(workArea.left, workArea.right - width));
    const LONG bottom = cursorOnTrackTop.y - gap;
    const LONG top = std::max<LONG>(workArea.top, bottom - height);
    PreviewLayout layout{};
    layout.window = RECT{left, top, left + width, top + height};
    if (thumbnail.cy > 0) {
        const int thumbLeft = (width - thumbnail.cx) / 2;
        layout.thumbnail = RECT{thumbLeft, padding, thumbLeft + thumbnail.cx, padding + thumbnail.cy};
    }
    const int textTop = thumbnail.cy > 0 ? padding + thumbnail.cy + padding : padding;
    layout.text = RECT{padding, textTop, width - padding, textTop + text.cy};
    return layout;
}

} // namespace timeline
