#include "OfflineNeuralRenderer.h"

#include "Log.h"
#include "PixelLayout.h"

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
#include <optional>
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

double Mean(const std::vector<double>& samples)
{
    if (samples.empty()) return 0.0;
    double total = 0.0;
    for (const double sample : samples) total += sample;
    return total / double(samples.size());
}

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

// Wall time of each render-loop stage, one sample per captured frame. Six
// clock reads per frame (~150 ns) is the only evidence that says which stage
// the 33.3 ms live budget actually goes to, so the collection stays in and the
// summary is logged once per attempt.
struct StageSamples {
    // Per source frame. read/guide/eval are measured in the iteration that submitted the
    // frame; render/write in the later iteration that drained its capture. Their sum is
    // what one frame costs, but no single iteration ever pays all of it.
    std::vector<double> read,guide,render,write,eval;
    // Per source frame: reading it off the source through to handing its pixels to the
    // encoder. That spans the whole pipeline, so it is roughly depth x loop time. It is a
    // latency and must never be read as a rate.
    std::vector<double> latency;
    void Push(double readMs, double guideMs, double renderMs, double evalMs, double writeMs,
              double latencyMs)
    {
        read.push_back(readMs);guide.push_back(guideMs);render.push_back(renderMs);
        eval.push_back(evalMs);write.push_back(writeMs);latency.push_back(latencyMs);
    }

    // Per render-loop iteration, and the only series that answers how fast the export
    // runs. These three advance together and are self-contained: `loop` is the
    // iteration's wall time, `loopDrain` is what draining an older frame's capture cost
    // inside it, and the residual is loop - read - eval - drain, which is genuinely
    // unaccounted work rather than the pipeline latency the per-frame series carries.
    std::vector<double> loop,loopDrain,loopResidual;
    void PushLoop(double loopMs, double readMs, double evalMs, double drainMs)
    {
        loop.push_back(loopMs);loopDrain.push_back(drainMs);
        loopResidual.push_back(loopMs - readMs - evalMs - drainMs);
    }
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
    StageSamples stages;
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
         << " + resolve wait " << MillisPerFrame(stages.resolve, frames)
         << " + encoder back pressure " << MillisPerFrame(stages.write, frames)
         // The stage accumulators and the loop clock are independent measurements of the
         // same rate. They should agree; a gap is work no stage is counting.
         << " ms, measured loop " << Mean(attempt.stages.loop)
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

double MillisecondsSince(SteadyClock::time_point start)
{
    return std::chrono::duration<double,std::milli>(SteadyClock::now()-start).count();
}

double Quantile(std::vector<double> samples, double q)
{
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t index = static_cast<size_t>(std::ceil(q * double(samples.size() - 1)));
    return samples[std::min(index, samples.size() - 1)];
}

// One line per stage, once per attempt: mean/p50/p95 plus the total the stage cost over
// the whole attempt. The table is in two halves, and mixing them is the mistake it is
// laid out to prevent. The per-frame rows say what a frame costs; `source frame latency`
// is how long one frame takes to cross a deliberately pipelined renderer, so it grows
// with pipeline depth and says nothing about throughput. The per-iteration rows below it
// are the throughput: `render-loop throughput` is the wall time of one loop iteration,
// and only its residual means work nothing else measured.
void LogStageTable(const StageSamples& stages)
{
    const auto row = [](const char* name, const std::vector<double>& samples) {
        double total = 0.0;
        for (const double sample : samples) total += sample;
        LOG("Render stage " << name << " n=" << samples.size()
            << " mean=" << Mean(samples) << " p50=" << Quantile(samples, 0.5)
            << " p95=" << Quantile(samples, 0.95) << " totalMs=" << total);
    };
    row("read", stages.read);
    row("guide", stages.guide);
    // The readback copy runs on the resolve worker, so this is not the copy's cost: it is
    // only the part of it that the decode, guide and submit it overlapped did not hide.
    row("resolve wait(copy not hidden)", stages.render);
    row("submit(guide+render+gate)", stages.eval);
    row("write", stages.write);
    row("source frame latency", stages.latency);
    row("render-loop throughput", stages.loop);
    row("loop drain(resolve+write)", stages.loopDrain);
    row("loop unaccounted", stages.loopResidual);
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
    timing.guideMsMean = Mean(attempt.stages.guide);
    timing.captureMsMean = Mean(attempt.stages.render);
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

// Segment files sit beside the staging video and are named from its stem:
// <staging>/neural-00000.mkv, neural-00001.mkv, ...
std::filesystem::path SegmentFilePath(const std::filesystem::path& stagingVideoPath, uint64_t index)
{
    std::wstring digits = std::to_wstring(index);
    if (digits.size() < 5) digits.insert(0, 5 - digits.size(), L'0');
    return stagingVideoPath.parent_path() /
           (stagingVideoPath.stem().wstring() + L"-" + digits + stagingVideoPath.extension().wstring());
}

// Captured frames queued for the writer thread. Deep enough to cover a freshly
// spawned ffmpeg bringing up its encoder (~60 ms of not draining its stdin once
// its CUDA context is created at spawn, ~110 ms before) without stalling the
// render loop, and bounded in bytes so a large frame size cannot balloon the
// queue. 64 MiB held 15 NV12 frames at 2578x1080, ~73 ms of a 4.9 ms loop, and
// the export measured one ~55 ms render-loop stall per 2 s segment on top of
// it; 128 MiB covers the start-up at the ~3 ms loop the decoder can now feed.
inline constexpr size_t kQueuedFrameBytes = size_t{128} << 20;

// Rotating encoder used only when request.segmentFrames > 0. The render loop
// only hands frames over: a private writer thread owns the file being written,
// rotates to the next one and pushes each full file to a private finalize
// thread, which muxes it, reaps its ffmpeg, publishes it through the sink in
// index order, and starts the following file's encoder ahead of time. So the
// render loop pays neither the process spawn (~140 ms), nor the stall a fresh
// ffmpeg takes before it drains its pipe (~110 ms per file), nor the per-frame
// pipe write. An encoder failure surfaces to the render loop on a later
// Write() or at Finish(), which fails the attempt exactly as a synchronous
// write error did.
template<class Encoder>
class SegmentWriter {
public:
    SegmentWriter(std::function<std::unique_ptr<Encoder>()> factory,
                  const std::filesystem::path& stagingVideoPath, uint32_t segmentFrames,
                  int64_t frameDuration100ns, NeuralSegmentSink sink, std::stop_token stop)
        : factory_(std::move(factory)), staging_(stagingVideoPath), segmentFrames_(segmentFrames),
          frameDuration_(frameDuration100ns), sink_(std::move(sink)), stop_(std::move(stop)) {}
    ~SegmentWriter() { Cancel(); }
    SegmentWriter(const SegmentWriter&) = delete;
    SegmentWriter& operator=(const SegmentWriter&) = delete;

    // Starts a fresh sequence at index 0. A software-encoder retry deletes every
    // file the previous attempt wrote and reuses its names, so a consumer that
    // sees index 0 again knows the earlier segments are gone.
    void BeginAttempt(const EncoderSpec& spec)
    {
        Cancel();
        const uint64_t previous = started_;
        for (uint64_t index = 0; index < previous; ++index) Remove(index);
        started_ = 0;written_ = 0;spec_ = spec;
        {
            std::lock_guard lock(mutex_);
            failure_ = EncodeError::None;quit_ = false;closing_ = false;writing_ = true;
        }
        if (previous && sink_.onRestart) sink_.onRestart();
        finalizer_ = std::jthread([this] { Finalize(); });
        writer_ = std::jthread([this] { WriteQueuedFrames(); });
        // Segment 0's encoder starts while the attempt prerolls, so the first
        // captured frame never waits for a spawn either.
        RequestWarm(0);
    }

    // Hands one captured frame to the writer thread. This blocks only when the
    // encoder has fallen kQueuedFrameBytes behind, which no longer happens for
    // an ffmpeg start-up stall.
    EncodeError Write(const JobFrame& frame, std::vector<uint8_t>&& bgra, std::stop_token stop)
    {
        const size_t bytes = bgra.size();
        std::unique_lock lock(mutex_);
        if (failure_ != EncodeError::None) return failure_;
        // One frame is always allowed through, however large it is.
        if (queuedBytes_ && queuedBytes_ + bytes > kQueuedFrameBytes) {
            space_.wait(lock, stop, [&] {
                return quit_ || failure_ != EncodeError::None ||
                       queuedBytes_ + bytes <= kQueuedFrameBytes;
            });
            if (stop.stop_requested() || quit_) return EncodeError::Cancelled;
            if (failure_ != EncodeError::None) return failure_;
        }
        frames_.push_back(Frame{std::move(bgra), frame.frameNumber, frame.timestamp100ns});
        queuedBytes_ += bytes;
        lock.unlock();
        work_.notify_all();
        return EncodeError::None;
    }

    bool TakeRecycled(std::vector<uint8_t>& buffer)
    {
        std::lock_guard lock(mutex_);
        if (recycled_.empty()) return false;
        buffer = std::move(recycled_.back());
        recycled_.pop_back();
        return true;
    }

    // Writes every queued frame, finalizes the last partial segment and waits
    // for every pending file, so no segment can ever be published after the
    // job's result.
    EncodeError Finish()
    {
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
        }
        work_.notify_all();ready_.notify_all();
        if (writer_.joinable()) writer_.join();
        ready_.notify_all();
        if (finalizer_.joinable()) finalizer_.join();
        DropWarm();
        std::lock_guard lock(mutex_);
        if (failure_ != EncodeError::None) return failure_;
        return frames_.empty() && queue_.empty() ? EncodeError::None : EncodeError::FinishFailed;
    }

    void Cancel()
    {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        work_.notify_all();ready_.notify_all();space_.notify_all();
        if (writer_.joinable()) writer_.join();
        if (finalizer_.joinable()) finalizer_.join();
        std::deque<Pending> abandoned;
        {
            std::lock_guard lock(mutex_);
            abandoned.swap(queue_);
            frames_.clear();queuedBytes_ = 0;
        }
        // Published files stay; only the unfinished ones are removed.
        for (auto& pending : abandoned) {
            pending.encoder->Cancel();
            Remove(pending.segment.index);
        }
        DropWarm();
        if (current_) DropCurrent();
    }

private:
    // Match the capture ring: more idle buffers would retain memory without allowing
    // another readback to be in flight.
    static constexpr size_t kRecycledFrameCapacity = 4;

    struct Frame {
        std::vector<uint8_t> bgra;
        uint64_t frameNumber{};
        int64_t timestamp100ns{};
    };
    struct Pending {
        std::unique_ptr<Encoder> encoder;
        NeuralRenderSegment segment;
    };

    // Writer thread: owns current_, written_ and the current segment's tally.
    void WriteQueuedFrames()
    {
        for (;;) {
            Frame frame;
            {
                std::unique_lock lock(mutex_);
                work_.wait(lock, [this] {
                    return quit_ || closing_ || !frames_.empty() ||
                           failure_ != EncodeError::None;
                });
                // Cancel() and the failure path clean up behind this thread.
                if (quit_ || failure_ != EncodeError::None) break;
                if (frames_.empty()) {
                    if (!closing_) continue;
                    lock.unlock();
                    if (current_) {
                        if (currentFrames_) Handoff();
                        else DropCurrent();
                    }
                    break;
                }
                frame = std::move(frames_.front());
                frames_.pop_front();
                queuedBytes_ -= frame.bgra.size();
            }
            space_.notify_all();
            const EncodeError error = WriteFrame(frame);
            if (error != EncodeError::None) {
                Fail(error);
                return;
            }
            {
                std::lock_guard lock(mutex_);
                if (recycled_.size() < kRecycledFrameCapacity)
                    recycled_.push_back(std::move(frame.bgra));
            }
        }
        {
            std::lock_guard lock(mutex_);
            writing_ = false;
        }
        ready_.notify_all();
    }

    EncodeError WriteFrame(const Frame& frame)
    {
        const uint64_t index = written_ / segmentFrames_;
        if (!current_ || index != currentIndex_) {
            const EncodeError rotated = Rotate(index, frame);
            if (rotated != EncodeError::None) return rotated;
        }
        const EncodeError error = current_->WriteFrame(frame.bgra, stop_);
        if (error != EncodeError::None) return error;
        ++written_;++currentFrames_;currentLast_ = frame.timestamp100ns;
        return EncodeError::None;
    }

    // Pointer swaps in the steady state: the encoder for `index` was started by
    // the finalize thread a whole segment ago. Only a job without a factory or
    // a start the finalize thread never reached pays a spawn here.
    EncodeError Rotate(uint64_t index, const Frame& frame)
    {
        std::unique_ptr<Encoder> next;
        const EncodeError warmError = ClaimWarm(index, next);
        if (warmError != EncodeError::None) return warmError;
        if (!next) {
            next = factory_ ? factory_() : nullptr;
            if (!next) return EncodeError::InvalidSpecification;
            NoteStarted(index);
            const EncodeError startError = next->Start(spec_, SegmentFilePath(staging_, index));
            if (startError != EncodeError::None) {
                next->Cancel();
                Remove(index);
                return startError;
            }
        }
        if (current_ && currentFrames_) Handoff();
        current_ = std::move(next);
        currentIndex_ = index;currentFrames_ = 0;
        currentFirstFrameNumber_ = frame.frameNumber;
        currentFirstTimestamp_ = frame.timestamp100ns;currentLast_ = frame.timestamp100ns;
        RequestWarm(index + 1);
        return EncodeError::None;
    }

    // Arms the encoder for `index` on the finalize thread. Rotate() always
    // claims exactly the index armed by the previous rotation, so an armed
    // encoder is never overwritten.
    void RequestWarm(uint64_t index)
    {
        if (!factory_) return;
        {
            std::lock_guard lock(mutex_);
            if (quit_ || failure_ != EncodeError::None) return;
            warmIndex_ = index;warmPending_ = true;warmError_ = EncodeError::None;
        }
        NoteStarted(index);
        ready_.notify_all();
    }

    // Takes the encoder armed for `index`. It waits only in the pathological
    // case where the finalize thread has not performed the start yet (a segment
    // shorter than one spawn, or a long mux queued ahead of it); `next` stays
    // empty when nothing was armed for this index and the caller must spawn it.
    EncodeError ClaimWarm(uint64_t index, std::unique_ptr<Encoder>& next)
    {
        std::unique_lock lock(mutex_);
        if (warmIndex_ == index && (warmPending_ || warm_ || warmError_ != EncodeError::None)) {
            warmDone_.wait(lock, [this] { return !warmPending_; });
            const EncodeError error = warmError_;
            warmError_ = EncodeError::None;
            next = std::move(warm_);
            if (error != EncodeError::None) return error;
        }
        if (!quit_) return EncodeError::None;
        // Cancelled while this file was armed: hand nothing back, and leave
        // neither a process nor a file behind for the cancel to trip over.
        lock.unlock();
        if (next) {
            next->Cancel();next.reset();
            Remove(index);
        }
        return EncodeError::Cancelled;
    }

    // Discards an armed encoder nobody will write to: kills its ffmpeg and
    // removes the file it opened, so neither an orphan process nor a stray
    // neural-NNNNN.mkv survives a finished or cancelled job. Both threads must
    // already be joined, so no start can land after this.
    void DropWarm()
    {
        std::unique_ptr<Encoder> warm;
        uint64_t index = 0;
        {
            std::lock_guard lock(mutex_);
            warm = std::move(warm_);index = warmIndex_;
            warmPending_ = false;warmError_ = EncodeError::None;
        }
        if (!warm) return;
        warm->Cancel();
        Remove(index);
    }

    // Every file the attempt may have opened, so a restart or a cancel deletes
    // it even if it was only armed.
    void NoteStarted(uint64_t index)
    {
        std::lock_guard lock(mutex_);
        started_ = std::max(started_, index + 1);
    }

    void Handoff()
    {
        NeuralRenderSegment segment;
        segment.index = currentIndex_;
        segment.firstFrameNumber = currentFirstFrameNumber_;
        segment.firstTimestamp100ns = currentFirstTimestamp_;
        segment.end100ns = currentLast_ + frameDuration_;
        segment.frameCount = currentFrames_;
        segment.fileName = SegmentFilePath(staging_, currentIndex_).filename().wstring();
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(Pending{std::move(current_), std::move(segment)});
        }
        ready_.notify_all();
        current_.reset();currentFrames_ = 0;
    }

    void DropCurrent()
    {
        current_->Cancel();
        Remove(currentIndex_);
        current_.reset();currentFrames_ = 0;
    }

    // A half-written file is not a segment: the render loop learns of the error
    // on its next Write() or at Finish(), and nothing is published.
    void Fail(EncodeError error)
    {
        std::deque<Frame> dropped;
        {
            std::lock_guard lock(mutex_);
            failure_ = error;writing_ = false;
            dropped.swap(frames_);queuedBytes_ = 0;
        }
        space_.notify_all();ready_.notify_all();
        if (current_) DropCurrent();
    }

    void Finalize()
    {
        for (;;) {
            Pending pending;
            uint64_t warmIndex = 0;
            bool startWarm = false;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] {
                    return quit_ || !queue_.empty() || warmPending_ || (closing_ && !writing_);
                });
                // A finished file wins over arming the next one: a consumer is
                // waiting on the file, nobody waits on the spawn. Arming keeps
                // working while closing, because draining the queued frames can
                // still cross a segment boundary.
                if (!quit_ && !queue_.empty()) {
                    pending = std::move(queue_.front());
                    queue_.pop_front();
                } else if (!quit_ && warmPending_) {
                    startWarm = true;warmIndex = warmIndex_;
                } else {
                    // Leaving an armed request outstanding would block a
                    // Rotate() waiting on it; an empty slot only means the
                    // writer thread spawns that encoder itself.
                    warmPending_ = false;
                    lock.unlock();
                    warmDone_.notify_all();
                    return;
                }
            }
            if (startWarm) {
                std::unique_ptr<Encoder> warm = factory_();
                const EncodeError error = warm
                    ? warm->Start(spec_, SegmentFilePath(staging_, warmIndex))
                    : EncodeError::InvalidSpecification;
                if (error != EncodeError::None) {
                    if (warm) warm->Cancel();
                    warm.reset();
                    Remove(warmIndex);
                }
                {
                    std::lock_guard lock(mutex_);
                    warm_ = std::move(warm);warmError_ = error;warmPending_ = false;
                }
                warmDone_.notify_all();
                continue;
            }
            // The mux and process exit happen here, off the render loop.
            const EncodeError error = pending.encoder->Finish(stop_);
            if (error != EncodeError::None) {
                pending.encoder->Cancel();
                Remove(pending.segment.index);
                {
                    std::lock_guard lock(mutex_);
                    failure_ = error;
                    // A Rotate() blocked on an armed start must see the failure
                    // rather than wait for a thread that is exiting.
                    if (warmPending_) {warmPending_ = false;warmError_ = error;}
                }
                space_.notify_all();warmDone_.notify_all();work_.notify_all();
                return;
            }
            if (sink_.onSegment) sink_.onSegment(pending.segment);
        }
    }

    void Remove(uint64_t index) const
    {
        std::error_code error;
        std::filesystem::remove(SegmentFilePath(staging_, index), error);
    }

    std::function<std::unique_ptr<Encoder>()> factory_;
    std::filesystem::path staging_;
    uint32_t segmentFrames_{};
    int64_t frameDuration_{};
    NeuralSegmentSink sink_;
    std::stop_token stop_;
    EncoderSpec spec_{};
    std::unique_ptr<Encoder> current_;
    uint64_t currentIndex_{},currentFrames_{},currentFirstFrameNumber_{};
    int64_t currentFirstTimestamp_{},currentLast_{};
    uint64_t written_{};
    std::mutex mutex_;
    std::condition_variable work_;
    std::condition_variable ready_;
    std::condition_variable warmDone_;
    // The render loop waits here, and only here, when the encoder falls behind.
    std::condition_variable_any space_;
    std::deque<Frame> frames_;
    std::vector<std::vector<uint8_t>> recycled_;
    size_t queuedBytes_{};
    std::deque<Pending> queue_;
    std::jthread finalizer_;
    std::jthread writer_;
    // The encoder for the next file, started ahead of time by the finalizer.
    std::unique_ptr<Encoder> warm_;
    uint64_t warmIndex_{},started_{};
    EncodeError warmError_{EncodeError::None};
    EncodeError failure_{EncodeError::None};
    bool warmPending_{},quit_{},closing_{},writing_{};
};

// One-frame lookahead over the source. A private thread runs the decoder while
// the render thread has the previous frame on the GPU, which is the only
// overlap the loop can take without changing what it does: the thread hands
// over exactly what source.Read() produced, in the order it produced it, and
// the render loop still classifies every status, validates every timestamp and
// checks every identity itself.
//
// This layer adds one queued BGRA frame beyond the frame being rendered. The
// production VideoDecoder has its own four-frame queue; those buffers are not
// included here. A cancel stays prompt because this thread is never more than
// one source.Read() ahead, and that read observes the job's stop token.
template<class Source>
class FramePrefetch {
public:
    FramePrefetch(Source& source, std::stop_token stop)
        : source_(source), stop_(std::move(stop))
    {
        try {
            worker_ = std::jthread([this] { Decode(); });
        } catch (const std::system_error& error) {
            // No thread: read on the render thread, exactly as before.
            LOG("Source prefetch thread could not start; decoding synchronously. error="
                << error.code().value());
        }
    }
    ~FramePrefetch() { Stop(); }
    FramePrefetch(const FramePrefetch&) = delete;
    FramePrefetch& operator=(const FramePrefetch&) = delete;

    // The caller passes the same JobFrame every iteration, so the buffer it still
    // owns is the one the loop has finished with. It travels back to the decode
    // thread here and reaches the decoder's pool through the source adapter,
    // which is what keeps the read path from allocating a frame per frame.
    JobRead Next(JobFrame& frame)
    {
        if (!worker_.joinable()) return source_.Read(frame, stop_);
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return item_.has_value() || terminal_.has_value(); });
        if (!item_) return *terminal_;  // the decoder has nothing left to give
        const JobRead read = item_->read;
        std::vector<uint8_t> spent = std::move(frame.bgra);
        frame = std::move(item_->frame);
        item_.reset();
        spent_ = std::move(spent);
        lock.unlock();
        space_.notify_one();
        return read;
    }

private:
    struct Item {
        JobRead read{};
        JobFrame frame;
    };

    void Decode()
    {
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                space_.wait(lock, [this] { return !item_ || quit_; });
                if (quit_) return;
            }
            Item item;
            {
                std::lock_guard lock(mutex_);
                item.frame.bgra = std::move(spent_);
                spent_.clear();
            }
            // A throwing read is the loop's failure to classify, not this
            // thread's to swallow: it becomes the Error the loop would have
            // seen, and the decoder is not touched again.
            try {
                item.read = source_.Read(item.frame, stop_);
            } catch (...) {
                item.read = JobRead::Error;
            }
            const JobRead read = item.read;
            {
                std::lock_guard lock(mutex_);
                if (quit_) return;
                if (read != JobRead::FrameReady) terminal_ = read;
                item_ = std::move(item);
            }
            ready_.notify_one();
            if (read != JobRead::FrameReady) return;
        }
    }

    void Stop()
    {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        space_.notify_all();ready_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    Source& source_;
    std::stop_token stop_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable space_;
    std::optional<Item> item_;
    // The single buffer in transit from the render loop back to the decoder.
    std::vector<uint8_t> spent_;
    std::optional<JobRead> terminal_;
    bool quit_{};
    std::jthread worker_;
};

template<class Source, class Evaluator, class Encoder, class Evidence, class Clock, class Paused>
NeuralRenderResult RunJob(const NeuralRenderRequest& request,
                          OfflineNeuralRenderer::ProgressCallback progress,
                          std::stop_token stop, Source& source, Evaluator& evaluator,
                          Encoder& encoder, Evidence evidenceProvider, Clock clock, Paused paused,
                          const NeuralSegmentSink& segments)
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
    // Replaced once the evaluator has settled on a capture format: a GPU-converted
    // capture is NV12, which is 1.5 bytes per pixel rather than 4.
    size_t expectedBytes = static_cast<size_t>(expectedBytes64);
    // A segmented job publishes finalized files while it renders; segmentFrames
    // == 0 keeps the single staging file and never starts a finalize thread.
    std::optional<SegmentWriter<Encoder>> writer;
    if (request.segmentFrames) {
        writer.emplace([&encoder] { return encoder.Create(); }, request.stagingVideoPath,
                       request.segmentFrames, frameDuration, segments, stop);
    }
    auto cancelOutput = [&] { if (writer) writer->Cancel(); else encoder.Cancel(); };
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
        cancelOutput();source.Close();result.cancelled = true;
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
                              request.guides, source.Layout())) {
        source.Close();
        return fail(NeuralRenderFailure::Neural, L"The neural renderer could not be initialized.");
    }
    expectedBytes = static_cast<size_t>(
        EncoderFrameBytes(evaluator.CapturePixelFormat(), request.width, request.height));

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
        EncoderSpec spec{request.width, request.height, request.fps, kind,
                         evaluator.CapturePixelFormat()};
        spec.nvencPreset = request.nvencPreset;
        if (writer) {
            // Segment 0's encoder is armed here and starts while this attempt
            // prerolls, so the first captured frame never waits for a spawn.
            writer->BeginAttempt(spec);
        } else {
            const EncodeError startError = encoder.Start(spec, request.stagingVideoPath);
            if (startError != EncodeError::None) {
                attempt.failure = startError == EncodeError::Cancelled
                    ? NeuralRenderFailure::Cancelled : NeuralRenderFailure::Encoder;
                attempt.encoderError = startError;return attempt;
            }
        }
        StageTimers stages;
        if constexpr (requires { evaluator.ResetStageDetail(); }) evaluator.ResetStageDetail();
        const auto reportStages=[&]{ReportStageTimings(kind,stages,attempt,evaluator);};
        ScopeExit<decltype(reportStages)> reportOnExit{reportStages};
        // Submission runs ahead of encoding, so frame accounting has to be tracked
        // separately from what has actually been written out.
        struct InFlightCapture {
            uint64_t frameNumber{};
            int64_t timestamp100ns{};
            double readMs{},guideMs{},evalMs{},neuralGpuMs{};
            SteadyClock::time_point frameStart{};
        };
        std::deque<InFlightCapture> inFlight;
        uint64_t submitted = 0;
        evaluator.DiscardPending();
        // What draining an older frame's capture cost inside the current iteration. The
        // drain belongs to the iteration that runs it, not to the frame it drains, and
        // keeping the two apart is what stops pipeline latency leaking into the
        // throughput numbers. Reset at the top of every iteration.
        double iterationDrainMs = 0.0;

        // Waits on the oldest in-flight capture only, then hands its pixels to the
        // segment writer or the encoder's feeder thread. The buffer is recycled from a
        // finished write so the full-frame allocation and its zero-fill do not repeat
        // every frame.
        auto drainOldest = [&]() -> NeuralRenderFailure {
            const InFlightCapture queued = inFlight.front();
            std::vector<uint8_t> pixels;
            const bool recycled = writer ? writer->TakeRecycled(pixels)
                                         : encoder.TakeRecycled(pixels);
            if (!recycled) pixels.clear();
            double captureMs = 0.0;
            {
                StageClock clock(stages.resolve);
                if (!evaluator.ResolveOldest(pixels, captureMs) || pixels.size() != expectedBytes) {
                    return evaluatorFailure();
                }
            }
            inFlight.pop_front();
            const size_t written = pixels.size();
            double writeMs = 0.0;
            EncodeError writeError;
            {
                StageClock clock(stages.write);
                const auto writeStart = SteadyClock::now();
                JobFrame frameMeta;
                frameMeta.frameNumber = queued.frameNumber;
                frameMeta.timestamp100ns = queued.timestamp100ns;
                writeError = writer ? writer->Write(frameMeta, std::move(pixels), stop)
                                    : encoder.WriteFrameAsync(std::move(pixels), stop);
                writeMs = MillisecondsSince(writeStart);
            }
            if (writeError != EncodeError::None) {
                attempt.encoderError=writeError;
                return writeError == EncodeError::Cancelled
                    ? NeuralRenderFailure::Cancelled : NeuralRenderFailure::Encoder;
            }
            ++attempt.frames;++attempt.evaluations;attempt.bytes+=written;
            if (!attempt.hasTimestamp) {
                attempt.firstTimestamp=queued.timestamp100ns;
                attempt.hasTimestamp=true;
            }
            attempt.lastTimestamp=queued.timestamp100ns;
            attempt.neuralGpuMs.push_back(queued.neuralGpuMs);
            emit(NeuralRenderPhase::NeuralRendering,attempt.frames,attempt.bytes,true);
            attempt.stages.Push(queued.readMs, queued.guideMs, captureMs, queued.evalMs, writeMs,
                                MillisecondsSince(queued.frameStart));
            return NeuralRenderFailure::None;
        };
        auto abort = [&](NeuralRenderFailure failure) {
            // Frames already captured but not yet read back are valid output: write
            // them out so a failed attempt never omits a frame it successfully
            // evaluated. A cancelled job discards them instead, which keeps
            // cancellation prompt.
            if (failure != NeuralRenderFailure::Cancelled) {
                while (!inFlight.empty() && drainOldest() == NeuralRenderFailure::None) {}
            }
            attempt.failure = failure;
            if (failure == NeuralRenderFailure::Cancelled) attempt.encoderError = EncodeError::Cancelled;
            cancelOutput();
        };
        // Submits one frame, retrying the exact same frame on neural/GPU-stall
        // failures. The last permitted retry resets history; a frame that still
        // fails ends the attempt (never skipped). Device loss is not retried.
        // `pipelined` records the capture without waiting for the GPU: the pixels
        // come back from drainOldest, which waits only on that frame's fence.
        auto evaluate = [&](const JobFrame& frame, HistoryReset reason, bool capture,
                            bool pipelined, JobEvaluation& out) {
            FrameIdentity id = identity(frame, reason);
            for (uint32_t retry = 0;; ++retry) {
                if (stop.stop_requested()) return NeuralRenderFailure::Cancelled;
                const uint64_t before = evaluator.EvaluationCount();
                // Keep the pixel buffer's capacity across retries: the receipt gate
                // resubmits the same frame up to 120 times, and re-growing a full
                // frame each time dominates the retry cost.
                std::vector<uint8_t> recycledPixels = std::move(out.bgra);
                out = JobEvaluation{};
                out.bgra = std::move(recycledPixels);
                bool ok;
                {
                    StageClock clock(stages.submit);
                    ok = pipelined
                        ? evaluator.SubmitAsync(frame, id, out) &&
                              evaluator.EvaluationCount() > before
                        : evaluator.Submit(frame, id, capture, out) &&
                              evaluator.EvaluationCount() > before &&
                              (!capture || out.bgra.size() == expectedBytes);
                }
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
        // Joined on every exit from the attempt, so no thread is left holding
        // the decoder when the caller closes or reopens it.
        FramePrefetch<Source> prefetch(source, stop);
        // Lives across iterations only so its BGRA buffer can be handed back to the
        // decoder on the next read. Every read assigns the whole frame, so nothing
        // from the previous iteration survives into this one.
        JobFrame frame;
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
            const auto frameStart = SteadyClock::now();
            iterationDrainMs = 0.0;
            JobRead read;
            {
                StageClock clock(stages.source);
                read = prefetch.Next(frame);
            }
            // What the loop still pays for the decode: the residual wait for a
            // frame the decoder started while the previous one was on the GPU.
            const double readMs = MillisecondsSince(frameStart);
            if (read == JobRead::EndOfStream) {
                // Report the source failure directly. Falling through to Finish here used
                // to overwrite it with the encoder error that cancelling produces.
                if (submitted == 0) {abort(NeuralRenderFailure::Source);return attempt;}
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
                const NeuralRenderFailure failure = evaluate(frame, reason, false, false, ignored);
                if (failure != NeuralRenderFailure::None) {abort(failure);return attempt;}
                prerollEvaluated = true;
                continue;
            }
            if (boundedEnd && frame.timestamp100ns >= rangeEnd) {
                if (submitted == 0) {abort(NeuralRenderFailure::Source);return attempt;}
                break;
            }
            const auto evalStart = SteadyClock::now();
            JobEvaluation evaluation;
            const bool pipelined = submitted > 0;
            for (uint64_t capture = 1; ; ++capture) {
                const HistoryReset reason = capture > 1 ? HistoryReset::None
                    : (attempt.frames == 0 && !prerollEvaluated) ? HistoryReset::FirstFrame
                    : frame.discontinuity ? HistoryReset::SourceChange : HistoryReset::None;
                const NeuralRenderFailure failure = evaluate(frame, reason, true, pipelined, evaluation);
                if (failure != NeuralRenderFailure::None) {abort(failure);return attempt;}
                if (pipelined) break;
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
            const double evalMs = MillisecondsSince(evalStart);
            if (pipelined) {
                // Record the capture and move straight on to the next source frame. The GPU
                // keeps working while the previous frame is copied back and encoded.
                InFlightCapture queued;
                queued.frameNumber = frame.frameNumber;
                queued.timestamp100ns = frame.timestamp100ns;
                queued.readMs = readMs;queued.guideMs = evaluation.guideMs;
                queued.evalMs = evalMs;queued.neuralGpuMs = evaluation.neuralGpuMs;
                queued.frameStart = frameStart;
                inFlight.push_back(std::move(queued));
                ++submitted;
                if (evaluator.Pending() >= evaluator.MaxPending()) {
                    const auto drainStart = SteadyClock::now();
                    const NeuralRenderFailure failure = drainOldest();
                    iterationDrainMs += MillisecondsSince(drainStart);
                    if (failure != NeuralRenderFailure::None) {abort(failure);return attempt;}
                }
                attempt.stages.PushLoop(MillisecondsSince(frameStart), readMs, evalMs,
                                        iterationDrainMs);
                continue;
            }
            // The first captured frame stays synchronous: the receipt gate above needs
            // the pixels in hand, and they are written here once the gate opens. A
            // segmented job takes ownership of them; the single-file encoder hands
            // them to its feeder thread.
            const uint64_t captured = evaluation.bgra.size();
            double writeMs = 0.0;
            EncodeError writeError;
            {
                StageClock clock(stages.write);
                const auto writeStart = SteadyClock::now();
                writeError = writer ? writer->Write(frame, std::move(evaluation.bgra), stop)
                                    : encoder.WriteFrameAsync(std::move(evaluation.bgra), stop);
                writeMs = MillisecondsSince(writeStart);
            }
            if (writeError != EncodeError::None) {
                attempt.encoderError=writeError;
                abort(writeError == EncodeError::Cancelled
                    ? NeuralRenderFailure::Cancelled : NeuralRenderFailure::Encoder);
                return attempt;
            }
            ++attempt.frames;++attempt.evaluations;attempt.bytes+=captured;
            if (!attempt.hasTimestamp) {
                attempt.firstTimestamp=frame.timestamp100ns;
                attempt.hasTimestamp=true;
            }
            attempt.lastTimestamp=frame.timestamp100ns;
            attempt.neuralGpuMs.push_back(evaluation.neuralGpuMs);
            emit(NeuralRenderPhase::NeuralRendering,attempt.frames,attempt.bytes,true);
            attempt.stages.Push(readMs, evaluation.guideMs, evaluation.captureMs, evalMs, writeMs,
                                MillisecondsSince(frameStart));
            // Nothing is pipelined yet on this frame: it captured synchronously inside
            // evalMs, so only the write is left to count as this iteration's drain.
            attempt.stages.PushLoop(MillisecondsSince(frameStart), readMs, evalMs, writeMs);
            ++submitted;
        }
        while (!inFlight.empty()) {
            if (stop.stop_requested()) {abort(NeuralRenderFailure::Cancelled);return attempt;}
            const NeuralRenderFailure failure = drainOldest();
            if (failure != NeuralRenderFailure::None) {abort(failure);return attempt;}
        }
        LogStageTable(attempt.stages);
        const EncodeError finishError=writer?writer->Finish():encoder.Finish(stop);
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
        cancelOutput();
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
        cancelOutput();source.Close();
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
    // The test source hands out BGRA; only the production decoder can choose NV12.
    PixelLayout Layout()const{return PixelLayout::Bgra;}
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
    // The test interface stays synchronous; these shims give RunJob the same async shape
    // the production adapter has, so the pipelined control flow is what the tests run.
    std::deque<JobEvaluation> captured{};
    bool Initialize(HWND window,uint32_t width,uint32_t height,double fps,const GuideControls& guides,PixelLayout){
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
    bool SubmitAsync(const JobFrame& frame,const FrameIdentity& id,JobEvaluation& out){
        if(!Submit(frame,id,true,out))return false;
        captured.push_back(std::move(out));
        return true;
    }
    uint32_t Pending()const{return uint32_t(captured.size());}
    static constexpr uint32_t MaxPending(){return 2u;}
    bool ResolveOldest(std::vector<uint8_t>& pixels,double& captureMs){
        if(captured.empty())return false;
        pixels=std::move(captured.front().bgra);captured.pop_front();
        captureMs=0.0;return true;
    }
    void DiscardPending(){captured.clear();}
    static constexpr EncoderPixelFormat CapturePixelFormat(){return EncoderPixelFormat::Bgra;}
    bool FeatureCreated()const{return evaluator.FeatureCreated();}
    uint64_t EvaluationCount()const{return evaluator.EvaluationCount();}
    void ResetTemporal(){evaluator.ResetTemporal();}
    NeuralRenderFailure LastFailure()const{return evaluator.LastFailure();}
    uint64_t PeakLocalVideoMemoryMiB()const{return evaluator.PeakLocalVideoMemoryMiB();}
};
struct TestEncoderAdapter {
    IFrameEncoder* encoder{};
    std::unique_ptr<IFrameEncoder> owned;
    std::function<std::unique_ptr<IFrameEncoder>()> factory;
    std::vector<std::vector<uint8_t>> recycled{};
    EncodeError Start(const EncoderSpec& spec,const std::filesystem::path& path){
        recycled.clear();return encoder->Start(spec,path);
    }
    EncodeError WriteFrame(std::span<const uint8_t> frame,std::stop_token stop){return encoder->WriteFrame(frame,stop);}
    // The single-file write path feeds the real encoder from a worker thread; the
    // test shim writes synchronously and recycles the buffer exactly like production.
    EncodeError WriteFrameAsync(std::vector<uint8_t>&& frame,std::stop_token stop){
        const EncodeError error=encoder->WriteFrame(frame,stop);
        if(recycled.size()<2)recycled.push_back(std::move(frame));
        return error;
    }
    bool TakeRecycled(std::vector<uint8_t>& buffer){
        if(recycled.empty())return false;
        buffer=std::move(recycled.back());recycled.pop_back();return true;
    }
    EncodeError Flush(std::stop_token){return EncodeError::None;}
    EncodeError Finish(std::stop_token stop){return encoder->Finish(stop);}
    void Cancel(){encoder->Cancel();}
    std::unique_ptr<TestEncoderAdapter> Create(){
        std::unique_ptr<IFrameEncoder> made=factory?factory():nullptr;
        if(!made)return {};
        auto adapter=std::make_unique<TestEncoderAdapter>();
        adapter->encoder=made.get();adapter->owned=std::move(made);adapter->factory=factory;
        return adapter;
    }
};
#else
struct ProductionSourceAdapter {
    VideoDecoder decoder;
    bool Open(const std::filesystem::path& path,std::stop_token stop,double seekSeconds){
        if(!decoder.OpenSequential(path.wstring(),MediaSourceKind::LocalFile,stop))return false;
        return seekSeconds<=0.0||decoder.SeekSeconds(seekSeconds);
    }
    void Close(){decoder.Close();}
    // Fixed for the decoder session once Open has probed the geometry (NV12 for even
    // sizes, BGRA otherwise), so the evaluator can be initialized for it.
    PixelLayout Layout()const{return decoder.PixelLayout();}
    JobRead Read(JobFrame& frame,std::stop_token stop){
        VideoFrame decoded;
        // Whatever this frame still carries has already been rendered and written,
        // so it goes back to the decoder rather than being freed here and
        // reallocated - and zero-filled - by the next pipe read.
        decoder.RecycleFrameBuffer(std::move(frame.bgra));
        const auto read = decoder.ReadNextBlocking(decoded, stop);
        frame = {std::move(decoded.bgra), decoded.timestamp100ns, decoded.discontinuity,
                 decoded.frameNumber, decoded.sourceGeneration};
        if (read == VideoReadResult::FrameReady) return JobRead::FrameReady;
        if (read == VideoReadResult::EndOfStream) return JobRead::EndOfStream;
        if (read == VideoReadResult::Cancelled) return JobRead::Cancelled;
        return JobRead::Error;
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

// Runs a capture's readback copy off the render loop.
//
// The copy is around 10 MiB of memcpy per frame and, taken inline, it was the largest
// single item in the loop. It depends on nothing the loop does next, so it happens here
// while the loop decodes, builds guides and submits the following frame. The readback slot
// the view points into stays reserved by the renderer until Join returns, so the GPU
// cannot land the next capture on top of the memory being copied.
//
// The worker thread is created on first use and lives for the job. Spawning one per frame
// was never an option: Windows thread creation costs tens of microseconds, which is the
// same reason the parallel passes share a pool rather than spawning.
class DeferredCapture {
public:
    DeferredCapture()=default;
    DeferredCapture(const DeferredCapture&)=delete;
    DeferredCapture& operator=(const DeferredCapture&)=delete;
    ~DeferredCapture(){Shutdown();}

    // True while a posted copy has not been joined, and therefore while a readback slot
    // is still spoken for.
    bool Posted()const{std::scoped_lock lock(m_mutex);return m_posted;}

    // Hands the copy to the worker. `scratch` is recycled as the destination buffer, so
    // the full-frame allocation does not repeat every frame.
    void Post(const D3D12Renderer::CaptureReadbackView& view,std::vector<uint8_t>&& scratch){
        if(!m_worker.joinable())m_worker=std::thread([this]{Loop();});
        {
            std::scoped_lock lock(m_mutex);
            m_view=view;m_pixels=std::move(scratch);m_posted=true;m_busy=true;
        }
        m_wake.notify_one();
    }

    // Blocks until the posted copy has finished and moves its bytes into `pixels`.
    // False when nothing was posted, which leaves `pixels` alone.
    bool Join(std::vector<uint8_t>& pixels){
        std::unique_lock lock(m_mutex);
        if(!m_posted)return false;
        m_idle.wait(lock,[this]{return !m_busy;});
        pixels=std::move(m_pixels);m_pixels.clear();m_posted=false;
        return true;
    }

    void Shutdown(){
        if(!m_worker.joinable())return;
        {std::scoped_lock lock(m_mutex);m_quit=true;}
        m_wake.notify_one();
        m_worker.join();
    }

private:
    void Loop(){
        for(;;){
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock,[this]{return m_busy||m_quit;});
            // A copy already posted is finished before quitting, so a Join racing the
            // shutdown still gets its bytes rather than blocking forever.
            if(!m_busy)return;
            const D3D12Renderer::CaptureReadbackView view=m_view;
            std::vector<uint8_t> pixels=std::move(m_pixels);
            lock.unlock();
            D3D12Renderer::CopyCaptureView(view,pixels);
            lock.lock();
            m_pixels=std::move(pixels);m_busy=false;
            lock.unlock();
            m_idle.notify_one();
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_wake,m_idle;
    D3D12Renderer::CaptureReadbackView m_view{};
    std::vector<uint8_t> m_pixels;
    bool m_posted=false,m_busy=false,m_quit=false;
    std::thread m_worker;
};

struct ProductionEvaluatorAdapter {
    D3D12RendererOwner renderer;uint64_t successfulEvaluations{};
    TemporalGuideGenerator guides;
    uint32_t width{},height{};double fps{};
    NeuralRenderFailure lastFailure{NeuralRenderFailure::None};
    // Requested before Initialize; the renderer decides what it can actually deliver.
    bool gpuColorConversion{false};
    // Layout of the frames the source hands over, converted on the GPU when NV12.
    PixelLayout sourceLayout{PixelLayout::Bgra};
    bool Initialize(HWND window,uint32_t w,uint32_t h,double rate,const GuideControls& controls,PixelLayout layout){
        width=w;height=h;fps=rate;sourceLayout=layout;const auto [gridW,gridH]=TemporalGuideGenerator::AnalysisGrid(w,h,rate);
        renderer=MakeD3D12Renderer();
        if(!renderer)return false;
        renderer->SetCaptureFormat(gpuColorConversion?CaptureFormat::Nv12:CaptureFormat::Bgra);
        renderer->SetSourceLayout(layout);
        if(!renderer->Initialize(window,w,h,w,h,gridW,gridH,DefaultNeuralCarrierQuality()))return false;
        // Both sides apply the same even-size rule, so this only fires if that rule drifts.
        if(renderer->ActiveSourceLayout()!=layout){
            LOG("Renderer could not take the decoder's "<<(layout==PixelLayout::Nv12?"NV12":"BGRA")<<" source layout.");
            return false;
        }
        // Nothing ever looks at this renderer's swapchain: the encoder is fed from the
        // cache render target that EnqueueEvaluatedFrameCapture draws for itself.
        guides.SetControls(controls);renderer->SetDLSS(true);renderer->SetHeadless(true);return true;
    }
    CapturedVideoFrame captureScratch;
    // Declared after `renderer` so the worker is joined before the renderer, and with it
    // the mapped readback memory a running copy is reading, goes away.
    DeferredCapture deferred;
    // Set when a readback slot could not be opened. The slot is gone by then, so every
    // later capture would answer for the wrong frame; the drain has to stop instead.
    bool resolveBroken=false;
    // Guide generation cost, split out of the submit stage for the stage log.
    // Shared by the synchronous and the pipelined submit paths, which differ
    // only in how the capture is read back.
    SteadyClock::duration guideCost{};
    // The renderer resets NGX history when guide.id.reset != None (requested
    // reset or a cut detected by the guide generator) and stamps guide.id on
    // the capture, so the job can verify it received the frame it submitted.
    bool Submit(const JobFrame& frame,const FrameIdentity& id,bool capture,JobEvaluation& out){
        GuideFrame guide;const auto guideStart=SteadyClock::now();
        {
            StageClock clock(guideCost);
            if(!guides.Generate(frame.bgra.data(),width,height,width,height,fps,id,guide,sourceLayout)){
                lastFailure=NeuralRenderFailure::Neural;return false;
            }
        }
        out.guideMs=MillisecondsSince(guideStart);
        const float frameMs=static_cast<float>(1000.0/fps);
        if(!capture){
            if(!renderer->RenderFrame(frame.bgra.data(),frame.bgra.size(),id,guide,frameMs)){
                lastFailure=ClassifyRendererFailure(*renderer);return false;
            }
            out.id=guide.id;
        }else{
            // The receipt gate for the first frame needs the pixels in hand before it
            // can decide whether to resubmit. Handing out.bgra's buffer to the capture
            // and taking it back keeps its capacity across the resubmits.
            const auto captureStart=SteadyClock::now();
            captureScratch.pixels=std::move(out.bgra);
            if(!renderer->RenderFrameForCache(frame.bgra.data(),frame.bgra.size(),id,guide,frameMs,captureScratch)){
                lastFailure=ClassifyRendererFailure(*renderer);return false;
            }
            out.captureMs=MillisecondsSince(captureStart);
            out.bgra=std::move(captureScratch.pixels);captureScratch.pixels.clear();
            out.id=captureScratch.id;
        }
        out.neuralGpuMs=renderer->LastNeuralGpuMs();++successfulEvaluations;return true;
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
              <<" ms, GPU waits render-slot "
              <<NanosPerFrameMillis(renderer->RenderSlotWaitNanos(),frames)
              <<" + capture-submit "
              <<NanosPerFrameMillis(renderer->CaptureSubmitSlotWaitNanos(),frames)
              <<" + capture-resolve "
              <<NanosPerFrameMillis(renderer->CaptureResolveWaitNanos(),frames)
              <<" + present-slot "
              <<NanosPerFrameMillis(renderer->PresentSlotWaitNanos(),frames)
              <<" = total "<<NanosPerFrameMillis(renderer->FenceWaitNanos(),frames)
              <<" ms, Present "<<NanosPerFrameMillis(renderer->PresentNanos(),frames)<<" ms.";
        return detail.str();
    }
    // Records the capture without waiting for the GPU. The pixels come back later from
    // ResolveOldest, which waits only on that one frame's fence.
    bool SubmitAsync(const JobFrame& frame,const FrameIdentity& id,JobEvaluation& out){
        GuideFrame guide;const auto guideStart=SteadyClock::now();
        {
            StageClock clock(guideCost);
            if(!guides.Generate(frame.bgra.data(),width,height,width,height,fps,id,guide,sourceLayout)){
                lastFailure=NeuralRenderFailure::Neural;return false;
            }
        }
        out.guideMs=MillisecondsSince(guideStart);
        const float frameMs=static_cast<float>(1000.0/fps);
        if(!renderer->RenderFrame(frame.bgra.data(),frame.bgra.size(),id,guide,frameMs)){
            lastFailure=ClassifyRendererFailure(*renderer);return false;
        }
        if(!renderer->EnqueueEvaluatedFrameCapture()){
            lastFailure=ClassifyRendererFailure(*renderer);return false;
        }
        out.id=guide.id;out.neuralGpuMs=renderer->LastNeuralGpuMs();++successfulEvaluations;return true;
    }
    uint32_t Pending()const{return renderer?renderer->PendingCaptureCount():0u;}
    static constexpr uint32_t MaxPending(){return D3D12Renderer::CaptureSlots;}
    // On success every byte of pixels is overwritten, and the buffer is only resized
    // when it does not already hold exactly one frame, so handing back the previous
    // frame's buffer recycles it for this readback.
    //
    // The bytes are normally already there: the copy was posted at the end of the previous
    // drain and ran while the loop decoded, guided and submitted a frame. What the caller
    // times here is only the part of the copy that did not fit under that work. Before
    // returning, the next oldest capture is posted so the same overlap covers the drain
    // after this one, which is why the returned frame is still the oldest one: the posted
    // copy and the deque the job drains advance together.
    bool ResolveOldest(std::vector<uint8_t>& pixels,double& captureMs){
        if(!renderer||resolveBroken)return false;
        std::vector<uint8_t> spare=std::move(pixels);pixels.clear();
        const auto captureStart=SteadyClock::now();
        bool ok;
        if(deferred.Posted()){
            ok=deferred.Join(pixels);
            renderer->EndResolveOldestCapture();
        }else{
            // The job's first drain, and the tail flush once the ring has run dry: with
            // nothing posted there is nothing to overlap, so this one is copied inline.
            captureScratch.pixels=std::move(spare);spare.clear();
            ok=renderer->ResolveOldestCapture(captureScratch);
            pixels=std::move(captureScratch.pixels);captureScratch.pixels.clear();
        }
        captureMs=MillisecondsSince(captureStart);
        if(!ok)return false;
        PostNextResolve(std::move(spare));
        return true;
    }
    // Reserves the oldest remaining capture's readback slot and starts its copy on the
    // worker. A slot that cannot be opened has already been retired by the renderer, which
    // would slide every later frame's pixels one place against the job's deque, so the
    // failure is latched and the next drain reports it instead.
    void PostNextResolve(std::vector<uint8_t>&& scratch){
        if(!renderer||!renderer->PendingCaptureCount())return;
        D3D12Renderer::CaptureReadbackView view;
        if(!renderer->BeginResolveOldestCapture(view)){
            resolveBroken=true;
            lastFailure=ClassifyRendererFailure(*renderer);
            return;
        }
        deferred.Post(view,std::move(scratch));
    }
    void DiscardPending(){
        if(!renderer)return;
        // The posted copy holds a slot, so it has to be joined before the ring can drain.
        std::vector<uint8_t> dropped;
        if(deferred.Join(dropped))renderer->EndResolveOldestCapture();
        resolveBroken=false;
        while(renderer->PendingCaptureCount()){
            CapturedVideoFrame discarded;
            if(!renderer->ResolveOldestCapture(discarded))break;
        }
    }
    // The cache render target is B8G8R8A8, so ffmpeg is told to consume BGRA and the
    // per-pixel channel swizzle that used to run on every readback disappears.
    // What Initialize settled on, which is BGRA unless the GPU conversion was both
    // asked for and possible at this size.
    EncoderPixelFormat CapturePixelFormat()const{
        return renderer&&renderer->ActiveCaptureFormat()==CaptureFormat::Nv12
            ?EncoderPixelFormat::Nv12:EncoderPixelFormat::Bgra;
    }
    bool FeatureCreated()const{return renderer&&renderer->DLSSFeatureCreated();}
    uint64_t EvaluationCount()const{return successfulEvaluations;}
    void ResetTemporal(){guides.Reset();DiscardPending();}
    NeuralRenderFailure LastFailure()const{return lastFailure;}
    uint64_t PeakLocalVideoMemoryMiB()const{return renderer?renderer->PeakLocalVideoMemoryMiB():0;}
};

// WriteFrame pushes a whole frame, over 30 MB at 4K, into ffmpeg's stdin and blocks
// whenever the encoder falls behind. Running it on a feeder thread is the missing half of
// the decoder's frame queue: decode, neural evaluation and encode now all overlap.
//
// The cost is that an encoder error surfaces up to QueueCapacity frames late. The NVENC
// to libx264 retry path already cancels and re-reads the source from frame zero, so it
// still recovers; it just wastes a couple more frames before noticing.
//
// The queue absorbs the encoder's jitter, so its depth is a time budget, not a count. At
// two frames it held ~13 ms and the render loop wore the difference: the write stage sat
// at a p50 of 0.002 ms with a mean of 1.57, i.e. almost every frame handed over free
// while a thin tail blocked for tens of milliseconds. Eight frames is ~50 ms at the rate
// the loop actually runs. The memory is QueueCapacity plus the two buffers in
// circulation, so it scales with frame size: ~106 MiB at 2578x1080, ~330 MiB at 4K.
struct ProductionEncoderAdapter {
    RawVideoEncoder encoder;
    static constexpr size_t QueueCapacity=8;

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

    EncodeError WriteFrame(std::span<const uint8_t> frame,std::stop_token stop){return encoder.WriteFrame(frame,stop);}

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

    // One fresh ffmpeg pipe per segment; a single-file job never calls this.
    std::unique_ptr<ProductionEncoderAdapter> Create(){return std::make_unique<ProductionEncoderAdapter>();}

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

#endif

} // namespace

#ifndef OFFLINE_NEURAL_RENDERER_TESTING
namespace {

uint64_t FileTimeValue(const FILETIME& time)
{
    ULARGE_INTEGER value{};value.LowPart=time.dwLowDateTime;value.HighPart=time.dwHighDateTime;
    return value.QuadPart;
}

uint64_t ProcessStartFileTime()
{
    FILETIME creation{},exit{},kernel{},user{};
    if(!GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user))return 0;
    return FileTimeValue(creation);
}

std::optional<uint64_t> LastWriteFileTime(const std::filesystem::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes))return std::nullopt;
    return FileTimeValue(attributes.ftLastWriteTime);
}

} // namespace

std::filesystem::path ResolveNeuralRuntimeLogPath(const std::filesystem::path& runtimeDirectory)
{
    // ReShade truncates its log when the proxy loads, and rotates to
    // ReShade.log1 when ReShade.log is still held. A file last written before
    // this process started belongs to an earlier session and must never be
    // read as evidence.
    static const uint64_t started=ProcessStartFileTime();
    std::filesystem::path best;uint64_t newest=0;
    for(const wchar_t* name:{L"ReShade.log",L"ReShade.log1"}){
        const std::filesystem::path candidate=runtimeDirectory/name;
        const auto written=LastWriteFileTime(candidate);
        if(!written||*written<started)continue;
        if(best.empty()||*written>newest){best=candidate;newest=*written;}
    }
    return best;
}

std::string ReadNeuralRuntimeSessionLog(const std::filesystem::path& runtimeDirectory)
{
    constexpr uintmax_t Limit=4u*1024u*1024u;std::string latest;
    uintmax_t lastSize=std::numeric_limits<uintmax_t>::max();
    int stableSamples=0;
    for(int attempt=0;attempt<20;++attempt){
        const std::filesystem::path path=ResolveNeuralRuntimeLogPath(runtimeDirectory);
        std::error_code error;
        const auto size=path.empty()?uintmax_t{0}:std::filesystem::file_size(path,error);
        if(!path.empty()&&!error&&size<=Limit){
            std::ifstream input(path,std::ios::binary);
            if(input){
                latest={std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
                const auto evidence=ParseNeuralRuntimeEvidence(latest);
                stableSamples=size==lastSize?stableSamples+1:1;
                lastSize=size;
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
    std::function<std::string()> evidenceProvider,Clock clock,std::function<bool()> paused,
    std::function<std::unique_ptr<IFrameEncoder>()> encoderFactory)
    : testSource_(&source),testEvaluator_(&evaluator),testEncoder_(&encoder),
      testEvidenceProvider_(std::move(evidenceProvider)),testClock_(std::move(clock)),
      testPaused_(std::move(paused)),testEncoderFactory_(std::move(encoderFactory)) {}
#endif

NeuralRenderResult OfflineNeuralRenderer::Run(const NeuralRenderRequest& request,
                                               ProgressCallback progress,std::stop_token stop,
                                               const NeuralSegmentSink& segments)
{
#ifdef OFFLINE_NEURAL_RENDERER_TESTING
    if(!testSource_||!testEvaluator_||!testEncoder_||!testEvidenceProvider_)
        return NeuralRenderResult{.failure=NeuralRenderFailure::Protocol,
                                  .detail=L"Offline renderer test dependencies are incomplete."};
    TestSourceAdapter source{*testSource_};TestEvaluatorAdapter evaluator{*testEvaluator_};
    TestEncoderAdapter encoder{testEncoder_,{},testEncoderFactory_};
    const Clock clock=testClock_?testClock_:[]{return SteadyClock::now();};
    const std::function<bool()> paused=testPaused_?testPaused_:[]{return false;};
    return RunJob(request,std::move(progress),stop,source,evaluator,encoder,
                  testEvidenceProvider_,clock,paused,segments);
#else
    const auto runtimeDirectory=ModuleDirectory();
    ProductionSourceAdapter source;ProductionEvaluatorAdapter evaluator;ProductionEncoderAdapter encoder;
    evaluator.gpuColorConversion=request.gpuColorConversion;
    return RunJob(request,std::move(progress),stop,source,evaluator,encoder,
        [runtimeDirectory]{return ReadNeuralRuntimeSessionLog(runtimeDirectory);},
        []{return SteadyClock::now();},
        [pauseEvent=request.pauseEvent]{
            return pauseEvent&&WaitForSingleObject(pauseEvent,0)==WAIT_OBJECT_0;
        },segments);
#endif
}
