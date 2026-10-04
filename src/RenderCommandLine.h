#pragma once

#include "ExportPipeline.h"
#include "NeuralPresets.h"
#include "RenderCliContract.h"
#include "UpscalingPolicy.h"

#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// `DLSSVideoPlayer.exe --render <input> ...`: the "Export with DLSS stages"
// pipeline without the window. It exists for batch work and for the benchmark
// harness, which both want the same file the dialog writes without driving a
// GUI - issue #64 asks for exactly the upscale -> interpolate -> enhance chain
// the dialog already runs.
//
// Pure: this decides what the command line asks for and nothing else. The
// geometry, the frame rate and the runtime's multiplier cap are only known
// once the source is probed, so the range and the plan are resolved by the
// caller against them, through the same PlanExport the dialog uses.
namespace render_command {

// What the process returns: render_cli's codes, which dlss5-convert reads.
using render_cli::kExitOk;
using render_cli::kExitBadArguments;
using render_cli::kExitRefused;
using render_cli::kExitFailed;
using render_cli::kExitCancelled;

enum class Mode {
    // No --render and no --help: the player starts exactly as it always has.
    Player,
    Help,
    Render,
    // `--probe <input>`: what the source is and what this machine can do with
    // it, without rendering anything (dlss5-convert probe).
    Probe,
    BadArguments,
};

struct Command {
    std::wstring input;
    // Empty means "<input stem>-dlss.mkv beside the input" (DefaultOutput),
    // with the source's default container's extension once it is read.
    std::wstring output;
    // Timecodes as typed, in ParseTimecode's grammar. Resolved once the source
    // frame rate is known, which this parser does not have.
    std::wstring rangeStart, rangeEnd;
    bool hasRange{};
    // Index into neural_presets::kPresets; empty uses the saved Neural settings,
    // as the dialog does.
    std::optional<size_t> preset;
    // The Neural settings dialog's knobs, applied over the preset (or the
    // saved settings): neural passes 1..4, and the Look group's intensity,
    // local tone and local structure 0..2 and colour strength 0..1. Empty
    // leaves the value the preset or the saved settings give.
    std::optional<int> passes;
    std::optional<float> intensity, localTone, localStructure, colorStrength;
    // Encoder settings' quality ladder, by its stable name (standard, high,
    // lossless; EncoderQualityName); empty uses the saved rung.
    std::string quality;
    // Super Resolution's history for sr without nr (UpscalingHistoryName);
    // empty uses the saved choice.
    std::optional<UpscalingHistory> history;
    // Super Resolution's engine for sr without nr; empty uses the saved choice.
    // --history alone implies dlss, the engine it belongs to.
    std::optional<SuperResolutionEngine> engine;
    // One of kProcessingScaleRungs; empty uses the saved processing scale.
    std::optional<uint32_t> processingScale;
    // Stages, output rung and multiplier, in the dialog's own vocabulary so the
    // plan comes out of the same PlanExport.
    ExportSelection selection;
    bool quiet{};
    // --probe only: JSON rather than key=value lines, and whether to bring up
    // the frame-generation probe, which needs a device of its own.
    bool json{};
    bool capabilities{};
    // ParseRuntimeArguments keeps --safe-mode in the user arguments; the render
    // honours it by refusing the neural stage rather than rejecting the flag.
    bool safeMode{};
};

struct Parsed {
    Mode mode{Mode::Player};
    Command command;
    std::wstring error;
};

inline bool EqualsIgnoringCase(std::wstring_view left, std::wstring_view right)
{
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (std::towlower(left[index]) != std::towlower(right[index])) return false;
    }
    return true;
}

inline bool IsHelpFlag(std::wstring_view argument)
{
    return argument == L"--help" || argument == L"-h" || argument == L"/?";
}

// A plain decimal within [low, high]: digits with at most one point, no sign,
// no exponent, no trailing text. What the dialog's sliders can reach.
inline std::optional<float> ParseLevel(std::wstring_view text, float low, float high)
{
    if (text.empty() || text.size() > 12) return std::nullopt;
    double value = 0.0, scale = 0.0;
    bool digits = false;
    for (const wchar_t character : text) {
        if (character == L'.') {
            if (scale != 0.0) return std::nullopt;
            scale = 1.0;
            continue;
        }
        if (character < L'0' || character > L'9') return std::nullopt;
        digits = true;
        if (scale == 0.0) value = value * 10.0 + double(character - L'0');
        else { scale /= 10.0; value += scale * double(character - L'0'); }
    }
    if (!digits || value < low || value > high) return std::nullopt;
    return float(value);
}

// Strict decimal: digits only, no sign, no trailing text, and it must fit.
inline std::optional<uint32_t> ParseCount(std::wstring_view text)
{
    if (text.empty() || text.size() > 9) return std::nullopt;
    uint32_t value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') return std::nullopt;
        value = value * 10u + uint32_t(character - L'0');
    }
    return value;
}

// `userArguments` is RuntimeArguments::userArguments: argv without the
// executable and without the bootstrap marker.
inline Parsed Parse(std::span<const std::wstring> userArguments)
{
    Parsed parsed;
    bool render = false, probe = false;
    for (const std::wstring& argument : userArguments) {
        if (IsHelpFlag(argument)) {
            parsed.mode = Mode::Help;
            return parsed;
        }
        render = render || argument == L"--render";
        probe = probe || argument == L"--probe";
    }
    if (probe) {
        const auto bad = [&](std::wstring error) {
            parsed.mode = Mode::BadArguments;
            parsed.error = std::move(error);
            return parsed;
        };
        if (render) return bad(L"--probe and --render are two commands; give one.");
        for (size_t index = 0; index < userArguments.size(); ++index) {
            const std::wstring& argument = userArguments[index];
            if (argument == L"--json") { parsed.command.json = true; continue; }
            if (argument == L"--capabilities") { parsed.command.capabilities = true; continue; }
            if (argument == L"--safe-mode") { parsed.command.safeMode = true; continue; }
            if (argument != L"--probe") return bad(L"Unknown argument for --probe: " + argument);
            if (!parsed.command.input.empty()) return bad(L"--probe was given twice.");
            if (index + 1 >= userArguments.size() || userArguments[index + 1].empty())
                return bad(L"--probe needs a value.");
            parsed.command.input = userArguments[++index];
        }
        parsed.mode = Mode::Probe;
        return parsed;
    }
    // Anything without --render is a player launch, and ParseArgs owns it -
    // including a bare file path, the drag-onto-the-exe case.
    if (!render) return parsed;

    const auto bad = [&](std::wstring error) {
        parsed.mode = Mode::BadArguments;
        parsed.error = std::move(error);
        return parsed;
    };
    Command& command = parsed.command;
    bool seenRender = false, seenRange = false, seenPreset = false, seenStages = false,
         seenHeight = false, seenMultiplier = false, seenOut = false, seenQuiet = false,
         seenScale = false, seenPasses = false, seenIntensity = false, seenTone = false,
         seenStructure = false, seenColor = false, seenEncode = false, seenHistory = false, seenEngine = false;
    std::wstring_view presetName;
    // What the dialog opens with: the neural pass alone.
    command.selection = ExportSelection{};
    command.selection.neural = true;
    for (size_t index = 0; index < userArguments.size(); ++index) {
        const std::wstring& argument = userArguments[index];
        // Flags that take no value.
        if (argument == L"--quiet") {
            if (seenQuiet) return bad(L"--quiet was given twice.");
            seenQuiet = command.quiet = true;
            continue;
        }
        if (argument == L"--safe-mode") {
            command.safeMode = true;
            continue;
        }
        const auto once = [&](bool& seen) {
            if (seen) return false;
            seen = true;
            return true;
        };
        // render_cli lists them, so dlss5-convert forwards what this takes.
        const bool takesValue = render_cli::IsRenderValueOption(argument);
        if (argument == L"--quality")
            return bad(std::wstring(render_cli::kRetiredQualityRefusal));
        if (!takesValue) return bad(L"Unknown argument: " + argument);
        if (index + 1 >= userArguments.size() || userArguments[index + 1].empty())
            return bad(argument + L" needs a value.");
        const std::wstring& value = userArguments[++index];
        // Neural settings' Look group: 0 to 2, colour strength 0 to 1.
        const auto look = [&](bool& seen, std::optional<float>& target, float high) -> std::optional<std::wstring> {
            if (!once(seen)) return argument + L" was given twice.";
            const auto level = ParseLevel(value, 0.0f, high);
            if (!level)
                return argument + (high < 2.0f ? L" takes a value from 0 to 1, such as 0.8."
                                               : L" takes a value from 0 to 2, such as 1.5.");
            target = *level;
            return std::nullopt;
        };
        if (argument == L"--render") {
            if (!once(seenRender)) return bad(L"--render was given twice.");
            command.input = value;
        } else if (argument == L"--range") {
            if (!once(seenRange)) return bad(L"--range was given twice.");
            // Timecodes hold no '-', so exactly one separates the two ends.
            const size_t dash = value.find(L'-');
            if (dash == std::wstring::npos || dash == 0 || dash + 1 == value.size() ||
                value.find(L'-', dash + 1) != std::wstring::npos)
                return bad(L"--range takes START-END, for example 0:10-0:25 or f0-f300.");
            command.rangeStart = value.substr(0, dash);
            command.rangeEnd = value.substr(dash + 1);
            command.hasRange = true;
        } else if (argument == L"--preset") {
            if (!once(seenPreset)) return bad(L"--preset was given twice.");
            presetName = value;
        } else if (argument == L"--stages") {
            if (!once(seenStages)) return bad(L"--stages was given twice.");
            command.selection.upscale = command.selection.neural = command.selection.frameGeneration = false;
            size_t start = 0;
            for (;;) {
                const size_t comma = value.find(L',', start);
                const std::wstring_view stage = std::wstring_view(value).substr(
                    start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
                bool* slot = EqualsIgnoringCase(stage, L"sr") ? &command.selection.upscale
                    : EqualsIgnoringCase(stage, L"nr") ? &command.selection.neural
                    : EqualsIgnoringCase(stage, L"fg") ? &command.selection.frameGeneration
                    : nullptr;
                if (!slot) return bad(L"--stages takes a comma-separated list of sr, nr and fg.");
                if (*slot) return bad(L"--stages names a stage twice.");
                *slot = true;
                if (comma == std::wstring::npos) break;
                start = comma + 1;
            }
        } else if (argument == L"--height") {
            if (!once(seenHeight)) return bad(L"--height was given twice.");
            const auto height = ParseCount(value);
            bool rung = false;
            for (const uint32_t candidate : kUpscaleRungHeights) rung = rung || (height && *height == candidate);
            if (!rung) return bad(L"--height takes 1080, 1440 or 2160.");
            command.selection.targetHeight = *height;
        } else if (argument == L"--processing-scale") {
            if (!once(seenScale)) return bad(L"--processing-scale was given twice.");
            const auto percent = ParseCount(value);
            if (!percent || !IsProcessingScaleRung(*percent))
                return bad(L"--processing-scale takes 100, 75 or 50.");
            command.processingScale = *percent;
        } else if (argument == L"--passes") {
            if (!once(seenPasses)) return bad(L"--passes was given twice.");
            const auto passes = ParseCount(value);
            if (!passes || *passes < 1 || *passes > 4) return bad(L"--passes takes 1, 2, 3 or 4.");
            command.passes = int(*passes);
        } else if (argument == L"--intensity") {
            if (auto error = look(seenIntensity, command.intensity, 2.0f)) return bad(std::move(*error));
        } else if (argument == L"--local-tone") {
            if (auto error = look(seenTone, command.localTone, 2.0f)) return bad(std::move(*error));
        } else if (argument == L"--local-structure") {
            if (auto error = look(seenStructure, command.localStructure, 2.0f)) return bad(std::move(*error));
        } else if (argument == L"--color-strength") {
            if (auto error = look(seenColor, command.colorStrength, 1.0f)) return bad(std::move(*error));
        } else if (argument == L"--encode") {
            if (!once(seenEncode)) return bad(L"--encode was given twice.");
            if (value != L"standard" && value != L"high" && value != L"lossless")
                return bad(L"--encode takes standard, high or lossless.");
            command.quality = value == L"standard" ? "standard" : value == L"high" ? "high" : "lossless";
        } else if (argument == L"--sr-engine") {
            if (!once(seenEngine)) return bad(L"--sr-engine was given twice.");
            std::string lower;
            for (const wchar_t c : value) lower.push_back(c < 0x80 ? static_cast<char>(std::towlower(c)) : '?');
            command.engine = ParseSuperResolutionEngine(lower);
            if (!command.engine) return bad(L"--sr-engine takes vsr or dlss.");
        } else if (argument == L"--history") {
            if (!once(seenHistory)) return bad(L"--history was given twice.");
            command.history = ParseUpscalingHistory(value == L"temporal" ? "temporal" : value == L"per-frame" ? "per-frame" : "");
            if (!command.history) return bad(L"--history takes temporal or per-frame.");
        } else if (argument == L"--multiplier") {
            if (!once(seenMultiplier)) return bad(L"--multiplier was given twice.");
            const auto multiplier = ParseCount(value);
            if (!multiplier || *multiplier < 2 || *multiplier > 5)
                return bad(L"--multiplier takes 2, 3, 4 or 5.");
            command.selection.multiplier = *multiplier;
        } else if (argument == L"--out") {
            if (!once(seenOut)) return bad(L"--out was given twice.");
            // The container follows the extension (ExportContainerFor). Which
            // of them this source may be written as is settled once it is
            // read: a photo is a .png or .jpg, a video a .mkv or .mp4.
            if (!ExportContainerFor(std::filesystem::path(value).extension().wstring()))
                return bad(L"--out must name a .mkv, .mp4, .gif, .png or .jpg file.");
            command.output = value;
        } else {
            // In render_cli's table but not read here: refused, never taken
            // for another option.
            return bad(L"Unknown argument: " + argument);
        }
    }
    const ExportSelection& selection = command.selection;
    // Options that only mean something to a stage that was not asked for are
    // refused rather than ignored: a --height on an export that does not
    // upscale is a script that believes something untrue about its output.
    if (seenPreset) {
        if (!selection.neural) return bad(L"--preset needs the nr stage.");
        const size_t found = [&] {
            for (size_t candidate = 0; candidate < neural_presets::kPresetCount; ++candidate) {
                const std::string_view key = neural_presets::kPresets[candidate].key;
                if (EqualsIgnoringCase(presetName, std::wstring(key.begin(), key.end()))) return candidate;
            }
            return neural_presets::kPresetCount;
        }();
        if (found == neural_presets::kPresetCount)
            return bad(L"--preset takes natural, detail-only, gentle or strong.");
        command.preset = found;
    }
    if (seenHeight && !selection.upscale) return bad(L"--height needs the sr stage.");
    // The model's scale is restored to the SOURCE size, so it has nothing to
    // act on in a pass that upscales, and nothing at all without the model.
    if (seenScale && (!selection.neural || selection.upscale))
        return bad(L"--processing-scale needs the nr stage without sr.");
    if (seenMultiplier && !selection.frameGeneration) return bad(L"--multiplier needs the fg stage.");
    if ((seenPasses || seenIntensity || seenTone || seenStructure || seenColor) && !selection.neural)
        return bad(L"--passes, --intensity, --local-tone, --local-structure and --color-strength need the nr stage.");
    // The model keeps its own history on the carrier it runs on, so History is
    // Super Resolution's choice only when the model does not run (the dialog
    // greys it out the same way).
    if (seenHistory && (!selection.upscale || selection.neural))
        return bad(L"--history needs the sr stage without nr.");
    if (seenEngine && (!selection.upscale || selection.neural))
        return bad(L"--sr-engine needs the sr stage without nr: with nr the upscale is the model's carrier.");
    if (seenHistory && command.engine == SuperResolutionEngine::RtxVsr)
        return bad(L"--history is DLSS Super Resolution's; RTX VSR has none. Drop it, or use --sr-engine dlss.");
    if (seenHistory && !command.engine) command.engine = SuperResolutionEngine::Dlss;
    if (seenEncode && !selection.upscale && !selection.neural)
        return bad(L"--encode needs the sr or nr stage: frame generation alone keeps its own encode.");
    // Frame generation converts a whole file and copies the source's audio onto
    // it with no retime, so it has no range of its own; a range reaches it only
    // through the worker pass, whose carrier already covers just that range.
    if (command.hasRange && !selection.upscale && !selection.neural)
        return bad(L"--range needs the sr or nr stage: frame generation alone converts the whole file.");
    parsed.mode = Mode::Render;
    return parsed;
}

// Beside the input, so a batch over a folder leaves each result next to its
// source. `-dlss` rather than the dialog's `-neural`, because an upscale-only
// or frame-generation-only export is not neural.
inline std::filesystem::path DefaultOutput(const std::filesystem::path& input)
{
    std::filesystem::path output = input.parent_path() / input.stem();
    output += L"-dlss.mkv";
    return output;
}

// "1/2 Super Resolution and neural rendering: 120/300 frames (40%)". The total
// is 0 until the pass knows it, and the line says so rather than dividing by it.
inline std::wstring ProgressLine(uint32_t pass, uint32_t passes, std::wstring_view label,
                                 uint64_t done, uint64_t total)
{
    std::wstring line = std::to_wstring(pass) + L"/" + std::to_wstring(passes) + L" " +
                        std::wstring(label) + L": " + std::to_wstring(done);
    if (!total) return line + L" frames";
    const uint64_t clamped = done < total ? done : total;
    return line + L"/" + std::to_wstring(total) + L" frames (" +
           std::to_wstring(clamped * 100u / total) + L"%)";
}

inline std::wstring Usage()
{
    return
        L"Usage: DLSSVideoPlayer.exe --render <input> [options]\n"
        L"\n"
        L"Runs Export with DLSS stages without opening the player, and writes one file.\n"
        L"\n"
        L"  --stages LIST      Comma-separated: sr (DLSS Super Resolution), nr (neural\n"
        L"                     rendering), fg (frame generation). They run in the order\n"
        L"                     sr, nr, fg whatever order they are listed in. Default: nr.\n"
        L"  --height N         Output height for sr: 1080, 1440 or 2160. Default: 1440.\n"
        L"  --multiplier N     Output frames per source frame for fg: 2 to 5, as far as\n"
        L"                     this GPU admits. Default: 2.\n"
        L"  --preset NAME      Neural look for nr: natural, detail-only, gentle or strong.\n"
        L"                     Default: the Neural settings saved by the player.\n"
        L"  --passes N         Neural passes for nr, 1 to 4, stacked by the add-on; each\n"
        L"                     costs one more model evaluation per frame. Default: the\n"
        L"                     preset's or the saved setting.\n"
        L"  --intensity X      nr intensity, 0 to 1; up to 2 is accepted and renders as 1,\n"
        L"                     all the runtime applies. --local-tone X and\n"
        L"                     --local-structure X, 0 to 2, and --color-strength X, 0 to 1,\n"
        L"                     are the rest of Neural settings' Look group. Each applies\n"
        L"                     over --preset.\n"
        L"  --encode Q         The encode: standard (8-bit), high (10-bit) or lossless.\n"
        L"                     Default: the player's saved Encoder settings.\n"
        L"  --sr-engine E      The upscaler for sr without nr: vsr (RTX Video Super\n"
        L"                     Resolution, recommended) or dlss (DLSS Super Resolution).\n"
        L"                     Default: the player's saved choice. With nr it is DLSS.\n"
        L"  --history H        DLSS Super Resolution's history for sr without nr: temporal\n"
        L"                     or per-frame; implies --sr-engine dlss. Default: the\n"
        L"                     player's saved choice.\n"
        L"  --processing-scale N  The resolution the model runs at, as a percentage of\n"
        L"                     the source: 100, 75 or 50, restored to the source size by\n"
        L"                     Super Resolution. For nr without sr. Default: the player's\n"
        L"                     saved setting, 100 unless changed.\n"
        L"  --range START-END  Render only this part, e.g. 0:10-0:25 or f0-f300. Needs sr\n"
        L"                     or nr. Default: the whole source.\n"
        L"  --out FILE         The file to write, in the container its extension names:\n"
        L"                     .mkv or .mp4 for a video, also .gif for an animation,\n"
        L"                     .png or .jpg for a photo. An existing file is replaced.\n"
        L"                     Default: <input>-dlss.mkv (.gif, .png) beside the input,\n"
        L"                     never replaced.\n"
        L"  --quiet            Print only the final line.\n"
        L"  --help             Print this and exit.\n"
        L"\n"
        L"       DLSSVideoPlayer.exe --probe <input> [--json] [--capabilities]\n"
        L"\n"
        L"Prints what the source is and which stages this machine can run on it,\n"
        L"without rendering. --capabilities also measures frame generation's rate\n"
        L"cap, which brings up a device of its own. dlss5-convert probe runs this.\n"
        L"\n"
        L"Exit codes: 0 done, 2 bad arguments, 3 refused (the reason is printed),\n"
        L"4 failed, 5 cancelled (Ctrl+C).";
}

} // namespace render_command
