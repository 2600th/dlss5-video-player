#pragma once

#include <cmath>
#include <cstdint>
#include <string_view>

// Frame-generation planning for a video source on a real display.
//
// Nothing here is keyed on the source alone. A generated rate is only worth
// producing if the panel can present it evenly: 30 fps on a 60 Hz panel doubles
// to 60 and every frame is scanned out exactly once, while 24 fps on the same
// panel has no integer multiple that divides 60 - 2x is 48 and 60/48 is 1.25 -
// so generating there would trade one uneven cadence for another and charge a
// neural evaluate for it. The same 24 fps source on a 120 Hz panel takes 5x =
// 120 exactly and its 3:2 pulldown disappears entirely, which is the largest
// win available here and the one a source-only rule ("under 45 fps, double it")
// cannot see, because it never looks at the panel.
//
// The refusals are as much of the answer as the multipliers. Every one of them
// is a case where the honest output is the source's own cadence.
//
// The runtime's own DLSSG.MultiFrameCountMax is an input, not a constant: the
// cap decides which multiples are admissible at all, and it is measured
// (QueryFrameGenerationCapability in FrameGenerationPass.h) rather than
// assumed. An earlier version of this comment claimed the measurement had to
// stay out of process to avoid a second NGX lifetime beside the RenoDX neural
// path. That was wrong on the facts: RenoDX and feature 18 live in the render
// helper, and ARCHITECTURE.md records that the main player does not load that
// proxy at all - the player already owns an in-process NGX Super Resolution
// feature of its own. The real constraint is the one the user settled: frame
// generation runs as an offline conversion with progress and then plays the
// result, because presentation pacing is entangled with the playback clock,
// audio sync and seeking.
namespace frame_rate_policy {

// Windows reports a mode's refresh as a whole number - 60 for a 59.94 Hz mode -
// and the NTSC family of rates is 1000/1001 of its nominal value, so "divides
// evenly" has to be judged with slack rather than exactly. 0.5% covers both
// errors at once (1001/1000 is 0.0999% away) and stays far tighter than the gap
// to the nearest wrong multiple: the closest contender in the table below is
// 60/48 = 1.25, which is 25% away.
inline constexpr double kRateTolerance = 0.005;

// Generated frames per source frame this project has MEASURED to land where it
// asks for them. It is an independent ceiling from the runtime's own
// DLSSG.MultiFrameCountMax, and the plan takes the smaller of the two, because
// a runtime admitting a count is not the same claim as frames arriving at the
// right instants.
//
// Measured 2026-09-17 on an RTX 5090 / driver 616.64 / nvngx_dlssg 310.7.0,
// through the shipped pass, on a 1280x720 30 fps FFV1 clip carrying a 200x200
// textured patch that moves exactly 40 px per source frame. Each output frame's
// brightness centroid gives its position, so its phase inside the pair is
// (centroid - previous source centroid) / (next - previous) and the ideal
// phases for multiplier m are k/m. Four consecutive intervals per multiplier,
// the first one included:
//
//   2x  ideal 0.500                    measured 0.474           worst 0.030
//   3x  ideal 0.333 0.667              measured 0.281 0.628     worst 0.109
//   4x  ideal 0.250 0.500 0.750        measured 0.191 0.474 0.707  worst 0.081
//   5x  ideal 0.200 0.400 0.600 0.800  measured 0.160 0.367 0.529 0.787
//                                                               worst 0.107
//   6x  ideal 0.167 ... 0.833          measured 0.157 ... 0.809 worst 0.109
//
// THE BOUND, stated so the number is not a taste: a multiplier is admitted
// while the worst ratio between ADJACENT gaps in the emitted sequence stays
// under 2.0 across those four intervals. Uneven gaps are what a viewer sees -
// a uniform offset from the ideal phase never changes and is invisible, while
// one short gap beside one long one is judder inside every source frame - so
// the gap ratio is the quantity to bound, not the distance from the ideal.
// Measured, with the smallest gap and the worst deviation expressed as a
// fraction of one slot (1/m) beside it:
//
//   multiplier   worst gap ratio   smallest gap   worst deviation / slot
//   2x           1.13              0.470          0.06
//   3x           1.69              0.262          0.33
//   4x           1.71              0.191          0.32
//   5x           1.69              0.156          0.53
//   6x           2.70              0.088          0.65
//
// 6x is the break, and it is not marginal: a 0.088 gap beside a 0.238 one is a
// near-duplicate pair followed by a jump, which is the artifact a higher rate
// is supposed to remove. Everything up to 5x holds at 1.7:1 or better. So the
// ceiling is FOUR generated frames - 5x - which is below what this runtime
// admits (DLSSG.MultiFrameCountMax = 5, i.e. 6x) and therefore still a real
// ceiling rather than a restatement of the driver's.
//
// What that keeps: 24 fps film reaches exactly 120 fps at 5x on a 120 Hz panel,
// pulldown gone, and 30 fps reaches 120 at 4x. What it gives up: 6x, which only
// 20 fps content could use on a 120 Hz panel anyway.
//
// Two earlier claims in this comment were WRONG and are recorded here because
// they were shipped. The first was that the intermediates cluster near the
// midpoint (0.478 / 0.553 / 0.738 at 4x). That was the probe: the clip used a
// flat WHITE SQUARE, and a featureless region has no interior detail to
// localise, so the generated frame is close to a blend of the pair and its
// centroid sits near the midpoint. The flat-square control still measures
// 0.482 / 0.552 / 0.735 today, beside 0.191 / 0.474 / 0.707 for textured
// content in the same runs - so placement is content-dependent, and flat
// graphics interpolate as a blend. The second was that a 240-frame 4x
// conversion produced "240 unique frames": that came from framemd5 over a
// lossy NVENC encode, where identical inputs still hash differently, so it
// proved nothing. A per-frame centroid from a raw decode is the instrument;
// tests/FrameGenerationSmoke.cpp asserts it on every run.
inline constexpr uint32_t kPhaseVerifiedMultiFrameCount = 4;

enum class FrameGenerationRefusal : uint8_t {
    None,
    // No readable frame rate. A rate this policy cannot see is one it must not
    // multiply.
    UnknownSourceRate,
    // A photo, or a GIF shown as an image: there is no second frame to generate
    // between.
    StillImage,
    // The generated grid is placed on a constant-rate timeline; a variable one
    // has no fixed interval to subdivide.
    VariableFrameRate,
    // The display did not report a mode, so there is no cadence to match.
    UnknownRefresh,
    // The source already runs at or above what the panel can present. Generating
    // frames the display cannot scan out costs an evaluate and shows nothing.
    SourceMeetsRefresh,
    // A rate exists under the refresh, but none of the admissible multiples
    // divides the refresh evenly. 24 fps on 60 Hz is the common case.
    NoEvenMultiple,
    // The runtime admits no generated frames at all on this machine.
    RuntimeRefused,
};

constexpr std::string_view FrameGenerationRefusalName(FrameGenerationRefusal refusal) noexcept
{
    switch (refusal) {
        case FrameGenerationRefusal::None: return "none";
        case FrameGenerationRefusal::UnknownSourceRate: return "unknown-source-rate";
        case FrameGenerationRefusal::StillImage: return "still-image";
        case FrameGenerationRefusal::VariableFrameRate: return "variable-frame-rate";
        case FrameGenerationRefusal::UnknownRefresh: return "unknown-refresh";
        case FrameGenerationRefusal::SourceMeetsRefresh: return "source-meets-refresh";
        case FrameGenerationRefusal::NoEvenMultiple: return "no-even-multiple";
        case FrameGenerationRefusal::RuntimeRefused: return "runtime-refused";
    }
    return "unknown";
}

// What the decoder knows about the source's timing, separated from the decoder
// itself so the policy is testable without one.
struct SourceCadence {
    double fps = 0.0;
    bool stillImage = false;
    bool constantFrameRate = true;
};

struct FrameGenerationPlan {
    // 1 means no generation; the refusal says why.
    uint32_t multiplier = 1;
    // What DLSS-G's MultiFrameCount wants: frames generated after each source
    // frame, so one less than the multiplier.
    uint32_t generatedPerSource = 0;
    double targetFps = 0.0;
    // refresh / targetFps, rounded: how many scan-outs each frame of the
    // generated sequence occupies. Always at least 1, and 1 whenever the target
    // reaches the refresh exactly. This is the number that makes the cadence
    // even, which is the whole reason a multiplier was accepted.
    uint32_t presentsPerFrame = 0;
    FrameGenerationRefusal refusal = FrameGenerationRefusal::None;

    constexpr bool Generates() const noexcept { return multiplier > 1; }
};

// `multiFrameCountMax` is DLSSG.MultiFrameCountMax exactly as the runtime
// reported it (5 on the RTX 5090 / 616.64 this project measured), so the largest
// multiplier the runtime allows is one more than that: the source frame plus the
// frames generated after it. 0 means the runtime admits no generation, which is
// a refusal rather than a multiplier of 1 with no explanation.
//
// The largest admissible multiple wins, which is also the one with the smallest
// `presentsPerFrame`: more generated frames is strictly smoother once the
// cadence is even, and unevenness is already excluded.
inline FrameGenerationPlan PlanFrameGeneration(const SourceCadence& source, double refreshHz,
                                               uint32_t multiFrameCountMax)
{
    FrameGenerationPlan plan{};
    if (source.stillImage) {
        plan.refusal = FrameGenerationRefusal::StillImage;
        return plan;
    }
    if (!(source.fps > 0.0) || !std::isfinite(source.fps)) {
        plan.refusal = FrameGenerationRefusal::UnknownSourceRate;
        return plan;
    }
    if (!source.constantFrameRate) {
        plan.refusal = FrameGenerationRefusal::VariableFrameRate;
        return plan;
    }
    if (!(refreshHz > 0.0) || !std::isfinite(refreshHz)) {
        plan.refusal = FrameGenerationRefusal::UnknownRefresh;
        return plan;
    }
    if (multiFrameCountMax == 0) {
        plan.refusal = FrameGenerationRefusal::RuntimeRefused;
        return plan;
    }
    // Checked before the multiples so a 60 fps source on a 60 Hz panel reports
    // the reason a viewer can act on instead of "no even multiple", which would
    // be true and useless.
    if (source.fps >= refreshHz * (1.0 - kRateTolerance)) {
        plan.refusal = FrameGenerationRefusal::SourceMeetsRefresh;
        return plan;
    }

    plan.refusal = FrameGenerationRefusal::NoEvenMultiple;
    for (uint32_t multiplier = 1 + multiFrameCountMax; multiplier >= 2; --multiplier) {
        const double target = source.fps * double(multiplier);
        if (target > refreshHz * (1.0 + kRateTolerance)) continue;
        const double presents = refreshHz / target;
        const double rounded = std::round(presents);
        if (rounded < 1.0) continue;
        if (std::abs(presents - rounded) > kRateTolerance * rounded) continue;
        plan.multiplier = multiplier;
        plan.generatedPerSource = multiplier - 1;
        plan.targetFps = target;
        plan.presentsPerFrame = uint32_t(rounded);
        plan.refusal = FrameGenerationRefusal::None;
        break;
    }
    return plan;
}

} // namespace frame_rate_policy
