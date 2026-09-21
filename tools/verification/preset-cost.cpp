// Measures what each named neural preset costs to render.
//
// The project's rule is that a quality choice is an explicit ladder with its
// measured cost printed beside each rung. NeuralPresets.h shipped the ladder
// and said what each rung moves, but not what it costs - so a viewer could
// see that "Strong" raises intensity and local structure and had no way to
// know whether choosing it doubles the render.
//
// The answer was not obvious in either direction. Every preset runs the same
// NGX feature at the same resolution for the same frame count and differs
// only in inference parameters, which argues the cost is identical; but NR
// intensity and colour strength are inputs to the network, and nothing in
// the published interface promises they are free.
//
// So it is measured rather than asserted. For each preset this writes the
// add-on overrides exactly as the player does, runs NeuralRangeRenderSmoke -
// a 640x360 range render with the shipped preroll, which is the same code
// path a live session takes - and reports the wall time it printed. Several
// passes, because a single timing of a six-second render is noise.
//
// Build:
//   cl /nologo /std:c++20 /EHsc /W4 /I src tools\verification\preset-cost.cpp
//      src\ReShadeConfig.cpp src\NeuralSettings.cpp
// Run:
//   preset-cost.exe <NeuralRangeRenderSmoke.exe> <ffmpeg-dir> <NeuralWorker.exe>
//                   <scratch-dir> [passes]

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "NeuralPresets.h"
#include "NeuralSettings.h"
#include "ReShadeConfig.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::wstring Quote(std::wstring_view value)
{
    std::wstring result(1, L'"');
    size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result.push_back(character);
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

// Wall time of one render, or a negative number when it did not complete.
double RunOnce(const fs::path& smoke, const fs::path& ffmpeg, const fs::path& worker,
               const fs::path& scratch)
{
    std::wstring command = Quote(smoke.wstring()) + L" " + Quote(ffmpeg.wstring()) + L" " +
                           Quote(worker.wstring()) + L" " + Quote(scratch.wstring());
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const auto began = std::chrono::steady_clock::now();
    if (!CreateProcessW(smoke.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return -1.0;
    CloseHandle(process.hThread);
    const DWORD waited = WaitForSingleObject(process.hProcess, 600000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    if (waited != WAIT_OBJECT_0) return -1.0;
    // 125 is the no-adapter skip, which is not a measurement.
    if (code != 0) return code == 125 ? -2.0 : -1.0;
    return elapsed;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 5) {
        std::puts("usage: preset-cost <NeuralRangeRenderSmoke.exe> <ffmpeg-dir> "
                  "<NeuralWorker.exe> <scratch-dir> [passes]");
        return 2;
    }
    const fs::path smoke = fs::absolute(argv[1]);
    const fs::path ffmpeg = fs::absolute(argv[2]);
    const fs::path worker = fs::absolute(argv[3]);
    const fs::path scratch = fs::absolute(argv[4]);
    // The add-on configuration lives beside the worker, which is where the
    // player stages the runtime.
    const fs::path runtimeDirectory = worker.parent_path();
    const int passes = argc > 5 ? std::max(1, _wtoi(argv[5])) : 3;

    if (!fs::is_regular_file(smoke)) {
        std::puts("the range-render smoke was not found");
        return 2;
    }
    const fs::path ini = runtimeDirectory / L"ReShade.ini";
    if (!fs::is_regular_file(ini)) {
        std::puts("ReShade.ini was not found in the runtime directory");
        return 2;
    }

    std::printf("%-14s %10s %10s %10s\n", "preset", "best", "median", "worst");
    for (const auto& preset : neural_presets::kPresets) {
        const auto overrides = NeuralAddonOverridesFor(preset.settings);
        // Exactly what the player writes before a job, so the measurement is
        // of the configuration a viewer would actually get.
        if (!ConfigureNeuralAddon(ini, true, overrides).ok) {
            std::printf("%-14s  could not be applied to the add-on\n",
                        std::string(preset.key).c_str());
            continue;
        }

        std::vector<double> times;
        for (int pass = 0; pass < passes; ++pass) {
            // A fresh scratch directory per pass: the smoke reuses a cached
            // source clip but must not reuse a render.
            std::error_code error;
            fs::remove_all(scratch, error);
            const double elapsed = RunOnce(smoke, ffmpeg, worker, scratch);
            if (elapsed == -2.0) { std::puts("no adapter; nothing to measure"); return 125; }
            if (elapsed < 0.0) break;
            times.push_back(elapsed);
        }
        if (times.empty()) {
            std::printf("%-14s  render failed\n", std::string(preset.key).c_str());
            continue;
        }
        std::sort(times.begin(), times.end());
        std::printf("%-14s %9.2fs %9.2fs %9.2fs\n", std::string(preset.key).c_str(),
                    times.front(), times[times.size() / 2], times.back());
    }
    return 0;
}
