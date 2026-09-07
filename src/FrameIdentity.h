#pragma once

#include <cstdint>
#include <string_view>

// Why a frame's temporal history was (or must be) discarded. Every stage that
// declares a reset names the reason so logs, receipts and tests can prove which
// event produced it rather than inferring it from a bare boolean.
enum class HistoryReset : uint8_t {
    None,
    FirstFrame,
    Seek,
    Cut,
    Drop,
    Retry,
    SourceChange,
    WorkerRestart,
    FeatureRecreate,
    Preroll,
};

constexpr std::string_view HistoryResetName(HistoryReset reset) noexcept
{
    switch (reset) {
        case HistoryReset::None: return "none";
        case HistoryReset::FirstFrame: return "first-frame";
        case HistoryReset::Seek: return "seek";
        case HistoryReset::Cut: return "cut";
        case HistoryReset::Drop: return "drop";
        case HistoryReset::Retry: return "retry";
        case HistoryReset::SourceChange: return "source-change";
        case HistoryReset::WorkerRestart: return "worker-restart";
        case HistoryReset::FeatureRecreate: return "feature-recreate";
        case HistoryReset::Preroll: return "preroll";
    }
    return "unknown";
}

// Identity attached to every decoded frame, generated guide and neural result.
// `frameNumber`/`pts100ns`/`sourceGeneration`/`jobId` identify the source
// sample; `historyGeneration`/`reset` identify the temporal history it belongs
// to. Consumers reject a guide or captured output whose source identity does
// not match the frame they were asked to process.
struct FrameIdentity {
    uint64_t frameNumber{};       // 0-based, monotonic per decoder open
    int64_t pts100ns{};
    uint32_t sourceGeneration{};  // bumped by open, seek and decoder restarts
    uint32_t historyGeneration{}; // bumped by whoever declares a reset
    uint64_t jobId{};             // 0 for live playback
    HistoryReset reset{HistoryReset::None};

    friend bool operator==(const FrameIdentity&, const FrameIdentity&) = default;

    // True when both identities name the same source sample of the same job,
    // regardless of which temporal history they were evaluated in.
    constexpr bool SameSource(const FrameIdentity& other) const noexcept
    {
        return frameNumber == other.frameNumber && pts100ns == other.pts100ns &&
               sourceGeneration == other.sourceGeneration && jobId == other.jobId;
    }
};
