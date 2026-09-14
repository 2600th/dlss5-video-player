#pragma once
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// NVIDIA Optical Flow Accelerator (NVOFA) backend for the temporal motion guide.
//
// Why this exists: the CPU block matcher in TemporalGuides.cpp analyses a 160x90 luma
// grid, so one motion vector covers 24x24 source pixels at 1080p and its finest step is
// 0.25 of a cell - 3.0 render pixels. Measured against a synthetic pan on non-periodic
// content, that estimator accepts 0% of cells at 0.5 px/frame and 2% at 1.0 px/frame:
// below roughly three pixels of motion per frame it does not report a small vector, it
// reports none, and a zeroed vector tells the reconstruction that nothing moved. NVOFA
// runs on a dedicated engine, estimates on a 1, 2 or 4 pixel grid, and returns S10.5
// fixed point - 1/32 of a pixel.
//
// The class owns only the optical-flow side: the input ping-pong the engine compares,
// the flow, cost and backward flow surfaces it writes, the 1x1 global flow vector it
// estimates, and the two fences that join its engine to the application queue. Turning
// the flow grid into the full-resolution motion texture NGX reads is the caller's pass,
// because the caller owns that texture and its root signature.
//
// Initialize() returns false for every unavailable case - no nvofapi64.dll, a GPU or
// driver without the engine, an unsupported surface format or geometry - so the caller
// silently keeps the CPU estimator. A refused capability is not one of those cases:
// both directions and the global flow estimate are asked for first and given up one at
// a time, because coarse hardware flow still beats a 24x24 CPU block match. Nothing
// here is ever bundled: nvofapi64.dll is loaded from the driver install by name.
class OpticalFlowNvof {
public:
    OpticalFlowNvof() = default;
    ~OpticalFlowNvof();
    OpticalFlowNvof(const OpticalFlowNvof&) = delete;
    OpticalFlowNvof& operator=(const OpticalFlowNvof&) = delete;

    // `width`/`height` are the resolution of the frames handed to Capture(), which is
    // also the unit the emitted vectors are measured in.
    bool Initialize(ID3D12Device* device, uint32_t width, uint32_t height);
    void Shutdown();

    bool Available() const { return m_ready; }
    // 1, 2 or 4: the side of the square block one flow vector describes.
    uint32_t Grid() const { return m_grid; }
    uint32_t FlowWidth() const { return m_flowW; }
    uint32_t FlowHeight() const { return m_flowH; }
    ID3D12Resource* Flow() const { return m_flow.Get(); }
    // Null when the engine did not offer a cost surface; the resolve pass then has no
    // confidence to gate on.
    ID3D12Resource* Cost() const { return m_cost.Get(); }
    // Null unless the engine accepted NV_OF_PRED_DIRECTION_BOTH, in which case the same
    // Execute also writes the reverse field, on the reference frame's grid. That is what
    // the resolve pass's round-trip gate reads; without it the gate stays off.
    ID3D12Resource* BackwardFlow() const { return m_backFlow.Get(); }

    // The engine's own estimate of the dominant motion of a pair, in input pixels.
    // Invalid while global flow is off, before the first pair, and after a Reset() until
    // a fresh pair has been estimated. It describes the most recent Execute whose copy
    // has completed, which is a frame or two behind the field because nothing here waits
    // on the GPU - and no CPU consumer could have better anyway, since the guide
    // analysis for a frame runs before the engine has seen that frame.
    struct GlobalFlow { float x = 0.0f; float y = 0.0f; bool valid = false; };
    GlobalFlow LastGlobalFlow() const { return m_global; }
    // False until two distinct frames have been captured, i.e. until a flow field can
    // exist at all. The caller must emit zero motion while this is false.
    bool HasPrevious() const { return m_hasPrevious; }

    // Discards the previous frame, so the next Submit() produces no flow. Used for the
    // first frame of a stream and for every scene cut. The global flow estimate goes
    // with it: it describes a pair from the far side of the discontinuity.
    void Reset() { m_hasPrevious = false; m_executed = false; m_globalPending = false; m_global = {}; }

    // Records a copy of `frame` - B8G8R8A8_UNORM at the initialized size - into the
    // engine's current input slot. Must be followed by Submit() on the same queue after
    // the recorded list has been executed.
    void Capture(ID3D12GraphicsCommandList* cmd, ID3D12Resource* frame,
                 D3D12_RESOURCE_STATES frameState);

    // Signals the application fence past the recorded copy, runs the engine against the
    // previous frame, and makes `queue` wait for the result on the GPU. No CPU stall.
    // Returns false when the engine refused the frame; the caller then falls back for
    // this frame. `fenceValue` must be a value the caller has not signalled before.
    bool Submit(ID3D12CommandQueue* queue);

    // The flow surface is handed to the engine in COMMON and read by the resolve pass as
    // a shader resource; these bracket that read.
    void BeginRead(ID3D12GraphicsCommandList* cmd);
    void EndRead(ID3D12GraphicsCommandList* cmd);

private:
    bool RegisterResource(ID3D12Resource* resource, void** handle);
    void UnregisterAll();
    // Latches the last completed global flow copy and records the next one into the
    // list Capture() is already filling. The class keeps no queue of its own, so this is
    // the only way the vector reaches the CPU without a stall.
    void ReadbackGlobalFlow(ID3D12GraphicsCommandList* cmd);

    struct Api;
    Api* m_api = nullptr;          // opaque so the NVOFA headers stay out of this header
    void* m_session = nullptr;
    void* m_library = nullptr;

    ID3D12Device* m_device = nullptr;
    uint32_t m_width = 0, m_height = 0, m_flowW = 0, m_flowH = 0, m_grid = 0;
    bool m_ready = false;
    bool m_hasPrevious = false;
    bool m_executed = false;       // a pair has actually been through the engine
    uint32_t m_current = 0;        // index of the slot Capture() last wrote

    Microsoft::WRL::ComPtr<ID3D12Resource> m_input[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_flow;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cost;
    void* m_inputHandle[2]{nullptr, nullptr};
    void* m_flowHandle = nullptr;
    void* m_costHandle = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_backFlow;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_backCost;
    void* m_backFlowHandle = nullptr;
    void* m_backCostHandle = nullptr;

    // Global flow is one vector in a 1x1 surface. The readback buffer beside it stays
    // mapped for the life of the session and is read only once the application fence
    // has passed the value that publishes the copy.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_globalSurface;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_globalReadback;
    void* m_globalHandle = nullptr;
    const unsigned char* m_globalMapped = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_globalFootprint{};
    uint64_t m_globalValue = 0;
    bool m_globalPending = false;
    GlobalFlow m_global{};

    // The engine runs asynchronously: the application signals m_appFence once its copy
    // has landed, the engine waits on it, and signals m_ofaFence when the flow is ready.
    Microsoft::WRL::ComPtr<ID3D12Fence> m_appFence;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_ofaFence;
    uint64_t m_appValue = 0;
    uint64_t m_ofaValue = 0;
};

// Whether hardware flow can be asked for a session, at what geometry, and what its
// vectors have to be multiplied by. Decided from sizes alone, before any device call,
// so the renderer can log the choice and a test can check it on a machine with no flow
// engine.
//
// The engine compares the decoded frame itself - Capture() copies that exact texture
// and D3D12 CopyResource carries no scale - so it is asked for the decoded size rather
// than the DLSS input size. On every neural-size path the two are equal and this is the
// same request as before. A runtime Super Resolution session is the case where they
// differ, and it used to be the case that silently kept the CPU block matcher. NGX
// reads motion in DLSS input pixels, which is also the unit TemporalGuides emits, so a
// vector measured on the decoded grid is converted by the per-axis ratio between the
// two sizes. That conversion is exact rather than approximate because the pass that
// produces the DLSS input is a full-image resample of the decoded texture - one
// full-screen triangle over uv 0..1, no crop and no letterbox - so the two grids differ
// by nothing except that ratio.
//
// Nothing else is refused here. A geometry the engine cannot run is the engine's own
// answer: Initialize() puts the size asked for against the four NV_OF_CAPS bounds below
// and says which one was missed, and the caller keeps the CPU estimator for exactly
// that case.
struct HardwareFlowPlan {
    bool attempt = false;
    // The geometry to initialize the engine at: the decoded frame's own size.
    uint32_t width = 0, height = 0;
    // Engine-input pixels -> DLSS input pixels. Exactly 1 on a neural-size path, where
    // the division is of a number by itself.
    float motionScaleX = 1.0f, motionScaleY = 1.0f;
    // Why the CPU estimator is kept. Empty while `attempt` is true.
    const char* refusal = "";
};

constexpr HardwareFlowPlan PlanHardwareFlow(uint32_t sourceW, uint32_t sourceH,
                                            uint32_t renderW, uint32_t renderH) noexcept
{
    HardwareFlowPlan plan;
    if (!sourceW || !sourceH || !renderW || !renderH) {
        plan.refusal = "the decoded frame or the DLSS input has a zero dimension";
        return plan;
    }
    plan.attempt = true;
    plan.width = sourceW;
    plan.height = sourceH;
    plan.motionScaleX = float(renderW) / float(sourceW);
    plan.motionScaleY = float(renderH) / float(sourceH);
    return plan;
}

// The engine's own geometry limits, as Initialize() applies them. A zero maximum is a
// query that did not answer, and a bound nobody reported cannot refuse anything.
constexpr bool FlowGeometrySupported(uint32_t width, uint32_t height,
                                     uint32_t minWidth, uint32_t minHeight,
                                     uint32_t maxWidth, uint32_t maxHeight) noexcept
{
    if (!maxWidth || !maxHeight) return true;
    return width >= minWidth && height >= minHeight && width <= maxWidth &&
           height <= maxHeight;
}
