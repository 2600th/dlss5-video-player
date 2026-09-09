#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stop_token>
#include <optional>
#include <utility>
#include <thread>
#include "UiLayout.h"
#include "FrameIdentity.h"

#ifdef VIDEO_DECODER_TESTING
struct VideoDecoderTestAccess;
#endif

struct VideoFrame {
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
    bool OpenSequential(const std::wstring& path,
                        MediaSourceKind sourceKind = MediaSourceKind::LocalFile,
                        std::stop_token stop = {});
    void Close();
    bool ReadNext(VideoFrame& out);
    VideoReadResult ReadNextAvailable(VideoFrame& out, std::stop_token stop = {});
    VideoReadResult ReadNextBlocking(VideoFrame& out, std::stop_token stop = {});
    bool SeekSeconds(double seconds);
    void Swap(VideoDecoder& other) noexcept;

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
                  FFmpegAcceleration acceleration = FFmpegAcceleration::Cuda);

    bool OpenFFmpeg(const std::wstring& path, std::stop_token stop,
                    FFmpegAcceleration initialAcceleration);
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
    VideoReadResult ReadNextFFmpegAvailable(VideoFrame& out, std::stop_token stop);
    VideoReadResult ReadNextFFmpegProcessAvailable(VideoFrame& out, std::stop_token stop);
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
    uint32_t m_sourceGeneration = 0;
    bool m_restartDiscontinuity = false;
    MediaSourceKind m_sourceKind = MediaSourceKind::LocalFile;
    std::vector<uint8_t> m_pendingFrame;
    size_t m_pendingFrameBytes = 0;
    std::chrono::steady_clock::time_point m_lastFrameByte{};
    std::chrono::milliseconds m_networkStallTimeout{15000};
    std::chrono::milliseconds m_probeTimeout{15000};
    static constexpr size_t FrameQueueCapacity = 4;
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
