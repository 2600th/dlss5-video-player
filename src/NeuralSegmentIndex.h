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
