#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// One finalized neural output file produced by a running render job. Each file
// starts at its own pts zero; firstTimestamp100ns places it on the source
// timeline. end100ns is exclusive.
struct NeuralSegment {
    std::filesystem::path path;
    uint64_t index{};
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
    void Append(NeuralSegment segment)
    {
        const std::lock_guard lock(mutex_);
        // A relaunched or confused producer must never reorder the timeline.
        if (!segments_.empty() && segment.index <= segments_.back().index) return;
        totalFrames_ += segment.frameCount;
        segments_.push_back(std::move(segment));
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

    void Restart()
    {
        const std::lock_guard lock(mutex_);
        segments_.clear();
        totalFrames_ = 0;
        finished_ = false;
        paceStart_.reset();
    }

    // Drops everything a later job appended, keeping the first `count`
    // segments. A resumed session hands earlier coverage to the next job, so a
    // relaunch of that job must undo only its own segments.
    void TruncateTo(size_t count)
    {
        const std::lock_guard lock(mutex_);
        if (count >= segments_.size()) return;
        for (size_t i = count; i < segments_.size(); ++i) totalFrames_ -= segments_[i].frameCount;
        segments_.resize(count);
        finished_ = false;
        paceStart_.reset();
    }

    // Adopted coverage from an earlier job is complete as far as that job went;
    // a resumed session has more to publish, so playback must not treat the
    // current head as the end of the stream.
    void Unfinish()
    {
        const std::lock_guard lock(mutex_);
        finished_ = false;
        // Adopted coverage arrived under an earlier job; the next job's pace
        // starts from its own first segment.
        paceStart_.reset();
    }

    void Finish()
    {
        const std::lock_guard lock(mutex_);
        finished_ = true;
    }

    bool Finished() const
    {
        const std::lock_guard lock(mutex_);
        return finished_;
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
        // Segments are appended in increasing index order and each one starts
        // where the previous ended, so the starts are sorted.
        const auto above = std::upper_bound(
            segments_.begin(), segments_.end(), timestamp100ns,
            [](int64_t value, const NeuralSegment& segment) { return value < segment.firstTimestamp100ns; });
        if (above == segments_.begin()) return std::nullopt;
        const NeuralSegment& candidate = *std::prev(above);
        if (timestamp100ns >= candidate.end100ns) return std::nullopt;
        return candidate;
    }

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
    bool finished_{};
    std::optional<std::chrono::steady_clock::time_point> paceStart_;
    std::chrono::steady_clock::time_point paceLatest_{};
    uint64_t paceBaseFrames_{};
};
