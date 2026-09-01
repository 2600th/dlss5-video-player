#pragma once

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
    Acquiring,
    Decoding,
    NeuralRendering,
    Encoding,
    Validating,
    Ready,
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
};

struct NeuralRenderProgress {
    NeuralRenderPhase phase{NeuralRenderPhase::Acquiring};
    uint64_t completedFrames{};
    uint64_t totalFrames{};
    uint64_t bytes{};
    std::chrono::milliseconds elapsed{};
    std::chrono::milliseconds estimatedRemaining{};
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
    std::wstring detail;
};

NeuralRuntimeEvidence ParseNeuralRuntimeEvidence(std::string_view reshadeLogSegment);

#ifdef OFFLINE_NEURAL_RENDERER_TESTING
enum class OfflineFrameRead { FrameReady, EndOfStream, Error, Cancelled };

struct OfflineDecodedFrame {
    std::vector<uint8_t> bgra;
    int64_t timestamp100ns{};
    bool discontinuity{};
};

class IFrameSource {
public:
    virtual ~IFrameSource() = default;
    virtual bool Open(const std::filesystem::path& path, std::stop_token stop) = 0;
    virtual void Close() = 0;
    virtual OfflineFrameRead Read(OfflineDecodedFrame& frame, std::stop_token stop) = 0;
};

class INeuralFrameEvaluator {
public:
    virtual ~INeuralFrameEvaluator() = default;
    virtual bool Initialize(HWND renderWindow, uint32_t width, uint32_t height, double fps) = 0;
    virtual bool Submit(const OfflineDecodedFrame& frame, bool temporalReset, bool capture,
                        std::vector<uint8_t>& bgra) = 0;
    virtual bool FeatureCreated() const = 0;
    virtual uint64_t EvaluationCount() const = 0;
    virtual void ResetTemporal() = 0;
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
    OfflineNeuralRenderer(IFrameSource& source, INeuralFrameEvaluator& evaluator,
                          IFrameEncoder& encoder,
                          std::function<std::string()> evidenceProvider,
                          Clock clock = {});
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
#endif
};
