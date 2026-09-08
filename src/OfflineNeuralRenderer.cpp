#include "OfflineNeuralRenderer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <thread>

#ifndef OFFLINE_NEURAL_RENDERER_TESTING
#include "D3D12Renderer.h"
#include "TemporalGuides.h"
#include "VideoDecoder.h"
#include "DLSSBackend.h"
#endif

namespace {

using SteadyClock = std::chrono::steady_clock;

enum class JobRead { FrameReady, EndOfStream, Error, Cancelled };

struct JobFrame {
    std::vector<uint8_t> bgra;
    int64_t timestamp100ns{};
    bool discontinuity{};
    uint64_t frameNumber{};
    uint32_t sourceGeneration{};
};

// Outputs of one evaluator submission. `id` is the identity the evaluator
// stamped on its guide/capture; `id.reset` != None means temporal history was
// actually reset for this frame (requested by the job or a detected cut).
struct JobEvaluation {
    std::vector<uint8_t> bgra;
    FrameIdentity id{};
    double guideMs{};
    double captureMs{};
    double neuralGpuMs{};
};

struct AttemptResult {
    NeuralRenderFailure failure{NeuralRenderFailure::None};
    EncodeError encoderError{EncodeError::None};
    uint64_t frames{};
    uint64_t bytes{};
    uint64_t evaluations{};
    uint32_t historyResets{};
    bool hasTimestamp{};
    int64_t firstTimestamp{};
    int64_t lastTimestamp{};
    std::vector<double> neuralGpuMs;
    double guideMsTotal{};
    double captureMsTotal{};
};

std::string LowerAscii(std::string_view value)
{
    std::string result(value);
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') character = char(character - 'A' + 'a');
    }
    return result;
}

uint64_t HighestEvaluationCount(std::string_view lower)
{
    uint64_t highest = 0;
    constexpr std::array<std::string_view,2> markers{
        "evaluation count=", "evaluation succeeded (count="};
    for(const auto marker:markers){
        size_t position = 0;
        while ((position = lower.find(marker, position)) != std::string_view::npos) {
            position += marker.size();
            uint64_t value = 0;
            const char* first = lower.data() + position;
            const char* last = lower.data() + lower.size();
            const auto parsed = std::from_chars(first, last, value);
            if (parsed.ec == std::errc{}) highest = std::max(highest, value);
        }
    }
    return highest;
}

NeuralRenderTiming SummarizeTiming(AttemptResult& attempt, uint64_t peakLocalVramMiB)
{
    NeuralRenderTiming timing;
    timing.peakLocalVramMiB = peakLocalVramMiB;
    timing.samples = attempt.neuralGpuMs.size();
    if (timing.samples == 0) return timing;
    auto& samples = attempt.neuralGpuMs;
    std::sort(samples.begin(), samples.end());
    const auto quantile = [&](double q) {
        const size_t index = static_cast<size_t>(std::ceil(q * double(samples.size() - 1)));
        return samples[std::min(index, samples.size() - 1)];
    };
    timing.neuralGpuMsP50 = quantile(0.5);
    timing.neuralGpuMsP95 = quantile(0.95);
    timing.neuralGpuMsMax = samples.back();
    timing.guideMsMean = attempt.guideMsTotal / double(timing.samples);
    timing.captureMsMean = attempt.captureMsTotal / double(timing.samples);
    return timing;
}

const wchar_t* AttemptFailureDetail(NeuralRenderFailure failure)
{
    switch (failure) {
        case NeuralRenderFailure::GpuStall:
            return L"The GPU stalled while evaluating a frame.";
        case NeuralRenderFailure::DeviceRemoved:
            return L"The GPU device was removed during neural rendering.";
        case NeuralRenderFailure::RetryExhausted:
            return L"A frame failed every retry; the render was not continued past it.";
        case NeuralRenderFailure::Identity:
            return L"The neural output did not match the submitted source frame.";
        case NeuralRenderFailure::Source:
            return L"The source decoder failed during neural rendering.";
        case NeuralRenderFailure::Encoder:
            return L"The neural video encoder failed.";
        default:
            return L"A frame was not produced by feature 18.";
    }
}

template<class Source, class Evaluator, class Encoder, class Evidence, class Clock, class Paused>
NeuralRenderResult RunJob(const NeuralRenderRequest& request,
                          OfflineNeuralRenderer::ProgressCallback progress,
                          std::stop_token stop, Source& source, Evaluator& evaluator,
                          Encoder& encoder, Evidence evidenceProvider, Clock clock, Paused paused)
{
    NeuralRenderResult result;
    result.jobId = request.jobId;
    auto fail = [&](NeuralRenderFailure failure, std::wstring detail) {
        result.failure = failure;result.detail = std::move(detail);return result;
    };
    if (request.sourcePath.empty() || request.stagingVideoPath.empty() ||
        !request.width || !request.height || !std::isfinite(request.fps) || request.fps <= 0.0 ||
        !std::isfinite(request.durationSeconds) || request.durationSeconds <= 0.0) {
        return fail(NeuralRenderFailure::Source, L"Invalid neural render request.");
    }
    const int64_t frameDuration = static_cast<int64_t>(std::llround(10000000.0 / request.fps));
    const int64_t sourceDuration = static_cast<int64_t>(std::llround(request.durationSeconds * 10000000.0));
    const int64_t rangeStart = request.range.start100ns;
    const bool boundedEnd = request.range.end100ns != 0;
    const int64_t rangeEnd = boundedEnd ? request.range.end100ns : sourceDuration;
    if (rangeStart < 0 || rangeStart >= rangeEnd || rangeEnd > sourceDuration + frameDuration) {
        return fail(NeuralRenderFailure::Source,
                    L"The neural render range is outside the source timeline.");
    }
    const uint64_t totalFrames = std::max<uint64_t>(1, static_cast<uint64_t>(
        std::llround(double(rangeEnd - rangeStart) / 10000000.0 * request.fps)));
    const uint64_t expectedBytes64 = uint64_t{request.width} * request.height * 4u;
    if (expectedBytes64 > std::numeric_limits<size_t>::max()) {
        return fail(NeuralRenderFailure::Source, L"Neural render dimensions are too large.");
    }
    const size_t expectedBytes = static_cast<size_t>(expectedBytes64);
    const auto started = clock();
    auto lastProgressTime = started;
    uint64_t reportedCompleted = 0;
    uint64_t reportedBytes = 0;
    double smoothedFramesPerMs = 0.0;
    NeuralRenderProgress previous{};
    auto emitProgress = [&](NeuralRenderPhase phase, uint64_t completed, uint64_t bytes,
                            bool frameTick, NeuralRenderFailure recovering) {
        const auto now = clock();
        reportedCompleted = std::max(reportedCompleted, completed);
        reportedBytes = std::max(reportedBytes, bytes);
        if (frameTick && completed > previous.completedFrames) {
            const double milliseconds = std::max(1.0,
                std::chrono::duration<double, std::milli>(now - lastProgressTime).count());
            const double instant = double(completed - previous.completedFrames) / milliseconds;
            smoothedFramesPerMs = smoothedFramesPerMs == 0.0
                ? instant : smoothedFramesPerMs * 0.75 + instant * 0.25;
            lastProgressTime = now;
        }
        NeuralRenderProgress snapshot;
        snapshot.phase = phase;
        snapshot.completedFrames = reportedCompleted;
        snapshot.totalFrames = std::max(totalFrames, reportedCompleted);
        snapshot.bytes = reportedBytes;
        snapshot.elapsed = std::max(previous.elapsed,
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started));
        if (snapshot.completedFrames < snapshot.totalFrames && smoothedFramesPerMs > 0.0) {
            snapshot.estimatedRemaining = std::chrono::milliseconds(static_cast<int64_t>(
                std::ceil(double(snapshot.totalFrames - snapshot.completedFrames) /
                          smoothedFramesPerMs)));
        }
        snapshot.recovering = recovering;
        snapshot.retries = result.frameRetries;
        if (progress) progress(snapshot);
        previous = snapshot;
    };
    auto emit = [&](NeuralRenderPhase phase, uint64_t completed, uint64_t bytes, bool frameTick) {
        emitProgress(phase, completed, bytes, frameTick, NeuralRenderFailure::None);
    };
    auto cancelled = [&](std::wstring detail) {
        encoder.Cancel();source.Close();result.cancelled = true;
        return fail(NeuralRenderFailure::Cancelled, std::move(detail));
    };
    auto evaluatorFailure = [&] {
        const NeuralRenderFailure failure = evaluator.LastFailure();
        return failure == NeuralRenderFailure::None ? NeuralRenderFailure::Neural : failure;
    };
    // The job's own history generation: bumped for every reset it requests.
    // The evaluator stamps its own generation on its outputs; SameSource
    // comparisons ignore both.
    uint32_t historyGeneration = 0;
    auto identity = [&](const JobFrame& frame, HistoryReset reset) {
        if (reset != HistoryReset::None) ++historyGeneration;
        return FrameIdentity{frame.frameNumber, frame.timestamp100ns, frame.sourceGeneration,
                             historyGeneration, request.jobId, reset};
    };

    emit(NeuralRenderPhase::Acquiring, 0, 0, false);
    if (stop.stop_requested()) return cancelled(L"Neural render was cancelled.");
    if (!source.Open(request.sourcePath, stop, double(rangeStart) / 10000000.0)) {
        if (stop.stop_requested()) return cancelled(L"Neural render was cancelled.");
        return fail(NeuralRenderFailure::Source, L"The source video could not be opened.");
    }
    emit(NeuralRenderPhase::Decoding, 0, 0, false);
    if (!evaluator.Initialize(request.renderWindow, request.width, request.height, request.fps,
                              request.guides)) {
        source.Close();
        return fail(NeuralRenderFailure::Neural, L"The neural renderer could not be initialized.");
    }

    const bool singleFrameSource = totalFrames == 1;
    const uint64_t primeLimit = singleFrameSource
        ? 120 : std::max<uint64_t>(2, std::min<uint64_t>(totalFrames, 120));
    uint64_t primed = 0;
    for (JobFrame primingFrame; !evaluator.FeatureCreated() && primed < primeLimit; ++primed) {
        if (stop.stop_requested()) return cancelled(L"Neural render was cancelled.");
        if (!singleFrameSource || primed == 0) {
            const JobRead read = source.Read(primingFrame, stop);
            if (read == JobRead::Cancelled) return cancelled(L"Neural render was cancelled.");
            if (read != JobRead::FrameReady) {
                source.Close();
                return fail(NeuralRenderFailure::Source,
                            L"Feature 18 could not be primed from the source.");
            }
        } else {
            // A photo may need several presents to create feature 18. Reuse it
            // only for warm-up; capture below still reopens and reads it once.
            primingFrame.discontinuity = false;
        }
        const HistoryReset reason = primed == 0 ? HistoryReset::FirstFrame
            : primingFrame.discontinuity ? HistoryReset::SourceChange : HistoryReset::None;
        JobEvaluation ignored;
        if (!evaluator.Submit(primingFrame, identity(primingFrame, reason), false, ignored)) {
            source.Close();
            return fail(evaluatorFailure(), L"Feature 18 priming failed.");
        }
    }
    if (!evaluator.FeatureCreated()) {
        source.Close();
        return fail(NeuralRenderFailure::Neural, L"Feature 18 was not created.");
    }
    const NeuralRuntimeEvidence armedEvidence=
        ParseNeuralRuntimeEvidence(evidenceProvider());
    if(!armedEvidence.Valid()){
        source.Close();
        return fail(NeuralRenderFailure::Neural,
                    L"Feature 18 inline interception was not armed before frame capture.");
    }
    result.feature18ArmedBeforeCapture=true;
    uint64_t successfulAttemptBaseline=armedEvidence.highestObservedEvaluation;

    // Capture restarts from the preroll position: frames before the range are
    // evaluated without capture so the history at range.start matches a
    // continuous render. A whole-source render has no preroll.
    const int64_t prerollStart = rangeStart > 0
        ? std::max<int64_t>(0, rangeStart - int64_t{request.prerollFrames} * frameDuration) : 0;
    auto reopenAtPreroll = [&] {
        source.Close();
        if (!source.Open(request.sourcePath, stop, double(prerollStart) / 10000000.0)) return false;
        evaluator.ResetTemporal();
        return true;
    };
    if (!reopenAtPreroll()) {
        if (stop.stop_requested()) return cancelled(L"Neural render was cancelled.");
        return fail(NeuralRenderFailure::Source,
                    L"The source could not be restarted at the capture start.");
    }

    auto runAttempt = [&](EncoderKind kind) {
        AttemptResult attempt;
        const EncodeError startError = encoder.Start(
            EncoderSpec{request.width, request.height, request.fps, kind},
            request.stagingVideoPath);
        if (startError != EncodeError::None) {
            attempt.failure = startError == EncodeError::Cancelled
                ? NeuralRenderFailure::Cancelled : NeuralRenderFailure::Encoder;
            attempt.encoderError = startError;return attempt;
        }
        auto abort = [&](NeuralRenderFailure failure) {
            attempt.failure = failure;
            if (failure == NeuralRenderFailure::Cancelled) attempt.encoderError = EncodeError::Cancelled;
            encoder.Cancel();
        };
        // Submits one frame, retrying the exact same frame on neural/GPU-stall
        // failures. The last permitted retry resets history; a frame that still
        // fails ends the attempt (never skipped). Device loss is not retried.
        auto evaluate = [&](const JobFrame& frame, HistoryReset reason, bool capture,
                            JobEvaluation& out) {
            FrameIdentity id = identity(frame, reason);
            for (uint32_t retry = 0;; ++retry) {
                if (stop.stop_requested()) return NeuralRenderFailure::Cancelled;
                const uint64_t before = evaluator.EvaluationCount();
                out = JobEvaluation{};
                const bool ok = evaluator.Submit(frame, id, capture, out) &&
                    evaluator.EvaluationCount() > before &&
                    (!capture || out.bgra.size() == expectedBytes);
                if (ok) {
                    if (!out.id.SameSource(id)) return NeuralRenderFailure::Identity;
                    if (out.id.reset != HistoryReset::None) ++attempt.historyResets;
                    return NeuralRenderFailure::None;
                }
                NeuralRenderFailure failure = evaluatorFailure();
                if (failure == NeuralRenderFailure::DeviceRemoved) return failure;
                if (failure != NeuralRenderFailure::GpuStall) failure = NeuralRenderFailure::Neural;
                if (request.frameRetryLimit == 0) return failure;
                if (retry >= request.frameRetryLimit) return NeuralRenderFailure::RetryExhausted;
                ++result.frameRetries;
                emitProgress(NeuralRenderPhase::Recovering, attempt.frames, attempt.bytes, false, failure);
                // Earlier retries resubmit the identical identity; only the
                // final one discards history so a poisoned state cannot fail
                // the same frame forever.
                if (retry + 1 == request.frameRetryLimit) {
                    id.reset = HistoryReset::Retry;id.historyGeneration = ++historyGeneration;
                }
            }
        };
        bool prerollEvaluated = false;
        bool hasPrevious = false;
        int64_t previousTimestamp = 0;
        uint64_t previousFrameNumber = 0;
        for (;;) {
            if (stop.stop_requested()) {abort(NeuralRenderFailure::Cancelled);return attempt;}
            if (paused()) {
                emit(NeuralRenderPhase::Paused, attempt.frames, attempt.bytes, false);
                while (paused()) {
                    if (stop.stop_requested()) {abort(NeuralRenderFailure::Cancelled);return attempt;}
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            JobFrame frame;
            const JobRead read = source.Read(frame, stop);
            if (read == JobRead::EndOfStream) {
                if (attempt.frames == 0) {abort(NeuralRenderFailure::Source);return attempt;}
                break;
            }
            if (read == JobRead::Cancelled) {abort(NeuralRenderFailure::Cancelled);return attempt;}
            if (read != JobRead::FrameReady) {abort(NeuralRenderFailure::Source);return attempt;}
            if (frame.timestamp100ns < 0 ||
                (hasPrevious && (frame.timestamp100ns <= previousTimestamp ||
                                 frame.frameNumber != previousFrameNumber + 1))) {
                abort(NeuralRenderFailure::Source);return attempt;
            }
            hasPrevious = true;previousTimestamp = frame.timestamp100ns;
            previousFrameNumber = frame.frameNumber;
            if (frame.timestamp100ns < rangeStart) {
                JobEvaluation ignored;
                const HistoryReset reason = !prerollEvaluated ? HistoryReset::Preroll
                    : frame.discontinuity ? HistoryReset::SourceChange : HistoryReset::None;
                const NeuralRenderFailure failure = evaluate(frame, reason, false, ignored);
                if (failure != NeuralRenderFailure::None) {abort(failure);return attempt;}
                prerollEvaluated = true;
                continue;
            }
            if (boundedEnd && frame.timestamp100ns >= rangeEnd) {
                if (attempt.frames == 0) {abort(NeuralRenderFailure::Source);return attempt;}
                break;
            }
            JobEvaluation evaluation;
            for (uint64_t capture = 1; ; ++capture) {
                const HistoryReset reason = capture > 1 ? HistoryReset::None
                    : (attempt.frames == 0 && !prerollEvaluated) ? HistoryReset::FirstFrame
                    : frame.discontinuity ? HistoryReset::SourceChange : HistoryReset::None;
                const NeuralRenderFailure failure = evaluate(frame, reason, true, evaluation);
                if (failure != NeuralRenderFailure::None) {abort(failure);return attempt;}
                if (attempt.frames > 0) break;
                // The runtime logs successful evaluations sparsely. Capture the
                // first source frame until a fresh receipt exists, retaining
                // only its latest pixels for encoding. Each retry has its own
                // baseline, and these extra captures never extend the timeline.
                if (capture == 1 || capture % 10 == 0) {
                    const auto receipt = ParseNeuralRuntimeEvidence(evidenceProvider());
                    if (!receipt.Valid()) {abort(NeuralRenderFailure::Neural);return attempt;}
                    if (receipt.highestObservedEvaluation > successfulAttemptBaseline) break;
                }
                if (capture >= 120) {abort(NeuralRenderFailure::Neural);return attempt;}
            }
            const EncodeError writeError = encoder.WriteFrame(evaluation.bgra, stop);
            if (writeError != EncodeError::None) {
                attempt.failure = writeError == EncodeError::Cancelled
                    ? NeuralRenderFailure::Cancelled : NeuralRenderFailure::Encoder;
                attempt.encoderError=writeError;encoder.Cancel();return attempt;
            }
            ++attempt.frames;++attempt.evaluations;attempt.bytes+=evaluation.bgra.size();
            if (!attempt.hasTimestamp) {
                attempt.firstTimestamp=frame.timestamp100ns;
                attempt.hasTimestamp=true;
            }
            attempt.lastTimestamp=frame.timestamp100ns;
            attempt.neuralGpuMs.push_back(evaluation.neuralGpuMs);
            attempt.guideMsTotal+=evaluation.guideMs;attempt.captureMsTotal+=evaluation.captureMs;
            emit(NeuralRenderPhase::NeuralRendering,attempt.frames,attempt.bytes,true);
        }
        const EncodeError finishError=encoder.Finish(stop);
        if(finishError!=EncodeError::None){
            attempt.failure=finishError==EncodeError::Cancelled
                ? NeuralRenderFailure::Cancelled:NeuralRenderFailure::Encoder;
            attempt.encoderError=finishError;return attempt;
        }
        return attempt;
    };

    // NVENC accepts odd dimensions but silently pads them to an even size,
    // which breaks exact source/neural pairing and cache validation. libx264's
    // yuv444p path preserves odd image dimensions.
    EncoderKind selected = (request.width % 2 || request.height % 2)
        ? EncoderKind::H264Software : EncoderKind::HevcNvenc;
    AttemptResult attempt = runAttempt(selected);
    if (attempt.failure == NeuralRenderFailure::Cancelled)
        return cancelled(L"Neural render was cancelled.");
    if (attempt.failure == NeuralRenderFailure::Encoder &&
        ShouldRetryWithSoftware(selected, attempt.encoderError)) {
        encoder.Cancel();
        std::error_code removeError;std::filesystem::remove(request.stagingVideoPath, removeError);
        if (!reopenAtPreroll()) {
            if (stop.stop_requested()) return cancelled(L"Neural render was cancelled.");
            return fail(NeuralRenderFailure::Source,
                        L"The source could not be restarted for software encoding.");
        }
        const NeuralRuntimeEvidence retryEvidence=
            ParseNeuralRuntimeEvidence(evidenceProvider());
        if(!retryEvidence.Valid()){
            return fail(NeuralRenderFailure::Neural,
                        L"Feature 18 evidence was not valid before the software retry.");
        }
        successfulAttemptBaseline=retryEvidence.highestObservedEvaluation;
        selected=EncoderKind::H264Software;
        attempt=runAttempt(selected);
    }
    if (attempt.failure == NeuralRenderFailure::Cancelled)
        return cancelled(L"Neural render was cancelled.");
    if (attempt.failure != NeuralRenderFailure::None) {
        encoder.Cancel();source.Close();
        result.historyResets=attempt.historyResets;
        return fail(attempt.failure, AttemptFailureDetail(attempt.failure));
    }
    source.Close();
    emit(NeuralRenderPhase::Encoding,attempt.frames,attempt.bytes,false);
    emit(NeuralRenderPhase::Validating,attempt.frames,attempt.bytes,false);
    result.evidence=ParseNeuralRuntimeEvidence(evidenceProvider());
    result.historyResets=attempt.historyResets;
    if(!result.evidence.Valid()||
       result.evidence.highestObservedEvaluation<=successfulAttemptBaseline){
        return fail(NeuralRenderFailure::Neural,
                    L"Feature 18 runtime evidence did not advance after captured rendering or contained a later failure.");
    }
    result.ok=true;result.encoder=selected;result.frameCount=attempt.frames;
    result.nativeEvaluations=attempt.evaluations;
    result.verifiedNeuralFrames=attempt.frames;
    result.firstTimestamp100ns=attempt.firstTimestamp;
    result.duration100ns=attempt.lastTimestamp-attempt.firstTimestamp+frameDuration;
    result.timing=SummarizeTiming(attempt,evaluator.PeakLocalVideoMemoryMiB());
    emit(NeuralRenderPhase::Ready,attempt.frames,attempt.bytes,false);
    return result;
}

#ifdef OFFLINE_NEURAL_RENDERER_TESTING
struct TestSourceAdapter {
    IFrameSource& source;
    bool Open(const std::filesystem::path& path,std::stop_token stop,double seekSeconds){return source.Open(path,stop,seekSeconds);}
    void Close(){source.Close();}
    JobRead Read(JobFrame& frame,std::stop_token stop){
        OfflineDecodedFrame decoded;const auto read=source.Read(decoded,stop);
        frame={std::move(decoded.bgra),decoded.timestamp100ns,decoded.discontinuity,
               decoded.frameNumber,decoded.sourceGeneration};
        switch(read){
            case OfflineFrameRead::FrameReady:return JobRead::FrameReady;
            case OfflineFrameRead::EndOfStream:return JobRead::EndOfStream;
            case OfflineFrameRead::Cancelled:return JobRead::Cancelled;
            default:return JobRead::Error;
        }
    }
};
struct TestEvaluatorAdapter {
    INeuralFrameEvaluator& evaluator;
    bool Initialize(HWND window,uint32_t width,uint32_t height,double fps,const GuideControls& guides){
        return evaluator.Initialize(window,width,height,fps,guides);
    }
    bool Submit(const JobFrame& frame,const FrameIdentity& id,bool capture,JobEvaluation& out){
        OfflineEvaluation evaluation;
        if(!evaluator.Submit(OfflineDecodedFrame{frame.bgra,frame.timestamp100ns,frame.discontinuity,
                                                 frame.frameNumber,frame.sourceGeneration},
                             id,capture,evaluation))return false;
        out.bgra=std::move(evaluation.bgra);out.id=evaluation.id;
        out.neuralGpuMs=evaluator.LastNeuralGpuMs();return true;
    }
    bool FeatureCreated()const{return evaluator.FeatureCreated();}
    uint64_t EvaluationCount()const{return evaluator.EvaluationCount();}
    void ResetTemporal(){evaluator.ResetTemporal();}
    NeuralRenderFailure LastFailure()const{return evaluator.LastFailure();}
    uint64_t PeakLocalVideoMemoryMiB()const{return evaluator.PeakLocalVideoMemoryMiB();}
};
struct TestEncoderAdapter {
    IFrameEncoder& encoder;
    EncodeError Start(const EncoderSpec& spec,const std::filesystem::path& path){return encoder.Start(spec,path);}
    EncodeError WriteFrame(std::span<const uint8_t> frame,std::stop_token stop){return encoder.WriteFrame(frame,stop);}
    EncodeError Finish(std::stop_token stop){return encoder.Finish(stop);}
    void Cancel(){encoder.Cancel();}
};
#else
struct ProductionSourceAdapter {
    VideoDecoder decoder;
    bool Open(const std::filesystem::path& path,std::stop_token stop,double seekSeconds){
        if(!decoder.OpenSequential(path.wstring(),MediaSourceKind::LocalFile,stop))return false;
        return seekSeconds<=0.0||decoder.SeekSeconds(seekSeconds);
    }
    void Close(){decoder.Close();}
    JobRead Read(JobFrame& frame,std::stop_token stop){
        for(;;){
            VideoFrame decoded;const auto read=decoder.ReadNextAvailable(decoded,stop);
            if(read==VideoReadResult::NotReady){std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
            frame={std::move(decoded.bgra),decoded.timestamp100ns,decoded.discontinuity,
                   decoded.frameNumber,decoded.sourceGeneration};
            if(read==VideoReadResult::FrameReady)return JobRead::FrameReady;
            if(read==VideoReadResult::EndOfStream)return JobRead::EndOfStream;
            if(read==VideoReadResult::Cancelled)return JobRead::Cancelled;
            return JobRead::Error;
        }
    }
};

NeuralRenderFailure ClassifyRendererFailure(const D3D12Renderer& renderer)
{
    using FenceWait=d3d12_renderer_detail::FenceWaitResult;
    switch(renderer.LastFenceWaitResult()){
        case FenceWait::TimedOut:return NeuralRenderFailure::GpuStall;
        case FenceWait::DeviceRemoved:return NeuralRenderFailure::DeviceRemoved;
        default:break;
    }
    return renderer.GpuUnusable()?NeuralRenderFailure::DeviceRemoved:NeuralRenderFailure::Neural;
}

double MillisecondsSince(SteadyClock::time_point start)
{
    return std::chrono::duration<double,std::milli>(SteadyClock::now()-start).count();
}

struct ProductionEvaluatorAdapter {
    D3D12RendererOwner renderer;uint64_t successfulEvaluations{};
    TemporalGuideGenerator guides;
    uint32_t width{},height{};double fps{};
    NeuralRenderFailure lastFailure{NeuralRenderFailure::None};
    bool Initialize(HWND window,uint32_t w,uint32_t h,double rate,const GuideControls& controls){
        width=w;height=h;fps=rate;const auto [gridW,gridH]=TemporalGuideGenerator::AnalysisGrid(w,h,rate);
        renderer=MakeD3D12Renderer();
        if(!renderer||!renderer->Initialize(window,w,h,w,h,gridW,gridH,DefaultNeuralCarrierQuality()))return false;
        guides.SetControls(controls);renderer->SetDLSS(true);return true;
    }
    // The renderer resets NGX history when guide.id.reset != None (requested
    // reset or a cut detected by the guide generator) and stamps guide.id on
    // the capture, so the job can verify it received the frame it submitted.
    bool Submit(const JobFrame& frame,const FrameIdentity& id,bool capture,JobEvaluation& out){
        GuideFrame guide;const auto guideStart=SteadyClock::now();
        if(!guides.Generate(frame.bgra.data(),width,height,width,height,fps,id,guide)){
            lastFailure=NeuralRenderFailure::Neural;return false;
        }
        out.guideMs=MillisecondsSince(guideStart);
        const float frameMs=static_cast<float>(1000.0/fps);
        if(!capture){
            if(!renderer->RenderFrame(frame.bgra.data(),frame.bgra.size(),id,guide,frameMs)){
                lastFailure=ClassifyRendererFailure(*renderer);return false;
            }
            out.id=guide.id;
        }else{
            CapturedVideoFrame captured;const auto captureStart=SteadyClock::now();
            if(!renderer->RenderFrameForCache(frame.bgra.data(),frame.bgra.size(),id,guide,frameMs,captured)){
                lastFailure=ClassifyRendererFailure(*renderer);return false;
            }
            out.captureMs=MillisecondsSince(captureStart);
            out.bgra=std::move(captured.bgra);out.id=captured.id;
        }
        out.neuralGpuMs=renderer->LastNeuralGpuMs();++successfulEvaluations;return true;
    }
    bool FeatureCreated()const{return renderer&&renderer->DLSSFeatureCreated();}
    uint64_t EvaluationCount()const{return successfulEvaluations;}
    void ResetTemporal(){guides.Reset();}
    NeuralRenderFailure LastFailure()const{return lastFailure;}
    uint64_t PeakLocalVideoMemoryMiB()const{return renderer?renderer->PeakLocalVideoMemoryMiB():0;}
};

struct ProductionEncoderAdapter {
    RawVideoEncoder encoder;
    EncodeError Start(const EncoderSpec& spec,const std::filesystem::path& path){return encoder.Start(spec,path);}
    EncodeError WriteFrame(std::span<const uint8_t> frame,std::stop_token stop){return encoder.WriteFrame(frame,stop);}
    EncodeError Finish(std::stop_token stop){return encoder.Finish(stop);}
    void Cancel(){encoder.Cancel();}
};

std::filesystem::path ModuleDirectory()
{
    std::wstring path(32768,L'\0');const DWORD length=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    if(!length||length>=path.size())return {};path.resize(length);return std::filesystem::path(path).parent_path();
}

#endif

} // namespace

#ifndef OFFLINE_NEURAL_RENDERER_TESTING
std::string ReadNeuralRuntimeLogSegment(const std::filesystem::path& path,uintmax_t offset)
{
    constexpr uintmax_t Limit=4u*1024u*1024u;std::string latest;
    uintmax_t lastSegmentSize=std::numeric_limits<uintmax_t>::max();
    int stableSamples=0;
    for(int attempt=0;attempt<20;++attempt){
        std::error_code error;const auto size=std::filesystem::file_size(path,error);
        // A proxy that (re)creates the log after the offset was captured leaves
        // a smaller file. Read that fresh session from its start instead of
        // waiting for a segment that can never appear.
        const uintmax_t start=(!error&&size<offset)?0u:offset;
        if(!error&&size>=start&&size-start<=Limit){
            const uintmax_t segmentSize=size-start;
            std::ifstream input(path,std::ios::binary);
            if(input){input.seekg(static_cast<std::streamoff>(start));
                latest={std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
                const auto evidence=ParseNeuralRuntimeEvidence(latest);
                stableSamples=segmentSize==lastSegmentSize?stableSamples+1:1;
                lastSegmentSize=segmentSize;
                if((evidence.Valid()||evidence.laterFailure)&&stableSamples>=3)return latest;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return latest;
}
#endif


NeuralRuntimeEvidence ParseNeuralRuntimeEvidence(std::string_view reshadeLogSegment)
{
    NeuralRuntimeEvidence evidence;const std::string lower=LowerAscii(reshadeLogSegment);
    evidence.upscalingOff=lower.find("active settings: upscaling=off")!=std::string::npos;
    evidence.inlineInterceptionContract=
        lower.find("enablehooks=2: ngx hooks only")!=std::string::npos&&
        lower.find("private feature-18 gpu ordering active")!=std::string::npos;
    const size_t created=lower.find("feature 18 created");
    const size_t evaluated=lower.find("inline feature 18 evaluation succeeded");
    evidence.feature18Created=created!=std::string::npos;
    evidence.feature18Evaluated=evaluated!=std::string::npos;
    const std::array<std::string_view,9> failures{
        "feature 18 create failed","feature 18 evaluation failed",
        "inline feature 18 evaluation failed","feature 18 evaluate raised an exception",
        "nr skipped:","nr declined an evaluate:","nr workset pool exhausted",
        "the game dlss output was retained","nr is paused for this feature"};
    for(const auto failure:failures){
        if(lower.find(failure)!=std::string::npos)evidence.laterFailure=true;
    }
    evidence.highestObservedEvaluation=HighestEvaluationCount(lower);
    return evidence;
}

#ifdef OFFLINE_NEURAL_RENDERER_TESTING
OfflineNeuralRenderer::OfflineNeuralRenderer(
    IFrameSource& source,INeuralFrameEvaluator& evaluator,IFrameEncoder& encoder,
    std::function<std::string()> evidenceProvider,Clock clock,std::function<bool()> paused)
    : testSource_(&source),testEvaluator_(&evaluator),testEncoder_(&encoder),
      testEvidenceProvider_(std::move(evidenceProvider)),testClock_(std::move(clock)),
      testPaused_(std::move(paused)) {}
#endif

NeuralRenderResult OfflineNeuralRenderer::Run(const NeuralRenderRequest& request,
                                               ProgressCallback progress,std::stop_token stop)
{
#ifdef OFFLINE_NEURAL_RENDERER_TESTING
    if(!testSource_||!testEvaluator_||!testEncoder_||!testEvidenceProvider_)
        return NeuralRenderResult{.failure=NeuralRenderFailure::Protocol,
                                  .detail=L"Offline renderer test dependencies are incomplete."};
    TestSourceAdapter source{*testSource_};TestEvaluatorAdapter evaluator{*testEvaluator_};
    TestEncoderAdapter encoder{*testEncoder_};
    const Clock clock=testClock_?testClock_:[]{return SteadyClock::now();};
    const std::function<bool()> paused=testPaused_?testPaused_:[]{return false;};
    return RunJob(request,std::move(progress),stop,source,evaluator,encoder,
                  testEvidenceProvider_,clock,paused);
#else
    const auto logPath=ModuleDirectory()/L"ReShade.log";std::error_code error;
    uintmax_t logOffset=std::filesystem::file_size(logPath,error);if(error)logOffset=0;
    ProductionSourceAdapter source;ProductionEvaluatorAdapter evaluator;ProductionEncoderAdapter encoder;
    return RunJob(request,std::move(progress),stop,source,evaluator,encoder,
        [logPath,logOffset]{return ReadNeuralRuntimeLogSegment(logPath,logOffset);},
        []{return SteadyClock::now();},
        [pauseEvent=request.pauseEvent]{
            return pauseEvent&&WaitForSingleObject(pauseEvent,0)==WAIT_OBJECT_0;
        });
#endif
}
