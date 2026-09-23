#pragma once

#include "ExportPipeline.h"
#include "NeuralPresets.h"
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

// What the process returns. Distinct codes, because a batch script's next
// step depends on WHICH of these happened: a bad argument is the script's bug,
// a refusal is this source or this machine, a failure is worth a retry, and a
// cancel was somebody's decision.
inline constexpr int kExitOk = 0;
inline constexpr int kExitBadArguments = 2;
inline constexpr int kExitRefused = 3;
inline constexpr int kExitFailed = 4;
inline constexpr int kExitCancelled = 5;

enum class Mode {
    // No --render and no --help: the player starts exactly as it always has.
    Player,
    Help,
    Render,
    BadArguments,
};

struct Command {
    std::wstring input;
    // Empty means "<input stem>-dlss.mkv beside the input" (DefaultOutput).
    std::wstring output;
    // Timecodes as typed, in ParseTimecode's grammar. Resolved once the source
    // frame rate is known, which this parser does not have.
    std::wstring rangeStart, rangeEnd;
    bool hasRange{};
    // Index into neural_presets::kPresets; empty uses the saved Neural settings,
    // as the dialog does.
    std::optional<size_t> preset;
    // One of kProcessingScaleRungs; empty uses the saved processing scale.
    std::optional<uint32_t> processingScale;
    // Stages, output rung and multiplier, in the dialog's own vocabulary so the
    // plan comes out of the same PlanExport.
    ExportSelection selection;
    bool quiet{};
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
    bool render = false;
    for (const std::wstring& argument : userArguments) {
        if (IsHelpFlag(argument)) {
            parsed.mode = Mode::Help;
            return parsed;
        }
        render = render || argument == L"--render";
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
         seenScale = false;
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
        const bool takesValue = argument == L"--render" || argument == L"--range" ||
            argument == L"--preset" || argument == L"--stages" || argument == L"--height" ||
            argument == L"--multiplier" || argument == L"--out" || argument == L"--processing-scale";
        if (!takesValue) return bad(L"Unknown argument: " + argument);
        if (index + 1 >= userArguments.size() || userArguments[index + 1].empty())
            return bad(argument + L" needs a value.");
        const std::wstring& value = userArguments[++index];
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
        } else if (argument == L"--multiplier") {
            if (!once(seenMultiplier)) return bad(L"--multiplier was given twice.");
            const auto multiplier = ParseCount(value);
            if (!multiplier || *multiplier < 2 || *multiplier > 5)
                return bad(L"--multiplier takes 2, 3, 4 or 5.");
            command.selection.multiplier = *multiplier;
        } else {
            if (!once(seenOut)) return bad(L"--out was given twice.");
            // The passes write Matroska and the finished file is moved into
            // place as it is, so another extension would name a container the
            // file is not.
            if (!EqualsIgnoringCase(std::filesystem::path(value).extension().wstring(), L".mkv"))
                return bad(L"--out must name a .mkv file: the export writes Matroska.");
            command.output = value;
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
        L"  --processing-scale N  The resolution the model runs at, as a percentage of\n"
        L"                     the source: 100, 75 or 50, restored to the source size by\n"
        L"                     Super Resolution. For nr without sr. Default: the player's\n"
        L"                     saved setting, 100 unless changed.\n"
        L"  --range START-END  Render only this part, e.g. 0:10-0:25 or f0-f300. Needs sr\n"
        L"                     or nr. Default: the whole source.\n"
        L"  --out FILE         The .mkv to write. An existing file is replaced. Default:\n"
        L"                     <input>-dlss.mkv beside the input, never replaced.\n"
        L"  --quiet            Print only the final line.\n"
        L"  --help             Print this and exit.\n"
        L"\n"
        L"Exit codes: 0 done, 2 bad arguments, 3 refused (the reason is printed),\n"
        L"4 failed, 5 cancelled (Ctrl+C).";
}

} // namespace render_command
