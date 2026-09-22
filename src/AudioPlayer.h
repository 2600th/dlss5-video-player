#pragma once
#include <windows.h>

#include "AudioClockPolicy.h"
#include "AudioTrackPolicy.h"
#include "WasapiRenderer.h"
#include <vector>
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

    // Delivers a default-endpoint change to the running renderer through the
    // same handler the OS calls. The audio smoke uses this to exercise
    // device-change recovery end to end - notification, latch, restart,
    // clock resuming - without changing the machine's default playback
    // device out from under whoever is using it. False when nothing is
    // playing. It is the handler, not a test double: the OS delivers the
    // identical call through the registered IMMNotificationClient.
    bool DeliverDefaultEndpointChange(const std::wstring& newDeviceId);

    // The source's audio streams, in container order. Empty when there is
    // one unremarkable track, when ffprobe could not be found, or when the
    // source is a stream the player did not enumerate - in all of which the
    // first stream is played, which is what happened before this existed.
    const std::vector<audio_track::Track>& AudioTracks() const { return m_tracks; }
    // Index among the audio streams, which is what `-map 0:a:N` takes.
    int SelectedAudioTrack() const { return m_selectedTrack; }
    // Switches track and restarts at the current position. False when the
    // index names no track, so a mis-click cannot silence the film.
    bool SelectAudioTrack(int audioIndex);

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

    std::wstring FindTool(const wchar_t* name) const;
    std::wstring FindFFmpeg() const { return FindTool(L"ffmpeg.exe"); }
    // Enumerates the source's audio streams and picks the opening one. Runs
    // once per loaded path: a seek respawns the child but the container has
    // not changed, and re-probing it would add an ffprobe to every seek.
    void ProbeAudioTracks(const std::wstring& videoPath);
    bool StartProcess(double seekSeconds, const std::shared_ptr<ReaderState>& state,
                      const WasapiRenderer::Format& format);
    void StopProcess(const std::shared_ptr<ReaderState>& state);
    static void ReaderThread(std::shared_ptr<ReaderState> state) noexcept;
    static void ThreadMain(const std::shared_ptr<ReaderState>& state);

    std::wstring m_path;
    std::wstring m_ffmpeg;
    std::vector<audio_track::Track> m_tracks;
    // The path m_tracks describes, so a seek reuses them and a new media load
    // re-enumerates.
    std::wstring m_tracksPath;
    int m_selectedTrack = 0;
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
