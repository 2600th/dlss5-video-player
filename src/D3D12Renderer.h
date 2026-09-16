#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include "D3D12FenceWait.h"
#include "DLSSBackend.h"
#include "PixelLayout.h"
#include "MediaSource.h"
#include "FrameIdentity.h"
#include "NgxSession.h"
#include "OpticalFlowNvof.h"

#include <functional>

struct D3D12RendererTestAccess;

namespace d3d12_renderer_detail {
// The NV12 source conversion's numbers as the tokens CreatePipelines pastes into
// the shader text, one set per conversion SourceNv12Conversion names. They are
// strings because a float cannot be turned back into a token, and tokens are what
// keeps the BT.709 limited-range arm compiling to the byte-identical program that
// every cached render on disk was made with.
//
// Luma is (Y*255 - lumaOffset) / lumaScale and chroma is (UV*255 - 128) /
// chromaScale; the four coefficients are the standard inverse matrix, R from V,
// G from U and V, B from U.
struct SourceNv12Constants {
    const char* lumaOffset;
    const char* lumaScale;
    const char* chromaScale;
    const char* redV;
    const char* greenU;
    const char* greenV;
    const char* blueU;
};

// Null for Unsupported, which is the whole gate: there is no nearest-variant
// branch to fall into.
const SourceNv12Constants* SourceNv12ConstantsFor(SourceNv12Conversion conversion);
} // namespace d3d12_renderer_detail

// A resource whose destruction a test wants to observe, handed to the renderer
// so the renderer's own teardown order releases it.
struct D3D12RendererTestOwnedResource {
    virtual ~D3D12RendererTestOwnedResource()=default;
};

// Substitutes for the GPU operations a test cannot perform: the fence wait and
// signal, the device-removed reason, the capture readback and the process exit a
// second retained renderer ends in - plus the one request a test makes of the real
// device, Device Removed Extended Data. A renderer holds one of these only when
// something installed it, and no production renderer does, so each site below
// falls through to the real call. They are grouped behind one pointer rather than
// living as six members so that the class is the same size in every translation
// unit - the five-member form made sizeof(D3D12Renderer) depend on a macro - and
// so the whole test surface is visible in one place.
struct D3D12RendererTestHooks {
    std::function<d3d12_renderer_detail::FenceWaitResult()> waitGPU;
    std::function<HRESULT(uint64_t)> frameSignal;
    std::function<HRESULT()> deviceRemovedReason;
    std::function<bool(std::vector<uint8_t>&)> cacheCapture;
    std::function<void()> exitProcess;
    std::unique_ptr<D3D12RendererTestOwnedResource> ownedResource;
    // Turn on DRED auto-breadcrumbs and page-fault reporting before the device is
    // created, so a removal is logged with what the GPU was doing: per-list breadcrumbs
    // after a fault, and after an explicit RemoveDevice the fact that DRED was on and
    // nothing was outstanding. Process-wide once set; costs the driver a breadcrumb
    // write per command list op.
    bool dred=false;
};

class D3D12Renderer;
// Drains the queue within the teardown budget before deleting. A renderer whose
// drain did not complete - and whose device is not gone - may still have command
// lists executing, and deleting it would release their resources and the NGX
// feature underneath them, which the DLSS guide S5.5 forbids. It is kept alive
// instead, with everything it holds. A process is allowed one: a second means the
// GPU stopped answering twice in this process, so every renderer built after it
// would queue behind the same silence, and the deleter ends the process with
// RetainedRendererExitCode. The helper's parent reads a non-zero exit as a crash
// and answers with its bounded relaunch, the same path a removed device takes.
struct D3D12RendererDeleter {
    void operator()(D3D12Renderer* renderer) const noexcept;
};
inline constexpr UINT RetainedRendererExitCode = ERROR_FATAL_APP_EXIT;
using D3D12RendererOwner=std::unique_ptr<D3D12Renderer,D3D12RendererDeleter>;
D3D12RendererOwner MakeD3D12Renderer();
struct GuideFrame;

// What the capture path reads back, and therefore what the encoder is fed.
//
//  * Bgra - the cache render target as it stands. ffmpeg converts every frame to
//    yuv420p on the CPU, which measured as the export's slowest stage.
//  * Nv12 - the same picture converted on the GPU: a full-resolution Y plane followed
//    by an interleaved half-resolution UV plane, BT.709 limited range. NVENC takes it
//    unchanged, and it is 1.5 bytes per pixel instead of 4 across PCIe and the pipe.
//    Requires even output dimensions.
enum class CaptureFormat { Bgra, Nv12 };

struct CapturedVideoFrame {
    // Tightly packed pixels in the renderer's active CaptureFormat, which the encoder
    // is started to match: 8-bit BGRA straight from the B8G8R8A8_UNORM cache render
    // target, or the NV12 Y plane followed by its interleaved UV plane. Neither costs a
    // CPU channel swizzle. Named for the payload, not a channel order, because the test
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
    // Presentation-only neural strength dial: the composite of the neural frame against
    // the original, which costs a present instead of a re-render.
    float strength = 1.0f;     // 0..2, 1 shows the neural frame untouched
    float ratioGuard = 2.0f;   // >= 1: two-sided bound on the luminance ratio above 1
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
                     bool temporalReset, bool motionGuides, float frameTimeMs);
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
        size_t rowPitch = 0;             // may exceed the tight row; rows are padded
        size_t bytes = 0;                // tightly packed size the copy produces
        uint32_t width = 0, height = 0;
        CaptureFormat format = CaptureFormat::Bgra;
        // NV12 only: the interleaved UV plane, half resolution in both axes, sitting in
        // the same readback buffer at its own aligned offset.
        const uint8_t* chromaBase = nullptr;
        size_t chromaRowPitch = 0;
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

    // Selects what the next Initialize builds its capture resources for. Ignored once
    // Initialize has run, and downgraded to Bgra when the output size is odd, so callers
    // must read ActiveCaptureFormat back rather than assume the request was honoured.
    void SetCaptureFormat(CaptureFormat format) { m_requestedCaptureFormat = format; }
    CaptureFormat ActiveCaptureFormat() const { return m_captureFormat; }

    // Layout of the bytes RenderFrame/RenderFrameForCache receive. Selected before
    // Initialize like the capture format, and likewise downgraded to Bgra when the source
    // size is odd, so callers read ActiveSourceLayout back. NV12 is converted on the GPU
    // into the same 8-bit sRGB BGRA texture the BGRA path uploads, so nothing after the
    // upload changes. UploadReferenceFrame (playback comparison) stays BGRA-only.
    void SetSourceLayout(PixelLayout layout) { m_requestedSourceLayout = layout; }
    PixelLayout ActiveSourceLayout() const { return m_sourceLayout; }
    size_t SourceFrameBytes() const { return PixelLayoutFrameBytes(m_sourceLayout, m_sourceW, m_sourceH); }

    // What the source declared about its colour, which is what the NV12 source
    // conversion is specialised for. Selected before Initialize like the layout
    // above: the conversion's coefficients and range mapping are compiled into
    // the shader as literals rather than read from a constant buffer, because
    // fxc folds the chroma scale into each coefficient and the luma scale into
    // each channel's multiply-add, and a constant-buffer value cannot be folded -
    // so a parameterised pass could not be bit-identical to the BT.709
    // limited-range program every cached render on disk was made with.
    //
    // A description SourceNv12ConversionFor refuses, paired with an NV12 layout,
    // fails Initialize rather than picking the nearest matrix. That pairing is
    // unreachable: VideoDecoder is the only producer of NV12 source frames and it
    // asks for that layout only for a description the same function accepted.
    void SetSourceColor(const SourceColorDescription& color) { m_sourceColor = color; }

    // Tearing is opt-in and belongs only to a renderer nobody watches. The offline
    // carrier presents into a hidden window purely so the neural add-on sees a present
    // per frame, and capping that at the display refresh would throttle an export that
    // already runs below real time. A visible playback window must never request it:
    // syncInterval 0 without the tearing flag still returns immediately, it just scans
    // out whole frames. Selected before Initialize, which creates the swapchain.
    void SetPresentTearing(bool allow) { m_requestedTearing = allow; }

    // Coarse accounting for the offline export, which otherwise cannot tell a slow GPU
    // apart from a swapchain that is pacing it. Both counters only ever move on the
    // thread that drives the renderer, so they need no synchronisation.
    //
    //  * FenceWaitNanos is time the CPU spent parked waiting for the GPU. A large share
    //    means the export is GPU bound and more CPU threads will not help.
    //  * PresentNanos is time inside IDXGISwapChain::Present, where DXGI blocks once the
    //    frame latency limit is reached. Every frame presents, hidden window or not: the
    //    RenoDX add-on runs its feature-18 pass per present, so an export that skipped
    //    them produced DLAA-only frames the evidence chain still counted as verified.
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

    void SetDLSS(bool enabled) { m_dlssEnabled = enabled; }
    bool DLSSAvailable() const { return m_dlss.Available(); }
    // True when DLSS refused every output this source could reach, as opposed to
    // being unavailable for a runtime or device reason.
    bool DLSSSourceOutsideRange() const { return m_dlss.SourceOutsideSupportedRange(); }
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
    // Zeroed at the start of each job. A renderer that outlives its job - the
    // resident helper keeps one device across several - would otherwise report
    // the highest usage any earlier job reached as this one's peak.
    void ResetPeakLocalVideoMemory() { m_peakLocalVideoMemoryMiB = 0; }
    // Point sample of the adapter's local-segment CurrentUsage, MiB, rather
    // than the running maximum above. An idle helper's question is "what is
    // parked right now", which has no maximum in it.
    uint64_t CurrentLocalVideoMemoryMiB() const;
    // Drains the queue and hands the NGX feature back, asking the runtime to
    // free its memory with it. Nothing else this renderer holds is touched;
    // the next RenderFrame re-creates the feature through the ordinary
    // EnsureFeature path. False when there was no feature, or the drain did
    // not complete - releasing a feature whose command lists have not retired
    // is what the DLSS guide S5.5 forbids, so the feature is kept instead.
    bool ReleaseDLSSFeatureForIdle();

private:
    friend struct D3D12RendererDeleter;
    ~D3D12Renderer();
    // An export source frame records two command lists (evaluate, then capture). Six
    // allocators keep three complete source frames in flight before CPU reuse waits.
    // Upload staging follows the allocator count because each in-flight frame owns its
    // upload until its fence completes (~+100 MB of upload heap at 4K over three).
    static constexpr uint32_t FrameCount = 6;
    // The swapchain does not: DXGI's default frame latency of 3 bounds queued presents,
    // so more backbuffers would only spend VRAM in the visible player.
    static constexpr uint32_t SwapchainBuffers = 3;
    static_assert(SwapchainBuffers <= FrameCount);
    // Root signature: [0] SRV table t0 (current view), [1] SRV table t1 (comparison
    // reference) and t2 (backward flow, read by the flow resolve alone), [2]
    // PresentConstantCount 32-bit constants (Params).
    static constexpr uint32_t RootView = 0, RootReference = 1, RootConstants = 2;
    // 16 present parameters plus the capture pass's source texel size.
    static constexpr uint32_t PresentConstantCount = 20;
    static constexpr uint32_t ReferenceSRV = 6;
    // NV12 source planes, bound at t0/t1 for the one conversion draw.
    static constexpr uint32_t SourceLumaSRV = 7, SourceChromaSRV = 8;
    // Hardware optical flow: the S10.5 flow grid, its per-cell cost and the reverse
    // field, bound at t0/t1/t2 for the one pass that turns them into full-resolution
    // motion. The cost and reverse slots hold null descriptors when the engine offered
    // neither, because the reference table spans both of them.
    static constexpr uint32_t NvofFlowSRV = 9, NvofCostSRV = 10, NvofBackFlowSRV = 11;
    static constexpr uint32_t SRVCount = 12;
    // RTV heap: FrameCount backbuffers, then [+0] DLSS colour, [+1] motion, [+2] cache
    // output, [+3] capture luma, [+4] capture chroma, [+5] decoded texture (NV12 source).
    static constexpr uint32_t DecodedRTV = FrameCount + 5, RTVCount = FrameCount + 6;
    // NVIDIA's D3D12 DLSS contract expects input resources in NON_PIXEL_SHADER_RESOURCE
    // at EvaluateFeature time. Debug/presentation passes temporarily transition selected
    // resources to PIXEL_SHADER_RESOURCE and restore them before the frame ends.
    static constexpr D3D12_RESOURCE_STATES GuideReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    static constexpr D3D12_RESOURCE_STATES DepthGuideReadState =
        D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    bool CreateDeviceAndSwapchain(HWND hwnd);
    bool CreateHeapsAndBackbuffers();
    bool CreatePipelines();
    // The one pass whose text is not the same for every renderer: the source
    // conversion is specialised for m_sourceColor. Static, and takes no device,
    // so a test can compile every arm without one.
    static bool CompileSourceNv12(SourceNv12Conversion conversion,
                                  Microsoft::WRL::ComPtr<ID3DBlob>& blob);
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
    // HR for a call on the live device inside a frame: logged the same way, then
    // asked whether the device is behind it. A removed device answers almost any
    // call with a loss code, and the first call to see it is as often an allocator
    // Reset or a Close as a Present, so a frame that fails there is a device loss
    // to latch, not a neural failure for the caller to retry.
    bool DeviceHR(HRESULT hr, const char* what);
    // The test hook when one is installed, otherwise the device's answer.
    HRESULT DeviceRemovedReason() const;
    // The one place the renderer records that its GPU cannot be used again, so the
    // device's reason is logged - with whatever DRED has to say, on every
    // configuration - once, on the transition. `reason` is what DeviceRemovedReason
    // answered the caller that classified `result`; the caller asks once and hands it on.
    void LatchGpuUnusable(d3d12_renderer_detail::FenceWaitResult result, HRESULT reason);
    // Presents the current backbuffer and accounts the time; a failure is logged
    // as `what` and classified like any other call on the device.
    bool PresentSwapchain(const char* what);
    bool RenderFrameInternal(const uint8_t* bgra, size_t bytes,
                             const float* guideGridRGBA32F, size_t guideBytes,
                             uint32_t gridW, uint32_t gridH,
                             bool temporalReset, bool motionGuides, float frameTimeMs,
                             const FrameIdentity* identity);
    // The rest of a frame once its upload list is on the queue: the guide and colour
    // passes, the NGX evaluate, the backbuffer pass and the Present. Split from
    // RenderFrameInternal at the first ExecuteCommandLists so that the caller has
    // exactly one exit after submission, which is where the slot gets published.
    bool RecordAndPresentFrame(uint32_t slot, ID3D12GraphicsCommandList* cmd, bool nvofFrame,
                               bool temporalReset, float frameTimeMs, const FrameIdentity* identity);
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
    // Uploads and the optical-flow capture are recorded separately from the frame, so
    // the queue can be signalled past them and the engine started without either a CPU
    // stall or a second reset of the allocator the rest of the frame is still using.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_uploadAllocators[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_uploadCmds[FrameCount];
    OpticalFlowNvof m_nvof;
    bool m_nvofActive = false;
    // Engine-input pixels -> DLSS input pixels for the flow resolve pass, from
    // PlanHardwareFlow. Both are 1 unless this is a Super Resolution session.
    float m_nvofMotionScaleX = 1.0f, m_nvofMotionScaleY = 1.0f;
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
    Microsoft::WRL::ComPtr<ID3D12Resource> m_backbuffers[SwapchainBuffers];

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoConvert;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPresent;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoCacheCapture; // present shader into a BGRA8 target
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoCaptureLuma;   // present shader into an R8 Y plane
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoCaptureChroma; // ...and a half-size R8G8 UV plane
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoSourceNv12;    // NV12 planes -> decoded BGRA8
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoMotionDebug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDepthDebug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDepthWrite;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoExpandGuides;
    // Turns the NVOFA S10.5 flow grid into the same R16G16_FLOAT motion texture
    // PSExpandGuides writes, so everything downstream is unchanged.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoNvofMotion;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_decodedTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_upload[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sourceLuma;    // NV12 source only, R8
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sourceChroma;  // NV12 source only, R8G8 half size
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dlssColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;      // R32_TYPELESS: D32 DSV + R32 SRV, same resource passed to NGX
    Microsoft::WRL::ComPtr<ID3D12Resource> m_motion;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dlssOutput;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_guideGrid;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_guideUpload[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cacheOutput;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_captureLuma;    // NV12 only
    Microsoft::WRL::ComPtr<ID3D12Resource> m_captureChroma;  // NV12 only
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cacheReadback[CaptureSlots];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_reference;   // source-size BGRA original member
    Microsoft::WRL::ComPtr<ID3D12Resource> m_referenceUpload[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestampHeap; // 2 timestamps per frame slot
    Microsoft::WRL::ComPtr<ID3D12Resource> m_timestampReadback;

    uint8_t* m_uploadMapped[FrameCount]{};
    uint8_t* m_guideMapped[FrameCount]{};
    uint8_t* m_referenceMapped[FrameCount]{};
    uint8_t* m_cacheReadbackMapped[CaptureSlots]{};
    // Mapped for the renderer's lifetime like the capture readbacks beside it, and for
    // the same reason: the alternative is a Map/Unmap pair on every frame.
    const uint64_t* m_timestampMapped = nullptr;
    uint64_t m_captureFence[CaptureSlots]{};
    uint32_t m_captureWrite = 0;
    uint32_t m_captureRead = 0;
    uint32_t m_capturePending = 0;
    uint64_t m_fenceWaitNanos = 0;
    uint64_t m_presentNanos = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_uploadFootprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_guideFootprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_cacheFootprint{};
    // Both planes share one readback buffer, chroma at an alignment-padded offset.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_lumaFootprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_chromaFootprint{};
    CaptureFormat m_requestedCaptureFormat = CaptureFormat::Bgra;
    CaptureFormat m_captureFormat = CaptureFormat::Bgra;
    // NV12 source: both planes in one upload buffer per slot, chroma at an aligned offset.
    // m_uploadFootprint keeps describing the BGRA layout, which the reference upload shares.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_sourceLumaFootprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_sourceChromaFootprint{};
    PixelLayout m_requestedSourceLayout = PixelLayout::Bgra;
    PixelLayout m_sourceLayout = PixelLayout::Bgra;
    SourceColorDescription m_sourceColor{};
    bool m_sourcePlanesInCopyDest = true;
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
    bool m_requestedTearing = false;
    bool m_allowTearing = false;
    bool m_recreateRequested = false;
    bool m_preserveSource = false;
    uint64_t m_framesPresented = 0;
    DebugView m_debugView = DebugView::Final;
    ColorSettings m_colorSettings{};
    ComparisonSettings m_comparison{};
    bool m_lastDLSSUsed = false;
    bool m_gpuUnusable = false;
    d3d12_renderer_detail::FenceWaitResult m_lastFenceWaitResult =
        d3d12_renderer_detail::FenceWaitResult::Completed;
    DLSSBackend m_dlss;
    // Renderers this process has kept alive after a drain that did not complete;
    // see D3D12RendererDeleter. Process-wide because the limit is on the process.
    static std::atomic<uint32_t> s_retainedRenderers;

    friend struct D3D12RendererTestAccess;
    // Null in every production renderer; see D3D12RendererTestHooks.
    std::unique_ptr<D3D12RendererTestHooks> m_testHooks;
};
