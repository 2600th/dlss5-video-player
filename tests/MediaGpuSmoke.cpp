// Opt-in real GPU verification; intentionally excluded from portable CTest.
// Usage: MediaGpuSmoke <ffmpeg-directory> <NeuralWorker.exe> <new-output-directory>
#include "MediaPipeline.h"
#include "NeuralWorker.h"
#include "VideoDecoder.h"

#include <windows.h>
#include <mfapi.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;

// Cooperative cancellation normally kills the isolated helper immediately.
// Terminate this opt-in test if a regression ignores cancellation entirely.
class Deadline {
public:
    Deadline() : watchdog_([this] {
        std::unique_lock lock(mutex_);
        if (cv_.wait_for(lock, 120s, [this] { return finished_; })) return;
        stop_.request_stop();
        if (cv_.wait_for(lock, 10s, [this] { return finished_; })) return;
        std::wcerr << L"FAIL: cancellation did not finish within the watchdog grace period.\n";
        TerminateProcess(GetCurrentProcess(), 124);
    }) {}
    ~Deadline() {
        { std::lock_guard lock(mutex_); finished_ = true; }
        cv_.notify_one();
    }
    std::stop_token Token() const { return stop_.get_token(); }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::stop_source stop_;
    bool finished_{};
    std::jthread watchdog_; // Destroyed first, while synchronization fields live.
};

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

bool Generate(const fs::path& helper, const std::vector<std::wstring>& args, const fs::path& log)
{
    std::wstring command = Quote(helper.wstring());
    for (const auto& arg : args) command += L" " + Quote(arg);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return false;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output; startup.hStdError = output;
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
    CloseHandle(output);
    if (!started) return false;
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 2000);
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && code == 0;
}

bool Check(bool condition, std::wstring_view detail, std::wofstream& report)
{
    if (!condition) {
        std::wcerr << L"FAIL: " << detail << L'\n';
        report << L"FAIL: " << detail << std::endl;
    }
    return condition;
}

bool Decode(const fs::path& media, uint32_t width, uint32_t height,
            uint64_t expectedFrames, bool requireMotion, std::stop_token stop, std::wofstream& report)
{
    VideoDecoder decoder;
    if (!Check(decoder.OpenSequential(media.wstring(), MediaSourceKind::LocalFile, stop),
        L"Decoder could not open " + media.wstring(), report)) return false;
    bool ok = Check(decoder.Width() == width && decoder.Height() == height,
        L"Export dimensions differ from the source", report);
    uint64_t frames = 0;
    std::vector<uint8_t> firstPixels;
    bool changedPixels = false;
    VideoFrame frame;
    while (!stop.stop_requested()) {
        const auto read = decoder.ReadNextAvailable(frame, stop);
        if (read == VideoReadResult::NotReady) { std::this_thread::sleep_for(1ms); continue; }
        if (read == VideoReadResult::EndOfStream) break;
        if (!Check(read == VideoReadResult::FrameReady, L"Export decode failed", report)) return false;
        if (!Check(frame.bgra.size() == size_t(width) * height * 4,
            L"Export contains an incomplete decoded frame", report)) return false;
        if (frames == 0) firstPixels = frame.bgra;
        else if (frame.bgra != firstPixels) changedPixels = true;
        if (++frames > expectedFrames) break;
    }
    report << L"decoded=" << media.filename().wstring() << L" dimensions=" << width << L'x' << height
           << L" frames=" << frames << L" fps=" << decoder.FrameRate()
           << L" duration=" << decoder.DurationSeconds() << L" changed_pixels=" << changedPixels << std::endl;
    return Check(!stop.stop_requested() && frames == expectedFrames,
        L"Unexpected decoded export frame count or timeout", report) &&
        Check(!requireMotion || changedPixels, L"Moving source became a frozen export", report) && ok;
}

bool VerifyInput(const fs::path& helpers, const fs::path& worker, const fs::path& source,
                 const fs::path& outputDirectory, std::wofstream& report)
{
    Deadline deadline;
    const auto stop = deadline.Token();
    VideoDecoder decoder;
    if (!Check(decoder.OpenSequential(source.wstring(), MediaSourceKind::LocalFile, stop),
        L"Source decoder could not open " + source.wstring(), report)) return false;
    const auto width = decoder.Width(), height = decoder.Height();
    const auto fps = decoder.FrameRate(), duration = decoder.DurationSeconds();
    const bool still = decoder.IsStillImage();
    decoder.Close();
    const uint64_t expectedFrames = static_cast<uint64_t>(std::llround(fps * duration));
    report << L"input=" << source.wstring() << L" dimensions=" << width << L'x' << height
           << L" fps=" << fps << L" duration=" << duration << L" expected_frames=" << expectedFrames << std::endl;
    const auto cache = outputDirectory / (source.stem().wstring() + L"-neural.mkv");
    const NeuralRenderRequest request{nullptr, source, cache, width, height, fps, duration};
    const auto start = std::chrono::steady_clock::now();
    const auto result = RunNeuralWorker(worker, request, {}, stop);
    // The next worker replaces its logs. Preserve the evidence for every input,
    // including rejected jobs, before starting another process.
    for (const auto* name : {L"ReShade.log", L"DLSSVideoPlayer.log"}) {
        std::error_code error;
        const auto log = worker.parent_path() / name;
        if (fs::is_regular_file(log, error)) {
            fs::copy_file(log, outputDirectory / (source.stem().wstring() + L"-" + name),
                fs::copy_options::none, error);
            if (error) report << L"log_copy_error=" << log.wstring() << L" " << error.value() << std::endl;
        }
    }
    report << L"neural_ok=" << result.ok << L" cancelled=" << result.cancelled
           << L" frames=" << result.frameCount << L" duration_100ns=" << result.duration100ns
           << L" native_evaluations=" << result.nativeEvaluations
           << L" verified_frames=" << result.verifiedNeuralFrames
           << L" feature18_armed=" << result.feature18ArmedBeforeCapture
           << L" evidence_valid=" << result.evidence.Valid()
           << L" highest_evaluation=" << result.evidence.highestObservedEvaluation
           << L" elapsed_seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
           << L" detail=" << result.detail << std::endl;
    std::wcout << source.filename().wstring() << L": neural=" << result.ok << L" frames=" << result.frameCount
               << L" verified=" << result.verifiedNeuralFrames << L" detail=" << result.detail << std::endl;
    if (!Check(result.ok && !result.cancelled && result.frameCount == expectedFrames &&
        result.nativeEvaluations > 0 && result.verifiedNeuralFrames == result.frameCount &&
        result.feature18ArmedBeforeCapture && result.evidence.Valid(), L"Strict neural evidence failed", report)) return false;
    const auto probe = ProbeMedia(helpers, cache, stop);
    if (!Check(probe.ok && probe.decodedFinalFrame && probe.width == width && probe.height == height &&
        probe.frameCount == expectedFrames && std::abs(double(probe.videoDuration100ns) / 1e7 - duration) < 0.03,
        L"Neural cache frame/duration validation failed: " + probe.detail, report)) return false;
    bool ok = true;
    CachedVideoExporter exporter(helpers);
    for (const auto* extension : {L".png", L".jpg", L".gif", L".mp4", L".mkv"}) {
        const auto output = outputDirectory / (source.stem().wstring() + L"-export" + extension);
        const bool image = std::wstring_view(extension) == L".png" || std::wstring_view(extension) == L".jpg";
        const bool gif = std::wstring_view(extension) == L".gif";
        const auto exported = exporter.Run({cache, source, output}, stop);
        if (!Check(exported.ok, L"Export failed: " + output.wstring() + L" " + exported.detail, report)) { ok = false; continue; }
        // GIF decoder expands centisecond timing to 100 fps; image exports are one frame.
        const uint64_t decodedFrames = image ? 1 : gif ? static_cast<uint64_t>(std::llround(duration * 100)) : expectedFrames;
        ok = Decode(output, width, height, decodedFrames, !image && !still, stop, report) && ok;
        if (!image) {
            const auto exportedProbe = ProbeMedia(helpers, output, stop);
            const uint64_t encodedFrames = gif ? static_cast<uint64_t>(std::llround(duration * 50)) : expectedFrames;
            report << L"probe=" << output.filename().wstring() << L" ok=" << exportedProbe.ok
                   << L" frames=" << exportedProbe.frameCount << L" duration_100ns=" << exportedProbe.duration100ns
                   << L" video_duration_100ns=" << exportedProbe.videoDuration100ns << std::endl;
            ok = Check(exportedProbe.ok && exportedProbe.decodedFinalFrame && exportedProbe.frameCount == encodedFrames &&
                std::abs(double(exportedProbe.videoDuration100ns) / 1e7 - duration) < 0.03,
                L"Export frame/duration validation failed: " + exportedProbe.detail, report) && ok;
        }
    }
    return Check(!still || expectedFrames == 1, L"Photo source did not remain a single frame", report) && ok;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 4) {
        std::wcerr << L"Usage: MediaGpuSmoke <ffmpeg-directory> <NeuralWorker.exe> <new-output-directory>\n";
        return 2;
    }
    const auto helpers = fs::absolute(argv[1]), worker = fs::absolute(argv[2]), root = fs::absolute(argv[3]);
    if (!fs::is_regular_file(helpers / L"ffmpeg.exe") || !fs::is_regular_file(helpers / L"ffprobe.exe") ||
        !fs::is_regular_file(worker) || !fs::create_directories(root)) {
        std::wcerr << L"Tools must exist and the output directory must be new.\n"; return 2;
    }
    std::wofstream report(root / L"results.txt");
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 2; }
    const auto photo = root / L"photo.png", gif = root / L"animation.gif", video = root / L"video.mp4";
    const auto ffmpeg = helpers / L"ffmpeg.exe";
    const bool fixtures =
        Generate(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi", L"-i",
            L"testsrc=s=641x359:r=1:d=1", L"-frames:v", L"1", photo.wstring()}, root / L"photo-generation.log") &&
        Generate(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi", L"-i",
            L"testsrc2=s=320x180:r=10:d=1", L"-vf", L"select='eq(n,0)+eq(n,1)+eq(n,4)'",
            L"-fps_mode", L"vfr", L"-final_delay", L"60", gif.wstring()}, root / L"gif-generation.log") &&
        Generate(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi", L"-i",
            L"testsrc2=s=640x360:r=24:d=1", L"-f", L"lavfi", L"-i", L"sine=frequency=440:duration=1",
            L"-c:v", L"libx264", L"-pix_fmt", L"yuv420p", L"-c:a", L"aac", L"-shortest", video.wstring()}, root / L"video-generation.log");
    bool ok = Check(fixtures, L"Fixture generation failed; inspect generation logs", report);
    if (fixtures) for (const auto& source : {photo, gif, video})
        ok = VerifyInput(helpers, worker, source, root, report) && ok;
    report << L"overall=" << (ok ? L"PASS" : L"FAIL") << std::endl;
    MFShutdown(); CoUninitialize();
    std::wcout << L"Evidence: " << root.wstring() << std::endl;
    return ok ? 0 : 1;
}
