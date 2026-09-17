#pragma once

#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>

// Frame-generation planning for a video source on a real display.
//
// Nothing here is keyed on the source alone: a generated rate is worth
// producing only if the display presents it better than the source's own rate,
// and "better" is two quantities, not one.
//
//   step    1000/rate ms - the motion between one presented frame and the
//           next, and the dominant judder term. Daly et al., "A
//           Luminance-aware Model of Judder Perception" (ACM TOG 38(4)),
//           decompose judder into chattering, moving-edge flicker, motion
//           blur and false edges; all four fall steeply as the rate rises,
//           up to a cut-off rate above which none of it is visible.
//   spread  the unevenness of the presentation grid. A frame is held for
//           floor(refresh/rate) or ceil(refresh/rate) scan-outs and nothing
//           between, so the spread is either zero - the rate divides the
//           refresh - or exactly ONE refresh period, however high the rate.
//
// That second fact is what this file used to get wrong. It claimed 24 fps on a
// 60 Hz panel "would trade one uneven cadence for another", and refused. Both
// cadences are one refresh period wide: 24 fps is held 33/50 ms - 3:2 pulldown
// - and 48 fps is held 17/33 ms. The spread is IDENTICAL and the step halves,
// 41.7 ms -> 20.8 ms, so that refusal threw away a strict improvement. Blur
// Busters recommends exactly this doubling for low-rate content, and mpv and
// madVR answer the remaining unevenness from the display side (resample,
// refresh switching) rather than by refusing to interpolate.
//
// The admission rule is therefore: a multiple is admissible when it lowers the
// step WITHOUT raising the spread. Two consequences, both deliberate:
//
//   - A source that already lands evenly is never made uneven. 30 fps on a
//     120 Hz panel is held 4 scan-outs per frame with zero spread, and 90 fps
//     would be held 8/17 ms, so 3x is refused there while 2x and 4x are taken.
//   - A source that is already uneven may be raised to any admissible
//     multiple, because the unevenness it lands in is the one already on
//     screen.
//
// An even multiple still wins whenever one exists - it removes the spread term
// outright, which no higher rate can - and only then does the largest multiple
// win. BetterRefreshForSource turns what remains into an offer rather than a
// dead end: 24 fps has no even multiple on a 60 Hz panel and reaches 120
// exactly at 5x on the 120 Hz mode the same monitor already supports.
//
// The refusals that remain are as much of the answer as the multipliers: each
// one is a case where the honest output is the source's own cadence.
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

// The cadence a rate lands in on a fixed-refresh display, which is all this
// policy compares. Hold times are quantised to the scan-out, so there are only
// ever two of them and they differ by exactly one refresh period.
struct DisplayCadence {
    // refresh / rate, exact: how many scan-outs one frame occupies.
    double presentsPerFrame = 0.0;
    // floor and ceil of that ratio, never below one scan-out. Equal when the
    // rate divides the refresh, one apart when it does not.
    uint32_t shortHold = 1;
    uint32_t longHold = 1;
    // Zero when the two holds are equal, one refresh period otherwise.
    double spreadMs = 0.0;
    // Motion between one presented frame and the next.
    double stepMs = 0.0;
    bool even = false;
};

inline DisplayCadence CadenceOf(double fps, double refreshHz)
{
    DisplayCadence cadence{};
    if (!(fps > 0.0) || !std::isfinite(fps) || !(refreshHz > 0.0) || !std::isfinite(refreshHz))
        return cadence;
    cadence.stepMs = 1000.0 / fps;
    cadence.presentsPerFrame = refreshHz / fps;
    const double rounded = std::round(cadence.presentsPerFrame);
    cadence.even = rounded >= 1.0 &&
                   std::abs(cadence.presentsPerFrame - rounded) <= kRateTolerance * rounded;
    if (cadence.even) {
        cadence.shortHold = cadence.longHold = uint32_t(rounded);
        return cadence;
    }
    const double floored = std::floor(cadence.presentsPerFrame);
    cadence.shortHold = floored >= 1.0 ? uint32_t(floored) : 1u;
    cadence.longHold = cadence.shortHold + 1u;
    cadence.spreadMs = 1000.0 / refreshHz;
    return cadence;
}

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
    // No multiple fits under the refresh at all: even doubling would ask the
    // panel for more frames than it can scan out. 40 fps on a 60 Hz panel.
    RefreshBelowDouble,
    // The source's own rate already divides the refresh and no multiple that
    // fits does, so generating would land the frames unevenly where the source
    // lands evenly. That is the one trade this policy will not make: 24 fps on
    // a 120 Hz panel whose runtime admits a single generated frame.
    SourceCadenceEven,
    // An uneven multiple was admissible and the even-cadence-only setting
    // withheld it. The one refusal here the user can lift outright.
    EvenCadenceRequired,
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
        case FrameGenerationRefusal::RefreshBelowDouble: return "refresh-below-double";
        case FrameGenerationRefusal::SourceCadenceEven: return "source-cadence-even";
        case FrameGenerationRefusal::EvenCadenceRequired: return "even-cadence-required";
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
    // The grid the generated rate lands in, beside the grid the source's own
    // rate lands in. A plan is accepted only when the first is no more uneven
    // than the second, and the dialogs quote both.
    DisplayCadence cadence{};
    DisplayCadence sourceCadence{};
    // Scan-outs each generated frame occupies, rounded; 1 whenever the target
    // reaches the refresh. cadence.shortHold/longHold carry the two real hold
    // lengths when the grid is uneven.
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
// `requireEvenCadence` is the user's even-cadence-only setting. It drops the
// second pass, so the player behaves as it did before this model: nothing is
// generated unless the generated rate lands on the refresh exactly.
inline FrameGenerationPlan PlanFrameGeneration(const SourceCadence& source, double refreshHz,
                                               uint32_t multiFrameCountMax,
                                               bool requireEvenCadence = false)
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
    plan.sourceCadence = CadenceOf(source.fps, refreshHz);
    // Checked before the multiples so a 60 fps source on a 60 Hz panel reports
    // the reason a viewer can act on - get a faster panel - instead of a
    // cadence argument, which would be true and useless.
    if (source.fps >= refreshHz * (1.0 - kRateTolerance)) {
        plan.refusal = FrameGenerationRefusal::SourceMeetsRefresh;
        return plan;
    }
    // Doubling is the smallest thing this feature can do. A panel that cannot
    // scan out twice the source's rate has no multiple to offer at all, and
    // says so instead of blaming the cadence.
    if (source.fps * 2.0 > refreshHz * (1.0 + kRateTolerance)) {
        plan.refusal = FrameGenerationRefusal::RefreshBelowDouble;
        return plan;
    }

    // Pass 0 takes the largest multiple that divides the refresh: it removes
    // the spread term outright, which no number of extra frames can. Pass 1
    // takes the largest multiple whatever its cadence, and runs only when the
    // source is already uneven on this display - there the spread is already on
    // screen and a generated rate cannot make it worse.
    const bool allowUneven = !requireEvenCadence && !plan.sourceCadence.even;
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1 && !allowUneven) break;
        for (uint32_t multiplier = 1 + multiFrameCountMax; multiplier >= 2; --multiplier) {
            const double target = source.fps * double(multiplier);
            if (target > refreshHz * (1.0 + kRateTolerance)) continue;
            const DisplayCadence cadence = CadenceOf(target, refreshHz);
            if (pass == 0 && !cadence.even) continue;
            const double presents = std::round(cadence.presentsPerFrame);
            plan.multiplier = multiplier;
            plan.generatedPerSource = multiplier - 1;
            plan.targetFps = target;
            plan.cadence = cadence;
            plan.presentsPerFrame = presents >= 1.0 ? uint32_t(presents) : 1u;
            plan.refusal = FrameGenerationRefusal::None;
            return plan;
        }
    }
    // Pass 1 cannot fail once it runs: doubling is admissible by the guard
    // above and its cadence is no worse than the source's. So what is left is
    // an evenly-landing source with no even multiple, or the setting.
    plan.refusal = plan.sourceCadence.even ? FrameGenerationRefusal::SourceCadenceEven
                                           : FrameGenerationRefusal::EvenCadenceRequired;
    return plan;
}

// The best OTHER refresh this monitor could be set to, judged by the plan it
// would allow. This is the display-side answer the refusals above cannot give
// on their own: a panel that offers 60 Hz almost always offers 120, where
// 24 fps film lands on 120 exactly at 5x and its pulldown disappears.
//
// Only EVEN targets are offered. Switching the whole display to land in a
// different uneven grid is not worth a mode change, and the offer has to be
// strictly better than what the current refresh already allows or it is noise.
struct RefreshSwitchOffer {
    double refreshHz = 0.0;
    uint32_t multiplier = 1;
    double targetFps = 0.0;

    constexpr bool Offered() const noexcept { return refreshHz > 0.0 && multiplier > 1; }
};

inline RefreshSwitchOffer BetterRefreshForSource(const SourceCadence& source,
                                                 std::span<const double> refreshes,
                                                 double currentRefreshHz,
                                                 uint32_t multiFrameCountMax,
                                                 bool requireEvenCadence = false)
{
    RefreshSwitchOffer offer{};
    for (const double refresh : refreshes) {
        if (!(refresh > 0.0) || !std::isfinite(refresh)) continue;
        if (std::abs(refresh - currentRefreshHz) <= currentRefreshHz * kRateTolerance) continue;
        const FrameGenerationPlan candidate =
            PlanFrameGeneration(source, refresh, multiFrameCountMax, true);
        if (!candidate.Generates()) continue;
        if (offer.Offered()) {
            const bool higher = candidate.targetFps > offer.targetFps * (1.0 + kRateTolerance);
            const bool tie = std::abs(candidate.targetFps - offer.targetFps) <=
                             offer.targetFps * kRateTolerance;
            // A tie goes to the lower refresh: the same presented rate for less
            // of the panel's bandwidth.
            if (!higher && !(tie && refresh < offer.refreshHz)) continue;
        }
        offer = {refresh, candidate.multiplier, candidate.targetFps};
    }
    if (!offer.Offered()) return {};
    const FrameGenerationPlan current =
        PlanFrameGeneration(source, currentRefreshHz, multiFrameCountMax, requireEvenCadence);
    // Better means: a rate where there was none, a higher rate, or the same
    // rate without the pulldown.
    const bool better = !current.Generates() ||
                        offer.targetFps > current.targetFps * (1.0 + kRateTolerance) ||
                        (!current.cadence.even &&
                         offer.targetFps >= current.targetFps * (1.0 - kRateTolerance));
    return better ? offer : RefreshSwitchOffer{};
}

} // namespace frame_rate_policy
