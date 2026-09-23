#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "D3D12FenceWait.h"
#include "DLSSBackend.h"
#include "PixelLayout.h"
#include "MediaSource.h"
#include "FrameIdentity.h"
#include "NgxSession.h"
#include "OpticalFlowNvof.h"
#include "PresentScalePolicy.h"
#include "TemporalStabilityPolicy.h"

#include <functional>

struct D3D12RendererTestAccess;

namespace d3d12_renderer_detail {

// Swapchain creation flags. A function so the one rule that matters can be
// asserted without a device: DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
// must never appear here.
//
// It is an attractive flag - it is what makes SetMaximumFrameLatency work, and
// it yields an object a message loop can block on instead of spinning. Setting
// it also stops neural rendering dead: the RenoDX add-on hooks this swapchain,
// and with the flag set its inline NR path allocates a fresh workset per
// evaluation, exhausts its pool within three frames, and logs "NR workset pool
// exhausted; preserving game output for this evaluation" while every later
// frame passes through untouched. The helper reports frames=0/0 and the player
// refuses the render with "A frame was not produced by feature 18".
//
// Found by bisect after the whole test suite stayed green through it. The
// player has no need of the flag: its message loop waits on a
// high-resolution timer (PrecisionSleeper), which measured lower CPU than the
// swapchain object did anyway.
inline constexpr UINT SwapchainFlags(bool allowTearing)
{
    return allowTearing ? UINT(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u;
}

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

// Which member of an original/neural pair the presentation shader shows. The values
// are persisted ([Comparison] Mode) and are the shader's mode numbers, so new ones
// only ever go on the end.
enum class ComparisonMode { Neural, Original, Blend, SplitVertical, Wipe, Difference, SideBySide, Quad };

struct ComparisonSettings {
    ComparisonMode mode = ComparisonMode::Neural;
    float amount = 0.5f;       // Blend: lerp(original, neural, amount)
    float splitX = 0.5f;       // SplitVertical/Wipe divider, in image UV [0,1]
    // Presentation-only neural strength dial: the composite of the neural frame against
    // the original, which costs a present instead of a re-render. The player calls it
    // the Mix and it is the one control for it: Blend at `amount` and a strength of
    // `amount` were the same lerp(original, neural, x) in the shader.
    float strength = 1.0f;     // 0..2, 1 shows the neural frame untouched
    float ratioGuard = 2.0f;   // >= 1: two-sided bound on the luminance ratio above 1
    float zoomScale = 1.0f;    // >= 1 magnifies around the zoom center
    float zoomCenterX = 0.5f;
    float zoomCenterY = 0.5f;
    // The original on the right of the split and wipe instead of the left.
    bool swap = false;
    // "Original" / "DLSS 5" tags drawn on the picture wherever both members share it.
    // Drawn from the atlas SetLabelAtlas uploads; without one nothing is drawn.
    bool labels = true;
    // The synced loupe: two circles, the original and DLSS 5 at the same image point,
    // point-sampled so the texels show. Centres and radius in backbuffer pixels, the
    // point in image UV, magnification in screen pixels per output texel.
    bool loupe = false;
    float loupeU = 0.5f, loupeV = 0.5f;
    float loupeLeftX = 0.0f, loupeLeftY = 0.0f, loupeRightX = 0.0f, loupeRightY = 0.0f;
    float loupeRadius = 0.0f;
    float loupeMagnification = 4.0f;
    // Difference: |DLSS 5 at the Mix - original| in linear light, times the gain, as
    // grey luma or per channel. Drawn without the image adjustments, which would
    // move a difference that is not in the render.
    float differenceGain = 4.0f;
    bool differenceLuma = true;
    // The spatial mask on the Mix (SetMask): white shows DLSS 5 at the Mix, black the
    // original, in every view of the neural member. Drawn only once a mask is uploaded.
    bool mask = false;
    bool maskInvert = false;
    // Quad's fourth pane: DLSS 5 at a second Mix, beside the first.
    float secondMix = 0.5f;
};

// Whether a comparison needs the window compositor (PSPresentScaled) even when the
// window is exactly the output's size, where the present would otherwise be PSPresent
// at 1:1. PSPresent is the cache capture's program and is never changed, so anything
// it cannot draw - the labels, the swap - lives in the compositor alone.
// Whether the present reads the original at all. The pure neural view at a Mix of 100%
// does not, and the player then skips uploading one (a source-size copy per frame).
inline bool ComparisonReadsReference(const ComparisonSettings& comparison)
{
    return comparison.mode != ComparisonMode::Neural || comparison.strength != 1.0f || comparison.loupe ||
           comparison.mask;
}

// Whether the present computes with the original's pixel values rather than only
// placing it beside or instead of the neural frame: the Mix off 100%, the mask,
// Difference, and 2x2 (whose panes include both). Those need the original as the
// model saw it - tone mapped, for an HDR source - so an HDR display compares them
// at SDR (HdrPolicy.h) instead of mixing PQ light with SDR light.
inline bool ComparisonCombinesPixels(const ComparisonSettings& comparison)
{
    return comparison.strength != 1.0f || comparison.mask || comparison.mode == ComparisonMode::Blend ||
           comparison.mode == ComparisonMode::Difference || comparison.mode == ComparisonMode::Quad;
}

inline bool ComparisonNeedsCompositor(const ComparisonSettings& comparison)
{
    return (comparison.mode != ComparisonMode::Neural && comparison.mode != ComparisonMode::Blend) ||
           comparison.loupe || comparison.mask;
}

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

    // `captureOutput` reserves the readback ring the export path drains. The
    // player never captures - only OfflineNeuralRenderer does - and the ring
    // is four full output frames of host-visible committed memory: 59 MB at
    // 1440p, 133 MB at 4K, allocated and never touched. Off by default, so a
    // caller that wants it has to say so.
    bool Initialize(HWND hwnd, uint32_t sourceW, uint32_t sourceH,
                    uint32_t outputW, uint32_t outputH,
                    uint32_t gridW, uint32_t gridH,
                    NVSDK_NGX_PerfQuality_Value quality, bool preserveSource = false,
                    bool captureOutput = false);
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
        // The frame this slot's capture was recorded for, stamped when the copy was
        // queued. It is the only thing that says which frame the bytes are: the job
        // compares it with the frame it queued in that position.
        FrameIdentity id{};
        // What the copy waits on before it may read `base`. A view from
        // BeginResolveOldestCapture has already been waited for; one from
        // ReserveOldestCapture is waited for by WaitAndCopyCaptureView, on whatever
        // thread runs the copy, which records the outcome in the last two fields for
        // CompleteReservedCapture to account for on the renderer's thread.
        ID3D12Fence* fence = nullptr;
        uint64_t fenceValue = 0;
        d3d12_renderer_detail::FenceWaitResult waitResult = d3d12_renderer_detail::FenceWaitResult::Completed;
        uint64_t waitNanos = 0;
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
    // BeginResolveOldestCapture without the fence wait, so the wait can leave the
    // renderer's thread along with the copy. The slot is reserved exactly as Begin
    // reserves it, and retired by Reserve itself when it fails. WaitAndCopyCaptureView
    // then waits and copies on any thread, touching no renderer state, and
    // CompleteReservedCapture - called on the renderer's thread for every successful
    // reservation, whatever the wait did - accounts for the wait, latches a device the
    // wait found lost, and retires the slot.
    bool ReserveOldestCapture(CaptureReadbackView& view);
    static bool WaitAndCopyCaptureView(CaptureReadbackView& view, std::vector<uint8_t>& pixels);
    bool CompleteReservedCapture(const CaptureReadbackView& view);
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
    // The temporal stability rung the cache capture applies (TemporalStabilityPolicy.h).
    // Off, the default, is the capture as it always was: the pass is not recorded and
    // the capture reads the neural output directly. Settable between jobs; the
    // history it keeps is judged per frame, so a new job starts it over by itself.
    void SetTemporalStability(TemporalStability level) { m_temporalStability = level; }
    TemporalStability ActiveTemporalStability() const { return m_temporalStability; }

    // Tearing is opt-in and belongs only to a renderer nobody watches. The offline
    // carrier presents into a hidden window purely so the neural add-on sees a present
    // per frame, and capping that at the display refresh would throttle an export that
    // already runs below real time. A visible playback window must never request it:
    // syncInterval 0 without the tearing flag still returns immediately, it just scans
    // out whole frames. Selected before Initialize, which creates the swapchain.
    void SetPresentTearing(bool allow) { m_requestedTearing = allow; }

    // The visible player's renderer presents at its window's size: before each frame
    // and each re-present, a client area that no longer matches the backbuffers
    // drains the queue and resizes them, and the present pass scales the output
    // into them itself (PSPresentScaled: bilinear up, area-averaged down). Without
    // it the backbuffers stayed at the output's size and DWM stretched them with a
    // bilinear filter. The offline carrier never sets it, so its hidden window,
    // its presents and its captures are exactly as they were. Selected before
    // Initialize, which compiles the scaled present only when it is asked for.
    void SetPresentFollowsWindow(bool follow) { m_followWindow = follow; }

    // HDR output (P3.1). A renderer that is allowed it compiles the HDR compositor
    // at Initialize and can then switch its swapchain between 8-bit sRGB and
    // R10G10B10A2 in the SMPTE ST 2084 / BT.2020 colour space. Only the visible
    // player asks: it requires SetPresentFollowsWindow, the offline carrier never
    // presents anything anyone sees, and the cache capture (PSPresent into the
    // BGRA8 cache target) is untouched either way - HDR is the backbuffer alone.
    // Selected before Initialize.
    void SetHdrOutputAllowed(bool allow) { m_hdrAllowed = allow; }
    // What the output under the window can show, asked of DXGI on each call: the
    // window may have moved to another monitor, and the user may have switched
    // Windows HDR on or off. sdrWhiteNits is the Windows "SDR content brightness"
    // (DISPLAYCONFIG_SDR_WHITE_LEVEL), or BT.2408's 203 nits where it cannot be read.
    struct DisplayHdrState {
        bool known = false;         // an output was found under the window
        bool hdr = false;           // it is in HDR mode (ST 2084 / BT.2020 desktop)
        float sdrWhiteNits = 203.0f;
        float maxLuminanceNits = 0.0f;
        std::wstring device;        // \\.\DISPLAYn, for the log
    };
    DisplayHdrState QueryDisplayHdr() const;
    // Switches the swapchain. Never touches the Windows HDR setting and never calls
    // SetHDRMetaData. Enabling fails - and leaves the swapchain in SDR - when HDR
    // output was not allowed, or the output refuses the ST 2084 colour space; the
    // return value is whether the swapchain is now what was asked for. The SDR white
    // level applies to everything SDR on an HDR swapchain: the neural frame, SDR
    // sources and the overlays.
    bool SetHdrOutput(bool enable, float sdrWhiteNits);
    bool HdrOutputActive() const { return m_hdrOutput; }
    // Distinct for every renderer this process builds, so a caller that applied a
    // setting to one can tell a replacement from it even at the same address.
    uint64_t Instance() const { return m_instance; }
    float HdrSdrWhiteNits() const { return m_sdrWhiteNits; }
    // The bytes the NEXT RenderFrame receives are ffmpeg's x2bgr10le: R10G10B10A2
    // holding PQ BT.2020 (VideoDecoder::SetHdrPresentation). Consumed by that one
    // frame, so a caller that forgets to say so gets the SDR reading, never a stale
    // HDR one. Ignored for an NV12 source.
    void SetNextSourcePq(bool pq) { m_nextSourcePq = pq; }
    uint32_t BackbufferW() const { return m_backbufferW; }
    uint32_t BackbufferH() const { return m_backbufferH; }

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
    // Fence waits a copy worker ran for reserved captures (CompleteReservedCapture). Not
    // part of FenceWaitNanos: the renderer's thread was working while they ran, and only
    // the Join that outlasted them shows up in the caller's own resolve stage.
    uint64_t CaptureWorkerWaitNanos() const { return m_captureWorkerWaitNanos; }
    // Zeroed at the start of each export attempt so a libx264 retry after an NVENC
    // failure is measured on its own, not on the sum of both passes.
    void ResetStageCounters() {
        m_fenceWaitNanos = 0;
        m_renderSlotWaitNanos = 0;
        m_captureSubmitSlotWaitNanos = 0;
        m_captureResolveWaitNanos = 0;
        m_presentSlotWaitNanos = 0;
        m_presentNanos = 0;
        m_captureWorkerWaitNanos = 0;
    }

    void SetDLSS(bool enabled) { m_dlssEnabled = enabled; }
    bool DLSSAvailable() const { return m_dlss.Available(); }
    // True when DLSS refused every output this source could reach, as opposed to
    // being unavailable for a runtime or device reason.
    bool DLSSSourceOutsideRange() const { return m_dlss.SourceOutsideSupportedRange(); }
    bool DLSSEnabled() const { return m_dlssEnabled && m_dlss.Available(); }
    // Whether anything this frame will read the motion and depth textures: the
    // NGX evaluate, or a debug view that draws them. False is the shipped
    // default - SetDLSS(false) runs on every media load - and on that path the
    // guide estimator, upload and two full-resolution passes have no consumer.
    bool GuidesRequired() const { return DLSSEnabled() || m_debugView != DebugView::Final; }
    bool LastFrameUsedDLSS() const { return m_lastDLSSUsed; }
    uint32_t DLSSInputW() const { return m_renderW; }
    uint32_t DLSSInputH() const { return m_renderH; }
    uint32_t OutputW() const { return m_outputW; }
    uint32_t OutputH() const { return m_outputH; }
    void SetDebugView(DebugView v) { m_debugView = v; m_presentStale = true; }
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
    void SetColorSettings(const ColorSettings& settings) { m_colorSettings = settings; m_presentStale = true; }
    void SetComparison(const ComparisonSettings& settings) { m_comparison = settings; m_presentStale = true; }
    const ComparisonSettings& GetComparison() const { return m_comparison; }
    // Compiles one entry point of the presentation program the way CreatePipelines
    // does, without a device, so a test can check the text on any machine.
    // hdrOutput: the HDR backbuffer build (HDR_OUTPUT defined).
    static bool CompilePresentProgram(const char* entry, const char* target,
                                      Microsoft::WRL::ComPtr<ID3DBlob>& blob, bool hdrOutput = false);
    // Source-size BGRA reference (the original member of the current pair). May be
    // called before RenderFrame or PresentCurrent; the copy rides on that submission.
    // pq: the bytes are x2bgr10le PQ BT.2020 rather than 8-bit sRGB BGRA (see
    // SetNextSourcePq); the reference texture takes the matching format.
    bool UploadReferenceFrame(const uint8_t* bgra, size_t bytes, bool pq = false);
    // A reference has been uploaded, or is queued behind the next submission.
    bool HasReference() const { return m_hasReference || m_referencePending; }
    // The tags the compositor draws on the picture: premultiplied BGRA, one row of
    // `rowHeight` pixels per tag, in the order Original, DLSS 5, Difference, DLSS 5 at
    // the second Mix,
    // each `rowWidths[i]` pixels wide from the left edge. Drawn by the caller at the
    // window's DPI; uploaded synchronously, so it drains the queue - call it when the
    // text or the DPI changes, not per frame. False leaves the previous atlas in use.
    bool SetLabelAtlas(const uint8_t* premultipliedBgra, uint32_t width, uint32_t height,
                       uint32_t rowHeight, const std::array<uint32_t, 4>& rowWidths);
    bool HasLabelAtlas() const { return m_labelAtlas != nullptr; }
    // The spatial mask, 8-bit grey at any size (the compositor stretches it over the
    // frame). Synchronous like SetLabelAtlas; ClearMask drains too, because the view
    // it nulls may be bound by a frame in flight.
    bool SetMask(const uint8_t* gray, uint32_t width, uint32_t height);
    void ClearMask();
    bool HasMask() const { return m_mask != nullptr; }
    // The subtitle overlay: premultiplied BGRA at the backbuffer's size, drawn by the
    // window compositor over everything else and never by PSPresent, so it cannot reach
    // the cache capture. Copied into an upload buffer now and onto the GPU by the next
    // frame or present, like the comparison reference; only a new size drains the
    // queue. Null hides it. False when the renderer has no window compositor.
    bool SetSubtitleOverlay(const uint8_t* premultipliedBgra, uint32_t width, uint32_t height);
    bool SubtitleOverlayShown() const { return m_subtitleShown || m_subtitlePending; }
    // The picture on screen, drawn again into an offscreen target of the present's own
    // size with the present's own constants and program - so a saved comparison is what
    // the window shows, tags and loupe included - and read back as tightly packed RGBA.
    // Synchronous: it drains the queue before and after. Call PresentCurrent first so
    // a reference uploaded since the last present is in it.
    bool CaptureComposedView(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
    // Something the present pass reads - colours, comparison, debug view, the
    // reference - changed since the last present was attempted. A paused player
    // presents only then, instead of re-presenting the same image at 60 Hz.
    bool PresentationStale() const { return m_presentStale; }

    d3d12_renderer_detail::FenceWaitResult LastFenceWaitResult() const { return m_lastFenceWaitResult; }
    bool GpuUnusable() const { return m_gpuUnusable; }
    // How many renderers this process has retained rather than destroyed,
    // because their bounded GPU drain did not complete. A retained renderer
    // still owns its swapchain, so the window underneath it must outlive it -
    // which is the difference between recovery working and DXGI refusing the
    // next CreateSwapChainForHwnd. Compare across a reset to learn whether
    // that particular renderer actually died; see RecoverUnusableRenderer.
    static uint32_t RetainedRendererCount() noexcept { return s_retainedRenderers.load(); }
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
    // Comparison-reference uploads, shared by frame slots ReferenceUploads apart. The
    // reference changes at most once per presented frame, and DXGI's frame latency of
    // three bounds the frames that can still be reading one.
    static constexpr uint32_t ReferenceUploads = 3;
    static_assert(FrameCount % ReferenceUploads == 0);
    // Root signature: [0] SRV table t0 (current view), [1] SRV table t1 (comparison
    // reference) and t2 (backward flow, read by the flow resolve alone), [2]
    // PresentConstantCount 32-bit constants (Params), [3] SRV table t3..t5 (the
    // compositor's mask, label atlas and subtitles), [4] ComposeConstantCount constants
    // (Compose).
    // The last two are read by PSPresentScaled alone, and appended so that the first
    // three keep the indices every other pass binds.
    static constexpr uint32_t RootView = 0, RootReference = 1, RootConstants = 2;
    static constexpr uint32_t RootOverlay = 3, RootCompose = 4;
    // 16 present parameters plus the capture pass's source texel size.
    static constexpr uint32_t PresentConstantCount = 20;
    // Pane, Label, LabelW, Target, Loupe, LoupeAt, Diff, Subs, Hdr; see the Compose
    // cbuffer in D3D12Renderer.cpp.
    static constexpr uint32_t ComposeConstantCount = 36;
    // Where Hdr sits in Compose, for the PQ source conversion that sets it alone.
    static constexpr uint32_t ComposeHdrOffset = 32;
    static constexpr uint32_t ReferenceSRV = 6;
    // NV12 source planes, bound at t0/t1 for the one conversion draw.
    static constexpr uint32_t SourceLumaSRV = 7, SourceChromaSRV = 8;
    // Hardware optical flow: the S10.5 flow grid, its per-cell cost and the reverse
    // field, bound at t0/t1/t2 for the one pass that turns them into full-resolution
    // motion. The cost and reverse slots hold null descriptors when the engine offered
    // neither, because the reference table spans both of them.
    static constexpr uint32_t NvofFlowSRV = 9, NvofCostSRV = 10, NvofBackFlowSRV = 11;
    // The compositor's overlay table: the spatial mask (t3), the label atlas (t4) and
    // the subtitle overlay (t5). Each holds a null view until something is uploaded.
    static constexpr uint32_t OverlaySRV = 12, LabelSRV = 13, SubtitleSRV = 14;
    // Temporal stability: two five-descriptor tables, one per history slot the pass can
    // read from (neural output, that slot's stabilized frame, motion, decoded source,
    // that slot's source), then one view of each slot's stabilized frame for the
    // capture to read. Written when the pass first runs; nothing reads them before.
    static constexpr uint32_t TemporalTableSRV = 15, TemporalTableSize = 5;
    static constexpr uint32_t TemporalOutputSRV = TemporalTableSRV + 2 * TemporalTableSize;
    // An HDR source frame (SetNextSourcePq), R10G10B10A2, for the one draw that
    // converts it into the linear colour texture.
    static constexpr uint32_t PqSourceSRV = TemporalOutputSRV + 2;
    static constexpr uint32_t SRVCount = PqSourceSRV + 1;
    // RTV heap: FrameCount backbuffers, then [+0] DLSS colour, [+1] motion, [+2] cache
    // output, [+3] capture luma, [+4] capture chroma, [+5] decoded texture (NV12 source),
    // [+6] the composed-view capture (CaptureComposedView), [+7] and [+8] the two
    // temporal stability slots.
    static constexpr uint32_t DecodedRTV = FrameCount + 5, ComposedRTV = FrameCount + 6;
    static constexpr uint32_t TemporalRTV = FrameCount + 7, RTVCount = FrameCount + 9;
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
    // The comparison reference and its uploads, on first use; see CreateVideoResources.
    // pq selects the texture's format, and a reference of the other kind is replaced.
    bool CreateReferenceResources(bool pq = false);
    // The HDR source texture, on the first PQ frame.
    bool CreatePqSourceResources();
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
    // Allocates the two history slots on first use, so a renderer that never runs
    // the pass - every player, and every job at Off - holds none of their memory.
    bool EnsureTemporalStabilityResources();
    // Records the pass for the frame the last RenderFrame produced and returns the
    // descriptor the capture should read in `captured`: SRV 1, the neural output,
    // when it is off. False when a rung was asked for and the pass cannot run.
    bool RecordTemporalStability(ID3D12GraphicsCommandList* cmd, uint32_t& captured);
    void RecordReferenceUpload(ID3D12GraphicsCommandList* cmd, uint32_t slot);
    bool CreateSubtitleResources(uint32_t width, uint32_t height);
    void RecordSubtitleUpload(ID3D12GraphicsCommandList* cmd, uint32_t slot);
    // targetWidth/targetHeight: the backbuffer the compositor draws into; the capture
    // passes pass neither, and PSPresent reads no Compose constant anyway.
    void SetPresentConstants(ID3D12GraphicsCommandList* cmd, const ColorSettings& colors,
                             const ComparisonSettings& comparison, bool useReference,
                             uint32_t targetWidth = 0, uint32_t targetHeight = 0);
    // Creates `texture`, fills it from tightly packed `pixels` and publishes its view at
    // `srvIndex`, synchronously: the queue is drained before the view is rewritten and
    // again after the copy, so the upload buffer can go. For the compositor's rare
    // uploads (the label atlas), never for per-frame data.
    bool UploadStaticTexture(Microsoft::WRL::ComPtr<ID3D12Resource>& texture, DXGI_FORMAT format,
                             const uint8_t* pixels, uint32_t width, uint32_t height,
                             uint32_t bytesPerPixel, uint32_t srvIndex, const wchar_t* name);
    // PresentCurrent's draw into `rtv`: viewport, constants, program, the view it reads.
    // hdrTarget: `rtv` is the HDR backbuffer, which takes the HDR compositor.
    void RecordViewDraw(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                        const present_scale::Target& target, bool hdrTarget);
    // The program and view the backbuffer pass binds for the current debug view,
    // shared by the frame path and PresentCurrent so the two cannot disagree about
    // which program an HDR backbuffer takes.
    ID3D12PipelineState* BackbufferProgram(bool scaled, bool hdrTarget) const;
    // What the backbuffer pass draws into: the window-sized backbuffer through the
    // scaled present, or - when the sizes agree, or the renderer does not follow its
    // window - the output's size through PSPresent, exactly as before.
    present_scale::Target CurrentPresentTarget() const;
    void FollowWindowSize();
    bool ResizeSwapchain(uint32_t width, uint32_t height);
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
    uint64_t m_captureWorkerWaitNanos = 0;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    uint32_t m_rtvInc=0,m_srvInc=0,m_dsvInc=0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_backbuffers[SwapchainBuffers];

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoConvert;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPresent;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPresentScaled; // only when m_followWindow
    // HDR output, only when m_hdrAllowed: PSPresentScaled and the two debug views
    // compiled with HDR_OUTPUT into R10G10B10A2, and the PQ source conversion.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPresentHdr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoMotionDebugHdr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDepthDebugHdr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoConvertPq;
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
    // The temporal stability pass has a root signature of its own: it reads five
    // textures, which the shared signature's two tables cannot bind.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_stabilityRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoTemporalStability;

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
    // Temporal stability history, two slots (TemporalStabilityPolicy.h History): the
    // stabilized output at output size and the decoded source it was made from.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_stableOutput[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_stableSource[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_reference;   // source-size BGRA original member
    Microsoft::WRL::ComPtr<ID3D12Resource> m_decodedPq;   // source-size R10G10B10A2 PQ frame
    Microsoft::WRL::ComPtr<ID3D12Resource> m_referenceUpload[ReferenceUploads];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_labelAtlas;  // premultiplied BGRA tags, see SetLabelAtlas
    Microsoft::WRL::ComPtr<ID3D12Resource> m_mask;        // R8 spatial mask, see SetMask
    Microsoft::WRL::ComPtr<ID3D12Resource> m_subtitle;    // premultiplied BGRA, see SetSubtitleOverlay
    Microsoft::WRL::ComPtr<ID3D12Resource> m_subtitleUpload[ReferenceUploads];
    uint8_t* m_subtitleMapped[ReferenceUploads]{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_subtitleFootprint{};
    uint32_t m_subtitleW = 0, m_subtitleH = 0, m_subtitleUploadSlot = 0;
    bool m_subtitlePending = false, m_subtitleInCopyDest = false, m_subtitleShown = false;
    uint32_t m_labelAtlasW = 0, m_labelAtlasH = 0, m_labelRowHeight = 0;
    std::array<uint32_t, 4> m_labelWidths{};
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestampHeap; // 2 timestamps per frame slot
    Microsoft::WRL::ComPtr<ID3D12Resource> m_timestampReadback;

    uint8_t* m_uploadMapped[FrameCount]{};
    uint8_t* m_guideMapped[FrameCount]{};
    uint8_t* m_referenceMapped[ReferenceUploads]{};
    uint8_t* m_cacheReadbackMapped[CaptureSlots]{};
    // Mapped for the renderer's lifetime like the capture readbacks beside it, and for
    // the same reason: the alternative is a Map/Unmap pair on every frame.
    const uint64_t* m_timestampMapped = nullptr;
    uint64_t m_captureFence[CaptureSlots]{};
    // Identity of the frame each readback slot holds, recorded at enqueue and handed
    // back when the slot resolves, so a ring that fell out of step is caught rather
    // than trusted.
    FrameIdentity m_captureId[CaptureSlots]{};
    // Identity of the frame the last RenderFrame recorded; empty after a failed or
    // unidentified frame, so a capture of it can never match a real one.
    FrameIdentity m_lastRenderedId{};
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
    bool m_presentStale = false;       // see PresentationStale
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
    bool m_followWindow = false;
    static inline std::atomic<uint64_t> s_instances{0};
    const uint64_t m_instance = ++s_instances;
    bool m_hdrAllowed = false;
    bool m_hdrOutput = false;       // the swapchain is R10G10B10A2 / ST 2084
    float m_sdrWhiteNits = 203.0f;
    bool m_nextSourcePq = false;    // consumed by the next RenderFrame
    bool m_framePq = false;         // the frame being recorded was a PQ one
    bool m_referencePq = false;     // m_reference is R10G10B10A2 PQ
    bool m_pqSourceInCopyDest = true;
    uint32_t m_backbufferW = 0, m_backbufferH = 0;
    bool m_allowTearing = false;
    bool m_recreateRequested = false;
    bool m_preserveSource = false;
    // True when a caller asked for the capture readback ring at Initialize.
    bool m_captureOutput=false;
    uint64_t m_framesPresented = 0;
    DebugView m_debugView = DebugView::Final;
    ColorSettings m_colorSettings{};
    ComparisonSettings m_comparison{};
    bool m_lastDLSSUsed = false;
    // Whether the last recorded frame reset NGX's history for any reason, the feature
    // recreate included, which the identity does not carry.
    bool m_lastFrameTemporalReset = false;
    TemporalStability m_temporalStability = TemporalStability::Off;
    temporal_stability::History m_stabilityHistory{};
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
