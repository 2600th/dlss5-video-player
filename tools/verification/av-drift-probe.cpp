// Measures, over a long run, whether the audio master clock stays locked to
// the sound actually leaving the endpoint - the one way an audio-mastered
// player can drift A/V.
//
// Why this exists. P3.3 asked for swr_set_compensation-style drift correction
// "against the IAudioClock error". The player is audio-mastered: every video
// frame is held until AudioPlayer::PositionSeconds reaches its timestamp, and
// that position is IAudioClock's played-frame count divided by the rate the
// samples were produced at. A device crystal that runs 50 ppm fast plays the
// whole film 50 ppm fast - pictures included, because they follow the clock -
// so nothing comes apart. Resampling would only be needed if something ELSE
// were the master (mpv's video-sync=display-resample, where video is locked to
// the refresh and audio is stretched to follow it), and nothing here is. See
// "Why there is no drift correction" in docs/ARCHITECTURE.md.
//
// That argument has one assumption a test can break: that the clock reports
// what is audible, rather than something that slides away from it over a film.
// AudioClockSmoke checks the clock against the wall clock for five seconds,
// which cannot see a slow slide. This plays a long generated clip through the
// real AudioPlayer - ffmpeg decode, pipe, WASAPI - whose audio is a timecode:
// a 20 ms 1 kHz burst starting exactly on every whole second. A WASAPI
// loopback capture of the same endpoint timestamps each burst as the engine
// hands it to the device, in QPC time. The player's clock is sampled on the
// same QPC timeline, so for burst k the question is simply what the clock
// read when second k was being played. That offset must stay put for the
// whole run; a slope in it is drift, and its spread is how far the picture -
// which follows the clock - can be from the sound.
//
// Not a ctest: loopback records whatever else the machine plays, and a run is
// minutes long. It is an instrument for whoever changes the audio clock.
//
// Build (x64 Native Tools prompt, from the repository root):
//   cl /nologo /std:c++20 /EHsc /W4 /O2 /permissive- /DUNICODE /D_UNICODE
//      /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I src tools\verification\av-drift-probe.cpp
//      src\AudioPlayer.cpp src\WasapiRenderer.cpp ole32.lib shell32.lib
// (the project's own defines: AudioPlayer.cpp does not compile without them)
// Run:
//   av-drift-probe.exe <ffmpeg-directory> <work-directory> [seconds=600]
//
// Measured results are recorded in docs/ARCHITECTURE.md beside the reasoning.

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <wrl/client.h>

#include "AudioPlayer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

// QPC in seconds, on the same timeline IAudioCaptureClient::GetBuffer reports
// its packet positions on (it reports them in 100 ns units of the same
// counter).
double QpcSeconds()
{
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return double(value.QuadPart);
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) / frequency;
}

bool Run(const fs::path& exe, std::wstring arguments)
{
    std::wstring command = L"\"" + exe.wstring() + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
        return false;
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return code == 0;
}

struct Onset {
    double qpcSeconds;
    float level;
};

// Loopback capture of the default render endpoint, reporting the QPC time of
// the first sample of every burst. A burst is a sample over kThreshold after
// at least half a second under it, so the burst's own cycles never re-trigger
// and neither does anything quieter than it.
class BurstListener {
public:
    bool Start()
    {
        thread_ = std::thread([this] { Capture(); });
        while (!ready_.load() && !failed_.load()) Sleep(5);
        return !failed_.load();
    }
    void Stop()
    {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    std::vector<Onset> Onsets()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return onsets_;
    }
    uint64_t Discontinuities() const { return discontinuities_.load(); }
    uint32_t SampleRate() const { return rate_; }

private:
    static constexpr float kThreshold = 0.2f;

    void Capture()
    {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) { failed_ = true; return; }
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator))) ||
            FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) ||
            FAILED(client->GetMixFormat(&mix)) || !mix || mix->wBitsPerSample != 32 ||
            FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                      10'000'000, 0, mix, nullptr)) ||
            FAILED(client->GetService(IID_PPV_ARGS(&capture))) || FAILED(client->Start())) {
            if (mix) CoTaskMemFree(mix);
            failed_ = true;
            CoUninitialize();
            return;
        }
        rate_ = mix->nSamplesPerSec;
        const uint32_t channels = mix->nChannels;
        CoTaskMemFree(mix);
        ready_ = true;

        const uint64_t quietNeeded = rate_ / 2;
        uint64_t quiet = quietNeeded;
        while (!stop_) {
            uint32_t packet = 0;
            if (FAILED(capture->GetNextPacketSize(&packet))) break;
            if (!packet) { Sleep(2); continue; }
            BYTE* data = nullptr;
            uint32_t frames = 0;
            DWORD flags = 0;
            UINT64 qpc100ns = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc100ns))) break;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discontinuities_;
            const double packetStart = double(qpc100ns) * 1e-7;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                quiet += frames;
            } else {
                const auto* samples = reinterpret_cast<const float*>(data);
                for (uint32_t frame = 0; frame < frames; ++frame) {
                    float level = 0.0f;
                    for (uint32_t channel = 0; channel < channels; ++channel)
                        level = std::max(level, std::abs(samples[size_t(frame) * channels + channel]));
                    if (level < kThreshold) { ++quiet; continue; }
                    if (quiet >= quietNeeded) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        onsets_.push_back({packetStart + double(frame) / double(rate_), level});
                    }
                    quiet = 0;
                }
            }
            capture->ReleaseBuffer(frames);
        }
        client->Stop();
        CoUninitialize();
    }

    std::thread thread_;
    std::atomic<bool> stop_{false}, ready_{false}, failed_{false};
    std::atomic<uint64_t> discontinuities_{0};
    uint32_t rate_ = 0;
    std::mutex mutex_;
    std::vector<Onset> onsets_;
};

struct ClockSample {
    double qpcSeconds;
    double position;
};

// The clock at `when`, interpolated between the samples either side of it.
// Negative when `when` is outside the sampled span or across a gap.
double ClockAt(const std::vector<ClockSample>& samples, double when)
{
    const auto after = std::lower_bound(samples.begin(), samples.end(), when,
        [](const ClockSample& sample, double value) { return sample.qpcSeconds < value; });
    if (after == samples.begin() || after == samples.end()) return -1.0;
    const auto before = after - 1;
    const double span = after->qpcSeconds - before->qpcSeconds;
    if (!(span > 0.0) || span > 0.05) return -1.0;
    const double t = (when - before->qpcSeconds) / span;
    return before->position + t * (after->position - before->position);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        std::puts("usage: av-drift-probe <ffmpeg-directory> <work-directory> [seconds=600]");
        return 2;
    }
    const fs::path helpers = fs::absolute(argv[1]);
    const fs::path work = fs::absolute(argv[2]);
    const int seconds = argc > 3 ? std::max(20, _wtoi(argv[3])) : 600;
    std::error_code error;
    fs::create_directories(work, error);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) { std::puts("COM failed"); return 2; }

    // A picture, so the player's -vn path is the one exercised, and an audio
    // timecode: 20 ms of 1 kHz at 0.5 starting on every whole second, as a
    // cosine so the first sample of each burst is already at full level.
    // FLAC, because a lossy codec's pre-echo would smear the very edge being
    // timed.
    const fs::path clip = work / (L"av-drift-" + std::to_wstring(seconds) + L"s.mkv");
    if (!fs::exists(clip)) {
        std::printf("generating a %d s timecoded clip...\n", seconds);
        const std::wstring duration = std::to_wstring(seconds);
        if (!Run(helpers / L"ffmpeg.exe",
                 L"-v error -nostdin -y -f lavfi -i testsrc2=s=320x180:r=24:d=" + duration +
                     L" -f lavfi -i \"aevalsrc=exprs='if(lt(mod(t\\,1)\\,0.02)\\,0.5*cos(2*PI*1000*t)\\,0)':s=48000:d=" +
                     duration + L"\" -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a flac -shortest \"" +
                     clip.wstring() + L"\"")) {
            std::puts("could not generate the clip");
            CoUninitialize();
            return 2;
        }
    }

    BurstListener listener;
    if (!listener.Start()) { std::puts("loopback capture could not start"); CoUninitialize(); return 2; }

    AudioPlayer::Settings settings;
    settings.helperDirectory = helpers.wstring();
    AudioPlayer audio(std::move(settings));
    if (!audio.Start(clip.wstring(), 0.0)) {
        std::puts("the player could not start audio");
        listener.Stop();
        CoUninitialize();
        return 2;
    }

    // The clock sampled on the QPC timeline, every 2 ms, which is finer than
    // the engine period it moves in.
    std::vector<ClockSample> samples;
    samples.reserve(size_t(seconds) * 600);
    const double started = QpcSeconds();
    double lastReport = started;
    while (QpcSeconds() - started < double(seconds) + 1.0) {
        const double before = QpcSeconds();
        const double position = audio.PositionSeconds();
        const double after = QpcSeconds();
        if (position >= 0.0) samples.push_back({0.5 * (before + after), position});
        if (after - lastReport >= 60.0) {
            lastReport = after;
            std::printf("  %4.0f s played, clock %.3f s\n", after - started, position);
            std::fflush(stdout);
        }
        Sleep(2);
    }
    audio.Stop();
    listener.Stop();
    CoUninitialize();

    if (samples.size() < 2) { std::puts("the clock never answered"); return 1; }

    // Rate of the clock against QPC over the whole run: the endpoint crystal
    // as this machine's performance counter sees it. This is how much fast or
    // slow the WHOLE film runs, pictures and sound together.
    // Inside the clip only: before the first second the clock is still
    // settling, and past the end it stands still and is carried on the
    // steady clock (audio_clock::Present), neither of which is the crystal.
    const auto inside = [&](const ClockSample& sample) {
        return sample.position >= 1.0 && sample.position <= double(seconds) - 1.0;
    };
    const auto firstInside = std::find_if(samples.begin(), samples.end(), inside);
    const auto lastInside = std::find_if(samples.rbegin(), samples.rend(), inside);
    if (firstInside == samples.end() || lastInside == samples.rend()) { std::puts("no clock inside the clip"); return 1; }
    const ClockSample& first = *firstInside;
    const ClockSample& last = *lastInside;
    const double clockPpm = ((last.position - first.position) / (last.qpcSeconds - first.qpcSeconds) - 1.0) * 1e6;

    struct Hit { int second; double offset; double qpc; };
    std::vector<Hit> hits;
    size_t rejected = 0;
    for (const Onset& onset : listener.Onsets()) {
        const double clock = ClockAt(samples, onset.qpcSeconds);
        if (clock < 0.0) { ++rejected; continue; }
        const int second = int(std::lround(clock));
        // Burst 0 opens under the stream's fade-in ramp, so its edge is not
        // where the timecode put it.
        if (second < 1) { ++rejected; continue; }
        const double offset = clock - double(second);
        // Anything a tenth of a second off a whole second is not one of ours.
        if (std::abs(offset) > 0.1) { ++rejected; continue; }
        hits.push_back({second, offset, onset.qpcSeconds});
    }
    if (hits.size() < 10) { std::printf("only %zu bursts were heard\n", hits.size()); return 1; }

    double lowest = hits.front().offset, highest = lowest, sum = 0.0;
    double sumX = 0.0, sumY = 0.0, sumXX = 0.0, sumXY = 0.0;
    double videoLowest = 1e9, videoHighest = -1e9;
    constexpr double kFps = 24.0;
    for (const Hit& hit : hits) {
        lowest = std::min(lowest, hit.offset);
        highest = std::max(highest, hit.offset);
        sum += hit.offset;
        const double x = double(hit.second);
        sumX += x; sumY += hit.offset; sumXX += x * x; sumXY += x * hit.offset;
        // What the picture showed while second k was being heard: the newest
        // frame whose timestamp the clock had reached, which is the rule the
        // presentation gate applies. Positive means the sound is ahead.
        const double clock = double(hit.second) + hit.offset;
        const double shownPts = std::floor(clock * kFps + 1e-9) / kFps;
        const double soundMinusPicture = double(hit.second) - shownPts;
        videoLowest = std::min(videoLowest, soundMinusPicture);
        videoHighest = std::max(videoHighest, soundMinusPicture);
    }
    const double n = double(hits.size());
    const double slope = (n * sumXY - sumX * sumY) / (n * sumXX - sumX * sumX);

    // The first and last minute, side by side: the plainest statement of
    // whether anything moved over the run.
    double headSum = 0.0, tailSum = 0.0;
    int headCount = 0, tailCount = 0;
    const int lastSecond = hits.back().second;
    for (const Hit& hit : hits) {
        if (hit.second <= 60) { headSum += hit.offset; ++headCount; }
        if (hit.second > lastSecond - 60) { tailSum += hit.offset; ++tailCount; }
    }

    std::printf("run: %d s clip, %zu clock samples, loopback at %u Hz, %llu capture discontinuities\n",
                seconds, samples.size(), listener.SampleRate(),
                (unsigned long long)listener.Discontinuities());
    std::printf("bursts matched: %zu of %d expected (%zu rejected)\n", hits.size(), seconds - 1, rejected);
    std::printf("endpoint clock vs QPC: %+.1f ppm (%+.1f ms per hour)\n", clockPpm, clockPpm * 3.6);
    std::printf("clock minus audible second: mean %+.3f ms, min %+.3f ms, max %+.3f ms, spread %.3f ms\n",
                sum / n * 1e3, lowest * 1e3, highest * 1e3, (highest - lowest) * 1e3);
    std::printf("  first minute mean %+.3f ms, last minute mean %+.3f ms\n",
                headCount ? headSum / headCount * 1e3 : 0.0, tailCount ? tailSum / tailCount * 1e3 : 0.0);
    std::printf("  trend %+.4f ms per hour of film\n", slope * 3600.0 * 1e3);
    std::printf("sound minus picture shown (24 fps gate): %+.3f .. %+.3f ms\n",
                videoLowest * 1e3, videoHighest * 1e3);

    // Bounded means the offset never wandered by a frame over the run and
    // shows no trend worth a frame over a three-hour film.
    const bool bounded = (highest - lowest) < 1.0 / kFps && std::abs(slope * 3.0 * 3600.0) < 1.0 / kFps;
    std::puts(bounded ? "BOUNDED" : "DRIFTING");
    return bounded ? 0 : 1;
}
