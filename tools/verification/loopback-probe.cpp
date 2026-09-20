// Records the default render endpoint via WASAPI loopback and reports whether
// anything was actually audible.
//
// Written for the waveOut -> WASAPI change. AudioClockSmoke asserts the clock
// contract, which a completely silent renderer would also satisfy: frames are
// still "played" if they are all zeroes, and the position still advances. The
// swapchain regression was exactly that shape - every measurement green, the
// feature dead - so the output is measured rather than assumed.
//
// Not a ctest: loopback captures whatever else the machine is playing, and a
// muted endpoint is not a broken player. It is an instrument for a person
// making a change to the audio path.
//
// Build:
//   cl /nologo /std:c++20 /EHsc /W4 tools\verification\loopback-probe.cpp ole32.lib
// Run (while something is playing):
//   loopback-probe.exe [seconds]

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv)
{
    const double seconds = argc > 1 ? _wtof(argv[1]) : 3.0;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) { std::puts("COM failed"); return 2; }

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* mix = nullptr;

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
        FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) ||
        FAILED(client->GetMixFormat(&mix)) || !mix) {
        std::puts("could not open the render endpoint for loopback");
        CoUninitialize();
        return 2;
    }

    const bool isFloat =
        mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat,
                     KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    std::printf("endpoint: %lu Hz, %u channels, %u bits, float=%d\n",
                mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample, int(isFloat));

    // Ten seconds of headroom so a slow reader never drops samples.
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  10'000'000, 0, mix, nullptr)) ||
        FAILED(client->GetService(IID_PPV_ARGS(&capture))) || FAILED(client->Start())) {
        std::puts("loopback capture could not start");
        CoTaskMemFree(mix);
        CoUninitialize();
        return 2;
    }

    const uint32_t channels = mix->nChannels;
    double peak = 0.0, sumSquares = 0.0;
    uint64_t samples = 0, silentFrames = 0, totalFrames = 0;

    const auto started = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() < seconds) {
        uint32_t packet = 0;
        if (FAILED(capture->GetNextPacketSize(&packet))) break;
        if (!packet) { Sleep(5); continue; }
        BYTE* data = nullptr;
        uint32_t frames = 0;
        DWORD flags = 0;
        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        totalFrames += frames;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            silentFrames += frames;
        } else if (isFloat && mix->wBitsPerSample == 32) {
            const auto* values = reinterpret_cast<const float*>(data);
            for (uint32_t i = 0; i < frames * channels; ++i) {
                const double value = values[i];
                peak = std::max(peak, std::abs(value));
                sumSquares += value * value;
                ++samples;
            }
        }
        capture->ReleaseBuffer(frames);
    }
    client->Stop();
    CoTaskMemFree(mix);
    CoUninitialize();

    const double rms = samples ? std::sqrt(sumSquares / double(samples)) : 0.0;
    std::printf("frames=%llu silent=%llu peak=%.6f rms=%.6f (%.1f dBFS)\n",
                (unsigned long long)totalFrames, (unsigned long long)silentFrames,
                peak, rms, rms > 0 ? 20.0 * std::log10(rms) : -999.0);

    // -60 dBFS is far below anything audible as content and far above the
    // numerical floor of a genuinely silent stream.
    if (rms > 0.001) { std::puts("AUDIBLE"); return 0; }
    std::puts("SILENT");
    return 1;
}
