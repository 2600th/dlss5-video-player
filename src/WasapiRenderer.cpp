#include "WasapiRenderer.h"

#include "AudioFadePolicy.h"
#include "HexText.h"
#include "Log.h"

#include <audiopolicy.h>
#include <mmreg.h>

#include <algorithm>
#include <cstring>
#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

// Shared-mode event-driven streams take the engine's own period when the
// requested duration is zero, which is what the endpoint is tuned for. Naming
// a number here only risks asking for something the driver then rounds.
constexpr REFERENCE_TIME kEnginePeriod = 0;

// PKEY_AudioEngine_DeviceFormat, spelled out rather than pulled in from
// functiondiscoverykeys_devpkey.h: that header only defines the storage under
// INITGUID, and defining INITGUID here would instantiate every other key in
// mmdeviceapi in this translation unit as well.
const PROPERTYKEY kDeviceFormatKey = {
    {0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

WasapiRenderer::Format DescribeFormat(const WAVEFORMATEX& wave)
{
    WasapiRenderer::Format format;
    format.sampleRate = wave.nSamplesPerSec;
    format.channels = wave.nChannels;
    format.bitsPerSample = wave.wBitsPerSample;
    if (wave.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        format.isFloat = true;
    } else if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               wave.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wave);
        format.isFloat = IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    return format;
}

// An exclusive client's last Release, bounded.
//
// mpv #1773: IAudioClient::Release can hang in the driver after the stream's
// format has changed, and passthrough changes it every time it starts or
// falls back - bitstream on one client, float PCM on the next. Every open
// already activates a new client rather than re-initializing an old one, and
// Close stops and resets the stream before letting go of it, which is what
// the drivers that hang are waiting for. This is the part that does not rely
// on the driver: the release runs on its own thread and is waited for a
// bounded time, so a driver that hangs there anyway costs one leaked client
// and a log line rather than the UI thread that asked for a seek.
struct ExclusiveClientParts {
    ComPtr<IAudioClock> clock;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioClient> client;
    ComPtr<IMMDevice> device;
};

constexpr DWORD kExclusiveReleaseBoundMs = 2000;

void ReleaseExclusiveBounded(ExclusiveClientParts parts)
{
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return;  // parts release here, unbounded, as every other client does
    std::thread releaser;
    try {
        releaser = std::thread([parts = std::move(parts), done]() mutable {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            parts = {};
            if (SUCCEEDED(com)) CoUninitialize();
            SetEvent(done);
        });
    } catch (...) {
        CloseHandle(done);
        return;
    }
    if (WaitForSingleObject(done, kExclusiveReleaseBoundMs) == WAIT_OBJECT_0) {
        releaser.join();
        CloseHandle(done);
        return;
    }
    // The thread still owns the client and will signal `done` if the driver
    // ever lets go, so neither can be closed here.
    LOG("Audio: the exclusive stream's IAudioClient::Release did not return within "
        << kExclusiveReleaseBoundMs << " ms (mpv #1773); leaving it behind and carrying on.");
    releaser.detach();
}

const char* CodecText(audio_passthrough::Codec codec)
{
    switch (codec) {
        case audio_passthrough::Codec::Ac3: return "AC-3";
        case audio_passthrough::Codec::Eac3: return "E-AC-3";
        case audio_passthrough::Codec::Dts: return "DTS";
        // Only the audio smoke opens one of these: the exclusive stream's
        // mechanics, measured on plain PCM where there is no receiver.
        default: return "16-bit PCM";
    }
}

} // namespace

// One object for both notification interfaces, because both answer the same
// question - has the stream this player is on gone away - and splitting them
// would double the boilerplate for no separation of concern.
//
// Lifetime: the back pointer sits behind an audio_endpoint::CallbackGate,
// whose note says why Close detaches it and unregisters without mutex_. The
// watcher itself is a COM object: the renderer keeps its reference until both
// Unregister calls have returned, and any reference the OS still holds after
// that keeps alive an object whose callbacks only ever see a null owner.
class WasapiRenderer::EndpointWatcher final : public IMMNotificationClient,
                                              public IAudioSessionEvents {
public:
    explicit EndpointWatcher(WasapiRenderer* owner) : gate_(owner) {}

    // Called WITHOUT the renderer's lock: a callback in flight may be waiting
    // for it. Returns once no callback can reach the renderer any more.
    void Detach() { gate_.Detach(); }

    ULONG STDMETHODCALLTYPE AddRef() override { return references_.fetch_add(1) + 1; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = references_.fetch_sub(1) - 1;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient))
            *object = static_cast<IMMNotificationClient*>(this);
        else if (riid == __uuidof(IAudioSessionEvents))
            *object = static_cast<IAudioSessionEvents*>(this);
        else { *object = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                     LPCWSTR deviceId) override
    {
        Forward([&](WasapiRenderer& owner) { owner.OnDefaultEndpointChanged(flow, role, deviceId); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState) override
    {
        Forward([&](WasapiRenderer& owner) { owner.OnEndpointStateChanged(deviceId, newState); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR deviceId, const PROPERTYKEY key) override
    {
        // The one that is routinely forgotten: the engine's mix format
        // changing under a stream that was opened at the old one.
        Forward([&](WasapiRenderer& owner) {
            owner.OnEndpointFormatChanged(deviceId, key == kDeviceFormatKey);
        });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR deviceId) override
    {
        Forward([&](WasapiRenderer& owner) {
            owner.OnEndpointStateChanged(deviceId, DEVICE_STATE_NOTPRESENT);
        });
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason) override
    {
        Forward([](WasapiRenderer& owner) { owner.OnSessionDisconnected(); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState) override { return S_OK; }

private:
    ~EndpointWatcher() = default;

    template <typename Handler>
    void Forward(Handler&& handler) { gate_.Forward(std::forward<Handler>(handler)); }

    std::atomic<ULONG> references_{1};
    audio_endpoint::CallbackGate<WasapiRenderer> gate_;
};

WasapiRenderer::~WasapiRenderer() { Close(); }

std::wstring WasapiRenderer::DeviceId() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return deviceId_;
}

void WasapiRenderer::OnDefaultEndpointChanged(EDataFlow flow, ERole role, const wchar_t* newDeviceId)
{
    std::wstring ours;
    { std::lock_guard<std::mutex> lock(mutex_); ours = deviceId_; }
    if (!audio_endpoint::DefaultChangeAffectsUs(flow, role, newDeviceId ? newDeviceId : L"", ours))
        return;
    if (!deviceLost_.exchange(true))
        LOG("Audio: the default playback endpoint changed; the owner will move onto it.");
}

void WasapiRenderer::OnEndpointStateChanged(const wchar_t* deviceId, DWORD newState)
{
    std::wstring ours;
    { std::lock_guard<std::mutex> lock(mutex_); ours = deviceId_; }
    if (!audio_endpoint::StateChangeAffectsUs(deviceId ? deviceId : L"", newState, ours)) return;
    if (!deviceLost_.exchange(true))
        LOG("Audio: the endpoint being played to is no longer active (state="
            << HexText(newState) << "); the owner will reopen.");
}

void WasapiRenderer::OnEndpointFormatChanged(const wchar_t* deviceId, bool isDeviceFormatKey)
{
    std::wstring ours;
    { std::lock_guard<std::mutex> lock(mutex_); ours = deviceId_; }
    if (!audio_endpoint::FormatChangeAffectsUs(deviceId ? deviceId : L"", ours, isDeviceFormatKey))
        return;
    if (!deviceLost_.exchange(true))
        LOG("Audio: the endpoint's mix format changed under the stream; the owner will reopen "
            "so the decoder is told the new one.");
}

void WasapiRenderer::OnSessionDisconnected()
{
    if (!deviceLost_.exchange(true))
        LOG("Audio: the render session was disconnected; the owner will reopen.");
}

bool WasapiRenderer::NoteDeviceLoss(HRESULT result)
{
    if (result != AUDCLNT_E_DEVICE_INVALIDATED) return false;
    if (!deviceLost_.exchange(true))
        LOG("Audio: the render endpoint was invalidated; the owner will reopen it.");
    return true;
}

bool WasapiRenderer::Open()
{
    std::lock_guard<std::mutex> lock(mutex_);
    deviceLost_ = false;
    noEndpoint_ = false;
    exclusive_ = false;

    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator_));
    if (FAILED(result)) { LOG("Audio: MMDeviceEnumerator failed hr=" << HexText(result)); return false; }

    result = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(result)) {
        noEndpoint_ = result == E_NOTFOUND;
        LOG("Audio: no default render endpoint hr=" << HexText(result));
        return false;
    }

    result = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
    if (FAILED(result)) { LOG("Audio: IAudioClient activation failed hr=" << HexText(result)); return false; }

    WAVEFORMATEX* mix = nullptr;
    result = client_->GetMixFormat(&mix);
    if (FAILED(result) || !mix) { LOG("Audio: GetMixFormat failed hr=" << HexText(result)); return false; }
    format_ = DescribeFormat(*mix);

    // The engine only ever mixes float in shared mode, so a non-float mix
    // format means something has been misread rather than that a conversion is
    // needed. Refusing is better than feeding the endpoint the wrong bytes.
    if (!format_.isFloat || format_.bitsPerSample != 32) {
        LOG("Audio: the endpoint's mix format is not 32-bit float ("
            << format_.bitsPerSample << " bits); refusing rather than guessing.");
        CoTaskMemFree(mix);
        return false;
    }

    result = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 kEnginePeriod, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(result)) { LOG("Audio: IAudioClient::Initialize failed hr=" << HexText(result)); return false; }
    if (!CompleteOpenLocked()) return false;

    LOG("Audio: WASAPI shared mode at " << format_.sampleRate << " Hz, " << format_.channels
        << " channels, 32-bit float; endpoint buffer " << bufferFrames_ << " frames ("
        << (double(bufferFrames_) * 1000.0 / double(format_.sampleRate)) << " ms).");
    return true;
}

WasapiRenderer::PassthroughOpen WasapiRenderer::OpenPassthrough(const audio_passthrough::Link& link)
{
    std::lock_guard<std::mutex> lock(mutex_);
    deviceLost_ = false;
    noEndpoint_ = false;
    exclusive_ = true;
    primed_ = false;
    startRequested_ = false;
    filler_.clear();
    fillerRetired_ = 0;
    const char* const codec = CodecText(link.codec);

    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator_));
    if (FAILED(result)) { LOG("Audio: MMDeviceEnumerator failed hr=" << HexText(result)); return PassthroughOpen::Failed; }
    result = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(result)) {
        noEndpoint_ = result == E_NOTFOUND;
        LOG("Audio: no default render endpoint for passthrough hr=" << HexText(result));
        return PassthroughOpen::Failed;
    }
    result = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
    if (FAILED(result)) { LOG("Audio: IAudioClient activation failed hr=" << HexText(result)); return PassthroughOpen::Failed; }

    // Asked first rather than discovered by Initialize failing: an exclusive
    // Initialize that fails leaves some drivers in a state the next open
    // inherits. The IEC 61937 extension first, then the plain form some
    // drivers want instead (see audio_passthrough::WaveFormat).
    WAVEFORMATEXTENSIBLE_IEC61937 chosen{};
    bool supported = false;
    const auto askedAt = std::chrono::steady_clock::now();
    for (const bool plain : {false, true}) {
        chosen = audio_passthrough::WaveFormat(link, plain);
        result = client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                            reinterpret_cast<const WAVEFORMATEX*>(&chosen), nullptr);
        if (result == S_OK) { supported = true; break; }
    }
    if (!supported) {
        const double askedMs = std::chrono::duration<double>(std::chrono::steady_clock::now() - askedAt).count() * 1e3;
        LOG("Audio: the endpoint does not take " << codec << " as IEC 61937 at "
            << link.transportRate << " Hz (hr=" << HexText(result) << ", answered in " << askedMs
            << " ms); nothing on it decodes this. Playing PCM instead.");
        return PassthroughOpen::Refused;
    }

    REFERENCE_TIME period = 0, minimumPeriod = 0;
    result = client_->GetDevicePeriod(&period, &minimumPeriod);
    if (FAILED(result) || period <= 0) { LOG("Audio: GetDevicePeriod failed hr=" << HexText(result)); return PassthroughOpen::Failed; }
    const auto* const wave = reinterpret_cast<const WAVEFORMATEX*>(&chosen);
    result = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 period, period, wave, nullptr);
    if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // The documented dance: the period has to be a whole number of the
        // device's own buffer, which it reports only now, and only a NEW
        // client may be initialized with the corrected one.
        UINT32 alignedFrames = 0;
        result = client_->GetBufferSize(&alignedFrames);
        if (FAILED(result) || !alignedFrames) { LOG("Audio: GetBufferSize for alignment failed hr=" << HexText(result)); return PassthroughOpen::Failed; }
        period = REFERENCE_TIME(10'000'000.0 * double(alignedFrames) / double(link.transportRate) + 0.5);
        client_.Reset();
        result = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
        if (FAILED(result)) { LOG("Audio: IAudioClient re-activation failed hr=" << HexText(result)); return PassthroughOpen::Failed; }
        result = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     period, period, wave, nullptr);
    }
    if (FAILED(result)) {
        LOG("Audio: the endpoint takes " << codec << " but an exclusive stream would not start (hr="
            << HexText(result)
            << (result == AUDCLNT_E_DEVICE_IN_USE ? ", another application holds it"
                : result == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED ? ", exclusive mode is disallowed for this device"
                : "")
            << "). Playing PCM instead.");
        return PassthroughOpen::Failed;
    }

    format_ = {};
    format_.sampleRate = link.transportRate;
    format_.channels = audio_passthrough::kTransportChannels;
    format_.bitsPerSample = audio_passthrough::kTransportBits;
    if (!CompleteOpenLocked()) return PassthroughOpen::Failed;

    const bool plain = chosen.FormatExt.Format.cbSize == sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    LOG("Audio: WASAPI exclusive passthrough, " << codec
        << (link.codec == audio_passthrough::Codec::None ? "" : " as IEC 61937") << " at " << link.transportRate
        << " Hz (" << link.encodedChannels << " channels at " << link.encodedRate << " Hz once decoded"
        << (plain ? ", plain extensible format" : "") << "); endpoint buffer " << bufferFrames_
        << " frames (" << (double(bufferFrames_) * 1000.0 / double(format_.sampleRate)) << " ms).");
    return PassthroughOpen::Opened;
}

bool WasapiRenderer::Exclusive() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return exclusive_ && client_;
}

// The caller holds mutex_ and has initialized client_ and set format_.
bool WasapiRenderer::CompleteOpenLocked()
{
    HRESULT result = S_OK;
    ready_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready_) { LOG("Audio: render event creation failed winerr=" << GetLastError()); return false; }
    result = client_->SetEventHandle(ready_);
    if (FAILED(result)) { LOG("Audio: SetEventHandle failed hr=" << HexText(result)); return false; }

    result = client_->GetBufferSize(&bufferFrames_);
    if (FAILED(result)) { LOG("Audio: GetBufferSize failed hr=" << HexText(result)); return false; }

    // Identity first: every notification decision compares against it, and a
    // notification can arrive the instant the callback is registered.
    LPWSTR rawId = nullptr;
    if (SUCCEEDED(device_->GetId(&rawId)) && rawId) {
        deviceId_.assign(rawId);
        CoTaskMemFree(rawId);
    } else {
        LOG("Audio: the endpoint would not name itself; default-device changes cannot be "
            "distinguished from changes to other devices, so only invalidation is noticed.");
    }

    fadeFrames_ = audio_fade::FrameCount(format_.sampleRate);
    lastFrame_.assign(format_.channels, 0.0f);
    framesFadedIn_ = 0;
    framesWritten_ = 0;
    refusingWrites_ = false;

    result = client_->GetService(IID_PPV_ARGS(&render_));
    if (FAILED(result)) { LOG("Audio: IAudioRenderClient failed hr=" << HexText(result)); return false; }

    result = client_->GetService(IID_PPV_ARGS(&clock_));
    if (FAILED(result)) { LOG("Audio: IAudioClock failed hr=" << HexText(result)); return false; }
    result = clock_->GetFrequency(&clockFrequency_);
    if (FAILED(result) || clockFrequency_ == 0) {
        LOG("Audio: IAudioClock::GetFrequency failed hr=" << HexText(result));
        return false;
    }

    // Volume is per-session, so this moves the player's own slider in the
    // mixer rather than the endpoint's master level. waveOutSetVolume did the
    // same thing. An exclusive stream has no mixer to move, and a bitstream
    // has no level to scale: the receiver owns it.
    if (!exclusive_ && FAILED(client_->GetService(IID_PPV_ARGS(&volume_))))
        LOG("Audio: ISimpleAudioVolume unavailable; the volume slider will not reach the mixer.");

    // Notifications rather than polling. Registered last, so everything a
    // callback reads is already in place.
    watcher_ = new EndpointWatcher(this);
    if (SUCCEEDED(enumerator_->RegisterEndpointNotificationCallback(watcher_)))
        watchingEndpoints_ = true;
    else
        LOG("Audio: endpoint notifications could not be registered; a default-device change "
            "will not be noticed until a call to the endpoint fails.");

    if (SUCCEEDED(client_->GetService(IID_PPV_ARGS(&session_))) && session_) {
        if (SUCCEEDED(session_->RegisterAudioSessionNotification(watcher_)))
            watchingSession_ = true;
        else
            LOG("Audio: session notifications could not be registered; a disconnect will not "
                "be noticed until a call to the endpoint fails.");
    }
    sinkState_ = {};
    return true;
}

void WasapiRenderer::Close()
{
    // Unregister before anything else is released, and detach before that:
    // a notification already in flight must not reach a renderer that is
    // halfway through tearing itself down. None of it under mutex_ - every
    // handler takes mutex_, so holding it here while Detach waits for them,
    // or while Unregister possibly does, would deadlock against a burst of
    // notifications (one per role on a default-device change). See the
    // lifetime note on EndpointWatcher.
    EndpointWatcher* watcher = nullptr;
    ComPtr<IAudioSessionControl> session;
    ComPtr<IMMDeviceEnumerator> enumerator;
    bool watchingSession = false, watchingEndpoints = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        watcher = std::exchange(watcher_, nullptr);
        session = session_;
        enumerator = enumerator_;
        watchingSession = std::exchange(watchingSession_, false);
        watchingEndpoints = std::exchange(watchingEndpoints_, false);
    }
    if (watcher) {
        watcher->Detach();
        if (watchingSession && session) session->UnregisterAudioSessionNotification(watcher);
        if (watchingEndpoints && enumerator) enumerator->UnregisterEndpointNotificationCallback(watcher);
        // Only now: the watcher had to outlive both Unregister calls.
        watcher->Release();
    }
    session.Reset();
    enumerator.Reset();

    ExclusiveClientParts exclusiveParts;
    bool exclusive = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_.Reset();
        if (client_ && started_) client_->Stop();
        started_ = false;
        volume_.Reset();
        exclusive = std::exchange(exclusive_, false);
        if (exclusive) {
            // Stopped and reset before the last reference goes, and released
            // off this thread with a bound; see ReleaseExclusiveBounded.
            if (client_) client_->Reset();
            exclusiveParts.clock = std::move(clock_);
            exclusiveParts.render = std::move(render_);
            exclusiveParts.client = std::move(client_);
            exclusiveParts.device = std::move(device_);
        }
        clock_.Reset();
        render_.Reset();
        client_.Reset();
        device_.Reset();
        enumerator_.Reset();
        if (ready_) { CloseHandle(ready_); ready_ = nullptr; }
        format_ = {};
        bufferFrames_ = 0;
        clockFrequency_ = 0;
        deviceId_.clear();
        sinkState_ = {};
        primed_ = false;
        startRequested_ = false;
        filler_.clear();
        fillerRetired_ = 0;
    }
    if (exclusive) ReleaseExclusiveBounded(std::move(exclusiveParts));
}

bool WasapiRenderer::Valid() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return client_ && render_ && clock_;
}

bool WasapiRenderer::WaitForSpace(DWORD timeoutMilliseconds, uint32_t& framesWanted)
{
    framesWanted = 0;
    HANDLE ready = nullptr;
    bool playing = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready = ready_; playing = started_;
        // An exclusive stream is primed before it starts, and a stream that
        // has not started never signals: the first buffer is wanted now.
        if (ready && exclusive_ && !primed_) { framesWanted = bufferFrames_; return true; }
    }
    if (!ready) return false;
    const bool signalled = WaitForSingleObject(ready, timeoutMilliseconds) == WAIT_OBJECT_0;

    // Some drivers stop asking for data without ever returning an error, so
    // every call below would succeed and the film would simply go quiet.
    // Nothing polling can see that; only the absence of the request can.
    {
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lock(mutex_);
        if (audio_sink::Dead(sinkState_, now, signalled, started_ && !refusingWrites_)) {
            if (!deviceLost_.exchange(true))
                LOG("Audio: the endpoint has not asked for data in over "
                    << audio_sink::kDeadSeconds << " s while playing; treating the sink as dead "
                    "and reopening.");
            return false;
        }
    }
    (void)playing;
    // A timeout is not otherwise a failure: the caller polls its stop flag
    // between waits, and a paused stream never signals.
    if (!signalled) return true;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!client_) return false;
    uint32_t padding = 0;
    const HRESULT result = client_->GetCurrentPadding(&padding);
    if (FAILED(result)) { NoteDeviceLoss(result); return false; }
    // An exclusive event-driven stream is double-buffered a period at a time:
    // each event is one whole buffer to fill, whatever padding reads, and a
    // request for anything else is refused (AUDCLNT_E_BUFFER_SIZE_ERROR).
    framesWanted = exclusive_ ? bufferFrames_ : bufferFrames_ > padding ? bufferFrames_ - padding : 0;
    return true;
}

WasapiRenderer::WriteResult WasapiRenderer::Write(const void* frames, uint32_t framesToWrite)
{
    if (!framesToWrite) return WriteResult::Written;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!render_) return WriteResult::Failed;
    // A tail has been queued and the stream is on its way down. Accepting more
    // would put a step back in after the ramp that removed it.
    if (refusingWrites_) return WriteResult::Refused;
    if (exclusive_) return WriteExclusiveLocked(frames, framesToWrite) ? WriteResult::Written : WriteResult::Failed;
    return WriteLocked(frames, framesToWrite, true) ? WriteResult::Written : WriteResult::Failed;
}

WasapiRenderer::WriteResult WasapiRenderer::WriteFiller()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // Nothing to replay before the first buffer, and priming with null data
    // would only start the device clock early.
    if (!exclusive_ || !primed_) return WriteResult::Written;
    if (!render_) return WriteResult::Failed;
    if (refusingWrites_) return WriteResult::Refused;
    return WriteExclusiveLocked(nullptr, 0) ? WriteResult::Written : WriteResult::Failed;
}

// The caller holds mutex_.
bool WasapiRenderer::WriteExclusiveLocked(const void* frames, uint32_t framesToWrite)
{
    framesToWrite = std::min(framesToWrite, bufferFrames_);
    BYTE* destination = nullptr;
    HRESULT result = render_->GetBuffer(bufferFrames_, &destination);
    if (FAILED(result)) { NoteDeviceLoss(result); return false; }
    const size_t bytesPerFrame = format_.BytesPerFrame();
    const size_t filmBytes = size_t(framesToWrite) * bytesPerFrame;
    // Muted: null data in the film's place, which the receiver plays as
    // silence. It is still the film's time, so it is not filler.
    if (frames && !muted_) std::memcpy(destination, frames, filmBytes);
    else std::memset(destination, 0, filmBytes);
    std::memset(destination + filmBytes, 0, size_t(bufferFrames_) * bytesPerFrame - filmBytes);
    result = render_->ReleaseBuffer(bufferFrames_, 0);
    if (FAILED(result)) { NoteDeviceLoss(result); return false; }
    if (const uint32_t nullFrames = bufferFrames_ - framesToWrite) {
        const uint64_t start = framesWritten_ + framesToWrite;
        if (!filler_.empty() && filler_.back().start + filler_.back().count == start)
            filler_.back().count += nullFrames;
        else
            filler_.push_back({start, nullFrames});
    }
    framesWritten_ += bufferFrames_;
    // Ranges the device has played past fold into one number, so the list
    // holds only what is still queued.
    uint64_t position = 0;
    if (clock_ && clockFrequency_ && SUCCEEDED(clock_->GetPosition(&position, nullptr))) {
        const auto played = uint64_t((long double)position / (long double)clockFrequency_ *
                                     (long double)format_.sampleRate);
        while (!filler_.empty() && filler_.front().start + filler_.front().count <= played) {
            fillerRetired_ += filler_.front().count;
            filler_.erase(filler_.begin());
        }
    }
    if (!primed_) {
        primed_ = true;
        if (startRequested_) {
            result = client_->Start();
            if (FAILED(result)) { NoteDeviceLoss(result); return false; }
            started_ = true;
        }
    }
    return true;
}

// The caller holds mutex_. `fadeIn` is false for the fade-out tail, which is
// already shaped and must not be re-scaled by a ramp that is still opening.
bool WasapiRenderer::WriteLocked(const void* frames, uint32_t framesToWrite, bool fadeIn)
{
    BYTE* destination = nullptr;
    HRESULT result = render_->GetBuffer(framesToWrite, &destination);
    if (FAILED(result)) { NoteDeviceLoss(result); return false; }
    std::memcpy(destination, frames, size_t(framesToWrite) * format_.BytesPerFrame());
    // Safe to treat as float: Open refuses any mix format that is not 32-bit
    // float rather than guessing at a conversion.
    float* const samples = reinterpret_cast<float*>(destination);
    if (fadeIn)
        audio_fade::ApplyFadeIn(samples, framesToWrite, format_.channels, fadeFrames_, framesFadedIn_);
    if (format_.channels) {
        const float* const last = samples + size_t(framesToWrite - 1) * format_.channels;
        lastFrame_.assign(last, last + format_.channels);
    }
    result = render_->ReleaseBuffer(framesToWrite, 0);
    if (FAILED(result)) { NoteDeviceLoss(result); return false; }
    framesWritten_ += framesToWrite;
    return true;
}

bool WasapiRenderer::Start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!client_ || started_) return client_ != nullptr;
    // Started once the first buffer is in: see primed_.
    if (exclusive_ && !primed_) {
        startRequested_ = true;
        refusingWrites_ = false;
        return true;
    }
    const HRESULT result = client_->Start();
    if (FAILED(result)) { NoteDeviceLoss(result); return false; }
    started_ = true;
    // Whatever stopped this stream either decayed it to silence or cut it, so
    // the next samples start from zero either way and want opening. A resume
    // that came straight back up would click exactly as the old path did.
    framesFadedIn_ = 0;
    refusingWrites_ = false;
    return true;
}

bool WasapiRenderer::Stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    startRequested_ = false;
    if (!client_ || !started_) return client_ != nullptr;
    const HRESULT result = client_->Stop();
    if (FAILED(result)) { NoteDeviceLoss(result); return false; }
    started_ = false;
    return true;
}

bool WasapiRenderer::Reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!client_) return false;
    if (started_) {
        const HRESULT stopped = client_->Stop();
        if (FAILED(stopped)) { NoteDeviceLoss(stopped); return false; }
        started_ = false;
    }
    const HRESULT result = client_->Reset();
    if (FAILED(result)) { NoteDeviceLoss(result); return false; }
    // Reset zeroes the played-frame count, so everything measured against it
    // has to go with it.
    framesFadedIn_ = 0;
    framesWritten_ = 0;
    refusingWrites_ = false;
    std::fill(lastFrame_.begin(), lastFrame_.end(), 0.0f);
    // An emptied exclusive stream is primed again before it restarts.
    primed_ = false;
    startRequested_ = false;
    filler_.clear();
    fillerRetired_ = 0;
    return true;
}

bool WasapiRenderer::FadeOutAndStop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!client_) return false;
        // A bitstream is not a waveform: scaling it would corrupt the bursts,
        // and the receiver mutes on its own when they stop. Writes close so
        // the reader keeps its frames for the resume, as below.
        if (exclusive_) {
            startRequested_ = false;
            refusingWrites_ = true;
            if (!started_) return true;
            const HRESULT result = client_->Stop();
            if (FAILED(result)) { NoteDeviceLoss(result); return false; }
            started_ = false;
            return true;
        }
        // Nothing is playing, so there is no waveform to be cut in half.
        if (!started_) return true;
        // Close the door first. Once this is set, lastFrame_ cannot change
        // under the polling below, so the tail is guaranteed to start from
        // the frame the endpoint will actually have played last.
        if (refusingWrites_) return true;
        refusingWrites_ = true;
        audio_fade::BuildFadeOutTail(lastFrame_.data(), format_.channels, fadeFrames_, fadeTail_);
    }

    // Wait for room before writing the tail.
    //
    // The reader fills whatever WaitForSpace reported, so at the moment a
    // seek or a pause arrives the endpoint buffer is typically FULL - which
    // is exactly when a tail is needed and exactly when there is no room for
    // one. Checking once and giving up, which this did at first, meant the
    // tail was almost never written in the player even though it was always
    // written in a test that had just started. The buffer drains on its own
    // in one engine period, so waiting for the room costs that and nothing.
    constexpr int kRoomPolls = 40;
    bool wroteTail = false;
    if (!fadeTail_.empty()) {
        for (int poll = 0; poll < kRoomPolls; ++poll) {
            bool settled = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!client_ || !render_) break;
                uint32_t padding = 0;
                if (FAILED(client_->GetCurrentPadding(&padding))) break;
                if (bufferFrames_ > padding && bufferFrames_ - padding >= fadeFrames_) {
                    wroteTail = WriteLocked(fadeTail_.data(), fadeFrames_, false);
                    settled = true;
                }
            }
            if (settled) break;
            Sleep(1);
        }
    }

    uint64_t queuedThrough = 0;
    { std::lock_guard<std::mutex> lock(mutex_); queuedThrough = framesWritten_; }
    LOG("Audio: ramped stop - tail " << (fadeTail_.empty() ? "not needed (already silent)"
                                        : wroteTail ? "queued" : "REFUSED (no room in the endpoint buffer)")
        << ", " << fadeFrames_ << " frames, draining through " << queuedThrough << ".");

    // Let what is queued reach the speaker before the clock is stopped;
    // stopping first would cut the tail off along with everything else.
    // Bounded by the endpoint's own buffer plus generous slack.
    constexpr int kDrainPolls = 40;
    for (int poll = 0; poll < kDrainPolls; ++poll) {
        uint64_t played = 0;
        if (!PlayedFrames(played) || played >= queuedThrough) break;
        Sleep(2);
    }
    // IAudioClock leads the speaker. GetPosition reports what the engine has
    // consumed, not what has been converted, so stopping the instant it
    // reaches the end still truncates the last of the ramp - measured as a
    // residual step of 0.32 where the ramp should have left 0.0005. One more
    // buffer's worth of grace covers the difference. It is tens of
    // milliseconds, on a seek that already costs sixty.
    {
        uint32_t graceMs = 0;
        { std::lock_guard<std::mutex> lock(mutex_);
          if (format_.sampleRate)
              graceMs = uint32_t(uint64_t(bufferFrames_) * 1000u / format_.sampleRate) + 2u; }
        if (graceMs) Sleep(graceMs);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!client_ || !started_) return true;
    const HRESULT result = client_->Stop();
    if (FAILED(result)) { NoteDeviceLoss(result); return false; }
    started_ = false;
    return true;
}

bool WasapiRenderer::PlayedFrames(uint64_t& frames) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clock_ || !clockFrequency_ || !format_.sampleRate) return false;
    uint64_t position = 0;
    const HRESULT result = clock_->GetPosition(&position, nullptr);
    if (FAILED(result)) return false;
    // GetPosition counts in units of GetFrequency per second, which is the
    // byte rate for a PCM stream rather than the frame rate. Converting
    // through seconds is exact enough here and does not assume which.
    frames = static_cast<uint64_t>(
        (static_cast<long double>(position) / static_cast<long double>(clockFrequency_)) *
        static_cast<long double>(format_.sampleRate));
    // The null data an exclusive stream sent in the film's absence: counted
    // by the device clock, not part of the film. Only the part already played
    // comes off, so the clock never steps back when a range is queued.
    if (exclusive_) {
        // Past what was written the device is replaying a stale buffer - after
        // the end of the film, say, if the stream is resumed - which is not
        // the film moving on either.
        frames = std::min(frames, framesWritten_);
        uint64_t filler = fillerRetired_;
        for (const FillerRange& range : filler_)
            if (frames > range.start) filler += std::min(range.count, frames - range.start);
        frames = frames > filler ? frames - filler : 0;
    }
    return true;
}

void WasapiRenderer::SetVolume(float volume01)
{
    std::lock_guard<std::mutex> lock(mutex_);
    muted_ = !(volume01 > 0.0f);
    if (!volume_) return;
    const float clamped = std::clamp(volume01, 0.0f, 1.0f);
    if (FAILED(volume_->SetMasterVolume(clamped, nullptr)))
        LOG("Audio: setting the session volume failed.");
}

// Its callbacks touch nothing but its own latch, so unlike the renderer's
// watcher there is no owner to detach: a notification delivered after the
// RenderEndpointArrival is gone lands on an object the OS's own reference is
// still keeping alive, and sets a flag nobody reads.
class RenderEndpointArrival::Client final : public IMMNotificationClient {
public:
    std::atomic<bool> arrived{false};

    ULONG STDMETHODCALLTYPE AddRef() override { return references_.fetch_add(1) + 1; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = references_.fetch_sub(1) - 1;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object) return E_POINTER;
        if (riid != __uuidof(IUnknown) && riid != __uuidof(IMMNotificationClient)) {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        *object = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR deviceId) override
    {
        if (audio_endpoint::DefaultChangeMayRestore(flow, role, deviceId ? deviceId : L"")) arrived = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD newState) override
    {
        if (audio_endpoint::StateChangeMayRestore(newState)) arrived = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { arrived = true; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    ~Client() = default;
    std::atomic<ULONG> references_{1};
};

RenderEndpointArrival::~RenderEndpointArrival()
{
    if (registered_ && enumerator_) enumerator_->UnregisterEndpointNotificationCallback(client_);
    if (client_) client_->Release();
}

bool RenderEndpointArrival::Watch(bool checkNow)
{
    if (client_) return registered_;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator_));
    if (FAILED(result)) {
        LOG("Audio: cannot watch for a new endpoint (MMDeviceEnumerator hr=" << HexText(result)
            << "); sound will not come back on its own.");
        return false;
    }
    client_ = new Client();
    result = enumerator_->RegisterEndpointNotificationCallback(client_);
    if (FAILED(result)) {
        LOG("Audio: cannot watch for a new endpoint (register hr=" << HexText(result)
            << "); sound will not come back on its own.");
        return false;
    }
    registered_ = true;
    if (checkNow) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
            client_->arrived = true;
    }
    return true;
}

bool RenderEndpointArrival::Arrived() { return client_ && client_->arrived.exchange(false); }
