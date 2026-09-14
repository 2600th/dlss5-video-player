#pragma once

// Leaf value types of a neural render job. Kept apart from
// OfflineNeuralRenderer.h so consumers that only describe a job (range
// selection, runtime policy, the cache manifest) do not inherit <windows.h>,
// the encoder pipeline, and the orchestrator class along with it.

#include "ResidentHelperPolicy.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
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
    // Two point samples of the helper process's local-segment usage, beside
    // the running per-frame maximum above. They are what makes the idle-VRAM
    // policy decidable from a receipt instead of a debugger: the parked cost
    // of residency is the first, and what the policy gave back is the second.
    //
    // postJobLocalVramMiB: what this process still held the moment this job's
    //   render went quiet. A single-shot helper exits here, so this is only a
    //   parked figure for a resident one.
    // idleLocalVramMiB: what it still held after the idle grace that preceded
    //   THIS job, i.e. after the idle policy had acted on the previous job's
    //   memory. Zero when no idle period preceded this job - the first job a
    //   process serves, and every single-shot render.
    uint64_t postJobLocalVramMiB{};
    uint64_t idleLocalVramMiB{};
    // Which arm produced the two samples above. Fixed for a helper process, so
    // it is a property of the run rather than of the job; carried here so two
    // receipts with different idle numbers can be told apart.
    resident_helper::IdleVramPolicy idleVramPolicy{resident_helper::kDefaultIdleVramPolicy};

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

// Cold-start stack of one neural render, in the order the phases happen. Every
// boundary is named once here and measured where the transition is actually
// observable: the player owns the four it can see from outside the helper, the
// helper owns the five that only exist inside it. The handoff's cold-start
// table (CreateProcess+AV, ReShade proxy, NGX init, CreateFeature, first
// segment) maps onto HelperStart..FirstOutput; the proxy's own load is inside
// HelperStart because the proxy is the helper's dxgi import and is therefore
// resolved by the loader before the entry point runs.
enum class NeuralColdStartPhase : uint32_t {
    Request,      // player: request instant -> the decision to render or to replay a cache entry
    Preflight,    // player: the feature-18 probe helper, when one ran
    Launch,       // player: that decision -> the render helper's process created
    HelperStart,  // helper: process creation -> its own entry point (AV scan, loader, proxy)
    RuntimeReady, // helper: entry point -> add-on contract verified, media runtime and render window up
    NeuralInit,   // helper: render start -> source opened and evaluator initialized (device, NGX)
    FeatureArm,   // helper: -> feature 18 created, evaluated and inline interception armed
    FirstOutput,  // helper: -> first finalized output file published
    Attach,       // player: first playable output in hand -> first neural frame presented
};
inline constexpr size_t kNeuralColdStartPhaseCount = 9;

constexpr std::string_view NeuralColdStartPhaseName(NeuralColdStartPhase phase) noexcept
{
    switch (phase) {
        case NeuralColdStartPhase::Request: return "request";
        case NeuralColdStartPhase::Preflight: return "preflight";
        case NeuralColdStartPhase::Launch: return "launch";
        case NeuralColdStartPhase::HelperStart: return "helperStart";
        case NeuralColdStartPhase::RuntimeReady: return "runtimeReady";
        case NeuralColdStartPhase::NeuralInit: return "neuralInit";
        case NeuralColdStartPhase::FeatureArm: return "featureArm";
        case NeuralColdStartPhase::FirstOutput: return "firstOutput";
        case NeuralColdStartPhase::Attach: return "attach";
    }
    return "unknown";
}

// The phases only the helper can see. A timeline arriving over the metadata
// pipe may claim these and nothing else: the player's own measurements are not
// the helper's to report.
inline constexpr uint32_t kNeuralColdStartHelperPhases =
    (1u << static_cast<uint32_t>(NeuralColdStartPhase::HelperStart)) |
    (1u << static_cast<uint32_t>(NeuralColdStartPhase::RuntimeReady)) |
    (1u << static_cast<uint32_t>(NeuralColdStartPhase::NeuralInit)) |
    (1u << static_cast<uint32_t>(NeuralColdStartPhase::FeatureArm)) |
    (1u << static_cast<uint32_t>(NeuralColdStartPhase::FirstOutput));

// Durations, never stamps: the phases are measured in two processes against two
// unrelated monotonic origins, and only their lengths mean anything on another
// machine. A phase with no value did not happen - a reused preflight verdict, a
// single-file render that publishes no segment, a run that stopped before it got
// there - which is a different claim from a phase that took no measurable time,
// so the two are never collapsed into one. Same rule as NeuralTimingClearsFloor:
// a missing measurement is not a verdict.
class NeuralColdStartTimeline {
public:
    using Duration = std::chrono::microseconds;

    void Record(NeuralColdStartPhase phase, Duration elapsed) { phases_[Index(phase)] = elapsed; }
    std::optional<Duration> Phase(NeuralColdStartPhase phase) const { return phases_[Index(phase)]; }
    // Request instant to first neural frame presented: the number the toggle-to-picture
    // acceptance criterion is written against, measured end to end rather than added up.
    void RecordTotal(Duration elapsed) { total_ = elapsed; }
    std::optional<Duration> Total() const { return total_; }
    bool Empty() const
    {
        if (total_) return false;
        for (const auto& phase : phases_) if (phase) return false;
        return true;
    }
    // Adopts the phases `other` measured and leaves the rest alone, so the two
    // halves of one timeline join without either erasing the other's absences.
    void Merge(const NeuralColdStartTimeline& other)
    {
        for (size_t index = 0; index < phases_.size(); ++index)
            if (other.phases_[index]) phases_[index] = other.phases_[index];
        if (other.total_) total_ = other.total_;
    }

    friend bool operator==(const NeuralColdStartTimeline&, const NeuralColdStartTimeline&) = default;

private:
    static constexpr size_t Index(NeuralColdStartPhase phase) noexcept
    {
        return static_cast<size_t>(phase);
    }

    std::array<std::optional<Duration>, kNeuralColdStartPhaseCount> phases_{};
    std::optional<Duration> total_{};
};

// Reports the helper's share of the timeline once, as soon as the helper has
// produced something the player can show or has stopped trying.
using NeuralColdStartCallback = std::function<void(const NeuralColdStartTimeline&)>;

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
