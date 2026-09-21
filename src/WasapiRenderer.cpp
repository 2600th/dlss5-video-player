#include "WasapiRenderer.h"

#include "AudioFadePolicy.h"
#include "Log.h"

#include <audiopolicy.h>
#include <mmreg.h>

#include <algorithm>
#include <cstring>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

// Shared-mode event-driven streams take the engine's own period when the
// requested duration is zero, which is what the endpoint is tuned for. Naming
// a number here only risks asking for something the driver then rounds.
constexpr REFERENCE_TIME kEnginePeriod = 0;

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

} // namespace

WasapiRenderer::~WasapiRenderer() { Close(); }

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

    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator_));
    if (FAILED(result)) { LOG("Audio: MMDeviceEnumerator failed hr=0x" << std::hex << result); return false; }

    result = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(result)) { LOG("Audio: no default render endpoint hr=0x" << std::hex << result); return false; }

    result = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
    if (FAILED(result)) { LOG("Audio: IAudioClient activation failed hr=0x" << std::hex << result); return false; }

    WAVEFORMATEX* mix = nullptr;
    result = client_->GetMixFormat(&mix);
    if (FAILED(result) || !mix) { LOG("Audio: GetMixFormat failed hr=0x" << std::hex << result); return false; }
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
    if (FAILED(result)) { LOG("Audio: IAudioClient::Initialize failed hr=0x" << std::hex << result); return false; }

    ready_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready_) { LOG("Audio: render event creation failed winerr=" << GetLastError()); return false; }
    result = client_->SetEventHandle(ready_);
    if (FAILED(result)) { LOG("Audio: SetEventHandle failed hr=0x" << std::hex << result); return false; }

    result = client_->GetBufferSize(&bufferFrames_);
    if (FAILED(result)) { LOG("Audio: GetBufferSize failed hr=0x" << std::hex << result); return false; }

    fadeFrames_ = audio_fade::FrameCount(format_.sampleRate);
    lastFrame_.assign(format_.channels, 0.0f);
    framesFadedIn_ = 0;
    framesWritten_ = 0;
    refusingWrites_ = false;

    result = client_->GetService(IID_PPV_ARGS(&render_));
    if (FAILED(result)) { LOG("Audio: IAudioRenderClient failed hr=0x" << std::hex << result); return false; }

    result = client_->GetService(IID_PPV_ARGS(&clock_));
    if (FAILED(result)) { LOG("Audio: IAudioClock failed hr=0x" << std::hex << result); return false; }
    result = clock_->GetFrequency(&clockFrequency_);
    if (FAILED(result) || clockFrequency_ == 0) {
        LOG("Audio: IAudioClock::GetFrequency failed hr=0x" << std::hex << result);
        return false;
    }

    // Volume is per-session, so this moves the player's own slider in the
    // mixer rather than the endpoint's master level. waveOutSetVolume did the
    // same thing.
    if (FAILED(client_->GetService(IID_PPV_ARGS(&volume_))))
        LOG("Audio: ISimpleAudioVolume unavailable; the volume slider will not reach the mixer.");

    LOG("Audio: WASAPI shared mode at " << format_.sampleRate << " Hz, " << format_.channels
        << " channels, 32-bit float; endpoint buffer " << bufferFrames_ << " frames ("
        << (double(bufferFrames_) * 1000.0 / double(format_.sampleRate)) << " ms).");
    return true;
}

void WasapiRenderer::Close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_ && started_) client_->Stop();
    started_ = false;
    volume_.Reset();
    clock_.Reset();
    render_.Reset();
    client_.Reset();
    device_.Reset();
    enumerator_.Reset();
    if (ready_) { CloseHandle(ready_); ready_ = nullptr; }
    format_ = {};
    bufferFrames_ = 0;
    clockFrequency_ = 0;
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
    { std::lock_guard<std::mutex> lock(mutex_); ready = ready_; }
    if (!ready) return false;
    // A timeout is not a failure: the caller polls its stop flag between
    // waits, and a paused stream never signals.
    if (WaitForSingleObject(ready, timeoutMilliseconds) != WAIT_OBJECT_0) return true;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!client_) return false;
    uint32_t padding = 0;
    const HRESULT result = client_->GetCurrentPadding(&padding);
    if (FAILED(result)) { NoteDeviceLoss(result); return false; }
    framesWanted = bufferFrames_ > padding ? bufferFrames_ - padding : 0;
    return true;
}

bool WasapiRenderer::Write(const void* frames, uint32_t framesToWrite)
{
    if (!framesToWrite) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!render_) return false;
    // A tail has been queued and the stream is on its way down. Accepting more
    // would put a step back in after the ramp that removed it.
    if (refusingWrites_) return true;
    return WriteLocked(frames, framesToWrite, true);
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
    return true;
}

bool WasapiRenderer::FadeOutAndStop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!client_) return false;
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
    return true;
}

void WasapiRenderer::SetVolume(float volume01)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!volume_) return;
    const float clamped = std::clamp(volume01, 0.0f, 1.0f);
    if (FAILED(volume_->SetMasterVolume(clamped, nullptr)))
        LOG("Audio: setting the session volume failed.");
}
