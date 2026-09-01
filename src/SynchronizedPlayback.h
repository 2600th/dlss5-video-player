#pragma once

#include "VideoDecoder.h"

#include <filesystem>
#include <memory>
#include <stop_token>

enum class ComparisonView { Original, Neural };
constexpr ComparisonView ToggleComparisonView(ComparisonView view) noexcept
{
    return view == ComparisonView::Original ? ComparisonView::Neural : ComparisonView::Original;
}

struct SynchronizedFramePair {
    VideoFrame original;
    VideoFrame neural;
    int64_t timestamp100ns{};
};

enum class SynchronizedReadResult {
    PairReady,
    NotReady,
    EndOfStream,
    Error,
    Cancelled,
    OutOfSync,
};

#ifdef SYNCHRONIZED_PLAYBACK_TESTING
class ISynchronizedFrameSource {
public:
    virtual ~ISynchronizedFrameSource() = default;
    virtual bool Open(const std::filesystem::path& path, std::stop_token stop) = 0;
    virtual void Close() = 0;
    virtual VideoReadResult Read(VideoFrame& frame, std::stop_token stop) = 0;
    virtual bool SeekSeconds(double seconds) = 0;
    virtual uint32_t Width() const = 0;
    virtual uint32_t Height() const = 0;
    virtual double FrameRate() const = 0;
    virtual double DurationSeconds() const = 0;
};
#endif

class SynchronizedPlayback {
public:
    SynchronizedPlayback();
#ifdef SYNCHRONIZED_PLAYBACK_TESTING
    SynchronizedPlayback(ISynchronizedFrameSource& original,
                         ISynchronizedFrameSource& neural);
#endif
    ~SynchronizedPlayback();
    SynchronizedPlayback(const SynchronizedPlayback&) = delete;
    SynchronizedPlayback& operator=(const SynchronizedPlayback&) = delete;
    SynchronizedPlayback(SynchronizedPlayback&&) noexcept;
    SynchronizedPlayback& operator=(SynchronizedPlayback&&) noexcept;

    bool Open(const std::filesystem::path& originalPath,
              const std::filesystem::path& neuralPath = {},
              std::stop_token stop = {});
    void Close();
    SynchronizedReadResult ReadNextAvailable(std::stop_token stop = {});
    bool SeekSeconds(double seconds, std::stop_token stop = {});
    bool SetView(ComparisonView view);
    ComparisonView View() const;
    const VideoFrame* VisibleFrame() const;
    const SynchronizedFramePair* CurrentPair() const;
    void SetPaused(bool paused);
    bool Paused() const;
    bool Step();
    bool NeuralAvailable() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
