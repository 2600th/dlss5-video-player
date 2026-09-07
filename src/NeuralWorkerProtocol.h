#pragma once

// Versioned metadata pipe between the hook-free player and the isolated
// neural helper. Only progress, preflight and final results cross the pipe;
// encoded video stays in the cache. This header is the single definition
// shared by the parent launcher, the helper executable and the tests.

#include "OfflineNeuralRenderer.h"

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace neural_worker_protocol {

inline constexpr uint32_t kMagic = 0x3152574Eu; // NWR1
inline constexpr uint16_t kVersion = 2;
inline constexpr uint32_t kMaximumPayloadBytes = 64 * 1024;
inline constexpr uint32_t kMaximumDetailBytes = 4 * 1024;

enum class WireKind : uint16_t { Progress = 1, Result = 2, Preflight = 3 };

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
    uint32_t detailBytes;
};

// Followed by `jsonBytes` of UTF-8 JSON describing the probed runtime.
struct WirePreflight {
    uint8_t ok;
    uint8_t reserved[3];
    uint32_t jsonBytes;
};
#pragma pack(pop)

static_assert(sizeof(WireHeader) == 12);
static_assert(sizeof(WireProgress) == 52);
static_assert(sizeof(WireResult) == 140);
static_assert(sizeof(WirePreflight) == 8);

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
