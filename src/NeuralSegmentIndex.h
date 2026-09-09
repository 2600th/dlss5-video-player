#pragma once
#include <algorithm>
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
    }

    void Restart()
    {
        const std::lock_guard lock(mutex_);
        segments_.clear();
        totalFrames_ = 0;
        finished_ = false;
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
    }

    // Adopted coverage from an earlier job is complete as far as that job went;
    // a resumed session has more to publish, so playback must not treat the
    // current head as the end of the stream.
    void Unfinish()
    {
        const std::lock_guard lock(mutex_);
        finished_ = false;
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
};
