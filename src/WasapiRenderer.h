#pragma once

#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include "AudioEndpointPolicy.h"
#include "AudioPassthroughPolicy.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// The audio output endpoint, shared-mode and event-driven.
//
// It replaces waveOut, which is a compatibility shim over this same API on
// every supported Windows. Two reasons, neither of them latency:
//
//  * Format. waveOut was opened at a fixed 16-bit 48 kHz stereo and ffmpeg was
//    told to produce exactly that, so every source was quantized to 16 bits
//    and resampled to 48 kHz before Windows then converted it again to
//    whatever the endpoint's mix format actually is. Rendering at the mix
//    format removes both conversions.
//
//  * Device changes. A waveOutWrite to an endpoint that has gone away fails,
//    the reader thread breaks, and there is no sound for the rest of the
//    session - unplug a pair of headphones and the film plays on in silence.
//    WASAPI reports AUDCLNT_E_DEVICE_INVALIDATED, which is recoverable.
//
// Not latency: the measured seek cost on the waveOut path was 60 ms, not the
// 682 ms of queue depth the task list attributed to it, because a seek resets
// the device and restarts ffmpeg. The queue depth was never in the path.
//
// The position this reports is frames PLAYED, from IAudioClock, which is the
// same guarantee waveOutGetPosition's TIME_SAMPLES gave and what makes the
// audio-master clock correct rather than optimistic.
//
// One exception to shared mode: bitstream passthrough (OpenPassthrough) is an
// EXCLUSIVE stream, because the shared engine mixes in float and would turn
// IEC 61937 bursts into noise. See AudioPassthroughPolicy.h.
class WasapiRenderer {
public:
    // What the endpoint wants, and therefore what ffmpeg is told to produce.
    struct Format {
        uint32_t sampleRate{};
        uint16_t channels{};
        uint16_t bitsPerSample{};
        bool isFloat{};
        uint32_t BytesPerFrame() const { return uint32_t(channels) * bitsPerSample / 8; }
        bool Valid() const { return sampleRate && channels && bitsPerSample; }
    };

    WasapiRenderer() = default;
    ~WasapiRenderer();
    WasapiRenderer(const WasapiRenderer&) = delete;
    WasapiRenderer& operator=(const WasapiRenderer&) = delete;

    // Opens the default render endpoint at its mix format. COM must already be
    // initialized on the calling thread.
    bool Open();

    // Opens the default render endpoint in exclusive mode carrying `link` as
    // an IEC 61937 bitstream: 16-bit stereo frames at link.transportRate, as
    // FFmpeg's spdif muxer writes them. Refused means the endpoint does not
    // take the format at all - no receiver, or one that does not decode this
    // codec - and Failed that it does but an exclusive stream would not start
    // (another application holds it, or exclusive mode is disallowed). The
    // caller falls back to Open on a fresh renderer either way; this one is
    // left closed.
    enum class PassthroughOpen { Opened, Refused, Failed };
    PassthroughOpen OpenPassthrough(const audio_passthrough::Link& link);
    // An exclusive passthrough stream is open.
    bool Exclusive() const;
    void Close();
    bool Valid() const;

    Format CurrentFormat() const { return format_; }

    // Blocks until the endpoint wants more, or the timeout elapses. Reports
    // how many frames it will take. False means the device went away - check
    // DeviceLost.
    bool WaitForSpace(DWORD timeoutMilliseconds, uint32_t& framesWanted);

    enum class WriteResult {
        Written,
        // A fade-out tail is queued and the stream is on its way down, so
        // nothing was taken. The caller keeps the frames for the resume:
        // this used to report success, and the reader threw away a buffer's
        // worth of the film whenever a pause landed mid-write.
        Refused,
        // The device went away - check DeviceLost.
        Failed,
    };

    // `frames` must hold framesToWrite * BytesPerFrame bytes in the current
    // format, and framesToWrite must not exceed what WaitForSpace reported.
    //
    // Exclusive streams take exactly one whole buffer per event, so there a
    // short write is completed with null data (see WriteFiller) rather than
    // refused by the endpoint.
    WriteResult Write(const void* frames, uint32_t framesToWrite);

    // Exclusive streams only, and a no-op otherwise: one buffer of null data
    // for an event the source had nothing ready for. Left unanswered, the
    // device plays the stale buffer again, and a receiver decodes a repeated
    // burst as a stutter. The null frames are not film, so the clock does not
    // count them (PlayedFrames).
    WriteResult WriteFiller();

    bool Start();
    bool Stop();

    // Stops after decaying the last frame to silence and letting the queue
    // play out, rather than cutting it mid-waveform. Bounded: the endpoint
    // holds one engine period, so the wait is tens of milliseconds at worst
    // and returns regardless.
    //
    // Writes are refused from the moment the tail is queued, so the reader
    // thread cannot append after it and re-introduce the step. Start() and
    // Reset() open them again.
    bool FadeOutAndStop();
    // Discards anything queued and zeroes the played-frame count. Used by seek
    // and stop, never at end of stream: zeroing there would make the
    // audio-master clock jump backwards during the last frames.
    bool Reset();

    // Frames of the source played since the last Reset. False when
    // unavailable. On an exclusive stream the null data WriteFiller sent is
    // not counted: the device clock runs through it, the film does not.
    bool PlayedFrames(uint64_t& frames) const;

    void SetVolume(float volume01);

    // Open failed because there is no default render endpoint at all, as
    // opposed to one that would not open.
    bool NoEndpoint() const { return noEndpoint_; }

    // Set once the endpoint reports itself invalidated. The owner reopens and
    // restarts the source rather than trying to splice into a new device
    // mid-stream: the mix format may differ, so the decoder has to be told.
    bool DeviceLost() const { return deviceLost_.load(); }

    // The endpoint this renderer is on, empty before Open.
    std::wstring DeviceId() const;

    // The endpoint notification handlers.
    //
    // The OS calls these through the registered IMMNotificationClient. The
    // audio smoke calls the same three directly, which is how the recovery
    // path is exercised end to end without changing the machine's default
    // playback device out from under whoever is using it. They are the
    // handlers, not a test seam: there is one implementation and both callers
    // reach it.
    void OnDefaultEndpointChanged(EDataFlow flow, ERole role, const wchar_t* newDeviceId);
    void OnEndpointStateChanged(const wchar_t* deviceId, DWORD newState);
    void OnEndpointFormatChanged(const wchar_t* deviceId, bool isDeviceFormatKey);
    // The session went away underneath the stream - device removed, audio
    // service restarted, or the format changed.
    void OnSessionDisconnected();

private:
    class EndpointWatcher;

    mutable std::mutex mutex_;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<IMMDevice> device_;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render_;
    Microsoft::WRL::ComPtr<IAudioClock> clock_;
    Microsoft::WRL::ComPtr<ISimpleAudioVolume> volume_;
    HANDLE ready_ = nullptr;   // signalled by the endpoint when it wants more
    Format format_{};
    uint32_t bufferFrames_ = 0;
    uint64_t clockFrequency_ = 0;
    std::atomic<bool> deviceLost_{false};
    bool noEndpoint_ = false;
    bool started_ = false;
    // Exclusive passthrough. Such a stream is primed with one buffer before it
    // is started - started empty, the device clock would run through the
    // silence ahead of the first burst and the picture would lead the sound by
    // it - so Start only records the request until the first Write lands.
    bool exclusive_ = false;
    bool primed_ = false;
    bool startRequested_ = false;
    // Bitstream cannot be attenuated, so a muted passthrough stream sends
    // null data in place of the film instead; the receiver owns the level.
    bool muted_ = false;
    // Where in the stream WriteFiller put null data, in frames written, so
    // PlayedFrames can leave out exactly the part of it already played.
    // Adjacent ranges merge, and ranges fully played retire into the total.
    struct FillerRange { uint64_t start, count; };
    std::vector<FillerRange> filler_;
    uint64_t fillerRetired_ = 0;
    // De-click state. See AudioFadePolicy.h for why the ramps exist and why
    // they are raised cosines.
    uint32_t fadeFrames_ = 0;
    uint64_t framesFadedIn_ = 0;
    uint64_t framesWritten_ = 0;
    // The last frame handed to the endpoint, which is where a fade-out has to
    // start from: the samples after it were never decoded.
    std::vector<float> lastFrame_;
    std::vector<float> fadeTail_;
    bool refusingWrites_ = false;
    // Endpoint identity and the notification registrations that watch it.
    std::wstring deviceId_;
    // Raw and hand-counted rather than a ComPtr: ComPtr's destructor needs
    // the complete type in this header, and the watcher's definition wants
    // to stay in the .cpp beside the handlers it forwards to. Exactly one
    // reference is owned here, taken in Open and released in Close.
    EndpointWatcher* watcher_ = nullptr;
    Microsoft::WRL::ComPtr<IAudioSessionControl> session_;
    bool watchingEndpoints_ = false;
    bool watchingSession_ = false;
    // Guard for a sink that stops asking for data without erroring.
    audio_sink::State sinkState_;

    // Records a device-invalidated result and returns whether it was one, so
    // every call site handles it the same way.
    bool NoteDeviceLoss(HRESULT result);
    bool WriteLocked(const void* frames, uint32_t framesToWrite, bool fadeIn);
    // Exclusive: one whole buffer, `framesToWrite` of it from `frames` (null
    // for none) and the rest null data.
    bool WriteExclusiveLocked(const void* frames, uint32_t framesToWrite);
    // What both opens share once the client is initialized.
    bool CompleteOpenLocked();
};

// Watches for a render endpoint to come back while there is none to play to.
//
// A renderer watches only the endpoint it is on, so once the last one had
// gone - or there was none when the film was opened - nothing was watching
// at all, and audio never came back when headphones were plugged in again.
// The owner holds one of these only while it has no renderer.
class RenderEndpointArrival {
public:
    RenderEndpointArrival() = default;
    ~RenderEndpointArrival();
    RenderEndpointArrival(const RenderEndpointArrival&) = delete;
    RenderEndpointArrival& operator=(const RenderEndpointArrival&) = delete;

    // COM must be initialized on the calling thread. `checkNow` also latches
    // an endpoint that is already there, for a caller whose open found none
    // an instant ago: one that arrived in between sent its notification
    // before anyone was listening.
    bool Watch(bool checkNow);
    // True once since the last call if something arrived worth a retry.
    bool Arrived();

private:
    class Client;
    // Raw for the same reason as WasapiRenderer::watcher_. One reference is
    // owned here, released after Unregister has returned.
    Client* client_ = nullptr;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    bool registered_ = false;
};
