// Opt-in hardware probe: registered under the `gpu` CTest label, which the
// portable suite excludes (`ctest -LE gpu`) and an RTX machine opts into with
// `ctest -L gpu`.
//
// It answers one question and reports the answer: does the raw NGX path admit
// NVSDK_NGX_Feature_FrameGeneration on this machine, with no Streamline
// anywhere in the process? A refusal by the runtime is a verdict - it is the
// answer this probe exists to obtain - so a refusal prints its reason and
// exits 0 like an admission does. A non-zero exit means only that no verdict
// could be reached at all: no D3D12 device to probe on, or NGX itself never
// initialized.
#include <windows.h>

#include "DLSSGBackend.h"
#include "NeuralPreflight.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kProbeWidth = 1920;
constexpr uint32_t kProbeHeight = 1080;
constexpr DXGI_FORMAT kProbeFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

std::string Narrow(std::wstring_view text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string narrow(size_t(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), narrow.data(), length, nullptr, nullptr);
    return narrow;
}

// Which nvngx_dlssg.dll the runtime ended up loading, which is the measured
// difference between a refusal and an admission on this machine: with none
// beside the executable the create answers 0xbad0000b, with the vendored one
// staged beside it the same create succeeds.
std::string LoadedDlssgSnippet()
{
    const HMODULE snippet = GetModuleHandleW(L"nvngx_dlssg.dll");
    if (!snippet) return "absent";
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(snippet, path, MAX_PATH);
    return length ? Narrow(std::wstring_view(path, length)) : std::string("loaded");
}

// The question is about the raw NGX path, so the report says outright which
// Streamline modules were in the process when the runtime answered.
std::string LoadedStreamlineModules()
{
    static constexpr const wchar_t* kModules[] = {L"sl.interposer.dll", L"sl.dlss_g.dll", L"sl.common.dll",
                                                  L"sl.pcl.dll", L"sl.reflex.dll"};
    std::string loaded;
    for (const wchar_t* name : kModules) {
        if (!GetModuleHandleW(name)) continue;
        if (!loaded.empty()) loaded += ',';
        loaded += Narrow(name);
    }
    return loaded.empty() ? std::string("none") : loaded;
}

// The renderer's adapter choice, which is what the player would probe on:
// DXGI's high-performance order, software adapters skipped, an NVIDIA one
// preferred over the first that merely creates a device.
ComPtr<IDXGIAdapter1> SelectAdapter(IDXGIFactory6* factory)
{
    ComPtr<IDXGIAdapter1> fallback;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr))) continue;
        if (!fallback) fallback = adapter;
        if (desc.VendorId == 0x10DE) return adapter;
    }
    return fallback;
}

} // namespace

int wmain()
{
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        std::cout << "probe=unreachable reason=CreateDXGIFactory2 failed\n";
        return 2;
    }
    ComPtr<IDXGIAdapter1> adapter = SelectAdapter(factory.Get());
    if (!adapter) {
        std::cout << "probe=unreachable reason=no D3D12 hardware adapter\n";
        return 2;
    }
    DXGI_ADAPTER_DESC1 adapterDesc{};
    adapter->GetDesc1(&adapterDesc);

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
        std::cout << "probe=unreachable reason=D3D12CreateDevice failed\n";
        return 2;
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&cmd))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        std::cout << "probe=unreachable reason=command queue/list bring-up failed\n";
        return 2;
    }

    DLSSGBackend backend;
    const DLSSGCapability capability = backend.Probe(device.Get(), cmd.Get(), kProbeWidth, kProbeHeight, kProbeFormat);

    // Whatever the create recorded on this list is submitted and waited out, so
    // a create that the runtime accepted but the GPU could not execute shows up
    // as a removed device below instead of being reported as an admission.
    HRESULT submitted = cmd->Close();
    if (SUCCEEDED(submitted)) {
        ID3D12CommandList* lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        submitted = queue->Signal(fence.Get(), 1);
    }
    if (SUCCEEDED(submitted)) {
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (done) {
            if (SUCCEEDED(fence->SetEventOnCompletion(1, done))) WaitForSingleObject(done, 10000);
            CloseHandle(done);
        }
    }

    std::cout << "adapter=" << Narrow(adapterDesc.Description) << "\n"
              << "probeGeometry=" << kProbeWidth << "x" << kProbeHeight << " format=" << int(kProbeFormat) << "\n"
              << "available=" << (capability.available ? "true" : "false") << "\n"
              << "multiFrameCountMax=" << capability.multiFrameCountMax << "\n"
              << "hagsEnabled=" << (capability.hagsEnabled ? "true" : "false") << "\n"
              << "createResult=" << HexResultText(uint32_t(capability.createResult)) << "\n"
              << "detail=" << Narrow(capability.detail) << "\n"
              << "dlssgSnippet=" << LoadedDlssgSnippet() << "\n"
              << "streamlineModules=" << LoadedStreamlineModules() << "\n"
              << "commandListSubmit=0x" << std::hex << uint32_t(submitted) << std::dec << "\n"
              << "deviceRemovedReason=0x" << std::hex << uint32_t(device->GetDeviceRemovedReason()) << std::dec
              << "\n";

    // NGX never coming up is the one outcome that is not an answer about Frame
    // Generation, so it is the only failure this probe reports.
    if (!backend.SessionEstablished()) {
        std::cout << "probe=unreachable reason=NGX did not initialize on this device\n";
        return 3;
    }
    return 0;
}
