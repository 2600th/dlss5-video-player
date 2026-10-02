#pragma once

#include "UpscalingPolicy.h"

#include <cmath>
#include <cstdint>
#include <cwctype>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    // Super Resolution's engine when the model does not run (UpscalingPolicy.h).
    SuperResolutionEngine engine{kRecommendedSuperResolutionEngine};
};

enum class ExportRefusal {
    None,
    NothingSelected,
    SourceGeometryUnknown,
    AlreadyAtTarget,
    MultiplierUnsupported,
    StillImage,
    // RTX VSR was chosen for a Super Resolution-only export where it cannot run:
    // a build without the RTX Video SDK, nvngx_vsr.dll missing, or no RTX GPU.
    VsrUnavailable,
    // Super Resolution without the neural pass used to be refused here: the
    // helper enabled the add-on for every job, so an upscale-only and an
    // upscale-plus-neural export of one clip came out byte-identical at
    // 9,548,373 bytes. The helper now starts a Super Resolution-only job with
    // the add-on disabled (NeuralWorkerMain), and ExportMatrixSmoke asserts the
    // two files differ, so the combination is offered.
};

struct ExportPlan {
    bool valid{};
    ExportRefusal refusal{ExportRefusal::None};
    // Stage one: the neural worker, which carries DLSS Super Resolution, the
    // neural pass, or both. False when only frame generation was asked for, or
    // when RTX VSR does the upscale.
    bool workerStage{};
    // Stage one instead: RTX VSR's upscale, in the player (VsrUpscalePass).
    bool vsrStage{};
    // Capture size for that stage. Equal to the source size when the export
    // does not upscale; the worker reads 0 as "source size" but this is stated
    // explicitly so a caller can show the user the number.
    uint32_t outputWidth{};
    uint32_t outputHeight{};
    // False runs Super Resolution with no neural pass: the helper starts with
    // the add-on disabled and drops the feature-18 verdicts a neural render is
    // held to.
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
// `vsrReady` is whether RTX VSR can run here at all, as far as can be known
// without a device: the build has it, its DLL is beside the player, the GPU is RTX.
inline ExportPlan PlanExport(const ExportSelection& selection, uint32_t sourceWidth,
                             uint32_t sourceHeight, double sourceFps, uint32_t maxMultiplier,
                             bool stillImage, bool vsrReady = true)
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
    // RTX VSR takes the upscale when the model does not run; the worker runs for
    // everything else in the first two stages.
    plan.vsrStage = selection.upscale && !selection.neural && selection.engine == SuperResolutionEngine::RtxVsr;
    if (plan.vsrStage && !vsrReady) return refuse(ExportRefusal::VsrUnavailable);
    plan.workerStage = (selection.upscale && !plan.vsrStage) || selection.neural;
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
    return (plan.workerStage || plan.vsrStage ? 1u : 0u) + (plan.frameGenStage ? 1u : 0u);
}

// ---- The file the export writes -----------------------------------------
//
// The Save dialog used to offer MKV, MP4, GIF, PNG and JPEG for every source
// and then rename the last pass's Matroska carrier onto whatever name came
// back, so "clip.mp4" was a Matroska file with an .mp4 name: ffprobe read
// format_name=matroska,webm, and a player that trusts the extension refuses
// it. The container now follows the extension, and only the containers the
// stage output can honestly be for this source are offered:
//
//   video      MKV (default), MP4
//   animation  GIF (default), MP4, MKV
//   photo      PNG (default), JPEG
//
// GIF is not offered for a video: a 1440p frame-generated export squeezed into
// a 256-colour palette at 50 fps is not what anyone picking these stages wants,
// and "Save converted video" still writes one. PNG and JPEG are one frame, so
// they are offered only where the source is one frame. MKV and MP4 keep the
// video bitstream exactly as the passes encoded it; only the container changes.
enum class ExportContainer { Matroska, Mp4, Gif, Png, Jpeg };

inline const wchar_t* ExportContainerExtension(ExportContainer container)
{
    switch (container) {
    case ExportContainer::Mp4: return L".mp4";
    case ExportContainer::Gif: return L".gif";
    case ExportContainer::Png: return L".png";
    case ExportContainer::Jpeg: return L".jpg";
    default: return L".mkv";
    }
}

// The Save dialog's filter line for one container: display name, then pattern.
inline std::pair<const wchar_t*, const wchar_t*> ExportContainerFilter(ExportContainer container)
{
    switch (container) {
    case ExportContainer::Mp4: return {L"MP4 video (*.mp4)", L"*.mp4"};
    case ExportContainer::Gif: return {L"Animated GIF (*.gif)", L"*.gif"};
    case ExportContainer::Png: return {L"PNG image (*.png)", L"*.png"};
    case ExportContainer::Jpeg: return {L"JPEG image (*.jpg)", L"*.jpg;*.jpeg"};
    default: return {L"Matroska video (*.mkv)", L"*.mkv"};
    }
}

// The container an extension names, with or without its dot, in any case.
inline std::optional<ExportContainer> ExportContainerFor(std::wstring_view extension)
{
    if (!extension.empty() && extension.front() == L'.') extension.remove_prefix(1);
    const auto is = [&](std::wstring_view candidate) {
        if (extension.size() != candidate.size()) return false;
        for (size_t i = 0; i < candidate.size(); ++i)
            if (std::towlower(extension[i]) != candidate[i]) return false;
        return true;
    };
    if (is(L"mkv")) return ExportContainer::Matroska;
    if (is(L"mp4")) return ExportContainer::Mp4;
    if (is(L"gif")) return ExportContainer::Gif;
    if (is(L"png")) return ExportContainer::Png;
    if (is(L"jpg") || is(L"jpeg")) return ExportContainer::Jpeg;
    return std::nullopt;
}

// What the dialog and --render offer for this source, the default first.
inline std::vector<ExportContainer> ExportContainerChoices(bool stillImage, bool animation)
{
    if (stillImage) return {ExportContainer::Png, ExportContainer::Jpeg};
    if (animation) return {ExportContainer::Gif, ExportContainer::Mp4, ExportContainer::Matroska};
    return {ExportContainer::Matroska, ExportContainer::Mp4};
}

inline bool ExportContainerOffered(ExportContainer container, bool stillImage, bool animation)
{
    for (const ExportContainer offered : ExportContainerChoices(stillImage, animation))
        if (offered == container) return true;
    return false;
}

// The name the export writes, from the one the Save dialog returned and the
// filter that was selected (0-based). A name whose extension is one of the
// offered containers stands; anything else - no extension, ".avi", a
// container this source is not offered - gets the selected filter's extension
// appended, so the file is always what its name says. The common dialog only
// appends its default extension to a name with none, which is how "clip.avi"
// used to become a Matroska file called clip.avi.
inline std::wstring ExportFileName(std::wstring_view chosen, size_t selectedFilter,
                                   bool stillImage, bool animation)
{
    std::wstring name(chosen);
    const size_t separator = name.find_last_of(L"\\/");
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && (separator == std::wstring::npos || dot > separator)) {
        const auto named = ExportContainerFor(std::wstring_view(name).substr(dot));
        if (named && ExportContainerOffered(*named, stillImage, animation)) return name;
    }
    const auto choices = ExportContainerChoices(stillImage, animation);
    return name + ExportContainerExtension(choices[selectedFilter < choices.size() ? selectedFilter : 0]);
}

// What the last step does with each stream it carries beside the video. MKV
// holds almost anything, so its streams are copied; the exceptions are MP4's
// timed text, which Matroska has no mapping for, and a stream with no codec
// Matroska can name. MP4 holds far less: audio it cannot carry is encoded to
// AAC rather than dropped, text subtitles become mov_text, and picture
// subtitles and font attachments, which MP4 has no place for, are left out
// rather than failing an export that may have spent an hour on the GPU. The
// caller reads the finished file's streams back, so a dropped subtitle is
// reported rather than silent.
enum class ExportStreamAction { Copy, EncodeAac, ToMovText, ToSubrip, Drop };

inline ExportStreamAction ExportStreamActionFor(ExportContainer container, std::string_view type,
                                                std::string_view codec)
{
    const auto oneOf = [&](std::initializer_list<std::string_view> names) {
        for (const std::string_view name : names) if (codec == name) return true;
        return false;
    };
    const bool noCodec = codec.empty() || codec == "none" || codec == "unknown";
    const bool textSubtitle = oneOf({"subrip", "srt", "ass", "ssa", "webvtt", "mov_text", "text"});
    if (container == ExportContainer::Matroska) {
        if (type == "audio") return noCodec ? ExportStreamAction::Drop : ExportStreamAction::Copy;
        if (type == "subtitle") {
            if (oneOf({"mov_text", "text"})) return ExportStreamAction::ToSubrip;
            if (textSubtitle || oneOf({"hdmv_pgs_subtitle", "dvd_subtitle", "dvb_subtitle"}))
                return ExportStreamAction::Copy;
            return ExportStreamAction::Drop;
        }
        // Fonts an ASS subtitle needs travel with it.
        if (type == "attachment") return ExportStreamAction::Copy;
        return ExportStreamAction::Drop;
    }
    if (container == ExportContainer::Mp4) {
        if (type == "audio") {
            if (noCodec) return ExportStreamAction::Drop;
            // What FFmpeg's mp4 muxer takes as it stands. PCM, Vorbis, DTS and
            // TrueHD are refused or need -strict, so they are encoded instead.
            return oneOf({"aac", "mp3", "mp2", "ac3", "eac3", "alac", "flac", "opus"})
                ? ExportStreamAction::Copy : ExportStreamAction::EncodeAac;
        }
        if (type == "subtitle") {
            if (codec == "mov_text") return ExportStreamAction::Copy;
            return textSubtitle ? ExportStreamAction::ToMovText : ExportStreamAction::Drop;
        }
        return ExportStreamAction::Drop;
    }
    // GIF, PNG and JPEG are pictures only.
    return ExportStreamAction::Drop;
}

// The sample entry an MP4 names the video with. FFmpeg writes HEVC as `hev1`
// by default, which QuickTime, Safari and iOS refuse to play; `hvc1` is the
// same bitstream under the tag Apple requires, and every other player reads
// both. Empty leaves FFmpeg's choice, which is right for H.264.
inline const wchar_t* ExportVideoTag(ExportContainer container, std::string_view videoCodec)
{
    return container == ExportContainer::Mp4 && videoCodec == "hevc" ? L"hvc1" : L"";
}

// The display aspect an export states for its video, as FFmpeg's -aspect
// takes it (P1.26). Empty for square pixels, which leaves the file exactly as
// it always was. Every pass keeps the source's pixel shape - a neural render
// is at the source's size, and Super Resolution scales both sides by one
// factor - so the source's display aspect is the output's. Without it a DVD
// rip (720x480 at 32:27) or HDV (1440x1080 at 4:3) looked right in the player,
// which reads the sample aspect, and exported stretched, because the encode
// and the mux both wrote square pixels.
//
// `displayAspect` is the upright display aspect the decoder reports. Within
// 0.1% of width:height is square: rounding in a probe, not an anamorphic
// source. The ratio is the nearest with a denominator up to 1000, which is
// exact for every broadcast and disc shape (4:3, 16:9, 2.35:1 as 47:20).
inline std::wstring ExportDisplayAspect(uint32_t width, uint32_t height, double displayAspect)
{
    if (!width || !height || !(displayAspect > 0.1) || !(displayAspect < 10.0)) return {};
    const double frame = double(width) / double(height);
    if (std::abs(displayAspect / frame - 1.0) < 0.001) return {};
    // Continued fractions: the convergents are the best approximations.
    uint64_t hNum = 1, hPrev = 0, kDen = 0, kPrev = 1;
    double x = displayAspect;
    uint64_t bestNum = 0, bestDen = 1;
    for (int step = 0; step < 32; ++step) {
        const double whole = std::floor(x);
        const uint64_t a = uint64_t(whole);
        const uint64_t num = a * hNum + hPrev, den = a * kDen + kPrev;
        if (den > 1000) break;
        bestNum = num; bestDen = den;
        hPrev = hNum; hNum = num; kPrev = kDen; kDen = den;
        if (std::abs(double(num) / double(den) - displayAspect) < 1e-9) break;
        const double fraction = x - whole;
        if (fraction < 1e-12) break;
        x = 1.0 / fraction;
    }
    if (!bestNum || !bestDen) return {};
    return std::to_wstring(bestNum) + L":" + std::to_wstring(bestDen);
}

// The process that reserved an export staging file, from its name:
// `.dlss-export-<pid>-<tick>-<sequence>.tmp` (ReserveExportStaging). Empty for
// any other name, so nothing the user put in the folder is ever taken for one.
inline std::optional<uint32_t> ExportStagingOwner(std::wstring_view name)
{
    constexpr std::wstring_view prefix = L".dlss-export-", suffix = L".tmp";
    if (name.size() <= prefix.size() + suffix.size() || !name.starts_with(prefix) || !name.ends_with(suffix))
        return std::nullopt;
    const std::wstring_view body = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    uint64_t fields[3]{};
    size_t field = 0, digits = 0;
    for (const wchar_t c : body) {
        if (c == L'-') {
            if (!digits || ++field == 3) return std::nullopt;
            digits = 0;
            continue;
        }
        if (c < L'0' || c > L'9' || ++digits > 20) return std::nullopt;
        fields[field] = fields[field] * 10 + uint64_t(c - L'0');
    }
    if (field != 2 || !digits || !fields[0] || fields[0] > 0xFFFFFFFFull) return std::nullopt;
    return uint32_t(fields[0]);
}
