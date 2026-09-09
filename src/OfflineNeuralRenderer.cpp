#include "OfflineNeuralRenderer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifndef OFFLINE_NEURAL_RENDERER_TESTING
#include "D3D12Renderer.h"
#include "TemporalGuides.h"
#include "VideoDecoder.h"
#include "DLSSBackend.h"
#include "Log.h"
#endif

namespace {

using SteadyClock = std::chrono::steady_clock;

// The export loop is one thread, so the stages below are disjoint and together account
// for all of its time. Whichever one dominates names the limiter, and the answer is
// otherwise pure guesswork: every stage waits on a different process or device.
//
//  * source  - the decoder child process, through the prefetch queue.
//  * submit  - CPU guide generation, the upload copies, and the D3D12 submission.
//  * resolve - parked on the GPU fence for a finished frame, plus the readback copy.
//  * write   - back pressure from the encoder child, i.e. ffmpeg cannot keep up.
struct StageTimers {
    SteadyClock::duration source{};
    SteadyClock::duration submit{};
    SteadyClock::duration resolve{};
    SteadyClock::duration write{};
};

class StageClock {
public:
    explicit StageClock(SteadyClock::duration& sink)
        : sink_(sink), start_(SteadyClock::now()) {}
    ~StageClock() { sink_ += SteadyClock::now() - start_; }
    StageClock(const StageClock&) = delete;
    StageClock& operator=(const StageClock&) = delete;
private:
    SteadyClock::duration& sink_;
    SteadyClock::time_point start_;
};

double MillisPerFrame(SteadyClock::duration total, uint64_t frames)
{
    if (!frames) return 0.0;
    const double micros = double(
        std::chrono::duration_cast<std::chrono::microseconds>(total).count());
    return micros / 1000.0 / double(frames);
}

double NanosPerFrameMillis(uint64_t nanos, uint64_t frames)
{
    if (!frames) return 0.0;
    return double(nanos) / 1.0e6 / double(frames);
}

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
    bool hasTimestamp{};
    int64_t firstTimestamp{};
    int64_t lastTimestamp{};
};

template <class F>
struct ScopeExit {
    F body;
    ~ScopeExit() { body(); }
};

// Reports the breakdown once per attempt, on every exit path including the failures.
// Adapters that can account for their own internals contribute a StageDetail suffix;
// the test adapters have nothing to add and are compiled past by the requires check.
template <class Evaluator>
void ReportStageTimings(EncoderKind kind, const StageTimers& stages,
                        const AttemptResult& attempt, const Evaluator& evaluator)
{
#ifndef OFFLINE_NEURAL_RENDERER_TESTING
    const uint64_t frames = attempt.frames;
    if (!frames) return;
    std::string detail;
    if constexpr (requires { evaluator.StageDetail(uint64_t{}); })
        detail = evaluator.StageDetail(frames);
    const SteadyClock::duration accounted =
        stages.source + stages.submit + stages.resolve + stages.write;
    std::ostringstream line;
    line << std::fixed << std::setprecision(2)
         << "Neural export stage cost per frame ("
         << (kind == EncoderKind::HevcNvenc ? "hevc_nvenc" : "libx264") << ", "
         << frames << " frames): total " << MillisPerFrame(accounted, frames)
         << " ms = source " << MillisPerFrame(stages.source, frames)
         << " + submit " << MillisPerFrame(stages.submit, frames)
         << " + resolve " << MillisPerFrame(stages.resolve, frames)
         << " + encoder back pressure " << MillisPerFrame(stages.write, frames)
         << " ms." << detail;
    LOG(line.str());
#else
    (void)kind;(void)stages;(void)attempt;(void)evaluator;
#endif
}

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
                source.Close();result.detail = L"Feature 18 could not be primed from the source.";return result;
            }
        } else {
            // A photo may need several presents to create feature 18. Reuse it
            // only for warm-up; capture below still reopens and reads it once.
            primingFrame.discontinuity = false;
        }
        std::vector<uint8_t> ignored;
        if (!evaluator.Submit(primingFrame, primed == 0 || primingFrame.discontinuity, false, ignored)) {
            source.Close();result.detail = L"Feature 18 priming failed.";return result;
        }
    }
    if (!evaluator.FeatureCreated()) {
        source.Close();result.detail = L"Feature 18 was not created.";return result;
    }
    const NeuralRuntimeEvidence armedEvidence=
        ParseNeuralRuntimeEvidence(evidenceProvider());
    if(!armedEvidence.Valid()){
        source.Close();
        result.detail=L"Feature 18 inline interception was not armed before frame capture.";
        return result;
    }
    result.feature18ArmedBeforeCapture=true;
    uint64_t successfulAttemptBaseline=armedEvidence.highestObservedEvaluation;

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
            EncoderSpec{request.width, request.height, request.fps, kind,
                        evaluator.CapturePixelFormat()},
            request.stagingVideoPath);
        if (startError != EncodeError::None) {
            attempt.failure = startError == EncodeError::Cancelled
                ? AttemptFailure::Cancelled : AttemptFailure::Encoder;
            attempt.encoderError = startError;return attempt;
        }
        StageTimers stages;
        if constexpr (requires { evaluator.ResetStageDetail(); }) evaluator.ResetStageDetail();
        const auto reportStages=[&]{ReportStageTimings(kind,stages,attempt,evaluator);};
        ScopeExit<decltype(reportStages)> reportOnExit{reportStages};
        bool temporalReset = true;
        // Submission runs ahead of encoding, so frame accounting has to be tracked
        // separately from what has actually been written out.
        uint64_t submitted = 0;
        bool haveSubmitTimestamp = false;
        int64_t lastSubmitTimestamp = 0;
        std::deque<int64_t> inFlight;
        evaluator.DiscardPending();

        // Waits on the oldest in-flight capture only, then hands its pixels to the
        // encoder thread. The buffer is recycled from a finished write so the full-frame
        // allocation and its zero-fill do not repeat every frame.
        auto drainOldest = [&]() -> bool {
            std::vector<uint8_t> pixels;
            if (!encoder.TakeRecycled(pixels)) pixels.clear();
            {
                StageClock clock(stages.resolve);
                if (!evaluator.ResolveOldest(pixels) || pixels.size() != expectedBytes) {
                    attempt.failure=AttemptFailure::Neural;encoder.Cancel();return false;
                }
            }
            const int64_t timestamp = inFlight.front();
            inFlight.pop_front();
            const size_t written = pixels.size();
            EncodeError writeError;
            {
                StageClock clock(stages.write);
                writeError = encoder.WriteFrameAsync(std::move(pixels), stop);
            }
            if (writeError != EncodeError::None) {
                attempt.failure = writeError == EncodeError::Cancelled
                    ? AttemptFailure::Cancelled : AttemptFailure::Encoder;
                attempt.encoderError=writeError;encoder.Cancel();return false;
            }
            ++attempt.frames;++attempt.evaluations;attempt.bytes+=written;
            if (!attempt.hasTimestamp) {
                attempt.firstTimestamp=timestamp;
                attempt.hasTimestamp=true;
            }
            attempt.lastTimestamp=timestamp;
            emit(NeuralRenderPhase::NeuralRendering,attempt.frames,attempt.bytes,true);
            return true;
        };

        for (;;) {
            if (stop.stop_requested()) {
                attempt.failure = AttemptFailure::Cancelled;attempt.encoderError=EncodeError::Cancelled;
                encoder.Cancel();return attempt;
            }
            JobFrame frame;
            JobRead read;
            {
                StageClock clock(stages.source);
                read = source.Read(frame, stop);
            }
            if (read == JobRead::EndOfStream) {
                // Report the source failure directly. Falling through to Finish here used
                // to overwrite it with the encoder error that cancelling produces.
                if(submitted==0){attempt.failure=AttemptFailure::Source;encoder.Cancel();return attempt;}
                break;
            }
            if (read == JobRead::Cancelled) {
                attempt.failure=AttemptFailure::Cancelled;attempt.encoderError=EncodeError::Cancelled;
                encoder.Cancel();return attempt;
            }
            if (read != JobRead::FrameReady) {
                attempt.failure=AttemptFailure::Source;encoder.Cancel();return attempt;
            }
            if (frame.timestamp100ns < 0 ||
                (haveSubmitTimestamp && frame.timestamp100ns <= lastSubmitTimestamp)) {
                attempt.failure=AttemptFailure::Source;encoder.Cancel();return attempt;
            }

            if (submitted == 0) {
                // The first frame stays synchronous: the runtime logs successful
                // evaluations sparsely, so it is captured repeatedly until a fresh
                // receipt exists, and that decision needs the pixels in hand. Each retry
                // has its own baseline, and these extra captures never extend the
                // timeline.
                std::vector<uint8_t> captured;
                for (uint64_t capture = 1; ; ++capture) {
                    if (stop.stop_requested()) {
                        attempt.failure=AttemptFailure::Cancelled;attempt.encoderError=EncodeError::Cancelled;
                        encoder.Cancel();return attempt;
                    }
                    const uint64_t before = evaluator.EvaluationCount();
                    captured.clear();
                    bool rendered;
                    {
                        StageClock clock(stages.submit);
                        rendered = evaluator.Submit(frame, temporalReset || (capture == 1 && frame.discontinuity), true, captured);
                    }
                    if (!rendered || evaluator.EvaluationCount() <= before ||
                        captured.size() != expectedBytes) {
                        attempt.failure=AttemptFailure::Neural;encoder.Cancel();return attempt;
                    }
                    temporalReset = false;
                    if (capture == 1 || capture % 10 == 0) {
                        const auto receipt = ParseNeuralRuntimeEvidence(evidenceProvider());
                        if (!receipt.Valid()) {
                            attempt.failure=AttemptFailure::Neural;encoder.Cancel();return attempt;
                        }
                        if (receipt.highestObservedEvaluation > successfulAttemptBaseline) break;
                    }
                    if (capture >= 120) {
                        attempt.failure=AttemptFailure::Neural;encoder.Cancel();return attempt;
                    }
                }
                const size_t written = captured.size();
                EncodeError writeError;
                {
                    StageClock clock(stages.write);
                    writeError = encoder.WriteFrameAsync(std::move(captured), stop);
                }
                if (writeError != EncodeError::None) {
                    attempt.failure = writeError == EncodeError::Cancelled
                        ? AttemptFailure::Cancelled : AttemptFailure::Encoder;
                    attempt.encoderError=writeError;encoder.Cancel();return attempt;
                }
                ++attempt.frames;++attempt.evaluations;attempt.bytes+=written;
                attempt.firstTimestamp=frame.timestamp100ns;attempt.hasTimestamp=true;
                attempt.lastTimestamp=frame.timestamp100ns;
                emit(NeuralRenderPhase::NeuralRendering,attempt.frames,attempt.bytes,true);
                ++submitted;haveSubmitTimestamp=true;lastSubmitTimestamp=frame.timestamp100ns;
                continue;
            }

            // Record the capture and move straight on to the next source frame. The GPU
            // keeps working while the previous frame is copied back and encoded.
            const uint64_t before = evaluator.EvaluationCount();
            {
                StageClock clock(stages.submit);
                if (!evaluator.SubmitAsync(frame, temporalReset || frame.discontinuity) ||
                    evaluator.EvaluationCount() <= before) {
                    attempt.failure=AttemptFailure::Neural;encoder.Cancel();return attempt;
                }
            }
            temporalReset = false;
            inFlight.push_back(frame.timestamp100ns);
            ++submitted;haveSubmitTimestamp=true;lastSubmitTimestamp=frame.timestamp100ns;
            if (evaluator.Pending() >= evaluator.MaxPending() && !drainOldest()) return attempt;
        }
        while (!inFlight.empty()) {
            if (stop.stop_requested()) {
                attempt.failure=AttemptFailure::Cancelled;attempt.encoderError=EncodeError::Cancelled;
                encoder.Cancel();return attempt;
            }
            if (!drainOldest()) return attempt;
        }
        const EncodeError finishError=encoder.Finish(stop);
        if(finishError!=EncodeError::None){
            attempt.failure=finishError==EncodeError::Cancelled
                ? AttemptFailure::Cancelled:AttemptFailure::Encoder;
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
        const NeuralRuntimeEvidence retryEvidence=
            ParseNeuralRuntimeEvidence(evidenceProvider());
        if(!retryEvidence.Valid()){
            result.detail=L"Feature 18 evidence was not valid before the software retry.";
            return result;
        }
        successfulAttemptBaseline=retryEvidence.highestObservedEvaluation;
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
    if(!result.evidence.Valid()||
       result.evidence.highestObservedEvaluation<=successfulAttemptBaseline){
        result.detail=L"Feature 18 runtime evidence did not advance after captured rendering or contained a later failure.";
        return result;
    }
    result.ok=true;result.encoder=selected;result.frameCount=attempt.frames;
    result.nativeEvaluations=attempt.evaluations;
    result.verifiedNeuralFrames=attempt.frames;
    const int64_t nominalFrameDuration=static_cast<int64_t>(
        std::llround(10000000.0/request.fps));
    result.duration100ns=attempt.lastTimestamp-attempt.firstTimestamp+nominalFrameDuration;
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
    // The test interface stays synchronous; these shims give RunJob the same async shape
    // the production adapter has, so the pipelined control flow is what the tests run.
    std::deque<std::vector<uint8_t>> captured{};
    bool Initialize(HWND window,uint32_t width,uint32_t height,double fps){return evaluator.Initialize(window,width,height,fps);}
    bool Submit(const JobFrame& frame,bool reset,bool capture,std::vector<uint8_t>& output){
        return evaluator.Submit(OfflineDecodedFrame{frame.bgra,frame.timestamp100ns,frame.discontinuity},
                                reset,capture,output);
    }
    bool SubmitAsync(const JobFrame& frame,bool reset){
        std::vector<uint8_t> output;
        if(!Submit(frame,reset,true,output))return false;
        captured.push_back(std::move(output));
        return true;
    }
    uint32_t Pending()const{return uint32_t(captured.size());}
    static constexpr uint32_t MaxPending(){return 2u;}
    bool ResolveOldest(std::vector<uint8_t>& pixels){
        if(captured.empty())return false;
        pixels=std::move(captured.front());captured.pop_front();return true;
    }
    void DiscardPending(){captured.clear();}
    static constexpr EncoderPixelFormat CapturePixelFormat(){return EncoderPixelFormat::Bgra;}
    bool FeatureCreated()const{return evaluator.FeatureCreated();}
    uint64_t EvaluationCount()const{return evaluator.EvaluationCount();}
    void ResetTemporal(){evaluator.ResetTemporal();}
};
struct TestEncoderAdapter {
    IFrameEncoder& encoder;
    std::vector<std::vector<uint8_t>> recycled{};
    EncodeError Start(const EncoderSpec& spec,const std::filesystem::path& path){
        recycled.clear();return encoder.Start(spec,path);
    }
    EncodeError WriteFrame(std::span<const uint8_t> frame,std::stop_token stop){return encoder.WriteFrame(frame,stop);}
    EncodeError WriteFrameAsync(std::vector<uint8_t>&& frame,std::stop_token stop){
        const EncodeError error=encoder.WriteFrame(frame,stop);
        if(recycled.size()<2)recycled.push_back(std::move(frame));
        return error;
    }
    bool TakeRecycled(std::vector<uint8_t>& buffer){
        if(recycled.empty())return false;
        buffer=std::move(recycled.back());recycled.pop_back();return true;
    }
    EncodeError Flush(std::stop_token){return EncodeError::None;}
    EncodeError Finish(std::stop_token stop){return encoder.Finish(stop);}
    void Cancel(){encoder.Cancel();}
};
#else
struct ProductionSourceAdapter {
    VideoDecoder decoder;
    bool Open(const std::filesystem::path& path,std::stop_token stop){
        return decoder.OpenSequential(path.wstring(),MediaSourceKind::LocalFile,stop);
    }
    void Close(){decoder.Close();}
    JobRead Read(JobFrame& frame,std::stop_token stop){
        VideoFrame decoded;
        const auto read = decoder.ReadNextBlocking(decoded, stop);
        frame = {std::move(decoded.bgra), decoded.timestamp100ns, decoded.discontinuity};
        if (read == VideoReadResult::FrameReady) return JobRead::FrameReady;
        if (read == VideoReadResult::EndOfStream) return JobRead::EndOfStream;
        if (read == VideoReadResult::Cancelled) return JobRead::Cancelled;
        return JobRead::Error;
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
    CapturedVideoFrame captureScratch;
    // Guide generation plus the DLSS evaluation. Shared by the synchronous and the
    // pipelined submit paths, which differ only in how the capture is read back.
    SteadyClock::duration guideCost{};
    bool Render(const JobFrame& frame,bool reset){
        GuideFrame guide;const bool temporalReset=forceReset||reset;forceReset=false;
        {
            StageClock clock(guideCost);
            if(!guides.Generate(frame.bgra.data(),width,height,width,height,fps,temporalReset,guide))return false;
        }
        const float frameMs=static_cast<float>(1000.0/fps);
        return renderer->RenderFrame(frame.bgra.data(),frame.bgra.size(),
            guide.guideGridRGBA32F.data(),guide.guideGridRGBA32F.size()*sizeof(float),
            guide.gridW,guide.gridH,temporalReset,frameMs);
    }

    // Splits the submit and resolve stages further. Guide generation is pure CPU work,
    // the fence wait is the GPU, and Present is DXGI pacing the hidden swapchain. Those
    // three have completely different fixes, and the outer stage totals cannot tell them
    // apart.
    void ResetStageDetail(){guideCost={};if(renderer)renderer->ResetStageCounters();}
    std::string StageDetail(uint64_t frames)const{
        if(!renderer||!frames)return {};
        std::ostringstream detail;
        detail<<std::fixed<<std::setprecision(2)
              <<" Of that: guides "<<MillisPerFrame(guideCost,frames)
              <<" ms, GPU fence wait "<<NanosPerFrameMillis(renderer->FenceWaitNanos(),frames)
              <<" ms, Present "<<NanosPerFrameMillis(renderer->PresentNanos(),frames)<<" ms.";
        return detail.str();
    }
    bool Submit(const JobFrame& frame,bool reset,bool capture,std::vector<uint8_t>& output){
        if(!Render(frame,reset))return false;
        if(!capture){++successfulEvaluations;return true;}
        captureScratch.pixels=std::move(output);
        const bool ok=renderer->CaptureRenderedFrame(captureScratch);
        output=std::move(captureScratch.pixels);captureScratch.pixels.clear();
        if(!ok)return false;
        ++successfulEvaluations;return true;
    }
    // Records the capture without waiting for the GPU. The pixels come back later from
    // ResolveOldest, which waits only on that one frame's fence.
    bool SubmitAsync(const JobFrame& frame,bool reset){
        if(!Render(frame,reset))return false;
        if(!renderer->EnqueueEvaluatedFrameCapture())return false;
        ++successfulEvaluations;return true;
    }
    uint32_t Pending()const{return renderer?renderer->PendingCaptureCount():0u;}
    static constexpr uint32_t MaxPending(){return D3D12Renderer::CaptureSlots;}
    bool ResolveOldest(std::vector<uint8_t>& pixels){
        if(!renderer)return false;
        captureScratch.pixels=std::move(pixels);
        const bool ok=renderer->ResolveOldestCapture(captureScratch);
        pixels=std::move(captureScratch.pixels);captureScratch.pixels.clear();
        return ok;
    }
    void DiscardPending(){
        if(!renderer)return;
        while(renderer->PendingCaptureCount()){
            CapturedVideoFrame discarded;
            if(!renderer->ResolveOldestCapture(discarded))break;
        }
    }
    // The cache render target is R8G8B8A8, so ffmpeg is told to consume RGBA and the
    // per-pixel channel swizzle that used to run on every readback disappears.
    static constexpr EncoderPixelFormat CapturePixelFormat(){return EncoderPixelFormat::Rgba;}
    bool FeatureCreated()const{return renderer&&renderer->DLSSFeatureCreated();}
    uint64_t EvaluationCount()const{return successfulEvaluations;}
    void ResetTemporal(){guides.Reset();forceReset=true;DiscardPending();}
};

// WriteFrame pushes a whole frame, over 30 MB at 4K, into ffmpeg's stdin and blocks
// whenever the encoder falls behind. Running it on a feeder thread is the missing half of
// the decoder's frame queue: decode, neural evaluation and encode now all overlap.
//
// The cost is that an encoder error surfaces up to QueueCapacity frames late. The NVENC
// to libx264 retry path already cancels and re-reads the source from frame zero, so it
// still recovers; it just wastes a couple more frames before noticing.
struct ProductionEncoderAdapter {
    RawVideoEncoder encoder;
    static constexpr size_t QueueCapacity=2;

    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<std::vector<uint8_t>> queue;
    std::vector<std::vector<uint8_t>> recycled;
    EncodeError latched{EncodeError::None};
    bool draining=false;
    std::jthread worker;

    ~ProductionEncoderAdapter(){StopWorker();}

    EncodeError Start(const EncoderSpec& spec,const std::filesystem::path& path){
        StopWorker();
        {
            std::lock_guard lock(mutex);
            queue.clear();recycled.clear();latched=EncodeError::None;draining=false;
        }
        const EncodeError started=encoder.Start(spec,path);
        if(started!=EncodeError::None)return started;
        worker=std::jthread([this](std::stop_token workerStop){WorkerLoop(workerStop);});
        return EncodeError::None;
    }

    EncodeError WriteFrameAsync(std::vector<uint8_t>&& frame,std::stop_token stop){
        std::unique_lock lock(mutex);
        cv.wait(lock,stop,[this]{return queue.size()<QueueCapacity||latched!=EncodeError::None;});
        if(latched!=EncodeError::None)return latched;
        if(stop.stop_requested())return EncodeError::Cancelled;
        if(!worker.joinable())return EncodeError::WriteFailed;
        queue.push_back(std::move(frame));
        lock.unlock();
        cv.notify_all();
        return EncodeError::None;
    }

    bool TakeRecycled(std::vector<uint8_t>& buffer){
        std::lock_guard lock(mutex);
        if(recycled.empty())return false;
        buffer=std::move(recycled.back());recycled.pop_back();return true;
    }

    EncodeError Flush(std::stop_token){
        {std::lock_guard lock(mutex);draining=true;}
        cv.notify_all();
        if(worker.joinable())worker.join();
        std::lock_guard lock(mutex);
        return latched;
    }

    EncodeError Finish(std::stop_token stop){
        const EncodeError flushed=Flush(stop);
        if(flushed!=EncodeError::None){encoder.Cancel();return flushed;}
        return encoder.Finish(stop);
    }

    void Cancel(){
        {
            std::lock_guard lock(mutex);
            draining=true;queue.clear();
            if(latched==EncodeError::None)latched=EncodeError::Cancelled;
        }
        cv.notify_all();
        // StopWorker first. It requests the worker's stop token, which is what unblocks a
        // write that is stuck on a pipe a stalled ffmpeg is not draining. Only once the
        // worker has been joined is it safe to tear the child down here.
        StopWorker();
        encoder.Cancel();
    }

private:
    void StopWorker(){
        {std::lock_guard lock(mutex);draining=true;}
        cv.notify_all();
        if(worker.joinable()){worker.request_stop();worker.join();}
        worker={};
    }

    void WorkerLoop(std::stop_token workerStop){
        for(;;){
            std::vector<uint8_t> frame;
            {
                std::unique_lock lock(mutex);
                if(!cv.wait(lock,workerStop,[this]{return !queue.empty()||draining;}))return;
                if(queue.empty())return;
                frame=std::move(queue.front());queue.pop_front();
            }
            cv.notify_all();
            // Cancellation rides on the worker's own stop token. RawVideoEncoder::WriteFrame
            // installs a stop_callback that terminates the ffmpeg job object, so requesting
            // it releases a blocked WriteFile. Tearing the child down from another thread
            // instead would pull the pipe handle out from under an in-flight write.
            const EncodeError error=encoder.WriteFrame(frame,workerStop);
            {
                std::lock_guard lock(mutex);
                if(error!=EncodeError::None&&latched==EncodeError::None)latched=error;
                if(recycled.size()<QueueCapacity+1)recycled.push_back(std::move(frame));
            }
            cv.notify_all();
            if(error!=EncodeError::None)return;
        }
    }
};

std::filesystem::path ModuleDirectory()
{
    std::wstring path(32768,L'\0');const DWORD length=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    if(!length||length>=path.size())return {};path.resize(length);return std::filesystem::path(path).parent_path();
}

std::string ReadLogSegment(const std::filesystem::path& path,uintmax_t offset)
{
    constexpr uintmax_t Limit=4u*1024u*1024u;std::string latest;
    uintmax_t lastSegmentSize=std::numeric_limits<uintmax_t>::max();
    int stableSamples=0;
    for(int attempt=0;attempt<20;++attempt){
        std::error_code error;const auto size=std::filesystem::file_size(path,error);
        if(!error&&size>=offset&&size-offset<=Limit){
            const uintmax_t segmentSize=size-offset;
            std::ifstream input(path,std::ios::binary);
            if(input){input.seekg(static_cast<std::streamoff>(offset));
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

} // namespace

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
