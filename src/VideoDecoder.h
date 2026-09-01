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
#include <utility>
#include <thread>
#include "UiLayout.h"

#ifdef VIDEO_DECODER_TESTING
struct VideoDecoderTestAccess;
#endif

struct VideoFrame {
    std::vector<uint8_t> bgra;
    int64_t timestamp100ns = 0;
    bool discontinuity = false;
};

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
    void Close();
    bool ReadNext(VideoFrame& out);
    VideoReadResult ReadNextAvailable(VideoFrame& out, std::stop_token stop = {});
    bool SeekSeconds(double seconds);
    bool SetDecodeSize(uint32_t width, uint32_t height);
    void Swap(VideoDecoder& other) noexcept;

    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    uint32_t NativeWidth() const { return m_nativeWidth ? m_nativeWidth : m_width; }
    uint32_t NativeHeight() const { return m_nativeHeight ? m_nativeHeight : m_height; }
    double FrameRate() const { return m_fps; }
    double DurationSeconds() const { return m_durationSec; }
    double DisplayAspectRatio() const { return m_displayAspect > 0.0 ? m_displayAspect : (m_height ? double(m_width)/double(m_height) : 16.0/9.0); }
    const std::wstring& Path() const { return m_path; }
    bool Ready() const { return m_backend != Backend::None && m_width != 0 && m_height != 0; }
    const wchar_t* BackendName() const;

private:
    enum class Backend { None, FFmpeg, MediaFoundation };
    enum class FFmpegAcceleration { Cuda, D3D11Va, Software };

    bool OpenFFmpeg(const std::wstring& path, std::stop_token stop);
    bool ProbeFFmpeg(const std::wstring& path, std::stop_token stop);
    bool StartFFmpeg(double seekSeconds,
                     FFmpegAcceleration acceleration = FFmpegAcceleration::Cuda);
    bool ReadNextFFmpeg(VideoFrame& out);
    VideoReadResult ReadNextFFmpegAvailable(VideoFrame& out, std::stop_token stop);
    VideoReadResult ReadNextFFmpegProcessAvailable(VideoFrame& out, std::stop_token stop);
    bool TryNextFFmpegAcceleration(DWORD exitCode);
    void StopFFmpeg(DWORD waitTimeout = 500);
    void StartFrameQueue();
    void StopFrameQueue();
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
    uint64_t m_ffmpegFrameIndex = 0;
    int64_t m_ffmpegSeekBase100ns = 0;
    FFmpegAcceleration m_ffmpegAcceleration = FFmpegAcceleration::Software;
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
