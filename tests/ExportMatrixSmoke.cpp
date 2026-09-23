// Every combination of the three export stages, on one short clip, end to end.
//
// The stages are DLSS Super Resolution, neural rendering and frame generation.
// They are separately tested elsewhere; what nothing tested before this is that
// they COMPOSE - that a file which has been upscaled can then be neural
// rendered, and that the result can then be frame generated, with the geometry
// and the frame count coming out the far end intact.
//
// Order is NVIDIA's, not ours. Their DLSS 5 neural rendering "normally runs
// last, on the fully upscaled frame"; the community Neural Upstream mod moves
// it earlier precisely because that is faster, which makes early the deviation
// and late the reference. Streamline's own guides put Super Resolution "before
// all other post-processing" and hand DLSS-G the final post-processed colour
// buffer, so frame generation is last. Hence Upscale -> Neural -> FrameGen.
//
// Super Resolution and the neural pass are one worker job: the renderer has
// always taken a source size and an output size separately, and RenoDX's
// NRPreUpscale defaults to 0 - neural AFTER the upscale - so handing the job a
// larger output size puts the two in the stock order inside a single pass.
//
// 720p30 for 3.5 s is deliberate: big enough that a pass-through result cannot
// be mistaken for a render, small enough that the whole matrix is a minute of
// GPU time rather than an afternoon.

#include "FrameGenerationPass.h"
#include "GpuTestGate.h"
#include "MediaPipeline.h"
#include "TestEnvironment.h"
#include "NeuralWorker.h"
#include "OfflineNeuralRenderer.h"
#include "UpscalingPolicy.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

constexpr uint32_t kSourceWidth = 1280;
constexpr uint32_t kSourceHeight = 720;
constexpr double kFps = 30.0;
constexpr double kSeconds = 3.5;
constexpr uint64_t kExpectedFrames = 105;   // 3.5 s at 30 fps
// One rung up from 720p on the player's own ladder, so the target is the number
// the shipping UI would pick rather than one invented for a test.
constexpr uint32_t kTargetHeight = 1440;

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

bool RunFfmpeg(const fs::path& ffmpeg, const std::vector<std::wstring>& arguments, const fs::path& log)
{
    std::wstring command = Quote(ffmpeg.wstring());
    for (const auto& argument : arguments) command += L" " + Quote(argument);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return false;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output; startup.hStdError = output;
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(ffmpeg.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
    CloseHandle(output);
    if (!started) return false;
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 120000);
    if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 124); WaitForSingleObject(process.hProcess, 2000); }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && code == 0;
}

int failures = 0;
void Check(bool condition, const std::wstring& what)
{
    if (condition) { std::wcout << L"  PASS  " << what << L'\n'; return; }
    ++failures;
    std::wcerr << L"  FAIL  " << what << L'\n';
}

struct StageOutcome {
    bool ok{};
    uint32_t width{}, height{};
    uint64_t frames{};
    // What the helper said it rendered, which the parent has already held to
    // what the job asked for.
    bool neural{};
    uint64_t verifiedNeuralFrames{};
    bool armedBeforeCapture{};
};

// One worker job: Super Resolution when `upscale`, the neural pass when
// `neural`, and both together in the stock order when both are asked for.
StageOutcome RunWorkerStage(const fs::path& worker, const fs::path& source, const fs::path& output,
                            uint32_t sourceWidth, uint32_t sourceHeight, bool upscale, bool neural)
{
    NeuralRenderRequest request{nullptr, source, output, sourceWidth, sourceHeight};
    request.fps = kFps;
    request.durationSeconds = kSeconds;
    request.requireNeural = neural;
    if (upscale) {
        const UpscalingSize target = UpscalingTarget(sourceWidth, sourceHeight, kTargetHeight);
        if (!target.grows) return {};
        request.outputWidth = target.width;
        request.outputHeight = target.height;
    }
    const NeuralRenderResult result = RunNeuralWorker(worker, request);
    if (!result.ok) {
        std::wcerr << L"        worker detail: " << result.detail << L'\n';
        return {};
    }
    return {true,
            request.outputWidth ? request.outputWidth : sourceWidth,
            request.outputHeight ? request.outputHeight : sourceHeight,
            result.frameCount, result.neural, result.verifiedNeuralFrames,
            result.feature18ArmedBeforeCapture};
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    test_support::ContainChildProcesses();
    if (const int skip = gpu_test_gate::SkipWithoutGpu()) return skip;
    if (argc != 4) {
        std::wcerr << L"Usage: ExportMatrixSmoke <ffmpeg-directory> <NeuralWorker.exe> <output-directory>\n";
        return 2;
    }
    const auto helpers = fs::absolute(argv[1]);
    const auto worker = fs::absolute(argv[2]);
    const auto root = fs::absolute(argv[3]) /
        std::format(L"{:%Y%m%d-%H%M%S}", std::chrono::floor<std::chrono::seconds>(
                                             std::chrono::system_clock::now()));
    if (!fs::is_regular_file(helpers / L"ffmpeg.exe") || !fs::is_regular_file(worker) ||
        !fs::create_directories(root)) {
        std::wcerr << L"FAIL: ffmpeg.exe and NeuralWorker.exe must exist and the run directory must be new.\n";
        return 2;
    }

    // testsrc2 rather than a flat pattern: the models are given interior detail
    // to work on, so a pass-through cannot read as a render. With a tone,
    // because the finished export has to carry it whichever stages ran.
    const auto source = root / L"matrix-source.mp4";
    if (!RunFfmpeg(helpers / L"ffmpeg.exe",
                   {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi", L"-i",
                    std::format(L"testsrc2=s={}x{}:r={}:d={}", kSourceWidth, kSourceHeight,
                                int(kFps), kSeconds),
                    L"-f", L"lavfi", L"-i", std::format(L"sine=frequency=440:duration={}", kSeconds),
                    L"-c:v", L"libx264", L"-pix_fmt", L"yuv420p", L"-c:a", L"aac", source.wstring()},
                   root / L"source-generation.log")) {
        std::wcerr << L"FAIL: could not generate the 720p30 source clip.\n";
        return 2;
    }
    const UpscalingSize target = UpscalingTarget(kSourceWidth, kSourceHeight, kTargetHeight);
    std::wcout << L"source " << kSourceWidth << L'x' << kSourceHeight << L" @ " << kFps
               << L" fps, " << kSeconds << L" s -> upscale target " << target.width << L'x'
               << target.height << L"\nevidence: " << root.wstring() << L"\n\n";

    // --- Stage combinations that go through the worker. -------------------
    std::wcout << L"[N] neural only, native resolution\n";
    const StageOutcome neuralOnly =
        RunWorkerStage(worker, source, root / L"n.mkv", kSourceWidth, kSourceHeight, false, true);
    Check(neuralOnly.ok, L"neural-only render succeeded");
    Check(neuralOnly.frames == kExpectedFrames, L"neural-only wrote every source frame");
    Check(neuralOnly.width == kSourceWidth && neuralOnly.height == kSourceHeight,
          L"neural-only kept the source geometry");

    std::wcout << L"[U] super resolution only\n";
    const StageOutcome upscaleOnly =
        RunWorkerStage(worker, source, root / L"u.mkv", kSourceWidth, kSourceHeight, true, false);
    Check(upscaleOnly.ok, L"upscale-only render succeeded");
    Check(upscaleOnly.frames == kExpectedFrames, L"upscale-only wrote every source frame");
    Check(upscaleOnly.width == target.width && upscaleOnly.height == target.height,
          L"upscale-only reached the target geometry");
    // The evidence says what happened rather than being left blank.
    Check(upscaleOnly.ok && !upscaleOnly.neural && upscaleOnly.verifiedNeuralFrames == 0 &&
              !upscaleOnly.armedBeforeCapture,
          L"upscale-only reports neural=false, verifiedNeuralFrames=0, feature 18 not armed");

    std::wcout << L"[U+N] super resolution then neural, one pass, stock order\n";
    const StageOutcome upscaleNeural =
        RunWorkerStage(worker, source, root / L"un.mkv", kSourceWidth, kSourceHeight, true, true);
    Check(upscaleNeural.ok, L"upscale+neural render succeeded");
    Check(upscaleNeural.frames == kExpectedFrames, L"upscale+neural wrote every source frame");
    Check(upscaleNeural.width == target.width && upscaleNeural.height == target.height,
          L"upscale+neural reached the target geometry");
    Check(upscaleNeural.ok && upscaleNeural.neural &&
              upscaleNeural.verifiedNeuralFrames == upscaleNeural.frames && upscaleNeural.armedBeforeCapture,
          L"upscale+neural reports every frame verified neural, feature 18 armed");

    // The assertion this suite was missing, and the reason it passed 21 checks
    // while the export dialog offered a checkbox that did nothing: geometry and
    // frame count come out identical whether or not the neural pass ran, so
    // only comparing the BYTES can tell the two apart.
    const auto fileSize = [](const fs::path& file) -> uintmax_t {
        std::error_code error;
        const uintmax_t size = fs::file_size(file, error);
        return error ? 0 : size;
    };
    const auto fileBytes = [](const fs::path& file) {
        std::ifstream stream(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };
    if (upscaleOnly.ok && upscaleNeural.ok) {
        const uintmax_t withoutNeural = fileSize(root / L"u.mkv");
        const uintmax_t withNeural = fileSize(root / L"un.mkv");
        std::wcout << L"  u.mkv=" << withoutNeural << L"  un.mkv=" << withNeural << L'\n';
        Check(withoutNeural > 0 && withNeural > 0, L"both renders produced a file");
        // They were byte-identical, at 9,548,373 bytes each, while the helper
        // enabled the add-on for every job, which is why Super Resolution alone
        // was refused. The helper now starts an upscale-only job with the
        // add-on disabled, so the two files must DIFFER: equal again means the
        // neural model ran in a job that asked for Super Resolution alone.
        Check(fileBytes(root / L"u.mkv") != fileBytes(root / L"un.mkv"),
              L"upscale-only and upscale+neural renders differ (requireNeural=false ran without the add-on)");
    }

    // --- Frame generation on top of each, which is the composition claim. --
    const auto generate = [&](const wchar_t* label, const fs::path& input, const fs::path& output,
                              uint32_t width, uint32_t height) {
        std::wcout << label;
        FrameGenerationRequest request{};
        request.source = input;
        // Video only, as the stage export runs it: the last step below
        // carries the original's streams onto every combination alike.
        request.carryStreams = false;
        request.output = output;
        request.multiplier = 2;          // the only multiple an Ada card admits
        FrameGenerationPass pass(helpers);
        std::stop_source stopSource;
        const FrameGenerationResult result = pass.Run(request, stopSource.get_token(), nullptr);
        if (!result.ok) std::wcerr << L"        framegen detail: " << result.detail << L'\n';
        Check(result.ok, std::wstring(L"framegen succeeded on ") + label);
        Check(result.width == width && result.height == height,
              std::wstring(L"framegen preserved geometry on ") + label);
        // 2x one frame per source pair: the last frame has no successor.
        Check(result.framesWritten >= kExpectedFrames * 2 - 2,
              std::wstring(L"framegen roughly doubled the frame count on ") + label);
        return result.ok;
    };

    if (fs::is_regular_file(source))
        generate(L"[F] frame generation only\n", source, root / L"f.mkv", kSourceWidth, kSourceHeight);
    if (neuralOnly.ok)
        generate(L"[N+F] neural then frame generation\n", root / L"n.mkv", root / L"nf.mkv",
                 kSourceWidth, kSourceHeight);
    if (upscaleOnly.ok)
        generate(L"[U+F] super resolution then frame generation\n", root / L"u.mkv",
                 root / L"uf.mkv", target.width, target.height);
    if (upscaleNeural.ok)
        generate(L"[U+N+F] all three, stock order\n", root / L"un.mkv", root / L"unf.mkv",
                 target.width, target.height);

    // --- The last step, on every combination. ------------------------------
    // Exports without frame generation used to be silent: the worker writes
    // its carrier video-only and the export moved it into place. The last
    // step carries the original's audio onto whatever the passes produced, in
    // the container the name asks for, so audio in must equal audio out for
    // all seven.
    const MediaStreamSummary sourceStreams = SummarizeMediaStreams(helpers, source, {});
    Check(sourceStreams.ok && sourceStreams.audioStreams == 1, L"the source carries one audio stream");
    for (const wchar_t* stem : {L"n", L"u", L"un", L"f", L"nf", L"uf", L"unf"}) {
        const fs::path produced = root / (std::wstring(stem) + L".mkv");
        if (!fs::is_regular_file(produced)) continue;
        for (const wchar_t* extension : {L".mkv", L".mp4"}) {
            const fs::path finished = root / (std::wstring(stem) + L"-final" + extension);
            const MaterializeResult result = MuxStageExport(helpers, {produced, source, finished}, {});
            if (!result.ok) std::wcerr << L"        mux detail: " << result.detail << L'\n';
            const MediaStreamSummary carried = SummarizeMediaStreams(helpers, finished, {});
            Check(result.ok && carried.ok && carried.audioStreams == sourceStreams.audioStreams,
                  std::wstring(L"[") + stem + L"] " + extension + L" carries the source's audio");
        }
    }

    std::wcout << L"\n" << (failures ? L"FAILED" : L"OK") << L": " << failures
               << L" failed check(s). Evidence in " << root.wstring() << L'\n';
    return failures ? 1 : 0;
}
