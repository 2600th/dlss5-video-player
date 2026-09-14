#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <cstdint>
#include <string>
#include <vector>
#include "PixelLayout.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stop_token>
#include <optional>
#include <utility>
#include <thread>
#include "MediaSource.h"
#include "FrameIdentity.h"

#ifdef VIDEO_DECODER_TESTING
struct VideoDecoderTestAccess;
#endif

// PixelLayout.h: BGRA is 4 bytes/pixel; NV12 is 3/2 - 11.1 MB BGRA -> 4.2 MB NV12 per
// 2578x1080 frame. OpenSequential (the neural export's source) selects Nv12 when the
// geometry allows it; Open (normal playback) always stays Bgra. A caller of
// OpenSequential can opt out of Nv12 (preferNv12=false) to stay Bgra even for even
// geometry, e.g. when the neural render needs ffmpeg's CPU conversion instead of
// spending GPU time on it.
using VideoPixelLayout = PixelLayout;
inline size_t FrameBytes(VideoPixelLayout layout, uint32_t w, uint32_t h) {
    return PixelLayoutFrameBytes(layout, w, h);
}

struct VideoFrame {
    // Holds the frame in `layout` - BGRA (w*h*4 bytes) or NV12 (w*h*3/2
    // bytes: Y plane then interleaved UV), per FrameBytes(layout, w, h).
    std::vector<uint8_t> bgra;
    int64_t timestamp100ns = 0;
    bool discontinuity = false;
    // Position on the decoder's constant-frame-rate timeline:
    // llround(timestamp100ns * fps / 1e7). Identical between an original and
    // its neural render because both decode the same CFR timeline.
    uint64_t frameNumber = 0;
    // Bumped by Open/OpenSequential, SeekSeconds and any internal decoder
    // restart, so frames from different decoder sessions never pair.
    uint32_t sourceGeneration = 0;
    // Trailing (not after bgra) so existing positional-brace VideoFrame{...}
    // initializers that predate NV12 support keep compiling unchanged.
    VideoPixelLayout layout = VideoPixelLayout::Bgra;
};

inline FrameIdentity IdentityOf(const VideoFrame& frame, uint32_t historyGeneration,
                                uint64_t jobId, HistoryReset reset)
{
    return FrameIdentity{frame.frameNumber, frame.timestamp100ns, frame.sourceGeneration,
                         historyGeneration, jobId, reset};
}

enum class VideoReadResult {
    FrameReady,
    NotReady,
    EndOfStream,
    Error,
    Stalled,
    Cancelled,
};

class VideoDecoder {
public:
#ifdef VIDEO_DECODER_TESTING
    enum class FailureStage {
        None,
        ProbeResume,
        DecodeResume,
    };
#endif

    VideoDecoder() = default;
    ~VideoDecoder();

    bool Open(const std::wstring& path,
              MediaSourceKind sourceKind = MediaSourceKind::LocalFile,
              std::stop_token stop = {});
    // preferNv12=false keeps the output Bgra (ffmpeg converts on the CPU) even for
    // even geometry, for a caller whose neural render needs the GPU for something
    // more scarce than the NV12->BGRA conversion.
    bool OpenSequential(const std::wstring& path,
                        MediaSourceKind sourceKind = MediaSourceKind::LocalFile,
                        std::stop_token stop = {},
                        bool preferNv12 = true);
    // Parameters of a file this process just produced, so opening it does not
    // pay for a probe. ffprobe is a child process: on a machine whose antivirus
    // scans one, a probe costs ~0.7 s (measured 684 ms against 32 ms in an
    // excluded directory), and a live neural session opens one segment file
    // every two seconds on the thread that presents frames.
    struct KnownMedia {
        uint32_t width{};
        uint32_t height{};
        double fps{};
        double durationSec{};
        // Acceleration memo key of the sibling the parameters came from. All
        // segments of one render carry one codec, so they share one key.
        std::string hardwareProfile;
        bool Valid() const { return width != 0 && height != 0 && fps > 0.0; }
    };
    bool OpenKnown(const std::wstring& path, const KnownMedia& media,
                   MediaSourceKind sourceKind = MediaSourceKind::LocalFile,
                   std::stop_token stop = {});
    // What a sibling file of the one this decoder has open can be opened with.
    KnownMedia Media() const { return {m_width, m_height, m_fps, m_durationSec, m_hardwareProfile}; }
    // Geometry, frame rate and duration only: runs the probe and starts no
    // decoder. The caller that just needs to describe a file was paying for a
    // full ffmpeg child it closed two lines later.
    bool OpenMetadata(const std::wstring& path,
                      MediaSourceKind sourceKind = MediaSourceKind::LocalFile,
                      std::stop_token stop = {});
    void Close();
    bool ReadNext(VideoFrame& out);
    VideoReadResult ReadNextAvailable(VideoFrame& out, std::stop_token stop = {});
    VideoReadResult ReadNextBlocking(VideoFrame& out, std::stop_token stop = {});
    bool SeekSeconds(double seconds);
    void Swap(VideoDecoder& other) noexcept;
    // Hands a fully consumed BGRA buffer back for a later pipe read. Without it the
    // read path allocates and zero-fills a frame per frame - 31.6 MiB at 4K, written
    // twice because the pipe overwrites every byte of it immediately after.
    // Buffers of the wrong size, and any past the pool's cap, are simply dropped.
    void RecycleFrameBuffer(std::vector<uint8_t>&& buffer);

    // Where a seek's latency actually goes. Published per seek because the seek
    // path is the only place the player blocks on a decoder restart, and the
    // reuse-versus-restart trade below is only defensible while it stays
    // measurable from tests and benchmarks, not just from the log line.
    struct SeekTiming {
        double teardownMs = 0.0;     // terminating the previous ffmpeg child
        double spawnMs = 0.0;        // CreateProcessW + job assignment + resume
        double firstByteMs = -1.0;   // seek entry -> first raw byte on the pipe
        double firstFrameMs = -1.0;  // seek entry -> first complete frame decoded
        double callMs = 0.0;         // SeekSeconds() itself
        // Frames the kept child still had to produce before the target.
        uint64_t drainedFrames = 0;
        // Frames between the position the caller would read next and the
        // target: negative means the seek had to rewind.
        int64_t forwardFrames = 0;
        bool reusedChild = false;
    };
    SeekTiming LastSeekTiming() const;

    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    uint32_t NativeWidth() const { return m_nativeWidth ? m_nativeWidth : m_width; }
    uint32_t NativeHeight() const { return m_nativeHeight ? m_nativeHeight : m_height; }
    double FrameRate() const { return m_fps; }
    double DurationSeconds() const { return m_durationSec; }
    bool IsStillImage() const { return m_stillImage; }
    bool IsAnimation() const { return m_gif; }
    double DisplayAspectRatio() const { return m_displayAspect > 0.0 ? m_displayAspect : (m_height ? double(m_width)/double(m_height) : 16.0/9.0); }
    const std::wstring& Path() const { return m_path; }
    bool Ready() const { return m_backend != Backend::None && m_width != 0 && m_height != 0; }
    const wchar_t* BackendName() const;
    // Bgra for Open (always), for OpenSequential(preferNv12=false), and for
    // OpenSequential when the geometry can't take NV12 (odd width/height); Nv12
    // for OpenSequential(preferNv12=true, the default) otherwise. Fixed once
    // OpenFFmpeg's probe completes and unchanged by acceleration fallbacks or
    // seek restarts for the rest of the session.
    VideoPixelLayout PixelLayout() const { return m_layout; }

private:
    // ffprobe's codec/pixel format for the open source. Hardware decode support
    // is per codec, so the memo of dead paths is keyed by this, never global.
    std::string m_hardwareProfile;
    bool m_stillImage{false};
    bool m_gif{false};
    enum class Backend { None, FFmpeg, MediaFoundation };
    enum class FFmpegAcceleration { Cuda, D3D11Va, Software };
    // A restart clears every decoded frame; a seek that keeps its child must
    // keep them, because frames already pulled out of the pipe cannot be read
    // a second time.
    enum class QueueBuffer { Discard, Keep };
    enum class SeekReuse { Reused, Restart };

    bool OpenImpl(const std::wstring& path, MediaSourceKind sourceKind,
                  std::stop_token stop, bool queueFrames,
                  FFmpegAcceleration acceleration = FFmpegAcceleration::Cuda,
                  bool sequential = false, bool sequentialNv12 = true,
                  const KnownMedia* known = nullptr);

    bool OpenFFmpeg(const std::wstring& path, std::stop_token stop,
                    FFmpegAcceleration initialAcceleration,
                    const KnownMedia* known);
    bool ProbeFFmpeg(const std::wstring& path, std::stop_token stop);
    // Restarts (seeks, resizes, recovery) default to the path that last produced
    // frames instead of re-running a hardware chain that already failed.
    bool StartFFmpeg(double seekSeconds,
                     std::optional<FFmpegAcceleration> acceleration = std::nullopt);
#ifdef VIDEO_DECODER_TESTING
public:
    // The dead-path memory is process-wide, so a test that exercises the
    // fallback chain has to start from a clean slate.
    static void ResetAccelerationAvailabilityForTesting();
private:
#endif
    bool ReadNextFFmpeg(VideoFrame& out);
    // Empty when the pool holds nothing of this exact size.
    std::vector<uint8_t> TakeRecycledBuffer(size_t frameBytes);
    VideoReadResult ReadNextFFmpegAvailable(VideoFrame& out, std::stop_token stop);
    // block: wait in the kernel for pipe bytes instead of peek/sleep-polling for them.
    // Only safe on the queue thread, whose parked ReadFile StopFrameQueue releases with
    // CancelSynchronousIo - never from the UI pump's non-blocking callers.
    VideoReadResult ReadNextFFmpegProcessAvailable(VideoFrame& out, std::stop_token stop,
                                                    bool block = false);
    VideoReadResult ClassifyFFmpegEnd(DWORD exitCode);
    bool TryNextFFmpegAcceleration(DWORD exitCode);
    void StopFFmpeg(DWORD waitTimeout = 500);
    // Forward seeks shorter than a restart are served by the running child.
    SeekReuse ReuseRunningChildForSeek(double seconds);
    double FFmpegHeadSeconds() const;
    void PublishSeekTiming(bool frameDelivered);
    void StartFrameQueue(QueueBuffer buffered = QueueBuffer::Discard);
    void StopFrameQueue(QueueBuffer buffered = QueueBuffer::Discard);
    void FrameQueueLoop(std::stop_token stop);

    bool OpenMediaFoundation(const std::wstring& path);
    bool ReadNextMediaFoundation(VideoFrame& out);

    std::wstring FindTool(const wchar_t* exeName) const;
    bool RunCapture(const std::wstring& exe, const std::wstring& arguments,
                    std::string& output, DWORD* exitCode,
                    std::stop_token stop, std::chrono::milliseconds timeout);

    Backend m_backend = Backend::None;
    Microsoft::WRL::ComPtr<IMFSourceReader> m_reader;
    std::wstring m_path;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_nativeWidth = 0;
    uint32_t m_nativeHeight = 0;
    int32_t m_stride = 0;
    double m_fps = 30.0;
    double m_durationSec = 0.0;
    double m_displayAspect = 0.0;

    std::wstring m_ffmpegExe;
    std::wstring m_ffprobeExe;
    HANDLE m_ffmpegProcess = nullptr;
    HANDLE m_ffmpegStdout = nullptr;
    HANDLE m_ffmpegJob = nullptr;
    // Frames this child has emitted, and the true CFR index of its first one.
    // The child emits headerless rawvideo, so the pair is the only position the
    // decoder has - and a forward seek that keeps the child running has to know
    // exactly it to hand out the frame a restart would have produced.
    uint64_t m_ffmpegEmittedFrames = 0;
    int64_t m_ffmpegSpawnFirstFrame = 0;
    int64_t m_ffmpegSeekBase100ns = 0;
    // Source frame that maps to m_ffmpegSeekBase100ns: the timeline origin. A
    // reused seek moves it forward, and the read path drops everything below.
    int64_t m_ffmpegFirstSourceFrame = 0;
    // Seeded from measured 1080p30 CUDA numbers (a restart reaches its first
    // frame in ~230ms, a frame walked past costs ~16ms) and then tracked per
    // decoder: whether skipping beats a restart depends on the file's own
    // decode cost, so a fixed frame threshold would be wrong for half the
    // sources.
    double m_restartFirstFrameMs = 230.0;
    double m_drainMsPerFrame = 16.0;
    SeekTiming m_seekTiming{};
    std::chrono::steady_clock::time_point m_seekStart{};
    double m_seekTargetSeconds = 0.0;
    bool m_seekTimingPending = false;
    bool m_seekReusedBuffered = false;
    mutable std::mutex m_seekTimingMutex;
    FFmpegAcceleration m_ffmpegAcceleration = FFmpegAcceleration::Software;
    // OpenSequential(true) vs Open(false); decides m_layout once the probe
    // knows the geometry. Fixed for the session: neither TryNextFFmpegAcceleration
    // nor a seek restart re-probes, so they never revisit it.
    bool m_sequentialOpen = false;
    // OpenSequential's preferNv12 argument (irrelevant when m_sequentialOpen is
    // false); false pins m_layout to Bgra even for even geometry. Reset on every
    // OpenImpl call the same way m_sequentialOpen is, so it never leaks from one
    // OpenSequential into a later Open() or OpenSequential(preferNv12=true).
    bool m_sequentialNv12 = true;
    VideoPixelLayout m_layout = VideoPixelLayout::Bgra;
    uint32_t m_sourceGeneration = 0;
    bool m_restartDiscontinuity = false;
    MediaSourceKind m_sourceKind = MediaSourceKind::LocalFile;
    std::vector<uint8_t> m_pendingFrame;
    size_t m_pendingFrameBytes = 0;
    std::chrono::steady_clock::time_point m_lastFrameByte{};
    std::chrono::milliseconds m_networkStallTimeout{15000};
    std::chrono::milliseconds m_probeTimeout{15000};
    static constexpr size_t FrameQueueCapacity = 4;
    // Only one read fills a buffer at a time, so a spare and the one in flight are
    // all the pool can use; more would just hold 31.6 MiB each at 4K.
    static constexpr size_t FrameBufferPoolCapacity = 2;
    // Held by the queue thread and by whichever thread returns a spent buffer, so it
    // is deliberately not m_frameMutex: recycling never waits on the queue.
    std::mutex m_bufferPoolMutex;
    std::vector<std::vector<uint8_t>> m_bufferPool;
    // Whole-frame allocations the pool did not cover; queue-thread only.
    uint64_t m_frameBufferFills = 0;
    // Time spent inside the blocking ReadFile (LocalFile queue thread only), reset at
    // the top of each FrameQueueLoop run so the perFrameMs log line can separate the
    // in-kernel wait for the child from the rest of pipeRead.
    std::chrono::steady_clock::duration m_frameBlockedNanos{};
    uint64_t m_frameReadCalls = 0;
    std::mutex m_frameMutex;
    std::condition_variable_any m_frameCv;
    std::deque<VideoFrame> m_frameQueue;
    VideoReadResult m_frameTerminal = VideoReadResult::NotReady;
    bool m_frameQueueEnabled = false;
    std::jthread m_frameThread;
#ifdef VIDEO_DECODER_TESTING
    struct Settings {
        std::wstring helperDirectory;
        std::chrono::milliseconds probeTimeout{15000};
        std::chrono::milliseconds stallTimeout{15000};
        FailureStage failureStage{FailureStage::None};
    };
    explicit VideoDecoder(Settings settings) : m_helperDirectory(std::move(settings.helperDirectory)),
        m_networkStallTimeout(settings.stallTimeout),m_probeTimeout(settings.probeTimeout),
        m_failureStage(settings.failureStage) {}
    friend struct VideoDecoderTestAccess;
    std::wstring m_helperDirectory;
    FailureStage m_failureStage{FailureStage::None};
#endif
};
