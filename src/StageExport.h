#pragma once

#include "ExportPipeline.h"
#include "MediaPipeline.h"
#include "NeuralRenderTypes.h"
#include "NeuralSettings.h"
#include "RuntimeLock.h"
#include "UpscalingPolicy.h"
#include "VsrPolicy.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

// ---- Export with DLSS stages: the one implementation --------------------
//
// The dialog (PlayerApp::StartStageExport, Ctrl+S) and `DLSSVideoPlayer.exe
// --render` - which dlss5-convert drives - both run the passes through
// RunStageExport and say what a plan is through the formatters below, so a
// script gets the file the dialog would have written rather than a second
// implementation of it that drifts. Everything the two callers differ in -
// where progress goes, how the outcome is shown, whether a range was asked
// for - is a parameter.

// The localization key that says why PlanExport refused a selection.
const wchar_t* ExportRefusalKey(ExportRefusal refusal);

// The dialog's Result line: "1920 × 1080 at 59.94 fps · 2 passes".
std::wstring StageExportResultSummary(const ExportPlan& plan);

// --render's first line, the source beside what the plan makes of it:
// "1280 x 720 at 30 fps -> 1920 x 1080 at 60 fps, 2 passes".
std::wstring StageExportPlanSummary(const ExportPlan& plan, uint32_t sourceWidth, uint32_t sourceHeight,
                                    double sourceFps);

// --render's "encode: ..." line: the rung, and for a Super Resolution pass the
// upscaler (and DLSS's history when the model does not run).
std::wstring StageExportEncodeSummary(const ExportPlan& plan, uint32_t sourceWidth, EncoderQuality quality,
                                      UpscalingHistory history);

// ---- What refuses a neural pass before a helper is asked for one --------
//
// Shared by the live render (NeuralJobRun) and the stage export, in the words
// the live path has always used, so a runtime one of them refuses is never
// rendered with by the other. The export used to check neither: it wrote the
// settings and ran the helper against whatever the directory held, and a
// drifted runtime the live path refused still produced a file called neural.

// Empty when every locked file matches.
std::wstring RuntimeLockRefusal(std::span<const RuntimeLockCheck> checks);

// Empty when the directory holds no module the lock does not name, and when it
// cannot be listed: the helper refuses that itself, with its own reason.
std::wstring UnlockedRuntimeModulesRefusal(const std::filesystem::path& runtimeDirectory, const RuntimeLock& lock);

// Both, the lock first, which is the order a live render meets them in. Empty
// when neither refuses. A cancelled check leaves hashes unverified and so
// reads as drift; the caller asks its stop token before believing it.
std::wstring StageExportRuntimeRefusal(const std::filesystem::path& runtimeDirectory, const RuntimeLock& lock,
                                       std::stop_token stop);

// Everything a neural render writes into the add-on's [RenoDX.DLSS5]: the
// Neural settings, and the model's place against the carrier's upscale that
// the processing scale needs (PreUpscaleOverride says when that is written).
std::vector<NeuralAddonOverride> RenderAddonOverrides(const std::filesystem::path& ini,
                                                      const NeuralSettings& settings,
                                                      uint32_t processingScale);

// ---- The passes themselves -----------------------------------------------

struct StageExportJob {
    ExportPlan plan;
    std::filesystem::path source;
    std::filesystem::path destination;
    // Where the intermediate passes are written: the cache root's
    // export-stages directory, beside the other derived carriers.
    std::filesystem::path scratch;
    // The player's directory: ffmpeg beside it, the helper in neural-runtime.
    std::filesystem::path helpers;
    uint32_t sourceWidth{},sourceHeight{};
    double fps{},duration{};
    // ExportDisplayAspect of the source; empty for square pixels.
    std::wstring displayAspect;
    // FrameGenerationRequest::holdDuplicates for the frame-generation pass.
    bool holdDuplicates{};
    // Whole for the dialog. A range reaches the worker pass only; frame
    // generation then reads that pass's carrier, which covers just the range.
    NeuralRenderRange range{};
    // RTX VSR's quality for a vsrStage plan: the comparison ladder's, saved.
    vsr_policy::Quality vsrQuality{vsr_policy::kDefaultQuality};
    uint32_t nvencPreset{5};
    // The model's resolution for a neural pass at the source size. An export
    // that upscales runs the model on the upscaled frame, as it always has,
    // whatever this says: a reduced model input and a Super Resolution output
    // are one carrier's two jobs, and it can only do one of them.
    uint32_t processingScale{kDefaultProcessingScale};
    // Super Resolution's history for an upscaling pass without the model; a pass
    // that runs the model keeps Temporal (CarrierUpscalingHistory).
    UpscalingHistory upscalingHistory{kRecommendedUpscalingHistory};
    // Written to the add-on before a neural pass. The dialog's tooltip has
    // always said the neural stage "runs the neural model with the settings
    // from Neural settings", but nothing wrote them: the export used whatever
    // the last live render had left in ReShade.ini.
    NeuralSettings neuralSettings{};
    // The capture-quality switches of Encoder settings, so the export's neural pass
    // writes the same way the cache does.
    bool captureDither{true};
    EncoderQuality quality{EncoderQuality::Standard};
    bool sourceDeband{false};
    bool suppliedExposure{false};
    // Ends the player's idle resident helper before this export's helper
    // starts in the same runtime directory. Empty for `--render`, which runs in
    // a process of its own and has none.
    std::function<void()> releaseResidentHelper;
    // The lock the runtime directory is held to before any worker pass; empty
    // is the embedded one. Tests only, which have no runtime that satisfies it.
    std::optional<RuntimeLock> runtimeLock;
};

// `passKey` null is the end of the passes, when the finished file is moved
// into place.
struct StageExportUpdate {
    uint32_t pass{},passes{};
    const wchar_t* passKey{};
    uint64_t completedFrames{},totalFrames{};
};

enum class StageExportStatus { Done, Refused, Failed, Cancelled };

struct StageExportOutcome {
    StageExportStatus status{StageExportStatus::Failed};
    std::wstring detail;
};

StageExportOutcome RunStageExport(const StageExportJob& job, std::stop_token stop,
                                  const std::function<void(const StageExportUpdate&)>& progress);
