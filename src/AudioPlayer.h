#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <string>
#include <thread>
#include <mutex>
#include <cstdint>
#include <memory>
#include <utility>

enum class AudioStartState {
    Playing,
    Paused,
};

class AudioPlayer {
public:
    // Where the helper process is found, and which of the bounded waits on the
    // stop path are forced to fail. Every one of those is a Win32 call that
    // cannot be made to fail on demand, so a caller that has to exercise the
    // recovery they guard says so here. Default-constructed is production:
    // ffmpeg.exe is located next to the module, WaveOut is opened, and every
    // wait is real.
    struct Settings {
        // Empty: search next to the module, then PATH.
        std::wstring helperDirectory;
        bool disableWaveOut{false};
        bool failTerminateJob{false};
        bool failInitialProcessWait{false};
        bool failGetExitCodeProcess{false};
        bool failFinalProcessWait{false};
        bool failInitialReaderWait{false};
        bool failFinalReaderWait{false};
    };

    AudioPlayer() = default;
    explicit AudioPlayer(Settings settings) : m_settings(std::move(settings)) {}
    ~AudioPlayer();

    bool Start(const std::wstring& videoPath, double seekSeconds = 0.0,
               AudioStartState state = AudioStartState::Playing);
    bool Seek(double seconds);
    void Pause(bool paused);
    void SetVolume(float volume01);
    float Volume() const { return m_volume; }
    void Stop();
    bool Active() const;
    bool HasAudioData() const;
    bool Paused() const;
    double PositionSeconds() const;
    // Seek position the current helper process was started at.
    double SeekBaseSeconds() const { return m_seekBaseSec; }
    // WaveOut buffers handed to the device since Start; 0 when stopped.
    uint64_t SubmittedBuffers() const;

private:
    struct ReaderState {
        HANDLE process = nullptr;
        HANDLE stdoutPipe = nullptr;
        HANDLE job = nullptr;
        HANDLE completed = nullptr;
        HWAVEOUT waveOut = nullptr;
        mutable std::mutex waveMutex;
        std::atomic<bool> stop{false};
        std::atomic<bool> paused{false};
        std::atomic<bool> hasAudioData{false};
        std::atomic<uint64_t> submittedBuffers{0};
        bool disableWaveOut = false;
        ~ReaderState();
    };

    std::wstring FindFFmpeg() const;
    bool StartProcess(double seekSeconds, const std::shared_ptr<ReaderState>& state);
    void StopProcess(const std::shared_ptr<ReaderState>& state);
    static void ReaderThread(std::shared_ptr<ReaderState> state) noexcept;
    static void ThreadMain(const std::shared_ptr<ReaderState>& state);

    std::wstring m_path;
    std::wstring m_ffmpeg;
    std::shared_ptr<ReaderState> m_reader;
    std::thread m_thread;
    double m_seekBaseSec = 0.0;
    float m_volume = 1.0f;
    Settings m_settings;
};
