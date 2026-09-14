#pragma once

// Leaf value types of a neural render job. Kept apart from
// OfflineNeuralRenderer.h so consumers that only describe a job (range
// selection, runtime policy, the cache manifest) do not inherit <windows.h>,
// the encoder pipeline, and the orchestrator class along with it.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

enum class NeuralRenderPhase {
    CheckingCache,
    Acquiring,
    Decoding,
    NeuralRendering,
    Encoding,
    Validating,
    Ready,
    Preflight,
    Paused,
    Recovering,
};

// Explicit failure classification. Every failed job names exactly one kind so
// the player can show a distinct state instead of a generic failure and so
// receipts record what actually stopped the render.
enum class NeuralRenderFailure : uint8_t {
    None,
    Source,
    Encoder,
    Neural,
    GpuStall,
    DeviceRemoved,
    WorkerCrashed,
    RetryExhausted,
    Cancelled,
    Preflight,
    Identity,
    Protocol,
};

constexpr std::string_view NeuralRenderFailureName(NeuralRenderFailure failure) noexcept
{
    switch (failure) {
        case NeuralRenderFailure::None: return "none";
        case NeuralRenderFailure::Source: return "source";
        case NeuralRenderFailure::Encoder: return "encoder";
        case NeuralRenderFailure::Neural: return "neural";
        case NeuralRenderFailure::GpuStall: return "gpu-stall";
        case NeuralRenderFailure::DeviceRemoved: return "device-removed";
        case NeuralRenderFailure::WorkerCrashed: return "worker-crashed";
        case NeuralRenderFailure::RetryExhausted: return "retry-exhausted";
        case NeuralRenderFailure::Cancelled: return "cancelled";
        case NeuralRenderFailure::Preflight: return "preflight";
        case NeuralRenderFailure::Identity: return "identity";
        case NeuralRenderFailure::Protocol: return "protocol";
    }
    return "unknown";
}

// Half-open source interval [start, end) on the decoder's CFR timeline. Both
// zero means the whole source. The encoded output always starts at pts 0; the
// absolute start is recorded beside it so playback can realign.
struct NeuralRenderRange {
    int64_t start100ns{};
    int64_t end100ns{};

    friend bool operator==(const NeuralRenderRange&, const NeuralRenderRange&) = default;
    constexpr bool Whole() const noexcept { return start100ns == 0 && end100ns == 0; }
};

// Frames evaluated (never captured) before the first captured frame so the
// temporal history at the range start matches a continuous render.
inline constexpr uint32_t kDefaultPrerollFrames = 60;
// Exact-frame retries of the same frame before the job is classified as
// retry-exhausted. Retries never skip a frame.
inline constexpr uint32_t kDefaultFrameRetryLimit = 3;

struct NeuralRenderTiming {
    uint64_t samples{};
    double neuralGpuMsP50{};
    double neuralGpuMsP95{};
    double neuralGpuMsMax{};
    double guideMsMean{};
    double captureMsMean{};
    uint64_t peakLocalVramMiB{};

    friend bool operator==(const NeuralRenderTiming&, const NeuralRenderTiming&) = default;
};

// A DLAA-only run is indistinguishable from a healthy one in every counter the
// evidence chain checks: feature 18 is created, it is evaluated, frames are
// produced and verified. What it cannot fake is GPU cost. Measured at 1920x1080
// on this project's RTX 5090 / driver 616.64: the worker that stopped
// presenting after a feature recreate reported a median of 0.46 ms per frame
// (docs/DLSS5_VIDEO_ROADMAP.md item 1 open gap), while healthy renders on the
// same machine and geometry report 3.683776, 3.696608 and 3.715776 ms
// (the three cache/v1 receipt.json files, 2534 to 2658 samples each) and the
// reference benchmark run reads 3.26 to 3.27 ms (docs/BENCHMARK.md reference
// table), the lowest healthy median in evidence. Those two ends are a factor of
// 7.1 apart, so the floor is their geometric midpoint, 1.223 ms at 1920x1080:
// 2.66x above the DLAA-only observation, 2.66x below the benchmark median and
// 3.01x below the lowest receipt median. Per output megapixel because the pass
// scales with pixels, and the remaining risk is one-sided in the safe
// direction: per-pixel neural cost only rises on slower hardware, so a fixed
// floor can misjudge a healthy run only on a GPU substantially faster than a
// 5090, while the failure it catches sits 7x below it on the fastest card that
// exists today.
inline constexpr double kNeuralGpuMsFloorPerMegapixel = 0.59;

inline double NeuralGpuMsFloor(uint32_t width, uint32_t height)
{
    return kNeuralGpuMsFloorPerMegapixel * (double(width) * double(height) / 1000000.0);
}

// No samples at all means the build carries no timing instrumentation (such
// builds report the ms fields as zero), so a missing measurement is never a
// verdict against the run.
inline bool NeuralTimingClearsFloor(const NeuralRenderTiming& timing, uint32_t width, uint32_t height)
{
    return timing.samples == 0 || timing.neuralGpuMsP50 >= NeuralGpuMsFloor(width, height);
}

struct NeuralRuntimeEvidence {
    bool upscalingOff{};
    bool inlineInterceptionContract{};
    bool feature18Created{};
    bool feature18Evaluated{};
    bool laterFailure{};
    uint64_t highestObservedEvaluation{};

    bool Valid() const noexcept
    {
        return upscalingOff && inlineInterceptionContract && feature18Created && feature18Evaluated &&
               highestObservedEvaluation > 0 && !laterFailure;
    }
};

// One finalized output file produced while the job is still running. Each file
// starts at its own pts zero; firstTimestamp100ns places it on the source
// timeline and end100ns is exclusive.
struct NeuralRenderSegment {
    uint64_t index{};
    uint64_t firstFrameNumber{};
    int64_t firstTimestamp100ns{};
    int64_t end100ns{};
    uint64_t frameCount{};
    std::wstring fileName;          // relative to the staging directory
};

// Consumer of finalized segments. onSegment is called from the job's finalize
// thread, in index order, only after that file's encoder exited successfully.
struct NeuralSegmentSink {
    std::function<void(const NeuralRenderSegment&)> onSegment;
    std::function<void()> onRestart; // a from-zero relaunch invalidated every earlier segment
};
