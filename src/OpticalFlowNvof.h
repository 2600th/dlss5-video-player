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
// the flow and cost surfaces it writes, and the two fences that join its engine to the
// application queue. Turning the flow grid into the full-resolution motion texture NGX
// reads is the caller's pass, because the caller owns that texture and its root
// signature.
//
// Initialize() returns false for every unavailable case - no nvofapi64.dll, a GPU or
// driver without the engine, an unsupported surface format or geometry - so the caller
// silently keeps the CPU estimator. Nothing here is ever bundled: nvofapi64.dll is
// loaded from the driver install by name.
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
    // False until two distinct frames have been captured, i.e. until a flow field can
    // exist at all. The caller must emit zero motion while this is false.
    bool HasPrevious() const { return m_hasPrevious; }

    // Discards the previous frame, so the next Submit() produces no flow. Used for the
    // first frame of a stream and for every scene cut.
    void Reset() { m_hasPrevious = false; }

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

    struct Api;
    Api* m_api = nullptr;          // opaque so the NVOFA headers stay out of this header
    void* m_session = nullptr;
    void* m_library = nullptr;

    ID3D12Device* m_device = nullptr;
    uint32_t m_width = 0, m_height = 0, m_flowW = 0, m_flowH = 0, m_grid = 0;
    bool m_ready = false;
    bool m_hasPrevious = false;
    uint32_t m_current = 0;        // index of the slot Capture() last wrote

    Microsoft::WRL::ComPtr<ID3D12Resource> m_input[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_flow;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cost;
    void* m_inputHandle[2]{nullptr, nullptr};
    void* m_flowHandle = nullptr;
    void* m_costHandle = nullptr;

    // The engine runs asynchronously: the application signals m_appFence once its copy
    // has landed, the engine waits on it, and signals m_ofaFence when the flow is ready.
    Microsoft::WRL::ComPtr<ID3D12Fence> m_appFence;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_ofaFence;
    uint64_t m_appValue = 0;
    uint64_t m_ofaValue = 0;
};
