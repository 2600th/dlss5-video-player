#pragma once

#include "FrameIdentity.h"
#include "GuideControls.h"
#include "MediaPipeline.h"
#include "NeuralRenderTypes.h"

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
