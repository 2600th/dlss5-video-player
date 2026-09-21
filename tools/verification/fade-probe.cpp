// Plays a signal whose amplitude at the moment of the cut is known, so the
// step a stop leaves behind can be measured instead of argued about.
//
// Why this exists. Running the audio harness under loopback-probe could not
// tell the ramped stop from the cut one: both showed the same handful of
// large sample-to-sample steps. That is an ambiguous result - it could mean
// the ramp does nothing, or that the harness's own transitions are not where
// those steps come from - and an ambiguous result is not evidence. This
// removes every variable except the one under test.
//
// The signal is a 1 kHz sine at 0.8 whose last written sample sits exactly on
// a positive peak, so a cut is a step of 0.8 every run rather than whatever
// phase it happened to land on. Direct current was tried first and this
// endpoint removes it: a held 0.8 came back through loopback as a decaying
// transient, which is a high-pass somewhere in the chain.
//
// Build:
//   cl /nologo /std:c++20 /EHsc /W4 /I src tools\verification\fade-probe.cpp
//      src\WasapiRenderer.cpp ole32.lib
// Run, with loopback-probe.exe recording alongside:
//   fade-probe.exe faded     (FadeOutAndStop)
//   fade-probe.exe cut       (Stop, which is what the path did before)
//
// Measured on the RTX 5090 machine's default endpoint, 48 kHz shared mode,
// 1056-frame (22 ms) engine buffer:
//
//   cut          largest step 0.910188, identical on every run
//   4 ms ramp    largest step 0.26 - 0.35
//   40 ms ramp   largest step 0.302620
//
// The ramp takes 0.91 down to about 0.3, which is 9.6 dB off the
// discontinuity. The last 0.3 is NOT the ramp: making the ramp ten times
// longer does not move it, and the renderer's own log confirms the tail was
// queued and drained in every run. It is the audio engine's transient as it
// takes the stream down, which the player does not control.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include "WasapiRenderer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

int wmain(int argc, wchar_t** argv)
{
    const bool faded = argc < 2 || std::wstring_view(argv[1]) != L"cut";
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) { std::puts("COM failed"); return 2; }

    WasapiRenderer renderer;
    if (!renderer.Open()) { std::puts("could not open the render endpoint"); CoUninitialize(); return 2; }
    const auto format = renderer.CurrentFormat();
    if (!renderer.Start()) { std::puts("the endpoint refused to start"); CoUninitialize(); return 2; }

    // A 1 kHz sine at 0.8, stopped at a controlled phase.
    //
    // Direct current was the first choice and this endpoint removes it - a
    // held 0.8 came back through loopback as a decaying transient, which is a
    // high-pass somewhere in the chain and makes the signal useless as a
    // ruler. A sine survives, and choosing where to stop it makes the step
    // deterministic rather than whatever phase the cut happened to land on.
    //
    // sin(2*pi*f*n/fs) peaks at n = fs/(4f), and every fs/f frames after. So
    // the last frame written is placed exactly on a positive peak: a cut
    // there is a step of 0.8 every single time, and a four-millisecond ramp
    // is 0.8 spread over 192 frames. Nothing overlaps.
    constexpr float kLevel = 0.8f;
    constexpr double kToneHz = 1000.0;
    const uint32_t period = uint32_t(double(format.sampleRate) / kToneHz);
    const uint32_t quarter = period / 4u;
    uint64_t target = uint64_t(format.sampleRate) * 2u;
    // Last index written is target-1, and it must sit on a peak.
    while ((target - 1u) % period != quarter) ++target;

    uint64_t written = 0;
    std::vector<float> block;
    constexpr double pi = 3.14159265358979323846;
    while (written < target) {
        uint32_t wanted = 0;
        if (!renderer.WaitForSpace(50, wanted)) { std::puts("the endpoint stopped accepting frames"); break; }
        if (!wanted) continue;
        if (written + wanted > target) wanted = uint32_t(target - written);
        block.assign(size_t(wanted) * format.channels, 0.0f);
        for (uint32_t frame = 0; frame < wanted; ++frame) {
            const double phase = 2.0 * pi * kToneHz * double(written + frame) / double(format.sampleRate);
            const float value = kLevel * float(std::sin(phase));
            for (uint16_t channel = 0; channel < format.channels; ++channel)
                block[size_t(frame) * format.channels + channel] = value;
        }
        if (!renderer.Write(block.data(), wanted)) { std::puts("write failed"); break; }
        written += wanted;
    }

    std::printf("wrote %llu frames of a %.0f Hz sine at %.2f, last sample on a peak, "
                "%u Hz, stopping %s\n",
                (unsigned long long)written, kToneHz, double(kLevel), format.sampleRate,
                faded ? "with the ramp" : "by cutting");
    if (faded) renderer.FadeOutAndStop();
    else renderer.Stop();

    renderer.Close();
    CoUninitialize();
    return 0;
}
