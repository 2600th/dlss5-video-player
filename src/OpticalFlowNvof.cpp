#include "OpticalFlowNvof.h"

#include "Log.h"

// The Optical Flow SDK headers are an NVIDIA component and are never committed or
// redistributed, exactly like the DLSS SDK beside them. A tree without them still
// builds: every entry point below reports the engine as unavailable and the caller
// keeps the CPU estimator. docs/BUILDING.md says where to put them.
#if defined(DLSS_VIDEO_PLAYER_HAS_NVOF)

#include <algorithm>
#include <cstring>
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

const char* ModeName(NV_OF_PRED_DIRECTION direction, bool global)
{
    if (direction == NV_OF_PRED_DIRECTION_BOTH)
        return global ? "both directions with global flow" : "both directions";
    return global ? "forward only with global flow" : "forward only";
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
    if (!FlowGeometrySupported(width, height, caps[0], caps[1], caps[2], caps[3])) {
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

    // Global flow arrives as a single NV_OF_FLOW_VECTOR, which is two int16s - the same
    // layout as a cell of the field. A device that names a format this side cannot
    // decode gets no global flow rather than a guessed decode; a query the driver does
    // not answer at all leaves the documented layout in place, which is also the format
    // the forward surface above is created with unqueried.
    DXGI_FORMAT globalFormat = DXGI_FORMAT_R16G16_SINT;
    uint32_t globalCount = 0;
    if (fn.nvOFGetSurfaceFormatCountD3D12(handle, NV_OF_BUFFER_USAGE_GLOBAL_FLOW,
                                          NV_OF_MODE_OPTICALFLOW, &globalCount) == NV_OF_SUCCESS &&
        globalCount > 0) {
        std::vector<DXGI_FORMAT> globalFormats(globalCount);
        fn.nvOFGetSurfaceFormatD3D12(handle, NV_OF_BUFFER_USAGE_GLOBAL_FLOW,
                                     NV_OF_MODE_OPTICALFLOW, globalFormats.data());
        if (std::find(globalFormats.begin(), globalFormats.end(), globalFormat) ==
            globalFormats.end()) {
            globalFormat = DXGI_FORMAT_UNKNOWN;
            LOG("NVOFA: the engine names no global flow format this build can decode, so "
                "global flow will not be asked for.");
        }
    }

    // Neither the surfaces nor the fences depend on which mode the engine accepts, so
    // they are created once here and the ladder below only adds what its rung needs.
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
    init.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;

    // Both directions and the global flow estimate are the two capabilities worth asking
    // this engine for: the reverse field lets the resolve pass discard vectors the engine
    // contradicts itself about, and the global vector is the only motion evidence a
    // scene-cut test can have that is not its own arithmetic again. Neither is worth
    // losing hardware flow over, though, so a refused capability steps down one rung
    // instead of failing, and the reverse field is given up last because it is the one
    // that changes what the reconstruction sees. A refused nvOFInit leaves the session
    // in a state the SDK offers no way to reset, so each rung gets a fresh one.
    struct Mode { NV_OF_PRED_DIRECTION direction; bool global; };
    const Mode ladder[] = {{NV_OF_PRED_DIRECTION_BOTH, true},
                           {NV_OF_PRED_DIRECTION_BOTH, false},
                           {NV_OF_PRED_DIRECTION_FORWARD, true},
                           {NV_OF_PRED_DIRECTION_FORWARD, false}};
    bool initialized = false;
    for (const Mode& mode : ladder) {
        if (mode.global && globalFormat == DXGI_FORMAT_UNKNOWN) continue;
        if (!handle) {
            status = fn.nvCreateOpticalFlowD3D12(device, reinterpret_cast<NvOFHandle*>(&m_session));
            if (status != NV_OF_SUCCESS || !m_session) {
                LOG("NVOFA unavailable: nvCreateOpticalFlowD3D12 returned " << StatusName(status)
                    << " while stepping down to " << ModeName(mode.direction, mode.global) << ".");
                Shutdown();
                return false;
            }
            handle = static_cast<NvOFHandle>(m_session);
        }
        init.predDirection = mode.direction;
        init.enableGlobalFlow = mode.global ? NV_OF_TRUE : NV_OF_FALSE;
        status = fn.nvOFInit(handle, &init);
        if (status == NV_OF_SUCCESS) {
            if (mode.direction == NV_OF_PRED_DIRECTION_BOTH) {
                m_backFlow = CreateSurface(device, DXGI_FORMAT_R16G16_SINT, m_flowW, m_flowH,
                                           L"NVOFA_BackFlow_S10_5");
                // enableOutputCost covers both directions at once, so the backward cost
                // exists exactly when the forward one does. It is allocated so the engine
                // has somewhere to put it; nothing reads it until the cost gate itself is
                // measured.
                if (m_cost)
                    m_backCost = CreateSurface(device, costFormat, m_flowW, m_flowH,
                                               L"NVOFA_BackCost");
            }
            if (mode.global)
                m_globalSurface = CreateSurface(device, globalFormat, 1, 1, L"NVOFA_GlobalFlow");
            const bool allocated = (mode.direction != NV_OF_PRED_DIRECTION_BOTH ||
                                    (m_backFlow && (!m_cost || m_backCost))) &&
                                   (!mode.global || m_globalSurface);
            if (allocated) {
                initialized = true;
                break;
            }
            LOG("NVOFA: the surfaces " << ModeName(mode.direction, mode.global)
                << " needs could not be created.");
            m_backFlow.Reset();
            m_backCost.Reset();
            m_globalSurface.Reset();
        } else {
            LOG("NVOFA: nvOFInit refused " << ModeName(mode.direction, mode.global) << " with "
                << StatusName(status) << ".");
        }
        if (fn.nvOFDestroy) fn.nvOFDestroy(handle);
        m_session = nullptr;
        handle = nullptr;
    }
    if (!initialized) {
        LOG("NVOFA unavailable: nvOFInit refused every mode, last with "
            << StatusName(status) << ".");
        Shutdown();
        return false;
    }

    if (!RegisterResource(m_input[0].Get(), &m_inputHandle[0]) ||
        !RegisterResource(m_input[1].Get(), &m_inputHandle[1]) ||
        !RegisterResource(m_flow.Get(), &m_flowHandle) ||
        (m_cost && !RegisterResource(m_cost.Get(), &m_costHandle)) ||
        (m_backFlow && !RegisterResource(m_backFlow.Get(), &m_backFlowHandle)) ||
        (m_backCost && !RegisterResource(m_backCost.Get(), &m_backCostHandle)) ||
        (m_globalSurface && !RegisterResource(m_globalSurface.Get(), &m_globalHandle))) {
        LOG("NVOFA unavailable: its surfaces could not be registered with the engine.");
        Shutdown();
        return false;
    }

    // The CPU side of global flow: four bytes copied out of the 1x1 surface into a buffer
    // that stays mapped. Losing it costs only the reading, not the estimate, so the
    // engine still comes up with global flow on.
    if (m_globalSurface) {
        const D3D12_RESOURCE_DESC surface = m_globalSurface->GetDesc();
        uint64_t total = 0;
        device->GetCopyableFootprints(&surface, 0, 1, 0, &m_globalFootprint, nullptr, nullptr,
                                      &total);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.Format = DXGI_FORMAT_UNKNOWN;
        buffer.SampleDesc = {1, 0};
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* mapped = nullptr;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&m_globalReadback))) ||
            FAILED(m_globalReadback->Map(0, nullptr, &mapped))) {
            m_globalReadback.Reset();
            LOG("NVOFA: the global flow readback buffer could not be created, so the "
                "estimate stays on the GPU.");
        } else {
            m_globalReadback->SetName(L"NVOFA_GlobalFlow_Readback");
            m_globalMapped = static_cast<const unsigned char*>(mapped);
        }
    }

    m_ready = true;
    LOG("NVOFA ready: " << width << "x" << height << " on a " << m_grid << "x" << m_grid
        << " grid (" << m_flowW << "x" << m_flowH << " vectors, S10.5 = 1/32 px), perf="
        << (init.perfLevel == NV_OF_PERF_LEVEL_SLOW ? "SLOW"
            : init.perfLevel == NV_OF_PERF_LEVEL_MEDIUM ? "MEDIUM" : "FAST")
        << ", cost="
        << (m_cost ? "on" : "unavailable")
        << ", direction="
        // What this knows is whether a backward field exists and is bound, which is
        // what the resolve pass needs to reject a vector on round-trip disagreement.
        // It deliberately does not claim the rejection happened: the gate itself
        // lives in the resolve shader, and a build with the backward field bound and
        // the rejection removed would still print "armed" here - which is exactly
        // what the 2026-09-15 gate A/B found when it tried to use this line as its
        // per-arm assertion (measurements/q1-gate-camera-original-20260915).
        << (m_backFlow ? "both, backward field bound for the round-trip gate"
                       : "forward only, no backward field so the gate cannot run")
        << ", global flow="
        << (!m_globalSurface ? "off" : m_globalMapped ? "on" : "on but unreadable") << ".");
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

    ReadbackGlobalFlow(cmd);
}

void OpticalFlowNvof::ReadbackGlobalFlow(ID3D12GraphicsCommandList* cmd)
{
    if (!m_globalMapped) return;
    // Submit() signals the application fence exactly once per frame, so the value it
    // reaches next is the one that publishes the copy recorded below, and reading the
    // buffer once the fence has passed that value is a comparison rather than a wait.
    if (m_globalPending && m_appFence->GetCompletedValue() >= m_globalValue) {
        int16_t vector[2]{};
        std::memcpy(vector, m_globalMapped, sizeof(vector));
        m_global = {vector[0] / 32.0f, vector[1] / 32.0f, true};
        m_globalPending = false;
    }
    if (!m_executed) return;

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = m_globalReadback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = m_globalFootprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = m_globalSurface.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    // No barrier: COMMON promotes to COPY_SOURCE for this copy and decays straight back.
    // The engine cannot be writing the surface meanwhile either, because its next
    // Execute waits on the fence value Submit() signals after this list.
    cmd->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    m_globalValue = m_appValue + 1;
    m_globalPending = true;
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
    // Null unless the mode that came up asked for them, which is exactly the condition
    // the SDK states for each: one Execute fills whichever of these are present.
    out.bwdOutputBuffer = static_cast<NvOFGPUBufferHandle>(m_backFlowHandle);
    out.bwdOutputCostBuffer = static_cast<NvOFGPUBufferHandle>(m_backCostHandle);
    out.globalFlowBuffer = static_cast<NvOFGPUBufferHandle>(m_globalHandle);
    out.fencePoint = &done;

    const NV_OF_STATUS status =
        m_api->fn.nvOFExecuteD3D12(static_cast<NvOFHandle>(m_session), &in, &out);
    if (status != NV_OF_SUCCESS) {
        LOG("NVOFA execute failed with " << StatusName(status) << "; this frame carries no flow.");
        return false;
    }

    // The surfaces now hold an estimate rather than whatever the allocation left there,
    // which is what makes the global flow copy in the next Capture() worth recording.
    m_executed = true;

    // A GPU-side wait, not a CPU one: the application queue simply does not start the
    // resolve pass until the engine has published the field.
    return SUCCEEDED(queue->Wait(m_ofaFence.Get(), done.value));
}

void OpticalFlowNvof::BeginRead(ID3D12GraphicsCommandList* cmd)
{
    if (!m_ready || !cmd) return;
    D3D12_RESOURCE_BARRIER barriers[3]{};
    uint32_t count = 0;
    // The backward cost is deliberately not in this list: the engine writes it, and
    // nothing reads it until the cost gate has thresholds that were measured.
    for (ID3D12Resource* resource : {m_flow.Get(), m_cost.Get(), m_backFlow.Get()}) {
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
    D3D12_RESOURCE_BARRIER barriers[3]{};
    uint32_t count = 0;
    for (ID3D12Resource* resource : {m_flow.Get(), m_cost.Get(), m_backFlow.Get()}) {
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
    for (void** handle : {&m_flowHandle, &m_costHandle, &m_backFlowHandle, &m_backCostHandle,
                          &m_globalHandle}) {
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
    m_executed = false;
    // Order matters and follows the SDK's own: every buffer is unregistered and its
    // resource released while the session is still alive (NvOFBufferD3D12's destructor
    // unregisters, and the resource it holds goes with it), and only then is the session
    // destroyed. Releasing the surfaces after nvOFDestroy faulted on the way out.
    UnregisterAll();
    m_flow.Reset();
    m_cost.Reset();
    m_backFlow.Reset();
    m_backCost.Reset();
    m_globalSurface.Reset();
    m_input[0].Reset();
    m_input[1].Reset();
    if (m_globalReadback && m_globalMapped) m_globalReadback->Unmap(0, nullptr);
    m_globalMapped = nullptr;
    m_globalReadback.Reset();
    m_globalPending = false;
    m_global = {};
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
void OpticalFlowNvof::ReadbackGlobalFlow(ID3D12GraphicsCommandList*) {}

bool OpticalFlowNvof::Initialize(ID3D12Device*, uint32_t, uint32_t)
{
    LOG("NVOFA unavailable: this build was configured without the NVIDIA Optical Flow SDK "
        "headers, so the CPU motion estimator is the only backend.");
    return false;
}

#endif  // DLSS_VIDEO_PLAYER_HAS_NVOF
