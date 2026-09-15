#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

// Timeline arithmetic for partially rendered video, shared by the segment index
// that holds the coverage and the session policy that decides what to render
// next. Pure integer work on 100ns source timestamps: no clock, no files, no
// locks, so the awkward cases - a seek into a hole, a hole one frame wide, two
// regions that meet exactly - are answered in a test rather than on screen.
//
// A session renders in runs, one per job, and a user who seeks backwards makes
// the next run start behind the last one. Coverage is therefore a set of
// disjoint spans rather than a single head, and "is this frame rendered" is a
// question about the set.

// Half-open [start,end). An empty or inverted span covers nothing.
struct CoverageSpan {
    int64_t start100ns{};
    int64_t end100ns{};

    int64_t Width() const { return end100ns > start100ns ? end100ns - start100ns : 0; }
    bool Empty() const { return Width() == 0; }
    bool Contains(int64_t timestamp100ns) const
    {
        return timestamp100ns >= start100ns && timestamp100ns < end100ns;
    }
};

// Sorted, disjoint, and joined where they touch. Spans that merely touch are
// merged because a render boundary is not a hole: one job's last frame and the
// next job's first are consecutive frames of the same video.
inline std::vector<CoverageSpan> MergeSpans(std::vector<CoverageSpan> spans)
{
    spans.erase(std::remove_if(spans.begin(), spans.end(),
                               [](const CoverageSpan& span) { return span.Empty(); }),
                spans.end());
    std::sort(spans.begin(), spans.end(), [](const CoverageSpan& a, const CoverageSpan& b) {
        return a.start100ns < b.start100ns;
    });
    std::vector<CoverageSpan> merged;
    for (const CoverageSpan& span : spans) {
        if (!merged.empty() && span.start100ns <= merged.back().end100ns) {
            merged.back().end100ns = std::max(merged.back().end100ns, span.end100ns);
            continue;
        }
        merged.push_back(span);
    }
    return merged;
}

// What is left to render inside `range`, given coverage that may start before it
// and end after it. `minWidth100ns` drops slivers: an integer-frame render head
// lands a few ticks short of a fractional frame rate's declared end, and asking
// a job to render less than a frame earns a range refusal from the worker.
inline std::vector<CoverageSpan> UncoveredSpans(const std::vector<CoverageSpan>& covered,
                                                CoverageSpan range, int64_t minWidth100ns)
{
    std::vector<CoverageSpan> holes;
    if (range.Empty()) return holes;
    int64_t cursor = range.start100ns;
    for (const CoverageSpan& span : MergeSpans(covered)) {
        if (span.end100ns <= cursor) continue;
        if (span.start100ns >= range.end100ns) break;
        if (span.start100ns > cursor) holes.push_back({cursor, std::min(span.start100ns, range.end100ns)});
        cursor = std::max(cursor, span.end100ns);
        if (cursor >= range.end100ns) break;
    }
    if (cursor < range.end100ns) holes.push_back({cursor, range.end100ns});
    holes.erase(std::remove_if(holes.begin(), holes.end(),
                               [minWidth100ns](const CoverageSpan& hole) {
                                   return hole.Width() < minWidth100ns;
                               }),
                holes.end());
    return holes;
}

// Which hole to render next, from where the user is watching.
//
// The hole under the playhead wins, clipped to start at the playhead: the user
// is waiting on this frame, and rendering the seconds they have already passed
// first would make them wait for all of it. Failing that, the nearest hole ahead
// wins, because playback is about to arrive there. Only when nothing is left
// ahead does the earliest hole behind get rendered, which is how a session that
// began mid-video eventually covers its opening.
inline std::optional<CoverageSpan> NextRenderTarget(const std::vector<CoverageSpan>& holes,
                                                    int64_t position100ns)
{
    const CoverageSpan* behind = nullptr;
    for (const CoverageSpan& hole : holes) {
        if (hole.Empty()) continue;
        if (hole.Contains(position100ns)) return CoverageSpan{position100ns, hole.end100ns};
        if (hole.start100ns > position100ns) return hole;
        if (!behind) behind = &hole;
    }
    return behind ? std::optional<CoverageSpan>(*behind) : std::nullopt;
}

// Coverage the user can actually watch from here: the merged span containing the
// playhead. A later disjoint region is not lead - playback cannot reach it
// without crossing a hole - so a session that measures its buffer against the
// newest rendered timestamp would attach with nothing to show.
inline std::optional<CoverageSpan> SpanContaining(const std::vector<CoverageSpan>& covered,
                                                  int64_t position100ns)
{
    for (const CoverageSpan& span : MergeSpans(covered))
        if (span.Contains(position100ns)) return span;
    return std::nullopt;
}

// Rendered video inside `range`. The render pace is measured against this and
// not against the newest rendered timestamp, which counts the holes as work
// done and reports a session as faster than real time when it is not.
inline int64_t CoveredDuration100ns(const std::vector<CoverageSpan>& covered, CoverageSpan range)
{
    if (range.Empty()) return 0;
    int64_t sum = 0;
    for (const CoverageSpan& span : MergeSpans(covered)) {
        const int64_t start = std::max(span.start100ns, range.start100ns);
        const int64_t end = std::min(span.end100ns, range.end100ns);
        if (end > start) sum += end - start;
    }
    return sum;
}

// Rendered fraction of `range`, for a progress readout. 0 when nothing is
// covered, 1 only when every frame of the range is.
inline double CoveredFraction(const std::vector<CoverageSpan>& covered, CoverageSpan range)
{
    if (range.Empty()) return 0.0;
    return double(CoveredDuration100ns(covered, range)) / double(range.Width());
}
