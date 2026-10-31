#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Where a video was left, kept per source in the settings file beside its
// subtitle choice and its mask, under the same key (compare_mask::SourceKey of
// the source's identity: its path, or "youtube:" and the video id).
//
// The player resumes by itself, as mpv's save-position-on-quit does, and says
// so in a toast that names the way back to the start. A question on every open
// would stop the playback that was just asked for, and an offer that times out
// starts from the beginning whenever it is missed. What is kept is bounded at
// both ends, the way Kodi bounds it: a position in the first seconds is not
// worth a jump, and one in the last few percent is a video that was finished,
// so the next open starts at the beginning and the entry is removed.
namespace resume {

// A position this early is not kept: starting over costs no more than this.
inline constexpr double kIgnoreStartSeconds = 10.0;
// The end: the last 5 % of the video, and never less than the last 10 s, so
// a two-minute trailer stopped in its final seconds counts as watched and a
// film stopped in its credits does too.
inline constexpr double kEndFraction = 0.05;
inline constexpr double kIgnoreEndSeconds = 10.0;
// How far a source's duration may move before the position is not trusted: a
// different file saved under the same name is not the video that was left.
inline constexpr double kDurationToleranceSeconds = 1.0;
inline constexpr double kDurationToleranceFraction = 0.01;
// As many as the subtitle choices keep; the oldest go first.
inline constexpr size_t kMaxRemembered = 200;

inline double EndThreshold(double durationSeconds)
{
    return std::max(kIgnoreEndSeconds, durationSeconds * kEndFraction);
}

// Whether `position` in a video of `duration` is worth coming back to. A video
// of unknown length, or too short to have a middle, has nothing to resume.
inline bool Worth(double positionSeconds, double durationSeconds)
{
    if (!std::isfinite(positionSeconds) || !std::isfinite(durationSeconds) || durationSeconds <= 0.0) return false;
    return positionSeconds >= kIgnoreStartSeconds && positionSeconds <= durationSeconds - EndThreshold(durationSeconds);
}

struct Record {
    uint64_t sequence = 0;
    double position = 0.0;
    double duration = 0.0;

    friend bool operator==(const Record&, const Record&) = default;
};

// Where to start a video of `durationNow`, or nothing for the beginning.
inline std::optional<double> ResumeAt(const Record& record, double durationNow)
{
    if (!Worth(record.position, durationNow)) return std::nullopt;
    const double tolerance = std::max(kDurationToleranceSeconds, durationNow * kDurationToleranceFraction);
    if (!std::isfinite(record.duration) || std::abs(record.duration - durationNow) > tolerance) return std::nullopt;
    return record.position;
}

// "<sequence>|<position>|<duration>", seconds to the millisecond.
inline std::wstring Format(const Record& record)
{
    wchar_t text[96]{};
    swprintf(text, std::size(text), L"%llu|%.3f|%.3f", static_cast<unsigned long long>(record.sequence),
             record.position, record.duration);
    return text;
}

inline std::optional<Record> Parse(std::wstring_view text)
{
    const auto field = [&text]() -> std::optional<std::wstring_view> {
        if (text.empty()) return std::nullopt;
        const size_t bar = text.find(L'|');
        const std::wstring_view value = text.substr(0, bar);
        text = bar == std::wstring_view::npos ? std::wstring_view{} : text.substr(bar + 1);
        return value;
    };
    const auto sequenceText = field(), positionText = field(), durationText = field();
    if (!sequenceText || !positionText || !durationText || !text.empty()) return std::nullopt;
    Record record;
    if (sequenceText->empty() || sequenceText->size() > 18) return std::nullopt;
    for (const wchar_t c : *sequenceText) {
        if (c < L'0' || c > L'9') return std::nullopt;
        record.sequence = record.sequence * 10u + uint64_t(c - L'0');
    }
    const auto seconds = [](std::wstring_view value) -> std::optional<double> {
        // Digits and one point: no sign, no exponent, nothing a hand edit could
        // turn into a seek to infinity.
        if (value.empty() || value.size() > 16) return std::nullopt;
        size_t points = 0;
        for (const wchar_t c : value) {
            if (c == L'.') ++points;
            else if (c < L'0' || c > L'9') return std::nullopt;
        }
        if (points > 1 || value.front() == L'.' || value.back() == L'.') return std::nullopt;
        return std::wcstod(std::wstring(value).c_str(), nullptr);
    };
    const auto position = seconds(*positionText), duration = seconds(*durationText);
    if (!position || !duration) return std::nullopt;
    record.position = *position;
    record.duration = *duration;
    return record;
}

// The oldest keys past `keep`, to delete from the settings file.
inline std::vector<std::wstring> Evict(const std::map<std::wstring, Record>& records, size_t keep = kMaxRemembered)
{
    std::vector<std::pair<uint64_t, std::wstring>> order;
    for (const auto& [key, record] : records) order.emplace_back(record.sequence, key);
    std::sort(order.begin(), order.end());
    std::vector<std::wstring> evicted;
    for (size_t index = 0; index + keep < order.size(); ++index) evicted.push_back(order[index].second);
    return evicted;
}

// "1:02:03" or "4:05", for the toast that says where playback resumed.
inline std::wstring ClockText(double seconds)
{
    const long long whole = std::isfinite(seconds) && seconds > 0.0 ? static_cast<long long>(seconds) : 0;
    wchar_t text[32]{};
    if (whole >= 3600)
        swprintf(text, std::size(text), L"%lld:%02lld:%02lld", whole / 3600, (whole / 60) % 60, whole % 60);
    else
        swprintf(text, std::size(text), L"%lld:%02lld", whole / 60, whole % 60);
    return text;
}

} // namespace resume
