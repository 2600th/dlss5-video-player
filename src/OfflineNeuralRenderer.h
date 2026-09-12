#pragma once

#include "FrameIdentity.h"
#include "GuideControls.h"
#include "MediaPipeline.h"

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

enum class NeuralRenderPhase {
    CheckingCache,
    Acquiring,
    Decoding,
    NeuralRendering,
    Encoding,
    Validating,
    Ready,
    Preflight,
    Paused,
    Recovering,
};

// Explicit failure classification. Every failed job names exactly one kind so
// the player can show a distinct state instead of a generic failure and so
// receipts record what actually stopped the render.
enum class NeuralRenderFailure : uint8_t {
    None,
    Source,
    Encoder,
    Neural,
    GpuStall,
    DeviceRemoved,
    WorkerCrashed,
    RetryExhausted,
    Cancelled,
    Preflight,
    Identity,
    Protocol,
};

constexpr std::string_view NeuralRenderFailureName(NeuralRenderFailure failure) noexcept
{
    switch (failure) {
        case NeuralRenderFailure::None: return "none";
        case NeuralRenderFailure::Source: return "source";
        case NeuralRenderFailure::Encoder: return "encoder";
        case NeuralRenderFailure::Neural: return "neural";
        case NeuralRenderFailure::GpuStall: return "gpu-stall";
        case NeuralRenderFailure::DeviceRemoved: return "device-removed";
        case NeuralRenderFailure::WorkerCrashed: return "worker-crashed";
        case NeuralRenderFailure::RetryExhausted: return "retry-exhausted";
        case NeuralRenderFailure::Cancelled: return "cancelled";
        case NeuralRenderFailure::Preflight: return "preflight";
        case NeuralRenderFailure::Identity: return "identity";
        case NeuralRenderFailure::Protocol: return "protocol";
    }
    return "unknown";
}

// Half-open source interval [start, end) on the decoder's CFR timeline. Both
// zero means the whole source. The encoded output always starts at pts 0; the
// absolute start is recorded beside it so playback can realign.
struct NeuralRenderRange {
    int64_t start100ns{};
    int64_t end100ns{};

    friend bool operator==(const NeuralRenderRange&, const NeuralRenderRange&) = default;
    constexpr bool Whole() const noexcept { return start100ns == 0 && end100ns == 0; }
};

// Frames evaluated (never captured) before the first captured frame so the
// temporal history at the range start matches a continuous render.
inline constexpr uint32_t kDefaultPrerollFrames = 60;
// Exact-frame retries of the same frame before the job is classified as
// retry-exhausted. Retries never skip a frame.
inline constexpr uint32_t kDefaultFrameRetryLimit = 3;

struct NeuralRenderTiming {
    uint64_t samples{};
    double neuralGpuMsP50{};
    double neuralGpuMsP95{};
    double neuralGpuMsMax{};
    double guideMsMean{};
    double captureMsMean{};
    uint64_t peakLocalVramMiB{};

    friend bool operator==(const NeuralRenderTiming&, const NeuralRenderTiming&) = default;
};

// A DLAA-only run is indistinguishable from a healthy one in every counter the
// evidence chain checks: feature 18 is created, it is evaluated, frames are
// produced and verified. What it cannot fake is GPU cost. Measured at 1920x1080
// on this project's RTX 5090 / driver 616.64: the worker that stopped
// presenting after a feature recreate reported a median of 0.46 ms per frame
// (docs/DLSS5_VIDEO_ROADMAP.md item 1 open gap), while healthy renders on the
// same machine and geometry report 3.683776, 3.696608 and 3.715776 ms
// (the three cache/v1 receipt.json files, 2534 to 2658 samples each) and the
// reference benchmark run reads 3.26 to 3.27 ms (docs/BENCHMARK.md reference
// table), the lowest healthy median in evidence. Those two ends are a factor of
// 7.1 apart, so the floor is their geometric midpoint, 1.223 ms at 1920x1080:
// 2.66x above the DLAA-only observation, 2.66x below the benchmark median and
// 3.01x below the lowest receipt median. Per output megapixel because the pass
// scales with pixels, and the remaining risk is one-sided in the safe
// direction: per-pixel neural cost only rises on slower hardware, so a fixed
// floor can misjudge a healthy run only on a GPU substantially faster than a
// 5090, while the failure it catches sits 7x below it on the fastest card that
// exists today.
inline constexpr double kNeuralGpuMsFloorPerMegapixel = 0.59;

inline double NeuralGpuMsFloor(uint32_t width, uint32_t height)
{
    return kNeuralGpuMsFloorPerMegapixel * (double(width) * double(height) / 1000000.0);
}

// No samples at all means the build carries no timing instrumentation (such
// builds report the ms fields as zero), so a missing measurement is never a
// verdict against the run.
inline bool NeuralTimingClearsFloor(const NeuralRenderTiming& timing, uint32_t width, uint32_t height)
{
    return timing.samples == 0 || timing.neuralGpuMsP50 >= NeuralGpuMsFloor(width, height);
}

struct NeuralRuntimeEvidence {
    bool upscalingOff{};
    bool inlineInterceptionContract{};
    bool feature18Created{};
    bool feature18Evaluated{};
    bool laterFailure{};
    uint64_t highestObservedEvaluation{};

    bool Valid() const noexcept
    {
        return upscalingOff && inlineInterceptionContract && feature18Created && feature18Evaluated &&
               highestObservedEvaluation > 0 && !laterFailure;
    }
};

// One finalized output file produced while the job is still running. Each file
// starts at its own pts zero; firstTimestamp100ns places it on the source
// timeline and end100ns is exclusive.
struct NeuralRenderSegment {
    uint64_t index{};
    uint64_t firstFrameNumber{};
    int64_t firstTimestamp100ns{};
    int64_t end100ns{};
    uint64_t frameCount{};
    std::wstring fileName;          // relative to the staging directory
};

// Consumer of finalized segments. onSegment is called from the job's finalize
// thread, in index order, only after that file's encoder exited successfully.
struct NeuralSegmentSink {
    std::function<void(const NeuralRenderSegment&)> onSegment;
    std::function<void()> onRestart; // a from-zero relaunch invalidated every earlier segment
};

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
    // attempt reads it.
    uint32_t nvencPreset{7};
    // Decode the source to NV12 and convert it to BGRA on the GPU (true) or let ffmpeg
    // convert on the CPU (false). Default false: the GPU conversion applies a fixed
    // BT.709 limited-range inverse and nothing probes the source's matrix or range,
    // so a BT.601 or full-range source would reach the model with shifted colour.
    bool gpuSourceConversion{false};
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
    // Absolute source pts of the first captured frame (== range.start100ns
    // for range renders, 0 for whole-source renders).
    int64_t firstTimestamp100ns{};
    NeuralRenderTiming timing{};
    std::wstring detail;
};

NeuralRuntimeEvidence ParseNeuralRuntimeEvidence(std::string_view reshadeLogSegment);

#ifndef OFFLINE_NEURAL_RENDERER_TESTING
// Reads the ReShade log that this process's proxy session is writing and polls
// until its feature-18 evidence stabilizes (or a bounded wait elapses).
// ReShade rotates to ReShade.log1 when ReShade.log is held by another process
// and a previous session's file may still be present, so the file is chosen by
// write time against this process's start time rather than by name. Shared by
// the offline job and the preflight probe so both judge feature 18 from the
// same evidence rules.
std::string ReadNeuralRuntimeSessionLog(const std::filesystem::path& runtimeDirectory);
// The log file selected by that rule; empty when neither candidate belongs to
// this process's session.
std::filesystem::path ResolveNeuralRuntimeLogPath(const std::filesystem::path& runtimeDirectory);
#endif

#ifdef OFFLINE_NEURAL_RENDERER_TESTING
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
    virtual bool Initialize(HWND renderWindow, uint32_t width, uint32_t height, double fps,
                            const GuideControls& guides) = 0;
    virtual bool Submit(const OfflineDecodedFrame& frame, const FrameIdentity& id, bool capture,
                        OfflineEvaluation& out) = 0;
    virtual bool FeatureCreated() const = 0;
    virtual uint64_t EvaluationCount() const = 0;
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
#endif

class OfflineNeuralRenderer {
public:
    using ProgressCallback = std::function<void(const NeuralRenderProgress&)>;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    OfflineNeuralRenderer() = default;
#ifdef OFFLINE_NEURAL_RENDERER_TESTING
    // `paused` replaces NeuralRenderRequest::pauseEvent: true while the job
    // must hold between frames. `encoderFactory` supplies the extra encoders a
    // segmented job rotates through; a single-file job never calls it.
    OfflineNeuralRenderer(IFrameSource& source, INeuralFrameEvaluator& evaluator,
                          IFrameEncoder& encoder,
                          std::function<std::string()> evidenceProvider,
                          Clock clock = {}, std::function<bool()> paused = {},
                          std::function<std::unique_ptr<IFrameEncoder>()> encoderFactory = {});
#endif

    // `segments` is used only when request.segmentFrames > 0.
    NeuralRenderResult Run(const NeuralRenderRequest& request,
                           ProgressCallback progress = {},
                           std::stop_token stop = {},
                           const NeuralSegmentSink& segments = {});

private:
#ifdef OFFLINE_NEURAL_RENDERER_TESTING
    IFrameSource* testSource_{};
    INeuralFrameEvaluator* testEvaluator_{};
    IFrameEncoder* testEncoder_{};
    std::function<std::string()> testEvidenceProvider_;
    Clock testClock_;
    std::function<bool()> testPaused_;
    std::function<std::unique_ptr<IFrameEncoder>()> testEncoderFactory_;
#endif
};
