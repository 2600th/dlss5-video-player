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

// Generated frames per source frame this project has measured to land where it
// asks for them, which is not the same number as the one the runtime admits.
//
// The runtime reports DLSSG.MultiFrameCountMax = 5 on an RTX 5090 / driver
// 616.64, so 6x is admissible - and 4x does produce four distinct, correctly
// ordered frames per interval; that much is measured through the shipped pass
// on a synthetic clip whose box moves exactly 40 px per source frame, and every
// output frame of a 240-frame 4x conversion was unique.
//
// Where they land is the problem. On that same clip the three intermediates of
// one source interval measured at 0.478, 0.553 and 0.738 of the way across it,
// against the 0.250 / 0.500 / 0.750 the timeline places them at. The motion is
// therefore delivered as roughly 48% / 7% / 19% / 26% of the interval instead
// of four equal quarters, which is micro-judder inside every source frame
// rather than the smoother motion the higher rate promises. A second interval
// on the same clip measured 0.390 / 0.490 / 0.750, so it is the shape of the
// placement and not one bad pair.
//
// One generated frame has no such failure mode: it is a single midpoint, and it
// measured 6.6% late on a 200 px displacement - a uniform offset, which is
// invisible because it never changes. So generation is capped here until the
// multi-frame placement is understood, and the cap is a measurement rather than
// a preference. Raising it is a matter of measuring that phases land where they
// are asked for, with the same synthetic clip.
inline constexpr uint32_t kPhaseVerifiedMultiFrameCount = 1;

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
