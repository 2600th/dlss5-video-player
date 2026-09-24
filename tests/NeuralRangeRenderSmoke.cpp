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
#include "ReShadeConfig.h"
#include "UpscalingPolicy.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

// `--processing-scale-cost WxH [seconds]`: the measurement behind the
// processing-scale ladder's printed costs (UpscalingPolicy.h). Renders one
// generated clip whole at every rung, with the add-on order each rung needs,
// and prints the render rate. Not part of the registered smoke, which keeps
// its three arguments: this is minutes of GPU time at 4K.
int MeasureProcessingScale(const fs::path& helpers, const fs::path& worker, const fs::path& root,
                           uint32_t width, uint32_t height, double seconds)
{
    constexpr double kCostFps = 30.0;
    const auto source = root / L"scale-source.mp4";
    if (!Generate(helpers / L"ffmpeg.exe",
                  {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi", L"-i",
                   std::format(L"testsrc2=s={}x{}:r={}:d={}", width, height, int(kCostFps), seconds),
                   L"-c:v", L"libx264", L"-pix_fmt", L"yuv420p", source.wstring()},
                  root / L"source-generation.log")) {
        std::wcerr << L"FAIL: could not generate the " << width << L'x' << height << L" clip.\n";
        return 2;
    }
    const auto ini = worker.parent_path() / L"ReShade.ini";
    int failures = 0;
    for (const uint32_t rung : kProcessingScaleRungs) {
        const std::vector<NeuralAddonOverride> order{{"NRPreUpscale", rung < 100 ? "1" : "0"}};
        if (!ConfigureNeuralAddon(ini, true, order).ok) {
            std::wcerr << L"FAIL: could not write NRPreUpscale for " << rung << L"%\n";
            return 2;
        }
        NeuralRenderRequest request{nullptr, source, root / std::format(L"scale-{}.mkv", rung),
                                    width, height, kCostFps, seconds};
        request.processingScale = rung;
        const auto started = std::chrono::steady_clock::now();
        const NeuralRenderResult result = RunNeuralWorker(worker, request);
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const ProcessingInput input = ProcessingSize(width, height, rung);
        std::wcout << std::format(L"{:>3}% model {}x{}: ok={} frames={} verified={} elapsed={:.2f}s "
                                  L"rate={:.1f} fps neuralGpuMs p50={:.2f} p95={:.2f}\n",
                                  rung, input.width, input.height, result.ok ? 1 : 0, result.frameCount,
                                  result.verifiedNeuralFrames, elapsed,
                                  elapsed > 0.0 ? double(result.frameCount) / elapsed : 0.0,
                                  result.timing.neuralGpuMsP50, result.timing.neuralGpuMsP95);
        if (!result.ok) {
            std::wcerr << L"  detail: " << result.detail << L'\n';
            ++failures;
        }
        for (const auto* name : {L"ReShade.log", L"NeuralWorker.log"}) {
            std::error_code error;
            fs::copy_file(worker.parent_path() / name, root / std::format(L"{}-{}", rung, name),
                          fs::copy_options::overwrite_existing, error);
        }
    }
    // Leave the add-on in the shipped order for whatever runs next.
    ConfigureNeuralAddon(ini, true, std::vector<NeuralAddonOverride>{{"NRPreUpscale", "0"}});
    std::wcout << L"evidence: " << root.wstring() << L'\n';
    return failures ? 1 : 0;
}

// Runs a helper and returns what it wrote to stdout and stderr, or nothing when
// it failed.
std::string CaptureOutput(const fs::path& helper, const std::vector<std::wstring>& arguments, const fs::path& log)
{
    if (!Generate(helper, arguments, log)) return {};
    std::ifstream in(log, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// framemd5 of the first video stream as stored - one line per packet with its
// timestamps, size and MD5, and the codec private data's - without the line
// naming the muxing library.
std::string PacketDigest(const fs::path& helpers, const fs::path& media, const fs::path& log)
{
    const std::string text = CaptureOutput(helpers / L"ffmpeg.exe",
        {L"-v", L"error", L"-nostdin", L"-i", media.wstring(), L"-map", L"0:v:0", L"-c", L"copy",
         L"-f", L"framemd5", L"-"}, log);
    std::string kept;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        const std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start + 1);
        if (line.rfind("#software", 0) != 0) kept += line;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return kept;
}

// The render loop's own throughput, from the stage line the helper logs for its
// last attempt: "measured loop X ms".
double MeasuredLoopMs(const fs::path& workerLog)
{
    std::ifstream log(workerLog);
    std::string line, last;
    while (std::getline(log, line))
        if (line.find("Neural export stage cost per frame") != std::string::npos) last = line;
    const size_t at = last.find("measured loop ");
    return at == std::string::npos ? 0.0 : std::atof(last.c_str() + at + 14);
}

struct CaptureArm {
    const wchar_t* name;
    EncoderQuality quality;
    bool gpuColorConversion;
};

// The rungs the direct path serves: Standard from a GPU-converted NV12 capture and
// High from its P010 one. Standard's default BGRA capture keeps the encoder child.
constexpr CaptureArm kDirectArms[] = {
    {L"standard-nv12", EncoderQuality::Standard, true},
    {L"high-p010", EncoderQuality::High, false},
};

void KeepEvidence(const fs::path& worker, const fs::path& root, const std::wstring& prefix)
{
    for (const auto* name : {L"ReShade.log", L"NeuralWorker.log"}) {
        std::error_code error;
        fs::copy_file(worker.parent_path() / name, root / (prefix + L"-" + name),
                      fs::copy_options::overwrite_existing, error);
    }
}

// `--direct-encode-identity`: P3.7's contract inside a real render. The same clip
// is rendered through feature 18 once with the capture encoded by NVENC straight
// from its D3D12 planes and once through the ffmpeg child, for each rung the
// direct path serves, and the two cache files must hold the same packets byte for
// byte - which is what lets the direct path share the child's cache key. The
// renders themselves are bit-identical run to run, so any difference is the
// encoder's.
int DirectEncodeIdentity(const fs::path& helpers, const fs::path& worker, const fs::path& root)
{
    constexpr uint32_t kIdentityWidth = 1280, kIdentityHeight = 720;
    constexpr double kIdentityFps = 30.0, kIdentitySeconds = 2.0;
    const auto source = root / L"identity-source.mp4";
    if (!Generate(helpers / L"ffmpeg.exe",
                  {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi", L"-i",
                   std::format(L"testsrc2=s={}x{}:r={}:d={}", kIdentityWidth, kIdentityHeight,
                               int(kIdentityFps), kIdentitySeconds),
                   L"-c:v", L"libx264", L"-pix_fmt", L"yuv420p", source.wstring()},
                  root / L"source-generation.log")) {
        std::wcerr << L"FAIL: could not generate the source clip.\n";
        return 2;
    }
    int failures = 0;
    for (const CaptureArm& arm : kDirectArms) {
        std::string digests[2];
        TemporalMetrics metrics[2];
        for (const EncoderPath path : {EncoderPath::Direct, EncoderPath::Ffmpeg}) {
            const std::wstring label = std::wstring(arm.name) + (path == EncoderPath::Direct ? L"-direct" : L"-child");
            NeuralRenderRequest request{nullptr, source, root / (label + L".mkv"), kIdentityWidth, kIdentityHeight,
                                        kIdentityFps, kIdentitySeconds};
            request.quality = arm.quality;
            request.gpuColorConversion = arm.gpuColorConversion;
            request.encoderPath = path;
            const NeuralRenderResult result = RunNeuralWorker(worker, request);
            KeepEvidence(worker, root, label);
            std::wcout << label << L": ok=" << result.ok << L" frames=" << result.frameCount
                       << L" verified=" << result.verifiedNeuralFrames << L'\n';
            if (!result.ok || result.frameCount != uint64_t(kIdentityFps * kIdentitySeconds) ||
                result.encoder != EncoderKind::HevcNvenc) {
                std::wcerr << L"FAIL: " << label << L" did not render: " << result.detail << L'\n';
                ++failures;
                continue;
            }
            digests[path == EncoderPath::Direct ? 0 : 1] =
                PacketDigest(helpers, request.stagingVideoPath, root / (label + L".framemd5"));
            metrics[path == EncoderPath::Direct ? 0 : 1] = result.metrics;
        }
        if (digests[0].empty() || digests[0] != digests[1]) {
            std::wcerr << L"FAIL: " << arm.name << L": the direct path's packets differ from the encoder "
                          L"child's; compare the .framemd5 files in " << root.wstring() << L'\n';
            ++failures;
        } else {
            std::wcout << arm.name << L": direct and child packets identical ("
                       << std::count(digests[0].begin(), digests[0].end(), '\n') << L" digest lines)\n";
        }
        // The render report (P2.11) is part of what a direct render must not lose:
        // the direct capture reads back only the rows the report samples, and has
        // to arrive at the child's numbers from them - both rungs measured, the
        // counts equal and every value within float noise of the child's.
        const TemporalMetrics& direct = metrics[0];
        const TemporalMetrics& child = metrics[1];
        const auto close = [](double a, double b) { return std::abs(a - b) <= 1e-6 + 1e-6 * std::abs(b); };
        const bool sameMetrics = direct.Measured() && child.Measured() && direct.frames == child.frames &&
            direct.pairs == child.pairs && direct.shots == child.shots &&
            close(direct.sourceWarpError, child.sourceWarpError) && close(direct.outputWarpError, child.outputWarpError) &&
            close(direct.sourceSigma, child.sourceSigma) && close(direct.outputSigma, child.outputSigma) &&
            close(direct.lumaShift, child.lumaShift) && close(direct.colorDelta, child.colorDelta);
        std::wcout << std::format(L"{}: metrics direct frames={} outputWarp={:.6f} sigma={:.6f} lumaShift={:.6f} "
                                  L"colorDelta={:.6f}; child frames={} outputWarp={:.6f} sigma={:.6f} "
                                  L"lumaShift={:.6f} colorDelta={:.6f}\n",
                                  arm.name, direct.frames, direct.outputWarpError, direct.outputSigma,
                                  direct.lumaShift, direct.colorDelta, child.frames, child.outputWarpError,
                                  child.outputSigma, child.lumaShift, child.colorDelta);
        if (!sameMetrics) {
            std::wcerr << L"FAIL: " << arm.name << L": the render report's metrics differ between the direct "
                          L"path and the encoder child, or one of them measured nothing.\n";
            ++failures;
        }
    }
    std::wcout << L"evidence: " << root.wstring() << L'\n';
    return failures ? 1 : 0;
}

// `--encoder-path-cost WxH [seconds]`: the measurement behind P3.7
// (docs/measurements/nvenc-direct-20260924/). Renders one generated clip whole
// through every capture arm - the default BGRA capture, which always takes the
// child, as the baseline, then each direct-capable rung through the child and
// through NVENC direct - and prints the render rate: wall clock over the whole
// helper run, and the render loop's own steady-state rate from its stage line.
// Not part of the registered smoke: minutes of GPU time at 4K.
int MeasureEncoderPaths(const fs::path& helpers, const fs::path& worker, const fs::path& root,
                        uint32_t width, uint32_t height, double seconds, uint32_t repeats)
{
    constexpr double kCostFps = 30.0;
    const auto source = root / L"cost-source.mp4";
    if (!Generate(helpers / L"ffmpeg.exe",
                  {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi", L"-i",
                   std::format(L"testsrc2=s={}x{}:r={}:d={}", width, height, int(kCostFps), seconds),
                   L"-c:v", L"libx264", L"-pix_fmt", L"yuv420p", source.wstring()},
                  root / L"source-generation.log")) {
        std::wcerr << L"FAIL: could not generate the " << width << L'x' << height << L" clip.\n";
        return 2;
    }
    struct Arm {
        const wchar_t* name;
        EncoderQuality quality;
        bool gpuColorConversion;
        EncoderPath path;
    };
    const Arm arms[] = {
        {L"standard-bgra-child", EncoderQuality::Standard, false, EncoderPath::Ffmpeg},
        {L"standard-nv12-child", EncoderQuality::Standard, true, EncoderPath::Ffmpeg},
        {L"standard-nv12-direct", EncoderQuality::Standard, true, EncoderPath::Direct},
        {L"high-p010-child", EncoderQuality::High, false, EncoderPath::Ffmpeg},
        {L"high-p010-direct", EncoderQuality::High, false, EncoderPath::Direct},
    };
    // CPU time of the helper and every child it starts - the ffmpeg encoder among
    // them - through a job this process joins, less this process's own share.
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job && !AssignProcessToJobObject(job, GetCurrentProcess())) {
        CloseHandle(job);
        return 2;
    }
    const auto cpuSeconds = [job] {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        if (!job || !QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr))
            return 0.0;
        FILETIME created{}, exited{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        const auto ticks = [](const FILETIME& time) {
            return int64_t((uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime);
        };
        return double(info.TotalUserTime.QuadPart + info.TotalKernelTime.QuadPart - ticks(kernel) - ticks(user)) / 1e7;
    };
    int failures = 0;
    for (uint32_t repeat = 0; repeat < repeats; ++repeat) {
        for (const Arm& arm : arms) {
            const std::wstring label = std::format(L"{}-{}", arm.name, repeat + 1);
            NeuralRenderRequest request{nullptr, source, root / (label + L".mkv"), width, height, kCostFps, seconds};
            request.quality = arm.quality;
            request.gpuColorConversion = arm.gpuColorConversion;
            request.encoderPath = arm.path;
            const double cpuBefore = cpuSeconds();
            const auto started = std::chrono::steady_clock::now();
            const NeuralRenderResult result = RunNeuralWorker(worker, request);
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const double cpu = cpuSeconds() - cpuBefore;
            KeepEvidence(worker, root, label);
            const double loopMs = MeasuredLoopMs(root / (label + L"-NeuralWorker.log"));
            std::wcout << std::format(L"{}x{} {}: ok={} frames={} elapsed={:.2f}s wall={:.1f} fps "
                                      L"loop={:.2f} ms ({:.1f} fps) cpu={:.1f}s ({:.1f} ms/frame) "
                                      L"neuralGpuMs p50={:.2f}\n",
                                      width, height, label, result.ok ? 1 : 0, result.frameCount, elapsed,
                                      elapsed > 0.0 ? double(result.frameCount) / elapsed : 0.0, loopMs,
                                      loopMs > 0.0 ? 1000.0 / loopMs : 0.0, cpu,
                                      result.frameCount ? cpu * 1000.0 / double(result.frameCount) : 0.0,
                                      result.timing.neuralGpuMsP50);
            if (!result.ok) {
                std::wcerr << L"  detail: " << result.detail << L'\n';
                ++failures;
            }
            std::error_code ignored;
            fs::remove(request.stagingVideoPath, ignored);
        }
    }
    if (job) CloseHandle(job);
    std::wcout << L"evidence: " << root.wstring() << L'\n';
    return failures ? 1 : 0;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    test_support::ContainChildProcesses();
    if (const int skip = gpu_test_gate::SkipWithoutGpu()) return skip;
    const bool measureScale =
        (argc == 6 || argc == 7) && std::wstring_view(argv[4]) == L"--processing-scale-cost";
    // <ffmpeg-directory> <NeuralWorker.exe> <output-directory> --direct-encode-identity
    const bool directIdentity = argc == 5 && std::wstring_view(argv[4]) == L"--direct-encode-identity";
    // ... --encoder-path-cost WxH [seconds] [repeats]
    const bool measureEncoder =
        (argc >= 6 && argc <= 8) && std::wstring_view(argv[4]) == L"--encoder-path-cost";
    if (argc != 4 && !measureScale && !directIdentity && !measureEncoder) {
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
    if (directIdentity) return DirectEncodeIdentity(helpers, worker, root);
    if (measureEncoder) {
        unsigned width = 0, height = 0;
        if (swscanf_s(argv[5], L"%ux%u", &width, &height) != 2 || !width || !height) return 2;
        const double seconds = argc >= 7 ? _wtof(argv[6]) : 10.0;
        const unsigned repeats = argc == 8 ? unsigned(_wtoi(argv[7])) : 1u;
        return MeasureEncoderPaths(helpers, worker, root, width, height, seconds > 0.0 ? seconds : 10.0,
                                   repeats ? repeats : 1u);
    }
    if (measureScale) {
        unsigned width = 0, height = 0;
        if (swscanf_s(argv[5], L"%ux%u", &width, &height) != 2 || !width || !height) return 2;
        const double seconds = argc == 7 ? _wtof(argv[6]) : 10.0;
        return MeasureProcessingScale(helpers, worker, root, width, height, seconds > 0.0 ? seconds : 10.0);
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
