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
    // Where the helper process is found, plus the faults a caller injects to
    // reach recovery that cannot be reached otherwise. Default-constructed is
    // production: ffmpeg.exe is located next to the module, WaveOut is opened,
    // and every wait is real.
    struct Settings {
        // Empty: search next to the module, then PATH.
        std::wstring helperDirectory;

        // Every field here disables something the player depends on - the audio
        // device, or a Win32 call the shutdown needs - so a non-default value
        // means no sound, or a live ffmpeg child and an unkilled job object left
        // behind. None of it is configuration: these exist so the recovery those
        // calls guard can be reached at all, and they are nested so that reaching
        // them has to be deliberate.
        struct FaultInjection {
            bool disableWaveOut{false};
            bool failTerminateJob{false};
            bool failInitialProcessWait{false};
            bool failGetExitCodeProcess{false};
            bool failFinalProcessWait{false};
            bool failInitialReaderWait{false};
            bool failFinalReaderWait{false};
        };
        FaultInjection faults;
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
