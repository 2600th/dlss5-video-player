#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <string>
#include <thread>
#include <mutex>
#include <cstdint>
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
    bool Active() const { return m_waveOut != nullptr; }
    bool HasAudioData() const { return m_hasAudioData.load(); }
    bool Paused() const { return m_paused.load(); }
    double PositionSeconds() const;

private:
    std::wstring FindFFmpeg() const;
    bool StartProcess(double seekSeconds);
    void StopProcess();
    void ThreadMain();

    std::wstring m_path;
    std::wstring m_ffmpeg;
    HANDLE m_process = nullptr;
    HANDLE m_stdout = nullptr;
    HANDLE m_job = nullptr;
    HWAVEOUT m_waveOut = nullptr;
    std::thread m_thread;
    mutable std::mutex m_waveMutex;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_hasAudioData{false};
    std::atomic<uint64_t> m_submittedBuffers{0};
    double m_seekBaseSec = 0.0;
    float m_volume = 1.0f;
    bool m_disableWaveOut = false;
    bool m_failTerminateJob = false;
    bool m_forceProcessWaitTimeout = false;
#ifdef AUDIO_PLAYER_TESTING
    struct Settings {
        std::wstring helperDirectory;
        bool disableWaveOut{false};
        bool failTerminateJob{false};
        bool forceProcessWaitTimeout{false};
    };
    explicit AudioPlayer(Settings settings) : m_helperDirectory(std::move(settings.helperDirectory)),
        m_disableWaveOut(settings.disableWaveOut),m_failTerminateJob(settings.failTerminateJob),
        m_forceProcessWaitTimeout(settings.forceProcessWaitTimeout) {}
    friend struct AudioPlayerTestAccess;
    std::wstring m_helperDirectory;
#endif
};
