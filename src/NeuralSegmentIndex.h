#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include "NeuralCoverage.h"

// One finalized neural output file produced by a running render job. Each file
// starts at its own pts zero; firstTimestamp100ns places it on the source
// timeline. end100ns is exclusive.
struct NeuralSegment {
    std::filesystem::path path;
    uint64_t runId{};  // the render job that published it
    uint64_t index{};  // the producer's own segment number inside that run
    uint64_t firstFrameNumber{};
    int64_t firstTimestamp100ns{};
    int64_t end100ns{};
    uint64_t frameCount{};
};

// Steady-state render pace observed from segment arrivals: wall time from the
// first segment this run published to the latest, over the frames of every
// segment after that first one. Excluding the first segment drops the job's
// startup (helper launch, preroll) exactly the way the reference constants in
// PlaybackTiming.h were measured.
struct SegmentPace {
    double wallMs{};
    uint64_t frames{};
    double MsPerFrame() const { return frames ? wallMs / double(frames) : 0.0; }
};

// Append-only, thread-safe. The render thread appends; the UI/playback threads read.
class NeuralSegmentIndex {
public:
    // Segments arrive from one run in timeline order, but a session renders in
    // several runs, and a user who seeks backwards makes the next run start
    // behind the last one. Storage is sorted by timestamp for that reason, and a
    // run's own index only orders that run.
    void Append(NeuralSegment segment)
    {
        const std::lock_guard lock(mutex_);
        if (segment.end100ns <= segment.firstTimestamp100ns) return;
        const auto at = std::upper_bound(
            segments_.begin(), segments_.end(), segment.firstTimestamp100ns,
            [](int64_t value, const NeuralSegment& existing) {
                return value < existing.firstTimestamp100ns;
            });
        // Ground that is already playable is never published over: a retargeted
        // run can overlap what an earlier one finished, and a republished file
        // would move the timeline under a decoder that is reading it.
        if (at != segments_.begin() && segment.firstTimestamp100ns < std::prev(at)->end100ns) return;
        // Measured from the span the producer declared, before the clamp below
        // can shorten it: dividing a clamped end by the full frame count yields
        // a duration too small to recognise the sub-frame seam this closes.
        const int64_t frameDuration =
            segment.frameCount
                ? (segment.end100ns - segment.firstTimestamp100ns) / int64_t(segment.frameCount)
                : 0;
        // A run filling a hole ends on a segment boundary rather than on the
        // hole's edge, so its last file can reach into the region beyond. Clamp
        // the declared end there: those frames are already served from the other
        // file, and one owner per timestamp is what makes a seek land in one
        // decoder.
        if (at != segments_.end())
            segment.end100ns = std::min(segment.end100ns, at->firstTimestamp100ns);
        if (segment.end100ns <= segment.firstTimestamp100ns) return;
        // A segment's exclusive end is rebuilt from an integer frame duration,
        // so a fractional frame rate declares it a few ticks below the next
        // segment's own first pts. Close that sub-frame hole: a playhead inside
        // it belongs to the earlier file, and reporting it as uncovered reads
        // as a producer contract break. A real gap - a run rebased further
        // ahead - is a frame or more wide and stays uncovered.
        if (frameDuration > 0 && at != segments_.begin()) {
            NeuralSegment& previous = *std::prev(at);
            const int64_t hole = segment.firstTimestamp100ns - previous.end100ns;
            if (hole > 0 && hole < frameDuration) previous.end100ns = segment.firstTimestamp100ns;
        }
        if (frameDuration > 0 && at != segments_.end()) {
            const int64_t hole = at->firstTimestamp100ns - segment.end100ns;
            if (hole > 0 && hole < frameDuration) segment.end100ns = at->firstTimestamp100ns;
        }
        totalFrames_ += segment.frameCount;
        segments_.insert(at, std::move(segment));
        ++revision_;
        const auto now = std::chrono::steady_clock::now();
        if (!paceStart_) {
            paceStart_ = now;
            paceBaseFrames_ = totalFrames_;
        }
        paceLatest_ = now;
    }

    SegmentPace Pace() const
    {
        const std::lock_guard lock(mutex_);
        if (!paceStart_ || totalFrames_ <= paceBaseFrames_) return {};
        return {std::chrono::duration<double, std::milli>(paceLatest_ - *paceStart_).count(),
                totalFrames_ - paceBaseFrames_};
    }

    // A new job's pace is its own: the clock starts at its first segment. The
    // adopted and chained cases were covered by `Unfinish` before coverage
    // became a set; without an explicit reset the idle gap between two jobs -
    // and every job startup while holes are being filled - counts as render
    // time and teaches the forecast a pace no GPU achieves.
    void ResetPace()
    {
        const std::lock_guard lock(mutex_);
        paceStart_.reset();
        paceBaseFrames_ = totalFrames_;
    }

    void Restart()
    {
        const std::lock_guard lock(mutex_);
        segments_.clear();
        totalFrames_ = 0;
        paceStart_.reset();
        ++revision_;
    }

    // Drops everything one run published, wherever those segments sit in the
    // timeline. A worker that restarted republishes from its own index zero, so
    // its earlier files are stale; coverage other runs left behind stays valid,
    // which is what lets a session retarget without redoing finished work.
    void DropRun(uint64_t runId)
    {
        const std::lock_guard lock(mutex_);
        uint64_t frames = 0;
        size_t matches = 0;
        for (const NeuralSegment& segment : segments_)
            if (segment.runId == runId) {
                frames += segment.frameCount;
                ++matches;
            }
        if (!matches) return;
        segments_.erase(std::remove_if(segments_.begin(), segments_.end(),
                                       [runId](const NeuralSegment& segment) {
                                           return segment.runId == runId;
                                       }),
                        segments_.end());
        totalFrames_ -= frames;
        ++revision_;
        paceStart_.reset();
    }

    // Bumped by every change to coverage. A repaint cannot be triggered off the
    // newest rendered timestamp any more: a run filling an earlier hole leaves
    // that unchanged while changing what the seek bar must show.
    uint64_t Revision() const
    {
        const std::lock_guard lock(mutex_);
        return revision_;
    }

    size_t Count() const
    {
        const std::lock_guard lock(mutex_);
        return segments_.size();
    }

    bool Empty() const
    {
        const std::lock_guard lock(mutex_);
        return segments_.empty();
    }

    std::optional<NeuralSegment> At(size_t index) const
    {
        const std::lock_guard lock(mutex_);
        if (index >= segments_.size()) return std::nullopt;
        return segments_[index];
    }

    std::optional<NeuralSegment> Containing(int64_t timestamp100ns) const
    {
        const std::lock_guard lock(mutex_);
        // Segments are stored sorted by start timestamp and never overlap, so a
        // playhead belongs to the last segment that starts at or before it.
        const auto above = std::upper_bound(
            segments_.begin(), segments_.end(), timestamp100ns,
            [](int64_t value, const NeuralSegment& segment) { return value < segment.firstTimestamp100ns; });
        if (above == segments_.begin()) return std::nullopt;
        const NeuralSegment& candidate = *std::prev(above);
        if (timestamp100ns >= candidate.end100ns) return std::nullopt;
        return candidate;
    }

    // Frame numbers are exact where a seeked source's pts is not, so a numbered
    // playhead picks its segment by number.
    std::optional<NeuralSegment> ContainingFrame(uint64_t frameNumber) const
    {
        const std::lock_guard lock(mutex_);
        const auto above = std::upper_bound(
            segments_.begin(), segments_.end(), frameNumber,
            [](uint64_t value, const NeuralSegment& segment) { return value < segment.firstFrameNumber; });
        if (above == segments_.begin()) return std::nullopt;
        const NeuralSegment& candidate = *std::prev(above);
        if (!candidate.frameCount ||
            frameNumber >= candidate.firstFrameNumber + candidate.frameCount) return std::nullopt;
        return candidate;
    }

    // The next segment along the timeline, whatever run published it. Used to
    // leave a file that ended inside its own declared window; the caller decides
    // whether what follows continues the picture or begins a later region.
    std::optional<NeuralSegment> After(int64_t timestamp100ns) const
    {
        const std::lock_guard lock(mutex_);
        const auto above = std::upper_bound(
            segments_.begin(), segments_.end(), timestamp100ns,
            [](int64_t value, const NeuralSegment& segment) { return value < segment.firstTimestamp100ns; });
        if (above == segments_.end()) return std::nullopt;
        return *above;
    }

    bool Covered(int64_t timestamp100ns) const { return Containing(timestamp100ns).has_value(); }

    // Every rendered region, sorted and joined where they touch. This is the
    // whole answer to "what is rendered": a session that has been retargeted
    // holds several, and the holes between them are what is left to do.
    std::vector<CoverageSpan> CoveredRanges() const
    {
        const std::lock_guard lock(mutex_);
        std::vector<CoverageSpan> spans;
        spans.reserve(segments_.size());
        for (const NeuralSegment& segment : segments_)
            spans.push_back({segment.firstTimestamp100ns, segment.end100ns});
        return MergeSpans(std::move(spans));
    }

    // The coverage playback can reach from here without crossing a hole. A
    // disjoint region further on is not buffer: measuring lead against the
    // newest rendered timestamp would attach a session with nothing to show.
    std::optional<CoverageSpan> PlayableSpan(int64_t timestamp100ns) const
    {
        return SpanContaining(CoveredRanges(), timestamp100ns);
    }

    // Span of the whole index, which is no longer one run: there can be holes
    // between Start100ns() and Head100ns(). Ask `Covered` about a timestamp and
    // `PlayableSpan` about a buffer; these two only bound the set.
    int64_t Start100ns() const
    {
        const std::lock_guard lock(mutex_);
        return segments_.empty() ? 0 : segments_.front().firstTimestamp100ns;
    }

    int64_t Head100ns() const
    {
        const std::lock_guard lock(mutex_);
        return segments_.empty() ? 0 : segments_.back().end100ns;
    }

    uint64_t TotalFrames() const
    {
        const std::lock_guard lock(mutex_);
        return totalFrames_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<NeuralSegment> segments_;
    uint64_t totalFrames_{};
    uint64_t revision_{};
    std::optional<std::chrono::steady_clock::time_point> paceStart_;
    std::chrono::steady_clock::time_point paceLatest_{};
    uint64_t paceBaseFrames_{};
};
