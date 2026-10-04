#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "RenderCliContract.h"

// `dlss5-convert`: the console front end for DLSSVideoPlayer.exe's --render
// and --probe (ConvertMain.cpp).
//
// The player is a Windows program, so a shell does not wait for it, does not
// show its progress, and cannot hand it Ctrl+C unless it is started with the
// right incantation (USAGE.md). This is a console program that does, and it
// adds what converting more than one file needs: folders, wildcards, an output
// folder, skipping what is already converted, one summary and one exit code.
// Every render is still DLSSVideoPlayer.exe --render, so there is one
// conversion pipeline and one set of refusals, whichever way it is started.
//
// Pure: what the command line asks for, where each output goes, and what the
// player is handed. Finding files and running the player are ConvertMain's.
namespace convert_command {

// The player's exit codes (render_cli, which render_command uses too), and the batch's.
using render_cli::kExitOk;
using render_cli::kExitBadArguments;
using render_cli::kExitRefused;
using render_cli::kExitFailed;
using render_cli::kExitCancelled;

enum class Mode { Convert, Probe, Help, Version, BadArguments };

struct Options {
    Mode mode{Mode::Convert};
    std::vector<std::wstring> inputs;
    // --render options, passed through as given: the player validates them,
    // and a batch refuses a bad one before it starts (see Parse).
    std::vector<std::wstring> renderOptions;
    std::wstring out;      // --out: one input only
    std::wstring outDir;   // --out-dir
    std::wstring format;   // --format: mkv, mp4, gif, png or jpg, lower case
    std::wstring suffix{L"-dlss"};
    std::wstring report;   // --report: a JSON summary written here
    std::wstring player;   // --player: the DLSSVideoPlayer.exe to run
    bool overwrite{};
    bool recursive{};
    bool failFast{};
    bool dryRun{};
    bool quiet{};
    // probe
    bool json{};
    bool capabilities{};
    std::wstring error;
};

inline std::wstring Lower(std::wstring_view text)
{
    std::wstring lowered(text);
    for (wchar_t& c : lowered) c = static_cast<wchar_t>(std::towlower(c));
    return lowered;
}

// The containers --format names, as the export writes them.
inline bool KnownFormat(std::wstring_view format)
{
    for (const wchar_t* known : {L"mkv", L"mp4", L"gif", L"png", L"jpg"})
        if (format == known) return true;
    return false;
}

// What a folder scan picks up. A named file is always tried, whatever its
// extension - the player reads whatever FFmpeg or Media Foundation can.
inline bool IsVideoExtension(std::wstring_view extension)
{
    const std::wstring lowered = Lower(extension);
    for (const wchar_t* known : {L".mp4", L".mkv", L".mov", L".m4v", L".avi", L".webm", L".wmv", L".flv",
                                 L".ts", L".m2ts", L".mts", L".mpg", L".mpeg", L".3gp", L".ogv", L".gif"})
        if (lowered == known) return true;
    return false;
}

inline bool IsPhotoExtension(std::wstring_view extension)
{
    const std::wstring lowered = Lower(extension);
    for (const wchar_t* known : {L".png", L".jpg", L".jpeg", L".bmp", L".tif", L".tiff", L".webp", L".jxr"})
        if (lowered == known) return true;
    return false;
}

inline bool HasWildcard(std::wstring_view input)
{
    return input.find_first_of(L"*?") != std::wstring_view::npos;
}

inline Options Parse(std::span<const std::wstring> arguments)
{
    Options options;
    const auto bad = [&](std::wstring error) {
        options.mode = Mode::BadArguments;
        options.error = std::move(error);
        return options;
    };
    size_t index = 0;
    if (!arguments.empty()) {
        if (arguments[0] == L"probe") { options.mode = Mode::Probe; ++index; }
        else if (arguments[0] == L"convert") ++index;
    }
    bool literal = false;
    for (; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        // After "--" everything is an input, so a file named "-x.mp4" can be.
        if (literal || argument.empty() || argument[0] != L'-' || argument == L"-") {
            options.inputs.push_back(argument);
            continue;
        }
        if (argument == L"--") { literal = true; continue; }
        if (argument == L"--help" || argument == L"-h" || argument == L"/?") { options.mode = Mode::Help; return options; }
        if (argument == L"--version") { options.mode = Mode::Version; return options; }
        // --suffix may be empty (the same names, in --out-dir); nothing else may.
        const auto value = [&]() -> const std::wstring* {
            if (index + 1 >= arguments.size() || (arguments[index + 1].empty() && argument != L"--suffix")) return nullptr;
            return &arguments[++index];
        };
        if (options.mode == Mode::Probe) {
            if (argument == L"--json") options.json = true;
            else if (argument == L"--capabilities") options.capabilities = true;
            else if (argument == L"--player") {
                const std::wstring* given = value();
                if (!given) return bad(L"--player needs a value.");
                options.player = *given;
            } else return bad(L"Unknown option for probe: " + argument);
            continue;
        }
        if (argument == L"--overwrite") options.overwrite = true;
        else if (argument == L"--recursive" || argument == L"-r") options.recursive = true;
        else if (argument == L"--fail-fast") options.failFast = true;
        else if (argument == L"--dry-run") options.dryRun = true;
        else if (argument == L"--quiet" || argument == L"-q") options.quiet = true;
        else if (render_cli::IsForwardedFlag(argument)) options.renderOptions.push_back(argument);
        else if (render_cli::IsForwardedValueOption(argument)) {
            const std::wstring* given = value();
            if (!given) return bad(argument + L" needs a value.");
            for (size_t seen = 0; seen < options.renderOptions.size(); ++seen)
                if (options.renderOptions[seen] == argument) return bad(argument + L" was given twice.");
            options.renderOptions.insert(options.renderOptions.end(), {argument, *given});
        } else if (argument == L"--out" || argument == L"-o" || argument == L"--out-dir" || argument == L"--format" ||
                   argument == L"--suffix" || argument == L"--report" || argument == L"--player") {
            const std::wstring* given = value();
            if (!given) return bad(argument + L" needs a value.");
            std::wstring& slot = argument == L"--out" || argument == L"-o" ? options.out
                : argument == L"--out-dir" ? options.outDir
                : argument == L"--format" ? options.format
                : argument == L"--suffix" ? options.suffix
                : argument == L"--report" ? options.report : options.player;
            slot = *given;
        } else if (argument == L"--quality") {
            return bad(std::wstring(render_cli::kRetiredQualityRefusal));
        } else {
            return bad(L"Unknown option: " + argument);
        }
    }
    if (options.inputs.empty()) return bad(options.mode == Mode::Probe ? L"probe needs a file." : L"Name at least one file or folder to convert.");
    if (options.mode == Mode::Probe) {
        if (options.inputs.size() != 1) return bad(L"probe reads one file.");
        return options;
    }
    if (!options.format.empty()) {
        options.format = Lower(options.format);
        if (options.format == L"jpeg") options.format = L"jpg";
        if (!KnownFormat(options.format)) return bad(L"--format takes mkv, mp4, gif, png or jpg.");
    }
    if (!options.out.empty()) {
        if (!options.outDir.empty()) return bad(L"--out names one file and --out-dir a folder; give one.");
        if (!options.format.empty()) return bad(L"--out names the container by its extension; --format is for many files.");
        if (options.recursive) return bad(L"--out names one file; --recursive finds many.");
    }
    // The suffix is part of a file name: no separators, and not nothing, or
    // the output would be the input.
    if (options.suffix.empty() && options.outDir.empty() && options.out.empty())
        return bad(L"--suffix cannot be empty unless --out-dir writes elsewhere.");
    if (options.suffix.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
        return bad(L"--suffix cannot hold a path separator or a character a file name cannot.");
    return options;
}

// The container a file converts to when neither --out nor --format says: what
// the export dialog would pick for it (MKV for a video, GIF for an animation,
// PNG for a photo).
inline std::wstring DefaultFormat(const std::filesystem::path& input)
{
    const std::wstring extension = Lower(input.extension().wstring());
    if (extension == L".gif") return L"gif";
    if (IsPhotoExtension(extension)) return L"png";
    return L"mkv";
}

// Where one input's result goes. `root` is the folder a scan started from,
// empty for a named file: with --out-dir, a recursive scan keeps the
// subfolders it found the file in, so two clip.mp4 in two folders stay two.
inline std::filesystem::path OutputFor(const std::filesystem::path& input, const std::filesystem::path& root,
                                       const Options& options)
{
    if (!options.out.empty()) return std::filesystem::path(options.out);
    std::filesystem::path folder = input.parent_path();
    if (!options.outDir.empty()) {
        folder = std::filesystem::path(options.outDir);
        if (!root.empty()) {
            const std::filesystem::path relative = input.parent_path().lexically_relative(root);
            if (!relative.empty() && relative != L"." && *relative.begin() != L"..") folder /= relative;
        }
    }
    const std::wstring format = options.format.empty() ? DefaultFormat(input) : options.format;
    return folder / (input.stem().wstring() + options.suffix + L"." + format);
}

// A folder scan leaves out what an earlier run wrote beside its sources, so
// running the same command twice converts nothing twice.
inline bool LooksLikeOutput(const std::filesystem::path& file, std::wstring_view suffix)
{
    if (suffix.empty()) return false;
    const std::wstring stem = Lower(file.stem().wstring());
    const std::wstring tail = Lower(suffix);
    return stem.size() > tail.size() && stem.compare(stem.size() - tail.size(), tail.size(), tail) == 0;
}

// The player's command line for one conversion.
inline std::vector<std::wstring> PlayerArguments(const Options& options, const std::filesystem::path& input,
                                                 const std::filesystem::path& output)
{
    std::vector<std::wstring> arguments{L"--render", input.wstring()};
    arguments.insert(arguments.end(), options.renderOptions.begin(), options.renderOptions.end());
    arguments.insert(arguments.end(), {L"--out", output.wstring()});
    if (options.quiet) arguments.push_back(L"--quiet");
    return arguments;
}

// NotStarted is a file a stopped batch never reached: it says nothing about
// the file, so it does not decide the exit code - what stopped the batch does.
enum class Outcome { Done, Skipped, BadArguments, Refused, Failed, Cancelled, NotStarted, Planned };

inline Outcome OutcomeOf(int exitCode)
{
    switch (exitCode) {
        case kExitOk: return Outcome::Done;
        case kExitBadArguments: return Outcome::BadArguments;
        case kExitRefused: return Outcome::Refused;
        case kExitCancelled: return Outcome::Cancelled;
        default: return Outcome::Failed;
    }
}

inline const wchar_t* OutcomeName(Outcome outcome)
{
    switch (outcome) {
        case Outcome::Done: return L"done";
        case Outcome::Skipped: return L"skipped";
        case Outcome::BadArguments: return L"bad arguments";
        case Outcome::Refused: return L"refused";
        case Outcome::Failed: return L"failed";
        case Outcome::Cancelled: return L"cancelled";
        case Outcome::NotStarted: return L"not started";
        case Outcome::Planned: return L"planned";
    }
    return L"failed";
}

// One exit code for a batch. A cancel wins, because a script must not take a
// batch somebody stopped for one that finished; otherwise the worst of what
// went wrong - a failure is worth a retry, a refusal is this file or this
// machine, a bad argument is the script's - and 0 when every file was done or
// already there.
inline int BatchExitCode(std::span<const Outcome> outcomes)
{
    bool refused = false, badArguments = false, failed = false;
    for (const Outcome outcome : outcomes) {
        if (outcome == Outcome::Cancelled) return kExitCancelled;
        failed = failed || outcome == Outcome::Failed;
        refused = refused || outcome == Outcome::Refused;
        badArguments = badArguments || outcome == Outcome::BadArguments;
    }
    if (failed) return kExitFailed;
    if (refused) return kExitRefused;
    if (badArguments) return kExitBadArguments;
    return kExitOk;
}

inline std::wstring Usage()
{
    return
        L"Usage: dlss5-convert <file|folder|wildcard>... [options]\n"
        L"       dlss5-convert probe <file> [--json] [--capabilities]\n"
        L"\n"
        L"Converts videos, animations and photos with the DLSS 5 Video Player's export\n"
        L"pipeline (DLSSVideoPlayer.exe --render), waits for each, and shows progress.\n"
        L"\n"
        L"What to render (as the player's Export with DLSS stages):\n"
        L"  --stages LIST          sr, nr and/or fg, comma-separated. Default: nr.\n"
        L"  --height N             sr output height: 1080, 1440 or 2160. Default: 1440.\n"
        L"  --multiplier N         fg frames per source frame: 2 to 5. Default: 2.\n"
        L"  --preset NAME          nr look: natural, detail-only, gentle or strong.\n"
        L"  --passes N             nr passes, 1 to 4 (each one more model evaluation).\n"
        L"  --intensity X          nr intensity 0-1 (above 1 renders as 1); also\n"
        L"                         --local-tone X, --local-structure X (0-2) and\n"
        L"                         --color-strength X (0-1). Each applies over --preset;\n"
        L"                         unset ones keep the player's saved settings.\n"
        L"  --processing-scale N   nr without sr: 100, 75 or 50.\n"
        L"  --encode Q             standard (8-bit), high (10-bit) or lossless encode.\n"
        L"  --sr-engine E          sr without nr: vsr (RTX VSR, recommended) or dlss.\n"
        L"  --history H            sr without nr, DLSS only: temporal or per-frame.\n"
        L"  --range START-END      Part of each source, e.g. 0:10-0:25 or f0-f300.\n"
        L"\n"
        L"Where it goes:\n"
        L"  -o, --out FILE         The file to write, for a single input.\n"
        L"  --out-dir DIR          Write results here (created if missing). A recursive\n"
        L"                         scan keeps its subfolders.\n"
        L"  --format FMT           mkv, mp4, gif, png or jpg. Default: mkv for a video,\n"
        L"                         gif for an animation, png for a photo.\n"
        L"  --suffix TEXT          Added to each name. Default: -dlss.\n"
        L"  --overwrite            Replace results that exist. Default: skip them.\n"
        L"\n"
        L"Batches:\n"
        L"  -r, --recursive        Look inside subfolders of a folder.\n"
        L"  --fail-fast            Stop at the first file that fails or is refused.\n"
        L"  --dry-run              Print what would run, and run nothing.\n"
        L"  --report FILE          Write a JSON summary of every file.\n"
        L"  -q, --quiet            Only each file's result and the summary.\n"
        L"\n"
        L"A folder converts the videos in it (.mp4 .mkv .mov .avi .webm .ts .gif ...)\n"
        L"and leaves out files already carrying the suffix. A named file is always\n"
        L"tried. Wildcards (*.mp4) are expanded here, so they work in cmd.exe too.\n"
        L"\n"
        L"probe prints what a file is and which stages this machine can run on it;\n"
        L"--capabilities also measures frame generation on this GPU.\n"
        L"\n"
        L"Ctrl+C cancels the file being converted and stops the batch.\n"
        L"Exit codes: 0 every file done or skipped, 2 bad arguments, 3 refused,\n"
        L"4 failed, 5 cancelled. With several files, the worst one decides.";
}

} // namespace convert_command
