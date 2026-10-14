#include "FsrFrameGenBackend.h"

#include "FsrFrameGenAbi.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <dxgi1_6.h>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

// The LOADER is the official entry point: applications load it, and it brings
// up the implementation (amd_fidelityfx_framegeneration_dx12.dll) with the
// initialization its configure/dispatch dispatch tables require. Loading the
// implementation directly left those tables half-initialized - the first
// ffxConfigure then walked a context object graph containing -1 sentinels and
// faulted (access at 0xFFFFFFFFFFFFFFFF, inside the runtime's own code).
constexpr wchar_t kDllName[] = L"amd_fidelityfx_loader_dx12.dll";

// Where to look, in order. The exe directory first, because a packaged player
// carries the runtime beside itself (that is how AMD ships it and how the SDK's
// samples stage it); then the two layouts a developer's FidelityFX-SDK clone
// produces, so a locally built build-ninja picks it up without extra copying.
std::vector<std::wstring> SearchPaths()
{
    std::vector<std::wstring> paths;
    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
        const fs::path dir = fs::path(exe).parent_path();
        paths.push_back((dir / kDllName).wstring());
        // The loader DLL and its siblings are sometimes staged under a component
        // subdirectory instead of the root.
        paths.push_back((dir / L"framegeneration" / kDllName).wstring());
    }
    if (wchar_t sdk[MAX_PATH]{}; GetEnvironmentVariableW(L"FFX_SDK_ROOT", sdk, MAX_PATH) > 0) {
        const fs::path root(sdk);
        paths.push_back((root / L"Kits" / L"FidelityFX" / L"signedbin" / kDllName).wstring());
    }
    paths.push_back(fs::path(L"F:/dlss5-video/FidelityFX-SDK/Kits/FidelityFX/signedbin") /
                    kDllName);
    return paths;
}

// ASCII (what the loader and the API diagnostics emit) to wide, without
// dragging <locale> or a code-page guess in - the same helper the converter uses.
std::wstring ToWide(const char* text)
{
    if (!text) return {};
    return std::wstring(text, text + std::strlen(text));
}

// ffxApiHeader::pNext is an ffxApiHeader*, so linking a chain means taking the
// address of the next descriptor's header - not the descriptor itself.
template <typename Descriptor>
fsr_fg::ffxApiHeader* NextHeader(Descriptor& descriptor)
{
    return &descriptor.header;
}

std::wstring DescribeResult(const char* call, int32_t code)
{
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "0x%08x", static_cast<unsigned>(code));
    return ToWide(call) + L" answered " + ToWide(buffer);
}

D3D12_RESOURCE_DESC DescribeTexture(uint32_t width, uint32_t height, DXGI_FORMAT format,
                                    D3D12_RESOURCE_FLAGS flags)
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
    desc.Flags = flags;
    return desc;
}

// The island's lists are short and self-contained, so the barrier bookkeeping
// is a helper here rather than the caller's remembered-state scheme: every
// island submission starts from COMMON and returns there.
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

} // namespace

namespace fsr_fg {

// description.format is an FfxApiSurfaceFormat, NOT a DXGI_FORMAT - the two
// enums share names but not values, and transcribing the DXGI number verbatim
// made the runtime read the renderer's FP16 linear frame as R8G8B8A8_UNORM
// (DXGI 10 happens to be FFX's RGBA8): garbage colours and a useless motion
// estimate, which is exactly the ghosted, broken output this was first seen
// as. The playback pass-through in the offline conversion (DXGI BGRA8 = 87)
// was likewise out of the runtime's enum entirely. Only the formats this
// backend actually receives are mapped; anything else reports UNKNOWN rather
// than a number the runtime will misread.
uint32_t FfxFormatFromDxgi(DXGI_FORMAT format)
{
    switch (format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_B8G8R8A8_UNORM:     return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM:  return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R32_FLOAT:          return FFX_API_SURFACE_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16G16_FLOAT:       return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
        default:                             return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

FfxApiResource GetResourceDX12(ID3D12Resource* resource, uint32_t state, uint32_t usages)
{
    FfxApiResource result{};
    result.resource = resource;
    result.state = state;
    if (!resource) return result;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    // The v4 description is flat (type/format/width/height/depth/mipCount/
    // flags/usage); every field below is placed for THAT layout. The first
    // transcription filled a nested 28-byte variant, so the runtime read a
    // 1x1 texture with a garbage type where a full-size one was declared -
    // tolerated by WARP, a device removal on the real driver.
    result.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    result.description.format = FfxFormatFromDxgi(desc.Format);
    result.description.width = static_cast<uint32_t>(desc.Width);
    result.description.height = desc.Height;
    result.description.depth = 1;
    result.description.mipCount = desc.MipLevels;
    result.description.flags = 0;
    result.description.usage = usages;
    return result;
}

} // namespace fsr_fg

using fsr_fg::ffxCreateBackendDX12Desc;
using fsr_fg::ffxCreateContextDescFrameGeneration;
using fsr_fg::ffxCreateContextDescFrameGenerationVersion;
using fsr_fg::ffxCreateContextDescHeader;
using fsr_fg::ffxDispatchDescFrameGeneration;
using fsr_fg::ffxDispatchDescFrameGenerationPrepareV2;
using fsr_fg::ffxDispatchDescHeader;
using fsr_fg::GetResourceDX12;
using fsr_fg::kFfxReturnOk;
using fsr_fg::PfnFfxCreateContext;
using fsr_fg::PfnFfxDestroyContext;
using fsr_fg::PfnFfxConfigure;
using fsr_fg::ffxConfigureDescFrameGeneration;
using fsr_fg::PfnFfxDispatch;

namespace {

PfnFfxCreateContext g_createContext = nullptr;
PfnFfxDestroyContext g_destroyContext = nullptr;
PfnFfxDispatch g_dispatch = nullptr;
PfnFfxConfigure g_configure = nullptr;

} // namespace

FsrFrameGenBackend::~FsrFrameGenBackend() { Shutdown(); }

std::wstring FsrFrameGenBackend::RuntimeSearchDescription()
{
    std::wstring text;
    for (const auto& path : SearchPaths()) {
        if (!text.empty()) text += L", ";
        text += path;
    }
    return text;
}

bool FsrFrameGenBackend::RuntimePresent(std::wstring* foundPath)
{
    for (const auto& path : SearchPaths()) {
        std::error_code ec;
        if (fs::is_regular_file(path, ec)) {
            if (foundPath) *foundPath = path;
            return true;
        }
    }
    if (foundPath) foundPath->clear();
    return false;
}

bool FsrFrameGenBackend::LoadRuntime()
{
    if (m_module) return true;

    std::wstring path;
    if (!RuntimePresent(&path)) {
        m_lastError = 0x80070002;   // ERROR_FILE_NOT_FOUND
        m_lastDetail = L"AMD FidelityFX frame generation was not found. Looked in: " +
                       RuntimeSearchDescription() +
                       L". Copy amd_fidelityfx_framegeneration_dx12.dll from "
                       L"FidelityFX-SDK/Kits/FidelityFX/signedbin next to the executable.";
        return false;
    }

    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR so the runtime finds its own siblings, and
    // not LOAD_WITH_ALTERED_SEARCH_PATH: the search is ours, not the loader's.
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
    g_configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(m_module, "ffxConfigure"));
    if (!g_createContext || !g_dispatch || !g_configure) {
        // A DLL that loads but does not export the C API is a different problem
        // from one that is absent, and the log should say so.
        m_lastDetail = path + L" does not export the FidelityFX C API (ffxCreateContext"
                       L"/ffxDispatch). It may be a loader shim rather than the runtime.";
        m_lastError = 0x8007007E;   // ERROR_PROC_NOT_FOUND
        FreeLibrary(m_module);
        m_module = nullptr;
        return false;
    }

    m_runtimePath = path;
    m_lastDetail.clear();
    m_lastError = 0;
    return true;
}

bool FsrFrameGenBackend::CreateContext(ID3D12Device* device)
{
    if (!device) {
        m_lastError = 0x80070057;   // ERROR_INVALID_PARAMETER
        m_lastDetail = L"FidelityFX frame generation was given no device.";
        return false;
    }

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.header.pNext = nullptr;
    backend.device = device;

    ffxCreateContextDescFrameGenerationVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    version.header.pNext = NextHeader(backend);
    version.version = FFX_FRAMEGENERATION_VERSION;

    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext = NextHeader(version);
    // DISPLAY_RESOLUTION_MOTION_VECTORS is what makes the runtime derive its
    // own motion from the colour buffers at display resolution - the mode the
    // MPCVR-FSR-FrameGen reference runs with zero-vector input. Without it the
    // runtime reads the (zero) vectors we declare and every generated frame is
    // a repeat. NO_SWAPCHAIN_CONTEXT_NOTIFY is implied by this backend having
    // no swapchain at all; the player renders into its own chain.
    create.flags = FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
                   FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    create.displaySize = {m_width, m_height};
    create.maxRenderSize = {m_width, m_height};
    // The playback renderer runs in FP16 linear light (scRGB's storage, minus
    // the negative-range scRGB actually allows), the offline conversion in the
    // decoder's BGRA8 or 10-bit. Each maps to its own FFX surface format and
    // transfer function; feeding the runtime the wrong pairing makes it tonemap
    // or clamp against the wrong curve.
    switch (m_format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            create.backBufferFormat = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
            break;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            create.backBufferFormat = FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
            break;
        default:
            create.backBufferFormat = FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
            break;
    }

    const int32_t rc = g_createContext(&m_context, &create.header, nullptr);
    if (rc != kFfxReturnOk || !m_context) {
        m_context = nullptr;
        m_lastError = static_cast<uint32_t>(rc);
        m_lastDetail = DescribeResult("ffxCreateContext(FrameGeneration)", rc) +
                       L". The runtime may not support this GPU, or it may be a "
                       L"different FidelityFX version than this backend was built against.";
        return false;
    }
    return true;
}

bool FsrFrameGenBackend::Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                                    uint32_t width, uint32_t height,
                                    DXGI_FORMAT backbufferFormat)
{
    m_device = device;
    m_width = width;
    m_height = height;
    m_format = backbufferFormat;

    // `cmd` is optional: the playback renderer has no command list at
    // construction time, so it passes null and the guides upload lazily on the
    // first Evaluate (UploadGuides). Only the offline pass hands one in here.
    if (!device || width == 0 || height == 0) {
        m_lastError = 0x80070057;   // ERROR_INVALID_PARAMETER
        m_lastDetail = L"FidelityFX frame generation needs a device and a "
                       L"non-empty backbuffer geometry.";
        return false;
    }

    if (!LoadRuntime()) return false;
    if (!CreateContext(m_device)) return false;

    // The API declares depth and motion-vector inputs and validates them, but it
    // derives its own motion from the colour buffers - a video frame has no
    // camera and no real vectors to give it. Both guides are therefore constant
    // and allocated once: depth at 1.0 (the far plane) and motion at zero.
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    // The guides are filled with GPU-side clears (ClearRenderTargetView)
    // instead of a CPU-mapped staging upload: with the runtime's context
    // alive, writing to a mapped staging buffer between two command-list
    // executions deterministically hung the next execute (TDR, 0x887A0006) -
    // reproduced in isolation, three out of three, with no other GPU work in
    // the list. RTV clears need no staging buffer and no CPU writes; both
    // formats accept render targets on every adapter this player supports.
    D3D12_RESOURCE_DESC depthDesc = DescribeTexture(width, height, DXGI_FORMAT_R32_FLOAT,
                                                     D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
            IID_PPV_ARGS(m_flatDepth.GetAddressOf()))))
        return false;
    m_flatDepth->SetName(L"FidelityFG_FlatDepth_R32F");

    D3D12_RESOURCE_DESC motionDesc = DescribeTexture(width, height, DXGI_FORMAT_R16G16_FLOAT,
                                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &motionDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
            IID_PPV_ARGS(m_zeroMotion.GetAddressOf()))))
        return false;
    m_zeroMotion->SetName(L"FidelityFG_ZeroMotion_RG16F");

    // CPU-visible RTV pair for the clears.
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 2;
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc,
                                            IID_PPV_ARGS(m_guideRtvHeap.GetAddressOf()))))
        return false;
    const UINT rtvIncrement = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_RENDER_TARGET_VIEW_DESC depthRtv{};
    depthRtv.Format = DXGI_FORMAT_R32_FLOAT;
    depthRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(m_flatDepth.Get(), &depthRtv,
                                   m_guideRtvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_RENDER_TARGET_VIEW_DESC motionRtv{};
    motionRtv.Format = DXGI_FORMAT_R16G16_FLOAT;
    motionRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(m_zeroMotion.Get(), &motionRtv,
                                   {m_guideRtvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                                    rtvIncrement});

    m_multiFrameCountMax = 1;   // one interpolated frame per pair, i.e. 2x
    m_frameId = 0;
    m_evaluations = 0;
    m_lastError = 0;
    m_lastDetail.clear();
    // The guides upload lazily: the playback renderer has no command list at
    // construction time, so when `cmd` is null the first Evaluate does it.
    m_uploaded = cmd != nullptr;
    if (m_uploaded && !UploadGuides(cmd)) return false;
    return true;
}

bool FsrFrameGenBackend::UploadGuides(ID3D12GraphicsCommandList* cmd)
{
    if (!cmd || !m_device) {
        m_lastDetail = L"FidelityFX's guides need a command list to be uploaded with.";
        return false;
    }
    if (!m_guideRtvHeap) {
        m_lastDetail = L"FidelityFX's guides were created without their RTV heap.";
        return false;
    }
    // GPU-side fill: depth = 1.0 (the far plane), motion = zero. The guides
    // were created in RENDER_TARGET state; the clears run there, and both move
    // to the shader-read state the descriptor chain declares afterwards.
    const FLOAT depthClear[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    const FLOAT motionClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    cmd->ClearRenderTargetView(m_guideRtvHeap->GetCPUDescriptorHandleForHeapStart(),
                               depthClear, 0, nullptr);
    const UINT rtvIncrement = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    cmd->ClearRenderTargetView({m_guideRtvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                                rtvIncrement},
                               motionClear, 0, nullptr);

    D3D12_RESOURCE_BARRIER guidesReady[2]{};
    guidesReady[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    guidesReady[0].Transition.pResource = m_flatDepth.Get();
    guidesReady[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    guidesReady[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    guidesReady[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    guidesReady[1].Transition.pResource = m_zeroMotion.Get();
    guidesReady[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    guidesReady[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(2, guidesReady);
    m_uploaded = true;
    return true;
}

bool FsrFrameGenBackend::Evaluate(ID3D12GraphicsCommandList* cmd,
                                  ID3D12Resource* backbuffer,
                                  ID3D12Resource* motion,
                                  ID3D12Resource* depth,
                                  ID3D12Resource* outputInterpolated,
                                  uint32_t multiFrameCount,
                                  uint32_t multiFrameIndex,
                                  bool reset,
                                  uint64_t backbufferFrameId,
                                  float frameTimeMs)
{
    (void)motion;
    (void)depth;

    if (!m_context || !cmd || !backbuffer || !outputInterpolated) {
        m_lastError = 0x80070057;
        m_lastDetail = L"FidelityFX frame generation was asked to evaluate before it was "
                       L"initialized, or with a null target.";
        return false;
    }
    // The playback renderer initializes without a command list; its first
    // Evaluate is where the constant guides finally reach the device.
    if (!m_uploaded && !UploadGuides(cmd)) {
        m_lastError = 0x80070057;
        return false;
    }

    // The API takes an explicit backbuffer state and a UAV flag on the output.
    // The caller (FrameGenerationPass) hands over an output resource created
    // with ALLOW_UNORDERED_ACCESS, which is what FFX_API_RESOURCE_USAGE_UAV
    // declares; stating it again here keeps that contract visible at the call
    // site rather than buried in the resource creation.

    // Per-frame configuration, BEFORE prepare - the step the first integration
    // skipped, and the one that actually turns frame generation on. The
    // runtime's default state is disabled, so without this dispatch the
    // generated output was a passthrough of the presented frame, which is
    // where the ghosting came from. MPCVR-FSR-FrameGen (the working reference)
    // issues exactly this every frame: enabled, no async, no swapchain hooks.
    ffxConfigureDescFrameGeneration config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.header.pNext = nullptr;
    config.swapChain = nullptr;
    config.frameGenerationEnabled = true;
    config.allowAsyncWorkloads = false;
    config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    config.onlyPresentGenerated = false;
    config.generationRect = {0, 0, static_cast<int32_t>(m_width), static_cast<int32_t>(m_height)};
    config.frameID = backbufferFrameId ? backbufferFrameId : m_frameId + 1;
    // Field bisect: FFX_FG_MIN_CONFIG drops everything optional, FFX_FG_DEBUG
    // asks the runtime to validate instead of trusting.
    if (GetEnvironmentVariableW(L"FFX_FG_MIN_CONFIG", nullptr, 0)) {
        config.flags = 0;
        config.generationRect = {};
        config.frameID = 0;
        config.allowAsyncWorkloads = false;
    } else if (GetEnvironmentVariableW(L"FFX_FG_DEBUG", nullptr, 0)) {
        config.flags |= 1 << 6;
    }
    int32_t rc = g_configure(&m_context, &config.header);
    if (rc != kFfxReturnOk) {
        m_lastError = static_cast<uint32_t>(rc);
        m_lastDetail = DescribeResult("ffxConfigure(FrameGeneration)", rc);
        return false;
    }

    ffxDispatchDescFrameGenerationPrepareV2 prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.header.pNext = nullptr;
    // Any difference other than +1 resets the runtime's temporal state, so feed
    // it our own counter when the caller has none.
    prepare.frameID = backbufferFrameId ? backbufferFrameId : ++m_frameId;
    prepare.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    prepare.commandList = cmd;
    prepare.renderSize = {m_width, m_height};
    prepare.jitterOffset[0] = 0.0f;
    prepare.jitterOffset[1] = 0.0f;
    prepare.motionVectorScale[0] = 1.0f;
    prepare.motionVectorScale[1] = 1.0f;
    // FidelityFX scales its motion estimate by this interval, so a wrong value
    // slides the intermediate frame to the wrong place - a 30 fps source given
    // the synthetic 60 fps default would land its generated frame a quarter of
    // the way along the motion. The playback chain therefore measures the real
    // interval off the decode timestamps and passes it in; the offline
    // conversion, which shows each source frame for half as long at 2x, keeps
    // the synthetic derivation.
    const float interval = frameTimeMs > 0.0f
                               ? frameTimeMs
                               : 1000.0f / 60.0f / static_cast<float>(multiFrameCount + 1);
    prepare.frameTimeDelta = interval;
    prepare.reset = reset;
    // A flat depth (everything at the far plane) is the honest value for footage
    // that has no depth buffer, and the camera parameters below are placeholders
    // the API requires but does not use when it owns the motion estimation.
    prepare.cameraNear = 0.1f;
    prepare.cameraFar = 1000.0f;
    prepare.cameraFovAngleVertical = 1.04719755f;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = GetResourceDX12(m_flatDepth.Get(),
                                   FFX_API_RESOURCE_STATE_COMPUTE_READ,
                                   FFX_API_RESOURCE_USAGE_DEPTHTARGET);
    prepare.motionVectors = GetResourceDX12(m_zeroMotion.Get(),
                                            FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prepare.cameraUp[1] = 1.0f;
    prepare.cameraRight[0] = 1.0f;
    prepare.cameraForward[2] = 1.0f;

    rc = g_dispatch(&m_context, &prepare.header);
    if (rc != kFfxReturnOk) {
        m_lastError = static_cast<uint32_t>(rc);
        m_lastDetail = DescribeResult("ffxDispatch(FrameGenerationPrepareV2)", rc);
        return false;
    }

    ffxDispatchDescFrameGeneration dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    dispatch.header.pNext = nullptr;
    dispatch.commandList = cmd;
    dispatch.presentColor = GetResourceDX12(
        backbuffer, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.outputs[0] = GetResourceDX12(
        outputInterpolated, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
        FFX_API_RESOURCE_USAGE_UAV);
    dispatch.numGeneratedFrames = 1;
    dispatch.reset = reset;
    // SCRGB is the transfer FidelityFX expects for linear FP16 input; the
    // encoder-domain formats keep their display curves.
    dispatch.backbufferTransferFunction =
        m_format == DXGI_FORMAT_R16G16B16A16_FLOAT
            ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB
            : (m_format == DXGI_FORMAT_R10G10B10A2_UNORM
                   ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ
                   : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB);
    dispatch.minMaxLuminance[0] = 0.0f;
    dispatch.minMaxLuminance[1] = 1000.0f;
    dispatch.generationRect = {0, 0, static_cast<int32_t>(m_width), static_cast<int32_t>(m_height)};
    dispatch.frameID = prepare.frameID;

    rc = g_dispatch(&m_context, &dispatch.header);
    if (rc != kFfxReturnOk) {
        m_lastError = static_cast<uint32_t>(rc);
        m_lastDetail = DescribeResult("ffxDispatch(FrameGeneration)", rc) + L" at evaluate " +
                       std::to_wstring(multiFrameIndex) + L" of " +
                       std::to_wstring(multiFrameCount) + L".";
        return false;
    }
    // From here on this context is in the state the runtime knows how to
    // destroy - see the never-dispatched note in Shutdown.
    m_dispatched = true;

    // The same UAV barrier the working reference records right after its
    // dispatch: the runtime's generation pass writes the output across
    // multiple shaders, and a global UAV barrier orders those writes before
    // anything downstream (readback or present) samples the texture. It changes
    // no resource state, so the caller's UNORDERED_ACCESS contract below holds.
    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cmd->ResourceBarrier(1, &uavBarrier);

    // The pass owns the output's state after this: it transitions
    // UNORDERED_ACCESS -> COPY_SOURCE, reads back, and puts it back. Emitting
    // that transition here as well made the pass's own barrier illegal (its
    // before-state no longer matched), which is the same class of bug as the
    // equal-state transition the retail runtime rejects.
    //
    // FFX leaves the output in UAV state, which is what the pass expects, so
    // nothing has to be emitted here.
    ++m_evaluations;
    return true;
}

void FsrFrameGenBackend::Shutdown()
{
    // Both playback minidumps fault at the same address inside
    // amd_fidelityfx_framegeneration_dx12.dll (+0x10b9b2) while tearing down a
    // context that was created but never dispatched - the player rebuilds its
    // renderer when the hardware decoder falls back, and the first instance's
    // generation context dies young. The offline conversion, which always
    // dispatches before destroying, never crashes here. So a never-dispatched
    // context is dropped without the callback: its device-bound resources are
    // reclaimed when the device goes, and not calling into the runtime is
    // strictly safer than calling into a code path the vendor demonstrably got
    // wrong.
    if (m_context && g_destroyContext && m_dispatched) g_destroyContext(&m_context, nullptr);
    m_context = nullptr;

    m_sharedInput.Reset();
    m_sharedOutput.Reset();
    m_sharedFence.Reset();
    m_islandList.Reset();
    m_islandAllocator.Reset();
    m_islandQueue.Reset();
    if (m_islandFenceEvent) {
        CloseHandle(m_islandFenceEvent);
        m_islandFenceEvent = nullptr;
    }
    m_islandFence.Reset();
    m_islandDevice.Reset();
    m_island = false;
    m_islandFenceValue = 0;
    m_flatDepth.Reset();
    m_zeroMotion.Reset();
    m_guideRtvHeap.Reset();
    m_device = nullptr;
    m_width = 0;
    m_height = 0;
    m_frameId = 0;
    m_multiFrameCountMax = 0;
    m_uploaded = false;
    m_dispatched = false;
}

void FsrFrameGenBackend::Abandon()
{
    // For a device that may still be executing work: drop the references without
    // calling back into the runtime, and leave the module loaded so the DLL is
    // not yanked out from under a queue that has not drained.
    m_context = nullptr;
    m_flatDepth.Reset();
    m_zeroMotion.Reset();
    m_guideRtvHeap.Reset();
    m_multiFrameCountMax = 0;
}

void FsrFrameGenBackend::AbandonIsland()
{
    // The island's device was removed (or its wait timed out). Everything the
    // runtime touched lives on that device and dies with it, so nothing here
    // calls back into the runtime - same rule as the never-dispatched note in
    // Shutdown, for the same reason. The caller's shared textures and fence are
    // NOT ours to release.
    m_context = nullptr;
    m_sharedInput.Reset();
    m_sharedOutput.Reset();
    m_sharedFence.Reset();
    m_islandList.Reset();
    m_islandAllocator.Reset();
    m_islandQueue.Reset();
    if (m_islandFenceEvent) {
        CloseHandle(m_islandFenceEvent);
        m_islandFenceEvent = nullptr;
    }
    m_islandFence.Reset();
    m_islandDevice.Reset();
    m_island = false;
    m_flatDepth.Reset();
    m_zeroMotion.Reset();
    m_guideRtvHeap.Reset();
    m_device = nullptr;
    m_multiFrameCountMax = 0;
}

namespace {

// Device Removed Extended Data off the island's own device. The renderer's
// copy of this lives in D3D12Renderer and cannot be reached from here, and the
// island's death is the interesting one when the bridge breaks, so it gets its
// own dump - breadcrumbs first (which list, how far in), then the page fault
// with its live and recently-freed allocations.
void DumpIslandDred(ID3D12Device* device, std::wstring& detail)
{
    if (!device) return;
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
        detail += L" [island DRED unavailable - not turned on in this process]";
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    if (FAILED(dred->GetAutoBreadcrumbsOutput(&crumbs)) || !crumbs.pHeadAutoBreadcrumbNode) {
        detail += L" [island DRED: no breadcrumbs outstanding]";
    } else {
        for (const D3D12_AUTO_BREADCRUMB_NODE* node = crumbs.pHeadAutoBreadcrumbNode; node;
             node = node->pNext) {
            const UINT completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0u;
            char line[256]{};
            std::snprintf(line, sizeof(line), " [island DRED: stopped after %u of %u ops]",
                          static_cast<unsigned>(completed),
                          static_cast<unsigned>(node->BreadcrumbCount));
            detail += ToWide(line);
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA) {
        char line[160]{};
        std::snprintf(line, sizeof(line), " [island DRED page fault va=0x%llx]",
                      static_cast<unsigned long long>(fault.PageFaultVA));
        detail += ToWide(line);
    }
}

} // namespace

bool FsrFrameGenBackend::CreateIsland(const LUID& adapterLuid)
{
    // A device of the runtime's own, on the SAME physical GPU the renderer is
    // on: enumerated by LUID, never by description (two identical cards share
    // one). The enumeration mirrors MPCVR-FSR-FrameGen's CreateD3D, which is
    // the arrangement measured to survive this runtime.
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        m_lastError = 0x80070057;
        m_lastDetail = L"The FidelityFX island could not create a DXGI factory.";
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;
         factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapter.Reset();
            continue;
        }
        if (desc.AdapterLuid.HighPart != adapterLuid.HighPart ||
            desc.AdapterLuid.LowPart != adapterLuid.LowPart) {
            adapter.Reset();
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                        __uuidof(ID3D12Device), nullptr))) {
            break;
        }
        adapter.Reset();
    }
    if (!adapter) {
        m_lastError = 0x80070002;   // ERROR_FILE_NOT_FOUND - no such adapter
        m_lastDetail = L"The adapter the renderer is on could not be found again for the "
                       L"FidelityFX island.";
        return false;
    }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                 IID_PPV_ARGS(&m_islandDevice)))) {
        m_lastError = 0x80070057;
        m_lastDetail = L"The FidelityFX island could not create its own D3D12 device.";
        return false;
    }
    m_islandDevice->SetName(L"FidelityFG_Island");

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    if (FAILED(m_islandDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_islandQueue)))) {
        m_lastDetail = L"The FidelityFX island could not create its command queue.";
        return false;
    }
    if (FAILED(m_islandDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&m_islandAllocator))) ||
        FAILED(m_islandDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 m_islandAllocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&m_islandList)))) {
        m_lastDetail = L"The FidelityFX island could not create its command list.";
        return false;
    }
    if (FAILED(m_islandList->Close())) {
        m_lastDetail = L"The FidelityFX island could not close its initial command list.";
        return false;
    }
    if (FAILED(m_islandDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                           IID_PPV_ARGS(&m_islandFence)))) {
        m_lastDetail = L"The FidelityFX island could not create its fence.";
        return false;
    }
    m_islandFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_islandFenceEvent) {
        m_lastDetail = L"The FidelityFX island could not create its fence event.";
        return false;
    }
    m_islandFenceValue = 0;
    return true;
}

bool FsrFrameGenBackend::ExecuteAndWaitIsland()
{
    // The island's submissions are never left running: every EvaluateIsolated
    // waits its own work out before returning. That is what makes the caller's
    // next command-list reset legal (its input write has retired) and what
    // turns a runtime fault into a detectable event instead of a surprise three
    // frames later.
    if (FAILED(m_islandList->Close())) {
        m_lastDetail = L"The FidelityFX island could not close its command list.";
        return false;
    }
    ID3D12CommandList* lists[] = {m_islandList.Get()};
    m_islandQueue->ExecuteCommandLists(1, lists);
    const uint64_t value = ++m_islandFenceValue;
    if (FAILED(m_islandQueue->Signal(m_islandFence.Get(), value))) {
        m_lastDetail = L"The FidelityFX island could not signal its fence.";
        return false;
    }
    if (m_islandFence->GetCompletedValue() < value) {
        if (FAILED(m_islandFence->SetEventOnCompletion(value, m_islandFenceEvent))) {
            m_lastDetail = L"The FidelityFX island could not arm its fence event.";
            return false;
        }
        // Bounded: a runtime that never finishes is a dead island, and an
        // unbounded wait here would hang the render thread with it.
        const DWORD waited = WaitForSingleObject(m_islandFenceEvent, 10000);
        if (waited != WAIT_OBJECT_0) {
            m_lastDetail = L"The FidelityFX island did not finish within 10 s.";
            return false;
        }
    }
    return true;
}

bool FsrFrameGenBackend::InitializeIsolated(const LUID& adapterLuid,
                                            uint32_t width, uint32_t height,
                                            DXGI_FORMAT format,
                                            HANDLE sharedInput, HANDLE sharedOutput,
                                            HANDLE sharedFence)
{
    try {
        if (!sharedInput || !sharedOutput || !sharedFence || width == 0 || height == 0) {
            m_lastError = 0x80070057;   // ERROR_INVALID_PARAMETER
            m_lastDetail = L"The FidelityFX island was asked for no shared resources, "
                           L"or for an empty geometry.";
            return false;
        }
        if (format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_R10G10B10A2_UNORM) {
            m_lastError = 0x80070057;
            m_lastDetail = L"The FidelityFX island was handed a format it does not run in.";
            return false;
        }

        Shutdown();

        m_width = width;
        m_height = height;
        m_format = format;

        if (!LoadRuntime()) return false;
        if (!CreateIsland(adapterLuid)) return false;
        // From here on the island's device is "the device" for every
        // device-scoped call in this class, guides included.
        m_device = m_islandDevice.Get();
        if (!CreateContext(m_device)) return false;

        if (FAILED(m_islandDevice->OpenSharedHandle(sharedInput, IID_PPV_ARGS(&m_sharedInput))) ||
            FAILED(m_islandDevice->OpenSharedHandle(sharedOutput, IID_PPV_ARGS(&m_sharedOutput))) ||
            FAILED(m_islandDevice->OpenSharedHandle(sharedFence, IID_PPV_ARGS(&m_sharedFence)))) {
            m_lastError = 0x80070005;   // ERROR_ACCESS_DENIED
            m_lastDetail = L"The FidelityFX island could not open the shared frame or fence "
                           L"handles the renderer offered.";
            return false;
        }

        // Guides: same flat depth and zero motion as every other mode, filled
        // with GPU-side clears on the island's own list - a mapped staging
        // buffer next to a live context is the documented TDR, three for three.
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC depthDesc = DescribeTexture(width, height, DXGI_FORMAT_R32_FLOAT,
                                                        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        if (FAILED(m_device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &depthDesc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                IID_PPV_ARGS(m_flatDepth.GetAddressOf()))))
            return false;
        m_flatDepth->SetName(L"FidelityFG_Island_FlatDepth_R32F");
        D3D12_RESOURCE_DESC motionDesc = DescribeTexture(width, height, DXGI_FORMAT_R16G16_FLOAT,
                                                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        if (FAILED(m_device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &motionDesc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                IID_PPV_ARGS(m_zeroMotion.GetAddressOf()))))
            return false;
        m_zeroMotion->SetName(L"FidelityFG_Island_ZeroMotion_RG16F");

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = 2;
        if (FAILED(m_device->CreateDescriptorHeap(&rtvHeapDesc,
                                                  IID_PPV_ARGS(m_guideRtvHeap.GetAddressOf()))))
            return false;
        const UINT rtvIncrement = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_RENDER_TARGET_VIEW_DESC depthRtv{};
        depthRtv.Format = DXGI_FORMAT_R32_FLOAT;
        depthRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_device->CreateRenderTargetView(m_flatDepth.Get(), &depthRtv,
                                         m_guideRtvHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_RENDER_TARGET_VIEW_DESC motionRtv{};
        motionRtv.Format = DXGI_FORMAT_R16G16_FLOAT;
        motionRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_device->CreateRenderTargetView(m_zeroMotion.Get(), &motionRtv,
                                         {m_guideRtvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                                          rtvIncrement});

        if (FAILED(m_islandAllocator->Reset()) ||
            FAILED(m_islandList->Reset(m_islandAllocator.Get(), nullptr))) {
            m_lastDetail = L"The FidelityFX island could not reset its list for the guides.";
            return false;
        }
        if (!UploadGuides(m_islandList.Get())) return false;
        if (!ExecuteAndWaitIsland()) return false;

        m_multiFrameCountMax = 1;   // one interpolated frame per pair, i.e. 2x
        m_frameId = 0;
        m_evaluations = 0;
        m_lastError = 0;
        m_lastDetail.clear();
        m_island = true;
        return true;
    } catch (const std::exception& ex) {
        m_lastDetail = L"FidelityFX frame generation (island) threw during initialization: " +
                       ToWide(ex.what());
        OutputDebugStringA("FsrFrameGenBackend: InitializeIsolated threw\n");
        AbandonIsland();
        return false;
    } catch (...) {
        m_lastDetail = L"FidelityFX frame generation (island) threw an unknown error during "
                       L"initialization.";
        OutputDebugStringA("FsrFrameGenBackend: InitializeIsolated threw (unknown)\n");
        AbandonIsland();
        return false;
    }
}

bool FsrFrameGenBackend::EvaluateIsolated(ID3D12Resource* backbuffer,
                                          ID3D12Resource* outputInterpolated,
                                          uint64_t waitValue, bool reset,
                                          uint64_t frameId, float frameTimeMs)
{
    if (!m_island || !m_context || !m_sharedInput || !m_sharedOutput || !m_sharedFence ||
        !backbuffer || !outputInterpolated) {
        m_lastError = 0x80070057;
        m_lastDetail = L"FidelityFX frame generation (island) was asked to evaluate before it "
                       L"was initialized, or with a null target.";
        return false;
    }
    // The island died earlier and was abandoned; say so plainly rather than
    // walking a null queue.
    if (!m_islandQueue || !m_islandList) {
        m_lastDetail = L"FidelityFX frame generation (island) is gone; the caller must have "
                       L"missed the failure.";
        return false;
    }

    try {
        // GPU-side wait for the caller's convert into the shared input. This is
        // the one cross-device ordering the protocol needs on the way IN; the
        // way OUT is covered by this call's own completion wait.
        if (waitValue && FAILED(m_islandQueue->Wait(m_sharedFence.Get(), waitValue))) {
            m_lastDetail = L"FidelityFX frame generation (island) could not wait for the "
                           L"renderer's frame on the shared fence.";
            DumpIslandDred(m_islandDevice.Get(), m_lastDetail);
            AbandonIsland();
            return false;
        }

        if (FAILED(m_islandAllocator->Reset()) ||
            FAILED(m_islandList->Reset(m_islandAllocator.Get(), nullptr))) {
            m_lastDetail = L"FidelityFX frame generation (island) could not reset its list.";
            AbandonIsland();
            return false;
        }

        // Each submission starts and ends in COMMON on the island's list, so
        // the transitions below are always legal regardless of what the
        // caller's list believes about the same textures.
        D3D12_RESOURCE_BARRIER toRead[2]{};
        toRead[0] = Transition(backbuffer, D3D12_RESOURCE_STATE_COMMON,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        toRead[1] = Transition(outputInterpolated, D3D12_RESOURCE_STATE_COMMON,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_islandList->ResourceBarrier(2, toRead);

        ffxConfigureDescFrameGeneration config{};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.header.pNext = nullptr;
        config.swapChain = nullptr;
        config.frameGenerationEnabled = true;
        config.allowAsyncWorkloads = false;
        config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        config.onlyPresentGenerated = false;
        config.generationRect = {0, 0, static_cast<int32_t>(m_width), static_cast<int32_t>(m_height)};
        config.frameID = frameId;
        if (GetEnvironmentVariableW(L"FFX_FG_MIN_CONFIG", nullptr, 0)) {
            config.flags = 0;
            config.generationRect = {};
            config.frameID = 0;
            config.allowAsyncWorkloads = false;
        } else if (GetEnvironmentVariableW(L"FFX_FG_DEBUG", nullptr, 0)) {
            config.flags |= 1 << 6;
        }
        int32_t rc = g_configure(&m_context, &config.header);
        if (rc != kFfxReturnOk) {
            m_lastError = static_cast<uint32_t>(rc);
            m_lastDetail = DescribeResult("island ffxConfigure(FrameGeneration)", rc);
            return false;
        }

        ffxDispatchDescFrameGenerationPrepareV2 prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prepare.header.pNext = nullptr;
        prepare.frameID = frameId;
        prepare.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        prepare.commandList = m_islandList.Get();
        prepare.renderSize = {m_width, m_height};
        prepare.jitterOffset[0] = 0.0f;
        prepare.jitterOffset[1] = 0.0f;
        prepare.motionVectorScale[0] = 1.0f;
        prepare.motionVectorScale[1] = 1.0f;
        const float interval = frameTimeMs > 0.0f
                                   ? frameTimeMs
                                   : 1000.0f / 60.0f / 2.0f;
        prepare.frameTimeDelta = interval;
        prepare.reset = reset;
        prepare.cameraNear = 0.1f;
        prepare.cameraFar = 1000.0f;
        prepare.cameraFovAngleVertical = 1.04719755f;
        prepare.viewSpaceToMetersFactor = 1.0f;
        prepare.depth = GetResourceDX12(m_flatDepth.Get(),
                                        FFX_API_RESOURCE_STATE_COMPUTE_READ,
                                        FFX_API_RESOURCE_USAGE_DEPTHTARGET);
        prepare.motionVectors = GetResourceDX12(m_zeroMotion.Get(),
                                                FFX_API_RESOURCE_STATE_COMPUTE_READ);
        prepare.cameraUp[1] = 1.0f;
        prepare.cameraRight[0] = 1.0f;
        prepare.cameraForward[2] = 1.0f;

        rc = g_dispatch(&m_context, &prepare.header);
        if (rc != kFfxReturnOk) {
            m_lastError = static_cast<uint32_t>(rc);
            m_lastDetail = DescribeResult("island ffxDispatch(FrameGenerationPrepareV2)", rc);
            return false;
        }

        ffxDispatchDescFrameGeneration dispatch{};
        dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        dispatch.header.pNext = nullptr;
        dispatch.commandList = m_islandList.Get();
        dispatch.presentColor = GetResourceDX12(backbuffer, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.outputs[0] = GetResourceDX12(
            outputInterpolated, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
            FFX_API_RESOURCE_USAGE_UAV);
        dispatch.numGeneratedFrames = 1;
        dispatch.reset = reset;
        dispatch.backbufferTransferFunction =
            m_format == DXGI_FORMAT_R10G10B10A2_UNORM
                ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ
                : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dispatch.minMaxLuminance[0] = 0.0f;
        dispatch.minMaxLuminance[1] = 1000.0f;
        dispatch.generationRect = {0, 0, static_cast<int32_t>(m_width), static_cast<int32_t>(m_height)};
        dispatch.frameID = frameId;

        rc = g_dispatch(&m_context, &dispatch.header);
        if (rc != kFfxReturnOk) {
            m_lastError = static_cast<uint32_t>(rc);
            m_lastDetail = DescribeResult("island ffxDispatch(FrameGeneration)", rc);
            return false;
        }
        // From here on this context is in the state the runtime knows how to
        // destroy - see the never-dispatched note in Shutdown.
        m_dispatched = true;

        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        m_islandList->ResourceBarrier(1, &uavBarrier);

        D3D12_RESOURCE_BARRIER toCommon[2]{};
        toCommon[0] = Transition(backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_COMMON);
        toCommon[1] = Transition(outputInterpolated, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_COMMON);
        m_islandList->ResourceBarrier(2, toCommon);

        if (!ExecuteAndWaitIsland()) {
            m_lastDetail += L" (island submit)";
            DumpIslandDred(m_islandDevice.Get(), m_lastDetail);
            AbandonIsland();
            return false;
        }
        // The island's own health, asked once its work has retired: a removal
        // shows up here and nowhere earlier.
        const HRESULT reason = m_islandDevice->GetDeviceRemovedReason();
        if (reason != S_OK) {
            char line[128]{};
            std::snprintf(line, sizeof(line), " [island device removed 0x%08lx]",
                          static_cast<unsigned long>(reason));
            m_lastDetail = std::wstring(L"FidelityFX frame generation (island) device was removed "
                                        L"after its work retired") +
                           ToWide(line);
            DumpIslandDred(m_islandDevice.Get(), m_lastDetail);
            AbandonIsland();
            return false;
        }
        ++m_evaluations;
        return true;
    } catch (const std::exception& ex) {
        m_lastDetail += L" (island threw: " + ToWide(ex.what()) + L")";
        OutputDebugStringA("FsrFrameGenBackend: EvaluateIsolated threw\n");
        AbandonIsland();
        return false;
    } catch (...) {
        m_lastDetail += L" (island threw an unknown error)";
        OutputDebugStringA("FsrFrameGenBackend: EvaluateIsolated threw (unknown)\n");
        AbandonIsland();
        return false;
    }
}