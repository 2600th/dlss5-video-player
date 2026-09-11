#include "OpticalFlowNvof.h"

#include "Log.h"

// The Optical Flow SDK headers are an NVIDIA component and are never committed or
// redistributed, exactly like the DLSS SDK beside them. A tree without them still
// builds: every entry point below reports the engine as unavailable and the caller
// keeps the CPU estimator. docs/BUILDING.md says where to put them.
#if defined(DLSS_VIDEO_PLAYER_HAS_NVOF)

#include <algorithm>
#include <vector>

#include "nvOpticalFlowCommon.h"
#include "nvOpticalFlowD3D12.h"

using Microsoft::WRL::ComPtr;

struct OpticalFlowNvof::Api {
    NV_OF_D3D12_API_FUNCTION_LIST fn{};
};

namespace {

typedef NV_OF_STATUS(NVOFAPI* PfnCreateInstanceD3D12)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST*);

const char* StatusName(NV_OF_STATUS status)
{
    switch (status) {
        case NV_OF_SUCCESS: return "SUCCESS";
        case NV_OF_ERR_OF_NOT_AVAILABLE: return "OF_NOT_AVAILABLE";
        case NV_OF_ERR_UNSUPPORTED_DEVICE: return "UNSUPPORTED_DEVICE";
        case NV_OF_ERR_DEVICE_DOES_NOT_EXIST: return "DEVICE_DOES_NOT_EXIST";
        case NV_OF_ERR_INVALID_PTR: return "INVALID_PTR";
        case NV_OF_ERR_INVALID_PARAM: return "INVALID_PARAM";
        case NV_OF_ERR_INVALID_CALL: return "INVALID_CALL";
        case NV_OF_ERR_INVALID_VERSION: return "INVALID_VERSION";
        case NV_OF_ERR_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case NV_OF_ERR_NOT_INITIALIZED: return "NOT_INITIALIZED";
        case NV_OF_ERR_UNSUPPORTED_FEATURE: return "UNSUPPORTED_FEATURE";
        default: return "GENERIC";
    }
}

ComPtr<ID3D12Resource> CreateSurface(ID3D12Device* device, DXGI_FORMAT format,
                                     uint32_t width, uint32_t height, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> resource;
    // COMMON is the state a resource must be in to cross to another engine, and the
    // engine hands it back in COMMON too, so every surface here lives there by default.
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&resource))))
        return nullptr;
    resource->SetName(name);
    return resource;
}

}  // namespace

OpticalFlowNvof::~OpticalFlowNvof() { Shutdown(); }

bool OpticalFlowNvof::Initialize(ID3D12Device* device, uint32_t width, uint32_t height)
{
    if (!device || !width || !height) return false;
    m_device = device;
    m_width = width;
    m_height = height;

    // Never bundled and never loaded from beside the executable: this is a driver
    // component and the driver install is the only place it may come from.
    HMODULE library = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) {
        LOG("NVOFA unavailable: nvofapi64.dll is not present in the driver install.");
        return false;
    }
    m_library = library;
    auto create = reinterpret_cast<PfnCreateInstanceD3D12>(
        GetProcAddress(library, "NvOFAPICreateInstanceD3D12"));
    if (!create) {
        LOG("NVOFA unavailable: nvofapi64.dll exports no NvOFAPICreateInstanceD3D12.");
        Shutdown();
        return false;
    }

    m_api = new Api();
    NV_OF_STATUS status = create(NV_OF_API_VERSION, &m_api->fn);
    if (status != NV_OF_SUCCESS) {
        LOG("NVOFA unavailable: NvOFAPICreateInstanceD3D12 returned " << StatusName(status) << ".");
        Shutdown();
        return false;
    }
    const NV_OF_D3D12_API_FUNCTION_LIST& fn = m_api->fn;
    if (!fn.nvCreateOpticalFlowD3D12 || !fn.nvOFInit || !fn.nvOFExecuteD3D12 ||
        !fn.nvOFRegisterResourceD3D12 || !fn.nvOFGetCaps ||
        !fn.nvOFGetSurfaceFormatCountD3D12 || !fn.nvOFGetSurfaceFormatD3D12) {
        LOG("NVOFA unavailable: the D3D12 function list is incomplete.");
        Shutdown();
        return false;
    }
    status = fn.nvCreateOpticalFlowD3D12(device, reinterpret_cast<NvOFHandle*>(&m_session));
    if (status != NV_OF_SUCCESS || !m_session) {
        LOG("NVOFA unavailable: nvCreateOpticalFlowD3D12 returned " << StatusName(status) << ".");
        Shutdown();
        return false;
    }
    auto handle = static_cast<NvOFHandle>(m_session);

    // The engine states its own limits; asking for a geometry outside them fails with
    // E_FAIL rather than an NV_OF_STATUS, so check first and say which bound was missed.
    uint32_t caps[4]{}, count = 1;
    const NV_OF_CAPS bounds[4] = {NV_OF_CAPS_WIDTH_MIN, NV_OF_CAPS_HEIGHT_MIN,
                                  NV_OF_CAPS_WIDTH_MAX, NV_OF_CAPS_HEIGHT_MAX};
    for (int i = 0; i < 4; ++i) {
        count = 1;
        if (fn.nvOFGetCaps(handle, bounds[i], &caps[i], &count) != NV_OF_SUCCESS) caps[i] = 0;
    }
    if (caps[2] && caps[3] && (width < caps[0] || height < caps[1] ||
                               width > caps[2] || height > caps[3])) {
        LOG("NVOFA unavailable: " << width << "x" << height << " is outside the engine's "
            << caps[0] << "x" << caps[1] << ".." << caps[2] << "x" << caps[3] << " range.");
        Shutdown();
        return false;
    }

    uint32_t gridCount = 0;
    if (fn.nvOFGetCaps(handle, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, nullptr, &gridCount) !=
            NV_OF_SUCCESS || gridCount == 0) {
        LOG("NVOFA unavailable: the engine reported no supported output grid size.");
        Shutdown();
        return false;
    }
    std::vector<uint32_t> grids(gridCount);
    fn.nvOFGetCaps(handle, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, grids.data(), &gridCount);
    // Grid 2 rather than the finest on offer. One vector per 2x2 pixels is still a
    // twelvefold refinement on the CPU estimator's 24x24 block at 1080p, and it is what
    // both ComfyUI-DLSS5-NR and dlss5-bridge default to. Measured here, grid 1 at MEDIUM
    // put the frame loop at 17.2 ms against 8.9 ms before, which is not a price a
    // real-time path can pay for accuracy finer than the output can show. Falls back to
    // whatever the engine does offer.
    std::sort(grids.begin(), grids.end());
    m_grid = grids.front();
    for (uint32_t candidate : grids) {
        if (candidate >= 2) { m_grid = candidate; break; }
    }
    if (m_grid < 1) m_grid = 1;

    // The input surface format has to be one the engine named, not one that looks right.
    uint32_t formatCount = 0;
    if (fn.nvOFGetSurfaceFormatCountD3D12(handle, NV_OF_BUFFER_USAGE_INPUT,
                                          NV_OF_MODE_OPTICALFLOW, &formatCount) != NV_OF_SUCCESS ||
        formatCount == 0) {
        LOG("NVOFA unavailable: the engine named no input surface format.");
        Shutdown();
        return false;
    }
    std::vector<DXGI_FORMAT> inputFormats(formatCount);
    fn.nvOFGetSurfaceFormatD3D12(handle, NV_OF_BUFFER_USAGE_INPUT, NV_OF_MODE_OPTICALFLOW,
                                 inputFormats.data());
    if (std::find(inputFormats.begin(), inputFormats.end(), DXGI_FORMAT_B8G8R8A8_UNORM) ==
        inputFormats.end()) {
        LOG("NVOFA unavailable: the engine does not accept B8G8R8A8_UNORM input, which is "
            "the format the decoded frame already has.");
        Shutdown();
        return false;
    }

    // The cost surface is optional. It carries a per-cell confidence the resolve pass can
    // gate on; without it every vector is taken at face value.
    DXGI_FORMAT costFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t costCount = 0;
    if (fn.nvOFGetSurfaceFormatCountD3D12(handle, NV_OF_BUFFER_USAGE_COST, NV_OF_MODE_OPTICALFLOW,
                                          &costCount) == NV_OF_SUCCESS && costCount > 0) {
        std::vector<DXGI_FORMAT> costFormats(costCount);
        fn.nvOFGetSurfaceFormatD3D12(handle, NV_OF_BUFFER_USAGE_COST, NV_OF_MODE_OPTICALFLOW,
                                     costFormats.data());
        costFormat = std::find(costFormats.begin(), costFormats.end(), DXGI_FORMAT_R8_UINT) !=
                             costFormats.end()
                         ? DXGI_FORMAT_R8_UINT
                         : costFormats[0];
    }

    NV_OF_INIT_PARAMS init{};
    init.width = width;
    init.height = height;
    init.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(m_grid);
    init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    // FAST, which is what both ComfyUI-DLSS5-NR and dlss5-bridge run. Measured here:
    // MEDIUM at grid 2 put the offline loop at 12.28 ms/frame against 8.92 ms before
    // this backend existed, and a live session that had been dropping nothing dropped
    // 27 of 87 frames - and every one of those drops resets the temporal history the
    // accuracy was bought for, so the slower level costs more than it returns.
    // dlss5-bridge measured the same shape at 3840x1600 grid 2: FAST 1.6 ms, MEDIUM
    // 6.2, SLOW 11.5, and calls the slower levels "the knob to reach for when fine
    // detail drifts" - a knob to turn deliberately, once it can be measured, not a
    // default to pay for on every frame.
    init.perfLevel = NV_OF_PERF_LEVEL_FAST;
    init.enableExternalHints = NV_OF_FALSE;
    init.enableOutputCost = costFormat != DXGI_FORMAT_UNKNOWN ? NV_OF_TRUE : NV_OF_FALSE;
    init.hPrivData = nullptr;
    init.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED;
    init.enableRoi = NV_OF_FALSE;
    init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
    init.enableGlobalFlow = NV_OF_FALSE;
    init.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
    status = fn.nvOFInit(handle, &init);
    if (status != NV_OF_SUCCESS) {
        LOG("NVOFA unavailable: nvOFInit returned " << StatusName(status) << ".");
        Shutdown();
        return false;
    }

    m_flowW = (width + m_grid - 1) / m_grid;
    m_flowH = (height + m_grid - 1) / m_grid;
    m_input[0] = CreateSurface(device, DXGI_FORMAT_B8G8R8A8_UNORM, width, height, L"NVOFA_Input0");
    m_input[1] = CreateSurface(device, DXGI_FORMAT_B8G8R8A8_UNORM, width, height, L"NVOFA_Input1");
    m_flow = CreateSurface(device, DXGI_FORMAT_R16G16_SINT, m_flowW, m_flowH, L"NVOFA_Flow_S10_5");
    if (costFormat != DXGI_FORMAT_UNKNOWN)
        m_cost = CreateSurface(device, costFormat, m_flowW, m_flowH, L"NVOFA_Cost");
    if (!m_input[0] || !m_input[1] || !m_flow) {
        LOG("NVOFA unavailable: its surfaces could not be created.");
        Shutdown();
        return false;
    }

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_appFence))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_ofaFence)))) {
        LOG("NVOFA unavailable: its synchronization fences could not be created.");
        Shutdown();
        return false;
    }

    if (!RegisterResource(m_input[0].Get(), &m_inputHandle[0]) ||
        !RegisterResource(m_input[1].Get(), &m_inputHandle[1]) ||
        !RegisterResource(m_flow.Get(), &m_flowHandle) ||
        (m_cost && !RegisterResource(m_cost.Get(), &m_costHandle))) {
        LOG("NVOFA unavailable: its surfaces could not be registered with the engine.");
        Shutdown();
        return false;
    }

    m_ready = true;
    LOG("NVOFA ready: " << width << "x" << height << " on a " << m_grid << "x" << m_grid
        << " grid (" << m_flowW << "x" << m_flowH << " vectors, S10.5 = 1/32 px), perf="
        << (init.perfLevel == NV_OF_PERF_LEVEL_SLOW ? "SLOW"
            : init.perfLevel == NV_OF_PERF_LEVEL_MEDIUM ? "MEDIUM" : "FAST")
        << ", cost="
        << (m_cost ? "on" : "unavailable") << ".");
    return true;
}

bool OpticalFlowNvof::RegisterResource(ID3D12Resource* resource, void** handle)
{
    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 params{};
    params.resource = resource;
    params.inputFencePoint.fence = m_appFence.Get();
    params.inputFencePoint.value = m_appValue;
    params.hOFGpuBuffer = reinterpret_cast<NvOFGPUBufferHandle*>(handle);
    params.outputFencePoint.fence = m_ofaFence.Get();
    params.outputFencePoint.value = ++m_ofaValue;
    const NV_OF_STATUS status =
        m_api->fn.nvOFRegisterResourceD3D12(static_cast<NvOFHandle>(m_session), &params);
    return status == NV_OF_SUCCESS && *handle != nullptr;
}

void OpticalFlowNvof::Capture(ID3D12GraphicsCommandList* cmd, ID3D12Resource* frame,
                              D3D12_RESOURCE_STATES frameState)
{
    if (!m_ready || !cmd || !frame) return;
    m_current ^= 1u;
    ID3D12Resource* destination = m_input[m_current].Get();

    D3D12_RESOURCE_BARRIER barriers[2]{};
    uint32_t count = 0;
    if (frameState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        barriers[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[count].Transition.pResource = frame;
        barriers[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[count].Transition.StateBefore = frameState;
        barriers[count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ++count;
    }
    barriers[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[count].Transition.pResource = destination;
    barriers[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[count].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    ++count;
    cmd->ResourceBarrier(count, barriers);

    cmd->CopyResource(destination, frame);

    count = 0;
    if (frameState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        barriers[count].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[count].Transition.StateAfter = frameState;
        ++count;
    }
    barriers[count].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[count].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    ++count;
    cmd->ResourceBarrier(count, barriers);
}

bool OpticalFlowNvof::Submit(ID3D12CommandQueue* queue)
{
    if (!m_ready || !queue) return false;
    // Frame one has nothing to compare against. Say so rather than executing against an
    // undefined slot; the caller emits zero motion for exactly this case.
    if (!m_hasPrevious) {
        m_hasPrevious = true;
        return false;
    }

    // The engine must not start before the copy recorded by Capture() has landed. That
    // copy is in the list the caller has just executed, so the signal orders the two.
    const uint64_t ready = ++m_appValue;
    if (FAILED(queue->Signal(m_appFence.Get(), ready))) return false;

    NV_OF_FENCE_POINT wait{};
    wait.fence = m_appFence.Get();
    wait.value = ready;
    NV_OF_FENCE_POINT done{};
    done.fence = m_ofaFence.Get();
    done.value = ++m_ofaValue;

    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};
    in.inputFrame = static_cast<NvOFGPUBufferHandle>(m_inputHandle[m_current]);
    in.referenceFrame = static_cast<NvOFGPUBufferHandle>(m_inputHandle[m_current ^ 1u]);
    in.externalHints = nullptr;
    // Each pair is judged on its own. The engine's temporal hints carry state across
    // calls, which survives a scene cut and makes the same pair answer differently
    // depending on what came before it.
    in.disableTemporalHints = NV_OF_TRUE;
    in.hPrivData = nullptr;
    in.numRois = 0;
    in.roiData = nullptr;
    in.numFencePoints = 1;
    in.fencePoint = &wait;

    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
    out.outputBuffer = static_cast<NvOFGPUBufferHandle>(m_flowHandle);
    out.outputCostBuffer = static_cast<NvOFGPUBufferHandle>(m_costHandle);
    out.hPrivData = nullptr;
    out.fencePoint = &done;

    const NV_OF_STATUS status =
        m_api->fn.nvOFExecuteD3D12(static_cast<NvOFHandle>(m_session), &in, &out);
    if (status != NV_OF_SUCCESS) {
        LOG("NVOFA execute failed with " << StatusName(status) << "; this frame carries no flow.");
        return false;
    }

    // A GPU-side wait, not a CPU one: the application queue simply does not start the
    // resolve pass until the engine has published the field.
    return SUCCEEDED(queue->Wait(m_ofaFence.Get(), done.value));
}

void OpticalFlowNvof::BeginRead(ID3D12GraphicsCommandList* cmd)
{
    if (!m_ready || !cmd) return;
    D3D12_RESOURCE_BARRIER barriers[2]{};
    uint32_t count = 0;
    for (ID3D12Resource* resource : {m_flow.Get(), m_cost.Get()}) {
        if (!resource) continue;
        barriers[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[count].Transition.pResource = resource;
        barriers[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[count].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barriers[count].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ++count;
    }
    if (count) cmd->ResourceBarrier(count, barriers);
}

void OpticalFlowNvof::EndRead(ID3D12GraphicsCommandList* cmd)
{
    if (!m_ready || !cmd) return;
    D3D12_RESOURCE_BARRIER barriers[2]{};
    uint32_t count = 0;
    for (ID3D12Resource* resource : {m_flow.Get(), m_cost.Get()}) {
        if (!resource) continue;
        barriers[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[count].Transition.pResource = resource;
        barriers[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[count].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[count].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        ++count;
    }
    if (count) cmd->ResourceBarrier(count, barriers);
}

void OpticalFlowNvof::UnregisterAll()
{
    if (!m_api || !m_api->fn.nvOFUnregisterResourceD3D12) return;
    for (void*& handle : m_inputHandle) {
        if (!handle) continue;
        NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 params{};
        params.hOFGpuBuffer = static_cast<NvOFGPUBufferHandle>(handle);
        m_api->fn.nvOFUnregisterResourceD3D12(&params);
        handle = nullptr;
    }
    for (void** handle : {&m_flowHandle, &m_costHandle}) {
        if (!*handle) continue;
        NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 params{};
        params.hOFGpuBuffer = static_cast<NvOFGPUBufferHandle>(*handle);
        m_api->fn.nvOFUnregisterResourceD3D12(&params);
        *handle = nullptr;
    }
}

void OpticalFlowNvof::Shutdown()
{
    m_ready = false;
    m_hasPrevious = false;
    // Order matters and follows the SDK's own: every buffer is unregistered and its
    // resource released while the session is still alive (NvOFBufferD3D12's destructor
    // unregisters, and the resource it holds goes with it), and only then is the session
    // destroyed. Releasing the surfaces after nvOFDestroy faulted on the way out.
    UnregisterAll();
    m_flow.Reset();
    m_cost.Reset();
    m_input[0].Reset();
    m_input[1].Reset();
    if (m_api && m_session && m_api->fn.nvOFDestroy) {
        m_api->fn.nvOFDestroy(static_cast<NvOFHandle>(m_session));
    }
    m_session = nullptr;
    m_appFence.Reset();
    m_ofaFence.Reset();
    delete m_api;
    m_api = nullptr;
    // The module is deliberately never unloaded. nvofapi64.dll is a driver component
    // that starts engine-side state of its own, and the SDK's own NvOFD3D12API holds it
    // for the life of the process for the same reason.
    m_library = nullptr;
}

#else  // DLSS_VIDEO_PLAYER_HAS_NVOF

struct OpticalFlowNvof::Api {};

OpticalFlowNvof::~OpticalFlowNvof() = default;
void OpticalFlowNvof::Shutdown() {}
void OpticalFlowNvof::Capture(ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES) {}
bool OpticalFlowNvof::Submit(ID3D12CommandQueue*) { return false; }
void OpticalFlowNvof::BeginRead(ID3D12GraphicsCommandList*) {}
void OpticalFlowNvof::EndRead(ID3D12GraphicsCommandList*) {}
bool OpticalFlowNvof::RegisterResource(ID3D12Resource*, void**) { return false; }
void OpticalFlowNvof::UnregisterAll() {}

bool OpticalFlowNvof::Initialize(ID3D12Device*, uint32_t, uint32_t)
{
    LOG("NVOFA unavailable: this build was configured without the NVIDIA Optical Flow SDK "
        "headers, so the CPU motion estimator is the only backend.");
    return false;
}

#endif  // DLSS_VIDEO_PLAYER_HAS_NVOF
