#pragma once
#include <windows.h>

#include "AudioClockPolicy.h"
#include "WasapiRenderer.h"
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
            // Runs the whole pipe-reading path with no render endpoint opened,
            // which is how the process and job-object teardown is reached on a
            // machine that does have one.
            bool disableAudioDevice{false};
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
    // Buffers handed to the endpoint since Start; 0 when stopped.
    uint64_t SubmittedBuffers() const;

    // Restarts audio on the new default endpoint if the old one went away -
    // headphones unplugged, a default-device change, a driver restart. Cheap
    // when nothing happened, so the player calls it from its frame tick.
    //
    // waveOut had no equivalent: a write to a departed endpoint failed, the
    // reader thread broke, and the film played on in silence for the rest of
    // the session.
    bool ServiceDeviceChanges();

private:
    struct ReaderState {
        HANDLE process = nullptr;
        HANDLE stdoutPipe = nullptr;
        HANDLE job = nullptr;
        HANDLE completed = nullptr;
        // Null when the device is disabled by fault injection: the pipe is
        // still read and the process still torn down, there is just nowhere
        // for the samples to go.
        std::unique_ptr<WasapiRenderer> renderer;
        // Copied out of the renderer so the clock can be read without taking
        // its lock behind the UI thread.
        uint32_t sampleRate = 0;
        std::atomic<bool> stop{false};
        std::atomic<bool> paused{false};
        std::atomic<bool> hasAudioData{false};
        std::atomic<uint64_t> submittedBuffers{0};
        // Latched when the endpoint is invalidated - an unplugged pair of
        // headphones, a default-device change. The owner restarts the whole
        // pipeline on the new endpoint, because its mix format may differ and
        // ffmpeg has to be told.
        std::atomic<bool> deviceLost{false};
        bool disableAudioDevice = false;
        ~ReaderState();
    };

    std::wstring FindFFmpeg() const;
    bool StartProcess(double seekSeconds, const std::shared_ptr<ReaderState>& state,
                      const WasapiRenderer::Format& format);
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
    // PositionSeconds is const and is the only place that can notice the clock
    // has stopped, so the staleness it tracks is mutable. Guarded because the
    // position is read from more than the thread that starts and stops it.
    mutable std::mutex m_clockMutex;
    mutable audio_clock::StallState m_clock;
    mutable bool m_clockStalled = false;
    // The last position the clock actually answered with. A lost endpoint
    // stops answering, so this is where playback resumes from.
    mutable std::atomic<double> m_lastKnownPosition{0.0};
};
