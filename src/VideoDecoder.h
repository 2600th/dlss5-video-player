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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <optional>
#include <utility>
#include <thread>
#include "MediaSource.h"
#include "FrameIdentity.h"

// Which hardware decode paths are known dead for a given ffprobe codec/pixel
// profile. Defined in the .cpp: callers only ever hold one, never look inside.
class AccelerationMemo;
// A memo of its own, for a caller that must neither inherit the process-wide
// memory nor publish into it.
std::shared_ptr<AccelerationMemo> MakeAccelerationMemo();

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
    enum class FailureStage {
        None,
        ProbeResume,
        DecodeResume,
    };

    // Where the helper executables are found, how long a probe and a stalled
    // network read may run, and which process resume is forced to fail. Every
    // one of those is either an environment fact or a Win32 call that cannot be
    // made to fail on demand, so a caller that has to exercise the recovery they
    // guard says so here. Default-constructed is production.
    struct Settings {
        // Empty: search next to the module, then PATH.
        std::wstring helperDirectory;
        // Bounds every ffprobe child this decoder spawns, local file or stream
        // alike: a header read that takes longer than this is a wedged child,
        // not a slow disk (an antivirus-scanned probe measures ~0.7 s).
        std::chrono::milliseconds probeTimeout{15000};
        // Network reads only: a local pipe waits in the kernel instead.
        std::chrono::milliseconds stallTimeout{15000};

        // Skipping a ResumeThread strands the spawned helper suspended, so a
        // non-default value here leaks a child process. This is not
        // configuration: it exists so the resume-failure recovery can be reached
        // at all, and it is nested so that reaching it has to be deliberate.
        struct FaultInjection {
            FailureStage resume{FailureStage::None};
        };
        FaultInjection faults;

        // Null: consult the memo shared by every decoder in this process. A
        // dead path proven once should not be re-proven by the next decoder,
        // and re-proving costs a spawned child per open, so sharing is the
        // default and a private memo is the exception.
        std::shared_ptr<AccelerationMemo> accelerationMemo;
    };

    VideoDecoder() = default;
    explicit VideoDecoder(Settings settings) : m_networkStallTimeout(settings.stallTimeout),
        m_probeTimeout(settings.probeTimeout),m_helperDirectory(std::move(settings.helperDirectory)),
        m_failureStage(settings.faults.resume),
        m_accelerationMemo(std::move(settings.accelerationMemo)) {}
    ~VideoDecoder();

    // preferNv12=true asks for the NV12 source layout on a PLAYBACK open, which
    // is a throughput decision and nothing else: 5.5 MB instead of 14.7 MB per
    // 2560x1440 frame down the pipe, converted by the same GPU pass the export
    // path already uses. Measured on an RTX 5090, two concurrent 2560x1440
    // decoders - which is what one neural pair costs - went 137.7 fps each to
    // 316.0 fps each, or 14.5 ms per pair to 6.3 ms against an 8.34 ms budget
    // at 119.88 fps. It is a REQUEST: odd geometry and any colour description
    // the GPU conversion does not implement still decode to BGRA.
    bool Open(const std::wstring& path,
              MediaSourceKind sourceKind = MediaSourceKind::LocalFile,
              std::stop_token stop = {}, bool preferNv12 = false);
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
        // What the sibling DECLARED about its colour. A `known` open runs no
        // probe, so without this it declares nothing and can never take the
        // NV12 path - which is most of the cost of a neural pair, since the
        // segment member is opened this way at every boundary. Carrying it is
        // safe precisely because it is carried rather than assumed: the caller
        // copies it off a file it probed, and SourceNv12ConversionFor still
        // refuses any description the GPU conversion does not implement.
        SourceColorDescription color;
        bool Valid() const { return width != 0 && height != 0 && fps > 0.0; }
    };
    bool OpenKnown(const std::wstring& path, const KnownMedia& media,
                   MediaSourceKind sourceKind = MediaSourceKind::LocalFile,
                   std::stop_token stop = {}, bool preferNv12 = false);
    // What a sibling file of the one this decoder has open can be opened with.
    KnownMedia Media() const { return {m_source.width, m_source.height, m_source.fps, m_source.durationSec, m_source.hardwareProfile, m_source.color}; }
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
    // Whole-frame buffers the read path had to allocate (and zero-fill) because
    // the pool had none, since the frame queue last started. Once playback is
    // under way this should stop growing; a count that grows with every frame
    // is a caller that is not handing its buffers back.
    uint64_t FrameBufferFills() const { return m_frameBufferFills.load(std::memory_order_relaxed); }

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

    uint32_t Width() const { return m_source.width; }
    uint32_t Height() const { return m_source.height; }
    uint32_t NativeWidth() const { return m_source.nativeWidth ? m_source.nativeWidth : m_source.width; }
    uint32_t NativeHeight() const { return m_source.nativeHeight ? m_source.nativeHeight : m_source.height; }
    double FrameRate() const { return m_source.fps; }
    double DurationSeconds() const { return m_source.durationSec; }
    bool IsStillImage() const { return m_source.stillImage; }
    bool IsAnimation() const { return m_source.gif; }
    // Whether FrameRate() is the source's own rate or ProbeFFmpeg's 30.0
    // fallback for a stream that reported neither avg_frame_rate nor
    // r_frame_rate. FrameRate() cannot be asked: it is clamped to 1..240,
    // overridden for stills and GIFs and never zero, so a fabricated 30.0 reads
    // exactly like a real 30 fps source. False with no media open.
    bool FrameRateKnown() const { return m_source.avgFrameRate > 0.0 || m_source.nominalFrameRate > 0.0; }
    // Whether the source has one fixed cadence: ffprobe's avg_frame_rate
    // (frames over duration) and r_frame_rate (the rate the container declares)
    // agree within frame_rate_policy::kRateTolerance - 0.005, the same 0.5% the
    // policy matches rates with, which covers the 1000/1001 NTSC offset. They
    // diverge on a variable-frame-rate recording, whose single FrameRate()
    // number is an average no individual frame is spaced at, so FrameRate()
    // alone cannot tell the two apart. False when only one of the two rates was
    // reported (nothing to corroborate it with), false for an unknown rate,
    // false for a single-frame still image (which has no cadence at all -
    // FrameRateKnown() is the signal about its rate), and false with no media
    // open.
    bool ConstantFrameRate() const;
    double DisplayAspectRatio() const { return m_source.displayAspect > 0.0 ? m_source.displayAspect : (m_source.height ? double(m_source.width)/double(m_source.height) : 16.0/9.0); }
    const std::wstring& Path() const { return m_path; }
    bool Ready() const { return m_backend != Backend::None && m_source.width != 0 && m_source.height != 0; }
    // Bgra unless the open ASKED for Nv12 - OpenSequential(preferNv12=true, its
    // default) or a playback Open/OpenKnown(preferNv12=true) - and then only
    // when the geometry is even and the stream declared a colour description the
    // GPU conversion implements. Odd geometry and an undeclared or unsupported
    // description stay Bgra whoever asked. Fixed once
    // OpenFFmpeg's probe completes and unchanged by acceleration fallbacks or
    // seek restarts for the rest of the session.
    VideoPixelLayout PixelLayout() const { return m_source.layout; }
    // What the source stream declared about its own colour, as read by the same
    // ffprobe call that produced the geometry above. Every field is Unspecified
    // for a stream that declares nothing, for a `known` open (which runs no
    // probe), and for a Media Foundation open - MF's reader is configured for
    // RGB32/ARGB32 output, so it hands out BGRA and is never the backend behind
    // an NV12 layout. Unspecified is the refusal, never an assumed BT.709.
    const SourceColorDescription& ColorDescription() const { return m_source.color; }

private:
    // Everything the open source is known by - what ffprobe, a KnownMedia or
    // Media Foundation's reader said about the stream, plus the layout this
    // session decodes it to. One aggregate so that an Open resets it, Swap
    // swaps it and Media() reads it as a unit: a field added here is covered by
    // all three without anyone having to enumerate it in Swap, which is how the
    // colour description, the memo key and the NV12 decision were left behind.
    struct Source {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t nativeWidth = 0;
        uint32_t nativeHeight = 0;
        int32_t stride = 0;
        double fps = 30.0;
        // The rates the source itself reported, 0.0 for one it did not, kept
        // beside fps because fps is the number the decoder paces with - clamped,
        // rewritten to 1.0 for a still and 100.0 for a GIF, and 30.0 when the
        // probe found nothing - and so cannot answer whether the source stated a
        // rate or whether it states the same one twice.
        double avgFrameRate = 0.0;      // stream=avg_frame_rate: frames over duration
        double nominalFrameRate = 0.0;  // stream=r_frame_rate: the container's declared cadence
        // What the packet timestamps say about the spacing, which outranks
        // both rates above when it has an answer. See VariableFrameRatePolicy.h
        // for why neither declared rate can be trusted on the sources that
        // matter. spacingDecided is false when the probe could not sample
        // enough packets, and then the two declared rates are all there is.
        bool spacingDecided = false;
        bool spacingConstant = true;
        double durationSec = 0.0;
        double displayAspect = 0.0;
        bool stillImage = false;
        bool gif = false;
        // ffprobe's codec/pixel format for the open source. Hardware decode support
        // is per codec, so the memo of dead paths is keyed by this, never global.
        std::string hardwareProfile;
        SourceColorDescription color{};
        // The four colour entries exactly as ffprobe printed them, for the log line
        // that refuses the GPU conversion. The mapped description is what the code
        // gates on, but "other" is a diagnosis nobody can act on and "bt2020nc" is.
        std::string colorTags;
        // OpenSequential(true) vs Open(false); decides layout once the probe
        // knows the geometry. Fixed for the session: neither TryNextFFmpegAcceleration
        // nor a seek restart re-probes, so they never revisit it.
        bool sequentialOpen = false;
        // OpenSequential's preferNv12 argument (irrelevant when sequentialOpen is
        // false); false pins layout to Bgra even for even geometry. Reset with the
        // rest on every open, so it never leaks from one OpenSequential into a
        // later Open() or OpenSequential(preferNv12=true).
        bool sequentialNv12 = true;
        // Whichever open asked for NV12 - OpenSequential by its own argument, a
        // playback Open/OpenKnown by preferNv12. One flag so the gate below has
        // one question to ask, and reset with the rest on every open.
        bool nv12Requested = false;
        // True only when a PLAYBACK open asked. The gate is narrower there than
        // for an export open - see the comment beside playbackConvertible - and
        // the decision is taken in a different function from OpenImpl, so it
        // travels in the aggregate rather than as an argument.
        bool playbackNv12Requested = false;
        VideoPixelLayout layout = VideoPixelLayout::Bgra;
    };
    Source m_source;
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
                  const KnownMedia* known = nullptr, bool playbackNv12 = false);

    bool OpenFFmpeg(const std::wstring& path, std::stop_token stop,
                    FFmpegAcceleration initialAcceleration,
                    const KnownMedia* known);
    bool ProbeFFmpeg(const std::wstring& path, std::stop_token stop);
    void ProbePacketSpacing(const std::wstring& path, const std::wstring& inputOptions,
                            std::stop_token stop);
    // Restarts (seeks, resizes, recovery) default to the path that last produced
    // frames instead of re-running a hardware chain that already failed.
    bool StartFFmpeg(double seekSeconds,
                     std::optional<FFmpegAcceleration> acceleration = std::nullopt);
    // The memo this decoder consults: its own when one was injected, the
    // process-wide one otherwise.
    AccelerationMemo& AccelerationMemory() const;
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
    void FrameQueueThread(std::stop_token stop) noexcept;
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
    uint32_t m_sourceGeneration = 0;
    bool m_restartDiscontinuity = false;
    MediaSourceKind m_sourceKind = MediaSourceKind::LocalFile;
    std::vector<uint8_t> m_pendingFrame;
    size_t m_pendingFrameBytes = 0;
    std::chrono::steady_clock::time_point m_lastFrameByte{};
    std::chrono::milliseconds m_networkStallTimeout{15000};
    std::chrono::milliseconds m_probeTimeout{15000};
    static constexpr size_t FrameQueueCapacity = 4;
    // One per queue slot. A consumer that catches up drains the whole queue in one
    // burst - a late frame being dropped, a pair skipped by the cadence - and hands
    // back that many buffers before the queue thread takes any. A pool of two
    // dropped half of them and the refill allocated them again: measured 120 fills
    // over 240 frames read in bursts of four, against 5 with this capacity. The
    // buffers it keeps are ones the full queue was already holding.
    static constexpr size_t FrameBufferPoolCapacity = FrameQueueCapacity;
    // Held by the queue thread and by whichever thread returns a spent buffer, so it
    // is deliberately not m_frameMutex: recycling never waits on the queue.
    std::mutex m_bufferPoolMutex;
    std::vector<std::vector<uint8_t>> m_bufferPool;
    // Whole-frame allocations the pool did not cover. Written by whichever
    // thread reads the pipe, read by FrameBufferFills() from any other.
    std::atomic<uint64_t> m_frameBufferFills{0};
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
    // Empty: ffprobe/ffmpeg are located next to the module, then on PATH.
    std::wstring m_helperDirectory;
    FailureStage m_failureStage{FailureStage::None};
    std::shared_ptr<AccelerationMemo> m_accelerationMemo;
};
