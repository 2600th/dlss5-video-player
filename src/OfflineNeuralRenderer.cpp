#include "OfflineNeuralRenderer.h"
#include "PlatformPaths.h"

#include "Log.h"
#include "PixelLayout.h"
#include "MediaSource.h"

#include <algorithm>
#include <array>
#include <atomic>
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

#include "D3D12Renderer.h"
#include "DeferredCapture.h"
#include "TemporalGuides.h"
#include "VideoDecoder.h"
#include "DLSSBackend.h"
#include "FrameResample.h"
#include "UpscalingPolicy.h"

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
    // The guide generator's motion for this frame, per analysis-grid cell and in
    // cells, x and y interleaved: what the render report's warping error moves the
    // previous frame by. Empty from an evaluator with no guide generator.
    std::vector<float> motionCells;
    uint32_t gridWidth{}, gridHeight{};
};

// One captured frame's side of the render report: its source reduced to the
// metric grid when it was submitted, since the decoder has recycled the pixels by
// the time the capture comes back.
struct MetricSample {
    temporal_metrics::Plane source;
    std::vector<float> motion;
    bool newShot{};
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

    // Nine series of one double per frame. Growing them doubling by doubling
    // reallocates and copies each about log2(frames) times on the render thread,
    // a full series at a time, so they are sized once for the frames the attempt
    // expects to capture.
    void Reserve(size_t frames)
    {
        for (auto* series : {&read, &guide, &render, &write, &eval, &latency, &loop, &loopDrain,
                             &loopResidual})
            series->reserve(frames);
    }
};

struct AttemptResult {
    NeuralRenderFailure failure{NeuralRenderFailure::None};
    EncodeError encoderError{EncodeError::None};
    uint64_t frames{};
    uint64_t bytes{};
    // Evaluate calls the neural backend itself completed while this attempt
    // captured: sampled at the first capture, so the preroll and the receipt
    // gate's uncaptured resubmits are left out, and every frame retry counts. Published as the result's nativeEvaluations,
    // which is what makes the cache's evidence gate a second witness rather
    // than a copy of `frames`.
    uint64_t neuralEvaluations{};
    uint32_t historyResets{};
    bool hasTimestamp{};
    int64_t firstTimestamp{};
    int64_t lastTimestamp{};
    std::vector<double> neuralGpuMs;
    StageSamples stages;
    // The render report's metrics over this attempt's captured frames. A software
    // retry measures its own frames from the start, like every counter above.
    temporal_metrics::Accumulator metrics;
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
// <staging>/neural-00000.mkv, neural-00001.mkv, ... A software-encoder retry
// renumbers from 0 under a name of its own, neural-r1-00000.mkv, rather than
// reusing the failed attempt's: the player's decoder holds a published file
// open without delete sharing, so deleting it fails silently and the retry's
// encoder then truncated a file that was still being decoded.
std::filesystem::path SegmentFilePath(const std::filesystem::path& stagingVideoPath, uint32_t attempt,
                                      uint64_t index)
{
    std::wstring digits = std::to_wstring(index);
    if (digits.size() < 5) digits.insert(0, 5 - digits.size(), L'0');
    std::wstring stem = stagingVideoPath.stem().wstring();
    if (attempt) stem += L"-r" + std::to_wstring(attempt);
    return stagingVideoPath.parent_path() / (stem + L"-" + digits + stagingVideoPath.extension().wstring());
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
                  int64_t frameDuration100ns, NeuralSegmentSink sink, std::stop_token stop,
                  uint32_t firstSegmentFrames = 0)
        : factory_(std::move(factory)), staging_(stagingVideoPath), segmentFrames_(segmentFrames),
          firstSegmentFrames_(firstSegmentFrames ? firstSegmentFrames : segmentFrames),
          frameDuration_(frameDuration100ns), sink_(std::move(sink)), stop_(std::move(stop)) {}
    ~SegmentWriter() { Cancel(); }
    SegmentWriter(const SegmentWriter&) = delete;
    SegmentWriter& operator=(const SegmentWriter&) = delete;

    // Starts a fresh sequence at index 0. A software-encoder retry deletes every
    // file the previous attempt wrote, best effort, and writes under new names
    // (see SegmentFilePath), so a consumer that sees index 0 again knows the
    // earlier segments are gone and none of them is overwritten under it.
    void BeginAttempt(const EncoderSpec& spec)
    {
        Cancel();
        const uint64_t previous = started_;
        for (uint64_t index = 0; index < previous; ++index) Remove(index);
        // Both threads are joined, so nothing reads the attempt number while it moves.
        if (begun_) ++attempt_;
        begun_ = true;
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
    // Idle frame buffers kept for the render loop's next readbacks. Sized to what the
    // queue can hold rather than to the capture ring: a start-up stall puts that many
    // buffers in circulation, and with room for four every buffer past the fourth was
    // freed on its way back and allocated again by the drain that replaced it - a
    // value-initialised 33 MB at 4K. The pool is bounded by the same bytes as the queue
    // and only ever holds buffers that were already allocated.
    static constexpr size_t kMinRecycledFrames = 4;
    static size_t RecycledFrameCapacity(size_t frameBytes)
    {
        return std::max(kMinRecycledFrames, kQueuedFrameBytes / std::max<size_t>(1, frameBytes));
    }

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
                if (recycled_.size() < RecycledFrameCapacity(frame.bgra.size()))
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
        // Segment 0 may be shorter than the rest, so the boundary is not a
        // plain division: everything after it sits on the full-length grid.
        const uint64_t index = written_ < firstSegmentFrames_
            ? 0 : 1 + (written_ - firstSegmentFrames_) / segmentFrames_;
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
            const EncodeError startError = next->Start(spec_, Path(index));
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
        segment.fileName = Path(currentIndex_).filename().wstring();
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
                    ? warm->Start(spec_, Path(warmIndex))
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

    std::filesystem::path Path(uint64_t index) const { return SegmentFilePath(staging_, attempt_, index); }

    void Remove(uint64_t index) const
    {
        std::error_code error;
        std::filesystem::remove(Path(index), error);
    }

    std::function<std::unique_ptr<Encoder>()> factory_;
    std::filesystem::path staging_;
    uint32_t segmentFrames_{};
    uint32_t firstSegmentFrames_{};
    int64_t frameDuration_{};
    NeuralSegmentSink sink_;
    std::stop_token stop_;
    EncoderSpec spec_{};
    std::unique_ptr<Encoder> current_;
    uint64_t currentIndex_{},currentFrames_{},currentFirstFrameNumber_{};
    int64_t currentFirstTimestamp_{},currentLast_{};
    uint64_t written_{};
    // Which attempt's names the files are written under; 0 until a retry.
    uint32_t attempt_{};
    bool begun_{};
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
                          const NeuralSegmentSink& segments,
                          const NeuralColdStartCallback& coldStart)
{
    NeuralRenderResult result;
    result.jobId = request.jobId;
    result.neural = request.requireNeural;
    // Cold-start phases run on the real clock rather than the job's progress
    // clock: the first-output boundary is only observable on the finalize
    // thread, and a caller-supplied clock is not shared across threads.
    NeuralColdStartTimeline coldStartTimeline;
    SteadyClock::time_point coldStartMark = SteadyClock::now();
    std::atomic<bool> coldStartReported{false};
    auto markColdStart = [&](NeuralColdStartPhase phase) {
        const auto now = SteadyClock::now();
        coldStartTimeline.Record(
            phase, std::chrono::duration_cast<std::chrono::microseconds>(now - coldStartMark));
        coldStartMark = now;
    };
    auto reportColdStart = [&] {
        if (coldStart && !coldStartReported.exchange(true)) coldStart(coldStartTimeline);
    };
    // Reports on every exit, so a run that stopped before it published anything
    // still says how far the stack got. Declared ahead of the segment writer so
    // it runs after that writer's thread has joined.
    ScopeExit<decltype(reportColdStart)> reportColdStartOnExit{reportColdStart};
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
    // The capture size, resolved once. 0 means "no upscale", which is every
    // caller that predates Super Resolution being part of an export.
    const uint32_t outputWidth = request.outputWidth ? request.outputWidth : request.width;
    const uint32_t outputHeight = request.outputHeight ? request.outputHeight : request.height;
    // An output SMALLER than the source is not an upscale, and DLSS refuses it.
    // Caught here rather than at the renderer so the refusal names the request.
    if (outputWidth < request.width || outputHeight < request.height) {
        return fail(NeuralRenderFailure::Source,
                    L"The neural render output size is smaller than the source, which is not an upscale.");
    }
    // What the model is shown. Below 100% the source adapter hands over frames
    // already reduced to this size, and the carrier restores the source size.
    const ProcessingInput modelInput =
        ProcessingSize(request.width, request.height, request.processingScale);
    if (!IsProcessingScaleRung(request.processingScale) ||
        (request.processingScale != kDefaultProcessingScale &&
         (outputWidth != request.width || outputHeight != request.height))) {
        return fail(NeuralRenderFailure::Source,
                    L"A reduced processing scale renders back to the source size and cannot also upscale.");
    }
    const uint64_t expectedBytes64 = uint64_t{outputWidth} * outputHeight * 4u;
    if (expectedBytes64 > std::numeric_limits<size_t>::max()) {
        return fail(NeuralRenderFailure::Source, L"Neural render dimensions are too large.");
    }
    // Replaced once the evaluator has settled on a capture format: a GPU-converted
    // capture is NV12, which is 1.5 bytes per pixel rather than 4.
    size_t expectedBytes = static_cast<size_t>(expectedBytes64);
    // The finalize thread publishes each file, so the first-output boundary is
    // stamped there. Everything above it is written before capture can begin
    // and the encoder queue's lock orders the two, so the timeline that leaves
    // with the first file is complete. A relaunched sequence repeats index 0;
    // only the first one is a cold start.
    NeuralSegmentSink instrumented = segments;
    instrumented.onSegment = [&, forward = segments.onSegment](const NeuralRenderSegment& segment) {
        if (!segment.index && !coldStartTimeline.Phase(NeuralColdStartPhase::FirstOutput))
            markColdStart(NeuralColdStartPhase::FirstOutput);
        if (forward) forward(segment);
        reportColdStart();
    };
    // A segmented job publishes finalized files while it renders; segmentFrames
    // == 0 keeps the single staging file and never starts a finalize thread.
    std::optional<SegmentWriter<Encoder>> writer;
    if (request.segmentFrames) {
        writer.emplace([&encoder] { return encoder.Create(); }, request.stagingVideoPath,
                       request.segmentFrames, frameDuration, instrumented, stop,
                       request.firstSegmentFrames);
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
    if (!evaluator.Initialize(request.renderWindow, modelInput.width, modelInput.height,
                              outputWidth, outputHeight, request.fps,
                              request.guides, source.Layout(), source.ColorDescription())) {
        source.Close();
        return fail(NeuralRenderFailure::Neural, L"The neural renderer could not be initialized.");
    }
    // Super Resolution from the source size may not reach every rung: DLSS
    // admits a bounded input-to-output ratio, and the renderer adopts the
    // largest output the runtime accepts for this source. Encoding that under
    // the size the caller asked for would be a wrong-sized file, so the job
    // says what the runtime can do instead.
    if constexpr (requires { evaluator.OutputSize(); }) {
        const auto [settledWidth, settledHeight] = evaluator.OutputSize();
        if (settledWidth != outputWidth || settledHeight != outputHeight) {
            source.Close();
            std::wostringstream detail;
            detail << L"DLSS Super Resolution cannot reach " << outputWidth << L"x" << outputHeight
                   << L" from a " << request.width << L"x" << request.height << L" source on this runtime; "
                   << L"the largest output it admits is " << settledWidth << L"x" << settledHeight
                   << L". Choose a lower output height.";
            return fail(NeuralRenderFailure::Source, detail.str());
        }
    }
    // The source open and the evaluator's own bring-up (device, NGX) are one
    // boundary: nothing between them is separately observable from here.
    //
    // A job served by an evaluator an earlier job left initialized did not pay
    // that bring-up, so it reports no value for the phase rather than a zero:
    // an absent phase did not happen, which is a different claim from one that
    // took no time, and a cold-start table that cannot tell them apart is
    // worthless. The mark is deliberately not advanced either, which leaves the
    // source open this job DID pay inside the next boundary it reports instead
    // of dropping it on the floor.
    const bool evaluatorReused = [&] {
        if constexpr (requires { evaluator.Reused(); }) return evaluator.Reused();
        else return false;
    }();
    if (!evaluatorReused) markColdStart(NeuralColdStartPhase::NeuralInit);
    // The neural backend's own Evaluate tally: the NGX count in production, and
    // whatever the injected evaluator vouches for in the tests.
    auto neuralEvaluations = [&]() -> uint64_t { return evaluator.NeuralEvaluations(); };
    expectedBytes = static_cast<size_t>(
        EncoderFrameBytes(evaluator.CapturePixelFormat(), outputWidth, outputHeight));

    const bool singleFrameSource = totalFrames == 1;
    const uint64_t primeLimit = singleFrameSource
        ? 120 : std::max<uint64_t>(2, std::min<uint64_t>(totalFrames, 120));
    uint64_t primed = 0;
    JobFrame primingFrame;
    // Presents a priming frame that has already been decoded, without capturing
    // it. Priming presents are what let the add-on observe the raw NGX calls.
    auto presentPrimingFrame = [&](HistoryReset reason) {
        JobEvaluation ignored;
        return evaluator.Submit(primingFrame, identity(primingFrame, reason), false, ignored);
    };
    for (; !evaluator.FeatureCreated() && primed < primeLimit; ++primed) {
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
        if (!presentPrimingFrame(reason)) {
            source.Close();
            return fail(evaluatorFailure(), L"Feature 18 priming failed.");
        }
    }
    if (!evaluator.FeatureCreated()) {
        source.Close();
        return fail(NeuralRenderFailure::Neural, L"Feature 18 was not created.");
    }
    // The priming loop above runs for a Super Resolution-only job too: what it
    // waits for is the NGX carrier feature, which D3D12Renderer creates on the
    // second present whether or not an add-on is watching, and a capture
    // before that would encode the un-upscaled source. Everything from here to
    // the capture is about feature 18 - the add-on's arming, the re-hook that
    // coaxes it and the log baseline the receipt gate reads - and a job that
    // runs with the add-on disabled has none of it to wait for.
    NeuralRuntimeEvidence armedEvidence;
    if (request.requireNeural) armedEvidence = ParseNeuralRuntimeEvidence(evidenceProvider());
    if (request.requireNeural && !armedEvidence.Valid() && primed > 0 && evaluator.RequestFeatureRehook()) {
        // The add-on arms its NGX detours asynchronously, and a build that
        // missed the very first CreateFeature stays in a standby state until it
        // sees another one. One hook-visible re-create clears that. It belongs
        // here, before capture: the only evidence the job has that the neural
        // pass ran is the add-on's own evaluation counter, so releasing the
        // feature once capture is waiting on that counter destroys the state it
        // is waiting for. Sixty presents is the same asynchronous-arming budget
        // the sibling feeder projects hold a rebuild for, and the counter shows
        // up on the add-on's first neural evaluation, well inside it.
        constexpr uint64_t kRehookArmPresents = 60;
        for (uint64_t rearm = 0; rearm < kRehookArmPresents; ++rearm) {
            if (stop.stop_requested()) return cancelled(L"Neural render was cancelled.");
            if (!presentPrimingFrame(HistoryReset::None)) {
                source.Close();
                return fail(evaluatorFailure(), L"Feature 18 priming failed.");
            }
            // The runtime logs sparsely; re-reading its log every present costs
            // more than it learns.
            if (rearm % 10 != 9) continue;
            armedEvidence = ParseNeuralRuntimeEvidence(evidenceProvider());
            if (armedEvidence.Valid()) break;
        }
    }
    if (request.requireNeural && !armedEvidence.Valid()) {
        source.Close();
        return fail(NeuralRenderFailure::Neural,
                    L"Feature 18 inline interception was not armed before frame capture.");
    }
    result.feature18ArmedBeforeCapture=request.requireNeural;
    // Priming is what creates feature 18 and what the add-on arms its detours
    // on; a retained feature skips the loop above entirely, so on that path
    // nothing this phase names happened and it reports nothing rather than a
    // zero. A reused evaluator that still had to prime - the add-on lost the
    // feature under us - reports the arm it really paid.
    if (!evaluatorReused || primed > 0) markColdStart(NeuralColdStartPhase::FeatureArm);
    // Baseline read after any re-hook, so a create the add-on observed late
    // cannot be mistaken for the captured sequence's own evaluation.
    const uint64_t successfulAttemptBaseline=armedEvidence.highestObservedEvaluation;
    // Whether the capture pass is held to the add-on's log counter advancing
    // past that baseline. It is a one-shot proof per process (see the receipt
    // gate), so only the first capture pass of a fresh evaluator can be held
    // to it; a reused evaluator and a software-encoder retry are held to the
    // backend's own count and the timing floor instead.
    bool holdToLogReceipt=request.requireNeural&&!evaluatorReused;
    // Set once the gate has seen that receipt. The log only grows, so a pass that
    // restarts after the gate opened has nothing left to wait for.
    bool receiptGateOpened=false;
    // The gate's polls. It needs to know whether a line has appeared, not to wait
    // for the file to settle, so a reader that can tail the log without the
    // stability wait is asked that way; every verdict read below still waits.
    auto pollEvidence=[&]()->std::string{
        if constexpr(requires{evidenceProvider.get().Poll();})return evidenceProvider.get().Poll();
        else return evidenceProvider();
    };

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
        // The backend's own evaluation count, read at the first capture and on
        // every exit, so the attempt can be held to having actually evaluated
        // the frames it claims. Sampled around the capture pass rather than
        // per frame because a retry pass re-renders everything and only its
        // own work counts; sampled after the preroll because those frames are
        // evaluated without being captured.
        uint64_t neuralEvaluationsBefore = neuralEvaluations();
        AttemptResult attempt;
        {
            // Capped so a nonsense duration cannot reserve gigabytes up front: past
            // about 4.8 hours at 60 fps the series simply grow as they did before.
            constexpr uint64_t kReservedFramesCap = uint64_t{1} << 20;
            const size_t frames = static_cast<size_t>(std::min(totalFrames, kReservedFramesCap));
            attempt.stages.Reserve(frames);attempt.neuralGpuMs.reserve(frames);
        }
        // The captured frames, not the source: an upscaling job encodes what
        // came out of Super Resolution.
        EncoderSpec spec{outputWidth, outputHeight, request.fps, kind,
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
        const auto recordNeuralEvaluations = [&] {
            const uint64_t now = neuralEvaluations();
            attempt.neuralEvaluations = now > neuralEvaluationsBefore ? now - neuralEvaluationsBefore : 0;
        };
        // Declared after the stage report so it runs before it: the stage table
        // is the last thing an attempt writes.
        ScopeExit<decltype(recordNeuralEvaluations)> recordOnExit{recordNeuralEvaluations};
        // Submission runs ahead of encoding, so frame accounting has to be tracked
        // separately from what has actually been written out.
        struct InFlightCapture {
            uint64_t frameNumber{};
            int64_t timestamp100ns{};
            // The frame queued in this position, which the identity the readback
            // slot resolves with has to name.
            FrameIdentity id{};
            double readMs{},guideMs{},evalMs{},neuralGpuMs{};
            SteadyClock::time_point frameStart{};
            MetricSample metric;
        };
        std::deque<InFlightCapture> inFlight;
        uint64_t submitted = 0;
        evaluator.DiscardPending();
        // What draining an older frame's capture cost inside the current iteration. The
        // drain belongs to the iteration that runs it, not to the frame it drains, and
        // keeping the two apart is what stops pipeline latency leaking into the
        // throughput numbers. Reset at the top of every iteration.
        double iterationDrainMs = 0.0;

        // The render report's two halves (TemporalMetrics.h). The source is reduced
        // at submit, on the guide generator's own grid so its vectors move the cells
        // they were solved for; the capture is reduced when it comes back, in the
        // layout it was captured in. A frame that cannot be sampled is left out of
        // the report and never fails the render.
        const PixelLayout captureLayout=evaluator.CapturePixelFormat()==EncoderPixelFormat::Nv12
            ?PixelLayout::Nv12:PixelLayout::Bgra;
        auto sampleSource=[&](const JobFrame& submittedFrame,JobEvaluation& evaluation)->MetricSample{
            MetricSample sample;
            uint32_t gridWidth=evaluation.gridWidth,gridHeight=evaluation.gridHeight;
            if(!gridWidth||!gridHeight){
                const auto grid=TemporalGuideGenerator::AnalysisGrid(request.width,request.height,request.fps);
                gridWidth=grid.first;gridHeight=grid.second;
            }
            if(!temporal_metrics::Sample(submittedFrame.bgra,source.Layout(),request.width,request.height,
                                         gridWidth,gridHeight,sample.source))sample.source={};
            sample.motion=std::move(evaluation.motionCells);
            sample.newShot=evaluation.id.reset!=HistoryReset::None;
            return sample;
        };
        auto measure=[&](const MetricSample& sample,std::span<const uint8_t> captured){
            if(sample.source.y.empty())return;
            temporal_metrics::Plane output;
            if(!temporal_metrics::Sample(captured,captureLayout,outputWidth,outputHeight,
                                         sample.source.width,sample.source.height,output))return;
            attempt.metrics.Add(sample.source,output,sample.motion,sample.newShot);
        };

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
            FrameIdentity resolved{};
            {
                StageClock clock(stages.resolve);
                if (!evaluator.ResolveOldest(pixels, resolved, captureMs) ||
                    pixels.size() != expectedBytes) {
                    return evaluatorFailure();
                }
            }
            inFlight.pop_front();
            // The slot and the queue advance together, so the bytes that just came
            // back must be the frame queued first. A ring that slid out of step would
            // otherwise publish every later frame one place off, as verified.
            if (!resolved.SameSource(queued.id)) {
                LOG("Neural capture identity mismatch: expected frame#" << queued.id.frameNumber
                    << " pts=" << queued.id.pts100ns << ", readback holds frame#" << resolved.frameNumber
                    << " pts=" << resolved.pts100ns);
                return NeuralRenderFailure::Identity;
            }
            measure(queued.metric,pixels);
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
            ++attempt.frames;attempt.bytes+=written;
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
            // cancellation prompt, and an identity mismatch means the readback ring
            // is out of step, so nothing still in it can be trusted to be the frame
            // it claims.
            if (failure != NeuralRenderFailure::Cancelled && failure != NeuralRenderFailure::Identity) {
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
        // Optional only so the receipt gate can restart the pass from the preroll.
        std::optional<FramePrefetch<Source>> prefetch;
        prefetch.emplace(source, stop);
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
                read = prefetch->Next(frame);
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
            if (submitted == 0 && holdToLogReceipt && !receiptGateOpened) {
                // The add-on's log counter is a one-shot proof per PROCESS, not
                // per job. It reports "inline feature 18 evaluation succeeded
                // (count=N)" at N=1 and N=60 and then goes quiet: measured on
                // this machine's RTX 4080 SUPER, a first job took the NGX
                // evaluation count 0 -> 120 and logged exactly those two lines,
                // and a second job in the same process took it 120 -> 240 and
                // logged nothing at all while evaluating every frame at 5.26 ms
                // of real GPU time. So a reused evaluator can never watch that
                // counter advance, however many times it resubmits, and waiting
                // for it would refuse a healthy job after 120 pointless
                // resubmits of its first frame.
                //
                // What a reused job proves instead, per job and without any
                // session high-water mark: the backend's own evaluation count
                // advanced at least once per captured frame (checked with the
                // verdicts below) and the neural GPU time per frame clears the
                // floor a DLAA-only run cannot (NeuralTimingClearsFloor, which
                // is the check that actually catches the failure this gate was
                // built for). The session evidence itself was already verified
                // before capture and describes the live feature this job used.
                //
                // A fresh process is held to the counter as well: frame 0 is
                // resubmitted until a receipt past the armed baseline exists.
                // The resubmits are not captured - each used to be a full
                // synchronous capture and readback - and a preroll of about 60
                // frames usually opens the gate before the first one.
                const HistoryReset firstReason = !prerollEvaluated ? HistoryReset::FirstFrame
                    : frame.discontinuity ? HistoryReset::SourceChange : HistoryReset::None;
                // The observed add-on cadence is one log line every sixty
                // evaluations, so twice that carries a full cadence of margin
                // wherever the baseline happened to land. Nothing may release
                // the NGX feature while this gate is running: the counter it
                // waits for stops advancing when the add-on's worksets go.
                constexpr uint64_t kReceiptGateResubmits = 120;
                uint64_t resubmits = 0;
                for (;; ++resubmits) {
                    if (stop.stop_requested()) {abort(NeuralRenderFailure::Cancelled);return attempt;}
                    const auto receipt = ParseNeuralRuntimeEvidence(pollEvidence());
                    if (!receipt.Valid()) {abort(NeuralRenderFailure::Neural);return attempt;}
                    if (receipt.highestObservedEvaluation > successfulAttemptBaseline) break;
                    if (resubmits >= kReceiptGateResubmits) {abort(NeuralRenderFailure::Neural);return attempt;}
                    JobEvaluation ignored;
                    const NeuralRenderFailure failure = evaluate(
                        frame, resubmits == 0 ? firstReason : HistoryReset::None, false, false, ignored);
                    if (failure != NeuralRenderFailure::None) {abort(failure);return attempt;}
                }
                receiptGateOpened = true;
                if (resubmits) {
                    // Every resubmit fed frame 0 into the temporal history again, and
                    // how many it took depends on when the add-on's log flushed, so
                    // the same cache key produced different bytes run to run. The
                    // capture starts from the history it would have had with none.
                    LOG("Feature 18 receipt gate opened after " << resubmits
                        << " uncaptured resubmit(s) of the first frame; restoring its history.");
                    if (prerollEvaluated) {
                        // Resetting here would throw the preroll away, and the preroll
                        // is what makes a range's first frame match a continuous render.
                        // Run it again instead; the gate stays open.
                        prefetch.reset();
                        if (!reopenAtPreroll()) {
                            abort(stop.stop_requested() ? NeuralRenderFailure::Cancelled
                                                        : NeuralRenderFailure::Source);
                            return attempt;
                        }
                        prefetch.emplace(source, stop);
                        prerollEvaluated = false;hasPrevious = false;
                        continue;
                    }
                    // No preroll: frame 0 resets history anyway, and a full reset of
                    // the guides with it leaves nothing the resubmits touched.
                    evaluator.ResetTemporal();
                }
            }
            // Sampled here, after any gate resubmits, so the backend's count is held
            // to exactly the submissions that produced captured frames and their retries.
            if (submitted == 0) neuralEvaluationsBefore = neuralEvaluations();
            const auto evalStart = SteadyClock::now();
            JobEvaluation evaluation;
            const bool pipelined = submitted > 0;
            {
                const HistoryReset reason = (attempt.frames == 0 && !prerollEvaluated) ? HistoryReset::FirstFrame
                    : frame.discontinuity ? HistoryReset::SourceChange : HistoryReset::None;
                const NeuralRenderFailure failure = evaluate(frame, reason, true, pipelined, evaluation);
                if (failure != NeuralRenderFailure::None) {abort(failure);return attempt;}
            }
            const double evalMs = MillisecondsSince(evalStart);
            if (pipelined) {
                // Record the capture and move straight on to the next source frame. The GPU
                // keeps working while the previous frame is copied back and encoded.
                InFlightCapture queued;
                queued.frameNumber = frame.frameNumber;
                queued.timestamp100ns = frame.timestamp100ns;
                queued.id = identity(frame, HistoryReset::None);
                queued.readMs = readMs;queued.guideMs = evaluation.guideMs;
                queued.evalMs = evalMs;queued.neuralGpuMs = evaluation.neuralGpuMs;
                queued.frameStart = frameStart;
                queued.metric = sampleSource(frame, evaluation);
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
            measure(sampleSource(frame, evaluation), evaluation.bgra);
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
            ++attempt.frames;attempt.bytes+=captured;
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
        if(request.requireNeural&&!retryEvidence.Valid()){
            return fail(NeuralRenderFailure::Neural,
                        L"Feature 18 evidence was not valid before the software retry.");
        }
        // The retry is held to the reused-evaluator standard. The first pass
        // already watched the log counter advance, which is what vouches for
        // this process; the counter then stops at 60, and NVENC failing after
        // the first 60 evaluations - every range render's preroll, every live
        // session - left the retry resubmitting frame 0 120 times for a line
        // that never came, then failing with "evidence did not advance". The
        // backend's per-frame count and the timing floor below still judge
        // every frame this pass captures.
        holdToLogReceipt=false;
        selected=EncoderKind::H264Software;
        attempt=runAttempt(selected);
    }
    // Read after the last attempt, and on every exit from it: the counters describe the
    // job, so a software-encoder retry's second pass belongs in the same tally. Only the
    // production adapter owns a guide generator; the test one is compiled past.
    if constexpr(requires{evaluator.SceneCuts();})result.sceneCuts=evaluator.SceneCuts();
    if (attempt.failure == NeuralRenderFailure::Cancelled)
        return cancelled(L"Neural render was cancelled.");
    if (attempt.failure != NeuralRenderFailure::None) {
        cancelOutput();source.Close();
        result.historyResets=attempt.historyResets;
        return fail(attempt.failure, AttemptFailureDetail(attempt.failure));
    }
    source.Close();
    result.metrics=attempt.metrics.Finish();
    emit(NeuralRenderPhase::Encoding,attempt.frames,attempt.bytes,false);
    emit(NeuralRenderPhase::Validating,attempt.frames,attempt.bytes,false);
    result.evidence=ParseNeuralRuntimeEvidence(evidenceProvider());
    result.historyResets=attempt.historyResets;
    // Summarized before the verdicts below so a refusal carries the numbers it
    // was based on into the receipt.
    result.timing=SummarizeTiming(attempt,evaluator.PeakLocalVideoMemoryMiB());
    // Everything from here to the timing floor judges the NEURAL pass, so a job
    // that asked for Super Resolution alone is not held to any of it. Each one
    // exists to stop frames that never went through feature 18 being published
    // as neural output; an upscale-only job makes no such claim, and its result
    // says neural=false with verifiedNeuralFrames=0 so nothing downstream can
    // infer one. It is held to the opposite claim instead. With the add-on
    // disabled the session log names no feature-18 evaluation; one that does
    // means the add-on ran, and the frames are neural output under a label
    // that says otherwise - exactly the byte-identical pair this job existed
    // to end.
    if(!request.requireNeural&&(result.evidence.feature18Created||result.evidence.feature18Evaluated)){
        return fail(NeuralRenderFailure::Neural,
                    L"The neural add-on ran in a job that asked for Super Resolution alone, "
                    L"so its frames are not Super Resolution-only output.");
    }
    if(request.requireNeural&&!result.evidence.Valid()){
        return fail(NeuralRenderFailure::Neural,
                    L"Feature 18 runtime evidence was incomplete or contained a later failure.");
    }
    // The add-on's log counter can only be watched to advance once per process
    // (see the receipt gate). A fresh evaluator is held to it for the job as a
    // whole: the baseline was read when the feature was armed, so an advance
    // the first pass watched still counts after a software-encoder retry, and
    // a retry whose first pass never got that far is still refused when the
    // counter never moved. Only the in-loop gate skips it on the retry. A
    // reused evaluator cannot be held to it at all. Every job is held to the
    // backend's own count as well, which is per process, monotonic, and has to
    // have advanced at least once for every frame this attempt captured - the
    // count the cache entry then carries as its own evidence.
    if(request.requireNeural&&!evaluatorReused&&
       result.evidence.highestObservedEvaluation<=successfulAttemptBaseline){
        return fail(NeuralRenderFailure::Neural,
                    L"Feature 18 runtime evidence did not advance after captured rendering.");
    }
    // The backend's count is NGX's own tally of Evaluate calls, which is the
    // Super Resolution carrier whichever job this is. A job that asked for
    // Super Resolution alone is held to it too: a captured frame NGX never
    // evaluated is the source at its own size, not an upscale.
    if(attempt.neuralEvaluations<attempt.frames){
        std::wostringstream detail;
        if(request.requireNeural){
            detail<<L"The neural backend evaluated "<<attempt.neuralEvaluations
                  <<L" frames while "<<attempt.frames<<L" were captured, so the captured frames "
                  <<L"are not all neural output.";
        }else{
            detail<<L"DLSS Super Resolution evaluated "<<attempt.neuralEvaluations
                  <<L" frames while "<<attempt.frames<<L" were captured, so the captured frames "
                  <<L"are not all upscaled.";
        }
        return fail(NeuralRenderFailure::Neural,detail.str());
    }
    if(request.requireNeural&&!NeuralTimingClearsFloor(result.timing,outputWidth,outputHeight)){
        std::wostringstream detail;
        detail<<std::fixed<<std::setprecision(2)
              <<L"The neural pass did not run: median neural GPU time was "<<result.timing.neuralGpuMsP50
              <<L" ms per frame at "<<outputWidth<<L"x"<<outputHeight<<L", below the "
              <<NeuralGpuMsFloor(outputWidth,outputHeight)
              <<L" ms floor for that geometry, so the frames are upscaler output. "
              <<L"Check that the neural add-on is loaded and that feature 18 stays armed, then render again.";
        return fail(NeuralRenderFailure::Neural,detail.str());
    }
    result.ok=true;result.encoder=selected;result.frameCount=attempt.frames;
    result.nativeEvaluations=attempt.neuralEvaluations;
    result.verifiedNeuralFrames=request.requireNeural?attempt.frames:0;
    result.firstTimestamp100ns=attempt.firstTimestamp;
    result.duration100ns=attempt.lastTimestamp-attempt.firstTimestamp+frameDuration;
    emit(NeuralRenderPhase::Ready,attempt.frames,attempt.bytes,false);
    return result;
}

// The processing-scale reduction, applied where the frame is read so it runs
// on the prefetch thread beside the render rather than inside it. Inactive at
// 100%, where frames pass through untouched.
struct FrameReduction {
    uint32_t fromWidth{},fromHeight{},toWidth{},toHeight{};
    bool Active()const{return toWidth&&toHeight&&(toWidth!=fromWidth||toHeight!=fromHeight);}
    void Configure(const NeuralRenderRequest& request){
        const ProcessingInput input=ProcessingSize(request.width,request.height,request.processingScale);
        fromWidth=request.width;fromHeight=request.height;
        toWidth=input.reduced?input.width:0;toHeight=input.reduced?input.height:0;
    }
    // Reduces `frame` in place. `spare` is a buffer to write into, typically the
    // reduced frame the caller handed back; the full-size one is returned in
    // it, for a decoder to reuse. False for a frame that is not the size the
    // job was probed at, which the job reports as a source failure.
    bool Apply(std::vector<uint8_t>& frame,std::vector<uint8_t>& spare)const{
        if(!frame_resample::DownscaleBgraArea(frame,fromWidth,fromHeight,toWidth,toHeight,spare))return false;
        frame.swap(spare);
        return true;
    }
};

struct InjectedSourceAdapter {
    IFrameSource& source;
    bool gpuConversion{true};
    FrameReduction reduction;
    bool Open(const std::filesystem::path& path,std::stop_token stop,double seekSeconds){return source.Open(path,stop,seekSeconds);}
    void Close(){source.Close();}
    // An injected source hands out BGRA; only the production decoder can choose NV12.
    PixelLayout Layout()const{return PixelLayout::Bgra;}
    // BGRA needs no conversion, so an injected source declares nothing and nothing
    // reads this - the same position an undeclared real source is left in.
    SourceColorDescription ColorDescription()const{return {};}
    JobRead Read(JobFrame& frame,std::stop_token stop){
        OfflineDecodedFrame decoded;const auto read=source.Read(decoded,stop);
        frame={std::move(decoded.bgra),decoded.timestamp100ns,decoded.discontinuity,
               decoded.frameNumber,decoded.sourceGeneration};
        if(read==OfflineFrameRead::FrameReady&&reduction.Active()){
            std::vector<uint8_t> spare;
            if(!reduction.Apply(frame.bgra,spare))return JobRead::Error;
        }
        switch(read){
            case OfflineFrameRead::FrameReady:return JobRead::FrameReady;
            case OfflineFrameRead::EndOfStream:return JobRead::EndOfStream;
            case OfflineFrameRead::Cancelled:return JobRead::Cancelled;
            default:return JobRead::Error;
        }
    }
};
struct InjectedEvaluatorAdapter {
    INeuralFrameEvaluator& evaluator;
    // The injected interface stays synchronous; these shims give RunJob the same async shape
    // the production adapter has, so the pipelined control flow is what the caller runs.
    std::deque<JobEvaluation> captured{};
    bool Initialize(HWND window,uint32_t width,uint32_t height,uint32_t outputWidth,uint32_t outputHeight,double fps,const GuideControls& guides,
                    PixelLayout,const SourceColorDescription&){
        return evaluator.Initialize(window,width,height,outputWidth,outputHeight,fps,guides);
    }
    // A test evaluator owns no device and is constructed per Run, so it never
    // inherits an armed feature. Answered here rather than left to RunJob's
    // fallback so the job's reuse branches stay runtime branches in this build
    // too, and the tests exercise the same control flow production does.
    bool Reused()const{return false;}
    bool Submit(const JobFrame& frame,const FrameIdentity& id,bool capture,JobEvaluation& out){
        OfflineEvaluation evaluation;
        if(!evaluator.Submit(OfflineDecodedFrame{frame.bgra,frame.timestamp100ns,frame.discontinuity,
                                                 frame.frameNumber,frame.sourceGeneration},
                             id,capture,evaluation))return false;
        out.bgra=std::move(evaluation.bgra);out.id=evaluation.id;
        out.neuralGpuMs=evaluator.LastNeuralGpuMs();return true;
    }
    // Production learns which frame a pipelined capture holds only when its readback
    // slot resolves, so the identity the evaluator stamped travels with the pixels and
    // is judged at the drain. What the submit hands back is the request's source
    // identity with the evaluator's own history decision, as production's guide does.
    bool SubmitAsync(const JobFrame& frame,const FrameIdentity& id,JobEvaluation& out){
        if(!Submit(frame,id,true,out))return false;
        captured.push_back(out);
        out.id=FrameIdentity{id.frameNumber,id.pts100ns,id.sourceGeneration,
                             out.id.historyGeneration,id.jobId,out.id.reset};
        return true;
    }
    uint32_t Pending()const{return uint32_t(captured.size());}
    static constexpr uint32_t MaxPending(){return 2u;}
    // The test evaluator always captures BGRA.
    static constexpr EncoderPixelFormat CapturePixelFormat(){return EncoderPixelFormat::Bgra;}
    bool ResolveOldest(std::vector<uint8_t>& pixels,FrameIdentity& id,double& captureMs){
        if(captured.empty())return false;
        pixels=std::move(captured.front().bgra);id=captured.front().id;captured.pop_front();
        captureMs=0.0;return true;
    }
    void DiscardPending(){captured.clear();}
    bool FeatureCreated()const{return evaluator.FeatureCreated();}
    uint64_t EvaluationCount()const{return evaluator.EvaluationCount();}
    uint64_t NeuralEvaluations()const{return evaluator.NeuralEvaluations();}
    void ResetTemporal(){evaluator.ResetTemporal();}
    bool RequestFeatureRehook(){return evaluator.RequestFeatureRehook();}
    NeuralRenderFailure LastFailure()const{return evaluator.LastFailure();}
    uint64_t PeakLocalVideoMemoryMiB()const{return evaluator.PeakLocalVideoMemoryMiB();}
};
struct InjectedEncoderAdapter {
    IFrameEncoder* encoder{};
    std::unique_ptr<IFrameEncoder> owned;
    std::function<std::unique_ptr<IFrameEncoder>()> factory;
    std::vector<std::vector<uint8_t>> recycled{};
    EncodeError Start(const EncoderSpec& spec,const std::filesystem::path& path){
        recycled.clear();return encoder->Start(spec,path);
    }
    EncodeError WriteFrame(std::span<const uint8_t> frame,std::stop_token stop){return encoder->WriteFrame(frame,stop);}
    // The single-file write path feeds the real encoder from a worker thread; the
    // injected shim writes synchronously and recycles the buffer exactly like production.
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
    std::unique_ptr<InjectedEncoderAdapter> Create(){
        std::unique_ptr<IFrameEncoder> made=factory?factory():nullptr;
        if(!made)return {};
        auto adapter=std::make_unique<InjectedEncoderAdapter>();
        adapter->encoder=made.get();adapter->owned=std::move(made);adapter->factory=factory;
        return adapter;
    }
};
struct ProductionSourceAdapter {
    VideoDecoder decoder;
    bool gpuConversion{true};
    FrameReduction reduction;
    bool Open(const std::filesystem::path& path,std::stop_token stop,double seekSeconds){
        if(!decoder.OpenSequential(path.wstring(),MediaSourceKind::LocalFile,stop,gpuConversion))return false;
        return seekSeconds<=0.0||decoder.SeekSeconds(seekSeconds);
    }
    void Close(){decoder.Close();}
    // Fixed for the decoder session once Open has probed the source (NV12 only for
    // even sizes that also declared a colour description the GPU conversion
    // implements, BGRA otherwise), so the evaluator can be initialized for it.
    PixelLayout Layout()const{return decoder.PixelLayout();}
    // What the probe read off the source. Nv12 above already implies this names a
    // conversion the shader has; it travels on so the shader can be specialised
    // for it instead of assuming one.
    SourceColorDescription ColorDescription()const{return decoder.ColorDescription();}
    JobRead Read(JobFrame& frame,std::stop_token stop){
        VideoFrame decoded;
        // Whatever this frame still carries has already been rendered and written,
        // so it goes back to the decoder rather than being freed here and
        // reallocated - and zero-filled - by the next pipe read. A reduced frame
        // is the wrong size for the decoder's pool; it becomes the next
        // reduction's destination instead.
        std::vector<uint8_t> spent=std::move(frame.bgra);
        if(!reduction.Active())decoder.RecycleFrameBuffer(std::move(spent));
        const auto read = decoder.ReadNextBlocking(decoded, stop);
        frame = {std::move(decoded.bgra), decoded.timestamp100ns, decoded.discontinuity,
                 decoded.frameNumber, decoded.sourceGeneration};
        if (read == VideoReadResult::FrameReady && reduction.Active()) {
            if (!reduction.Apply(frame.bgra, spent)) return JobRead::Error;
            decoder.RecycleFrameBuffer(std::move(spent));
        }
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

// The fence wait and the readback copy. Kept beside the alias so DeferredCaptureWorker
// stays free of D3D12 and its lifecycle can be tested without a device.
struct D3D12CaptureCopy {
    void operator()(D3D12Renderer::CaptureReadbackView& view,std::vector<uint8_t>& pixels)const{
        D3D12Renderer::WaitAndCopyCaptureView(view,pixels);
    }
};
using DeferredCapture=DeferredCaptureWorker<D3D12Renderer::CaptureReadbackView,D3D12CaptureCopy>;

struct ProductionEvaluatorAdapter {
    D3D12RendererOwner renderer;uint64_t successfulEvaluations{};
    TemporalGuideGenerator guides;
    uint32_t width{},height{};
    // Capture size. Equal to width/height unless the job upscales, and part of
    // the reuse comparison because D3D12Renderer sizes the swapchain, the NGX
    // feature and the readback from it and can resize none of them.
    uint32_t outputWidth{},outputHeight{};
    double fps{};
    NeuralRenderFailure lastFailure{NeuralRenderFailure::None};
    // Requested before Initialize; the renderer decides what it can actually deliver.
    bool gpuColorConversion{false};
    // What the live renderer was actually built for, so Initialize can tell a
    // retained device that still fits this job from one that does not.
    bool builtGpuColorConversion{false};
    // The job's temporal choices (TemporalSettings.h), set before Initialize like the
    // conversion flags above. Pure CPU state, applied wherever the guide controls are.
    TemporalSettings temporal{};
    // Benchmark guide sources (GuideFiles.h), set before Initialize like the temporal
    // choices. Applied inside the guide stage, so the stage clock counts a file read
    // where it would have counted the estimator.
    guide_files::Sources guideFiles{};
    bool ApplyGuideFiles(const FrameIdentity& id,GuideFrame& guide){
        std::string error;
        if(guide_files::Apply(guideFiles,id.frameNumber,guide,&error))return true;
        LOG("Benchmark guide files failed at frame#"<<id.frameNumber<<": "<<error);
        lastFailure=NeuralRenderFailure::Source;return false;
    }
    void ApplyGuideSettings(const GuideControls& controls){
        guides.SetControls(controls);guides.SetSceneCutSensitivity(temporal.sceneCuts);
        if(renderer)renderer->SetTemporalStability(temporal.stability);
    }
    // Layout of the frames the source hands over, converted on the GPU when NV12.
    PixelLayout sourceLayout{PixelLayout::Bgra};
    // Which conversion the live renderer's source pass was COMPILED for, which is
    // Unsupported for a BGRA source because no such pass exists then. The program
    // is specialised at bring-up and has no setter, so a job whose source declares
    // a different matrix or range cannot inherit this device.
    SourceNv12Conversion sourceConversion{SourceNv12Conversion::Unsupported};
    // True when the last Initialize kept a retained device and NGX instance
    // instead of building them. The job then paid no neural bring-up; whether
    // it also skipped the feature arm is `featureReleasedWhileIdle`.
    bool reused=false;
    // Set when the idle policy handed the feature-18 workset back between
    // jobs. The device, the NGX instance and the negotiated sizes all survived
    // that, so the next job keeps them and re-arms the feature alone - which
    // is the whole trade the FreeFeature arm makes.
    bool featureReleasedWhileIdle=false;
    // Set before Initialize for a job below 100% processing scale and for an
    // export that upscales (SuperResolutionCarrier): the carrier is then true
    // Super Resolution from the frame the model is shown, created at exactly
    // that size (the renderer's preserve-source mode), rather than DLAA at the
    // output size over an input the renderer resampled to it - which is how an
    // upscaling export's carrier used to run.
    bool superResolutionCarrier=false;
    // The output DLSS settled for. Preserve-source asks the runtime which
    // input sizes an output admits and shrinks the output when the source is
    // below its minimum; the job has sized its encoder for the requested one.
    std::pair<uint32_t,uint32_t> OutputSize()const{
        return renderer?std::pair{renderer->OutputW(),renderer->OutputH()}:std::pair{outputWidth,outputHeight};
    }
    // What the live renderer was built with, for the reuse comparison.
    bool builtSuperResolutionCarrier=false;
    bool Reused()const{return reused;}
    bool Initialize(HWND window,uint32_t w,uint32_t h,uint32_t ow,uint32_t oh,double rate,const GuideControls& controls,
                    PixelLayout layout,const SourceColorDescription& color){
        if(!ow||!oh){ow=w;oh=h;}
        const SourceNv12Conversion conversion=layout==PixelLayout::Nv12
            ?SourceNv12ConversionFor(color):SourceNv12Conversion::Unsupported;
        // Everything compared here is fixed at bring-up and has no setter: the
        // swapchain and the NGX feature are sized by Initialize, the capture
        // format picks the readback layout, and the source layout picks the
        // upload path. D3D12Renderer cannot resize any of them, so a job that
        // differs in any one of them gets the device built again - which is
        // also the only way to release the feature the add-on holds.
        //
        // A feature handed back while idle is the one exception: nothing the
        // device holds changed, only the workset, so the job re-arms it rather
        // than rebuilding everything underneath it.
        reused=renderer&&(renderer->DLSSFeatureCreated()||featureReleasedWhileIdle)&&
               width==w&&height==h&&outputWidth==ow&&outputHeight==oh&&fps==rate&&
               sourceLayout==layout&&sourceConversion==conversion&&
               builtGpuColorConversion==gpuColorConversion&&
               builtSuperResolutionCarrier==superResolutionCarrier;
        // Answered, so spent: this job either re-arms the released feature or
        // rebuilds the device, and either way the next Initialize must judge
        // the feature on what it can see rather than on a stale promise.
        featureReleasedWhileIdle=false;
        if(reused){
            // Guide controls are pure CPU state and are the one thing a job may
            // change without rebuilding anything.
            ApplyGuideSettings(controls);return true;
        }
        Release();
        width=w;height=h;outputWidth=ow;outputHeight=oh;fps=rate;sourceLayout=layout;sourceConversion=conversion;
        const auto [gridW,gridH]=TemporalGuideGenerator::AnalysisGrid(w,h,rate);
        renderer=MakeD3D12Renderer();
        if(!renderer)return false;
        builtGpuColorConversion=gpuColorConversion;
        builtSuperResolutionCarrier=superResolutionCarrier;
        renderer->SetCaptureFormat(gpuColorConversion?CaptureFormat::Nv12:CaptureFormat::Bgra);
        renderer->SetSourceLayout(layout);
        renderer->SetSourceColor(color);
        // This swapchain is a hidden formality that exists so the neural add-on sees a
        // present per frame; no one ever looks at it, and holding presents to the display
        // refresh would cap an export that already runs below real time.
        renderer->SetPresentTearing(true);
        // ow/oh, not w/h: this is where Super Resolution either happens or does
        // not. They are equal for every job that does not upscale, which is how
        // this read for as long as the pass could only render at source size.
        if(!renderer->Initialize(window,w,h,ow,oh,gridW,gridH,DefaultNeuralCarrierQuality(),superResolutionCarrier,true))return false;
        // Both sides apply the same even-size rule, so this only fires if that rule drifts.
        if(renderer->ActiveSourceLayout()!=layout){
            LOG("Renderer could not take the decoder's "<<(layout==PixelLayout::Nv12?"NV12":"BGRA")<<" source layout.");
            return false;
        }
        // Nobody looks at this renderer's swapchain - the encoder is fed from the cache
        // render target EnqueueEvaluatedFrameCapture draws for itself - but every frame
        // still presents: the RenoDX add-on performs its feature-18 pass per present.
        ApplyGuideSettings(controls);renderer->SetDLSS(true);return true;
    }
    // Drops the device, the NGX instance and the feature-18 workset the add-on
    // holds. The readback worker is joined first: it copies out of mapped
    // memory the renderer owns, which the reset below would unmap under it.
    void Release(){
        DiscardPending();deferred.Shutdown();renderer.reset();
        successfulEvaluations=0;lastFailure=NeuralRenderFailure::None;resolveBroken=false;
        featureReleasedWhileIdle=false;
    }
    // Hands the feature-18 workset back between jobs, keeping everything else
    // this adapter retains. Only legal with no job running: the pending
    // captures are drained first, and the renderer waits for every command
    // list that referenced the feature before releasing it, which is what the
    // DLSS guide S5.5 requires and what a mid-job release would break.
    bool ReleaseFeatureForIdle(){
        if(!renderer||!renderer->DLSSFeatureCreated())return false;
        DiscardPending();
        if(!renderer->ReleaseDLSSFeatureForIdle())return false;
        featureReleasedWhileIdle=true;
        return true;
    }
    // What the adapter's device is holding right now, rather than the peak it
    // reached during a job.
    uint64_t CurrentLocalVideoMemoryMiB()const{
        return renderer?renderer->CurrentLocalVideoMemoryMiB():0;
    }
    // Per-job state on an evaluator that is about to serve another job. Each of
    // these would otherwise describe the previous one:
    //  - successfulEvaluations becomes result.nativeEvaluations, which a
    //    successful result is required to have earned itself;
    //  - TemporalGuideGenerator::Reset deliberately keeps its scene-cut tally
    //    ("evidence about the whole job") and its history generation keeps
    //    climbing, so the generator is replaced rather than reset;
    //  - a posted readback copy still holds a capture slot;
    //  - the renderer's peak local video memory is a running maximum, so the
    //    previous job's peak would be reported as this job's.
    // NGX temporal history needs nothing here: the job's first submit carries
    // HistoryReset::FirstFrame, which the renderer acts on.
    void ResetForJob(const GuideControls& controls){
        DiscardPending();
        successfulEvaluations=0;lastFailure=NeuralRenderFailure::None;
        guides={};ApplyGuideSettings(controls);
        guideCost={};
        captureScratch.pixels.clear();captureScratch.id={};
        if(renderer){renderer->ResetStageCounters();renderer->ResetPeakLocalVideoMemory();}
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
    // reset or a cut detected by the guide generator). A capture carries the
    // identity its readback slot recorded when the copy was queued, so the job
    // can verify it received the frame it submitted.
    // The guide's motion in analysis-grid cells, for the render report. The grid
    // carries render pixels, and this adapter always renders at the source size.
    static void CopyGuideMotion(const GuideFrame& guide,uint32_t renderWidth,uint32_t renderHeight,JobEvaluation& out){
        out.gridWidth=guide.gridW;out.gridHeight=guide.gridH;
        const size_t cells=size_t(guide.gridW)*guide.gridH;
        if(!cells||!renderWidth||!renderHeight||guide.guideGridRGBA32F.size()<cells*4u){out.motionCells.clear();return;}
        const float toCellsX=float(guide.gridW)/float(renderWidth),toCellsY=float(guide.gridH)/float(renderHeight);
        out.motionCells.resize(cells*2u);
        for(size_t cell=0;cell<cells;++cell){
            out.motionCells[cell*2u]=guide.guideGridRGBA32F[cell*4u]*toCellsX;
            out.motionCells[cell*2u+1u]=guide.guideGridRGBA32F[cell*4u+1u]*toCellsY;
        }
    }
    bool Submit(const JobFrame& frame,const FrameIdentity& id,bool capture,JobEvaluation& out){
        GuideFrame guide;const auto guideStart=SteadyClock::now();
        {
            StageClock clock(guideCost);
            if(!guides.Generate(frame.bgra.data(),frame.bgra.size(),width,height,width,height,fps,id,guide,sourceLayout)){
                lastFailure=NeuralRenderFailure::Neural;return false;
            }
            if(guideFiles.Active()&&!ApplyGuideFiles(id,guide))return false;
        }
        out.guideMs=MillisecondsSince(guideStart);
        if(capture)CopyGuideMotion(guide,width,height,out);
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
              <<" ms, Present "<<NanosPerFrameMillis(renderer->PresentNanos(),frames)
              <<" ms; capture fence wait on the copy worker "
              <<NanosPerFrameMillis(renderer->CaptureWorkerWaitNanos(),frames)<<" ms, overlapped.";
        return detail.str();
    }
    // Records the capture without waiting for the GPU. The pixels come back later from
    // ResolveOldest, which waits only on that one frame's fence.
    bool SubmitAsync(const JobFrame& frame,const FrameIdentity& id,JobEvaluation& out){
        GuideFrame guide;const auto guideStart=SteadyClock::now();
        {
            StageClock clock(guideCost);
            if(!guides.Generate(frame.bgra.data(),frame.bgra.size(),width,height,width,height,fps,id,guide,sourceLayout)){
                lastFailure=NeuralRenderFailure::Neural;return false;
            }
            if(guideFiles.Active()&&!ApplyGuideFiles(id,guide))return false;
        }
        out.guideMs=MillisecondsSince(guideStart);
        CopyGuideMotion(guide,width,height,out);
        const float frameMs=static_cast<float>(1000.0/fps);
        if(!renderer->RenderFrame(frame.bgra.data(),frame.bgra.size(),id,guide,frameMs)){
            lastFailure=ClassifyRendererFailure(*renderer);return false;
        }
        if(!renderer->EnqueueEvaluatedFrameCapture()){
            lastFailure=ClassifyRendererFailure(*renderer);return false;
        }
        // The guide's identity: which frame the capture holds is only known when its
        // readback slot resolves, and ResolveOldest hands that back for the drain.
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
    bool ResolveOldest(std::vector<uint8_t>& pixels,FrameIdentity& id,double& captureMs){
        if(!renderer||resolveBroken)return false;
        std::vector<uint8_t> spare=std::move(pixels);pixels.clear();
        const auto captureStart=SteadyClock::now();
        bool ok;
        if(deferred.Posted()){
            // The view comes back as the worker left it: the identity its slot recorded,
            // and how the fence wait it ran went.
            D3D12Renderer::CaptureReadbackView view;
            const bool joined=deferred.Join(pixels,&view);
            ok=joined&&renderer->CompleteReservedCapture(view);
            id=view.id;
        }else{
            // The job's first drain, and the tail flush once the ring has run dry: with
            // nothing posted there is nothing to overlap, so this one is copied inline.
            captureScratch.pixels=std::move(spare);spare.clear();
            ok=renderer->ResolveOldestCapture(captureScratch);
            pixels=std::move(captureScratch.pixels);captureScratch.pixels.clear();
            id=captureScratch.id;captureScratch.id={};
        }
        captureMs=MillisecondsSince(captureStart);
        if(!ok){
            // A device removal or fence timeout in the readback must be reported as such,
            // not as whatever failure the previous frame left behind.
            lastFailure=ClassifyRendererFailure(*renderer);
            return false;
        }
        PostNextResolve(std::move(spare));
        return true;
    }
    // Reserves the oldest remaining capture's readback slot and starts its wait and copy
    // on the worker, so the render thread no longer parks on that capture's fence here. A
    // slot that cannot be opened has already been retired by the renderer, which would
    // slide every later frame's pixels one place against the job's deque, so the failure
    // is latched and the next drain reports it instead.
    void PostNextResolve(std::vector<uint8_t>&& scratch){
        if(!renderer||!renderer->PendingCaptureCount())return;
        D3D12Renderer::CaptureReadbackView view;
        if(!renderer->ReserveOldestCapture(view)){
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
        D3D12Renderer::CaptureReadbackView view;
        if(deferred.Join(dropped,&view))renderer->CompleteReservedCapture(view);
        resolveBroken=false;
        while(renderer->PendingCaptureCount()){
            CapturedVideoFrame discarded;
            if(!renderer->ResolveOldestCapture(discarded))break;
        }
    }
    // What Initialize settled on, which is BGRA unless the GPU conversion was both
    // asked for and possible at this size.
    EncoderPixelFormat CapturePixelFormat()const{
        return renderer&&renderer->ActiveCaptureFormat()==CaptureFormat::Nv12
            ?EncoderPixelFormat::Nv12:EncoderPixelFormat::Bgra;
    }
    bool FeatureCreated()const{return renderer&&renderer->DLSSFeatureCreated();}
    uint64_t EvaluationCount()const{return successfulEvaluations;}
    // The NGX backend's own count of Evaluate calls that returned success. Unlike
    // the add-on's log counter this is per process and monotonic with no logging
    // cadence in the way, so a job that reused an armed feature can still prove
    // the neural pass ran once per frame it captured.
    uint64_t NeuralEvaluations()const{return renderer?renderer->DLSSEvaluations():0;}
    void ResetTemporal(){guides.Reset();DiscardPending();}
    bool RequestFeatureRehook(){
        if(!renderer)return false;
        renderer->RequestDLSSRecreate();return true;
    }
    NeuralRenderFailure LastFailure()const{return lastFailure;}
    uint64_t PeakLocalVideoMemoryMiB()const{return renderer?renderer->PeakLocalVideoMemoryMiB():0;}
    const SceneCutAccounting& SceneCuts()const{return guides.SceneCuts();}
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

    // Drains the queue through the worker. A job stop arriving mid-drain (up to eight
    // 4K frames, or forever if ffmpeg has wedged) requests the worker's own token,
    // which is the only thing that releases a blocked WriteFile; the caller then takes
    // the Cancel path exactly as an inline WriteFrame(stop) used to.
    EncodeError Flush(std::stop_token stop){
        {std::lock_guard lock(mutex);draining=true;}
        cv.notify_all();
        if(worker.joinable()){
            std::stop_callback release(stop,[this]{worker.request_stop();});
            worker.join();
        }
        std::lock_guard lock(mutex);
        if(latched==EncodeError::None&&stop.stop_requested())latched=EncodeError::Cancelled;
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
    // platform_paths distinguishes "could not be determined" from a truncated
    // path; these callers have always treated an empty path as the former.
    return platform_paths::ModuleDirectory().value_or(std::filesystem::path{});
}

} // namespace

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

namespace {

// What has been read of one session log so far. The log is append-only for
// the life of the proxy that writes it, so every read after the first fetches
// only the bytes appended since the last one instead of the whole file again.
// A different file, or one shorter than what was read, starts over.
class SessionLogTail {
public:
    // Brings the text up to the file's current end. False when the file cannot
    // be read or is larger than `limit`, which the caller reports as unreadable.
    bool Refresh(const std::filesystem::path& path, uintmax_t limit)
    {
        if (path.empty()) return false;
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(path, error);
        if (error || size > limit) return false;
        if (path != path_ || size < text_.size()) {
            path_ = path;
            text_.clear();
        }
        if (size == text_.size()) return true;
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        const size_t read = text_.size();
        input.seekg(static_cast<std::streamoff>(read));
        text_.resize(static_cast<size_t>(size));
        input.read(text_.data() + read, static_cast<std::streamsize>(size - read));
        text_.resize(read + static_cast<size_t>(std::max<std::streamsize>(0, input.gcount())));
        return true;
    }
    const std::string& Text() const { return text_; }

private:
    std::filesystem::path path_;
    std::string text_;
};

// The session-log read, with the two knobs residency needs.
//
// `resolve` is called per attempt because a single-shot helper may start before
// its own proxy has written anything, so which candidate is newest can change
// while this waits. A resident helper pins its answer instead - see
// SessionEvidence.
//
// `stabilize` waits for the add-on's asynchronous arming to appear and for the
// file to stop growing. Without it the log is read once and returned as it
// stands, which is all a job that reused an already-armed feature needs, and
// all the receipt gate's polls need.
//
// `tail` carries what earlier reads already fetched, so a poll reads only what
// was appended since.
template <class Resolve>
std::string ReadSessionLog(Resolve resolve, bool stabilize, SessionLogTail& tail)
{
    constexpr uintmax_t Limit=4u*1024u*1024u;std::string latest;
    size_t lastSize=std::numeric_limits<size_t>::max();
    int stableSamples=0;
    for(int attempt=0;attempt<20;++attempt){
        if(tail.Refresh(resolve(),Limit)){
            latest=tail.Text();
            if(!stabilize)return latest;
            const auto evidence=ParseNeuralRuntimeEvidence(latest);
            stableSamples=latest.size()==lastSize?stableSamples+1:1;
            lastSize=latest.size();
            if((evidence.Valid()||evidence.laterFailure)&&stableSamples>=3)return latest;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return latest;
}

} // namespace

std::string ReadNeuralRuntimeSessionLog(const std::filesystem::path& runtimeDirectory)
{
    SessionLogTail tail;
    return ReadSessionLog([&]{return ResolveNeuralRuntimeLogPath(runtimeDirectory);},true,tail);
}

std::string ReadNeuralRuntimeSessionLogSnapshot(const std::filesystem::path& runtimeDirectory)
{
    SessionLogTail tail;
    return ReadSessionLog([&]{return ResolveNeuralRuntimeLogPath(runtimeDirectory);},false,tail);
}


NeuralRuntimeEvidence ParseNeuralRuntimeEvidence(std::string_view reshadeLogSegment)
{
    NeuralRuntimeEvidence evidence;const std::string lower=LowerAscii(reshadeLogSegment);
    // Proof that the neural pass ran 1:1 rather than upscaling, taken from the
    // resources and the evaluate rather than from a settings echo.
    //
    // 4.70 announced "active settings: upscaling=OFF", built from the
    // NREnableUpscaling key. 6.x deleted that key - the working resolution is
    // now a mode plus a scale - and the settings line it prints no longer
    // mentions upscaling at all, so the old probe reads false on every 6.x log
    // and Valid() rejects a render that was perfectly good. Both versions say
    // what the resources and the evaluate actually were, which is the better
    // evidence anyway: a config echo states an intention, "(native 1:1)" and
    // "[native]" state an outcome. 4.70 writes "(native)", 6.5.3 "(native 1:1)".
    //
    // A pre-SR evaluate - the model on the carrier's input, ahead of its
    // upscale, which is how a reduced processing scale runs - tags its line
    // "[pre-SR]" instead of "[native]". It is the same outcome at the model's
    // own size: RenoDX 6.5.3 logs "created inline NR resources ws1 1440x810 ->
    // 1440x810 (native 1:1)" and "pre-SR feature 18 evaluation succeeded (...
    // NR input 1440x810 ..., output 1440x810, 1 stack pass(es) [pre-SR])" for
    // a 75% render of a 1920x1080 source, measured on this project's own run.
    evidence.nativeResolution=
        lower.find("created inline nr resources")!=std::string::npos&&
        lower.find("(native")!=std::string::npos&&
        (lower.find("[native]")!=std::string::npos||lower.find("[pre-sr]")!=std::string::npos);
    // That the pass came through this player's own inline NGX path and not the
    // host's Streamline route. 4.70's half of this was the startup banner
    // "private feature-18 GPU ordering active"; 6.x prints no architecture
    // banners at all, so the surviving proof is the hook mode the add-on
    // reports plus the inline resources it created for our feature.
    evidence.inlineInterceptionContract=
        lower.find("enablehooks=2: ngx hooks only")!=std::string::npos&&
        lower.find("created inline nr resources")!=std::string::npos;
    const size_t created=lower.find("feature 18 created");
    // 6.x splits the evaluate log by path: the inline line stayed, and a
    // "pre-SR" line joined it for NR ahead of a host upscale. This player owns
    // both sides of its own pipeline, so either line is the frame it asked for.
    const bool evaluated=
        lower.find("inline feature 18 evaluation succeeded")!=std::string::npos||
        lower.find("pre-sr feature 18 evaluation succeeded")!=std::string::npos;
    evidence.feature18Created=created!=std::string::npos;
    evidence.feature18Evaluated=evaluated;
    // Both vocabularies, so pinning either add-on keeps its own failures
    // detected. The 4.70-only entries cost a string search and catch a
    // downgrade; the 6.x entries are the ones that would otherwise let a
    // declined or exception-thrown frame be published as verified.
    const std::array<std::string_view,18> failures{
        // Shared.
        "feature 18 create failed","feature 18 evaluation failed",
        "inline feature 18 evaluation failed","feature 18 evaluate raised an exception",
        "nr skipped:","nr declined an evaluate:",
        "the game dlss output was retained",
        // 4.70 only - gone from 6.x, kept so a re-pin stays covered.
        "nr workset pool exhausted","nr is paused for this feature",
        // 6.x additions. The create path gained an exception log of its own,
        // the evaluate path gained pre-SR and Streamline decline reasons, and
        // the workset pool reports exhaustion as a rejected request now.
        "feature 18 create raised an exception","feature 18 create failed with",
        "feature 18 evaluate failed with","pre-sr feature 18 evaluate raised an exception",
        "pre-sr feature 18 evaluate failed with","pre-sr nr declined:",
        "streamline nr declined:","nr workset request rejected",
        "workset/codec setup failed"};
    // Deliberately NOT in that list, because 6.5.3 says both on a healthy run:
    //   "NR skipped (after-upscale): compute-state restore target incomplete"
    //   "compute-state shadow: ... NR declines frames until the shadow installs"
    // Each is a retry notice whose own next line is "injection admitted after N
    // incomplete-target decline(s)" - the observed render logged two of them and
    // then verified every frame. The colon form "NR skipped:" that IS listed is
    // the terminal one (unsupported list type, no guide dimensions); the
    // parenthesised form is the transient. The frame counts are the real guard
    // here, and a list that fires on a startup retry rejects good renders.
    for(const auto failure:failures){
        if(lower.find(failure)!=std::string::npos)evidence.laterFailure=true;
    }
    evidence.highestObservedEvaluation=HighestEvaluationCount(lower);
    return evidence;
}

namespace {

// The reader a job's evidence comes from, as an object rather than a free
// function, for the two reasons residency introduces.
//
// The log path is pinned after the first attempt that resolves one.
// ResolveNeuralRuntimeLogPath picks the candidate written most recently after
// this process started, which is the right rule for a process that renders once
// and exits. A resident helper outlives other players' helpers, and ReShade
// rotates to ReShade.log1 while this process holds ReShade.log, so re-resolving
// per read lets somebody else's file win a race this process cannot see. Our
// own proxy's file does not change while we live.
//
// The stability wait is skipped for the first read of a job that inherited an
// armed feature. That wait exists for the add-on's asynchronous NGX detour
// arming, which an earlier job in this process already proved; the log is
// append-only within a session, so a single read already contains every line
// that job's final read saw and the baseline it yields cannot be stale-low.
// Every later read in the job - including the one the advance check is made
// against - still waits.
class SessionEvidence {
public:
    explicit SessionEvidence(std::filesystem::path runtimeDirectory)
        : runtimeDirectory_(std::move(runtimeDirectory)) {}

    void BeginJob(bool featureAlreadyArmed) { skipStabilityOnce_ = featureAlreadyArmed; }

    std::string operator()()
    {
        const bool stabilize = !skipStabilityOnce_;
        skipStabilityOnce_ = false;
        return ReadSessionLog([this] { return Pinned(); }, stabilize, tail_);
    }

    // The log as it stands, tailed from the last read: the receipt gate asks
    // this between resubmits, where waiting for the file to settle was at least
    // 200 ms per poll and bought nothing - a line not there yet is there on a
    // later poll, and every verdict is still read through operator().
    std::string Poll()
    {
        return ReadSessionLog([this] { return Pinned(); }, false, tail_);
    }

    // True once the pinned log has grown past what a read can return. Past that
    // point every read comes back empty and every job would fail its evidence
    // check, so the helper stops offering itself for reuse instead of failing
    // the next job to arrive. Only a resident helper can reach it.
    bool LogTooLargeToReuse() const
    {
        if (pinned_.empty()) return false;
        std::error_code error;
        const auto size = std::filesystem::file_size(pinned_, error);
        return !error && size > kReuseLogLimit;
    }

private:
    const std::filesystem::path& Pinned()
    {
        if (pinned_.empty()) pinned_ = ResolveNeuralRuntimeLogPath(runtimeDirectory_);
        return pinned_;
    }

    // Half the read limit, so a job that starts under it cannot grow past it
    // and read empty before it finishes.
    static constexpr uintmax_t kReuseLogLimit = 2u * 1024u * 1024u;
    std::filesystem::path runtimeDirectory_;
    std::filesystem::path pinned_;
    SessionLogTail tail_;
    bool skipStabilityOnce_{};
};

} // namespace

// The production device, evaluator, encoder and session-log reader, kept across
// Run calls. Declared in reverse teardown order: the encoder's ffmpeg child and
// feeder thread go before the device whose readback memory they were fed from,
// and the source decoder last.
struct OfflineNeuralRenderer::Retained {
    explicit Retained(std::filesystem::path runtimeDirectory)
        : evidence(std::move(runtimeDirectory)) {}
    ProductionSourceAdapter source;
    ProductionEvaluatorAdapter evaluator;
    ProductionEncoderAdapter encoder;
    SessionEvidence evidence;
};

OfflineNeuralRenderer::OfflineNeuralRenderer() = default;
OfflineNeuralRenderer::~OfflineNeuralRenderer() = default;

bool OfflineNeuralRenderer::ReusableForAnotherJob() const
{
    return !retained_ || !retained_->evidence.LogTooLargeToReuse();
}

OfflineNeuralRenderer::MemoryFootprint OfflineNeuralRenderer::SampleMemoryFootprint() const
{
    MemoryFootprint footprint;
    if (!retained_) return footprint;
    footprint.localVramMiB = retained_->evaluator.CurrentLocalVideoMemoryMiB();
    footprint.featureArmed = retained_->evaluator.FeatureCreated();
    return footprint;
}

OfflineNeuralRenderer::IdleFeatureRelease OfflineNeuralRenderer::ReleaseIdleFeatureMemory()
{
    IdleFeatureRelease observed;
    observed.before = SampleMemoryFootprint();
    if (retained_) observed.released = retained_->evaluator.ReleaseFeatureForIdle();
    // Sampled after the attempt either way: a release that could not drain the
    // queue still has to report what the adapter says, because "nothing moved"
    // is what separates a refused release from a runtime that declined to free.
    observed.after = SampleMemoryFootprint();
    return observed;
}

OfflineNeuralRenderer::OfflineNeuralRenderer(
    IFrameSource& source,INeuralFrameEvaluator& evaluator,IFrameEncoder& encoder,
    std::function<std::string()> evidenceProvider,Clock clock,std::function<bool()> paused,
    std::function<std::unique_ptr<IFrameEncoder>()> encoderFactory)
    : source_(&source),evaluator_(&evaluator),encoder_(&encoder),
      evidenceProvider_(std::move(evidenceProvider)),clock_(std::move(clock)),
      paused_(std::move(paused)),encoderFactory_(std::move(encoderFactory)) {}

NeuralRenderResult OfflineNeuralRenderer::Run(const NeuralRenderRequest& request,
                                               ProgressCallback progress,std::stop_token stop,
                                               const NeuralSegmentSink& segments,
                                               NeuralColdStartCallback coldStart)
{
    if(source_||evaluator_||encoder_||evidenceProvider_)
    {
        // A partial injection is a programming error, not a degraded run: falling
        // back to the production adapters here would silently ignore the fakes.
        if(!source_||!evaluator_||!encoder_||!evidenceProvider_)
            return NeuralRenderResult{.failure=NeuralRenderFailure::Protocol,
                                      .detail=L"Offline renderer was constructed with an incomplete set of collaborators."};
        InjectedSourceAdapter source{*source_};InjectedEvaluatorAdapter evaluator{*evaluator_};
        source.reduction.Configure(request);
        InjectedEncoderAdapter encoder{encoder_,{},encoderFactory_};
        const Clock clock=clock_?clock_:[]{return SteadyClock::now();};
        const std::function<bool()> paused=paused_?paused_:[]{return false;};
        return RunJob(request,std::move(progress),stop,source,evaluator,encoder,
                      evidenceProvider_,clock,paused,segments,coldStart);
    }
    if(!retained_)retained_=std::make_unique<Retained>(ModuleDirectory());
    Retained& state=*retained_;
    // A job that unwound without closing its decoder must not leave the next
    // one an open one; every other path already closes it.
    state.source.Close();
    state.source.reduction.Configure(request);
    // The reduction reads BGRA; an NV12 source would reach it in the wrong
    // layout, so a reduced job decodes to BGRA whatever the setting says.
    state.source.gpuConversion=request.gpuSourceConversion&&!state.source.reduction.Active();
    state.evaluator.gpuColorConversion=request.gpuColorConversion;
    {
        const ProcessingInput model=ProcessingSize(request.width,request.height,request.processingScale);
        state.evaluator.superResolutionCarrier=SuperResolutionCarrier(model.width,model.height,
            request.outputWidth?request.outputWidth:request.width,request.outputHeight?request.outputHeight:request.height);
    }
    state.evaluator.temporal=request.temporal;
    state.evaluator.guideFiles=request.guideFiles;
    if(request.guideFiles.Active()){
        const auto& files=request.guideFiles;
        LOG("Benchmark guide sources: motion="
            <<(files.motion==guide_files::MotionSource::File?"file:"+files.motionDirectory.string()
               :files.motion==guide_files::MotionSource::Cpu?std::string("cpu"):std::string("estimator"))
            <<" depth="<<(files.depthFromFile?"file:"+files.depthDirectory.string():std::string("estimator"))
            <<" dump="<<(files.dumpDirectory.empty()?std::string("none"):files.dumpDirectory.string()));
    }
    // Read before the reset, because the reset is allowed to drop the feature.
    const bool inheritedArmedFeature=state.evaluator.renderer&&state.evaluator.FeatureCreated();
    state.evaluator.ResetForJob(request.guides);
    state.evidence.BeginJob(inheritedArmedFeature);
    NeuralRenderResult result=RunJob(request,std::move(progress),stop,state.source,
        state.evaluator,state.encoder,std::ref(state.evidence),
        []{return SteadyClock::now();},
        [pauseEvent=request.pauseEvent]{
            return pauseEvent&&WaitForSingleObject(pauseEvent,0)==WAIT_OBJECT_0;
        },segments,coldStart);
    // Three distinguishable outcomes, and the middle one is what the
    // FreeFeature idle policy produces: the device and the NGX instance were
    // inherited but the workset was not, so the job skipped neuralInit and
    // paid featureArm. Reporting that as a reuse would hide the cost the
    // policy is being measured for.
    residency_=state.evaluator.Reused()
        ?(inheritedArmedFeature?Residency::FeatureReused:Residency::FeatureRecreated)
        :inheritedArmedFeature?Residency::FeatureRecreated:Residency::Initialized;
    return result;
}
