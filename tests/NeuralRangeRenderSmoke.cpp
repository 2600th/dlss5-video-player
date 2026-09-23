#include "GpuTestGate.h"
#include "TestEnvironment.h"
// Opt-in real GPU verification; registered under the `gpu` CTest label, which
// the portable suite excludes (`ctest -LE gpu`).
//
// Usage: NeuralRangeRenderSmoke <ffmpeg-directory> <NeuralWorker.exe>
//                               <output-directory>
//
// Renders a RANGE of a clip, which is the one shape of neural render that had
// no automated coverage and the one every live session performs.
//
// Why it exists
// -------------
// fd9279b added DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT to the
// swapchain and stopped neural rendering producing a single frame. All 21
// tests stayed green - MediaGpuSmoke included, which already renders through
// feature 18 and already asserts on the runtime evidence - and it shipped.
//
// The gap was found by putting the flag back and bisecting the difference
// between what the suite rendered and what the user did. It is not size:
// re-running at 2560x1440 for 120 frames still passed. It is not GPU source
// conversion, and it is not segmented output. It is the range.
//
// A whole-source render captures every frame it evaluates, and each capture's
// readback fence retires that submission. The add-on recycles its NR worksets
// on exactly those fences - "GPU-safe NR workset pool active: up to 4 scratch
// generations; exact queue fences recycle only completed submissions" - so a
// whole-source render can never build up more than one unretired generation
// and the pool is never under pressure.
//
// A range render begins with preroll: kDefaultPrerollFrames frames evaluated
// and deliberately NOT captured, so the temporal history at the range start
// matches a continuous render. Nothing reads those back, nothing retires them,
// and the four generations are gone before the first captured frame:
//
//   NR workset pool exhausted; preserving game output for this evaluation
//
// after which every frame passes through untouched, the helper reports
// frames=0/0, and the player refuses the render with "A frame was not produced
// by feature 18".
//
// Live playback always renders a range - it starts at the playhead - so this
// was the whole feature, while every GPU test in the suite rendered from zero.
//
// Kept small on purpose. The trigger is the preroll, not the pixel count:
// verified to reproduce identically at 640x360 and at 2560x1440, so this runs
// at the small size and finishes in seconds.
#include "NeuralWorker.h"
#include "OfflineNeuralRenderer.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 360;
constexpr double kFps = 24.0;
constexpr double kSourceSeconds = 6.0;
// Starts well past zero so the render is a genuine range with a full preroll
// behind it, and runs long enough that a pass-through result cannot coincide
// with the expected count.
constexpr int64_t kRangeStart100ns = 10'000'000;   // 1.0 s
constexpr int64_t kRangeEnd100ns = 40'000'000;     // 4.0 s
constexpr uint64_t kExpectedFrames = 72;           // [1,4) s at 24 fps

// CommandLineToArgvW's quoting rules, so a path with a space or a backslash
// run survives the round trip into the child's argv.
std::wstring Quote(std::wstring_view value)
{
    std::wstring result(1, L'"');
    size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result.push_back(character); slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

// CreateProcessW rather than _wsystem: cmd.exe needs the whole command line
// wrapped again when the program path is already quoted, and getting that
// wrong fails as "is not recognized as an internal or external command".
bool Generate(const fs::path& ffmpeg, const std::vector<std::wstring>& arguments,
              const fs::path& log)
{
    std::wstring command = Quote(ffmpeg.wstring());
    for (const auto& argument : arguments) command += L" " + Quote(argument);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return false;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = output;
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(ffmpeg.c_str(), command.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                        &startup, &process) != FALSE;
    CloseHandle(output);
    if (!started) return false;
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 60000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 2000);
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && code == 0;
}

// The add-on names this failure in its own log. Without it, an exhausted pool
// reaches the reader as frames=0 with no cause - which is what made the
// original regression cost a bisect to diagnose.
std::wstring WorksetPoolComplaint(const fs::path& reshadeLog)
{
    std::ifstream log(reshadeLog);
    if (!log) return {};
    std::string line;
    while (std::getline(log, line)) {
        const size_t found = line.find("workset pool exhausted");
        if (found == std::string::npos) continue;
        const size_t start = line.find("DLSS5 Generic:");
        const std::string fragment =
            line.substr(start == std::string::npos ? found : start, 160);
        return std::wstring(fragment.begin(), fragment.end());
    }
    return {};
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    test_support::ContainChildProcesses();
    if (const int skip = gpu_test_gate::SkipWithoutGpu()) return skip;
    if (argc != 4) {
        std::wcerr << L"Usage: NeuralRangeRenderSmoke <ffmpeg-directory> "
                      L"<NeuralWorker.exe> <output-directory>\n";
        return 2;
    }
    const auto helpers = fs::absolute(argv[1]);
    const auto worker = fs::absolute(argv[2]);
    const auto root = fs::absolute(argv[3]) /
        std::format(L"{:%Y%m%d-%H%M%S}",
                    std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    if (!fs::is_regular_file(helpers / L"ffmpeg.exe") || !fs::is_regular_file(worker) ||
        !fs::create_directories(root)) {
        std::wcerr << L"FAIL: ffmpeg.exe and NeuralWorker.exe must exist and the run "
                      L"directory must be new.\n";
        return 2;
    }

    // testsrc2 rather than a flat pattern: the model is given interior detail
    // to work on, so a pass-through result cannot read as a render.
    const auto source = root / L"range-source.mp4";
    if (!Generate(helpers / L"ffmpeg.exe",
                  {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi", L"-i",
                   std::format(L"testsrc2=s={}x{}:r={}:d={}", kWidth, kHeight,
                               int(kFps), kSourceSeconds),
                   L"-c:v", L"libx264", L"-pix_fmt", L"yuv420p", source.wstring()},
                  root / L"source-generation.log")) {
        std::wcerr << L"FAIL: could not generate the source clip; see "
                   << (root / L"source-generation.log").wstring() << L'\n';
        return 2;
    }

    const auto output = root / L"range-neural.mkv";
    NeuralRenderRequest request{nullptr, source, output, kWidth, kHeight, kFps, kSourceSeconds};
    request.range = NeuralRenderRange{kRangeStart100ns, kRangeEnd100ns};
    // Left at the shipped default rather than lowered: the default is what
    // live playback uses, and the preroll is the thing under test.
    request.prerollFrames = kDefaultPrerollFrames;

    const auto started = std::chrono::steady_clock::now();
    const NeuralRenderResult result = RunNeuralWorker(worker, request);
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    // Preserve the evidence before the next worker launch truncates it.
    for (const auto* name : {L"ReShade.log", L"NeuralWorker.log", L"DLSSVideoPlayer.log"}) {
        std::error_code error;
        const auto original = worker.parent_path() / name;
        if (fs::is_regular_file(original, error))
            fs::copy_file(original, root / name, fs::copy_options::overwrite_existing, error);
    }

    std::wcout << L"range render [" << kRangeStart100ns / 1e7 << L',' << kRangeEnd100ns / 1e7
               << L") s: ok=" << result.ok
               << L" frames=" << result.frameCount << L'/' << kExpectedFrames
               << L" verified=" << result.verifiedNeuralFrames
               << L" native_evaluations=" << result.nativeEvaluations
               << L" feature18_armed=" << result.feature18ArmedBeforeCapture
               << L" evidence_valid=" << result.evidence.Valid()
               << L" preroll=" << request.prerollFrames
               << L" elapsed=" << elapsed << L"s\n"
               << L"evidence: " << root.wstring() << L'\n';

    if (result.ok && !result.cancelled && result.frameCount == kExpectedFrames &&
        result.nativeEvaluations > 0 && result.verifiedNeuralFrames == result.frameCount &&
        result.feature18ArmedBeforeCapture && result.evidence.Valid())
        return 0;

    std::wcerr << L"FAIL: the range render did not produce " << kExpectedFrames
               << L" verified feature-18 frames. detail=" << result.detail << L'\n';
    if (const std::wstring complaint = WorksetPoolComplaint(root / L"ReShade.log");
        !complaint.empty())
        std::wcerr << L"CAUSE: " << complaint << L"\n"
                   << L"       That is the fd9279b failure. Check "
                      L"d3d12_renderer_detail::SwapchainFlags - "
                      L"DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT must not be set.\n";
    return 1;
}
