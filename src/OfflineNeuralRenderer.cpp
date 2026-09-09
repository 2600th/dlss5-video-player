#include "OfflineNeuralRenderer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
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
// spawned ffmpeg bringing up its encoder (~110 ms of not draining its stdin)
// without stalling the render loop, and bounded in bytes so a large frame size
// cannot balloon the queue.
inline constexpr size_t kQueuedFrameBytes = size_t{64} << 20;

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
    const size_t expectedBytes = static_cast<size_t>(expectedBytes64);
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
        const EncoderSpec spec{request.width, request.height, request.fps, kind};
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
        auto abort = [&](NeuralRenderFailure failure) {
            attempt.failure = failure;
            if (failure == NeuralRenderFailure::Cancelled) attempt.encoderError = EncodeError::Cancelled;
            cancelOutput();
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
            // A segmented job takes ownership of the captured pixels; the
            // single-file encoder still writes them on this thread.
            const uint64_t captured = evaluation.bgra.size();
            const EncodeError writeError = writer
                ? writer->Write(frame, std::move(evaluation.bgra), stop)
                : encoder.WriteFrame(evaluation.bgra, stop);
            if (writeError != EncodeError::None) {
                attempt.failure = writeError == EncodeError::Cancelled
                    ? NeuralRenderFailure::Cancelled : NeuralRenderFailure::Encoder;
                attempt.encoderError=writeError;cancelOutput();return attempt;
            }
            ++attempt.frames;++attempt.evaluations;attempt.bytes+=captured;
            if (!attempt.hasTimestamp) {
                attempt.firstTimestamp=frame.timestamp100ns;
                attempt.hasTimestamp=true;
            }
            attempt.lastTimestamp=frame.timestamp100ns;
            attempt.neuralGpuMs.push_back(evaluation.neuralGpuMs);
            attempt.guideMsTotal+=evaluation.guideMs;attempt.captureMsTotal+=evaluation.captureMs;
            emit(NeuralRenderPhase::NeuralRendering,attempt.frames,attempt.bytes,true);
        }
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
    IFrameEncoder* encoder{};
    std::unique_ptr<IFrameEncoder> owned;
    std::function<std::unique_ptr<IFrameEncoder>()> factory;
    EncodeError Start(const EncoderSpec& spec,const std::filesystem::path& path){return encoder->Start(spec,path);}
    EncodeError WriteFrame(std::span<const uint8_t> frame,std::stop_token stop){return encoder->WriteFrame(frame,stop);}
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
    // One fresh ffmpeg pipe per segment; a single-file job never calls this.
    std::unique_ptr<ProductionEncoderAdapter> Create(){return std::make_unique<ProductionEncoderAdapter>();}
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
    return RunJob(request,std::move(progress),stop,source,evaluator,encoder,
        [runtimeDirectory]{return ReadNeuralRuntimeSessionLog(runtimeDirectory);},
        []{return SteadyClock::now();},
        [pauseEvent=request.pauseEvent]{
            return pauseEvent&&WaitForSingleObject(pauseEvent,0)==WAIT_OBJECT_0;
        },segments);
#endif
}
