#include "NeuralWorker.h"
#include "NarrowText.h"
#include "PlatformPaths.h"
#include "NeuralWorkerProtocol.h"
#include "HardErrorSuppression.h"
#include "AtomicFile.h"
#include "Utf8Text.h"
#include "KillOnCloseJob.h"
#include "StrictJson.h"
#include "UpscalingPolicy.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <future>
#include <iterator>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace neural_worker_protocol;

namespace {

constexpr std::wstring_view kWorkerMode = L"--neural-worker";
constexpr std::wstring_view kPreflightMode = L"--neural-preflight";
constexpr std::wstring_view kRestartedFlag = L"--configuration-restarted";
constexpr std::wstring_view kIdleVramPolicyFlag = L"--idle-vram-policy";
// Section and key of the player's own settings file - not the neural runtime's
// ReShade.ini. The idle-VRAM policy is a resource decision, so it must stay
// out of the settings digest the render cache key hashes: a render made while
// the helper frees memory between jobs is the same render, and flipping this
// must not retire a single cache entry.
constexpr wchar_t kIdleVramPolicySection[] = L"NeuralHelper";
constexpr wchar_t kIdleVramPolicyKey[] = L"IdleVramPolicy";

std::wstring QuoteArgument(std::wstring_view value)
{
    std::wstring quoted(1, L'"');
    size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(slashes, L'\\');
            quoted.push_back(character);
        }
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring MakeCommandLine(const std::filesystem::path& executable,
                             const std::vector<std::wstring>& arguments)
{
    std::wstring command = QuoteArgument(executable.wstring());
    for (const std::wstring& argument : arguments) {
        command.push_back(L' ');
        command += QuoteArgument(argument);
    }
    return command;
}

bool ParseUnsigned(std::wstring_view text, uint64_t& value)
{
    if (text.empty()) return false;
    value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') return false;
        const uint64_t digit = static_cast<uint64_t>(character - L'0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    return true;
}

bool ParseDouble(std::wstring_view text, double& value)
{
    if (text.empty() || text.size() >= 128) return false;
    std::wstring copy(text);
    wchar_t* end = nullptr;
    value = std::wcstod(copy.c_str(), &end);
    return end == copy.c_str() + copy.size() && std::isfinite(value);
}


std::wstring HandleText(HANDLE handle)
{
    return std::to_wstring(reinterpret_cast<uintptr_t>(handle));
}

// The parent's end of the metadata pipe, read by a thread of its own.
//
// The pump used to peek the pipe and, finding it empty, wait 20 ms on the
// helper process before looking again, so every Progress, Segment and Result
// sat in the pipe for up to a poll interval - measured 15-16 ms median, the
// timer tick rounding the 20 ms wait. A Segment is what makes a finished
// segment playable, so that delay came straight out of the live buffer. This
// thread is blocked in ReadFile instead, queues what it reads, and sets
// `ready`, which the pump waits on beside the helper process and its stop
// token.
//
// The queue is bounded, so a helper that writes faster than the parent reads
// is still held back by the pipe, as it always was: the thread stops reading
// at kMetadataReadAheadBytes until the pump takes some. State lives in a
// shared block so a reader that cannot be cancelled can be let go without
// anything it uses being freed under it.
struct MetadataPipeState {
    HANDLE pipe{};
    HANDLE ready{};   // auto-reset: set whenever the queue grows or the read ends
    std::mutex mutex;
    std::condition_variable room;
    std::vector<std::byte> queue;
    bool ended{};     // no more bytes will arrive
    bool failed{};    // ...because a read failed, not because the helper closed its end
    bool stopping{};

    MetadataPipeState() = default;
    MetadataPipeState(const MetadataPipeState&) = delete;
    MetadataPipeState& operator=(const MetadataPipeState&) = delete;
    ~MetadataPipeState()
    {
        if (pipe) CloseHandle(pipe);
        if (ready) CloseHandle(ready);
    }
};

// The pipe's own size (StartHelper), so one read takes whatever one burst of
// the helper's left in it.
constexpr DWORD kMetadataPipeBytes = 64 * 1024;
// How far the reader may run ahead of the pump: one drain budget, which is far
// more than a well-behaved helper writes between two passes.
constexpr size_t kMetadataReadAheadBytes = neural_worker_detail::kMetadataDrainByteBudget;

void ReadMetadataPipe(const std::shared_ptr<MetadataPipeState>& state)
{
    // Allocated once for the pipe's life. The drain this replaces zero-filled
    // a 4 KiB array on every pass of its loop.
    std::vector<std::byte> chunk(kMetadataPipeBytes);
    for (;;) {
        {
            std::unique_lock lock(state->mutex);
            state->room.wait(lock, [&] {
                return state->stopping || state->queue.size() < kMetadataReadAheadBytes;
            });
            if (state->stopping) {
                state->ended = true;
                break;
            }
        }
        DWORD read = 0;
        if (!ReadFile(state->pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)) {
            const DWORD error = GetLastError();
            std::lock_guard lock(state->mutex);
            state->ended = true;
            // The helper closing its end is how every stream finishes, and a
            // cancelled read is this parent finishing it.
            state->failed = error != ERROR_BROKEN_PIPE && !state->stopping;
            break;
        }
        if (!read) continue;
        std::lock_guard lock(state->mutex);
        state->queue.insert(state->queue.end(), chunk.begin(), chunk.begin() + read);
        SetEvent(state->ready);
    }
    SetEvent(state->ready);
}

class MetadataPipe {
public:
    // Takes ownership of `pipe`, the parent's read end.
    explicit MetadataPipe(HANDLE pipe) : state_(std::make_shared<MetadataPipeState>())
    {
        state_->pipe = pipe;
        state_->ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!state_->ready) return;
        try {
            reader_ = std::thread([state = state_] { ReadMetadataPipe(state); });
        } catch (...) {
        }
    }

    ~MetadataPipe()
    {
        {
            std::lock_guard lock(state_->mutex);
            state_->stopping = true;
        }
        state_->room.notify_all();
        if (!reader_.joinable()) return;
        // Normally the read has ended already: a pipe is dropped after its
        // helper is gone, and the helper's write end went with it. A write end
        // something else still holds leaves the thread in ReadFile, which is
        // what the cancel is for; one that will not cancel is let go with the
        // state it owns rather than hanging the caller, as the shutdown write
        // is (SendShutdownAndCloseBounded).
        const HANDLE thread = reader_.native_handle();
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (WaitForSingleObject(thread, attempt ? 50 : 0) == WAIT_OBJECT_0) {
                reader_.join();
                return;
            }
            CancelSynchronousIo(thread);
        }
        reader_.detach();
    }

    MetadataPipe(const MetadataPipe&) = delete;
    MetadataPipe& operator=(const MetadataPipe&) = delete;

    bool Valid() const { return reader_.joinable(); }
    HANDLE Ready() const { return state_->ready; }
    HANDLE Pipe() const { return state_->pipe; }

    size_t Buffered() const
    {
        std::lock_guard lock(state_->mutex);
        return state_->queue.size();
    }

    struct Taken {
        size_t bytes{};
        bool failed{};   // the read failed and nothing it delivered is left
    };

    // Moves at most `budget` queued bytes into `out`, oldest first.
    Taken Take(std::vector<std::byte>& out, size_t budget)
    {
        Taken taken;
        {
            std::lock_guard lock(state_->mutex);
            taken.bytes = std::min(budget, state_->queue.size());
            const auto end = state_->queue.begin() + static_cast<std::ptrdiff_t>(taken.bytes);
            out.assign(state_->queue.begin(), end);
            state_->queue.erase(state_->queue.begin(), end);
            taken.failed = state_->failed && state_->queue.empty();
        }
        if (taken.bytes) state_->room.notify_all();
        return taken;
    }

    // Until the reader has everything the helper wrote, or `budget` runs out.
    // Asked once the helper process is gone: its last bytes may still be on
    // their way from the pipe into the queue.
    void WaitForEnd(DWORD budget)
    {
        const ULONGLONG started = GetTickCount64();
        for (;;) {
            {
                std::lock_guard lock(state_->mutex);
                if (state_->ended) return;
            }
            const ULONGLONG elapsed = GetTickCount64() - started;
            if (elapsed >= budget) return;
            WaitForSingleObject(state_->ready, static_cast<DWORD>(budget - elapsed));
        }
    }

private:
    std::shared_ptr<MetadataPipeState> state_;
    std::thread reader_;
};

class MetadataReader {
public:
    MetadataReader(OfflineNeuralRenderer::ProgressCallback progress, NeuralSegmentSink segments,
                   NeuralColdStartCallback timeline = {})
        : progress_(std::move(progress)), segments_(std::move(segments)),
          timeline_reported_(std::move(timeline)) {}

    // Bounded by kMetadataDrainByteBudget. This is the first statement of
    // every Pump iteration, ahead of the stop check, so a version that ran
    // until the pipe was dry let a helper writing faster than the parent reads
    // make the render uncancellable - the user pressed stop and nothing
    // happened until the helper chose to go quiet. `budgetSpent` tells the
    // pump the pipe still has work, so it checks its stop token and returns.
    bool ReadAvailable(MetadataPipe& pipe, bool* budgetSpent = nullptr, size_t* bytesRead = nullptr)
    {
        const MetadataPipe::Taken taken = pipe.Take(taken_, neural_worker_detail::kMetadataDrainByteBudget);
        if (budgetSpent) *budgetSpent = taken.bytes >= neural_worker_detail::kMetadataDrainByteBudget;
        if (bytesRead) *bytesRead = taken.bytes;
        // Fed in the 4 KiB pieces the pipe drain always used: Push bounds what
        // it holds undecoded, and a whole budget at once would trip that bound
        // on a stream of perfectly good messages.
        constexpr size_t kPiece = 4096;
        for (size_t offset = 0; offset < taken.bytes; offset += kPiece) {
            const size_t piece = std::min(kPiece, taken.bytes - offset);
            if (!Push(std::span<const std::byte>(taken_.data() + offset, piece))) return false;
        }
        if (taken.failed) {
            malformed_ = true;
            return false;
        }
        return true;
    }

    // The same decoder, fed from memory instead of the pipe.
    bool Push(std::span<const std::byte> chunk)
    {
        if (bytes_.size() + chunk.size() > kMaximumPayloadBytes * 2u) {
            malformed_ = true;
            return false;
        }
        bytes_.insert(bytes_.end(), chunk.begin(), chunk.end());
        return Consume();
    }

    bool Complete() const { return !malformed_ && bytes_.empty() && result_.has_value(); }
    bool PreflightComplete() const { return !malformed_ && bytes_.empty() && preflight_.has_value(); }
    bool Malformed() const { return malformed_; }
    const NeuralRenderResult& Result() const { return *result_; }
    const PreflightPayload& Preflight() const { return *preflight_; }
    // Empty until the helper reports its share of the cold-start timeline.
    const NeuralColdStartTimeline& Timeline() const { return timeline_; }
    // The job's temporal metrics, when the helper measured any.
    const std::optional<TemporalMetrics>& Metrics() const { return metrics_; }
    // The two idle-VRAM samples this job saw, if the helper took them. The
    // PostJob one belongs to this job; the Idle one arrived before it and
    // describes the grace this job's helper spent parked beforehand, which is
    // the cost this job's first frame just paid back.
    const std::optional<MemorySample>& PostJobMemory() const { return postJobMemory_; }
    const std::optional<MemorySample>& IdleMemory() const { return idleMemory_; }
    // A resident helper answered Hello. Evidence, not a gate: the parent hands
    // the first job over without waiting for it, because the command pipe holds
    // the frame until the helper's runtime is up and reading, and blocking on a
    // round trip would spend the cold start residency exists to remove.

private:
    bool Consume()
    {
        size_t offset = 0;
        while (bytes_.size() - offset >= sizeof(WireHeader)) {
            WireHeader header{};
            std::memcpy(&header, bytes_.data() + offset, sizeof(header));
            if (header.magic != kMagic || header.version != kVersion ||
                header.payloadBytes > kMaximumPayloadBytes ||
                (header.kind != static_cast<uint16_t>(WireKind::Progress) &&
                 header.kind != static_cast<uint16_t>(WireKind::Result) &&
                 header.kind != static_cast<uint16_t>(WireKind::Preflight) &&
                 header.kind != static_cast<uint16_t>(WireKind::Segment) &&
                 header.kind != static_cast<uint16_t>(WireKind::Timeline) &&
                 header.kind != static_cast<uint16_t>(WireKind::Ready) &&
                 header.kind != static_cast<uint16_t>(WireKind::Memory) &&
                 header.kind != static_cast<uint16_t>(WireKind::Metrics))) {
                malformed_ = true;
                return false;
            }
            const size_t messageBytes = sizeof(header) + static_cast<size_t>(header.payloadBytes);
            if (bytes_.size() - offset < messageBytes) break;
            const std::span<const std::byte> payload(bytes_.data() + offset + sizeof(header), header.payloadBytes);
            // A terminal message (result or preflight) must be the last one to
            // describe this job. Ready describes the process rather than the
            // job and may land on either side of it: the parent buffers Hello
            // and the first Job together, and a helper that reads commands on
            // its own thread can answer the Hello after that job's result is
            // already written.
            const bool announcement = header.kind == static_cast<uint16_t>(WireKind::Ready);
            if (!announcement && (result_.has_value() || preflight_.has_value())) {
                malformed_ = true;
                return false;
            }
            switch (static_cast<WireKind>(header.kind)) {
                case WireKind::Progress: {
                    const auto progress = DecodeProgress(payload);
                    if (!progress) { malformed_ = true; return false; }
                    if (progress_) progress_(*progress);
                    break;
                }
                case WireKind::Result: {
                    auto result = DecodeResult(payload);
                    if (!result) { malformed_ = true; return false; }
                    result_ = std::move(result);
                    break;
                }
                case WireKind::Preflight: {
                    auto preflight = DecodePreflight(payload);
                    if (!preflight) { malformed_ = true; return false; }
                    preflight_ = std::move(preflight);
                    break;
                }
                case WireKind::Segment: {
                    auto segment = DecodeSegment(payload);
                    if (!segment) { malformed_ = true; return false; }
                    // Indices arrive in order. A repeated 0 is the helper
                    // restarting the sequence: every earlier file is gone.
                    if (segment->index == 0) {
                        if (lastSegmentIndex_ && segments_.onRestart) segments_.onRestart();
                    } else if (!lastSegmentIndex_ || segment->index != *lastSegmentIndex_ + 1) {
                        malformed_ = true;
                        return false;
                    }
                    lastSegmentIndex_ = segment->index;
                    if (segments_.onSegment) segments_.onSegment(*segment);
                    break;
                }
                case WireKind::Timeline: {
                    // One cold start per job: a resident helper's second job
                    // reports firstOutput alone, because that is the only phase
                    // it paid for. This reader is built per job, so a second
                    // timeline inside one job still means the helper measured a
                    // startup it did not have.
                    auto timeline = DecodeTimeline(payload);
                    if (!timeline || !timeline_.Empty()) { malformed_ = true; return false; }
                    timeline_ = *timeline;
                    // Reported here rather than only on the returned result:
                    // this message arrives with the helper's first output file,
                    // and a caller that reports its own timeline when the first
                    // frame reaches the screen has already reported by the time
                    // this function returns.
                    if (timeline_reported_) timeline_reported_(timeline_);
                    break;
                }
                case WireKind::Ready: {
                    // Answers Hello and carries nothing: a helper that says it
                    // twice is not describing anything twice.
                    if (!payload.empty() || announced_) { malformed_ = true; return false; }
                    announced_ = true;
                    break;
                }
                case WireKind::Metrics: {
                    // One per job: the helper measures the job once, at its end.
                    auto metrics = DecodeMetrics(payload);
                    if (!metrics || metrics_) { malformed_ = true; return false; }
                    metrics_ = *metrics;
                    break;
                }
                case WireKind::Memory: {
                    const auto sample = DecodeMemory(payload);
                    if (!sample) { malformed_ = true; return false; }
                    // One of each per job at most: a second sample of the same
                    // stage would be a second answer to a question asked once.
                    auto& slot = sample->stage == MemoryStage::PostJob ? postJobMemory_ : idleMemory_;
                    if (slot) { malformed_ = true; return false; }
                    slot = *sample;
                    break;
                }
            }
            offset += messageBytes;
        }
        if (offset) bytes_.erase(bytes_.begin(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
        return true;
    }

    OfflineNeuralRenderer::ProgressCallback progress_;
    NeuralSegmentSink segments_;
    NeuralColdStartCallback timeline_reported_;
    std::optional<uint64_t> lastSegmentIndex_;
    std::vector<std::byte> bytes_;
    std::vector<std::byte> taken_;   // one drain's bytes, kept for its capacity
    NeuralColdStartTimeline timeline_;
    std::optional<NeuralRenderResult> result_;
    std::optional<PreflightPayload> preflight_;
    std::optional<MemorySample> postJobMemory_;
    std::optional<MemorySample> idleMemory_;
    std::optional<TemporalMetrics> metrics_;
    bool malformed_{};
    bool announced_{};
};

std::wstring ErrorDetail(std::wstring_view operation)
{
    return std::wstring(operation) + L" (Win32 error " + std::to_wstring(GetLastError()) + L").";
}

struct LaunchOutcome {
    bool launched{};
    bool cancelled{};
    DWORD exitCode{static_cast<DWORD>(-1)};
    std::wstring detail;
};

// ReShade truncates its log when the proxy loads and rotates to ReShade.log1
// when a previous file is still held open. Retiring both files before a launch
// keeps the helper on ReShade.log in the normal case; it is best effort only,
// because Windows refuses to delete a file another process holds without
// FILE_SHARE_DELETE. The helper therefore also selects its log by session
// (ResolveNeuralRuntimeLogPath) instead of trusting the name.
void RemoveStaleRuntimeLogs(const std::filesystem::path& runtimeDirectory)
{
    if (runtimeDirectory.empty()) return;
    std::error_code error;
    std::filesystem::remove(runtimeDirectory / L"ReShade.log", error);
    std::filesystem::remove(runtimeDirectory / L"ReShade.log1", error);
}

// Closes on scope exit so the eight failure paths below stop repeating the
// cleanup of everything that succeeded before them.
class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ScopedHandle(ScopedHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept
    {
        if (this != &other) {
            if (handle_) CloseHandle(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    ~ScopedHandle() { if (handle_) CloseHandle(handle_); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE Get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }
    HANDLE* Address() noexcept { return &handle_; }
    HANDLE Release() noexcept { return std::exchange(handle_, nullptr); }

private:
    HANDLE handle_{};
};

// One live helper process and the parent's ends of its pipes. `command` is set
// only for a resident helper; a single-shot helper takes its one job on its
// command line and has nothing to be told afterwards.
struct HelperProcess {
    HANDLE process{};
    HANDLE job{};
    std::unique_ptr<MetadataPipe> metadata;
    HANDLE command{};

    bool Valid() const noexcept { return process != nullptr; }
    bool Alive() const noexcept
    {
        return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }
};

// Ends the helper and releases every handle. `grace` is how long an orderly
// exit is waited for after the command channel closes; whatever is left is
// killed, because closing the job object is what guarantees nothing survives
// this function holding the GPU.
void EndHelper(HelperProcess& helper, DWORD grace)
{
    // Bounded, and it decides whether the grace period is worth waiting out.
    // This used to be an inline WriteCommand: a synchronous WriteFile on an
    // anonymous pipe, ahead of TerminateJobObject. A helper that had stopped
    // reading its commands - exactly the helper that needs ending - let the
    // pipe fill at around 16 KiB and the write never returned, so shutdown
    // hung instead of happening.
    bool asked = true;
    if (helper.command) {
        asked = neural_worker_detail::SendShutdownAndCloseBounded(
            helper.command, neural_worker_detail::kShutdownWriteBudget);
        helper.command = nullptr;   // ownership transferred, closed in there
    }
    // No point waiting out a grace period for a helper that never heard the
    // request; it goes straight to the kill below.
    if (asked && helper.process && grace) WaitForSingleObject(helper.process, grace);
    if (helper.job) TerminateJobObject(helper.job, ERROR_PROCESS_ABORTED);
    // A process that failed to be assigned to the job is not covered by it.
    if (helper.process) {
        TerminateProcess(helper.process, ERROR_PROCESS_ABORTED);
        WaitForSingleObject(helper.process, 2000);
    }
    // After the kill, so the reader finds the pipe broken rather than having
    // to be cancelled out of a read.
    helper.metadata.reset();
    if (helper.process) CloseHandle(helper.process);
    if (helper.job) CloseHandle(helper.job);
    helper = {};
}

// Builds the helper's command line from the handles it will inherit. The
// handles keep their numeric values across inheritance, which is why they can
// be named on the command line at all.
using HelperArgumentBuilder =
    std::function<std::vector<std::wstring>(HANDLE metadata, HANDLE command, HANDLE pause, HANDLE parent)>;

struct StartOutcome {
    HelperProcess helper;
    // The metadata and pause handles as the helper sees them. Inheritance
    // preserves the numbers, which is what lets the command line name them -
    // and what lets a later job be described by the same argument vector.
    HANDLE helperMetadata{};
    HANDLE helperPause{};
    std::wstring detail;
};

// Creates the pipes, the kill-on-close job object and the process, resumes it,
// and hands back the live handles. `commandChannel` adds the inbound pipe and
// the parent's process handle, which together are what resident mode is.
StartOutcome StartHelper(const std::filesystem::path& executable,
                         const HelperArgumentBuilder& arguments, HANDLE pauseEvent,
                         bool commandChannel, const std::function<void()>& processCreated)
{
    StartOutcome outcome;
    RemoveStaleRuntimeLogs(executable.parent_path());
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    ScopedHandle metadataRead, metadataWrite;
    // 64 KiB rather than the 4 KiB default: the helper writes 60 progress
    // messages a second plus segments, and a small buffer made it block on
    // every burst the parent had not yet read.
    if (!CreatePipe(metadataRead.Address(), metadataWrite.Address(), &security, kMetadataPipeBytes) ||
        !SetHandleInformation(metadataRead.Get(), HANDLE_FLAG_INHERIT, 0)) {
        outcome.detail = ErrorDetail(L"Creating the neural helper metadata pipe failed");
        return outcome;
    }
    ScopedHandle commandRead, commandWrite;
    if (commandChannel) {
        // Sized so Hello and the first job fit without the parent blocking on a
        // helper that has not started reading yet: residency must not cost the
        // cold start a round trip. A job vector is capped at 4 KiB of text.
        if (!CreatePipe(commandRead.Address(), commandWrite.Address(), &security, 16 * 1024) ||
            !SetHandleInformation(commandWrite.Get(), HANDLE_FLAG_INHERIT, 0)) {
            outcome.detail = ErrorDetail(L"Creating the neural helper command pipe failed");
            return outcome;
        }
    }
    ScopedHandle inheritedPause;
    if (pauseEvent && !DuplicateHandle(GetCurrentProcess(), pauseEvent, GetCurrentProcess(),
                                       inheritedPause.Address(), SYNCHRONIZE, TRUE, 0)) {
        outcome.detail = ErrorDetail(L"Sharing the neural helper pause event failed");
        return outcome;
    }
    // A resident helper waits on this and exits when it signals. The job object
    // already kills it when this process goes, but this process holds the GPU
    // through the helper, so it gets two independent guarantees rather than one.
    ScopedHandle inheritedParent;
    if (commandChannel && !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
                                           inheritedParent.Address(), SYNCHRONIZE, TRUE, 0)) {
        outcome.detail = ErrorDetail(L"Sharing the parent process handle with the neural helper failed");
        return outcome;
    }
    ScopedHandle job(CreateKillOnCloseJob());
    if (!job) {
        outcome.detail = ErrorDetail(L"Creating the neural helper job failed");
        return outcome;
    }

    std::array<HANDLE, 4> inherited{};
    DWORD inheritedCount = 0;
    inherited[inheritedCount++] = metadataWrite.Get();
    if (commandRead) inherited[inheritedCount++] = commandRead.Get();
    if (inheritedPause) inherited[inheritedCount++] = inheritedPause.Get();
    if (inheritedParent) inherited[inheritedCount++] = inheritedParent.Get();
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::byte> attributes(attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    const bool attributeListInitialized = attributeBytes &&
        InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes) != FALSE;
    if (!attributeListInitialized || !UpdateProcThreadAttribute(attributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), inheritedCount * sizeof(HANDLE), nullptr, nullptr)) {
        if (attributeListInitialized) DeleteProcThreadAttributeList(attributeList);
        outcome.detail = ErrorDetail(L"Restricting neural helper handle inheritance failed");
        return outcome;
    }

    std::wstring command = MakeCommandLine(executable, arguments(metadataWrite.Get(), commandRead.Get(),
                                                                 inheritedPause.Get(), inheritedParent.Get()));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION process{};
    const ScopedHardErrorSuppression noHardErrorDialog;
    const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
        executable.parent_path().c_str(), &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributeList);
    if (!created) {
        outcome.detail = ErrorDetail(L"Starting the isolated neural helper failed");
        return outcome;
    }
    // The helper exists from here on, suspended. This is the boundary the
    // player's Launch phase ends at: the process is the parent's last
    // observation before the loader window the helper measures itself.
    if (processCreated) processCreated();
    const bool assigned = AssignProcessToJobObject(job.Get(), process.hProcess) != FALSE;
    const DWORD resumed = assigned ? ResumeThread(process.hThread) : static_cast<DWORD>(-1);
    CloseHandle(process.hThread);
    if (!assigned || resumed == static_cast<DWORD>(-1)) {
        outcome.detail = assigned ? ErrorDetail(L"Resuming the isolated neural helper failed") :
                                    L"The isolated neural helper could not be assigned to its job.";
        HelperProcess doomed{process.hProcess, job.Release(), nullptr, nullptr};
        EndHelper(doomed, 0);
        return outcome;
    }
    auto metadata = std::make_unique<MetadataPipe>(metadataRead.Release());
    if (!metadata->Valid()) {
        outcome.detail = ErrorDetail(L"Starting the neural helper metadata reader failed");
        HelperProcess doomed{process.hProcess, job.Release(), std::move(metadata), nullptr};
        EndHelper(doomed, 0);
        return outcome;
    }
    outcome.helper = {process.hProcess, job.Release(), std::move(metadata), commandWrite.Release()};
    outcome.helperMetadata = metadataWrite.Get();
    outcome.helperPause = inheritedPause.Get();
    return outcome;
}

// How long a cancelled resident job is given to report the Result that says it
// cancelled. Beyond it the helper is killed, matching the budget a single-shot
// cancel has always had, and residency is dropped rather than trusted.
constexpr std::chrono::milliseconds kResidentCancelGrace{2000};

struct PumpOutcome {
    bool malformed{};
    bool exited{};
    bool cancelled{};
    bool completed{};   // a terminal message for this job arrived
};

// Pumps the helper's metadata pipe until this job is over. A single-shot helper
// is followed to its exit; a resident one is followed to its terminal message,
// because the process is meant to still be there afterwards.
PumpOutcome Pump(const HelperProcess& helper, MetadataReader& reader, std::stop_token stop,
                 bool resident)
{
    PumpOutcome outcome;
    std::optional<std::chrono::steady_clock::time_point> cancelDeadline;
    // The stop token as something the wait below can wake on. Without the
    // event the pump still works, falling back to the 20 ms look it used to
    // take at everything.
    ScopedHandle stopped(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const HANDLE stoppedEvent = stopped.Get();
    const std::stop_callback onStop(stop, [stoppedEvent] { if (stoppedEvent) SetEvent(stoppedEvent); });
    for (;;) {
        bool budgetSpent = false;
        if (!reader.ReadAvailable(*helper.metadata, &budgetSpent)) { outcome.malformed = true; break; }
        // Judged from the reader rather than from the process, because a helper
        // that writes its result and exits in the same breath is judged below
        // on what it said, not on the fact that it went.
        if (resident && (reader.Complete() || reader.PreflightComplete())) break;
        if (stop.stop_requested() && !outcome.cancelled) {
            outcome.cancelled = true;
            // A single-shot helper is killed by the caller. A resident one is
            // asked, because it answers with a Result that says cancelled and
            // then goes idle: killing it would throw away both.
            if (!resident || !WriteCommand(helper.command, CommandKind::Cancel, nullptr, 0)) break;
            cancelDeadline = std::chrono::steady_clock::now() + kResidentCancelGrace;
        }
        DWORD timeout = INFINITE;
        if (cancelDeadline) {
            const auto left = *cancelDeadline - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero()) break;
            timeout = static_cast<DWORD>(
                std::chrono::ceil<std::chrono::milliseconds>(left).count());
        }
        // The queue still has work: take it before sleeping, having checked
        // the stop token on the way, which is what the budget is for.
        if (budgetSpent) continue;
        if (!stoppedEvent) timeout = std::min<DWORD>(timeout, 20);
        // Data first, so a helper that wrote and exited is read before it is
        // judged gone. The stop event stays set once stop is requested, so it
        // leaves the set once the cancel has been acted on.
        const HANDLE handles[] = {helper.metadata->Ready(), helper.process, stoppedEvent};
        const DWORD count = outcome.cancelled || !stoppedEvent ? 2 : 3;
        const DWORD wait = WaitForMultipleObjects(count, handles, FALSE, timeout);
        if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_FAILED) { outcome.exited = !helper.Alive(); break; }
    }
    // A helper that has gone may still have its last bytes on their way from
    // the pipe into the queue.
    if (outcome.exited) helper.metadata->WaitForEnd(1000);
    reader.ReadAvailable(*helper.metadata);
    outcome.completed = reader.Complete() || reader.PreflightComplete();
    return outcome;
}

// Launches a single-shot helper, pumps its messages into `reader`, and returns
// once it exits or the caller cancels. The job object kills the whole helper
// tree on close.
LaunchOutcome LaunchHelper(const std::filesystem::path& executable,
                           const std::function<std::vector<std::wstring>(HANDLE metadata, HANDLE pause)>& arguments,
                           HANDLE pauseEvent, MetadataReader& reader, std::stop_token stop,
                           const std::function<void()>& processCreated)
{
    LaunchOutcome outcome;
    StartOutcome started = StartHelper(executable,
        [&](HANDLE metadata, HANDLE, HANDLE pause, HANDLE) { return arguments(metadata, pause); },
        pauseEvent, false, processCreated);
    if (!started.helper.Valid()) {
        outcome.detail = std::move(started.detail);
        return outcome;
    }
    outcome.launched = true;
    const PumpOutcome pump = Pump(started.helper, reader, stop, false);
    outcome.cancelled = pump.cancelled;
    // Only when the process actually went. GetExitCodeProcess succeeds on a
    // live process and answers STILL_ACTIVE (259), so asking unconditionally
    // reported a helper that was still running as "exited with code 259" - a
    // number that reads like a crash, classifies as WorkerCrashed, and hides
    // the real diagnosis. The pump already knows which happened.
    if (pump.exited) {
        // An unreadable code stays the -1 default: nonzero, so a crash.
        outcome.exitCode = neural_worker_detail::ReadHelperExitCode([&](DWORD* code) {
            return GetExitCodeProcess(started.helper.process, code) != FALSE;
        }).value_or(outcome.exitCode);
    } else {
        outcome.exitCode = 0;
    }
    // The old process is signalled and all its handles are closed here. Waiting
    // for full exit releases ReShade.log before the next proxy loads; overlapping
    // helpers otherwise put the real evidence in ReShade.log1.
    EndHelper(started.helper, 0);
    return outcome;
}

bool ValidRequest(const NeuralRenderRequest& request)
{
    if ((request.outputWidth || request.outputHeight) &&
        (!request.outputWidth || !request.outputHeight ||
         request.outputWidth < request.width || request.outputHeight < request.height))
        return false;
    if (request.sourcePath.empty() || request.stagingVideoPath.empty() || !request.width || !request.height ||
        !std::isfinite(request.fps) || request.fps <= 0.0 || !std::isfinite(request.durationSeconds) ||
        request.durationSeconds <= 0.0) return false;
    if (request.range.start100ns < 0 || request.range.end100ns < 0) return false;
    if (request.range.end100ns && request.range.end100ns <= request.range.start100ns) return false;
    // A reduced model input is restored to the SOURCE size by the carrier, so
    // it cannot share the job with a Super Resolution output of its own.
    if (!IsProcessingScaleRung(request.processingScale)) return false;
    if (request.processingScale != kDefaultProcessingScale && request.outputWidth &&
        (request.outputWidth != request.width || request.outputHeight != request.height)) return false;
    // Per-frame Super Resolution needs Super Resolution and no model on the carrier.
    if (request.upscalingHistory != kDefaultUpscalingHistory &&
        (request.requireNeural || !request.outputWidth ||
         (request.outputWidth == request.width && request.outputHeight == request.height))) return false;
    return true;
}

// Refusals that belong to the request rather than to any helper, so a resident
// helper and a single-shot one answer them identically and neither starts a
// process to say no.
std::optional<NeuralRenderResult> RefuseUnrunnableJob(const std::filesystem::path& executable,
                                                      const NeuralRenderRequest& request)
{
    NeuralRenderResult result;
    result.jobId = request.jobId;
    std::error_code fileError;
    if (executable.empty() || !std::filesystem::is_regular_file(executable, fileError) || fileError) {
        result.failure = NeuralRenderFailure::Protocol;
        result.detail = L"The isolated neural helper executable is unavailable.";
        return result;
    }
    if (!ValidRequest(request)) {
        result.failure = NeuralRenderFailure::Protocol;
        result.detail = L"Invalid neural helper request.";
        return result;
    }
    return std::nullopt;
}

// The result a job gets when the helper reported one. Shared so a resident job
// and a single-shot job cannot disagree about whose result they accepted.
NeuralRenderResult AcceptResult(const MetadataReader& reader, const NeuralRenderRequest& request)
{
    const uint64_t jobId = request.jobId;
    NeuralRenderResult result = reader.Result();
    // A finished render that answers a different question than the job asked:
    // Super Resolution-only frames for a neural job would be published as
    // neural, and neural frames for a Super Resolution-only job are the
    // byte-identical pair that job exists to end. Either is refused.
    if (result.jobId == jobId && result.ok && result.neural != request.requireNeural) {
        NeuralRenderResult mismatch;
        mismatch.jobId = jobId;
        mismatch.failure = NeuralRenderFailure::Identity;
        mismatch.detail = request.requireNeural
            ? L"The isolated neural helper returned a Super Resolution-only render for a neural job."
            : L"The isolated neural helper returned a neural render for a Super Resolution-only job.";
        return mismatch;
    }
    if (result.jobId == jobId) return result;
    // A helper that refused the job it was handed has no job id to report:
    // the id lives inside the argument vector it could not parse. Only one job
    // is ever in flight, and this result cannot publish anything, so its
    // diagnosis is worth more than the identity it could not state.
    if (!result.ok && !result.jobId) {
        result.jobId = jobId;
        return result;
    }
    NeuralRenderResult mismatch;
    mismatch.jobId = jobId;
    mismatch.failure = NeuralRenderFailure::Identity;
    mismatch.detail = L"The isolated neural helper reported a result for a different job.";
    return mismatch;
}

// Everything the reader collected about the helper itself rather than about
// the render, copied onto the result the caller will publish. It lives here
// instead of inside the result message because WireResult is a fixed 152
// bytes that a released Python decoder already reads by offset; the cold-start
// timeline and the VRAM samples travel as their own messages and are rejoined
// here, so one job produces one record whatever arrived separately.
void StampHelperObservations(const MetadataReader& reader, NeuralRenderResult& result)
{
    result.coldStart = reader.Timeline();
    if (const auto& metrics = reader.Metrics()) result.metrics = *metrics;
    // The idle sample was taken before this job by the same process, so it
    // names the same policy; the job's own sample wins where both are present.
    if (const auto& idle = reader.IdleMemory()) {
        result.timing.idleLocalVramMiB = idle->localVramMiB;
        result.timing.idleVramPolicy = idle->policy;
    }
    if (const auto& postJob = reader.PostJobMemory()) {
        result.timing.postJobLocalVramMiB = postJob->localVramMiB;
        result.timing.idleVramPolicy = postJob->policy;
    }
}

// One helper launch, judged. `restartRequested` is set when the helper repaired
// its own configuration and exited for a fresh process to replace it.
NeuralRenderResult RunHelperOnce(const std::filesystem::path& executable,
                                 const NeuralRenderRequest& request, std::stop_token stop,
                                 bool configurationRestarted, MetadataReader& reader,
                                 const std::function<void()>& processCreated,
                                 bool& restartRequested)
{
    NeuralRenderResult result;
    result.jobId = request.jobId;
    if (stop.stop_requested()) {
        result.cancelled = true;
        result.failure = NeuralRenderFailure::Cancelled;
        result.detail = L"Neural rendering was cancelled before the helper started.";
        return result;
    }
    const LaunchOutcome launch = LaunchHelper(executable,
        [&](HANDLE metadata, HANDLE pause) {
            return neural_worker_detail::BuildWorkerArguments(request, metadata, pause, configurationRestarted);
        }, request.pauseEvent, reader, stop, processCreated);
    if (!launch.launched) {
        result.failure = NeuralRenderFailure::Protocol;
        result.detail = launch.detail;
        return result;
    }
    if (launch.cancelled || stop.stop_requested()) {
        result.cancelled = true;
        result.failure = NeuralRenderFailure::Cancelled;
        result.detail = L"Neural rendering was cancelled.";
        return result;
    }
    if (launch.exitCode == neural_worker_detail::kConfigurationChangedExitCode &&
        !configurationRestarted && !reader.Malformed()) {
        restartRequested = true;
        return result;
    }
    if (launch.exitCode != 0) {
        result.failure = NeuralRenderFailure::WorkerCrashed;
        result.detail = L"The isolated neural helper exited with code " + std::to_wstring(launch.exitCode) +
                        L" before producing a result.";
        return result;
    }
    if (!reader.Complete()) {
        result.failure = NeuralRenderFailure::Protocol;
        result.detail = reader.Malformed() ? L"The isolated neural helper returned malformed metadata." :
                                             L"The isolated neural helper returned incomplete metadata.";
        return result;
    }
    return AcceptResult(reader, request);
}

NeuralRenderResult RunNeuralWorkerAttempt(const std::filesystem::path& executable,
                                          const NeuralRenderRequest& request,
                                          const OfflineNeuralRenderer::ProgressCallback& progress,
                                          const NeuralSegmentSink& segments,
                                          std::stop_token stop, bool configurationRestarted,
                                          const std::function<void()>& processCreated,
                                          const NeuralColdStartCallback& helperTimeline)
{
    MetadataReader reader(progress, segments, helperTimeline);
    bool restartRequested = false;
    NeuralRenderResult result = RunHelperOnce(executable, request, stop,
        configurationRestarted, reader, processCreated, restartRequested);
    if (restartRequested) {
        // The replacement helper measures its own cold start and reports it the
        // same way; the one this reader may already have reported belongs to a
        // process that exited before it rendered anything, and a merge of both
        // keeps the later one, exactly as `result.coldStart` does below.
        return RunNeuralWorkerAttempt(executable, request, progress, segments, stop, true,
                                      processCreated, helperTimeline);
    }
    // The reader outlives the judgement above, so a timeline or a memory
    // sample that arrived before a crash, a cancel or a rejected result is
    // still reported: the breakdown of a run that failed is the whole point of
    // measuring it.
    StampHelperObservations(reader, result);
    return result;
}

// The bounded from-zero relaunch, shared by the single-shot launcher and the
// resident host. A crashed helper or a removed device leaves no temporal
// history to resume, so the whole sequence restarts in a fresh process and
// frames are never spliced across helper instances. `attempt` is one helper
// instance being given one job, however it got there.
//
// `recover` runs between the failure and the relaunch and decides whether the
// relaunch is worth making: a helper that died took its verdict with it, and
// after a device removal the driver has restarted underneath this process. It
// returns a result to fail closed with, or nothing to let the relaunch happen.
//
// `attempt` is handed the launch number, 0 for the first helper; see ForLaunch.
template <class Attempt, class Recover>
NeuralRenderResult RunWithRelaunches(const OfflineNeuralRenderer::ProgressCallback& progress,
                                     const NeuralSegmentSink& segments, uint32_t crashRelaunchLimit,
                                     const Attempt& attempt, const Recover& recover)
{
    for (uint32_t tries = 0;; ++tries) {
        // A relaunch renders the range again from frame zero, so every segment
        // the previous helper published is superseded.
        if (tries && segments.onRestart) segments.onRestart();
        NeuralRenderResult result = attempt(tries);
        const bool relaunchable = !result.ok && !result.cancelled &&
            (result.failure == NeuralRenderFailure::WorkerCrashed ||
             result.failure == NeuralRenderFailure::DeviceRemoved ||
             result.failure == NeuralRenderFailure::GpuStall);
        if (!relaunchable) return result;
        if (tries >= crashRelaunchLimit) {
            result.failure = NeuralRenderFailure::RetryExhausted;
            result.detail = L"The neural helper did not recover after " + std::to_wstring(tries + 1) +
                            L" attempt(s): " + result.detail;
            return result;
        }
        if (progress) {
            NeuralRenderProgress recovering{};
            recovering.phase = NeuralRenderPhase::Recovering;
            recovering.recovering = result.failure;
            recovering.retries = tries + 1;
            progress(recovering);
        }
        // Reported as Recovering above first, so the probe that follows runs
        // under a phase the user can already see rather than in silence.
        if (auto refused = recover(result)) return *refused;
    }
}

// The request a given launch of the helper is sent. A relaunched helper numbers
// its segments from 0 again, and named from the same staging stem they are the
// previous helper's file names: the player's decoder may still hold one of
// those open without delete sharing, so the new helper's delete failed quietly
// and its encoder truncated a file that was being decoded. Each launch after
// the first writes under its own stem instead (neural-l1-00000.mkv). A
// single-file job is left alone: its staging path is the output the caller
// promotes, and nothing reads it while the helper writes.
NeuralRenderRequest ForLaunch(const NeuralRenderRequest& request, uint32_t launch)
{
    if (!launch || !request.segmentFrames) return request;
    NeuralRenderRequest relaunched = request;
    const std::filesystem::path& staging = request.stagingVideoPath;
    relaunched.stagingVideoPath = staging.parent_path() /
        (staging.stem().wstring() + L"-l" + std::to_wstring(launch) + staging.extension().wstring());
    return relaunched;
}

// The one controlled recovery between a helper that died and the relaunch that
// replaces it: re-run the feature-18 probe in a fresh process, and relaunch
// only if it still passes.
//
// A crash or a device removal invalidates the verdict the job was started on.
// A TDR restarts the display driver, so the adapter this process probed is not
// necessarily the one it now has, and spending the single relaunch on a
// runtime that can no longer arm feature 18 buys a second identical crash
// instead of a reason. The probe costs one process on a path that has already
// lost a render, and it is bounded by the same relaunch limit, so there is
// nothing here that can spin.
std::optional<NeuralRenderResult> ProbeBeforeRelaunch(const std::filesystem::path& executable,
                                                      std::stop_token stop,
                                                      const NeuralRenderResult& failed)
{
    const NeuralPreflightResult preflight = RunNeuralPreflight(executable, stop);
    if (preflight.ok) return std::nullopt;
    NeuralRenderResult closed;
    closed.jobId = failed.jobId;
    closed.coldStart = failed.coldStart;
    closed.timing = failed.timing;
    if (preflight.cancelled || stop.stop_requested()) {
        closed.cancelled = true;
        closed.failure = NeuralRenderFailure::Cancelled;
        closed.detail = L"Neural rendering was cancelled while the helper was recovering.";
        return closed;
    }
    closed.failure = NeuralRenderFailure::Preflight;
    closed.detail = L"The neural helper did not survive the render (" + failed.detail +
                    L") and its runtime no longer passes preflight, so it was not restarted: " +
                    (preflight.detail.empty() ? std::wstring(L"the probe did not arm feature 18.")
                                              : preflight.detail);
    return closed;
}

// How long an orderly exit is waited for after a resident helper is asked to
// shut down. It has nothing to finish - Release is only called between jobs -
// so this only covers the proxy's own unload, and the job object collects
// anything slower.
constexpr DWORD kResidentShutdownGrace = 1000;

// The receipt's "diagnosis" object, read back without a JSON parser: the
// probe wrote it, and the parent only needs the two strings out of it.
struct ReceiptDiagnosis {
    NeuralPreflightCause cause{NeuralPreflightCause::None};
    std::wstring detail;
};

std::string_view ScanDiagnosisMember(std::string_view object, std::string_view field)
{
    std::string key = "\"";
    key += field;
    key += "\":\"";
    const size_t start = object.find(key);
    if (start == std::string_view::npos) return {};
    size_t position = start + key.size();
    const size_t valueStart = position;
    while (position < object.size() && object[position] != '"') {
        position += object[position] == '\\' ? 2 : 1;
    }
    return object.substr(valueStart, std::min(position, object.size()) - valueStart);
}

std::wstring UnescapeJsonToWide(std::string_view escaped)
{
    std::string text;
    text.reserve(escaped.size());
    for (size_t index = 0; index < escaped.size(); ++index) {
        if (escaped[index] != '\\' || index + 1 == escaped.size()) {
            text.push_back(escaped[index]);
            continue;
        }
        switch (escaped[++index]) {
            case 'n': text.push_back('\n'); break;
            case 'r': text.push_back('\r'); break;
            case 't': text.push_back('\t'); break;
            // \uXXXX never appears: the probe escapes only control characters
            // it also never emits, and every other escape is the literal.
            default: text.push_back(escaped[index]); break;
        }
    }
    return utf8_text::ToWide(text);
}

ReceiptDiagnosis ScanReceiptDiagnosis(std::string_view json)
{
    ReceiptDiagnosis diagnosis;
    const size_t start = json.find("\"diagnosis\":{");
    if (start == std::string_view::npos) return diagnosis;
    const size_t end = json.find('}', start);
    const std::string_view object =
        json.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    const std::string_view cause = ScanDiagnosisMember(object, "cause");
    for (size_t index = 0; index < kNeuralPreflightCauseNames.size(); ++index) {
        if (kNeuralPreflightCauseNames[index] != cause) continue;
        diagnosis.cause = static_cast<NeuralPreflightCause>(index);
        break;
    }
    diagnosis.detail = UnescapeJsonToWide(ScanDiagnosisMember(object, "detail"));
    return diagnosis;
}

} // namespace

std::vector<std::wstring> neural_worker_detail::BuildWorkerArguments(
    const NeuralRenderRequest& request, HANDLE metadata, HANDLE pauseEvent, bool configurationRestarted)
{
    std::vector<std::wstring> arguments{
        std::wstring(kWorkerMode), L"--metadata-handle", HandleText(metadata),
        L"--source", request.sourcePath.wstring(), L"--staging", request.stagingVideoPath.wstring(),
        L"--width", std::to_wstring(request.width), L"--height", std::to_wstring(request.height),
        L"--fps", std::to_wstring(request.fps), L"--duration-100ns",
        std::to_wstring(static_cast<int64_t>(std::llround(request.durationSeconds * 10000000.0))),
        L"--job-id", std::to_wstring(request.jobId),
        L"--range-start-100ns", std::to_wstring(request.range.start100ns),
        L"--range-end-100ns", std::to_wstring(request.range.end100ns),
        L"--preroll-frames", std::to_wstring(request.prerollFrames),
        L"--frame-retry-limit", std::to_wstring(request.frameRetryLimit)};
    // Emitted only when they differ from the defaults, so an upscale-free job's
    // command line is byte-identical to the one this helper has always taken.
    if (request.outputWidth && request.outputHeight &&
        (request.outputWidth != request.width || request.outputHeight != request.height)) {
        arguments.emplace_back(L"--output-width");
        arguments.emplace_back(std::to_wstring(request.outputWidth));
        arguments.emplace_back(L"--output-height");
        arguments.emplace_back(std::to_wstring(request.outputHeight));
    }
    if (!request.requireNeural) {
        arguments.emplace_back(L"--require-neural");
        arguments.emplace_back(L"0");
    }
    // Absent means 100, the source size, which is every job before the rungs.
    if (request.processingScale != kDefaultProcessingScale) {
        arguments.emplace_back(L"--processing-scale");
        arguments.emplace_back(std::to_wstring(request.processingScale));
    }
    // Absent means temporal, which is every job before the choice existed.
    if (request.upscalingHistory != kDefaultUpscalingHistory) {
        const std::string_view history = UpscalingHistoryName(request.upscalingHistory);
        arguments.emplace_back(L"--sr-history");
        arguments.emplace_back(history.begin(), history.end());
    }
    const std::string guides = CanonicalGuideControls(request.guides);
    arguments.emplace_back(L"--guides");
    arguments.emplace_back(guides.begin(), guides.end());
    if (request.segmentFrames) {
        arguments.emplace_back(L"--segment-frames");
        arguments.emplace_back(std::to_wstring(request.segmentFrames));
    }
    if (request.firstSegmentFrames && request.firstSegmentFrames != request.segmentFrames) {
        arguments.emplace_back(L"--first-segment-frames");
        arguments.emplace_back(std::to_wstring(request.firstSegmentFrames));
    }
    // Absent means the CPU conversion inside ffmpeg, which is what every earlier
    // helper did, so an older parent and a newer helper still agree.
    if (request.gpuColorConversion) {
        arguments.emplace_back(L"--gpu-color-conversion");
        arguments.emplace_back(L"1");
    }
    // Absent means preset 7 (slowest/highest quality), so an older parent and a
    // newer helper still agree. That 7 is a version contract and NOT the
    // player's default, which is p5 - see EncoderSpec::nvencPreset - so the
    // shipping case does put the pair on the wire.
    if (request.nvencPreset != 7) {
        arguments.emplace_back(L"--nvenc-preset");
        arguments.emplace_back(std::to_wstring(request.nvencPreset));
    }
    // Absent means the CPU conversion inside ffmpeg, the same polarity as the
    // capture flag above: what every earlier helper did is what a missing flag
    // selects, so an older parent driving a newer helper cannot be switched onto
    // the NV12 source path behind its back.
    if (request.gpuSourceConversion) {
        arguments.emplace_back(L"--gpu-source-conversion");
        arguments.emplace_back(L"1");
    }
    // Absent means the defaults, so a default job's command line is the one this
    // helper has always taken and an older helper still accepts it.
    if (!request.temporal.IsDefault()) {
        const std::string temporal = CanonicalTemporalSettings(request.temporal);
        arguments.emplace_back(L"--temporal");
        arguments.emplace_back(temporal.begin(), temporal.end());
    }
    // Absent means the undithered capture every earlier helper wrote, so an
    // older parent cannot change the bytes a newer helper produces for it.
    if (request.captureDither) {
        arguments.emplace_back(L"--capture-dither");
        arguments.emplace_back(L"1");
    }
    // Absent means no deband, which is what every earlier helper showed the model.
    if (request.sourceDeband) {
        arguments.emplace_back(L"--deband");
        arguments.emplace_back(L"1");
    }
    // Absent means Standard, the only rung every earlier helper could write.
    if (request.quality != EncoderQuality::Standard) {
        const std::string_view name = EncoderQualityName(request.quality);
        arguments.emplace_back(L"--cache-quality");
        arguments.emplace_back(name.begin(), name.end());
    }
    if (pauseEvent) {
        arguments.emplace_back(L"--pause-event");
        arguments.emplace_back(HandleText(pauseEvent));
    }
    if (configurationRestarted) arguments.emplace_back(kRestartedFlag);
    return arguments;
}

std::vector<std::wstring> neural_worker_detail::BuildPreflightArguments(HANDLE metadata, bool configurationRestarted)
{
    std::vector<std::wstring> arguments{std::wstring(kPreflightMode), L"--metadata-handle", HandleText(metadata)};
    if (configurationRestarted) arguments.emplace_back(kRestartedFlag);
    return arguments;
}

std::vector<std::wstring> neural_worker_detail::BuildResidentArguments(
    HANDLE metadata, HANDLE command, HANDLE pauseEvent, HANDLE parentProcess,
    resident_helper::IdleVramPolicy idleVramPolicy)
{
    std::vector<std::wstring> arguments{std::wstring(kWorkerMode), L"--metadata-handle", HandleText(metadata),
                                        L"--command-handle", HandleText(command)};
    // One event for the helper's whole life: it has to be inheritable at
    // CreateProcess, so it cannot arrive with a later job. The player resets it
    // before every job, and the helper samples it between frames.
    if (pauseEvent) {
        arguments.emplace_back(L"--pause-event");
        arguments.emplace_back(HandleText(pauseEvent));
    }
    if (parentProcess) {
        arguments.emplace_back(L"--parent-process");
        arguments.emplace_back(HandleText(parentProcess));
    }
    // Written for both arms, the default included. An idle VRAM figure only
    // means something beside the policy that produced it, and a flag that is
    // absent for one arm leaves the launch line unable to say which ran.
    arguments.emplace_back(kIdleVramPolicyFlag);
    arguments.emplace_back(resident_helper::IdleVramPolicyName(idleVramPolicy));
    return arguments;
}

std::optional<neural_worker_detail::WorkerArguments> neural_worker_detail::ParseWorkerArguments(
    std::span<const std::wstring_view> arguments)
{
    if (arguments.size() < 2) return std::nullopt;
    WorkerArguments parsed;
    if (arguments[1] == kPreflightMode) parsed.preflight = true;
    else if (arguments[1] != kWorkerMode) return std::nullopt;
    size_t end = arguments.size();
    if (end > 2 && arguments[end - 1] == kRestartedFlag) {
        parsed.configurationRestarted = true;
        --end;
    }
    if (((end - 2) % 2) != 0) return std::nullopt;

    enum Key { Metadata, Source, Staging, Width, Height, Fps, Duration, JobId, RangeStart, RangeEnd, Preroll,
               RetryLimit, Guides, SegmentFrames, PauseEvent, GpuColorConversion, NvencPreset,
               GpuSourceConversion, FirstSegmentFrames, Command, ParentProcess, IdleVram,
               OutputWidth, OutputHeight, RequireNeural, ProcessingScale, Temporal, UpscalingHistoryKey,
               CaptureDither, CacheQuality, Deband, KeyCount };
    constexpr std::array<std::wstring_view, KeyCount> names{
        L"--metadata-handle", L"--source", L"--staging", L"--width", L"--height", L"--fps", L"--duration-100ns",
        L"--job-id", L"--range-start-100ns", L"--range-end-100ns", L"--preroll-frames", L"--frame-retry-limit",
        L"--guides", L"--segment-frames", L"--pause-event", L"--gpu-color-conversion", L"--nvenc-preset",
        L"--gpu-source-conversion", L"--first-segment-frames", L"--command-handle", L"--parent-process",
        kIdleVramPolicyFlag,
        // Absent on every helper invocation that does not upscale, which keeps
        // an older helper binary compatible with a newer player for the jobs
        // that binary can actually do.
        L"--output-width", L"--output-height", L"--require-neural", L"--processing-scale",
        // Absent at the defaults, for the same reason.
        L"--temporal", L"--sr-history",
        // Capture-side quality switches: absent is what every earlier helper did.
        L"--capture-dither", L"--cache-quality", L"--deband"};
    std::array<std::optional<std::wstring_view>, KeyCount> values{};
    for (size_t index = 2; index < end; index += 2) {
        const auto found = std::find(names.begin(), names.end(), arguments[index]);
        if (found == names.end()) return std::nullopt;
        auto& slot = values[static_cast<size_t>(found - names.begin())];
        if (slot.has_value()) return std::nullopt;
        slot = arguments[index + 1];
    }
    uint64_t rawHandle = 0;
    if (!values[Metadata] || !ParseUnsigned(*values[Metadata], rawHandle) || !rawHandle) return std::nullopt;
    parsed.metadata = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(rawHandle));
    if (parsed.preflight) {
        for (size_t key = 0; key < KeyCount; ++key) {
            if (key != Metadata && values[key].has_value()) return std::nullopt;
        }
        return parsed;
    }
    // Resident mode: the helper takes its jobs off the command channel, so its
    // launch line describes a helper and not a job. Exactly five keys may
    // appear, by allowlist - a job field here would be a job nobody asked for,
    // arriving beside a channel that is about to deliver one.
    if (values[Command]) {
        constexpr std::array<Key, 5> permitted{Metadata, PauseEvent, Command, ParentProcess, IdleVram};
        for (size_t key = 0; key < KeyCount; ++key) {
            const bool allowed = std::find(permitted.begin(), permitted.end(), static_cast<Key>(key)) !=
                                 permitted.end();
            if (!allowed && values[key].has_value()) return std::nullopt;
        }
        uint64_t rawCommand = 0;
        if (!ParseUnsigned(*values[Command], rawCommand) || !rawCommand) return std::nullopt;
        parsed.command = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(rawCommand));
        if (values[PauseEvent]) {
            uint64_t rawPause = 0;
            if (!ParseUnsigned(*values[PauseEvent], rawPause) || !rawPause) return std::nullopt;
            parsed.request.pauseEvent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(rawPause));
        }
        if (values[ParentProcess]) {
            uint64_t rawParent = 0;
            if (!ParseUnsigned(*values[ParentProcess], rawParent) || !rawParent) return std::nullopt;
            parsed.parentProcess = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(rawParent));
        }
        // Absent is the default arm, so a parent that predates the flag still
        // produces a helper that keeps its feature memory - which is what
        // every helper before the flag did. A value this build cannot name is
        // refused: a policy nobody can state makes the samples unattributable.
        if (values[IdleVram]) {
            const auto policy = resident_helper::ParseIdleVramPolicy(*values[IdleVram]);
            if (!policy) return std::nullopt;
            parsed.idleVramPolicy = *policy;
        }
        return parsed;
    }
    // The parent's process handle is only meaningful to a helper that outlives
    // one job; on a single-shot line it is a handle nobody would wait on, and
    // an idle policy is an instruction for an idle period that never comes.
    if (values[ParentProcess] || values[IdleVram]) return std::nullopt;
    constexpr std::array<Key, 12> required{Source, Staging, Width, Height, Fps, Duration, JobId, RangeStart,
                                           RangeEnd, Preroll, RetryLimit, Guides};
    for (const Key key : required) if (!values[key]) return std::nullopt;
    if (values[Source]->empty() || values[Staging]->empty()) return std::nullopt;
    uint64_t width = 0, height = 0, duration = 0, jobId = 0, rangeStart = 0, rangeEnd = 0, preroll = 0,
             retryLimit = 0;
    double fps = 0.0;
    if (!ParseUnsigned(*values[Width], width) || !ParseUnsigned(*values[Height], height) ||
        !ParseUnsigned(*values[Duration], duration) || !ParseDouble(*values[Fps], fps) ||
        !ParseUnsigned(*values[JobId], jobId) || !ParseUnsigned(*values[RangeStart], rangeStart) ||
        !ParseUnsigned(*values[RangeEnd], rangeEnd) || !ParseUnsigned(*values[Preroll], preroll) ||
        !ParseUnsigned(*values[RetryLimit], retryLimit) || !width || !height || width > UINT32_MAX ||
        height > UINT32_MAX || !duration || duration > INT64_MAX || fps <= 0.0 || rangeStart > INT64_MAX ||
        rangeEnd > INT64_MAX || preroll > UINT32_MAX || retryLimit > UINT32_MAX) return std::nullopt;
    const auto guides = ParseGuideControls(
        narrow_text::StrictAscii(*values[Guides]).value_or(std::string{}));
    if (!guides) return std::nullopt;
    NeuralRenderRequest& request = parsed.request;
    request.sourcePath = *values[Source];
    request.stagingVideoPath = *values[Staging];
    request.width = static_cast<uint32_t>(width);
    request.height = static_cast<uint32_t>(height);
    request.fps = fps;
    request.durationSeconds = static_cast<double>(duration) / 10000000.0;
    request.jobId = jobId;
    request.range = {static_cast<int64_t>(rangeStart), static_cast<int64_t>(rangeEnd)};
    request.prerollFrames = static_cast<uint32_t>(preroll);
    request.frameRetryLimit = static_cast<uint32_t>(retryLimit);
    request.guides = *guides;
    // Optional: absent means one output file for the whole range.
    if (values[SegmentFrames]) {
        uint64_t segmentFrames = 0;
        if (!ParseUnsigned(*values[SegmentFrames], segmentFrames) || segmentFrames > UINT32_MAX)
            return std::nullopt;
        request.segmentFrames = static_cast<uint32_t>(segmentFrames);
    }
    // Absent means the first file is as long as the rest, which is what every
    // helper before this flag did.
    if (values[FirstSegmentFrames]) {
        uint64_t firstFrames = 0;
        if (!ParseUnsigned(*values[FirstSegmentFrames], firstFrames) || firstFrames > UINT32_MAX)
            return std::nullopt;
        request.firstSegmentFrames = static_cast<uint32_t>(firstFrames);
    }
    // Both or neither: a width without a height is a malformed request, not a
    // half-specified one to be guessed at.
    if (values[OutputWidth] || values[OutputHeight]) {
        if (!values[OutputWidth] || !values[OutputHeight]) return std::nullopt;
        uint64_t outputWidth = 0, outputHeight = 0;
        if (!ParseUnsigned(*values[OutputWidth], outputWidth) || outputWidth > UINT32_MAX ||
            !ParseUnsigned(*values[OutputHeight], outputHeight) || outputHeight > UINT32_MAX)
            return std::nullopt;
        request.outputWidth = static_cast<uint32_t>(outputWidth);
        request.outputHeight = static_cast<uint32_t>(outputHeight);
    }
    // Absent means neural is required, which is what every job before Super
    // Resolution joined this pass asked for.
    if (values[RequireNeural]) {
        uint64_t neuralRequired = 0;
        if (!ParseUnsigned(*values[RequireNeural], neuralRequired) || neuralRequired > 1)
            return std::nullopt;
        request.requireNeural = neuralRequired == 1;
    }
    if (values[ProcessingScale]) {
        uint64_t percent = 0;
        if (!ParseUnsigned(*values[ProcessingScale], percent) || percent > UINT32_MAX ||
            !IsProcessingScaleRung(static_cast<uint32_t>(percent))) return std::nullopt;
        request.processingScale = static_cast<uint32_t>(percent);
    }
    if (values[Temporal]) {
        const auto temporal = ParseTemporalSettings(
            narrow_text::StrictAscii(*values[Temporal]).value_or(std::string{}));
        if (!temporal) return std::nullopt;
        request.temporal = *temporal;
    }
    if (values[UpscalingHistoryKey]) {
        const auto history = ParseUpscalingHistory(
            narrow_text::StrictAscii(*values[UpscalingHistoryKey]).value_or(std::string{}));
        if (!history) return std::nullopt;
        request.upscalingHistory = *history;
    }
    if (values[GpuColorConversion]) {
        uint64_t enabled = 0;
        if (!ParseUnsigned(*values[GpuColorConversion], enabled) || enabled > 1) return std::nullopt;
        request.gpuColorConversion = enabled != 0;
    }
    if (values[GpuSourceConversion]) {
        uint64_t enabled = 0;
        if (!ParseUnsigned(*values[GpuSourceConversion], enabled) || enabled > 1) return std::nullopt;
        request.gpuSourceConversion = enabled != 0;
    }
    if (values[CaptureDither]) {
        uint64_t enabled = 0;
        if (!ParseUnsigned(*values[CaptureDither], enabled) || enabled > 1) return std::nullopt;
        request.captureDither = enabled != 0;
    }
    if (values[Deband]) {
        uint64_t enabled = 0;
        if (!ParseUnsigned(*values[Deband], enabled) || enabled > 1) return std::nullopt;
        request.sourceDeband = enabled != 0;
    }
    // A rung this build cannot name is refused, never read as Standard: the
    // parent keyed the render for the rung it asked for. "standard" itself is
    // refused too, because the builder never sends it and a second spelling of
    // the default is a second wire contract nobody tests.
    if (values[CacheQuality]) {
        const std::wstring_view text = *values[CacheQuality];
        std::string narrow;
        for (const wchar_t character : text) {
            if (character > 0x7F) return std::nullopt;
            narrow.push_back(static_cast<char>(character));
        }
        EncoderQuality quality{};
        if (!ParseEncoderQuality(narrow, quality) || quality == EncoderQuality::Standard) return std::nullopt;
        request.quality = quality;
    }
    // Absent means 7, stated here rather than left to the struct's own default.
    // The two are different numbers and different decisions: 7 is the VERSION
    // contract - every helper before this flag existed encoded at p7, so an
    // older parent that cannot send it has to keep getting p7 - while the
    // struct's default is the product's choice, and moving that to p5 on a
    // measurement silently rewrote the wire contract when this line relied on
    // it. tests/NeuralWorkerTests.cpp pins both halves.
    request.nvencPreset = 7;
    if (values[NvencPreset]) {
        uint64_t preset = 0;
        if (!ParseUnsigned(*values[NvencPreset], preset) || !preset || preset > 7) return std::nullopt;
        request.nvencPreset = static_cast<uint32_t>(preset);
    }
    if (values[PauseEvent]) {
        uint64_t rawPause = 0;
        if (!ParseUnsigned(*values[PauseEvent], rawPause) || !rawPause) return std::nullopt;
        request.pauseEvent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(rawPause));
    }
    if (!ValidRequest(request)) return std::nullopt;
    return parsed;
}

NeuralRenderResult RunNeuralWorker(const std::filesystem::path& executable,
                                   const NeuralRenderRequest& request,
                                   OfflineNeuralRenderer::ProgressCallback progress,
                                   std::stop_token stop, const NeuralSegmentSink& segments,
                                   uint32_t crashRelaunchLimit,
                                   const std::function<void()>& processCreated,
                                   const NeuralColdStartCallback& helperTimeline)
{
    // Refusals every caller shares: a job the launcher cannot describe never
    // reaches a helper, resident or not.
    if (auto refusal = RefuseUnrunnableJob(executable, request)) return *refusal;
    return RunWithRelaunches(progress, segments, crashRelaunchLimit, [&](uint32_t launch) {
        return RunNeuralWorkerAttempt(executable, ForLaunch(request, launch), progress, segments, stop,
                                      false, processCreated, helperTimeline);
    }, [&](const NeuralRenderResult& failed) {
        return ProbeBeforeRelaunch(executable, stop, failed);
    });
}

// One resident helper, its pipes and the key it was started for. Defined here
// rather than in the header because everything in it is a handle the player
// has no business seeing.
struct ResidentNeuralHelper::Session {
    HelperProcess helper;
    resident_helper::HelperKey key;
    // Handle values as the helper sees them. Inheritance preserves the numbers,
    // which is what lets a job be described by the same argument vector the
    // helper would have been given on its command line: the parent's own copies
    // are closed, and these are what the helper must be told to write to.
    HANDLE helperMetadata{};
    HANDLE helperPause{};
    // The parent-side pause event this helper was launched with. A pause event
    // has to be inheritable at CreateProcess, so a resident helper's is fixed
    // for its life and a job carrying a different one could not be honoured -
    // it would be paused by an event nobody is setting. The player has exactly
    // one, so this is a guard against a future caller rather than a case.
    HANDLE launchPause{};
    // Set once this helper is the replacement for one that repaired its own
    // configuration, so it is never asked to repair it a second time.
    bool configurationRestarted{};
    // The helper's own path, because a job is delivered as the argv the helper
    // would have been started with and ParseWorkerArguments reads the mode out
    // of argv[1]. Sending the vector without argv[0] would shift every field by
    // one and the helper would refuse a job it understood perfectly.
    std::filesystem::path executable;

    bool Alive() const { return helper.Alive(); }
    // A Session is the only owner of its process and its pipes, so it cannot be
    // dropped on the floor: every path that ends a session goes through one of
    // the two below, and this catches any that ever forgets. EndHelper zeroes
    // the handles, so running twice is a no-op rather than a double close.
    ~Session() { Drop(); }
    // The helper is gone, or said something this parent will not read again.
    // Either way there is no orderly exit left to wait for.
    void Drop() { EndHelper(helper, 0); }
    void End() { EndHelper(helper, kResidentShutdownGrace); }

    std::wstring Start(const std::filesystem::path& executablePath, HANDLE pauseEvent,
                       resident_helper::IdleVramPolicy idleVramPolicy,
                       const std::function<void()>& processCreated)
    {
        executable = executablePath;
        StartOutcome started = StartHelper(executablePath,
            [idleVramPolicy](HANDLE metadata, HANDLE command, HANDLE pause, HANDLE parent) {
                return neural_worker_detail::BuildResidentArguments(metadata, command, pause, parent,
                                                                    idleVramPolicy);
            }, pauseEvent, true, processCreated);
        launchPause = pauseEvent;
        if (!started.helper.Valid()) return std::move(started.detail);
        helper = std::move(started.helper);
        helperMetadata = started.helperMetadata;
        helperPause = started.helperPause;
        // Answered with Ready whenever the helper gets to it. The first job
        // follows immediately rather than waiting for that answer: the pipe
        // holds both frames, and a round trip here would spend part of the cold
        // start residency exists to remove.
        WriteCommand(helper.command, CommandKind::Hello, nullptr, 0);
        return {};
    }

    enum class Dispatch { Sent, Gone, Unsendable };

    // Hands one job over as the argument vector the helper already accepts,
    // which keeps ParseWorkerArguments the single definition of what a job is.
    Dispatch Send(const NeuralRenderRequest& request)
    {
        if (request.pauseEvent != launchPause) return Dispatch::Unsendable;
        std::vector<std::wstring> arguments{executable.wstring()};
        const std::vector<std::wstring> job = neural_worker_detail::BuildWorkerArguments(
            request, helperMetadata, helperPause, configurationRestarted);
        arguments.insert(arguments.end(), job.begin(), job.end());
        if (arguments.size() > kMaximumJobArguments) return Dispatch::Unsendable;
        for (const std::wstring& argument : arguments) {
            if (argument.empty() || argument.size() * sizeof(wchar_t) > kMaximumJobArgumentBytes)
                return Dispatch::Unsendable;
        }
        const std::vector<std::byte> payload = EncodeJobArguments(arguments);
        if (payload.size() > kMaximumPayloadBytes) return Dispatch::Unsendable;
        return WriteCommand(helper.command, CommandKind::Job, payload.data(),
                            static_cast<uint32_t>(payload.size()))
            ? Dispatch::Sent : Dispatch::Gone;
    }
};

std::filesystem::path PlayerSettingsPath()
{
    const auto directory = platform_paths::ModuleDirectory();
    if (!directory) return {};
    return *directory / L"DLSSVideoPlayer.ini";
}

resident_helper::IdleVramPolicy ReadIdleVramPolicy(const std::filesystem::path& settingsIni)
{
    if (settingsIni.empty()) return resident_helper::kDefaultIdleVramPolicy;
    // Longer than either name, so a longer value is read back in full and then
    // refused rather than truncated into one that happens to match.
    wchar_t value[32]{};
    const DWORD length = GetPrivateProfileStringW(kIdleVramPolicySection, kIdleVramPolicyKey, L"",
                                                  value, static_cast<DWORD>(std::size(value)),
                                                  settingsIni.c_str());
    const auto policy = resident_helper::ParseIdleVramPolicy(std::wstring_view(value, length));
    return policy ? *policy : resident_helper::kDefaultIdleVramPolicy;
}

ResidentNeuralHelper::ResidentNeuralHelper()
    : ResidentNeuralHelper(ReadIdleVramPolicy(PlayerSettingsPath())) {}

ResidentNeuralHelper::ResidentNeuralHelper(resident_helper::IdleVramPolicy idleVramPolicy)
    : idleVramPolicy_(idleVramPolicy) {}

ResidentNeuralHelper::~ResidentNeuralHelper() { Release(); }

bool ResidentNeuralHelper::Resident() const { return session_ && session_->Alive(); }

void ResidentNeuralHelper::Release()
{
    if (!session_) return;
    session_->End();
    session_.reset();
}

NeuralRenderResult ResidentNeuralHelper::RunJob(const std::filesystem::path& executable,
                                                const resident_helper::HelperKey& key,
                                                const NeuralRenderRequest& request,
                                                const NeuralJobHooks& hooks, std::stop_token stop,
                                                resident_helper::HelperPlan* plan)
{
    using resident_helper::HelperPlan;
    // Looked at, not remembered: a resident helper exits by itself after 30 s
    // idle and after any job it could not finish, so the only trustworthy
    // answer comes from the process.
    resident_helper::ResidentState state;
    state.running = Resident();
    if (state.running) state.key = session_->key;
    const HelperPlan chosen = resident_helper::PlanForJob(state, key);
    if (plan) *plan = chosen;
    if (chosen == HelperPlan::SingleShot) {
        return RunNeuralWorker(executable, request, hooks.progress, stop, hooks.segments,
                               hooks.crashRelaunchLimit,
                               [&] { if (hooks.accepted) hooks.accepted(HelperPlan::SingleShot); },
                               hooks.helperTimeline);
    }
    if (auto refusal = RefuseUnrunnableJob(executable, request)) return *refusal;
    // The old helper holds the adapter and the runtime directory this job's
    // helper needs, so it goes first and completely.
    if (chosen == HelperPlan::Relaunch) Release();
    // What the job's helper actually was, which is not always what the policy
    // decided: a helper that was alive at the decision and gone by the
    // dispatch is a launch, and so is the replacement for a crashed one.
    bool firstAcceptance = true;
    const std::function<void(bool)> accepted = [&](bool launched) {
        if (hooks.accepted) {
            hooks.accepted(!launched ? HelperPlan::Reuse
                                     : (firstAcceptance && chosen == HelperPlan::Relaunch
                                            ? HelperPlan::Relaunch : HelperPlan::Launch));
        }
        firstAcceptance = false;
    };
    return RunWithRelaunches(hooks.progress, hooks.segments, hooks.crashRelaunchLimit,
        [&](uint32_t launch) {
            return RunAttempt(executable, key, ForLaunch(request, launch), hooks, stop, accepted);
        },
        [&](const NeuralRenderResult& failed) {
            // The dead helper's session is already dropped by the attempt that
            // judged it, so the probe gets the runtime directory to itself.
            return ProbeBeforeRelaunch(executable, stop, failed);
        });
}

NeuralRenderResult ResidentNeuralHelper::RunAttempt(const std::filesystem::path& executable,
                                                    const resident_helper::HelperKey& key,
                                                    const NeuralRenderRequest& request,
                                                    const NeuralJobHooks& hooks, std::stop_token stop,
                                                    const std::function<void(bool)>& accepted)
{
    for (bool configurationRestarted = false;; configurationRestarted = true) {
        NeuralRenderResult result;
        result.jobId = request.jobId;
        if (stop.stop_requested()) {
            result.cancelled = true;
            result.failure = NeuralRenderFailure::Cancelled;
            result.detail = L"Neural rendering was cancelled before the helper started.";
            return result;
        }
        // Per job, because a resident helper reports a timeline per job: the
        // first one measures a process starting, every later one measures only
        // the phase it actually paid for.
        MetadataReader reader(hooks.progress, hooks.segments, hooks.helperTimeline);
        // The reader outlives every judgement below, so a timeline or a VRAM
        // sample that arrived before a crash, a cancel or a rejected result is
        // still reported: the breakdown of a run that failed is the whole
        // point of measuring it.
        auto finish = [&](NeuralRenderResult judged) {
            StampHelperObservations(reader, judged);
            return judged;
        };

        // Two tries, because a helper that exited while idle is not an error to
        // report: the 30 s timeout, a self-invalidating exit after a failed job
        // and a shutdown from anywhere else all look like a closed pipe here,
        // and all of them mean start one and try again.
        bool dispatched = false;
        for (int attempt = 0; attempt < 2 && !dispatched; ++attempt) {
            const bool launching = !Resident();
            if (launching) {
                if (session_) session_->Drop();
                session_ = std::make_unique<Session>();
                session_->key = key;
                session_->configurationRestarted = configurationRestarted;
                std::wstring detail = session_->Start(executable, request.pauseEvent, idleVramPolicy_,
                                                      [&] { accepted(true); });
                if (!detail.empty()) {
                    session_.reset();
                    result.failure = NeuralRenderFailure::Protocol;
                    result.detail = std::move(detail);
                    return finish(std::move(result));
                }
            }
            const Session::Dispatch sent = session_->Send(request);
            if (sent == Session::Dispatch::Sent) {
                dispatched = true;
                // A reused helper's Launch phase ends here instead: handing the
                // job over is the last thing the parent does before the
                // helper's own clock starts.
                if (!launching) accepted(false);
                break;
            }
            if (sent == Session::Dispatch::Unsendable) {
                // The helper is fine; this job is not describable as a command
                // and no helper would accept it. Refuse the job, keep the helper.
                result.failure = NeuralRenderFailure::Protocol;
                result.detail = L"The neural render job could not be framed as a helper command.";
                return finish(std::move(result));
            }
            // The write found a closed pipe: the helper went away between the
            // decision and the dispatch.
            session_->Drop();
            session_.reset();
        }
        if (!dispatched) {
            result.failure = NeuralRenderFailure::Protocol;
            result.detail = L"The resident neural helper would not accept the job.";
            return finish(std::move(result));
        }

        const PumpOutcome pump = Pump(session_->helper, reader, stop, true);
        const bool exited = !session_->Alive();
        const HANDLE process = session_->helper.process;
        const std::optional<DWORD> exitCode = exited
            ? neural_worker_detail::ReadHelperExitCode([&](DWORD* code) {
                  return GetExitCodeProcess(process, code) != FALSE;
              })
            : std::optional<DWORD>(0);
        if (pump.malformed) {
            // Nothing this helper says afterwards can be trusted either.
            session_->Drop();
            session_.reset();
            result.failure = NeuralRenderFailure::Protocol;
            result.detail = L"The isolated neural helper returned malformed metadata.";
            return finish(std::move(result));
        }
        if (pump.completed) {
            result = AcceptResult(reader, request);
            // A helper that could not finish a job exits by itself: its session
            // log now carries a failure every later job would be scanned
            // against, and a removed device leaves it holding an unknown one.
            // A cancelled job and a finished one stay resident.
            const bool keep = (result.ok || result.cancelled) && session_->Alive();
            if (!keep) {
                session_->Drop();
                session_.reset();
            }
            return finish(std::move(result));
        }
        if (exited && exitCode == neural_worker_detail::kConfigurationChangedExitCode &&
            !configurationRestarted) {
            // The helper repaired the proxy's startup settings and exited for a
            // replacement to be launched once. The replacement measures its own
            // cold start, so this one's timeline is discarded with it.
            session_->Drop();
            session_.reset();
            continue;
        }
        session_->Drop();
        session_.reset();
        if (pump.cancelled || stop.stop_requested()) {
            result.cancelled = true;
            result.failure = NeuralRenderFailure::Cancelled;
            result.detail = L"Neural rendering was cancelled.";
            return finish(std::move(result));
        }
        if (exited && exitCode != DWORD{0}) {
            result.failure = NeuralRenderFailure::WorkerCrashed;
            result.detail = exitCode
                ? L"The isolated neural helper exited with code " + std::to_wstring(*exitCode) +
                      L" before producing a result."
                : std::wstring(L"The isolated neural helper exited before producing a result, and its exit "
                               L"code could not be read.");
            return finish(std::move(result));
        }
        // An exit code of zero with no result is the helper walking away from a
        // job it accepted, which is a broken contract rather than a crash.
        result.failure = NeuralRenderFailure::Protocol;
        result.detail = L"The isolated neural helper returned incomplete metadata.";
        return finish(std::move(result));
    }
}

neural_worker_detail::MetadataStreamOutcome neural_worker_detail::DecodeMetadataStream(
    std::span<const std::byte> bytes)
{
    MetadataStreamOutcome outcome;
    NeuralSegmentSink sink;
    sink.onSegment = [&](const NeuralRenderSegment& segment) { outcome.segments.push_back(segment); };
    sink.onRestart = [&] { ++outcome.restarts; outcome.segments.clear(); };
    MetadataReader reader([&](const NeuralRenderProgress&) { ++outcome.progressUpdates; }, sink);
    // Deliberately fragmented: the pipe delivers arbitrary chunks and a message
    // header can straddle two reads.
    constexpr size_t chunkBytes = 7;
    for (size_t offset = 0; offset < bytes.size(); offset += chunkBytes) {
        if (!reader.Push(bytes.subspan(offset, std::min(chunkBytes, bytes.size() - offset)))) break;
    }
    outcome.malformed = reader.Malformed();
    outcome.complete = reader.Complete();
    outcome.timeline = reader.Timeline();
    if (const auto& metrics = reader.Metrics()) outcome.metrics = *metrics;
    return outcome;
}

bool neural_worker_detail::SendShutdownAndCloseBounded(HANDLE command,
                                                       std::chrono::milliseconds budget)
{
    if (!command) return true;
    // The promise is shared so the thread can outlive this frame on the one
    // path where it has to.
    auto finished = std::make_shared<std::promise<bool>>();
    std::future<bool> written = finished->get_future();
    std::thread writer([command, finished] {
        const bool ok = WriteCommand(command, CommandKind::Shutdown, nullptr, 0);
        CloseHandle(command);
        finished->set_value(ok);
    });
    if (written.wait_for(budget) == std::future_status::ready) {
        writer.join();
        return written.get();
    }
    // The helper is not reading. Cancel the blocked write so the thread can
    // finish; CancelIoEx reaches synchronous I/O issued by another thread of
    // this process.
    CancelIoEx(command, nullptr);
    if (written.wait_for(budget) == std::future_status::ready) {
        writer.join();
        return false;
    }
    // CancelIoEx did not take. The caller is about to kill the helper, which
    // breaks the pipe, which releases this thread to close the handle and
    // exit. Letting it go is the price of never hanging shutdown - and the
    // alternative, closing a handle a blocked write still holds, is worse.
    writer.detach();
    return false;
}

neural_worker_detail::MetadataDrainPass neural_worker_detail::DrainMetadataPipeOnce(HANDLE pipe)
{
    MetadataDrainPass pass;
    // The caller keeps its handle; the reader owns a duplicate of it.
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), pipe, GetCurrentProcess(), &duplicate, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        pass.malformed = true;
        return pass;
    }
    MetadataPipe metadata(duplicate);
    if (!metadata.Valid()) {
        pass.malformed = true;
        return pass;
    }
    // Let the reader catch up with what is already in the pipe - until its
    // read-ahead is full or the pipe has nothing left for it - so the pass
    // below sees what a pump that had been waiting would see.
    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < 5000 && metadata.Buffered() < kMetadataReadAheadBytes) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || !available) break;
        Sleep(1);
    }
    MetadataReader reader({}, {});
    const bool ok = reader.ReadAvailable(metadata, &pass.budgetSpent, &pass.bytesRead);
    pass.malformed = !ok || reader.Malformed();
    return pass;
}

NeuralPreflightResult RunNeuralPreflight(const std::filesystem::path& executable, std::stop_token stop)
{
    NeuralPreflightResult result;
    std::error_code fileError;
    if (executable.empty() || !std::filesystem::is_regular_file(executable, fileError) || fileError) {
        result.detail = L"The isolated neural helper executable is unavailable.";
        return result;
    }
    for (bool restarted = false;; restarted = true) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            result.detail = L"Neural preflight was cancelled.";
            return result;
        }
        MetadataReader reader({}, {});
        const LaunchOutcome launch = LaunchHelper(executable,
            [&](HANDLE metadata, HANDLE) { return neural_worker_detail::BuildPreflightArguments(metadata, restarted); },
            nullptr, reader, stop, {});
        if (!launch.launched) { result.detail = launch.detail; return result; }
        if (launch.cancelled || stop.stop_requested()) {
            result.cancelled = true;
            result.detail = L"Neural preflight was cancelled.";
            return result;
        }
        if (launch.exitCode == neural_worker_detail::kConfigurationChangedExitCode && !restarted &&
            !reader.Malformed()) continue;
        if (launch.exitCode != 0) {
            result.detail = L"The neural preflight helper exited with code " + std::to_wstring(launch.exitCode) + L".";
            return result;
        }
        if (!reader.PreflightComplete()) {
            result.detail = reader.Malformed() ? L"The neural preflight helper returned malformed metadata." :
                                                 L"The neural preflight helper returned no receipt.";
            return result;
        }
        result.ok = reader.Preflight().ok;
        result.json = reader.Preflight().json;
        if (!result.ok) {
            const ReceiptDiagnosis diagnosis = ScanReceiptDiagnosis(result.json);
            result.cause = diagnosis.cause;
            result.detail = diagnosis.detail.empty() ? L"The neural runtime preflight did not arm feature 18." :
                                                       diagnosis.detail;
        }
        return result;
    }
}

void NeuralPreflightLatch::RecordFailure(const NeuralPreflightKey& key, std::wstring detail)
{
    const std::lock_guard<std::mutex> guard(mutex_);
    key_ = key;
    detail_ = std::move(detail);
    json_.clear();
    failed_ = true;
    passed_ = false;
}

void NeuralPreflightLatch::RecordSuccess(const NeuralPreflightKey& key, std::string json)
{
    const std::lock_guard<std::mutex> guard(mutex_);
    key_ = key;
    detail_.clear();
    json_ = std::move(json);
    failed_ = false;
    // A pass without a receipt is not reusable: the render's receipt would lose
    // the probe evidence it is supposed to carry.
    passed_ = !json_.empty();
}

std::wstring NeuralPreflightLatch::LatchedFailureDetail(const NeuralPreflightKey& key) const
{
    const std::lock_guard<std::mutex> guard(mutex_);
    return failed_ && key_ == key ? detail_ : std::wstring{};
}

std::string NeuralPreflightLatch::LatchedSuccessJson(const NeuralPreflightKey& key) const
{
    const std::lock_guard<std::mutex> guard(mutex_);
    return passed_ && key_ == key ? json_ : std::string{};
}

void NeuralPreflightLatch::Invalidate()
{
    const std::lock_guard<std::mutex> guard(mutex_);
    key_ = {};
    detail_.clear();
    json_.clear();
    failed_ = false;
    passed_ = false;
}

namespace {
// Identity the stored verdict belongs to, written as the file's first line and
// compared on load, so the file name only has to be a short unique-ish label.
// Losslessly encoded, so two cards whose names differ only outside ASCII get
// two identities. The old converter narrowed both to "" and they shared one -
// a verdict probed on an RTX 4080 was served for an RTX 5090. Encoding rather
// than refusing keeps the stored verdict working for those machines instead
// of costing them a five second probe on every launch.
std::string PreflightIdentity(const NeuralPreflightKey& key)
{
    return narrow_text::IdentityEncoded(key.gpu) + '|' +
           narrow_text::IdentityEncoded(key.driver) + '|' + key.runtimeDigest;
}
} // namespace

std::filesystem::path NeuralPreflightReceiptPath(const std::filesystem::path& cacheRoot,
                                                 const NeuralPreflightKey& key)
{
    if (cacheRoot.empty()) return {};
    // No identity, no receipt. An unrepresentable GPU or driver name narrows
    // to nothing, and every such name would otherwise share one filename AND
    // compare equal to the stored line - which is how one card's verdict came
    // to be served for another. Failing closed costs a preflight probe on a
    // machine whose card has a non-ASCII name; the alternative costs
    // correctness on every such machine.
    const std::string identity = PreflightIdentity(key);
    if (identity.empty()) return {};
    // FNV-1a, the same label hash the runtime lease uses for its mutex name: the
    // identity itself is stored in the file, so this only has to be a filename.
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const char c : identity) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 0x100000001b3ull;
    }
    wchar_t name[32]{};
    swprintf_s(name, L"%016llx.txt", static_cast<unsigned long long>(hash));
    return cacheRoot / L"preflight" / name;
}

std::string LoadNeuralPreflightReceipt(const std::filesystem::path& cacheRoot,
                                       const NeuralPreflightKey& key)
{
    const auto path = NeuralPreflightReceiptPath(cacheRoot, key);
    if (path.empty()) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string identity;
    if (!std::getline(input, identity) || identity != PreflightIdentity(key)) return {};
    const std::string json{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{}};
    // Only a receipt that records a pass may stand in for a probe, and only a
    // whole one. This used to search for "ok":true, which sits in the first
    // bytes of the receipt, so a file cut short by a crash mid-write still
    // read as a pass. Parsed, the truncated document is simply not JSON.
    strict_json::JsonValue root;
    if (!strict_json::JsonParser(json).ParseDocument(root) || root.kind != strict_json::JsonValue::Kind::Object)
        return {};
    const strict_json::JsonValue* ok = root.Member("ok");
    if (!ok || ok->kind != strict_json::JsonValue::Kind::Bool || !ok->boolean) return {};
    return json;
}

bool StoreNeuralPreflightReceipt(const std::filesystem::path& cacheRoot,
                                 const NeuralPreflightKey& key, std::string_view json)
{
    const auto path = NeuralPreflightReceiptPath(cacheRoot, key);
    if (path.empty() || json.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    // A reader sees the previous verdict or the new one and never part of one;
    // the temporary is unique per process and call, so two players storing the
    // same verdict never share one.
    return static_cast<bool>(
        atomic_file::Replace(path, PreflightIdentity(key) + "\n" + std::string(json)));
}

std::string MarkReusedNeuralPreflight(std::string_view json)
{
    const size_t brace = json.find('{');
    if (brace == std::string_view::npos) return std::string(json);
    std::string stamped(json.substr(0, brace + 1));
    stamped += "\"reusedVerdict\":true,";
    stamped += json.substr(brace + 1);
    return stamped;
}


std::wstring NeuralRuntimeLease::MutexName(const std::filesystem::path& runtimeDirectory)
{
    // FNV-1a over the normalized, lower-cased directory: every process must
    // derive the same name for the same runtime, and the name must stay well
    // inside the kernel object-name length limit for long paths. Global rather
    // than session-local: the runtime directory is shared by every player on
    // the machine, and two sessions each rewriting its ReShade.ini under a
    // lease only their own session could see excluded nothing.
    std::wstring path = runtimeDirectory.lexically_normal().wstring();
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t character : path) {
        if (character >= L'A' && character <= L'Z') character = wchar_t(character - L'A' + L'a');
        if (character == L'/') character = L'\\';
        hash = (hash ^ static_cast<uint64_t>(character)) * 1099511628211ull;
    }
    std::wstring name = L"Global\\DLSSVideoPlayer.neural-runtime.";
    for (int shift = 60; shift >= 0; shift -= 4) name.push_back(L"0123456789abcdef"[(hash >> shift) & 0xF]);
    return name;
}

NeuralRuntimeLease::NeuralRuntimeLease(const std::filesystem::path& runtimeDirectory,
                                       std::chrono::milliseconds wait)
{
    if (runtimeDirectory.empty()) return;
    const std::wstring name = MutexName(runtimeDirectory);
    mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!mutex_) {
        // A Global object another account created with a DACL this one cannot
        // open falls back to the session-local name: exclusion within this
        // session is worth more than none at all.
        const std::wstring local = L"Local\\" + name.substr(name.find(L'\\') + 1);
        mutex_ = CreateMutexW(nullptr, FALSE, local.c_str());
        if (!mutex_) return;
    }
    const DWORD milliseconds = wait.count() <= 0
        ? 0u : static_cast<DWORD>(std::min<long long>(wait.count(), INFINITE - 1));
    // WAIT_ABANDONED means the previous holder died without releasing it: the
    // runtime is ours, and its stale log is retired before the next launch.
    const DWORD result = WaitForSingleObject(mutex_, milliseconds);
    held_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
}

NeuralRuntimeLease::~NeuralRuntimeLease()
{
    if (mutex_ && held_) ReleaseMutex(mutex_);
    if (mutex_) CloseHandle(mutex_);
}
