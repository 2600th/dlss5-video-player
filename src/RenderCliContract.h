#pragma once

#include <algorithm>
#include <iterator>
#include <string_view>

// What `DLSSVideoPlayer.exe --render` (RenderCommandLine.h) and dlss5-convert
// (ConvertCommandLine.h), which runs it once per file, have to agree on: the
// exit codes, and which of the player's options the batch front end passes
// through. Each used to keep its own copy, so an option added to the player
// (--sr-engine) needed the same edit twice, and nothing noticed a miss.
// Adding an option is now one line here plus the player's parsing of its value.
namespace render_cli {

// What the player returns, and what a batch returns for the worst of its
// files. Distinct codes, because a batch script's next step depends on WHICH
// of these happened: a bad argument is the script's bug, a refusal is this
// source or this machine, a failure is worth a retry, and a cancel was
// somebody's decision.
inline constexpr int kExitOk = 0;
inline constexpr int kExitBadArguments = 2;
inline constexpr int kExitRefused = 3;
inline constexpr int kExitFailed = 4;
inline constexpr int kExitCancelled = 5;

// --render options that take a value and that dlss5-convert hands the player
// as given, once each: the player validates them.
inline constexpr std::wstring_view kForwardedValueOptions[] = {
    L"--stages", L"--height", L"--multiplier", L"--preset", L"--processing-scale", L"--range",
    L"--passes", L"--intensity", L"--local-tone", L"--local-structure", L"--color-strength",
    L"--encode", L"--history", L"--sr-engine",
};

// --render flags that dlss5-convert hands the player as given.
inline constexpr std::wstring_view kForwardedFlags[] = {L"--safe-mode"};

// --render options dlss5-convert sets itself for each file rather than passing
// through: the input (--render), where it goes (--out, from --out, --out-dir
// and --format) and --quiet (from its own -q).
inline constexpr std::wstring_view kConvertOwnedValueOptions[] = {L"--render", L"--out"};
inline constexpr std::wstring_view kConvertOwnedFlags[] = {L"--quiet"};

// The player retired --quality for --encode; both front ends say so in these words.
inline constexpr std::wstring_view kRetiredQualityRefusal =
    L"The encode is --encode standard, high or lossless (--quality is an option the player retired).";

inline constexpr bool Listed(std::wstring_view argument, const auto& table)
{
    return std::find(std::begin(table), std::end(table), argument) != std::end(table);
}

inline constexpr bool IsForwardedValueOption(std::wstring_view argument)
{
    return Listed(argument, kForwardedValueOptions);
}

inline constexpr bool IsForwardedFlag(std::wstring_view argument)
{
    return Listed(argument, kForwardedFlags);
}

// Every option --render takes a value for.
inline constexpr bool IsRenderValueOption(std::wstring_view argument)
{
    return IsForwardedValueOption(argument) || Listed(argument, kConvertOwnedValueOptions);
}

} // namespace render_cli
