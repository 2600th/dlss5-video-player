#pragma once

#include "FrameIdentity.h"
#include "GuideControls.h"
#include "MediaPipeline.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
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
    // Manual-reset event owned by the caller. Signalled means "pause"; the
    // worker checks it between frames and reports NeuralRenderPhase::Paused.
    HANDLE pauseEvent{};
};

struct NeuralRenderProgress {
    NeuralRenderPhase phase{NeuralRenderPhase::Acquiring};
    uint64_t completedFrames{};
    uint64_t totalFrames{};
    uint64_t bytes{};
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
    // must hold between frames.
    OfflineNeuralRenderer(IFrameSource& source, INeuralFrameEvaluator& evaluator,
                          IFrameEncoder& encoder,
                          std::function<std::string()> evidenceProvider,
                          Clock clock = {}, std::function<bool()> paused = {});
#endif

    NeuralRenderResult Run(const NeuralRenderRequest& request,
                           ProgressCallback progress = {},
                           std::stop_token stop = {});

private:
#ifdef OFFLINE_NEURAL_RENDERER_TESTING
    IFrameSource* testSource_{};
    INeuralFrameEvaluator* testEvaluator_{};
    IFrameEncoder* testEncoder_{};
    std::function<std::string()> testEvidenceProvider_;
    Clock testClock_;
    std::function<bool()> testPaused_;
#endif
};
