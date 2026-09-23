#pragma once

// Versioned metadata pipe between the hook-free player and the isolated
// neural helper. Only progress, finalized-segment announcements, the
// cold-start timeline, preflight and final results cross the pipe; encoded
// video stays in the cache. This header is the single definition shared by the
// parent launcher, the helper executable and the tests.

#include "OfflineNeuralRenderer.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neural_worker_protocol {

inline constexpr uint32_t kMagic = 0x3152574Eu; // NWR1
// 6 since the pipe is no longer one-way: the parent can hand a resident helper
// another job over a command channel instead of spawning a process per render.
// The parent rejects any header whose version is not exactly this, so a helper
// left over from an older build in neural-runtime/ fails closed instead of
// having a v5 stream read as a v6 one - and a v5 helper, which would see a
// command frame as malformed input, never has to.
inline constexpr uint16_t kVersion = 6;
inline constexpr uint32_t kMaximumPayloadBytes = 64 * 1024;
inline constexpr uint32_t kMaximumDetailBytes = 4 * 1024;
// A segment name is a bare file name joined to the staging directory by the
// parent, never a path.
inline constexpr uint32_t kMaximumSegmentNameBytes = 512;
// A job is handed over as the argument vector the helper already accepts on the
// command line. That is deliberate: `ParseWorkerArguments` stays the single
// definition of what a job is and the single place that validates one, so
// residency changes how a job arrives and not what a job means. A parallel
// struct here would be a second schema to keep in step with it.
inline constexpr uint32_t kMaximumJobArguments = 64;
inline constexpr uint32_t kMaximumJobArgumentBytes = 4 * 1024;

enum class WireKind : uint16_t {
    Progress = 1, Result = 2, Preflight = 3, Segment = 4, Timeline = 5, Ready = 6, Memory = 7,
    Metrics = 8
};

// Where in a resident helper's cycle a WireMemory sample was taken. The pair
// is what makes the idle-VRAM policy decidable: PostJob is what residency
// parks, Idle is what the policy left behind once the grace elapsed.
enum class MemoryStage : uint8_t { PostJob = 0, Idle = 1 };

// Parent to helper, on its own pipe. `Hello` asks a freshly launched helper to
// announce itself, which it does with `WireKind::Ready`; `Job` carries an
// argument vector; `Cancel` asks the running job to stop, which still reports a
// `Result` with `cancelled` set; `Shutdown` asks the helper to exit. Only `Job`
// carries a payload.
enum class CommandKind : uint16_t { Hello = 1, Job = 2, Cancel = 3, Shutdown = 4 };

#pragma pack(push, 1)
struct WireHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t payloadBytes;
};

struct WireProgress {
    uint32_t phase;
    uint64_t completedFrames;
    uint64_t totalFrames;
    uint64_t bytes;
    int64_t elapsedMilliseconds;
    int64_t estimatedRemainingMilliseconds;
    uint32_t recovering;
    uint32_t retries;
};

struct WireResult {
    uint8_t ok;
    uint8_t cancelled;
    uint8_t encoder;
    uint8_t feature18ArmedBeforeCapture;
    uint8_t nativeResolution;
    uint8_t inlineInterceptionContract;
    uint8_t feature18Created;
    uint8_t feature18Evaluated;
    uint8_t laterFailure;
    uint8_t failure;
    // 1 for a job that asked for Super Resolution alone and ran with the neural
    // add-on disabled. Zero is "neural", which is what every helper that wrote
    // this byte as reserved produced, so an older helper still decodes as the
    // neural job it was. It took the first reserved byte rather than a new
    // field because the Python decoder reads this struct by offset.
    uint8_t superResolutionOnly;
    uint8_t reserved[5];
    uint64_t frameCount;
    int64_t duration100ns;
    uint64_t nativeEvaluations;
    uint64_t verifiedNeuralFrames;
    uint64_t highestObservedEvaluation;
    uint64_t jobId;
    uint32_t historyResets;
    uint32_t frameRetries;
    int64_t firstTimestamp100ns;
    double neuralGpuMsP50;
    double neuralGpuMsP95;
    double neuralGpuMsMax;
    double guideMsMean;
    double captureMsMean;
    uint64_t peakLocalVramMiB;
    uint64_t timingSamples;
    // Scene-cut decisions the helper's guide generator took and withheld. Kept
    // ahead of detailBytes so that field stays the last one, describing the
    // payload that follows the struct.
    uint32_t acceptedStrongCuts;
    uint32_t acceptedWeakCuts;
    uint32_t suppressedCuts;
    uint32_t detailBytes;
};

// Followed by `nameBytes` of UTF-16 file name (name only, no directory). Sent
// only after that file's encoder exited successfully, so the file is complete
// and independently playable. Non-terminal: segments arrive in strictly
// increasing index order starting at 0, and a repeated 0 means the helper
// restarted the sequence from scratch.
struct WireSegment {
    uint64_t index;
    uint64_t firstFrameNumber;
    int64_t firstTimestamp100ns;
    uint64_t frameCount;
    int64_t frameDuration100ns;
    uint32_t nameBytes;
};

// Followed by `jsonBytes` of UTF-8 JSON describing the probed runtime.
struct WirePreflight {
    uint8_t ok;
    uint8_t reserved[3];
    uint32_t jsonBytes;
};

// The helper's own share of the cold-start timeline. Non-terminal and sent at
// most once per helper: it is the startup breakdown of one process, not a
// running measurement, and it travels ahead of the result so a cancelled or
// failed run still delivers it. `present` is a bit per NeuralColdStartPhase;
// only the helper-owned phases may be set, and a phase without its bit did not
// happen rather than took no time.
struct WireTimeline {
    uint32_t present;
    uint8_t reserved[4];
    int64_t microseconds[kNeuralColdStartPhaseCount];
};

// A point sample of the helper process's local-segment video memory, with the
// idle policy that process was launched under. Non-terminal and fixed size,
// like WireTimeline; unlike it, one is sent per stage per job cycle rather
// than once per process.
//
// The Idle sample is written with no job in flight and nobody pumping, so it
// waits in the pipe until the next job's reader drains it - which is the job
// whose first frame pays for whatever the policy gave back, so that is the
// receipt it belongs on. A helper that exits on the idle timeout instead
// leaves it unread, and logs it as well for exactly that case.
//
// `featureHeld` records the mechanism rather than a claim about the runtime:
// after a FreeFeature idle sample it is 0 because the workset was handed back,
// and whether the runtime actually returned the memory is the difference
// between this sample and the PostJob one before it.
struct WireMemory {
    uint8_t stage;        // MemoryStage
    uint8_t policy;       // resident_helper::IdleVramPolicy, fixed for the process
    uint8_t featureHeld;  // feature 18 is still armed at this sample
    uint8_t reserved[5];
    uint64_t localVramMiB;
};
// The render's temporal metrics (TemporalMetrics.h), at most once per job and
// ahead of its result. Its own message for the reason the timeline has one:
// WireResult is a fixed layout a released decoder reads by offset.
struct WireMetrics {
    uint64_t frames;
    uint64_t pairs;
    uint32_t shots;
    uint8_t reserved[4];
    double sourceWarpError;
    double outputWarpError;
    double sourceSigma;
    double outputSigma;
    double lumaShift;
    double colorDelta;
};
#pragma pack(pop)

static_assert(sizeof(WireHeader) == 12);
static_assert(sizeof(WireProgress) == 52);
static_assert(sizeof(WireResult) == 152);
static_assert(sizeof(WirePreflight) == 8);
static_assert(sizeof(WireSegment) == 44);
static_assert(sizeof(WireTimeline) == 80);
static_assert(sizeof(WireMemory) == 16);
static_assert(sizeof(WireMetrics) == 72);

inline bool IsKnownPhase(uint32_t phase) noexcept
{
    return phase <= static_cast<uint32_t>(NeuralRenderPhase::Recovering);
}

inline bool IsKnownFailure(uint32_t failure) noexcept
{
    return failure <= static_cast<uint32_t>(NeuralRenderFailure::Protocol);
}

inline bool IsKnownEncoder(uint8_t encoder) noexcept
{
    return encoder == static_cast<uint8_t>(EncoderKind::HevcNvenc) ||
           encoder == static_cast<uint8_t>(EncoderKind::H264Software);
}

inline bool IsBooleanByte(uint8_t value) noexcept { return value == 0 || value == 1; }

// A segment file name must be usable exactly as written inside the staging
// directory: no directory component, no traversal, no NUL.
inline bool IsValidSegmentName(std::wstring_view name) noexcept
{
    return !name.empty() && name.find_first_of(L"\\/:") == std::wstring_view::npos &&
           name.find(L'\0') == std::wstring_view::npos &&
           name.find(L"..") == std::wstring_view::npos;
}

inline bool WriteAll(HANDLE handle, const void* data, size_t bytes)
{
    const auto* cursor = static_cast<const std::byte*>(data);
    while (bytes) {
        const DWORD chunk = static_cast<DWORD>(bytes > MAXDWORD ? MAXDWORD : bytes);
        DWORD written = 0;
        if (!WriteFile(handle, cursor, chunk, &written, nullptr) || !written) return false;
        cursor += written;
        bytes -= written;
    }
    return true;
}

// Payloads up to this size travel in the same WriteFile as their header: every
// message but a preflight receipt or a job vector, including a result carrying
// the longest detail it may. Each WriteFile on a pipe is a kernel transition
// and, with a reader blocked on the other end, a wake-up for half a message.
inline constexpr uint32_t kCoalescedPayloadBytes = static_cast<uint32_t>(sizeof(WireResult)) + kMaximumDetailBytes;

// One frame in either direction. The bytes are exactly the header followed by
// the payload whichever way they are written; only the number of writes
// differs, so the wire format does not.
inline bool WriteFrame(HANDLE handle, uint16_t kind, const void* payload, uint32_t payloadBytes)
{
    const WireHeader header{kMagic, kVersion, kind, payloadBytes};
    if (payloadBytes <= kCoalescedPayloadBytes) {
        std::array<std::byte, sizeof(WireHeader) + kCoalescedPayloadBytes> frame;
        std::memcpy(frame.data(), &header, sizeof(header));
        if (payloadBytes) std::memcpy(frame.data() + sizeof(header), payload, payloadBytes);
        return WriteAll(handle, frame.data(), sizeof(header) + payloadBytes);
    }
    return WriteAll(handle, &header, sizeof(header)) && WriteAll(handle, payload, payloadBytes);
}

inline bool WriteMessage(HANDLE handle, WireKind kind, const void* payload, uint32_t payloadBytes)
{
    return WriteFrame(handle, static_cast<uint16_t>(kind), payload, payloadBytes);
}

inline bool IsKnownCommand(uint16_t kind) noexcept
{
    return kind >= static_cast<uint16_t>(CommandKind::Hello) &&
           kind <= static_cast<uint16_t>(CommandKind::Shutdown);
}

inline bool WriteCommand(HANDLE handle, CommandKind kind, const void* payload, uint32_t payloadBytes)
{
    return WriteFrame(handle, static_cast<uint16_t>(kind), payload, payloadBytes);
}

// `count`, then `count` pairs of byte length and UTF-16LE text. Lengths are
// byte counts rather than character counts because that is what the reader
// bounds-checks against, and an odd length is rejected rather than rounded.
inline std::vector<std::byte> EncodeJobArguments(const std::vector<std::wstring>& arguments)
{
    std::vector<std::byte> payload;
    const uint32_t count = static_cast<uint32_t>(arguments.size());
    payload.resize(sizeof(count));
    std::memcpy(payload.data(), &count, sizeof(count));
    for (const std::wstring& argument : arguments) {
        const uint32_t bytes = static_cast<uint32_t>(argument.size() * sizeof(wchar_t));
        const size_t offset = payload.size();
        payload.resize(offset + sizeof(bytes) + bytes);
        std::memcpy(payload.data() + offset, &bytes, sizeof(bytes));
        if (bytes) std::memcpy(payload.data() + offset + sizeof(bytes), argument.data(), bytes);
    }
    return payload;
}

// Rejects anything it cannot account for exactly: a count over the cap, a
// length that overruns the payload, an odd length, an empty or NUL-bearing
// argument, or trailing bytes nobody claimed. A job that does not decode is a
// protocol failure, not a job to attempt with whatever survived.
inline std::optional<std::vector<std::wstring>> DecodeJobArguments(std::span<const std::byte> payload)
{
    uint32_t count = 0;
    if (payload.size() < sizeof(count)) return std::nullopt;
    std::memcpy(&count, payload.data(), sizeof(count));
    if (!count || count > kMaximumJobArguments) return std::nullopt;
    std::vector<std::wstring> arguments;
    arguments.reserve(count);
    size_t cursor = sizeof(count);
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t bytes = 0;
        if (payload.size() - cursor < sizeof(bytes)) return std::nullopt;
        std::memcpy(&bytes, payload.data() + cursor, sizeof(bytes));
        cursor += sizeof(bytes);
        if (!bytes || bytes > kMaximumJobArgumentBytes || bytes % sizeof(wchar_t)) return std::nullopt;
        if (payload.size() - cursor < bytes) return std::nullopt;
        std::wstring argument(bytes / sizeof(wchar_t), L'\0');
        std::memcpy(argument.data(), payload.data() + cursor, bytes);
        cursor += bytes;
        if (argument.find(L'\0') != std::wstring::npos) return std::nullopt;
        arguments.push_back(std::move(argument));
    }
    if (cursor != payload.size()) return std::nullopt;
    return arguments;
}

inline WireProgress EncodeProgress(const NeuralRenderProgress& progress)
{
    return WireProgress{
        static_cast<uint32_t>(progress.phase), progress.completedFrames, progress.totalFrames,
        progress.bytes,
        std::chrono::duration_cast<std::chrono::milliseconds>(progress.elapsed).count(),
        std::chrono::duration_cast<std::chrono::milliseconds>(progress.estimatedRemaining).count(),
        static_cast<uint32_t>(progress.recovering), progress.retries};
}

inline std::optional<NeuralRenderProgress> DecodeProgress(std::span<const std::byte> payload)
{
    if (payload.size() != sizeof(WireProgress)) return std::nullopt;
    WireProgress wire{};
    std::memcpy(&wire, payload.data(), sizeof(wire));
    if (!IsKnownPhase(wire.phase) || wire.completedFrames > wire.totalFrames ||
        wire.elapsedMilliseconds < 0 || wire.estimatedRemainingMilliseconds < 0 ||
        !IsKnownFailure(wire.recovering)) return std::nullopt;
    const bool recovering = static_cast<NeuralRenderPhase>(wire.phase) == NeuralRenderPhase::Recovering;
    if (recovering != (wire.recovering != 0)) return std::nullopt;
    NeuralRenderProgress progress;
    progress.phase = static_cast<NeuralRenderPhase>(wire.phase);
    progress.completedFrames = wire.completedFrames;
    progress.totalFrames = wire.totalFrames;
    progress.bytes = wire.bytes;
    progress.elapsed = std::chrono::milliseconds(wire.elapsedMilliseconds);
    progress.estimatedRemaining = std::chrono::milliseconds(wire.estimatedRemainingMilliseconds);
    progress.recovering = static_cast<NeuralRenderFailure>(wire.recovering);
    progress.retries = wire.retries;
    return progress;
}

// `frameDuration100ns` is the job's CFR frame duration; the decoder rebuilds
// the segment's exclusive end from it, so the message stays fixed size.
inline std::vector<std::byte> EncodeSegment(const NeuralRenderSegment& segment,
                                            int64_t frameDuration100ns)
{
    std::wstring name = segment.fileName;
    const size_t maxCharacters = kMaximumSegmentNameBytes / sizeof(wchar_t);
    if (name.size() > maxCharacters) name.resize(maxCharacters);
    WireSegment wire{};
    wire.index = segment.index;
    wire.firstFrameNumber = segment.firstFrameNumber;
    wire.firstTimestamp100ns = segment.firstTimestamp100ns;
    wire.frameCount = segment.frameCount;
    wire.frameDuration100ns = frameDuration100ns;
    wire.nameBytes = static_cast<uint32_t>(name.size() * sizeof(wchar_t));
    std::vector<std::byte> payload(sizeof(wire) + wire.nameBytes);
    std::memcpy(payload.data(), &wire, sizeof(wire));
    if (wire.nameBytes) std::memcpy(payload.data() + sizeof(wire), name.data(), wire.nameBytes);
    return payload;
}

inline std::optional<NeuralRenderSegment> DecodeSegment(std::span<const std::byte> payload)
{
    if (payload.size() < sizeof(WireSegment)) return std::nullopt;
    WireSegment wire{};
    std::memcpy(&wire, payload.data(), sizeof(wire));
    if (!wire.frameCount || wire.frameDuration100ns <= 0 || wire.firstTimestamp100ns < 0 ||
        !wire.nameBytes || wire.nameBytes > kMaximumSegmentNameBytes ||
        (wire.nameBytes % sizeof(wchar_t)) != 0 ||
        payload.size() != sizeof(WireSegment) + wire.nameBytes) return std::nullopt;
    const uint64_t duration = static_cast<uint64_t>(wire.frameDuration100ns);
    const uint64_t headroom = static_cast<uint64_t>(INT64_MAX - wire.firstTimestamp100ns);
    if (wire.frameCount > headroom / duration) return std::nullopt;
    NeuralRenderSegment segment;
    segment.index = wire.index;
    segment.firstFrameNumber = wire.firstFrameNumber;
    segment.firstTimestamp100ns = wire.firstTimestamp100ns;
    segment.frameCount = wire.frameCount;
    segment.end100ns = wire.firstTimestamp100ns + static_cast<int64_t>(wire.frameCount * duration);
    const auto* name = reinterpret_cast<const wchar_t*>(payload.data() + sizeof(WireSegment));
    segment.fileName.assign(name, name + wire.nameBytes / sizeof(wchar_t));
    if (!IsValidSegmentName(segment.fileName)) return std::nullopt;
    return segment;
}

inline WireTimeline EncodeTimeline(const NeuralColdStartTimeline& timeline)
{
    WireTimeline wire{};
    for (uint32_t index = 0; index < kNeuralColdStartPhaseCount; ++index) {
        const auto phase = timeline.Phase(static_cast<NeuralColdStartPhase>(index));
        if (!phase) continue;
        wire.present |= 1u << index;
        wire.microseconds[index] = phase->count();
    }
    return wire;
}

// A helper may only claim the phases it can see, and only with durations it
// could have measured: anything else is a malformed helper, not a slow one.
inline std::optional<NeuralColdStartTimeline> DecodeTimeline(std::span<const std::byte> payload)
{
    if (payload.size() != sizeof(WireTimeline)) return std::nullopt;
    WireTimeline wire{};
    std::memcpy(&wire, payload.data(), sizeof(wire));
    if (!wire.present || (wire.present & ~kNeuralColdStartHelperPhases) ||
        wire.reserved[0] || wire.reserved[1] || wire.reserved[2] || wire.reserved[3]) return std::nullopt;
    NeuralColdStartTimeline timeline;
    for (uint32_t index = 0; index < kNeuralColdStartPhaseCount; ++index) {
        if (!(wire.present & (1u << index))) {
            // A slot without its bit describes nothing, so it must carry nothing.
            if (wire.microseconds[index]) return std::nullopt;
            continue;
        }
        if (wire.microseconds[index] < 0) return std::nullopt;
        timeline.Record(static_cast<NeuralColdStartPhase>(index),
                        std::chrono::microseconds(wire.microseconds[index]));
    }
    return timeline;
}

inline WireMetrics EncodeMetrics(const TemporalMetrics& metrics)
{
    WireMetrics wire{};
    wire.frames = metrics.frames;
    wire.pairs = metrics.pairs;
    wire.shots = metrics.shots;
    wire.sourceWarpError = metrics.sourceWarpError;
    wire.outputWarpError = metrics.outputWarpError;
    wire.sourceSigma = metrics.sourceSigma;
    wire.outputSigma = metrics.outputSigma;
    wire.lumaShift = metrics.lumaShift;
    wire.colorDelta = metrics.colorDelta;
    return wire;
}

// Refused unless it could describe a render: counts that cannot coexist, or a
// value no 8-bit picture can produce, are a malformed helper and not a number to
// publish in a receipt.
inline std::optional<TemporalMetrics> DecodeMetrics(std::span<const std::byte> payload)
{
    if (payload.size() != sizeof(WireMetrics)) return std::nullopt;
    WireMetrics wire{};
    std::memcpy(&wire, payload.data(), sizeof(wire));
    for (const uint8_t byte : wire.reserved) if (byte) return std::nullopt;
    if (!wire.frames || wire.pairs >= wire.frames || wire.shots > wire.frames) return std::nullopt;
    const double values[]{wire.sourceWarpError, wire.outputWarpError, wire.sourceSigma, wire.outputSigma,
                          wire.lumaShift, wire.colorDelta};
    for (const double value : values) if (!std::isfinite(value) || std::abs(value) > 512.0) return std::nullopt;
    if (wire.sourceWarpError < 0.0 || wire.outputWarpError < 0.0 || wire.sourceSigma < 0.0 ||
        wire.outputSigma < 0.0 || wire.colorDelta < 0.0) return std::nullopt;
    TemporalMetrics metrics;
    metrics.frames = wire.frames;
    metrics.pairs = wire.pairs;
    metrics.shots = wire.shots;
    metrics.sourceWarpError = wire.sourceWarpError;
    metrics.outputWarpError = wire.outputWarpError;
    metrics.sourceSigma = wire.sourceSigma;
    metrics.outputSigma = wire.outputSigma;
    metrics.lumaShift = wire.lumaShift;
    metrics.colorDelta = wire.colorDelta;
    return metrics;
}

// One VRAM sample as the parent reads it back.
struct MemorySample {
    MemoryStage stage{MemoryStage::PostJob};
    resident_helper::IdleVramPolicy policy{resident_helper::kDefaultIdleVramPolicy};
    bool featureHeld{};
    uint64_t localVramMiB{};
};

inline WireMemory EncodeMemory(const MemorySample& sample)
{
    WireMemory wire{};
    wire.stage = static_cast<uint8_t>(sample.stage);
    wire.policy = static_cast<uint8_t>(sample.policy);
    wire.featureHeld = sample.featureHeld ? 1 : 0;
    wire.localVramMiB = sample.localVramMiB;
    return wire;
}

// A sample whose stage or policy this parent cannot name describes nothing it
// could attribute, so it is refused rather than recorded under a guess.
inline std::optional<MemorySample> DecodeMemory(std::span<const std::byte> payload)
{
    if (payload.size() != sizeof(WireMemory)) return std::nullopt;
    WireMemory wire{};
    std::memcpy(&wire, payload.data(), sizeof(wire));
    if (wire.stage > static_cast<uint8_t>(MemoryStage::Idle) ||
        wire.policy > static_cast<uint8_t>(resident_helper::IdleVramPolicy::FreeFeature) ||
        !IsBooleanByte(wire.featureHeld)) return std::nullopt;
    for (const uint8_t byte : wire.reserved) if (byte) return std::nullopt;
    MemorySample sample;
    sample.stage = static_cast<MemoryStage>(wire.stage);
    sample.policy = static_cast<resident_helper::IdleVramPolicy>(wire.policy);
    sample.featureHeld = wire.featureHeld != 0;
    sample.localVramMiB = wire.localVramMiB;
    return sample;
}

inline std::vector<std::byte> EncodeResult(const NeuralRenderResult& result)
{
    std::wstring detail = result.detail;
    const size_t maxCharacters = kMaximumDetailBytes / sizeof(wchar_t);
    if (detail.size() > maxCharacters) detail.resize(maxCharacters);
    WireResult wire{};
    wire.ok = result.ok ? 1 : 0;
    wire.cancelled = result.cancelled ? 1 : 0;
    wire.encoder = static_cast<uint8_t>(result.encoder);
    wire.feature18ArmedBeforeCapture = result.feature18ArmedBeforeCapture ? 1 : 0;
    wire.nativeResolution = result.evidence.nativeResolution ? 1 : 0;
    wire.inlineInterceptionContract = result.evidence.inlineInterceptionContract ? 1 : 0;
    wire.feature18Created = result.evidence.feature18Created ? 1 : 0;
    wire.feature18Evaluated = result.evidence.feature18Evaluated ? 1 : 0;
    wire.laterFailure = result.evidence.laterFailure ? 1 : 0;
    wire.failure = static_cast<uint8_t>(result.failure);
    wire.superResolutionOnly = result.neural ? 0 : 1;
    wire.frameCount = result.frameCount;
    wire.duration100ns = result.duration100ns;
    wire.nativeEvaluations = result.nativeEvaluations;
    wire.verifiedNeuralFrames = result.verifiedNeuralFrames;
    wire.highestObservedEvaluation = result.evidence.highestObservedEvaluation;
    wire.jobId = result.jobId;
    wire.historyResets = result.historyResets;
    wire.frameRetries = result.frameRetries;
    wire.firstTimestamp100ns = result.firstTimestamp100ns;
    wire.neuralGpuMsP50 = result.timing.neuralGpuMsP50;
    wire.neuralGpuMsP95 = result.timing.neuralGpuMsP95;
    wire.neuralGpuMsMax = result.timing.neuralGpuMsMax;
    wire.guideMsMean = result.timing.guideMsMean;
    wire.captureMsMean = result.timing.captureMsMean;
    wire.peakLocalVramMiB = result.timing.peakLocalVramMiB;
    wire.timingSamples = result.timing.samples;
    wire.acceptedStrongCuts = result.sceneCuts.acceptedStrong;
    wire.acceptedWeakCuts = result.sceneCuts.acceptedWeak;
    wire.suppressedCuts = result.sceneCuts.suppressed;
    wire.detailBytes = static_cast<uint32_t>(detail.size() * sizeof(wchar_t));
    std::vector<std::byte> payload(sizeof(wire) + wire.detailBytes);
    std::memcpy(payload.data(), &wire, sizeof(wire));
    if (wire.detailBytes) std::memcpy(payload.data() + sizeof(wire), detail.data(), wire.detailBytes);
    return payload;
}

// Decodes and validates a result payload. A successful result must carry a
// complete, self-consistent verification set; anything else is rejected so a
// malformed helper can never publish a render.
inline std::optional<NeuralRenderResult> DecodeResult(std::span<const std::byte> payload)
{
    if (payload.size() < sizeof(WireResult)) return std::nullopt;
    WireResult wire{};
    std::memcpy(&wire, payload.data(), sizeof(wire));
    auto zeroed = [](std::span<const uint8_t> bytes) {
        for (const uint8_t value : bytes) if (value) return false;
        return true;
    };
    auto finite = [](double value) { return value == value && value >= 0.0 && value <= 1e12; };
    if (!IsBooleanByte(wire.ok) || !IsBooleanByte(wire.cancelled) || !IsKnownEncoder(wire.encoder) ||
        !IsBooleanByte(wire.feature18ArmedBeforeCapture) || !IsBooleanByte(wire.nativeResolution) ||
        !IsBooleanByte(wire.inlineInterceptionContract) || !IsBooleanByte(wire.feature18Created) ||
        !IsBooleanByte(wire.feature18Evaluated) || !IsBooleanByte(wire.laterFailure) ||
        !IsBooleanByte(wire.superResolutionOnly) ||
        !IsKnownFailure(wire.failure) || !zeroed(wire.reserved) ||
        wire.detailBytes > kMaximumDetailBytes || (wire.detailBytes % sizeof(wchar_t)) != 0 ||
        payload.size() != sizeof(WireResult) + wire.detailBytes ||
        !finite(wire.neuralGpuMsP50) || !finite(wire.neuralGpuMsP95) || !finite(wire.neuralGpuMsMax) ||
        !finite(wire.guideMsMean) || !finite(wire.captureMsMean) ||
        wire.firstTimestamp100ns < 0) return std::nullopt;

    NeuralRenderResult result;
    result.ok = wire.ok != 0;
    result.cancelled = wire.cancelled != 0;
    result.encoder = static_cast<EncoderKind>(wire.encoder);
    result.frameCount = wire.frameCount;
    result.duration100ns = wire.duration100ns;
    result.nativeEvaluations = wire.nativeEvaluations;
    result.verifiedNeuralFrames = wire.verifiedNeuralFrames;
    result.feature18ArmedBeforeCapture = wire.feature18ArmedBeforeCapture != 0;
    result.evidence = {wire.nativeResolution != 0, wire.inlineInterceptionContract != 0,
        wire.feature18Created != 0, wire.feature18Evaluated != 0, wire.laterFailure != 0,
        wire.highestObservedEvaluation};
    result.failure = static_cast<NeuralRenderFailure>(wire.failure);
    result.neural = wire.superResolutionOnly == 0;
    result.jobId = wire.jobId;
    result.historyResets = wire.historyResets;
    result.frameRetries = wire.frameRetries;
    result.firstTimestamp100ns = wire.firstTimestamp100ns;
    result.timing = {wire.timingSamples, wire.neuralGpuMsP50, wire.neuralGpuMsP95, wire.neuralGpuMsMax,
        wire.guideMsMean, wire.captureMsMean, wire.peakLocalVramMiB};
    // Advisory evidence, not part of the verification set: the tally spans preroll and
    // every attempt, so it is not bounded by historyResets and there is nothing here a
    // consistency check could prove.
    result.sceneCuts = {wire.acceptedStrongCuts, wire.acceptedWeakCuts, wire.suppressedCuts};
    if (wire.detailBytes) {
        const auto* detail = reinterpret_cast<const wchar_t*>(payload.data() + sizeof(WireResult));
        result.detail.assign(detail, detail + wire.detailBytes / sizeof(wchar_t));
        if (result.detail.find(L'\0') != std::wstring::npos) return std::nullopt;
    }
    if (result.ok && (result.cancelled || !result.frameCount || result.duration100ns <= 0 ||
                      !result.nativeEvaluations ||
                      result.failure != NeuralRenderFailure::None)) return std::nullopt;
    // A neural result carries the complete feature-18 verification set. A Super
    // Resolution-only result carries the opposite, stated rather than left
    // blank: no frame verified as neural, nothing armed, and no feature-18
    // evaluation in the session log - a carrier-only job whose add-on ran
    // anyway produced neural pixels under a label that says otherwise.
    if (result.ok && result.neural &&
        (result.verifiedNeuralFrames < result.frameCount || !result.feature18ArmedBeforeCapture ||
         !result.evidence.Valid())) return std::nullopt;
    if (result.ok && !result.neural &&
        (result.verifiedNeuralFrames || result.feature18ArmedBeforeCapture ||
         result.evidence.feature18Evaluated)) return std::nullopt;
    if (result.cancelled && (result.ok || result.failure != NeuralRenderFailure::Cancelled)) return std::nullopt;
    if (!result.ok && !result.cancelled && result.failure == NeuralRenderFailure::None) return std::nullopt;
    return result;
}

struct PreflightPayload {
    bool ok{};
    std::string json;
};

inline std::vector<std::byte> EncodePreflight(const PreflightPayload& preflight)
{
    WirePreflight wire{};
    wire.ok = preflight.ok ? 1 : 0;
    wire.jsonBytes = static_cast<uint32_t>(preflight.json.size());
    std::vector<std::byte> payload(sizeof(wire) + wire.jsonBytes);
    std::memcpy(payload.data(), &wire, sizeof(wire));
    if (wire.jsonBytes) std::memcpy(payload.data() + sizeof(wire), preflight.json.data(), wire.jsonBytes);
    return payload;
}

inline std::optional<PreflightPayload> DecodePreflight(std::span<const std::byte> payload)
{
    if (payload.size() < sizeof(WirePreflight)) return std::nullopt;
    WirePreflight wire{};
    std::memcpy(&wire, payload.data(), sizeof(wire));
    if (!IsBooleanByte(wire.ok) || wire.reserved[0] || wire.reserved[1] || wire.reserved[2] ||
        payload.size() != sizeof(WirePreflight) + wire.jsonBytes || !wire.jsonBytes) return std::nullopt;
    PreflightPayload preflight;
    preflight.ok = wire.ok != 0;
    preflight.json.assign(reinterpret_cast<const char*>(payload.data() + sizeof(WirePreflight)), wire.jsonBytes);
    if (preflight.json.find('\0') != std::string::npos) return std::nullopt;
    return preflight;
}

} // namespace neural_worker_protocol
