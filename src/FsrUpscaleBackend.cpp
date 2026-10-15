// FsrUpscaleBackend.cpp — AMD FSR Upscaling 4.1.1 as the playback upscaler.
//
// Two things are worth knowing before reading the call sequence.
//
// The first is that FSR is a *temporal* upscaler built for a renderer, and a
// decoded frame is not a rendered one: there is no jitter, no depth, no motion
// vectors, no exposure and no camera. The class comment in the header says how
// each is supplied; every one of them is a constant, and the constants are
// written on the GPU rather than uploaded, because the frame generation's
// hardest-won lesson here is that touching a staging buffer (Map/Unmap) while an
// FFX context is alive takes the device down on the next execute.
//
// The second is that this backend is a replacement for a filter, not for a
// contract: it is handed the same linear FP16 frame at the same size and writes
// the same target the Catmull-Rom pass wrote, so "which upscaler ran" is one
// branch in the renderer and the sharpen, the present slot and everything above
// them never learn which one it was.

#include "FsrUpscaleBackend.h"

#include "FsrFrameGenAbi.h"
#include "FsrUpscaleAbi.h"

#include <atomic>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

// The shared ABI (context handle, resource description, entry-point types and
// the resource-state/usage bit flags) is the frame generation's declaration:
// both effects are the same C API. Only the TYPES are imported: the state,
// usage and descriptor-type values below are preprocessor macros, and a using
// declaration for one expands to `using ns::(1 << 2);` - the compiler reads
// the parenthesis as the name and stops.
using fsr_fg::ffxContext;
using fsr_fg::ffxCreateBackendDX12Desc;
using fsr_fg::kFfxReturnOk;
using fsr_fg::PfnFfxCreateContext;
using fsr_fg::PfnFfxDestroyContext;
using fsr_fg::PfnFfxDispatch;

using fsr_up::ffxCreateContextDescUpscale;
using fsr_up::ffxCreateContextDescUpscaleVersion;
using fsr_up::ffxDispatchDescUpscale;
using fsr_up::kApiVersion;

namespace {

// The loader is the official entry point (see FsrFrameGenBackend.cpp: it brings
// the implementation up with the dispatch tables its configure/dispatch call
// need), and it is the same DLL for every effect - the effect the context asks
// for is what selects the implementation it loads.
constexpr wchar_t kLoaderName[] = L"amd_fidelityfx_loader_dx12.dll";
// The implementation the loader brings up for the UPSCALE effect. Its absence
// is a much more informative failure than "CreateContext answered 0x..." with
// no hint that a file is missing, so it is checked up front.
constexpr wchar_t kUpscalerName[] = L"amd_fidelityfx_upscaler_dx12.dll";

std::vector<fs::path> SearchDirectories()
{
    std::vector<fs::path> directories;
    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0)
        directories.push_back(fs::path(exe).parent_path());
    if (wchar_t sdk[MAX_PATH]{}; GetEnvironmentVariableW(L"FFX_SDK_ROOT", sdk, MAX_PATH) > 0)
        directories.push_back(fs::path(sdk) / L"Kits" / L"FidelityFX" / L"signedbin");
    // A developer's SDK clone, so a locally built player picks it up without
    // staging anything (the frame generation searches the same place).
    directories.push_back(fs::path(L"F:/dlss5-video/FidelityFX-SDK/Kits/FidelityFX/signedbin"));
    return directories;
}

std::wstring ToWide(const char* text)
{
    if (!text) return {};
    return std::wstring(text, text + std::strlen(text));
}

std::wstring Hex(uint32_t value)
{
    wchar_t buffer[16];
    std::swprintf(buffer, 16, L"0x%08x", value);
    return std::wstring(buffer);
}

// The runtime's own diagnostics (the DEBUG_CHECKING flag is what makes it emit
// them): a wrong unit or an implausible camera reaches the log instead of
// failing silently. It may be called from the runtime's threads.
void FFX_API UpscaleMessage(uint32_t type, const wchar_t* message)
{
    if (!message) return;
    // Not logged here: the backend has no logger of its own on purpose (the
    // frame generation has none either). The renderer reports the outcome; a
    // per-dispatch callback would flood the log. Captured for the first few,
    // which is where an integration mistake shows up.
    static std::atomic<uint32_t> g_seen{0};
    if (g_seen.fetch_add(1u) < 8u)
        OutputDebugStringW((std::wstring(L"[fsr4] (") + std::to_wstring(type) + L") " +
                            message + L"\n").c_str());
}

PfnFfxCreateContext g_createContext = nullptr;
PfnFfxDestroyContext g_destroyContext = nullptr;
PfnFfxDispatch g_dispatch = nullptr;

template <typename Descriptor>
fsr_fg::ffxApiHeader* NextHeader(Descriptor& descriptor)
{
    return &descriptor.header;
}

D3D12_RESOURCE_DESC TextureDesc(uint32_t width, uint32_t height, DXGI_FORMAT format)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // RENDER_TARGET because the constants are written with a clear (see the
    // header): a clear is a GPU write with no staging buffer, and the TDR the
    // frame generation mapped its way into cannot happen without one.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return desc;
}

} // namespace

FsrUpscaleBackend::~FsrUpscaleBackend() { Shutdown(); }

std::wstring FsrUpscaleBackend::RuntimeSearchDescription()
{
    std::wstring text;
    for (const auto& directory : SearchDirectories()) {
        if (!text.empty()) text += L", ";
        text += directory.wstring();
    }
    return text;
}

bool FsrUpscaleBackend::RuntimePresent(std::wstring* foundPath, std::wstring* detail)
{
    std::error_code ec;
    std::wstring loader;
    for (const auto& directory : SearchDirectories()) {
        if (fs::is_regular_file(directory / kLoaderName, ec)) {
            loader = (directory / kLoaderName).wstring();
            break;
        }
    }
    if (loader.empty()) {
        if (detail)
            *detail = L"no " + std::wstring(kLoaderName) + L" in: " + RuntimeSearchDescription();
        return false;
    }
    // The loader without its UPSCALE implementation still loads, and then
    // CreateContext fails with a code that says nothing about a missing file.
    const fs::path sibling = fs::path(loader).parent_path() / kUpscalerName;
    if (!fs::is_regular_file(sibling, ec)) {
        if (detail)
            *detail = std::wstring(kUpscalerName) + L" is not beside " + loader +
                      L". Copy it from FidelityFX-SDK/Kits/FidelityFX/signedbin.";
        return false;
    }
    if (foundPath) *foundPath = loader;
    if (detail) detail->clear();
    return true;
}

bool FsrUpscaleBackend::LoadRuntime()
{
    if (m_module) return true;

    std::wstring path, detail;
    if (!RuntimePresent(&path, &detail)) {
        m_lastError = 0x80070002;   // ERROR_FILE_NOT_FOUND
        m_lastDetail = L"AMD FSR upscaling was not found. " + detail;
        return false;
    }

    m_module = LoadLibraryExW(path.c_str(), nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m_module) {
        m_lastError = static_cast<uint32_t>(GetLastError());
        m_lastDetail = L"Loading " + path + L" failed with Win32 error " +
                       std::to_wstring(GetLastError()) + L".";
        return false;
    }
    g_createContext = reinterpret_cast<PfnFfxCreateContext>(
        GetProcAddress(m_module, "ffxCreateContext"));
    g_destroyContext = reinterpret_cast<PfnFfxDestroyContext>(
        GetProcAddress(m_module, "ffxDestroyContext"));
    g_dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(m_module, "ffxDispatch"));
    if (!g_createContext || !g_dispatch) {
        m_lastDetail = path + L" does not export the FidelityFX C API "
                              L"(ffxCreateContext/ffxDispatch).";
        m_lastError = 0x8007007E;   // ERROR_PROC_NOT_FOUND
        FreeLibrary(m_module);
        m_module = nullptr;
        return false;
    }
    m_lastError = 0;
    m_lastDetail.clear();
    return true;
}

bool FsrUpscaleBackend::CreateContext(ID3D12Device* device)
{
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.header.pNext = nullptr;
    backend.device = device;

    // Mandatory since SDK 2.1: without it the runtime has no API version to
    // check against and CreateContext refuses the descriptor.
    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.header.pNext = NextHeader(backend);
    version.version = kApiVersion;

    ffxCreateContextDescUpscale create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.header.pNext = NextHeader(version);
    // HIGH_DYNAMIC_RANGE because the present chain's colour texture is FP16
    // linear light and an HDR source's highlights land well above 1.0 in it:
    // PqToLinear divides by SDR white, so a 1000-nit highlight is ~5.0. The
    // vendor's rule is that the flag goes with an extended-range buffer; what
    // it changes is where the history clamping happens (tonemapped space,
    // via an exactly invertible x/(max+1) pair, so SDR-range content is
    // unaffected). Without it the clamping runs on raw values.
    // AUTO_EXPOSURE is NOT set: the exposure is the constant 1.0 texture
    // below, so a bright scene cannot be read as an exposure change.
    // NON_LINEAR_COLORSPACE is NOT set: the input is linear light, and the
    // flag is what a display-encoded input would declare.
    // DEBUG_CHECKING is a bring-up affordance: it makes the runtime report
    // API misuse (wrong frameTimeDelta unit, implausible camera) through
    // fpMessage instead of failing silently.
    // DYNAMIC_RESOLUTION because the window is not the source's size and can
    // change under the context: the render size is the source and the upscale
    // size is the rung, and they are stated per dispatch.
    create.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE |
                   FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION |
                   FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    create.maxRenderSize = {m_renderW, m_renderH};
    create.maxUpscaleSize = {m_outputW, m_outputH};
    create.fpMessage = &UpscaleMessage;

    const int32_t rc = g_createContext(&m_context, &create.header, nullptr);
    if (rc != kFfxReturnOk || !m_context) {
        m_context = nullptr;
        m_lastError = static_cast<uint32_t>(rc);
        m_lastDetail = L"ffxCreateContext(Upscale) answered " + Hex(static_cast<uint32_t>(rc)) +
                       L". FSR 4 needs an RX 7000/9000 series discrete GPU and Shader "
                       L"Model 6.6 (DirectX 12 Agility SDK 1.4.9 or later); either one "
                       L"missing looks exactly like this.";
        return false;
    }
    return true;
}

bool FsrUpscaleBackend::CreateGuides(ID3D12Device* device)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    const D3D12_RESOURCE_DESC depthDesc =
        TextureDesc(m_renderW, m_renderH, DXGI_FORMAT_R32_FLOAT);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depthDesc,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(m_depth.GetAddressOf()))))
        return false;
    const D3D12_RESOURCE_DESC motionDesc =
        TextureDesc(m_renderW, m_renderH, DXGI_FORMAT_R16G16_FLOAT);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &motionDesc,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(m_motion.GetAddressOf()))))
        return false;
    const D3D12_RESOURCE_DESC exposureDesc = TextureDesc(1, 1, DXGI_FORMAT_R32_FLOAT);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &exposureDesc,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(m_exposure.GetAddressOf()))))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeap{};
    rtvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeap.NumDescriptors = 3;
    if (FAILED(device->CreateDescriptorHeap(&rtvHeap,
                                            IID_PPV_ARGS(m_guideRtvHeap.GetAddressOf()))))
        return false;
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = m_guideRtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(m_depth.Get(), nullptr, base);
    D3D12_CPU_DESCRIPTOR_HANDLE second = base;
    second.ptr += increment;
    device->CreateRenderTargetView(m_motion.Get(), nullptr, second);
    D3D12_CPU_DESCRIPTOR_HANDLE third = base;
    third.ptr += 2ull * increment;
    device->CreateRenderTargetView(m_exposure.Get(), nullptr, third);

    // A list of this backend's own: the renderer has no command list at the
    // point the rung is built, and these three textures are written once.
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(allocator.GetAddressOf()))))
        return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                         nullptr, IID_PPV_ARGS(list.GetAddressOf()))))
        return false;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.GetAddressOf()))))
        return false;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()))))
        return false;

    ID3D12Resource* resources[3] = {m_depth.Get(), m_motion.Get(), m_exposure.Get()};
    const float clears[3][4] = {{1.0f, 1.0f, 1.0f, 1.0f},    // flat far depth
                               {0.0f, 0.0f, 0.0f, 0.0f},     // no motion anywhere
                               {1.0f, 1.0f, 1.0f, 1.0f}};    // unit exposure
    for (int index = 0; index < 3; ++index) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resources[index];
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
        D3D12_CPU_DESCRIPTOR_HANDLE target = base;
        target.ptr += static_cast<UINT64>(index) * increment;
        list->ClearRenderTargetView(target, clears[index], 0, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        // The state every dispatch declares these in.
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
    }
    if (FAILED(list->Close())) return false;
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence.Get(), 1))) return false;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    if (FAILED(fence->SetEventOnCompletion(1, event))) {
        CloseHandle(event);
        return false;
    }
    WaitForSingleObject(event, 2000);
    CloseHandle(event);
    return true;
}

bool FsrUpscaleBackend::Initialize(ID3D12Device* device, uint32_t renderW, uint32_t renderH,
                                   uint32_t outputW, uint32_t outputH, DXGI_FORMAT colorFormat,
                                   DXGI_FORMAT outputFormat)
{
    m_renderW = renderW;
    m_renderH = renderH;
    m_outputW = outputW;
    m_outputH = outputH;
    m_colorFormat = colorFormat;
    m_outputFormat = outputFormat;
    m_lastDetail.clear();
    m_lastError = 0;

    if (!device || renderW == 0 || renderH == 0 || outputW < renderW || outputH < renderH) {
        m_lastError = 0x80070057;   // ERROR_INVALID_PARAMETER
        m_lastDetail = L"FSR upscaling needs a device and an output rung at least "
                       L"as large as the source.";
        return false;
    }
    // Guides first: they are written with a clear and a queue of this backend's
    // own, so nothing has to reach into the renderer's frame to make them.
    if (!CreateGuides(device)) {
        m_lastError = 0x8007000E;   // ERROR_OUTOFMEMORY
        m_lastDetail = L"FSR upscaling could not allocate its depth/motion/exposure "
                       L"textures.";
        Shutdown();
        return false;
    }
    if (!LoadRuntime()) return false;
    if (!CreateContext(device)) {
        Shutdown();
        return false;
    }
    return true;
}

bool FsrUpscaleBackend::Dispatch(ID3D12GraphicsCommandList* cmd, ID3D12Resource* color,
                                 ID3D12Resource* output, bool reset, float frameTimeMs)
{
    if (!m_context || !g_dispatch || m_failed) return false;

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.header.pNext = nullptr;
    dispatch.commandList = cmd;
    dispatch.color = fsr_fg::GetResourceDX12(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = fsr_fg::GetResourceDX12(m_depth.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ,
                                             FFX_API_RESOURCE_USAGE_DEPTHTARGET);
    dispatch.motionVectors =
        fsr_fg::GetResourceDX12(m_motion.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.exposure = fsr_fg::GetResourceDX12(m_exposure.Get(),
                                                FFX_API_RESOURCE_STATE_COMPUTE_READ);
    // Absent, which the API spells as an empty resource: with a flat depth and
    // zero motion there is nothing to mark reactive, and FSR's own shading-
    // change term is what then keeps history off pixels that moved.
    dispatch.reactive = fsr_fg::FfxApiResource{};
    dispatch.transparencyAndComposition = fsr_fg::FfxApiResource{};
    dispatch.output = fsr_fg::GetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
                                              FFX_API_RESOURCE_USAGE_UAV);
    // Zero jitter, reported as zero: the source is one fixed sample grid, and
    // shifting it per frame would be a wobble the (identity) reprojection
    // cannot undo. See the header.
    dispatch.jitterOffset = {0.0f, 0.0f};
    dispatch.motionVectorScale = {1.0f, 1.0f};
    dispatch.renderSize = {m_renderW, m_renderH};
    dispatch.upscaleSize = {m_outputW, m_outputH};
    // The present chain's own CAS runs after this and owns sharpening.
    dispatch.enableSharpening = false;
    dispatch.sharpness = 0.0f;
    dispatch.frameTimeDelta = frameTimeMs > 0.0f ? frameTimeMs : 16.6667f;
    dispatch.preExposure = 1.0f;
    dispatch.reset = reset;
    // A plausible camera for a depth buffer that is flat: nothing is derived
    // from it here, but the runtime's debug checking rejects an implausible one.
    dispatch.cameraNear = 0.1f;
    dispatch.cameraFar = 1000.0f;
    dispatch.cameraFovAngleVertical = 1.04719755f;   // 60 degrees
    dispatch.viewSpaceToMetersFactor = 1.0f;
    dispatch.flags = 0;

    const int32_t rc = g_dispatch(&m_context, &dispatch.header);
    if (rc != kFfxReturnOk) {
        m_lastError = static_cast<uint32_t>(rc);
        m_lastDetail = L"ffxDispatch(Upscale) answered " + Hex(static_cast<uint32_t>(rc)) + L".";
        // A failed dispatch is a machine-level answer, not a transient one: the
        // caller drops this backend and keeps the filter it replaced.
        m_failed = true;
        return false;
    }
    ++m_dispatches;
    return true;
}

void FsrUpscaleBackend::Shutdown()
{
    if (m_context && g_destroyContext) {
        g_destroyContext(&m_context, nullptr);
        m_context = nullptr;
    }
    m_depth.Reset();
    m_motion.Reset();
    m_exposure.Reset();
    m_guideRtvHeap.Reset();
    if (m_module) {
        FreeLibrary(m_module);
        m_module = nullptr;
    }
}
