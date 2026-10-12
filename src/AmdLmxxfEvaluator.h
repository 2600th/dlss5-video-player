#pragma once

// Stage B of docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md.
//
// The AMD backend behind the renderer's existing INeuralFrameEvaluator seam, so
// the offline job's frame loop, retry accounting and capture plumbing are the
// ones production already runs - nothing here re-implements a render loop.
//
// What this owns that the NVIDIA evaluator does not:
//
//   * A D3D12 device and one DIRECT queue of its own (or a borrowed pair), which
//     is what LmxxfNrCreateInfo wants. Nothing crosses processes.
//   * One PERSISTENT colour texture, uploaded into every frame. The runtime's
//     own documentation is explicit that changing the colour pointer, size,
//     format or exposure binding rebuilds or rebinds the codec chain and may
//     wait on the GPU, so the texture is created once in Initialize and its
//     pointer never moves.
//   * The documented per-frame call order, one job at a time (the session's
//     state machine allows exactly one job in flight):
//
//       upload into the persistent colour texture (+ barrier to the declared
//       read state) -> [rewrite the guide textures from this frame's files] ->
//       PrepareFrame -> RecordInputs -> Close + Execute (producer) ->
//       EnqueueHip -> fence wait -> RecordOutputs + transition private_output ->
//       COPY_SOURCE + copy into the readback buffer -> Close + Execute
//       (consumer) -> fence wait -> Retire
//
//     The output reader is submitted BEFORE Retire, which is the contract the
//     ABI states for output reuse ("submit the output reader before Retire,
//     Drain, or Destroy"). The stage A probe copied after Retire; this does not.
//   * The produced frame comes from job.private_output, NOT from the colour
//     texture: the colour texture is the codec's INPUT. See
//     third_party/lmxxf/LmxxfNrApi.h.
//
// Not in this stage (they are stage C's job): the backend-neutral evidence
// chain, the cache identity, the runtime lock and the preflight probe. The
// renderer's NVIDIA evidence gates are therefore not meaningful here and a
// stage B job runs with requireNeural = false; see tests/AmdLmxxfEvaluatorSmoke.cpp
// for exactly what is and is not proven by a run through this evaluator.

#include "AmdLmxxfRuntime.h"
#include "AmdLmxxfSession.h"
#include "OfflineNeuralRenderer.h"
#include "TemporalStabilityPolicy.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Where the runtime pack and the device come from. Every default resolves to the
// layout docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md section 3.4 describes:
//
//   runtimeDirectory/            LmxxfNrRuntime.dll, shaders/ (the runtime's)
//     lmxxf-modules/             gfx1200/ + gfx1201/, 40 hsaco each
//     native-game-tiled-assets/  weights + shaders   (LMXXF_WEIGHTS_DIR)
struct AmdLmxxfEvaluatorConfig {
    // Holds LmxxfNrRuntime.dll. Required.
    std::filesystem::path runtimeDirectory;
    // The directory Create is handed as assets_directory. Empty means
    // runtimeDirectory/lmxxf-modules, which is the only layout the runtime
    // accepts: native-game-tiled-assets/HIP carries a stale 24-per-arch subset
    // and Create refuses it with "SHA256SUMS incomplete".
    std::filesystem::path modulesDirectory;
    // The weights and shaders the runtime reads through LMXXF_WEIGHTS_DIR.
    // Empty means runtimeDirectory/native-game-tiled-assets.
    std::filesystem::path weightsDirectory;
    // Borrowed device and queue, or null to create a pair on the AMD adapter.
    // A shared device is what stage C wants (the worker's renderer owns them);
    // the smoke test leaves them null.
    ID3D12Device* device{};
    ID3D12CommandQueue* queue{};
    // LmxxfNrCreateInfo::flags. The zero-output fallback is a recovery mode for
    // a live queue contract and the recording leases are a separate job model;
    // this carries whichever the caller wants, and temporalHistory ORs the
    // lease flag in on top of it.
    uint32_t createFlags{0};

    // The offline temporal experiment (docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md
    // section 5.3). The runtime only engages its temporal model under the
    // recording-lease lifecycle, so this changes the create flags AND the
    // per-frame call order - it is not a switch on the frame info alone.
    //
    // The guides it supplies are CONSTANT, because the source it is written for
    // is synthetic and its motion is analytically known: a still scene has zero
    // motion, and a pan has a fixed per-frame displacement. That is the point of
    // measuring here first - it asks what the temporal model does when it is
    // told the truth, with no estimator in the loop at all. A real video host
    // has to compute both guides, and section 5.3 says why that is the
    // expensive half.
    bool temporalHistory{false};
    // Motion in frame pixels per frame, (x, y), current frame <- previous. The
    // motion texture carries this one value everywhere. Zero is a still scene.
    float motionPixelsPerFrameX{};
    float motionPixelsPerFrameY{};
    // What the depth texture carries, 0 = near and 1 = far. The runtime reads
    // depth to judge what the reprojection may trust, and a synthetic source has
    // no geometry, so a single plane is the honest answer for it.
    float depthValue{1.0f};

    // Per-frame guides read from files instead of held constant, in the layout
    // src/GuideFiles.h defines: one directory per guide, one Portable Float Map
    // per source frame named by the frame's own number, motion as three channels
    // of frame-pixel displacement, depth as one channel in [0,1]. Empty means
    // the two constants above and nothing else, so the two sources compose - a
    // caller may drive motion from files and leave depth constant.
    //
    // This is the half a synthetic source cannot answer. With files the host is
    // fed the motion and depth a real clip actually has, so the measurement run 5
    // takes on a still scene can be taken on moving content; and it is the only
    // way to ask whether the stored vector is READ at all, which a constant
    // cannot distinguish from a texture the runtime never samples.
    //
    // The number in the filename is the source's own 0-based decoded index
    // (FrameIdentity::frameNumber), which is what tools/benchmark's `--guide-dump`
    // writes and what the renderer's estimator is indexed by - so a dump can be
    // fed straight back, frame for frame, without renumbering.
    //
    // The texture POINTER never moves and the texture SIZE never changes, which
    // is the persistent-input rule the colour texture follows too. Only the
    // contents change, which is unavoidable: a real host computes a new field
    // every frame. A missing or malformed file FAILS the frame rather than
    // falling back to the constant, because a run that quietly mixed the two
    // would be a measurement of neither.
    std::filesystem::path motionGuideDirectory;
    std::filesystem::path depthGuideDirectory;

    // How much of the motion-compensated history the frame keeps
    // (TemporalStabilityPolicy.h). This is the player's own pass, not the
    // runtime's: it runs after the runtime has published the frame and before the
    // readback takes it, so nothing about it is a runtime feature and
    // TemporalStability::Off is the exact code path this evaluator had before the
    // pass existed. It is also NOT the same switch as `temporalHistory` above -
    // that one asks the runtime to fuse its own model under the lease lifecycle
    // and needs guides, while this one needs nothing from the runtime at all.
    //
    // No motion estimator is behind it: the pass decides trust from the two
    // source frames themselves, and this backend hands it a zero motion field, so
    // it accumulates where the source held still and returns the frame untouched
    // everywhere else. That is the whole of "P1" in
    // docs/AMD-TEMPORAL-ACCUMULATION-zh.md, and it is why the default here can be
    // a rung at all on a backend with no flow at all.
    //
    // Off rather than a rung is the struct's own default so that an evaluator
    // built without saying anything renders the picture it always did; the
    // caller that wants the default accumulation the backend ships with asks for
    // it by name (kAmdDefaultStability in AmdLmxxfPolicy.h).
    TemporalStability stability{TemporalStability::Off};

    // Per-stage trace, for the smoke test and for a receipt. Empty disables it.
    std::function<void(std::string_view)> log;
};

class AmdLmxxfEvaluator final : public INeuralFrameEvaluator
{
public:
    explicit AmdLmxxfEvaluator(AmdLmxxfEvaluatorConfig config);
    ~AmdLmxxfEvaluator() override;
    AmdLmxxfEvaluator(const AmdLmxxfEvaluator&) = delete;
    AmdLmxxfEvaluator& operator=(const AmdLmxxfEvaluator&) = delete;

    bool Initialize(HWND renderWindow, uint32_t width, uint32_t height, uint32_t outputWidth,
                    uint32_t outputHeight, double fps, const GuideControls& guides) override;
    bool Submit(const OfflineDecodedFrame& frame, const FrameIdentity& id, bool capture,
                OfflineEvaluation& out) override;
    bool FeatureCreated() const override;
    uint64_t EvaluationCount() const override;
    uint64_t NeuralEvaluations() const override;
    void ResetTemporal() override;
    NeuralRenderFailure LastFailure() const override;
    double LastNeuralGpuMs() const override;
    uint64_t PeakLocalVideoMemoryMiB() const override;
    // The same measurement asked as a point question rather than a running
    // maximum, which is what an idle helper reports between jobs. Zero until a
    // frame has been submitted, and zero is a measurement there rather than a
    // gap: see LocalVideoMemoryNote.
    uint64_t CurrentLocalVideoMemoryMiB() const noexcept { return currentLocalVramMib_; }

    // Diagnostics. Empty until the corresponding step ran.
    const std::string& LastError() const noexcept { return lastError_; }
    const std::string& AdapterName() const noexcept { return adapterName_; }
    // The runtime's own GetStatus line after PrepareSession: arch, module count,
    // HIP readiness. The ABI 3 capabilities struct no longer carries hip_ready.
    const std::string& RuntimeStatus() const noexcept { return runtimeStatus_; }
    // The same GetStatus line read again, live. RuntimeStatus() is the bring-up
    // snapshot, taken before the first frame, so the counters that only move once
    // frames flow - arch, HIP readiness, bridge creates, codec recreates, passes -
    // are all still at their initial values there. This answers them.
    std::string LiveRuntimeStatus() const;
    // What the local video memory sampling actually saw, so a zero in
    // PeakLocalVideoMemoryMiB() can be read as a measurement rather than a gap.
    std::string LocalVideoMemoryNote() const;
    // What the last produced frame was read back from, so a caller can prove the
    // host colour texture was not the source. Null when nothing was produced.
    const void* LastOutputResource() const noexcept { return lastOutput_; }
    bool LastOutputWasPrivate() const noexcept { return lastOutputWasPrivate_; }
    // Wall time of the last Submit, which is what a receipt's per-frame pacing
    // would carry. The runtime's own GetTimings network_ms is LastNeuralGpuMs.
    double LastSubmitMs() const noexcept { return lastSubmitMs_; }
    // The history= token from the runtime's own GetStatus, refreshed after every
    // lease frame. "off" means the frame rendered spatially and says so nowhere
    // else. "priming" is the first frame of a run, which has no history to fuse
    // with yet. "active" is the temporal model actually fusing. Empty when no
    // lease frame has run, which is every frame of a non-temporal evaluator.
    const std::string& HistoryStatus() const noexcept { return lastHistoryStatus_; }
    // How many frames went through the lease path, so a run can tell "the
    // temporal model was engaged and reported off" apart from "the lease path
    // never ran" - two things whose logs look identical otherwise.
    uint64_t TemporalFrames() const noexcept { return temporalFrames_; }
    // How many frames had their guides read from files rather than from the two
    // constants. Non-zero only when a guide directory was configured, and the
    // number a run has to check to know it measured the file path: a config that
    // silently lost its directory would otherwise produce a healthy-looking run
    // of the constant path.
    uint64_t GuideFrames() const noexcept { return guideFrames_; }
    // How many frames the host-side temporal accumulation ran on. Zero when
    // config_.stability is Off, and the number a run has to check to know the
    // pass it asked for actually ran - the rung is in no result field, so a
    // frame that skipped it otherwise looks like one that did not.
    uint64_t AccumulationFrames() const noexcept { return accumulationFrames_; }

private:
    // True when the session exists and every object the frame path needs does.
    bool Ready() const noexcept;
    // The device, queue, command objects and the three textures. Idempotent.
    bool BringUpDevice(std::string_view why);
    bool CreateFrameResources();
    // Loads the module, negotiates the ABI, creates and prepares the session.
    bool BringUpRuntime();
    void TearDown();

    void Log(std::string_view text) const;
    void Report(std::string_view call, int32_t status);
    bool Fail(NeuralRenderFailure failure, std::string detail);
    bool Refuse(std::string detail);

    // One frame in the default call order. Answers false with lastFailure_ set.
    bool SubmitFrame(const std::vector<uint8_t>& bgra, bool resetRequested,
                     uint64_t sourceFrameNumber, std::vector<uint8_t>& out);
    // One frame in the lease order, which is the only one the temporal model
    // runs under. The two differ in where RecordOutputs and the first
    // submission sit; see the class comment in AmdLmxxfSession.h.
    bool SubmitFrameLease(const std::vector<uint8_t>& bgra, bool resetRequested,
                          uint64_t sourceFrameNumber, std::vector<uint8_t>& out);
    // `sourceSlot` is which of the two source textures the temporal
    // accumulation's trust test should find this frame in, or -1 when it is not
    // running and the copy is skipped. It is the same slot the frame's output
    // goes into, which is what makes the pair the test compares the pair of
    // consecutive frames it means to compare.
    bool UploadColor(const std::vector<uint8_t>& bgra, int sourceSlot);
    // The motion and depth textures, plus the upload buffer that fills them. The
    // pointers and the sizes are fixed for the life of the session - the runtime
    // rebuilds its chain around a pointer that moves - and only the contents
    // ever change. This fills the two constants; UploadGuides replaces them per
    // frame when a guide directory was configured.
    bool CreateGuideTextures();
    // Rewrites the guide textures from the files for `sourceFrameNumber` and
    // records the copy into `list`. A no-op that answers true when neither
    // directory is set. Fails the frame on a missing or malformed file.
    bool UploadGuides(ID3D12GraphicsCommandList* list, uint64_t sourceFrameNumber);
    bool CopyProducedToReadback(ID3D12GraphicsCommandList* list);
    // ---- temporal accumulation (TemporalStabilityPolicy.h) ----------------
    //
    // Everything below exists only when config_.stability is not Off, and every
    // one of it is required for a frame to run at a rung: the pass has no
    // fall-back to the plain copy, because a rung that silently did not run
    // would publish a picture the job did not ask for and a receipt that says it
    // did.
    //
    // Three steps and a copy, in this order and for this reason:
    //
    //   decode     the runtime's 8-bit sRGB frame -> linear FP16, which is the
    //              input contract kTemporalStabilityHlsl's t0 states
    //   accumulate that pass itself, whose output is the history slot for this
    //              frame in linear FP16
    //   encode     the history slot -> 8-bit sRGB, which is what the readback
    //              buffer and every consumer of it carries
    bool CreateTemporalResources();
    bool CreateTemporalPipeline();
    // Records the three passes and the copy on `list`. `plan` is the frame's
    // step from temporal_stability::PlanFrame and `frame` the identity it was
    // taken for; both come from the caller because the upload has to put this
    // frame's source where the plan says the pass will read it, which happens
    // before either pass is recorded.
    bool RecordTemporalAccumulation(ID3D12GraphicsCommandList* list,
                                    const temporal_stability::Plan& plan,
                                    const FrameIdentity& frame);
    // The copy both the plain and the accumulated path end with: `from` into the
    // readback buffer at the frame's own footprint, in `format`.
    void CopyTextureToReadback(ID3D12GraphicsCommandList* list, ID3D12Resource* from,
                               DXGI_FORMAT format);
    // Maps the readback buffer and copies the frame out of it. Shared by both
    // call orders, which differ only in which list the copy was recorded into.
    bool ReadBackProduced(std::vector<uint8_t>& out);
    bool WaitForGpu();
    // Signals the frame fence and reports the value and the HRESULT separately.
    // EndRecording wants both as facts about the submission rather than as a
    // wait, and refuses a zero value or a failed status.
    bool SignalFence(uint64_t& value, int32_t& status);
    void RefreshHistoryStatus();
    void SampleLocalVram();

    AmdLmxxfEvaluatorConfig config_;
    amd_lmxxf::Runtime runtime_;
    std::optional<amd_lmxxf::Session> session_;

    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    // The lease order puts the producer and the consumer on the queue as two
    // separate submissions with the runtime's HIP work between them, so the
    // consumer needs a list and an allocator of its own - the first pair is
    // still executing when the second is recorded.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> consumerAllocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> consumerList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_{};
    uint64_t fenceValue_{};

    // Created once. `color_` is the pointer the runtime is told about every
    // frame, which is the whole point of the persistent-input rule.
    Microsoft::WRL::ComPtr<ID3D12Resource> color_;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    // The temporal guides, filled once in CreateGuideTextures from the two
    // constants and rewritten every frame by UploadGuides when a guide directory
    // was configured. Their absence is what makes history=off in the first
    // place, so they are only created when the caller asked for temporalHistory.
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_;
    Microsoft::WRL::ComPtr<ID3D12Resource> guideUpload_;
    // Both guide footprints and the mapped base of the one upload buffer that
    // carries them, kept rather than recomputed because the per-frame rewrite
    // needs the same three values the initial fill used. The buffer is reused
    // rather than doubled per frame for the same reason as the colour one:
    // SubmitFrameLease waits the queue out before it returns, so the frame that
    // is about to overwrite it is the one that just finished reading it.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT motionFootprint_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFootprint_{};
    uint8_t* guideUploadMapped_{};
    uint8_t* uploadMapped_{};

    // ---- the temporal accumulation's own objects (config_.stability) -------
    //
    // Two root signatures rather than one: the accumulation's table holds five
    // textures and eight constants, the conversions' holds one texture and none,
    // and a signature that has to describe both would be a signature neither of
    // them is.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> convertRootSig_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> accumulationRootSig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoDecode_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoAccumulate_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoEncode_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> accumulationSrvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> accumulationRtvHeap_;
    UINT accumulationSrvDescriptorSize_{};
    // The source frames the trust test compares, one per history slot. The text
    // is 8-bit sRGB, the same as the runtime's own frame and the same as what the
    // pass's t3/t4 expect to linearise themselves.
    Microsoft::WRL::ComPtr<ID3D12Resource> sourceSlot_[2];
    // The stabilized output per slot, in FP16 because it is what a recursive
    // filter is accumulated in: an 8-bit slot would quantise once per blend and
    // the rungs would stop separating from each other.
    Microsoft::WRL::ComPtr<ID3D12Resource> historySlot_[2];
    // The runtime's frame in the linear light the pass's t0 contract asks for,
    // and the result encoded back to the codes the readback carries.
    Microsoft::WRL::ComPtr<ID3D12Resource> neuralLinear_;
    Microsoft::WRL::ComPtr<ID3D12Resource> stabilised_;
    // One texel of zero motion. The pass needs a motion texture bound and reads
    // it as a vector; a zero field is what makes it a filter over the source's
    // own agreement rather than a motion-compensated one, which is the whole of
    // what this backend can honestly supply without an estimator.
    Microsoft::WRL::ComPtr<ID3D12Resource> zeroMotion_;
    temporal_stability::History accumulationHistory_{};
    uint64_t accumulationFrames_{};

    // The state the caller declared for color_ at the last RecordInputs and the
    // state the next transition must start from. The runtime reads the colour
    // resource in the declared state and does not transition it (it maps that
    // state onto its own codec textures), so the caller is the only writer of
    // this value.
    D3D12_RESOURCE_STATES colorState_{D3D12_RESOURCE_STATE_COPY_DEST};
    bool colorUploaded_{};

    uint32_t width_{};
    uint32_t height_{};
    uint64_t frameIndex_{};

    std::string adapterName_;
    std::string runtimeStatus_;
    std::string lastError_;
    NeuralRenderFailure lastFailure_{NeuralRenderFailure::None};
    double lastNeuralGpuMs_{};
    double lastSubmitMs_{};
    uint64_t evaluations_{};
    uint64_t neuralEvaluations_{};
    uint64_t peakLocalVramMib_{};
    // The local segment budget beside the usage, and why the usage could not be
    // read when it could not. A stage C preflight has to tell "this process is
    // using nothing" apart from "this driver will not say", and both look like a
    // bare zero in PeakLocalVideoMemoryMiB().
    uint64_t localVramBudgetMib_{};
    uint64_t currentLocalVramMib_{};
    std::string localVramNote_;
    const void* lastOutput_{};
    bool lastOutputWasPrivate_{};
    bool prepared_{};
    // Mirrors LMXXF_NR_CREATE_FLAG_RECORDING_LEASES being set on the session,
    // which is what selects the frame path.
    bool leases_{};
    // Frames that went through the lease path, and the history= token the
    // runtime answered with the last time it was asked. Kept as state because
    // the token is only ever printed into a status string.
    uint64_t temporalFrames_{};
    std::string lastHistoryStatus_;
    // Frames whose guides came from files. See GuideFrames().
    uint64_t guideFrames_{};
};
