#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "PlaybackTiming.h"
#include "UpscalingPolicy.h"

// The steady-state neural render paces this machine measured, so the
// live-session forecast speaks for this GPU rather than the reference.
// Stored per source geometry as `Samples=WxH:ms,ms,...;WxH:ms` for the
// named GPU; a different GPU starts from an empty profile, and the
// single-value `WxH:ms` form earlier versions wrote still loads as a
// one-sample ring. PlayerApp reads and writes the INI; the record, its text
// form and the profiles the forecast reads are kept here.
//
// One sample per geometry used to be the whole record, newest replacing
// oldest unconditionally. A session measured while another process
// saturated the CPU wrote 42.333431 ms/frame for 1920x1080 - 3.7x the
// 11.4222 ms mean of the eight idle sessions around it, with the GPU free -
// and because this record is persisted, that one number followed the user
// across restarts: the next sessions forecast under a third of real time
// and raised the "watching it live would pause to buffer almost
// continuously" warning on hardware that renders that clip at 2.9x real
// time. Contention can only ever make a render look slower, so the error is
// one-sided and the newest sample is not the most trustworthy one; keeping
// a few and taking the median lets the measurements outvote the outlier.
//
// The median rather than the minimum, which would be the fastest way to
// erase a contended sample: this forecast exists to refuse sessions that
// cannot keep up, an optimistic estimator hides exactly the warning that
// was missing when a 4K60 session dropped 848 of 869 frames, and one-sided
// error means a low quantile drifts towards the best case the machine has
// ever had rather than the case the user is about to get.
//
// Kept per processing-scale rung as well (live_session::
// ForecastAtProcessingScale): a 50% session's pace used to be filed under
// the source geometry, where it pulled the 100% forecast toward a pace
// 100% never reaches. The reduced rungs persist under `Samples75` and
// `Samples50`, which a build without rungs does not read.
namespace render_pace {

inline constexpr size_t kRingSamples = 5;

struct History {
    uint32_t width{}, height{};
    std::vector<double> msPerFrame;
    uint32_t scale{kDefaultProcessingScale};
};

// The reduced rungs' profiles, in kProcessingScaleRungs order after the
// source-scale one.
using ReducedProfiles = std::array<playback_timing::RenderPaceProfile, std::size(kProcessingScaleRungs) - 1>;

inline double Median(std::vector<double> samples)
{
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t middle = samples.size() / 2;
    return samples.size() % 2 ? samples[middle] : (samples[middle - 1] + samples[middle]) * 0.5;
}

inline void Record(std::vector<History>& histories, uint32_t width, uint32_t height, double msPerFrame,
                   uint32_t scale = kDefaultProcessingScale)
{
    if (!width || !height || !(msPerFrame > 0.0) || !IsProcessingScaleRung(scale)) return;
    size_t atScale = 0;
    for (History& history : histories) {
        if (history.scale != scale) continue;
        ++atScale;
        if (history.width != width || history.height != height) continue;
        history.msPerFrame.push_back(msPerFrame);
        if (history.msPerFrame.size() > kRingSamples) history.msPerFrame.erase(history.msPerFrame.begin());
        return;
    }
    // Same bound as the profile the forecast reads, and the same eviction:
    // a geometry nobody has played for six geometries is the one to lose.
    // Per rung, so trying a rung cannot evict the source-scale history.
    if (atScale == playback_timing::RenderPaceProfile::kMaxSamples)
        histories.erase(std::find_if(histories.begin(), histories.end(),
                                     [&](const History& history) { return history.scale == scale; }));
    histories.push_back({width, height, {msPerFrame}, scale});
}

// How many samples the ring for one geometry at one rung holds.
inline size_t SamplesKept(const std::vector<History>& histories, uint32_t scale, uint32_t width, uint32_t height)
{
    for (const History& history : histories)
        if (history.scale == scale && history.width == width && history.height == height)
            return history.msPerFrame.size();
    return 0;
}

// The source-scale profile for 100 (or anything that is not a reduced
// rung); the reduced rungs follow in kProcessingScaleRungs order.
inline playback_timing::RenderPaceProfile& ProfileAt(uint32_t scale, playback_timing::RenderPaceProfile& source,
                                                     ReducedProfiles& reduced)
{
    for (size_t rung = 1; rung < std::size(kProcessingScaleRungs); ++rung)
        if (kProcessingScaleRungs[rung] == scale) return reduced[rung - 1];
    return source;
}
inline const playback_timing::RenderPaceProfile& ProfileAt(uint32_t scale,
                                                           const playback_timing::RenderPaceProfile& source,
                                                           const ReducedProfiles& reduced)
{
    for (size_t rung = 1; rung < std::size(kProcessingScaleRungs); ++rung)
        if (kProcessingScaleRungs[rung] == scale) return reduced[rung - 1];
    return source;
}

// The forecast reads one number per geometry; that number is the geometry's
// median, recomputed whenever the history changes.
inline void Rebuild(const std::vector<History>& histories, playback_timing::RenderPaceProfile& source,
                    ReducedProfiles& reduced)
{
    source = {};
    for (auto& profile : reduced) profile = {};
    for (const History& history : histories)
        ProfileAt(history.scale, source, reduced).Record({history.width, history.height, Median(history.msPerFrame)});
}

inline std::wstring SamplesKey(uint32_t scale)
{
    return scale == kDefaultProcessingScale ? std::wstring(L"Samples") : L"Samples" + std::to_wstring(scale);
}

// Reads one `Samples` value into the record for `scale`. An entry without a
// geometry is skipped; a sample that is not a number is skipped on its own.
inline void Parse(std::wstring_view samples, uint32_t scale, std::vector<History>& histories)
{
    for (size_t start = 0; start < samples.size();) {
        size_t end = samples.find(L';', start);
        if (end == std::wstring_view::npos) end = samples.size();
        const std::wstring entry(samples.substr(start, end - start));
        start = end + 1;
        const size_t cross = entry.find(L'x');
        if (cross == std::wstring::npos) continue;
        const size_t colon = entry.find(L':', cross);
        if (colon == std::wstring::npos) continue;
        unsigned width = 0, height = 0;
        if (swscanf_s(entry.c_str(), L"%ux%u", &width, &height) != 2) continue;
        for (size_t sample = colon + 1; sample <= entry.size();) {
            size_t sampleEnd = entry.find(L',', sample);
            if (sampleEnd == std::wstring::npos) sampleEnd = entry.size();
            double ms = 0.0;
            if (swscanf_s(entry.substr(sample, sampleEnd - sample).c_str(), L"%lf", &ms) == 1)
                Record(histories, width, height, ms, scale);
            sample = sampleEnd + 1;
        }
    }
}

// The `Samples` value for `scale`; empty when that rung has nothing measured.
inline std::wstring Format(const std::vector<History>& histories, uint32_t scale)
{
    std::wstring samples;
    for (const History& history : histories) {
        if (history.scale != scale || history.msPerFrame.empty()) continue;
        if (!samples.empty()) samples += L';';
        wchar_t geometry[32]{};
        swprintf_s(geometry, L"%ux%u:", history.width, history.height);
        samples += geometry;
        for (size_t index = 0; index < history.msPerFrame.size(); ++index) {
            wchar_t text[32]{};
            swprintf_s(text, L"%.6f", history.msPerFrame[index]);
            if (index) samples += L',';
            samples += text;
        }
    }
    return samples;
}

} // namespace render_pace
