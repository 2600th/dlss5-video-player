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
#include "FrameIdentity.h"
#include "NgxSession.h"

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
struct GuideFrame;

struct CapturedVideoFrame {
    // Tightly packed 8-bit BGRA, matching the B8G8R8A8_UNORM cache render target. The
    // encoder is configured with EncoderPixelFormat::Bgra so no channel swizzle is
    // needed on the CPU. Named for the payload, not a channel order, because the test
    // hook may supply any layout.
    std::vector<uint8_t> pixels;
    uint32_t width{};
    uint32_t height{};
    FrameIdentity id;
};

// Which member of an original/neural pair the presentation shader shows.
enum class ComparisonMode { Neural, Original, Blend, SplitVertical, Wipe };

struct ComparisonSettings {
    ComparisonMode mode = ComparisonMode::Neural;
    float amount = 0.5f;       // Blend: lerp(original, neural, amount)
    float splitX = 0.5f;       // SplitVertical/Wipe divider, in image UV [0,1]
    float zoomScale = 1.0f;    // >= 1 magnifies around the zoom center
    float zoomCenterX = 0.5f;
    float zoomCenterY = 0.5f;
};

class D3D12Renderer {
public:
    D3D12Renderer()=default;
    enum class DebugView { Final, Input, MotionVectors, Depth };

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
    // Identity-checked entry points: the guide must have been generated for
    // exactly `frame` (same source sample, same job); otherwise the frame is
    // rejected and logged. The temporal reset comes from guide.id.reset or a
    // feature (re)creation performed during this frame.
    bool RenderFrame(const uint8_t* bgra, size_t bytes, const FrameIdentity& frame,
                     const GuideFrame& guide, float frameTimeMs);
    bool RenderFrameForCache(const uint8_t* bgra, size_t bytes, const FrameIdentity& frame,
                             const GuideFrame& guide, float frameTimeMs,
                             CapturedVideoFrame& capture);

    // Number of readback slots, and therefore the number of captures that may be in
    // flight before ResolveOldestCapture must be called. One of them is normally spoken
    // for by a copy still running on another thread (see BeginResolveOldestCapture), so
    // the count is one above the GPU pipeline depth it supports.
    static constexpr uint32_t CaptureSlots = 4;

    // Where an enqueued capture's bytes are sitting, handed out so the copy out of them
    // can run somewhere other than the render loop. Valid only between the
    // BeginResolveOldestCapture that produced it and the matching End.
    struct CaptureReadbackView {
        const uint8_t* base = nullptr;   // first byte of row 0, footprint offset applied
        size_t rowPitch = 0;             // may exceed width*4; rows are padded
        size_t bytes = 0;                // tightly packed size the copy produces
        uint32_t width = 0, height = 0;
    };

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

    // ResolveOldestCapture split in two, so the copy does not have to run on the thread
    // that owns the renderer. Begin waits on the oldest slot's fence and describes its
    // memory; the slot stays reserved, and PendingCaptureCount keeps counting it, until
    // End retires it. Nothing may enqueue over a slot whose copy is still running, and
    // holding one is exactly what the extra CaptureSlots entry pays for. Begin retires
    // the slot itself when it fails, so a failure cannot wedge the ring; the caller must
    // then not call End. Only CopyCaptureView is safe to run off the renderer's thread.
    bool BeginResolveOldestCapture(CaptureReadbackView& view);
    void EndResolveOldestCapture();
    // Unpacks a view into tightly packed BGRA. Touches no renderer state, so it may run
    // on any thread while the renderer keeps working, and it fans out on its own worker
    // pool rather than the default one for that reason.
    static void CopyCaptureView(const CaptureReadbackView& view, std::vector<uint8_t>& pixels);

    // Synchronous capture of the frame just rendered by RenderFrame. Valid only while
    // nothing is in flight. The offline job uses it for the first frame, whose evidence
    // receipt loop must read pixels back before deciding whether to resubmit.
    bool CaptureRenderedFrame(CapturedVideoFrame& capture) { return CaptureEvaluatedFrame(capture); }

    // Coarse accounting for the offline export, which otherwise cannot tell a slow GPU
    // apart from a swapchain that is pacing it. Both counters only ever move on the
    // thread that drives the renderer, so they need no synchronisation.
    //
    //  * FenceWaitNanos is time the CPU spent parked waiting for the GPU. A large share
    //    means the export is GPU bound and more CPU threads will not help.
    //  * PresentNanos is time inside IDXGISwapChain::Present, where DXGI blocks once the
    //    frame latency limit is reached. A headless export only presents the first
    //    DelayedRecreateFrame frames, so past the start of a job this stops growing; a
    //    total that keeps climbing means SetHeadless never took.
    uint64_t FenceWaitNanos() const { return m_fenceWaitNanos; }
    uint64_t RenderSlotWaitNanos() const { return m_renderSlotWaitNanos; }
    uint64_t CaptureSubmitSlotWaitNanos() const { return m_captureSubmitSlotWaitNanos; }
    uint64_t CaptureResolveWaitNanos() const { return m_captureResolveWaitNanos; }
    uint64_t PresentSlotWaitNanos() const { return m_presentSlotWaitNanos; }
    uint64_t PresentNanos() const { return m_presentNanos; }
    // Zeroed at the start of each export attempt so a libx264 retry after an NVENC
    // failure is measured on its own, not on the sum of both passes.
    void ResetStageCounters() {
        m_fenceWaitNanos = 0;
        m_renderSlotWaitNanos = 0;
        m_captureSubmitSlotWaitNanos = 0;
        m_captureResolveWaitNanos = 0;
        m_presentSlotWaitNanos = 0;
        m_presentNanos = 0;
    }


    // The offline export encodes the cache render target that EnqueueEvaluatedFrameCapture
    // draws for itself, and its window is never shown, so the backbuffer pass RenderFrame
    // records is thrown away: a full-resolution clear and draw of GPU work per frame, the
    // CPU time inside Present, and the DXGI frame-latency stall that paces the export at
    // display rate. Headless drops that pass. The early presents still go out, because the
    // NGX feature is created and re-created against those frame counts and the swapchain
    // hooks that watch for it need real presents to initialize.
    void SetHeadless(bool headless) { m_headless = headless; }
    bool PresentsThisFrame() const {
        return !m_headless || m_framesPresented <= ngx_session_detail::DelayedRecreateFrame;
    }

    void SetDLSS(bool enabled) { m_dlssEnabled = enabled; }
    bool DLSSAvailable() const { return m_dlss.Available(); }
    bool DLSSEnabled() const { return m_dlssEnabled && m_dlss.Available(); }
    bool LastFrameUsedDLSS() const { return m_lastDLSSUsed; }
    uint32_t DLSSInputW() const { return m_renderW; }
    uint32_t DLSSInputH() const { return m_renderH; }
    uint32_t OutputW() const { return m_outputW; }
    uint32_t OutputH() const { return m_outputH; }
    void SetDebugView(DebugView v) { m_debugView = v; }
    DebugView GetDebugView() const { return m_debugView; }
    void RequestDLSSRecreate() { m_recreateRequested = true; }
    bool DLSSFeatureCreated() const { return m_dlss.FeatureCreated(); }
    uint64_t DLSSEvaluations() const { return m_dlss.EvaluationCount(); }
    NVSDK_NGX_Result DLSSLastResult() const { return m_dlss.LastResult(); }
    // Signals and drains the queue. The player's seek path keeps the short
    // teardown budget; the offline renderer passes the render budget.
    d3d12_renderer_detail::FenceWaitResult WaitGPU(
        DWORD budgetMilliseconds = d3d12_renderer_detail::TeardownFenceWaitMilliseconds);
    bool PresentCurrent();
    void SetColorSettings(const ColorSettings& settings) { m_colorSettings = settings; }
    const ColorSettings& GetColorSettings() const { return m_colorSettings; }
    void SetComparison(const ComparisonSettings& settings) { m_comparison = settings; }
    const ComparisonSettings& GetComparison() const { return m_comparison; }
    // Source-size BGRA reference (the original member of the current pair). May be
    // called before RenderFrame or PresentCurrent; the copy rides on that submission.
    bool UploadReferenceFrame(const uint8_t* bgra, size_t bytes);

    d3d12_renderer_detail::FenceWaitResult LastFenceWaitResult() const { return m_lastFenceWaitResult; }
    bool GpuUnusable() const { return m_gpuUnusable; }
    // GPU time between the timestamp queries bracketing the last resolved DLSS
    // Evaluate; 0 until the first evaluated frame's fence has completed.
    double LastNeuralGpuMs() const { return m_lastNeuralGpuMs; }
    // Running maximum of the adapter's local-segment CurrentUsage sampled per frame.
    uint64_t PeakLocalVideoMemoryMiB() const { return m_peakLocalVideoMemoryMiB; }

private:
    friend struct D3D12RendererDeleter;
    ~D3D12Renderer();
    // An export source frame records two command lists (evaluate, then capture). Six
    // allocators keep three complete source frames in flight before CPU reuse waits.
    static constexpr uint32_t FrameCount = 6;
    // Root signature: [0] SRV table t0 (current view), [1] SRV table t1
    // (comparison reference), [2] PresentConstantCount 32-bit constants (Params).
    static constexpr uint32_t RootView = 0, RootReference = 1, RootConstants = 2;
    static constexpr uint32_t PresentConstantCount = 16;
    static constexpr uint32_t ReferenceSRV = 6;
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
    bool WaitForFrameSlot(uint32_t slot, uint64_t* stageWaitNanos = nullptr);
    bool SignalFrameSlot(uint32_t slot);
    bool WaitGPUForContinuedUse();
    bool WaitForFenceValue(uint64_t value, uint64_t* stageWaitNanos = nullptr);
    bool RenderFrameInternal(const uint8_t* bgra, size_t bytes,
                             const float* guideGridRGBA32F, size_t guideBytes,
                             uint32_t gridW, uint32_t gridH,
                             bool temporalReset, float frameTimeMs,
                             const FrameIdentity* identity);
    // Synchronous enqueue + resolve. Kept for the first-frame evidence loop, which must
    // read a capture back before it can decide whether to submit the same frame again.
    bool CaptureEvaluatedFrame(CapturedVideoFrame& capture);
    void RecordReferenceUpload(ID3D12GraphicsCommandList* cmd, uint32_t slot);
    void SetPresentConstants(ID3D12GraphicsCommandList* cmd, const ColorSettings& colors,
                             const ComparisonSettings& comparison, bool useReference);
    void HarvestNeuralTimings();
    void SampleLocalVideoMemory();
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
    Microsoft::WRL::ComPtr<IDXGIAdapter3> m_adapter3;
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
    uint64_t m_renderSlotWaitNanos = 0;
    uint64_t m_captureSubmitSlotWaitNanos = 0;
    uint64_t m_captureResolveWaitNanos = 0;
    uint64_t m_presentSlotWaitNanos = 0;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    uint32_t m_rtvInc=0,m_srvInc=0,m_dsvInc=0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_backbuffers[FrameCount];

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoConvert;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPresent;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoCacheCapture; // present shader into a BGRA8 target
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoMotionDebug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDepthDebug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDepthWrite;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoExpandGuides;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_decodedTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_upload[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dlssColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;      // R32_TYPELESS: D32 DSV + R32 SRV, same resource passed to NGX
    Microsoft::WRL::ComPtr<ID3D12Resource> m_motion;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dlssOutput;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_guideGrid;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_guideUpload[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cacheOutput;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cacheReadback[CaptureSlots];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_reference;   // source-size BGRA original member
    Microsoft::WRL::ComPtr<ID3D12Resource> m_referenceUpload[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestampHeap; // 2 timestamps per frame slot
    Microsoft::WRL::ComPtr<ID3D12Resource> m_timestampReadback;

    uint8_t* m_uploadMapped[FrameCount]{};
    uint8_t* m_guideMapped[FrameCount]{};
    uint8_t* m_referenceMapped[FrameCount]{};
    uint8_t* m_cacheReadbackMapped[CaptureSlots]{};
    uint64_t m_captureFence[CaptureSlots]{};
    uint32_t m_captureWrite = 0;
    uint32_t m_captureRead = 0;
    uint32_t m_capturePending = 0;
    uint64_t m_fenceWaitNanos = 0;
    uint64_t m_presentNanos = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_uploadFootprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_guideFootprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_cacheFootprint{};
    uint32_t m_numRows=0,m_guideRows=0;
    uint64_t m_rowSize=0,m_uploadBytes=0,m_guideRowSize=0,m_guideUploadBytes=0;
    uint32_t m_cacheRows=0;
    uint64_t m_cacheRowSize=0,m_cacheReadbackBytes=0;

    bool m_sourceInCopyDest = true;
    bool m_gridInCopyDest = true;
    bool m_referenceInCopyDest = true;
    bool m_referencePending = false;   // upload slot holds pixels not yet copied
    bool m_hasReference = false;       // a reference copy has been submitted
    uint32_t m_referenceUploadSlot = 0;
    bool m_neuralTimingPending[FrameCount]{};
    uint64_t m_timestampFrequency = 0;
    double m_lastNeuralGpuMs = 0.0;
    uint64_t m_peakLocalVideoMemoryMiB = 0;
    bool m_colorInRT = true;
    bool m_guidesInRT = true;
    bool m_depthInWrite = true;
    bool m_outputInUAV = true;
    bool m_dlssEnabled = true;
    bool m_allowTearing = false;
    bool m_recreateRequested = false;
    bool m_delayedRecreateDone = false;
    bool m_preserveSource = false;
    bool m_headless = false;
    uint64_t m_framesPresented = 0;
    DebugView m_debugView = DebugView::Final;
    ColorSettings m_colorSettings{};
    ComparisonSettings m_comparison{};
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
