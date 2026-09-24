#include "AudioTestGate.h"
#include "TestEnvironment.h"
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
// part. (tools/verification/av-drift-probe.cpp is that larger harness for the
// clock against the audible sound, over minutes rather than seconds.)
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
    test_support::ContainChildProcesses();
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

    // The player finds ffmpeg for itself - next to its own module, then PATH -
    // and this test executable has neither beside it. argv[1] was used to
    // GENERATE the clip and then dropped, so every Start() failed with
    // "Audio: ffmpeg.exe not found." and all eight clock assertions failed on a
    // machine whose audio is fine. Hand the player the same directory the
    // harness was given.
    AudioPlayer::Settings settings;
    settings.helperDirectory = helpers.wstring();
    AudioPlayer audio(std::move(settings));

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

    // ---- device-change service is inert when nothing changed ---------------
    // ServiceDeviceChanges runs on every frame tick. If it ever restarted
    // audio when the endpoint was fine, playback would stutter continuously -
    // a worse bug than the one it fixes. It must report "nothing to do" and
    // leave the clock alone.
    {
        const double before = audio.PositionSeconds();
        bool restarted = false;
        for (int tick = 0; tick < 200; ++tick) restarted = audio.ServiceDeviceChanges() || restarted;
        const double after = audio.PositionSeconds();
        Check(!restarted, "two hundred ticks restart nothing while the endpoint is healthy");
        if (before >= 0.0 && after >= 0.0)
            Check(after >= before, "servicing device changes does not move the clock backwards");
    }

    // ---- device-change recovery, end to end --------------------------------
    // This was the one thing the audio work could not verify: exercising it
    // used to mean changing the machine's default playback endpoint, which is
    // not this harness's to do. The recovery was reasoned, not measured.
    //
    // It is measured now. The renderer's notification handler is the same
    // entry point the OS calls through the registered IMMNotificationClient,
    // so delivering a default-device change to it drives the whole path -
    // latch, ServiceDeviceChanges, restart, clock resuming - without touching
    // anyone's audio settings.
    {
        Check(audio.Seek(8.0), "Seek to a known point before the device change");
        if (WaitForClock(audio, 5s) >= 0.0) {
            const double before = audio.PositionSeconds();
            Check(!audio.ServiceDeviceChanges(),
                  "nothing to service before the endpoint changes");

            // A different endpoint becomes the default for playback.
            Check(audio.DeliverDefaultEndpointChange(L"{0.0.0.00000000}.{not-the-one-we-are-on}"),
                  "the notification reached the running renderer");
            Check(audio.ServiceDeviceChanges(),
                  "a default-endpoint change restarts audio on the new default");
            // And exactly once: a second tick has nothing left to do.
            Check(!audio.ServiceDeviceChanges(),
                  "the restart is not repeated on the next tick");

            const double latency = WaitForClock(audio, 5s);
            Check(latency >= 0.0, "the clock answers again after the endpoint changed");
            std::cout << "device-change recovery latency: " << latency << " s\n";
            if (latency >= 0.0 && before >= 0.0) {
                const double after = audio.PositionSeconds();
                // Resumed where it was, not from the start of the file: the
                // recovery seeks to the last position the clock reported.
                CheckNear(before, after, 1.0,
                          "playback resumes where it was when the endpoint changed");
                std::cout << "position before/after the device change: " << before
                          << " s / " << after << " s\n";
                // And it is really playing, not merely answering.
                const auto mark = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(1000ms);
                const double later = audio.PositionSeconds();
                CheckNear(after + Seconds(mark), later, kFrameSeconds * 2,
                          "the clock advances at real time after the recovery");
            }
        }
    }

    // ---- a restart after a paused seek resumes at the seek -----------------
    // The clock is not read while paused, so the last position it reported
    // is the one from before the seek. A device restart used that and put
    // audio back where the viewer had left, not where they had gone.
    {
        Check(audio.Seek(15.0), "Seek to 15 s before pausing");
        if (WaitForClock(audio, 5s) >= 0.0) {
            std::this_thread::sleep_for(300ms);
            Check(audio.PositionSeconds() >= 15.0, "the clock reads past 15 s before the pause");
            audio.Pause(true);
            // The player's paused seek: a fresh start at the target, paused.
            Check(audio.Start(clip.wstring(), 4.0, AudioStartState::Paused), "paused seek to 4 s");
            Check(audio.DeliverDefaultEndpointChange(L"{0.0.0.00000000}.{not-the-one-we-are-on}"),
                  "the notification reached the paused renderer");
            Check(audio.ServiceDeviceChanges(), "the default-endpoint change restarts paused audio");
            CheckNear(4.0, audio.SeekBaseSeconds(), 1e-9,
                      "the restart resumes at the paused seek, not the last clock reading");
            Check(audio.Paused(), "the restart keeps audio paused");
            audio.Pause(false);
        }
    }

    // ---- the arrival watch that runs while there is no endpoint ------------
    // Unplugging the last endpoint is not this harness's to do, so this
    // proves the watch registers, latches an endpoint that is already there
    // when asked to look, reports it exactly once, and unregisters cleanly.
    {
        RenderEndpointArrival arrival;
        Check(arrival.Watch(true), "the endpoint arrival watch registers");
        Check(arrival.Arrived(), "an endpoint already present is latched when asked to look");
        Check(!arrival.Arrived(), "and reported once, not on every tick");
        RenderEndpointArrival quiet;
        Check(quiet.Watch(false), "a watch that does not look registers too");
        Check(!quiet.Arrived(), "and reports nothing until something arrives");
    }

    // ---- a write refused by a pause is not reported as written -------------
    // A pause that lands between the reader's paused check and its write used
    // to be answered "written", and the reader discarded the chunk: a
    // buffer's worth of the film gone on every such pause.
    {
        WasapiRenderer renderer;
        const bool opened = renderer.Open();
        Check(opened, "a bare renderer opens");
        if (opened) {
            const auto format = renderer.CurrentFormat();
            std::vector<std::byte> silence(size_t(format.BytesPerFrame()) * 16);
            Check(renderer.Start(), "the bare renderer starts");
            Check(renderer.FadeOutAndStop(), "the bare renderer ramps down");
            Check(renderer.Write(silence.data(), 16) == WasapiRenderer::WriteResult::Refused,
                  "a write after the ramp-down is refused, not reported as written");
            Check(renderer.Start(), "the bare renderer starts again");
            uint32_t wanted = 0;
            Check(renderer.WaitForSpace(500, wanted), "the restarted renderer asks for data");
            if (wanted)
                Check(renderer.Write(silence.data(), std::min<uint32_t>(wanted, 16)) ==
                          WasapiRenderer::WriteResult::Written,
                      "after the resume the same frames are taken");
        }
    }

    // ---- stop and seek right after a start --------------------------------
    // Both used to take about 600 ms when they landed in the first moments of
    // playback. The stop ramps the endpoint down, and the ramp waited for room
    // in a buffer the engine had not begun to drain, then for a drain that had
    // not begun, in polls of Sleep(1) and Sleep(2) that each round up to the
    // 15.6 ms system tick: forty of each. A stop or a seek is the viewer's
    // hand on the transport, so it is bounded here, early and settled alike.
    {
        double worstStop = 0.0, worstSeek = 0.0;
        for (int round = 0; round < 4; ++round) {
            Check(audio.Start(clip.wstring(), 5.0), "Start for the early stop");
            std::this_thread::sleep_for(std::chrono::milliseconds(10 * round));
            auto begin = std::chrono::steady_clock::now();
            audio.Stop();
            worstStop = std::max(worstStop, Seconds(begin));
            Check(audio.Start(clip.wstring(), 5.0), "Start for the early seek");
            std::this_thread::sleep_for(std::chrono::milliseconds(10 * round));
            begin = std::chrono::steady_clock::now();
            Check(audio.Seek(9.0), "Seek right after a start");
            worstSeek = std::max(worstSeek, Seconds(begin));
        }
        std::cout << "stop right after start: worst " << worstStop * 1000.0 << " ms\n";
        std::cout << "seek right after start: worst " << worstSeek * 1000.0 << " ms\n";
        Check(worstStop < 0.15, "a stop right after a start returns within 150 ms");
        Check(worstSeek < 0.25, "a seek right after a start returns within 250 ms");
        Check(WaitForClock(audio, 5s) >= 0.0, "the clock answers after the early seek");
        audio.Stop();
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

    // ---- passthrough: the bitstream, or PCM - never silence ----------------
    // After the player above has stopped: an exclusive stream cannot open
    // while anything else in this process holds the endpoint.
    //
    // A machine without an HDMI or S/PDIF receiver refuses the IEC 61937
    // format, and the refusal is the path worth proving here: the film has to
    // come back as PCM with a clock that runs, not as a quiet screen. A
    // machine that accepts it proves the bitstream's clock instead. Either
    // way the answer is printed, because which one ran is the evidence.
    {
        const auto ac3 = root / L"audio-clock-ac3.mkv";
        if (!fs::exists(ac3) &&
            !Generate(helpers / L"ffmpeg.exe",
                      {L"-v", L"error", L"-nostdin", L"-y",
                       L"-f", L"lavfi", L"-i", L"sine=frequency=440:duration=12:sample_rate=48000",
                       L"-ac", L"6", L"-c:a", L"ac3", L"-b:a", L"384k", ac3.wstring()})) {
            Check(false, "the AC-3 source could not be generated");
        } else {
            using audio_passthrough::State;
            const auto describe = [](State state) {
                switch (state) {
                    case State::Off: return "off";
                    case State::NotApplicable: return "not applicable";
                    case State::Active: return "active (bitstream to the receiver)";
                    case State::Refused: return "refused by the endpoint (PCM fallback)";
                    case State::Unavailable: return "exclusive mode unavailable (PCM fallback)";
                }
                return "?";
            };
            AudioPlayer::Settings passthroughSettings;
            passthroughSettings.helperDirectory = helpers.wstring();
            AudioPlayer pass(std::move(passthroughSettings));
            pass.SetPassthrough(true);
            Check(pass.Start(ac3.wstring(), 0.0), "an AC-3 source starts with passthrough on");
            const auto status = pass.PassthroughStatus();
            std::cout << "passthrough on this endpoint: " << describe(status.state) << '\n';
            Check(status.codec == audio_passthrough::Codec::Ac3, "the track is recognised as AC-3");
            Check(status.state == State::Active || status.state == State::Refused ||
                      status.state == State::Unavailable,
                  "an AC-3 track is offered to the endpoint as a bitstream");
            const double latency = WaitForClock(pass, 5s);
            Check(latency >= 0.0, "the clock answers whether the bitstream went out or PCM replaced it");
            std::cout << "passthrough start latency: " << latency << " s\n";
            if (latency >= 0.0) {
                const auto mark = std::chrono::steady_clock::now();
                const double atMark = pass.PositionSeconds();
                std::this_thread::sleep_for(1500ms);
                CheckNear(atMark + Seconds(mark), pass.PositionSeconds(), kFrameSeconds * 2,
                          "the clock advances at real time on the passthrough path");
            }
            if (status.state != State::Active)
                Check(pass.SubmittedBuffers() > 0, "the PCM fallback is writing to the endpoint, not silent");

            // mpv #1773: IAudioClient::Release can hang after a format change.
            // Every one of these swaps the endpoint between the IEC 61937
            // format (or the attempt at it) and float PCM, and each has to
            // come back in bounded time with a clock.
            for (int round = 0; round < 4; ++round) {
                pass.SetPassthrough(round % 2 == 1);
                const auto started = std::chrono::steady_clock::now();
                Check(pass.Seek(2.0 + round), "a restart across a format change succeeds");
                const double took = Seconds(started);
                std::cout << "format-change restart " << round << ": " << took << " s\n";
                Check(took < 3.0, "a restart across a format change is bounded");
                Check(WaitForClock(pass, 5s) >= 0.0, "the clock answers after the format change");
            }

            // A track that is not AC-3, E-AC-3 or DTS says so and plays PCM.
            pass.SetPassthrough(true);
            Check(pass.Start(clip.wstring(), 0.0), "an AAC source starts with passthrough on");
            Check(pass.PassthroughStatus().state == State::NotApplicable,
                  "an AAC track is not passed through");
            Check(pass.PassthroughStatus().trackCodec == "aac", "and the status says what it is");
            Check(WaitForClock(pass, 5s) >= 0.0, "the AAC track plays as PCM");
            pass.Stop();
        }
    }

    // ---- the exclusive stream's own mechanics, on 16-bit PCM ---------------
    // Without a receiver the bitstream path above never gets past the
    // refusal, which leaves the exclusive stream itself - primed before it
    // starts, one whole buffer per event, null data kept out of the clock, a
    // bounded release - unexercised. The same OpenPassthrough carries plain
    // 16-bit stereo PCM, which nearly every endpoint takes exclusively, so
    // that is run here through the renderer the passthrough path uses. The
    // bursts are the only thing it does not send.
    {
        audio_passthrough::Link pcm;
        pcm.subFormat = KSDATAFORMAT_SUBTYPE_PCM;
        pcm.transportRate = 48000;
        pcm.encodedRate = 48000;
        pcm.encodedChannels = 2;
        auto exclusive = std::make_unique<WasapiRenderer>();
        const auto opened = exclusive->OpenPassthrough(pcm);
        if (opened != WasapiRenderer::PassthroughOpen::Opened) {
            std::cout << "exclusive 16-bit PCM refused by this endpoint; its mechanics are not measured\n";
        } else {
            Check(exclusive->Exclusive(), "the renderer reports an exclusive stream");
            const uint32_t rate = exclusive->CurrentFormat().sampleRate;
            Check(exclusive->Start(), "an exclusive start is accepted before the first buffer");
            uint64_t played = 0;
            Check(exclusive->PlayedFrames(played) && played == 0,
                  "an unprimed exclusive stream has not started its clock");

            // A quiet tone, then a gap the source "had nothing" for, then the
            // tone again. The gap goes out as null data and must not count.
            constexpr double kTone = 0.6, kGap = 0.4, kAfter = 0.6;
            std::vector<int16_t> block;
            uint64_t film = 0, highest = 0;
            bool backwards = false;
            double fillerSeconds = 0.0;
            std::chrono::steady_clock::time_point started{};
            bool primed = false;
            for (;;) {
                const double elapsed = primed ? Seconds(started) : 0.0;
                if (elapsed >= kTone + kGap + kAfter) break;
                uint32_t wanted = 0;
                if (!exclusive->WaitForSpace(200, wanted)) { Check(false, "the exclusive stream keeps asking for data"); break; }
                if (!wanted) continue;
                const bool gap = primed && elapsed >= kTone && elapsed < kTone + kGap;
                if (gap) {
                    Check(exclusive->WriteFiller() == WasapiRenderer::WriteResult::Written,
                          "null data is taken for an event with nothing to give");
                    fillerSeconds += double(wanted) / double(rate);
                } else {
                    block.assign(size_t(wanted) * 2, 0);
                    for (uint32_t frame = 0; frame < wanted; ++frame) {
                        const double phase = 2.0 * 3.14159265358979323846 * 440.0 * double(film + frame) / double(rate);
                        const auto sample = int16_t(std::lround(1600.0 * std::sin(phase)));
                        block[size_t(frame) * 2] = sample;
                        block[size_t(frame) * 2 + 1] = sample;
                    }
                    Check(exclusive->Write(block.data(), wanted) == WasapiRenderer::WriteResult::Written,
                          "a whole buffer of film is taken");
                    film += wanted;
                    if (!primed) { primed = true; started = std::chrono::steady_clock::now(); }
                }
                if (exclusive->PlayedFrames(played)) {
                    if (played < highest) backwards = true;
                    highest = std::max(highest, played);
                }
            }
            Check(!backwards, "the exclusive clock never steps back across null data");
            const double wall = Seconds(started);
            Check(exclusive->PlayedFrames(played), "the exclusive clock answers");
            const double clock = double(played) / double(rate);
            std::cout << "exclusive PCM: " << wall << " s of wall time, " << fillerSeconds
                      << " s of null data, clock " << clock << " s\n";
            // Two buffers are queued ahead of the device at any moment, so the
            // clock trails the wall by up to that; the null data is not film.
            CheckNear(wall - fillerSeconds, clock, 0.05,
                      "the exclusive clock counts film and leaves the null data out");
            Check(clock <= double(film) / double(rate) + 1e-9, "the clock never runs past what was written");

            // Pause and resume, as the player does it.
            Check(exclusive->FadeOutAndStop(), "an exclusive stream stops without a ramp");
            std::this_thread::sleep_for(300ms);
            uint64_t stoppedAt = 0, stoppedLater = 0;
            exclusive->PlayedFrames(stoppedAt);
            std::this_thread::sleep_for(300ms);
            exclusive->PlayedFrames(stoppedLater);
            Check(stoppedAt == stoppedLater, "the exclusive clock holds still while stopped");
            Check(exclusive->Write(block.data(), 16) == WasapiRenderer::WriteResult::Refused,
                  "writes are refused while an exclusive stream is stopped");
            Check(exclusive->Start(), "the exclusive stream resumes");

            // mpv #1773: the release is bounded whatever the driver does.
            const auto closing = std::chrono::steady_clock::now();
            exclusive.reset();
            const double closeSeconds = Seconds(closing);
            std::cout << "exclusive close: " << closeSeconds << " s\n";
            Check(closeSeconds < 2.5, "closing an exclusive stream is bounded");
            // And the endpoint is free again for the shared stream after it.
            WasapiRenderer shared;
            Check(shared.Open(), "the shared stream opens after the exclusive one closed");
        }
    }

    CoUninitialize();
    if (failures) {
        std::cerr << failures << " audio clock assertion(s) failed\n";
        return 1;
    }
    std::cout << "AudioClockSmoke: all assertions passed\n";
    return 0;
}
