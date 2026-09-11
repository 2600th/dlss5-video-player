#pragma once

#include "NeuralSegmentIndex.h"
#include "VideoDecoder.h"

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#ifdef SYNCHRONIZED_PLAYBACK_TESTING
#include <functional>
#endif

enum class ComparisonView { Original, Neural };
constexpr ComparisonView ToggleComparisonView(ComparisonView view) noexcept
{
    return view == ComparisonView::Original ? ComparisonView::Neural : ComparisonView::Original;
}

// Half-open window [start100ns, end100ns) of the original timeline that the
// neural member covers; end 0 = the original's end. The neural file starts at
// its own zero, so its frames are shifted by start100ns before pairing.
struct SynchronizedRange {
    int64_t start100ns{};
    int64_t end100ns{};
};

struct SynchronizedFramePair {
    VideoFrame original;
    VideoFrame neural;
    int64_t timestamp100ns{};
    uint64_t frameNumber{};
};

enum class SynchronizedReadResult {
    PairReady,
    NotReady,
    EndOfStream,
    Error,
    Cancelled,
    OutOfSync,
    // Live mode only: the playhead reached the render head and the job has not
    // finalized the segment that covers it yet. Playback stalls, it never ends.
    WaitingForRender,
};

#ifdef SYNCHRONIZED_PLAYBACK_TESTING
class ISynchronizedFrameSource {
public:
    virtual ~ISynchronizedFrameSource() = default;
    virtual bool Open(const std::filesystem::path& path, std::stop_token stop) = 0;
    // Live segments after the first are opened with the parameters the first one
    // probed. A fake that does not care can leave this alone.
    virtual bool OpenKnown(const std::filesystem::path& path, const VideoDecoder::KnownMedia&,
                           std::stop_token stop) { return Open(path, stop); }
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
    // Live mode opens one decoder per segment file, so tests inject a factory
    // instead of a fixed pair. The created source is told which file to serve
    // by the path handed to its Open.
    using SegmentSourceFactory = std::function<std::unique_ptr<ISynchronizedFrameSource>()>;
    SynchronizedPlayback(ISynchronizedFrameSource& original,
                         ISynchronizedFrameSource& neural);
    SynchronizedPlayback(ISynchronizedFrameSource& original, SegmentSourceFactory segments);
#endif
    ~SynchronizedPlayback();
    SynchronizedPlayback(const SynchronizedPlayback&) = delete;
    SynchronizedPlayback& operator=(const SynchronizedPlayback&) = delete;
    SynchronizedPlayback(SynchronizedPlayback&&) noexcept;
    SynchronizedPlayback& operator=(SynchronizedPlayback&&) noexcept;

    bool Open(const std::filesystem::path& originalPath,
              const std::filesystem::path& neuralPath = {},
              std::stop_token stop = {},
              SynchronizedRange range = {});
    // Plays the original against a render job that is still running: the neural
    // member is the growing segment index instead of one finished file.
    bool OpenLive(const std::filesystem::path& originalPath,
                  std::shared_ptr<const NeuralSegmentIndex> segments,
                  SynchronizedRange range,
                  std::stop_token stop = {});
    SynchronizedRange Range() const;
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
    bool Live() const;
    int64_t LiveHead100ns() const;
    // Why the last read reported OutOfSync, empty when nothing failed.
    std::string LastFault() const;

private:
    bool SeekLive(double seconds, std::stop_token stop);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
