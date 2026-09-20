#pragma once

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <iostream>

// The gate every `gpu`-labelled smoke opens with.
//
// These tests were unrunnable anywhere but a developer's desk: CI runs
// `ctest -LE gpu` and no GPU runner exists, so 76 KB of DLSSGBackend.cpp and
// FrameGenerationPass.cpp carried no CI-executed assertion. Adding a runner is
// the fix, but on any machine without a GPU the suite has to SKIP rather than
// fail, or the runner's first red build is meaningless.
//
// The exit code is deliberately not 2. Every one of these smokes already
// returns 2 for ordinary failures - bad arguments, a missing ffmpeg.exe,
// unreadable media - so mapping 2 to "skipped" would hide exactly the
// regressions the runner exists to catch. 125 is the shell convention for
// "could not execute", and CachedExportTests already uses it here.
namespace gpu_test_gate {

inline constexpr int kSkipExitCode = 125;

// True when a D3D12-capable hardware adapter is present. Deliberately does not
// create a device or ask about DLSS: a machine WITH an RTX card whose driver
// refuses the feature is a failure worth seeing, not a skip.
inline bool HardwareAdapterPresent()
{
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;
         factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++index) {
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter->GetDesc1(&description)) &&
            !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            return true;
        adapter.Reset();
    }
    return false;
}

// Returns kSkipExitCode and explains itself when there is no adapter, or 0 to
// carry on. Callers write:
//     if (const int skip = gpu_test_gate::SkipWithoutGpu()) return skip;
inline int SkipWithoutGpu()
{
    if (HardwareAdapterPresent()) return 0;
    std::cout << "skipped: no D3D12 hardware adapter on this machine\n";
    return kSkipExitCode;
}

} // namespace gpu_test_gate
