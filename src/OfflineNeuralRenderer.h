#pragma once

#include "FrameIdentity.h"
#include "GuideControls.h"
#include "MediaPipeline.h"
#include "NeuralRenderTypes.h"
#include "TemporalGuides.h"
#include "TemporalSettings.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

struct NeuralRenderRequest {
    HWND renderWindow{};
    std::filesystem::path sourcePath;
    std::filesystem::path stagingVideoPath;
    uint32_t width{};
    uint32_t height{};
    double fps{};
    double durationSeconds{};
    uint64_t jobId{};
    NeuralRenderRange range{};
    uint32_t prerollFrames{kDefaultPrerollFrames};
    GuideControls guides{};
    uint32_t frameRetryLimit{kDefaultFrameRetryLimit};
    // Frames per finalized output file. 0 keeps the single-file behaviour:
    // every captured frame goes to stagingVideoPath and no segment is emitted.
    uint32_t segmentFrames{0};
    // Frames in the FIRST finalized file, when it should be shorter than the
    // rest. A live session cannot show anything until segment 0 is muxed, so a
    // short first file is the difference between waiting two seconds for the
    // picture and half a second. 0 means "same as segmentFrames".
    uint32_t firstSegmentFrames{0};
    // Manual-reset event owned by the caller. Signalled means "pause"; the
    // worker checks it between frames and reports NeuralRenderPhase::Paused.
    HANDLE pauseEvent{};
    // Convert the captured frame to NV12 on the GPU instead of handing ffmpeg BGRA and
    // letting it convert every frame on the CPU. Ignored when the output size is odd,
    // and by the test evaluator, which always captures BGRA.
    bool gpuColorConversion{false};
    // hevc_nvenc preset p1..p7; 7 is slowest/highest quality. Only the NVENC
    // attempt reads it. Default p5 on the measurement recorded beside
    // EncoderSpec::nvencPreset in MediaPipeline.h.
    uint32_t nvencPreset{5};
    // Decode the source to NV12 and convert it to BGRA on the GPU (true) or let ffmpeg
    // convert on the CPU (false). Default false on a measured throughput trade, not
    // on a colour risk: the decoder probes the source's matrix and range and only
    // delivers NV12 for a description the GPU conversion has coefficients for, so a
    // BT.601, full-range, undeclared or BT.2020 source falls back to the CPU
    // conversion with one log line rather than reaching the model mis-decoded.
    bool gpuSourceConversion{false};
    // ---- Appended, deliberately, at the end. ----------------------------
    // Several callers build this struct with a positional initialiser list
    // (`{nullptr, source, output, w, h, fps, seconds}`). Adding a member in the
    // middle silently re-binds every initialiser after it onto the wrong
    // member - inserted above `fps`, these two turned a 24 fps 6 s smoke into a
    // 24x6 output size with no frame rate, and the compiler said only
    // "possible loss of data". New members go here.
    // What the captured frames come out at. 0 means "the source size", which is
    // every caller that does not upscale and is what this pass did exclusively
    // before DLSS Super Resolution became part of an export.
    //
    // The renderer has always taken a source size and an output size
    // separately; this pass passed the source size for both. Handing it a real
    // output size makes the carrier a true Super Resolution pass, and because
    // RenoDX's NRPreUpscale defaults to 0 - neural AFTER the upscale - the
    // neural model then runs on the upscaled frame. That is NVIDIA's own DLSS 5
    // order: their neural rendering "normally runs last, on the fully upscaled
    // frame", and the community Neural Upstream mod exists precisely because
    // moving it earlier is faster and therefore NOT what stock does. An export
    // has no frame budget to protect, so it takes the stock order.
    uint32_t outputWidth{};
    uint32_t outputHeight{};
    // False renders the carrier alone - Super Resolution with no neural pass.
    // The helper starts with the neural add-on disabled in its ReShade.ini
    // (a relaunch when that flips it, since the proxy reads the file at load),
    // and the job skips the feature-18 arming check, the receipt gate and the
    // four verdicts after capture. Those exist to stop un-denoised frames being
    // published as neural; this job makes the opposite claim and is held to
    // that instead: no feature-18 evaluation may appear in its session log.
    bool requireNeural{true};
    // The resolution the model runs at, as a percentage of the source: one of
    // kProcessingScaleRungs (UpscalingPolicy.h). Below 100 the job area-reduces
    // each frame to ProcessingSize, runs the carrier as true Super Resolution
    // back to the source size, and the add-on (NRPreUpscale=1, written by the
    // parent) puts the model on the reduced input. Only for a job whose output
    // is the source size: it is refused beside an upscaling output.
    uint32_t processingScale{100};
    // How readily a scene cut resets the history (TemporalSettings.h). Defaults to
    // what every job did before the setting existed.
    TemporalSettings temporal{};
};

struct NeuralRenderProgress {
    NeuralRenderPhase phase{NeuralRenderPhase::Acquiring};
    uint64_t completedFrames{};
    uint64_t totalFrames{};
    uint64_t bytes{};
    // Source acquisition only: how much of the source has been copied locally
    // and how much is expected. Frames do not exist yet at that point, so a bar
    // driven by frame counts cannot move and the copy looks like a hang.
    double acquiredSeconds{};
    double expectedSeconds{};
    std::chrono::milliseconds elapsed{};
    std::chrono::milliseconds estimatedRemaining{};
    // Non-None only while phase == Recovering: the failure being retried.
    NeuralRenderFailure recovering{NeuralRenderFailure::None};
    uint32_t retries{};
};

struct NeuralRenderResult {
    bool ok{};
    bool cancelled{};
    EncoderKind encoder{EncoderKind::HevcNvenc};
    uint64_t frameCount{};
    int64_t duration100ns{};
    uint64_t nativeEvaluations{};
    uint64_t verifiedNeuralFrames{};
    bool feature18ArmedBeforeCapture{};
    NeuralRuntimeEvidence evidence{};
    NeuralRenderFailure failure{NeuralRenderFailure::None};
    uint64_t jobId{};
    uint32_t historyResets{};
    uint32_t frameRetries{};
    // Scene-cut decisions the job's guide generator took and withheld, over its
    // whole lifetime: preroll, every captured frame and any encoder retry pass.
    SceneCutAccounting sceneCuts{};
    // Absolute source pts of the first captured frame (== range.start100ns
    // for range renders, 0 for whole-source renders).
    int64_t firstTimestamp100ns{};
    NeuralRenderTiming timing{};
    // Cold-start phases this run measured. The helper fills its own five; the
    // player merges the four it owns before the receipt is written.
    NeuralColdStartTimeline coldStart{};
    std::wstring detail;
    // False for a job that asked for Super Resolution alone: the helper ran it
    // with the neural add-on disabled, and the result says so instead of
    // leaving the feature-18 fields empty for a reader to guess about. It is
    // the request's requireNeural carried back, so the parent can refuse a
    // result that answers a different question than it asked.
    bool neural{true};
};

NeuralRuntimeEvidence ParseNeuralRuntimeEvidence(std::string_view reshadeLogSegment);

// Reads the ReShade log that this process's proxy session is writing and polls
// until its feature-18 evidence stabilizes (or a bounded wait elapses).
// ReShade rotates to ReShade.log1 when ReShade.log is held by another process
// and a previous session's file may still be present, so the file is chosen by
// write time against this process's start time rather than by name. Shared by
// the offline job and the preflight probe so both judge feature 18 from the
// same evidence rules.
std::string ReadNeuralRuntimeSessionLog(const std::filesystem::path& runtimeDirectory);
// The same log, read once and returned as it stands. For a caller that is
// mid-render and wants to know whether the add-on has armed feature 18 YET:
// the stabilizing read above waits for arming to appear, and a caller that
// stops submitting frames in order to wait has stopped producing the only
// thing that can make it appear. RenoDX 6.x will not inject until its
// compute-state shadow has observed a command-list Reset, which is another
// evaluate away, so the probe polls with this between frames instead.
std::string ReadNeuralRuntimeSessionLogSnapshot(const std::filesystem::path& runtimeDirectory);
// The log file selected by that rule; empty when neither candidate belongs to
// this process's session.
std::filesystem::path ResolveNeuralRuntimeLogPath(const std::filesystem::path& runtimeDirectory);

enum class OfflineFrameRead { FrameReady, EndOfStream, Error, Cancelled };

struct OfflineDecodedFrame {
    std::vector<uint8_t> bgra;
    int64_t timestamp100ns{};
    bool discontinuity{};
    uint64_t frameNumber{};
    uint32_t sourceGeneration{};
};

// Output of one evaluator submission. `id` is the identity the evaluator
// stamped on its output (guide/capture identity); `id.reset` != None means the
// evaluator actually reset temporal history for this frame, whether the job
// asked for it or a cut was detected inside the guide generator.
struct OfflineEvaluation {
    std::vector<uint8_t> bgra;
    FrameIdentity id{};
};

class IFrameSource {
public:
    virtual ~IFrameSource() = default;
    virtual bool Open(const std::filesystem::path& path, std::stop_token stop,
                      double seekSeconds) = 0;
    virtual void Close() = 0;
    virtual OfflineFrameRead Read(OfflineDecodedFrame& frame, std::stop_token stop) = 0;
};

class INeuralFrameEvaluator {
public:
    virtual ~INeuralFrameEvaluator() = default;
    // `outputWidth`/`outputHeight` are the capture size; they equal the source
    // size unless the job upscales.
    virtual bool Initialize(HWND renderWindow, uint32_t width, uint32_t height,
                            uint32_t outputWidth, uint32_t outputHeight, double fps,
                            const GuideControls& guides) = 0;
    virtual bool Submit(const OfflineDecodedFrame& frame, const FrameIdentity& id, bool capture,
                        OfflineEvaluation& out) = 0;
    virtual bool FeatureCreated() const = 0;
    virtual uint64_t EvaluationCount() const = 0;
    // The backend's own tally of completed neural evaluations, as against
    // EvaluationCount, which is the evaluator's count of accepted submits. A
    // render is refused when this advanced fewer times than frames were
    // captured; an evaluator with no separate backend reports its submits.
    virtual uint64_t NeuralEvaluations() const { return EvaluationCount(); }
    virtual void ResetTemporal() = 0;
    // Asks for one hook-visible CreateFeature, for the case where the neural
    // add-on missed the first one. Returns false when the evaluator cannot make
    // that request. Never called once inline interception is known to be armed:
    // releasing a live feature tears the add-on's neural worksets down.
    virtual bool RequestFeatureRehook() { return false; }
    // Classification of the most recent failed Submit.
    virtual NeuralRenderFailure LastFailure() const = 0;
    virtual double LastNeuralGpuMs() const { return 0.0; }
    virtual uint64_t PeakLocalVideoMemoryMiB() const { return 0; }
};

class IFrameEncoder {
public:
    virtual ~IFrameEncoder() = default;
    virtual EncodeError Start(const EncoderSpec& spec,
                              const std::filesystem::path& output) = 0;
    virtual EncodeError WriteFrame(std::span<const uint8_t> bgra,
                                   std::stop_token stop) = 0;
    virtual EncodeError Finish(std::stop_token stop) = 0;
    virtual void Cancel() = 0;
};

// One instance may serve several jobs. A resident helper keeps one of these for
// its process lifetime so the D3D12 device, the NGX instance, the CUDA context
// and the feature-18 workset survive between jobs; the per-job state that would
// otherwise describe the previous job is reset on entry to Run.
class OfflineNeuralRenderer {
public:
    using ProgressCallback = std::function<void(const NeuralRenderProgress&)>;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    OfflineNeuralRenderer();
    // Out of line because the retained device state is an incomplete type here,
    // and because dropping it has to happen before the caller destroys the
    // render window its swapchain is attached to.
    ~OfflineNeuralRenderer();
    OfflineNeuralRenderer(const OfflineNeuralRenderer&) = delete;
    OfflineNeuralRenderer& operator=(const OfflineNeuralRenderer&) = delete;
    // Injects the collaborators the job would otherwise build itself. Default
    // construction is production: the real decoder, evaluator and encoder, the
    // ReShade log beside the module, and request.pauseEvent. `paused` replaces
    // NeuralRenderRequest::pauseEvent: true while the job must hold between
    // frames. `encoderFactory` supplies the extra encoders a segmented job
    // rotates through; a single-file job never calls it.
    OfflineNeuralRenderer(IFrameSource& source, INeuralFrameEvaluator& evaluator,
                          IFrameEncoder& encoder,
                          std::function<std::string()> evidenceProvider,
                          Clock clock = {}, std::function<bool()> paused = {},
                          std::function<std::unique_ptr<IFrameEncoder>()> encoderFactory = {});

    // `segments` is used only when request.segmentFrames > 0. `coldStart` is
    // invoked once, on the finalize thread when the first output file is
    // published or on the calling thread when the job ends without publishing
    // one, so a failed run still reports how far the stack got.
    NeuralRenderResult Run(const NeuralRenderRequest& request,
                           ProgressCallback progress = {},
                           std::stop_token stop = {},
                           const NeuralSegmentSink& segments = {},
                           NeuralColdStartCallback coldStart = {});

    // What the last Run did with the device, the NGX instance and the feature
    // this object retains. FeatureReused is the case residency exists for: the
    // job paid neither the neural bring-up nor the feature arm, and its
    // cold-start timeline says so by reporting neither phase.
    enum class Residency { Initialized, FeatureReused, FeatureRecreated };
    Residency LastResidency() const noexcept { return residency_; }

    // False when this instance must not serve another job. The session log the
    // evidence chain is read from is append-only for the life of the process,
    // and past a size a read cannot return the next job would fail its evidence
    // check for a reason that has nothing to do with it; a caller that can
    // relaunch should relaunch instead. Always true before the first Run.
    bool ReusableForAnotherJob() const;

    // A point sample of what this renderer's process holds on the adapter's
    // local segment, in MiB, from IDXGIAdapter3::QueryVideoMemoryInfo - the
    // same source as the per-frame peak in NeuralRenderTiming, asked as a
    // point question instead of a running maximum. All zero and unarmed
    // before the first production Run: there is no device, so nothing of ours
    // is resident to measure.
    struct MemoryFootprint {
        uint64_t localVramMiB{};
        bool featureArmed{};
    };
    MemoryFootprint SampleMemoryFootprint() const;

    // What an idle helper's attempt to give feature memory back observed.
    // `released` is the mechanism - the workset was handed back - and never a
    // claim that the runtime returned anything: the difference between the two
    // footprints is the only thing that says whether it did.
    struct IdleFeatureRelease {
        MemoryFootprint before;
        MemoryFootprint after;
        bool released{};
    };
    // Hands the feature-18 workset back while no job is running, keeping the
    // device, the NGX instance and the encoder's helper lookup. The next Run
    // re-arms the feature through the same path a job that inherited none
    // uses, so this costs the arm and saves whatever the runtime frees.
    // Safe to call with no retained device: it reports an unarmed footprint
    // and no release.
    IdleFeatureRelease ReleaseIdleFeatureMemory();

private:
    // Null unless a caller injected them; Run() builds the production adapters otherwise.
    IFrameSource* source_{};
    INeuralFrameEvaluator* evaluator_{};
    IFrameEncoder* encoder_{};
    std::function<std::string()> evidenceProvider_;
    Clock clock_;
    std::function<bool()> paused_;
    std::function<std::unique_ptr<IFrameEncoder>()> encoderFactory_;
    // The production device, evaluator, encoder and session-log reader, kept
    // across calls. Null until the first production Run; a renderer that was
    // handed collaborators uses those instead and never builds one.
    struct Retained;
    std::unique_ptr<Retained> retained_;
    Residency residency_{Residency::Initialized};
};
