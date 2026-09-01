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
};

enum class AttemptFailure { None, Encoder, Neural, Source, Cancelled };

struct AttemptResult {
    AttemptFailure failure{AttemptFailure::None};
    EncodeError encoderError{EncodeError::None};
    uint64_t frames{};
    uint64_t bytes{};
    uint64_t evaluations{};
    int64_t lastTimestamp{};
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

template<class Source, class Evaluator, class Encoder, class Evidence, class Clock>
NeuralRenderResult RunJob(const NeuralRenderRequest& request,
                          OfflineNeuralRenderer::ProgressCallback progress,
                          std::stop_token stop, Source& source, Evaluator& evaluator,
                          Encoder& encoder, Evidence evidenceProvider, Clock clock)
{
    NeuralRenderResult result;
    if (request.sourcePath.empty() || request.stagingVideoPath.empty() ||
        !request.width || !request.height || !std::isfinite(request.fps) || request.fps <= 0.0 ||
        !std::isfinite(request.durationSeconds) || request.durationSeconds <= 0.0) {
        result.detail = L"Invalid neural render request.";
        return result;
    }
    const uint64_t totalFrames = std::max<uint64_t>(
        1, static_cast<uint64_t>(std::llround(request.durationSeconds * request.fps)));
    const uint64_t expectedBytes64 = uint64_t{request.width} * request.height * 4u;
    if (expectedBytes64 > std::numeric_limits<size_t>::max()) {
        result.detail = L"Neural render dimensions are too large.";
        return result;
    }
    const size_t expectedBytes = static_cast<size_t>(expectedBytes64);
    const auto started = clock();
    auto lastProgressTime = started;
    uint64_t reportedCompleted = 0;
    uint64_t reportedBytes = 0;
    double smoothedFramesPerMs = 0.0;
    NeuralRenderProgress previous{};
    auto emit = [&](NeuralRenderPhase phase, uint64_t completed, uint64_t bytes, bool frameTick) {
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
        if (progress) progress(snapshot);
        previous = snapshot;
    };
    auto cancelled = [&](std::wstring detail) {
        encoder.Cancel();source.Close();result.cancelled = true;result.detail = std::move(detail);
        return result;
    };

    emit(NeuralRenderPhase::Acquiring, 0, 0, false);
    if (stop.stop_requested()) return cancelled(L"Neural render was cancelled.");
    if (!source.Open(request.sourcePath, stop)) {
        if (stop.stop_requested()) return cancelled(L"Neural render was cancelled.");
        result.detail = L"The source video could not be opened.";return result;
    }
    emit(NeuralRenderPhase::Decoding, 0, 0, false);
    if (!evaluator.Initialize(request.renderWindow, request.width, request.height, request.fps)) {
        source.Close();result.detail = L"The neural renderer could not be initialized.";return result;
    }

    const uint64_t primeLimit = std::max<uint64_t>(2, std::min<uint64_t>(totalFrames, 120));
    uint64_t primed = 0;
    while (!evaluator.FeatureCreated() && primed < primeLimit) {
        if (stop.stop_requested()) return cancelled(L"Neural render was cancelled.");
        JobFrame frame;
        const JobRead read = source.Read(frame, stop);
        if (read == JobRead::Cancelled) return cancelled(L"Neural render was cancelled.");
        if (read != JobRead::FrameReady) {
            source.Close();result.detail = L"Feature 18 could not be primed from the source.";return result;
        }
        std::vector<uint8_t> ignored;
        if (!evaluator.Submit(frame, primed == 0 || frame.discontinuity, false, ignored)) {
            source.Close();result.detail = L"Feature 18 priming failed.";return result;
        }
        ++primed;
    }
    if (!evaluator.FeatureCreated()) {
        source.Close();result.detail = L"Feature 18 was not created.";return result;
    }

    auto reopenFromZero = [&] {
        source.Close();
        if (!source.Open(request.sourcePath, stop)) return false;
        evaluator.ResetTemporal();
        return true;
    };
    if (!reopenFromZero()) {
        if (stop.stop_requested()) return cancelled(L"Neural render was cancelled.");
        result.detail = L"The source could not be restarted from frame zero.";return result;
    }

    auto runAttempt = [&](EncoderKind kind) {
        AttemptResult attempt;
        const EncodeError startError = encoder.Start(
            EncoderSpec{request.width, request.height, request.fps, kind},
            request.stagingVideoPath);
        if (startError != EncodeError::None) {
            attempt.failure = startError == EncodeError::Cancelled
                ? AttemptFailure::Cancelled : AttemptFailure::Encoder;
            attempt.encoderError = startError;return attempt;
        }
        bool temporalReset = true;
        for (;;) {
            if (stop.stop_requested()) {
                attempt.failure = AttemptFailure::Cancelled;attempt.encoderError=EncodeError::Cancelled;
                encoder.Cancel();return attempt;
            }
            JobFrame frame;
            const JobRead read = source.Read(frame, stop);
            if (read == JobRead::EndOfStream) {
                if(attempt.frames==0){attempt.failure=AttemptFailure::Source;encoder.Cancel();}
                break;
            }
            if (read == JobRead::Cancelled) {
                attempt.failure=AttemptFailure::Cancelled;attempt.encoderError=EncodeError::Cancelled;
                encoder.Cancel();return attempt;
            }
            if (read != JobRead::FrameReady) {
                attempt.failure=AttemptFailure::Source;encoder.Cancel();return attempt;
            }
            const uint64_t before = evaluator.EvaluationCount();
            std::vector<uint8_t> captured;
            if (!evaluator.Submit(frame, temporalReset || frame.discontinuity, true, captured) ||
                evaluator.EvaluationCount() <= before || captured.size() != expectedBytes) {
                attempt.failure=AttemptFailure::Neural;encoder.Cancel();return attempt;
            }
            temporalReset = false;
            const EncodeError writeError = encoder.WriteFrame(captured, stop);
            if (writeError != EncodeError::None) {
                attempt.failure = writeError == EncodeError::Cancelled
                    ? AttemptFailure::Cancelled : AttemptFailure::Encoder;
                attempt.encoderError=writeError;encoder.Cancel();return attempt;
            }
            ++attempt.frames;++attempt.evaluations;attempt.bytes+=captured.size();
            attempt.lastTimestamp=frame.timestamp100ns;
            emit(NeuralRenderPhase::NeuralRendering,attempt.frames,attempt.bytes,true);
        }
        const EncodeError finishError=encoder.Finish(stop);
        if(finishError!=EncodeError::None){
            attempt.failure=finishError==EncodeError::Cancelled
                ? AttemptFailure::Cancelled:AttemptFailure::Encoder;
            attempt.encoderError=finishError;return attempt;
        }
        return attempt;
    };

    EncoderKind selected = EncoderKind::HevcNvenc;
    AttemptResult attempt = runAttempt(selected);
    if (attempt.failure == AttemptFailure::Cancelled)
        return cancelled(L"Neural render was cancelled.");
    if (attempt.failure == AttemptFailure::Encoder &&
        ShouldRetryWithSoftware(selected, attempt.encoderError)) {
        encoder.Cancel();
        std::error_code removeError;std::filesystem::remove(request.stagingVideoPath, removeError);
        if (!reopenFromZero()) {
            if (stop.stop_requested()) return cancelled(L"Neural render was cancelled.");
            result.detail=L"The source could not be restarted for software encoding.";return result;
        }
        selected=EncoderKind::H264Software;
        attempt=runAttempt(selected);
    }
    if (attempt.failure == AttemptFailure::Cancelled)
        return cancelled(L"Neural render was cancelled.");
    if (attempt.failure != AttemptFailure::None) {
        encoder.Cancel();source.Close();
        result.detail = attempt.failure == AttemptFailure::Neural
            ? L"A frame was not produced by feature 18."
            : attempt.failure == AttemptFailure::Source
                ? L"The source decoder failed during neural rendering."
                : L"The neural video encoder failed.";
        return result;
    }
    source.Close();
    emit(NeuralRenderPhase::Encoding,attempt.frames,attempt.bytes,false);
    emit(NeuralRenderPhase::Validating,attempt.frames,attempt.bytes,false);
    result.evidence=ParseNeuralRuntimeEvidence(evidenceProvider());
    if(!result.evidence.Valid()){
        result.detail=L"Feature 18 runtime evidence was incomplete or contained a later failure.";
        return result;
    }
    result.ok=true;result.encoder=selected;result.frameCount=attempt.frames;
    result.nativeEvaluations=attempt.evaluations;
    result.duration100ns=static_cast<int64_t>(std::llround(
        (double(attempt.frames)/request.fps)*10000000.0));
    emit(NeuralRenderPhase::Ready,attempt.frames,attempt.bytes,false);
    return result;
}

#ifdef OFFLINE_NEURAL_RENDERER_TESTING
struct TestSourceAdapter {
    IFrameSource& source;
    bool Open(const std::filesystem::path& path,std::stop_token stop){return source.Open(path,stop);}
    void Close(){source.Close();}
    JobRead Read(JobFrame& frame,std::stop_token stop){
        OfflineDecodedFrame decoded;const auto read=source.Read(decoded,stop);
        frame={std::move(decoded.bgra),decoded.timestamp100ns,decoded.discontinuity};
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
    bool Initialize(HWND window,uint32_t width,uint32_t height,double fps){return evaluator.Initialize(window,width,height,fps);}
    bool Submit(const JobFrame& frame,bool reset,bool capture,std::vector<uint8_t>& output){
        return evaluator.Submit(OfflineDecodedFrame{frame.bgra,frame.timestamp100ns,frame.discontinuity},
                                reset,capture,output);
    }
    bool FeatureCreated()const{return evaluator.FeatureCreated();}
    uint64_t EvaluationCount()const{return evaluator.EvaluationCount();}
    void ResetTemporal(){evaluator.ResetTemporal();}
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
    bool Open(const std::filesystem::path& path,std::stop_token stop){
        return decoder.Open(path.wstring(),MediaSourceKind::LocalFile,stop);
    }
    void Close(){decoder.Close();}
    JobRead Read(JobFrame& frame,std::stop_token stop){
        for(;;){
            VideoFrame decoded;const auto read=decoder.ReadNextAvailable(decoded,stop);
            if(read==VideoReadResult::NotReady){std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
            frame={std::move(decoded.bgra),decoded.timestamp100ns,decoded.discontinuity};
            if(read==VideoReadResult::FrameReady)return JobRead::FrameReady;
            if(read==VideoReadResult::EndOfStream)return JobRead::EndOfStream;
            if(read==VideoReadResult::Cancelled)return JobRead::Cancelled;
            return JobRead::Error;
        }
    }
};

struct ProductionEvaluatorAdapter {
    D3D12RendererOwner renderer;uint64_t successfulEvaluations{};
    TemporalGuideGenerator guides;
    uint32_t width{},height{};double fps{};bool forceReset{true};
    bool Initialize(HWND window,uint32_t w,uint32_t h,double rate){
        width=w;height=h;fps=rate;const auto [gridW,gridH]=TemporalGuideGenerator::AnalysisGrid(w,h,rate);
        renderer=MakeD3D12Renderer();
        if(!renderer||!renderer->Initialize(window,w,h,w,h,gridW,gridH,DefaultNeuralCarrierQuality()))return false;
        renderer->SetDLSS(true);return true;
    }
    bool Submit(const JobFrame& frame,bool reset,bool capture,std::vector<uint8_t>& output){
        GuideFrame guide;const bool temporalReset=forceReset||reset;forceReset=false;
        if(!guides.Generate(frame.bgra.data(),width,height,width,height,fps,temporalReset,guide))return false;
        const float frameMs=static_cast<float>(1000.0/fps);
        if(!capture){const bool ok=renderer->RenderFrame(frame.bgra.data(),frame.bgra.size(),
            guide.guideGridRGBA32F.data(),guide.guideGridRGBA32F.size()*sizeof(float),
            guide.gridW,guide.gridH,temporalReset,frameMs);if(ok)++successfulEvaluations;return ok;}
        CapturedVideoFrame captured;
        if(!renderer->RenderFrameForCache(frame.bgra.data(),frame.bgra.size(),
            guide.guideGridRGBA32F.data(),guide.guideGridRGBA32F.size()*sizeof(float),
            guide.gridW,guide.gridH,temporalReset,frameMs,captured))return false;
        output=std::move(captured.bgra);++successfulEvaluations;return true;
    }
    bool FeatureCreated()const{return renderer&&renderer->DLSSFeatureCreated();}
    uint64_t EvaluationCount()const{return successfulEvaluations;}
    void ResetTemporal(){guides.Reset();forceReset=true;}
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

std::string ReadLogSegment(const std::filesystem::path& path,uintmax_t offset)
{
    constexpr uintmax_t Limit=4u*1024u*1024u;std::string latest;
    for(int attempt=0;attempt<10;++attempt){
        std::error_code error;const auto size=std::filesystem::file_size(path,error);
        if(!error&&size>=offset&&size-offset<=Limit){
            std::ifstream input(path,std::ios::binary);
            if(input){input.seekg(static_cast<std::streamoff>(offset));
                latest={std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
                const auto evidence=ParseNeuralRuntimeEvidence(latest);
                if(evidence.Valid()||evidence.laterFailure)return latest;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return latest;
}
#endif

} // namespace

NeuralRuntimeEvidence ParseNeuralRuntimeEvidence(std::string_view reshadeLogSegment)
{
    NeuralRuntimeEvidence evidence;const std::string lower=LowerAscii(reshadeLogSegment);
    evidence.upscalingOff=lower.find("active settings: upscaling=off")!=std::string::npos;
    const size_t created=lower.find("feature 18 created");
    const size_t evaluated=lower.find("inline feature 18 evaluation succeeded");
    evidence.feature18Created=created!=std::string::npos;
    evidence.feature18Evaluated=evaluated!=std::string::npos;
    size_t firstSuccess=std::string::npos;
    if(evidence.feature18Created)firstSuccess=created;
    if(evidence.feature18Evaluated)firstSuccess=std::min(firstSuccess,evaluated);
    const std::array<std::string_view,3> failures{
        "feature 18 create failed","feature 18 evaluation failed","inline feature 18 evaluation failed"};
    for(const auto failure:failures){
        const size_t position=lower.find(failure);
        if(position!=std::string::npos&&firstSuccess!=std::string::npos&&position>firstSuccess)
            evidence.laterFailure=true;
    }
    evidence.highestObservedEvaluation=HighestEvaluationCount(lower);
    return evidence;
}

#ifdef OFFLINE_NEURAL_RENDERER_TESTING
OfflineNeuralRenderer::OfflineNeuralRenderer(
    IFrameSource& source,INeuralFrameEvaluator& evaluator,IFrameEncoder& encoder,
    std::function<std::string()> evidenceProvider,Clock clock)
    : testSource_(&source),testEvaluator_(&evaluator),testEncoder_(&encoder),
      testEvidenceProvider_(std::move(evidenceProvider)),testClock_(std::move(clock)) {}
#endif

NeuralRenderResult OfflineNeuralRenderer::Run(const NeuralRenderRequest& request,
                                               ProgressCallback progress,std::stop_token stop)
{
#ifdef OFFLINE_NEURAL_RENDERER_TESTING
    if(!testSource_||!testEvaluator_||!testEncoder_||!testEvidenceProvider_)
        return NeuralRenderResult{.detail=L"Offline renderer test dependencies are incomplete."};
    TestSourceAdapter source{*testSource_};TestEvaluatorAdapter evaluator{*testEvaluator_};
    TestEncoderAdapter encoder{*testEncoder_};
    const Clock clock=testClock_?testClock_:[]{return SteadyClock::now();};
    return RunJob(request,std::move(progress),stop,source,evaluator,encoder,
                  testEvidenceProvider_,clock);
#else
    const auto logPath=ModuleDirectory()/L"ReShade.log";std::error_code error;
    uintmax_t logOffset=std::filesystem::file_size(logPath,error);if(error)logOffset=0;
    ProductionSourceAdapter source;ProductionEvaluatorAdapter evaluator;ProductionEncoderAdapter encoder;
    return RunJob(request,std::move(progress),stop,source,evaluator,encoder,
        [logPath,logOffset]{return ReadLogSegment(logPath,logOffset);},
        []{return SteadyClock::now();});
#endif
}
