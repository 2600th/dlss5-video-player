#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <cstdint>
#include <string>
#include <vector>

struct VideoFrame {
    std::vector<uint8_t> bgra;
    int64_t timestamp100ns = 0;
    bool discontinuity = false;
};

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    bool Open(const std::wstring& path);
    void Close();
    bool ReadNext(VideoFrame& out);
    bool SeekSeconds(double seconds);
    bool SetDecodeSize(uint32_t width, uint32_t height);

    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    uint32_t NativeWidth() const { return m_nativeWidth ? m_nativeWidth : m_width; }
    uint32_t NativeHeight() const { return m_nativeHeight ? m_nativeHeight : m_height; }
    double FrameRate() const { return m_fps; }
    double DurationSeconds() const { return m_durationSec; }
    double DisplayAspectRatio() const { return m_displayAspect > 0.0 ? m_displayAspect : (m_height ? double(m_width)/double(m_height) : 16.0/9.0); }
    const std::wstring& Path() const { return m_path; }
    const wchar_t* BackendName() const;

private:
    enum class Backend { None, FFmpeg, MediaFoundation };

    bool OpenFFmpeg(const std::wstring& path);
    bool ProbeFFmpeg(const std::wstring& path);
    bool StartFFmpeg(double seekSeconds);
    bool ReadNextFFmpeg(VideoFrame& out);
    void StopFFmpeg();

    bool OpenMediaFoundation(const std::wstring& path);
    bool ReadNextMediaFoundation(VideoFrame& out);

    static std::wstring FindTool(const wchar_t* exeName);
    static bool RunCapture(const std::wstring& exe, const std::wstring& arguments,
                           std::string& output, DWORD* exitCode = nullptr);

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
    uint64_t m_ffmpegFrameIndex = 0;
    int64_t m_ffmpegSeekBase100ns = 0;
};
