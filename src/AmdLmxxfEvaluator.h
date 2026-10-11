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

    // The two strength terms LmxxfNrFrameInfo carries, which are the detail and
    // colour controls the user moves. 1.0 is the runtime's own default, so a job
    // that never touches them - and every job before they were wired up - puts
    // the same frame info on the wire as it always did.
    //
    // Neither is bring-up state, which is why neither is in the adapter's reuse
    // key even though both live on this struct: the runtime reads them per
    // frame, so a reused session takes a new value with nothing rebuilt. That is
    // also why the adapter re-applies them to an evaluator it reuses - a config
    // written for the second job would otherwise never reach the session the
    // first one armed (AmdLmxxfEvaluatorAdapter::Initialize).
    float transferStrength{1.0f};
    float colorStrength{1.0f};

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
    // The strength terms of the frames this evaluator will produce. Per-frame
    // inputs rather than bring-up state, so a caller that only moved a slider
    // calls this on the evaluator it already has instead of asking for a new
    // one: see AmdLmxxfEvaluatorConfig::transferStrength.
    void SetFrameStrength(float transfer, float color) noexcept
    {
        config_.transferStrength = transfer;
        config_.colorStrength = color;
    }
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
    bool UploadColor(const std::vector<uint8_t>& bgra);
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
