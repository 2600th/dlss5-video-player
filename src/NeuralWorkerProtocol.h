#pragma once

// Versioned metadata pipe between the hook-free player and the isolated
// neural helper. Only progress, finalized-segment announcements, the
// cold-start timeline, preflight and final results cross the pipe; encoded
// video stays in the cache. This header is the single definition shared by the
// parent launcher, the helper executable and the tests.

#include "OfflineNeuralRenderer.h"

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neural_worker_protocol {

inline constexpr uint32_t kMagic = 0x3152574Eu; // NWR1
// 5 since the helper reports its share of the cold-start timeline as its own
// message kind. The parent rejects any header whose version is not exactly
// this, so a helper left over from an older build in neural-runtime/ fails
// closed instead of having a v4 stream read as a v5 one - and a v4 parent,
// which would see this kind as malformed metadata and refuse the whole render,
// never has to.
inline constexpr uint16_t kVersion = 5;
inline constexpr uint32_t kMaximumPayloadBytes = 64 * 1024;
inline constexpr uint32_t kMaximumDetailBytes = 4 * 1024;
// A segment name is a bare file name joined to the staging directory by the
// parent, never a path.
inline constexpr uint32_t kMaximumSegmentNameBytes = 512;

enum class WireKind : uint16_t { Progress = 1, Result = 2, Preflight = 3, Segment = 4, Timeline = 5 };

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
    uint8_t upscalingOff;
    uint8_t inlineInterceptionContract;
    uint8_t feature18Created;
    uint8_t feature18Evaluated;
    uint8_t laterFailure;
    uint8_t failure;
    uint8_t reserved[6];
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
#pragma pack(pop)

static_assert(sizeof(WireHeader) == 12);
static_assert(sizeof(WireProgress) == 52);
static_assert(sizeof(WireResult) == 152);
static_assert(sizeof(WirePreflight) == 8);
static_assert(sizeof(WireSegment) == 44);
static_assert(sizeof(WireTimeline) == 80);

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

inline bool WriteMessage(HANDLE handle, WireKind kind, const void* payload, uint32_t payloadBytes)
{
    const WireHeader header{kMagic, kVersion, static_cast<uint16_t>(kind), payloadBytes};
    return WriteAll(handle, &header, sizeof(header)) &&
           (!payloadBytes || WriteAll(handle, payload, payloadBytes));
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
    wire.upscalingOff = result.evidence.upscalingOff ? 1 : 0;
    wire.inlineInterceptionContract = result.evidence.inlineInterceptionContract ? 1 : 0;
    wire.feature18Created = result.evidence.feature18Created ? 1 : 0;
    wire.feature18Evaluated = result.evidence.feature18Evaluated ? 1 : 0;
    wire.laterFailure = result.evidence.laterFailure ? 1 : 0;
    wire.failure = static_cast<uint8_t>(result.failure);
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
        !IsBooleanByte(wire.feature18ArmedBeforeCapture) || !IsBooleanByte(wire.upscalingOff) ||
        !IsBooleanByte(wire.inlineInterceptionContract) || !IsBooleanByte(wire.feature18Created) ||
        !IsBooleanByte(wire.feature18Evaluated) || !IsBooleanByte(wire.laterFailure) ||
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
    result.evidence = {wire.upscalingOff != 0, wire.inlineInterceptionContract != 0,
        wire.feature18Created != 0, wire.feature18Evaluated != 0, wire.laterFailure != 0,
        wire.highestObservedEvaluation};
    result.failure = static_cast<NeuralRenderFailure>(wire.failure);
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
                      !result.nativeEvaluations || result.verifiedNeuralFrames < result.frameCount ||
                      !result.feature18ArmedBeforeCapture || !result.evidence.Valid() ||
                      result.failure != NeuralRenderFailure::None)) return std::nullopt;
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
