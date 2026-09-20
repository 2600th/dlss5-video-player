#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <iostream>

// The gate every `audio`-labelled test opens with, mirroring GpuTestGate.
//
// A hosted CI runner has no audio endpoint at all, and a machine with no
// speakers is not a machine with a broken player. These tests SKIP there
// rather than failing, for the same reason and with the same exit code the
// GPU smokes use.
namespace audio_test_gate {

inline constexpr int kSkipExitCode = 125;

// True when a default render endpoint exists. Deliberately does not open a
// stream: a machine WITH an endpoint whose driver refuses the format is a
// failure worth seeing, not a skip.
inline bool RenderEndpointPresent()
{
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))))
        return false;
    Microsoft::WRL::ComPtr<IMMDevice> device;
    return SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) && device;
}

// Callers write:
//     if (const int skip = audio_test_gate::SkipWithoutRenderDevice()) return skip;
// COM must already be initialized on this thread.
inline int SkipWithoutRenderDevice()
{
    if (RenderEndpointPresent()) return 0;
    std::cout << "skipped: no default audio render endpoint on this machine\n";
    return kSkipExitCode;
}

} // namespace audio_test_gate
