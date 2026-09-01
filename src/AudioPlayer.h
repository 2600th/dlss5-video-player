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

#ifdef AUDIO_PLAYER_TESTING
struct AudioPlayerTestAccess;
#endif

enum class AudioStartState {
    Playing,
    Paused,
};

class AudioPlayer {
public:
    AudioPlayer() = default;
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
    bool m_disableWaveOut = false;
    bool m_failTerminateJob = false;
    bool m_failInitialProcessWait = false;
    bool m_failGetExitCodeProcess = false;
    bool m_failFinalProcessWait = false;
    bool m_failInitialReaderWait = false;
    bool m_failFinalReaderWait = false;
#ifdef AUDIO_PLAYER_TESTING
    struct Settings {
        std::wstring helperDirectory;
        bool disableWaveOut{false};
        bool failTerminateJob{false};
        bool failInitialProcessWait{false};
        bool failGetExitCodeProcess{false};
        bool failFinalProcessWait{false};
        bool failInitialReaderWait{false};
        bool failFinalReaderWait{false};
    };
    explicit AudioPlayer(Settings settings) : m_helperDirectory(std::move(settings.helperDirectory)),
        m_disableWaveOut(settings.disableWaveOut),m_failTerminateJob(settings.failTerminateJob),
        m_failInitialProcessWait(settings.failInitialProcessWait),
        m_failGetExitCodeProcess(settings.failGetExitCodeProcess),
        m_failFinalProcessWait(settings.failFinalProcessWait),
        m_failInitialReaderWait(settings.failInitialReaderWait),
        m_failFinalReaderWait(settings.failFinalReaderWait) {}
    friend struct AudioPlayerTestAccess;
    std::wstring m_helperDirectory;
#endif
};
