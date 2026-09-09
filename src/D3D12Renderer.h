#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <memory>
#include <vector>
#include "D3D12FenceWait.h"
#include "DLSSBackend.h"

#ifdef D3D12_RENDERER_TESTING
#include <functional>
struct D3D12RendererTestAccess;
struct D3D12RendererTestOwnedResource {
    virtual ~D3D12RendererTestOwnedResource()=default;
};
#endif

class D3D12Renderer;
struct D3D12RendererDeleter {
    void operator()(D3D12Renderer* renderer) const noexcept;
};
using D3D12RendererOwner=std::unique_ptr<D3D12Renderer,D3D12RendererDeleter>;
D3D12RendererOwner MakeD3D12Renderer();

struct CapturedVideoFrame {
    // Tightly packed 8-bit RGBA, matching the R8G8B8A8_UNORM cache render target. The
    // encoder is configured with EncoderPixelFormat::Rgba so no channel swizzle is
    // needed on the CPU. Named for the payload, not a channel order, because the test
    // hook may supply any layout.
    std::vector<uint8_t> pixels;
    uint32_t width{};
    uint32_t height{};
};

class D3D12Renderer {
public:
    D3D12Renderer()=default;
    enum class DebugView { Final, Input, MotionVectors, Depth, BiasMask };

    struct ColorSettings {
        float brightness = 0.0f;   // exposure-like brightness, in stops (-2..+2)
        float contrast = 1.0f;     // 0..3
        float saturation = 1.0f;   // 0..3
        float gamma = 1.0f;        // 0.25..3
        float temperature = 0.0f;  // -1..+1 (cool..warm)
        float tint = 0.0f;         // -1..+1 (green..magenta)
    };

    bool Initialize(HWND hwnd, uint32_t sourceW, uint32_t sourceH,
                    uint32_t outputW, uint32_t outputH,
                    uint32_t gridW, uint32_t gridH,
                    NVSDK_NGX_PerfQuality_Value quality, bool preserveSource = false);
    bool RenderFrame(const uint8_t* bgra, size_t bytes,
                     const float* guideGridRGBA32F, size_t guideBytes,
                     uint32_t gridW, uint32_t gridH,
                     bool temporalReset, float frameTimeMs);
    bool RenderFrameForCache(const uint8_t* bgra, size_t bytes,
                             const float* guideGridRGBA32F, size_t guideBytes,
                             uint32_t gridW, uint32_t gridH,
                             bool temporalReset, float frameTimeMs,
                             CapturedVideoFrame& capture);

    // Number of readback slots, and therefore the number of captures that may be in
    // flight before ResolveOldestCapture must be called.
    static constexpr uint32_t CaptureSlots = 2;

    // Asynchronous capture. EnqueueEvaluatedFrameCapture records the cache draw and the
    // readback copy for the frame just rendered and signals a per-slot fence WITHOUT
    // draining the GPU. ResolveOldestCapture then waits on the oldest slot only. This
    // lets the CPU-side copy and the encoder overlap with GPU work on the next frame,
    // where the old synchronous path left the GPU idle for the whole CPU stage.
    bool EnqueueEvaluatedFrameCapture();
    uint32_t PendingCaptureCount() const { return m_capturePending; }
    // On success every byte of capture.pixels is overwritten, and the buffer is only
    // resized when it does not already hold exactly one frame. Hand back the same
    // CapturedVideoFrame each time and the per-frame allocation and its full-frame
    // zero-fill both disappear. On failure the capture is cleared.
    bool ResolveOldestCapture(CapturedVideoFrame& capture);

    // Synchronous capture of the frame just rendered by RenderFrame. Valid only while
    // nothing is in flight. The offline job uses it for the first frame, whose evidence
    // receipt loop must read pixels back before deciding whether to resubmit.
    bool CaptureRenderedFrame(CapturedVideoFrame& capture) { return CaptureEvaluatedFrame(capture); }


    void SetDLSS(bool enabled) { m_dlssEnabled = enabled; }
    bool DLSSAvailable() const { return m_dlss.Available(); }
    bool DLSSEnabled() const { return m_dlssEnabled && m_dlss.Available(); }
    bool DLSSRequested() const { return m_dlssEnabled; }
    bool LastFrameUsedDLSS() const { return m_lastDLSSUsed; }
    uint32_t DLSSInputW() const { return m_renderW; }
    uint32_t DLSSInputH() const { return m_renderH; }
    uint32_t OutputW() const { return m_outputW; }
    uint32_t OutputH() const { return m_outputH; }
    void SetDebugView(DebugView v) { m_debugView = v; }
    DebugView GetDebugView() const { return m_debugView; }
    void RequestDLSSRecreate() { m_recreateRequested = true; }
    uint64_t FramesPresented() const { return m_framesPresented; }
    bool DLSSFeatureCreated() const { return m_dlss.FeatureCreated(); }
    uint64_t DLSSEvaluations() const { return m_dlss.EvaluationCount(); }
    bool DLSSLastEvaluationUsedC() const { return m_dlss.LastEvaluationUsedC(); }
    NVSDK_NGX_Result DLSSLastResult() const { return m_dlss.LastResult(); }
    d3d12_renderer_detail::FenceWaitResult WaitGPU();
    bool PresentCurrent();
    void SetColorSettings(const ColorSettings& settings) { m_colorSettings = settings; }
    const ColorSettings& GetColorSettings() const { return m_colorSettings; }

private:
    friend struct D3D12RendererDeleter;
    ~D3D12Renderer();
    static constexpr uint32_t FrameCount = 3;
    // NVIDIA's D3D12 DLSS contract expects input resources in NON_PIXEL_SHADER_RESOURCE
    // at EvaluateFeature time. Debug/presentation passes temporarily transition selected
    // resources to PIXEL_SHADER_RESOURCE and restore them before the frame ends.
    static constexpr D3D12_RESOURCE_STATES GuideReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    static constexpr D3D12_RESOURCE_STATES DepthGuideReadState =
        D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    bool CreateDeviceAndSwapchain(HWND hwnd);
    bool CreateHeapsAndBackbuffers();
    bool CreatePipelines();
    bool CreateVideoResources();
    bool InitializeDLSS(bool& gpuSynchronized);
    bool CreateUploadForTexture(const D3D12_RESOURCE_DESC& desc,
                                Microsoft::WRL::ComPtr<ID3D12Resource>& upload,
                                uint8_t*& mapped,
                                D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                uint32_t& rows,
                                uint64_t& rowBytes,
                                uint64_t& totalBytes,
                                const char* name);
    void CopyMappedRows(uint8_t* mapped, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
                        const void* src, size_t tightRowBytes, uint32_t rows);
    bool WaitForFrameSlot(uint32_t slot);
    bool SignalFrameSlot(uint32_t slot);
    bool WaitGPUForContinuedUse();
    bool WaitForFenceValue(uint64_t value);
    // Synchronous enqueue + resolve. Kept for the first-frame evidence loop, which must
    // read a capture back before it can decide whether to submit the same frame again.
    bool CaptureEvaluatedFrame(CapturedVideoFrame& capture);
    d3d12_renderer_detail::FenceWaitResult DrainForRetirement();
    void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                 D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    D3D12_CPU_DESCRIPTOR_HANDLE RTV(uint32_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DSV() const;
    D3D12_CPU_DESCRIPTOR_HANDLE SRVCPU(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SRVGPU(uint32_t index) const;
    static float Halton(uint32_t index, uint32_t base);

    HWND m_hwnd = nullptr;
    uint32_t m_sourceW=0,m_sourceH=0,m_outputW=0,m_outputH=0,m_renderW=0,m_renderH=0,m_gridW=0,m_gridH=0;
    NVSDK_NGX_PerfQuality_Value m_quality = DefaultNeuralCarrierQuality();

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocators[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmds[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    uint64_t m_frameFence[FrameCount]{};
    uint32_t m_frameSlot = 0;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    uint32_t m_rtvInc=0,m_srvInc=0,m_dsvInc=0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_backbuffers[FrameCount];

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoConvert;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPresent;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoMotionDebug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDepthDebug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDepthWrite;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoExpandGuides;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_decodedTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_upload[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dlssColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;      // R32_TYPELESS: D32 DSV + R32 SRV, same resource passed to NGX
    Microsoft::WRL::ComPtr<ID3D12Resource> m_motion;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_biasCurrent;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dlssOutput;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_guideGrid;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_guideUpload[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cacheOutput;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cacheReadback[CaptureSlots];

    uint8_t* m_uploadMapped[FrameCount]{};
    uint8_t* m_guideMapped[FrameCount]{};
    uint8_t* m_cacheReadbackMapped[CaptureSlots]{};
    uint64_t m_captureFence[CaptureSlots]{};
    uint32_t m_captureWrite = 0;
    uint32_t m_captureRead = 0;
    uint32_t m_capturePending = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_uploadFootprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_guideFootprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_cacheFootprint{};
    uint32_t m_numRows=0,m_guideRows=0;
    uint64_t m_rowSize=0,m_uploadBytes=0,m_guideRowSize=0,m_guideUploadBytes=0;
    uint32_t m_cacheRows=0;
    uint64_t m_cacheRowSize=0,m_cacheReadbackBytes=0;

    bool m_sourceInCopyDest = true;
    bool m_gridInCopyDest = true;
    bool m_colorInRT = true;
    bool m_guidesInRT = true;
    bool m_depthInWrite = true;
    bool m_outputInUAV = true;
    bool m_dlssEnabled = true;
    bool m_allowTearing = false;
    bool m_recreateRequested = false;
    bool m_delayedRecreateDone = false;
    bool m_preserveSource = false;
    uint64_t m_framesPresented = 0;
    DebugView m_debugView = DebugView::Final;
    ColorSettings m_colorSettings{};
    bool m_lastDLSSUsed = false;
    bool m_gpuUnusable = false;
    d3d12_renderer_detail::FenceWaitResult m_lastFenceWaitResult =
        d3d12_renderer_detail::FenceWaitResult::Completed;
    DLSSBackend m_dlss;

#ifdef D3D12_RENDERER_TESTING
    friend struct D3D12RendererTestAccess;
    std::function<d3d12_renderer_detail::FenceWaitResult()> m_testWaitGPU;
    std::function<HRESULT(uint64_t)> m_testFrameSignal;
    std::function<HRESULT()> m_testDeviceRemovedReason;
    std::function<bool(std::vector<uint8_t>&)> m_testCacheCapture;
    std::unique_ptr<D3D12RendererTestOwnedResource> m_testOwnedResource;
#endif
};
