#pragma once

#include "UpscalingPolicy.h"

#include <cstdint>
#include <string>

// What an export of this player's three neural stages actually does, decided
// before any of it runs.
//
// The stages are DLSS Super Resolution, neural rendering and frame generation,
// and the order is NVIDIA's rather than this project's. Their DLSS 5 neural
// rendering "normally runs last, on the fully upscaled frame"; the community
// Neural Upstream mod moves it earlier precisely because that is faster, which
// makes early the deviation and late the reference. Streamline's guides put
// Super Resolution "before all other post-processing" and hand DLSS-G the final
// post-processed buffer. So: Upscale -> Neural -> FrameGen, always, and the
// dialog offers no way to reorder it - an order the user can get wrong is a
// support case, and there is no reading of the evidence where a different one
// is better for an export.
//
// Super Resolution and neural rendering are ONE job, not two. The offline
// renderer takes a source size and an output size separately, and RenoDX's
// NRPreUpscale defaults to 0 - neural after the upscale - so a single pass with
// a larger output size already runs the two in the reference order. Frame
// generation is a second job because it is a different pass over a finished
// file.
//
// Pure: no files, no GPU, no clock. Everything here is decided from the
// source's geometry and what the runtime admitted, so the refusals can be
// tested without either.

struct ExportSelection {
    bool upscale{};
    bool neural{};
    bool frameGeneration{};
    // Output rung, one of kUpscaleRungHeights. Read only when `upscale`.
    uint32_t targetHeight{1440};
    // Output frames per source frame. Read only when `frameGeneration`. 2 is
    // the only value an Ada card admits; Blackwell goes further.
    uint32_t multiplier{2};
};

enum class ExportRefusal {
    None,
    NothingSelected,
    SourceGeometryUnknown,
    AlreadyAtTarget,
    MultiplierUnsupported,
    StillImage,
    // Super Resolution without the neural pass. The offline renderer cannot
    // currently produce it: the helper enables the RenoDX add-on for every job
    // it runs (NeuralWorkerMain, ConfigureNeuralAddon(..., true)) and the
    // pre-capture arming check demands feature 18 regardless, so the neural
    // model runs whether or not the job asked for it. `requireNeural` only
    // skips the four verdicts AFTER the render - it never stopped the render
    // being neural.
    //
    // Measured, not assumed: an upscale-only and an upscale-plus-neural export
    // of the same clip came out byte-identical at 9,548,373 bytes. Offering the
    // combination would put a checkbox on screen that changes nothing, which is
    // worse than not offering it. Refused until the helper can be told to run
    // its carrier without the add-on.
    UpscaleNeedsNeural,
};

struct ExportPlan {
    bool valid{};
    ExportRefusal refusal{ExportRefusal::None};
    // Stage one: the neural worker, which carries Super Resolution, the neural
    // pass, or both. False when only frame generation was asked for.
    bool workerStage{};
    // Capture size for that stage. Equal to the source size when the export
    // does not upscale; the worker reads 0 as "source size" but this is stated
    // explicitly so a caller can show the user the number.
    uint32_t outputWidth{};
    uint32_t outputHeight{};
    // False runs Super Resolution with no neural pass, which also drops the
    // feature-18 verdicts a neural render is held to.
    bool requireNeural{};
    // Stage two.
    bool frameGenStage{};
    uint32_t multiplier{};
    // Frames per second of the finished file, for the dialog's summary line.
    double outputFps{};
};

// `maxMultiplier` is 1 + DLSSG.MultiFrameCountMax, or 0 when the runtime admits
// no generation at all. `stillImage` refuses frame generation outright: a photo
// has no successor frame to interpolate toward.
inline ExportPlan PlanExport(const ExportSelection& selection, uint32_t sourceWidth,
                             uint32_t sourceHeight, double sourceFps, uint32_t maxMultiplier,
                             bool stillImage)
{
    ExportPlan plan;
    const auto refuse = [&](ExportRefusal reason) { plan = {}; plan.refusal = reason; return plan; };

    if (!selection.upscale && !selection.neural && !selection.frameGeneration)
        return refuse(ExportRefusal::NothingSelected);
    if (!sourceWidth || !sourceHeight) return refuse(ExportRefusal::SourceGeometryUnknown);
    if (selection.frameGeneration && stillImage) return refuse(ExportRefusal::StillImage);

    plan.outputWidth = sourceWidth;
    plan.outputHeight = sourceHeight;
    plan.outputFps = sourceFps;

    if (selection.upscale) {
        const UpscalingSize target = UpscalingTarget(sourceWidth, sourceHeight, selection.targetHeight);
        // A rung at or below the source is not an upscale, and DLSS refuses a
        // non-growing output. Said here so the dialog can grey the rung out
        // rather than let the render fail minutes later.
        if (!target.grows) return refuse(ExportRefusal::AlreadyAtTarget);
        plan.outputWidth = target.width;
        plan.outputHeight = target.height;
    }
    // The worker runs whenever either of the first two stages was asked for.
    if (selection.upscale && !selection.neural) return refuse(ExportRefusal::UpscaleNeedsNeural);
    plan.workerStage = selection.upscale || selection.neural;
    plan.requireNeural = selection.neural;

    if (selection.frameGeneration) {
        // 2 is the floor: one generated frame per source pair. A runtime that
        // admits none, or fewer than asked, refuses rather than silently
        // delivering a different frame rate than the dialog promised.
        if (selection.multiplier < 2 || maxMultiplier < 2 || selection.multiplier > maxMultiplier)
            return refuse(ExportRefusal::MultiplierUnsupported);
        plan.frameGenStage = true;
        plan.multiplier = selection.multiplier;
        plan.outputFps = sourceFps * selection.multiplier;
    }

    plan.valid = true;
    return plan;
}

// How many separate passes the plan runs, which is what a progress bar divides
// by and what the dialog's estimate is built from.
inline uint32_t ExportStageCount(const ExportPlan& plan)
{
    return (plan.workerStage ? 1u : 0u) + (plan.frameGenStage ? 1u : 0u);
}
