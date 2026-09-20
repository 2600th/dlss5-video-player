#include "AudioTestGate.h"
// Opt-in real-device verification; registered under the `audio` CTest label,
// which the portable suite excludes.
//
// Usage: AudioClockSmoke <ffmpeg-directory> <output-directory>
//
// The player is audio-master: PlaybackTiming holds every video frame until the
// audio clock reaches its due time. That clock had one assertion anywhere in
// the suite -
//
//     CHECK_EQ(7.5, AudioPlayerTestAccess::SeekBase(*audio))
//
// - that a number given to Seek was stored. It was never correlated with
// elapsed time, never checked after a seek, never checked across a pause, and
// CONTRIBUTING.md asked contributors to test A/V sync by hand.
//
// The arithmetic is unforgiving: drift is 7.2 ms per ppm over a two-hour film,
// so an ordinary 50 ppm crystal drifts 360 ms - outside ITU-R BT.1359-1's
// +90/-185 ms acceptability window. Nothing here could have noticed.
//
// What this does and does not prove. It asserts the CLOCK contract - that
// PositionSeconds advances at real time, rebases on a seek, holds still while
// paused, and resumes - which is what every frame's due time is computed from.
// It does not prove acoustic sync: that the samples leaving the endpoint match
// the frame on screen would need a loopback capture correlating a tone burst
// against a marked frame, which is a different and much larger harness. The
// clock is the part the player actually controls, and it was the untested
// part.
#include "AudioPlayer.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

int failures = 0;

void Check(bool condition, const std::string& what)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << what << '\n';
}

void CheckNear(double expected, double actual, double tolerance, const std::string& what)
{
    if (std::abs(expected - actual) <= tolerance) return;
    ++failures;
    std::cerr << "FAIL: " << what << " (expected " << expected << " +/- " << tolerance
              << ", actual " << actual << ", off by " << (actual - expected) << ")\n";
}

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

bool Generate(const fs::path& ffmpeg, const std::vector<std::wstring>& arguments)
{
    std::wstring command = Quote(ffmpeg.wstring());
    for (const auto& argument : arguments) command += L" " + Quote(argument);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(ffmpeg.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && code == 0;
}

double Seconds(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
}

// Waits for the clock to start answering. Startup buffering means position is
// -1 for a while after Start; how long is exactly what this measures.
double WaitForClock(const AudioPlayer& audio, std::chrono::milliseconds budget)
{
    const auto started = std::chrono::steady_clock::now();
    while (Seconds(started) < std::chrono::duration<double>(budget).count()) {
        if (audio.PositionSeconds() >= 0.0) return Seconds(started);
        std::this_thread::sleep_for(5ms);
    }
    return -1.0;
}

// One frame at 24 fps. The player's own acceptance bound for a paired frame,
// and therefore the right unit for "the clock is where it should be".
constexpr double kFrameSeconds = 1.0 / 24.0;

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        std::cerr << "FAIL: COM could not start.\n";
        return 2;
    }
    if (const int skip = audio_test_gate::SkipWithoutRenderDevice()) { CoUninitialize(); return skip; }
    if (argc != 3) {
        std::wcerr << L"Usage: AudioClockSmoke <ffmpeg-directory> <output-directory>\n";
        CoUninitialize();
        return 2;
    }
    const auto helpers = fs::absolute(argv[1]);
    const auto root = fs::absolute(argv[2]);
    std::error_code error;
    fs::create_directories(root, error);
    if (!fs::is_regular_file(helpers / L"ffmpeg.exe")) {
        std::wcerr << L"FAIL: ffmpeg.exe must be staged at " << helpers.wstring() << L'\n';
        CoUninitialize();
        return 2;
    }

    // Thirty seconds, so a seek to 20 s still has room to play afterwards.
    const auto clip = root / L"audio-clock-source.mkv";
    if (!fs::exists(clip) &&
        !Generate(helpers / L"ffmpeg.exe",
                  {L"-v", L"error", L"-nostdin", L"-y",
                   L"-f", L"lavfi", L"-i", L"testsrc2=s=320x180:r=24:d=30",
                   L"-f", L"lavfi", L"-i", L"sine=frequency=1000:duration=30",
                   L"-c:v", L"libx264", L"-pix_fmt", L"yuv420p", L"-c:a", L"aac",
                   L"-shortest", clip.wstring()})) {
        std::wcerr << L"FAIL: could not generate the source clip.\n";
        CoUninitialize();
        return 2;
    }

    AudioPlayer audio;

    // ---- start -------------------------------------------------------------
    Check(audio.Start(clip.wstring(), 0.0), "Start from zero");
    const double startLatency = WaitForClock(audio, 5s);
    Check(startLatency >= 0.0, "the clock answers within five seconds of Start");
    std::cout << "start latency: " << startLatency << " s\n";
    if (startLatency >= 0.0) {
        const auto mark = std::chrono::steady_clock::now();
        const double atMark = audio.PositionSeconds();
        std::this_thread::sleep_for(1500ms);
        const double later = audio.PositionSeconds();
        CheckNear(atMark + Seconds(mark), later, kFrameSeconds,
                  "the clock advances at real time from a standing start");
    }

    // ---- forward seek ------------------------------------------------------
    Check(audio.Seek(20.0), "Seek forward to 20 s");
    const double forwardSeekLatency = WaitForClock(audio, 5s);
    Check(forwardSeekLatency >= 0.0, "the clock answers within five seconds of a forward seek");
    std::cout << "forward seek latency: " << forwardSeekLatency << " s\n";
    if (forwardSeekLatency >= 0.0) {
        const double position = audio.PositionSeconds();
        // At least the target, and no more than the target plus however long
        // it took to answer: a clock that ignored the seek reads far below,
        // one that rebased wrongly reads far above.
        Check(position >= 20.0 - kFrameSeconds,
              "the clock is at or past the forward seek target");
        Check(position <= 20.0 + forwardSeekLatency + 1.0,
              "the clock has not run away past the forward seek target");
        std::cout << "position after forward seek: " << position << " s\n";
    }

    // ---- backward seek -----------------------------------------------------
    Check(audio.Seek(3.0), "Seek backward to 3 s");
    const double backwardSeekLatency = WaitForClock(audio, 5s);
    Check(backwardSeekLatency >= 0.0, "the clock answers within five seconds of a backward seek");
    std::cout << "backward seek latency: " << backwardSeekLatency << " s\n";
    if (backwardSeekLatency >= 0.0) {
        const double position = audio.PositionSeconds();
        Check(position >= 3.0 - kFrameSeconds, "the clock is at or past the backward seek target");
        // The one that catches a clock which never rebased: it would still
        // read around 20 s.
        Check(position < 10.0, "the clock went backwards with the seek");
        std::cout << "position after backward seek: " << position << " s\n";
    }

    // ---- pause and resume --------------------------------------------------
    audio.Pause(true);
    // waveOutPause stops the device but buffers already queued may still
    // report for a moment; settle before sampling.
    std::this_thread::sleep_for(300ms);
    const double paused = audio.PositionSeconds();
    std::this_thread::sleep_for(1200ms);
    const double stillPaused = audio.PositionSeconds();
    if (paused >= 0.0 && stillPaused >= 0.0)
        CheckNear(paused, stillPaused, kFrameSeconds, "the clock holds still while paused");

    audio.Pause(false);
    const auto resumed = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(1200ms);
    const double afterResume = audio.PositionSeconds();
    if (stillPaused >= 0.0 && afterResume >= 0.0)
        CheckNear(stillPaused + Seconds(resumed), afterResume, kFrameSeconds * 2,
                  "the clock resumes from where it paused");

    // ---- drift over a longer window ---------------------------------------
    const auto driftStart = std::chrono::steady_clock::now();
    const double driftFrom = audio.PositionSeconds();
    std::this_thread::sleep_for(5s);
    const double driftTo = audio.PositionSeconds();
    if (driftFrom >= 0.0 && driftTo >= 0.0) {
        const double elapsed = Seconds(driftStart);
        std::cout << "drift over " << elapsed << " s: "
                  << ((driftTo - driftFrom) - elapsed) * 1000.0 << " ms\n";
        CheckNear(elapsed, driftTo - driftFrom, kFrameSeconds,
                  "the clock does not drift from real time over five seconds");
    }

    // ---- end of stream -----------------------------------------------------
    // The clock must never run backwards as the queue drains. Resetting the
    // device at EOF would snap the played-sample count to zero and make the
    // audio-master clock jump back during the last frames of a film, which is
    // why the drain at the end of the render loop exists.
    Check(audio.Seek(28.0), "Seek near the end");
    if (WaitForClock(audio, 5s) >= 0.0) {
        double highest = audio.PositionSeconds();
        bool wentBackwards = false;
        const auto watching = std::chrono::steady_clock::now();
        while (Seconds(watching) < 5.0) {
            const double now = audio.PositionSeconds();
            if (now >= 0.0) {
                if (now + kFrameSeconds < highest) wentBackwards = true;
                highest = std::max(highest, now);
            }
            std::this_thread::sleep_for(20ms);
        }
        Check(!wentBackwards, "the clock never runs backwards while the stream ends");
        std::cout << "highest position at end of stream: " << highest << " s\n";
    }

    audio.Stop();
    Check(audio.PositionSeconds() < 0.0, "a stopped player reports no clock");

    CoUninitialize();
    if (failures) {
        std::cerr << failures << " audio clock assertion(s) failed\n";
        return 1;
    }
    std::cout << "AudioClockSmoke: all assertions passed\n";
    return 0;
}
