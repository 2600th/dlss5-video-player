#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

// Whether a source's frames are evenly spaced, decided from the spacing
// itself rather than from the two rates the container declares.
//
// VideoDecoder::ConstantFrameRate() compares avg_frame_rate against
// r_frame_rate, and both are unreliable for exactly the sources that matter
// here. r_frame_rate is the smallest interval the container's timebase can
// express - for a screen recording that is whatever the compositor's fastest
// frame was - and avg_frame_rate is frames over duration. A capture that ran
// at 60 and dropped a tenth of its frames declares 60 and 54 and is caught; a
// capture that dropped almost nothing declares 60 and 59.8 and sails through,
// and a perfectly constant file whose duration metadata is slightly long is
// refused for no reason at all.
//
// Why it matters more for this player than for an ordinary one: frame
// generation interpolates between two frames on the assumption that the
// interval between them is the interval between every other pair. Feed it
// spacing that is 16 ms and then 83 ms and the motion it invents speeds up and
// lurches. Refusing with a stated reason is the behaviour the product already
// promises - "when it cannot, it says why instead of greying out" - and a
// variable rate is one more reason on that list.
namespace variable_frame_rate {

// One second at 24 fps. Fewer intervals than this and a single hiccup is a
// large fraction of the evidence, so the answer would be noise.
inline constexpr size_t kMinimumIntervals = 24;

// What a caller should ask the container for. Five seconds at 24 fps: long
// enough that a periodic pattern shows up, short enough that sampling it costs
// a fraction of a second.
inline constexpr size_t kRecommendedSamples = 120;

// Absolute slack, chosen to absorb millisecond-resolution timestamps. A
// 23.976 fps source stored with millisecond precision alternates between 41 ms
// and 42 ms forever; that is rounding, not a variable rate, and it is the
// single most common false positive available.
inline constexpr double kIntervalToleranceSeconds = 0.0015;

// Proportional slack, which takes over for slow sources where 1.5 ms is a
// smaller share of the interval than the container's own jitter.
inline constexpr double kIntervalToleranceFraction = 0.02;

// How much of the source may deviate before the spacing stops being constant.
// A dropped frame here and there is a hiccup - a disc that stuttered, a
// network read that arrived late - and refusing frame generation over one
// frame in a hundred would be worse than useless.
inline constexpr double kDeviatingFraction = 0.05;

// Timestamps handed over in coded order rather than presentation order carry
// no spacing information. A few reordered pairs are ordinary B-frame output;
// a quarter of them means the caller sampled the wrong thing, and the answer
// is "cannot tell" rather than a guess in either direction.
inline constexpr double kMaximumReorderedFraction = 0.25;

struct Verdict {
    // False when there was not enough usable evidence. The caller keeps
    // whatever answer it already had - it must never read as "variable",
    // because refusing a feature on absent evidence is the same mistake in
    // the other direction.
    bool decided = false;
    bool constant = true;
    double medianIntervalSeconds = 0.0;
    size_t intervals = 0;
    size_t deviatingIntervals = 0;
    size_t reorderedIntervals = 0;
};

// `presentationTimesSeconds` is consecutive frame presentation timestamps in
// the order the container lists them.
inline Verdict Classify(std::span<const double> presentationTimesSeconds)
{
    Verdict verdict{};
    if (presentationTimesSeconds.size() < 2) return verdict;

    std::vector<double> intervals;
    intervals.reserve(presentationTimesSeconds.size() - 1);
    for (size_t index = 1; index < presentationTimesSeconds.size(); ++index) {
        const double interval = presentationTimesSeconds[index] - presentationTimesSeconds[index - 1];
        if (!std::isfinite(interval)) continue;
        // A non-positive gap is a reordered pair, not a zero-length frame.
        if (interval <= 0.0) { ++verdict.reorderedIntervals; continue; }
        intervals.push_back(interval);
    }

    const size_t gaps = verdict.reorderedIntervals + intervals.size();
    if (gaps == 0) return verdict;
    if (double(verdict.reorderedIntervals) > kMaximumReorderedFraction * double(gaps)) return verdict;
    if (intervals.size() < kMinimumIntervals) return verdict;

    // The median, not the mean: a handful of long gaps must not drag the
    // reference interval toward themselves and make the majority look like the
    // deviation.
    std::vector<double> sorted = intervals;
    std::nth_element(sorted.begin(), sorted.begin() + ptrdiff_t(sorted.size() / 2), sorted.end());
    const double median = sorted[sorted.size() / 2];
    if (!(median > 0.0)) return verdict;

    const double tolerance =
        std::max(kIntervalToleranceSeconds, kIntervalToleranceFraction * median);
    size_t deviating = 0;
    for (const double interval : intervals)
        if (std::abs(interval - median) > tolerance) ++deviating;

    verdict.decided = true;
    verdict.medianIntervalSeconds = median;
    verdict.intervals = intervals.size();
    verdict.deviatingIntervals = deviating;
    verdict.constant = double(deviating) <= kDeviatingFraction * double(intervals.size());
    return verdict;
}

} // namespace variable_frame_rate
