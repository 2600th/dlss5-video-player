#pragma once

#include "NeuralSegmentIndex.h"
#include "PixelLayout.h"
#include "VideoDecoder.h"

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <functional>

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
    // Byte layout of the frames this source hands over. Both members of a pair
    // feed one renderer, whose layout is fixed at Initialize, so a mismatch is
    // a decode error rather than a thing to paper over; AdoptSegment checks it
    // the same way it checks geometry. A fake that only serves BGRA leaves this.
    virtual PixelLayout Layout() const { return PixelLayout::Bgra; }
    // Asked before Open. A REQUEST: odd geometry or a colour description the
    // GPU conversion does not implement still decodes to BGRA, and Layout()
    // reports what actually happened.
    virtual void PreferNv12(bool) {}
    // What the next sibling of the open file can be opened with.
    virtual VideoDecoder::KnownMedia Media() const
    {
        return {Width(), Height(), FrameRate(), DurationSeconds(), {}};
    }
};

class SynchronizedPlayback {
public:
    SynchronizedPlayback();
    // Live mode opens one decoder per segment file, so tests inject a factory
    // instead of a fixed pair. The created source is told which file to serve
    // by the path handed to its Open.
    using SegmentSourceFactory = std::function<std::unique_ptr<ISynchronizedFrameSource>()>;
    SynchronizedPlayback(ISynchronizedFrameSource& original,
                         ISynchronizedFrameSource& neural);
    SynchronizedPlayback(ISynchronizedFrameSource& original, SegmentSourceFactory segments);
    ~SynchronizedPlayback();
    SynchronizedPlayback(const SynchronizedPlayback&) = delete;
    SynchronizedPlayback& operator=(const SynchronizedPlayback&) = delete;
    SynchronizedPlayback(SynchronizedPlayback&&) noexcept;
    SynchronizedPlayback& operator=(SynchronizedPlayback&&) noexcept;

    // `originalMedia`, when valid, is the caller's own probe of `originalPath`
    // and the original is opened without another one - as OpenLive does. The
    // neural file is always probed: its duration is what proves it whole.
    bool Open(const std::filesystem::path& originalPath,
              const std::filesystem::path& neuralPath = {},
              std::stop_token stop = {},
              SynchronizedRange range = {},
              bool preferNv12 = false,
              const VideoDecoder::KnownMedia& originalMedia = {});
    // Plays the original against a render job that is still running: the neural
    // member is the growing segment index instead of one finished file.
    // `originalMedia` is the caller's own probe of `originalPath` - the player
    // has one open already, and probing the same file again costs a child
    // process the attach is waiting on.
    bool OpenLive(const std::filesystem::path& originalPath,
                  std::shared_ptr<const NeuralSegmentIndex> segments,
                  SynchronizedRange range,
                  std::stop_token stop = {},
                  const VideoDecoder::KnownMedia& originalMedia = {},
                  bool preferNv12 = false);
    SynchronizedRange Range() const;
    void Close();
    SynchronizedReadResult ReadNextAvailable(std::stop_token stop = {});
    bool SeekSeconds(double seconds, std::stop_token stop = {});
    bool SetView(ComparisonView view);
    ComparisonView View() const;
    const VideoFrame* VisibleFrame() const;
    const SynchronizedFramePair* CurrentPair() const;
    // The same pair, for a caller that needs it to OUTLIVE the next read.
    // CurrentPair()'s pointer is invalidated by ReadNextAvailable; this is not,
    // which is what lets PlayerApp retain the presented pair without copying
    // two full frames out of it on every presented frame.
    std::shared_ptr<const SynchronizedFramePair> CurrentPairShared() const;
    void SetPaused(bool paused);
    bool Paused() const;
    bool Step();
    bool NeuralAvailable() const;
    bool Live() const;
    int64_t LiveHead100ns() const;
    // Whether a live segment decoder has `path` open, or is opening it on
    // another thread. A file retired from the index is deleted only once this
    // is false: until then a decoder may still be reading it.
    bool HoldsFile(const std::filesystem::path& path) const;
    // Why the last read reported OutOfSync, empty when nothing failed.
    std::string LastFault() const;

private:
    bool SeekLive(double seconds, std::stop_token stop);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
