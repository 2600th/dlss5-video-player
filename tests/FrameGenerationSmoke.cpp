// Opt-in hardware experiment: registered under the `gpu` CTest label, which the
// portable suite excludes (`ctest -LE gpu`) and an RTX machine opts into with
// `ctest -L gpu`.
//
// DlssgProbeSmoke established that the raw NGX path ADMITS
// NVSDK_NGX_Feature_FrameGeneration on this machine; DlssgEvaluateSmoke
// established that it PRODUCES a real intermediate frame, using a synthetic
// pair with exactly known ground truth. Neither of those is a converted file.
// This program asks the only question left before the player can offer the
// feature: does a real clip come out the other end of FrameGenerationPass as a
// playable file at N times the rate and THE SAME LENGTH.
//
// Length is the whole point, which is why it is asserted rather than printed.
// Frame generation must change how many frames a video has and nothing else:
// a pass that drops the tail, double-counts a pair or emits a frame count that
// is not the multiple it claims produces a file that plays at the wrong speed
// or ends early, and every one of those looks like a successful conversion in
// a log. So the two assertions are the frame count against
// sourceFramesRead * multiplier and the probed duration against the source's,
// and a miss on either fails the test.
//
// Exit codes: 0 when the conversion ran and both assertions held, 1 when one of
// them missed, 2 when no question could be asked at all - no D3D12 device, or a
// runtime that refused the feature.
#include <windows.h>

#include "FrameGenerationPass.h"
#include "MediaPipeline.h"

#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stop_token>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;

// 1280x720 at 30 fps, the clip the DLAA path is already measured on. At
// multiplier 4 it becomes 120 fps, which is what the 120 Hz panel here plans
// for a 30 fps source (frame_rate_policy::PlanFrameGeneration).
constexpr const wchar_t* kDefaultSource = L"external/test-media/dlaa-smoke.mp4";
constexpr uint32_t kDefaultMultiplier = 4;

std::string Narrow(std::wstring_view text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string narrow(size_t(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), narrow.data(), length, nullptr, nullptr);
    return narrow;
}

const char* ErrorName(FrameGenerationError error)
{
    switch (error) {
    case FrameGenerationError::None: return "none";
    case FrameGenerationError::InvalidRequest: return "invalidRequest";
    case FrameGenerationError::Source: return "source";
    case FrameGenerationError::Device: return "device";
    case FrameGenerationError::Runtime: return "runtime";
    case FrameGenerationError::Encoder: return "encoder";
    case FrameGenerationError::Cancelled: return "cancelled";
    }
    return "unknown";
}

// The video extent when the probe measured one, the container's length
// otherwise. ProbeMedia fills videoDuration100ns only in FullValidation mode,
// which is the mode used here precisely because it also decodes through to the
// final frame - which is what "playable" means for the converted file.
int64_t ProbedDuration(const ProbeResult& probe)
{
    return probe.videoDuration100ns > 0 ? probe.videoDuration100ns : probe.duration100ns;
}

fs::path DefaultOutput()
{
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    return fs::path(module).parent_path() / L"frame-generation-smoke.mkv";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::cout << std::fixed << std::setprecision(3);

    const fs::path source = argc > 1 ? fs::path(argv[1]) : fs::path(kDefaultSource);
    const uint32_t multiplier = argc > 2 ? uint32_t(std::max(0L, std::wcstol(argv[2], nullptr, 10)))
                                         : kDefaultMultiplier;
    // FFmpeg's directory. Empty searches beside this executable and then PATH,
    // which is what a staged build gets; the CTest registration passes
    // FFMPEG_STAGED_DIR so a build tree that never staged the helpers can still
    // run it against the repository's own copy.
    const fs::path helpers = argc > 3 ? fs::path(argv[3]) : fs::path();
    const fs::path output = argc > 4 ? fs::path(argv[4]) : DefaultOutput();

    // VideoDecoder drives an FFmpeg child for local files and falls back to
    // Media Foundation's reader, which needs both of these started - the player
    // starts them before its first window.
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::cout << "experiment=unreachable reason=CoInitializeEx failed\n";
        return 2;
    }
    if (FAILED(MFStartup(MF_VERSION))) {
        CoUninitialize();
        std::cout << "experiment=unreachable reason=MFStartup failed\n";
        return 2;
    }

    int exitCode = 0;
    {
        // Measured before the conversion, because this is the number the player
        // has to have BEFORE it can choose a multiplier: it is the runtime's own
        // DLSSG.MultiFrameCountMax and not a constant, and the conversion below
        // clamps to it.
        const FrameGenerationCapability capability = QueryFrameGenerationCapability();

        std::stop_source stopSource;
        FrameGenerationPass pass(helpers);
        FrameGenerationRequest request;
        request.source = source;
        request.output = output;
        request.multiplier = multiplier;

        FrameGenerationProgress latest{};
        uint64_t progressReports = 0;
        const FrameGenerationResult result =
            pass.Run(request, stopSource.get_token(), [&](const FrameGenerationProgress& progress) {
                latest = progress;
                ++progressReports;
            });

        // Both files are probed the same way, with the same tool, so the two
        // durations are comparable: a difference between them is the pass's and
        // not the probe's.
        const ProbeResult sourceProbe = ProbeMedia(helpers, source, stopSource.get_token());
        const ProbeResult outputProbe = result.ok ? ProbeMedia(helpers, output, stopSource.get_token())
                                                  : ProbeResult{};
        const int64_t sourceDuration100ns = ProbedDuration(sourceProbe);
        const int64_t probedDuration100ns = ProbedDuration(outputProbe);
        const double durationDeltaMs = double(probedDuration100ns - sourceDuration100ns) / 10000.0;
        // What the pass actually ran at, which is the requested multiplier only
        // when the runtime admitted it; a clamp says so in detail and shows up
        // here as a smaller number.
        const uint64_t effectiveMultiplier =
            result.sourceFps > 0.0 ? uint64_t(std::llround(result.outputFps / result.sourceFps)) : 0;

        std::cout << "source=" << Narrow(source.wstring()) << "\n"
                  << "output=" << Narrow(output.wstring()) << "\n"
                  << "capabilityAvailable=" << (capability.available ? "true" : "false") << "\n"
                  << "capabilityMultiFrameCountMax=" << capability.multiFrameCountMax << "\n"
                  << "capabilityDetail=" << Narrow(capability.detail) << "\n"
                  << "geometry=" << result.width << "x" << result.height << "\n"
                  << "multiplierRequested=" << multiplier << "\n"
                  << "multiplierEffective=" << effectiveMultiplier << "\n"
                  << "sourceFps=" << result.sourceFps << "\n"
                  << "outputFps=" << result.outputFps << "\n"
                  << "sourceFramesRead=" << latest.sourceFramesRead << "\n"
                  << "sourceFramesTotal=" << latest.sourceFramesTotal << "\n"
                  << "framesWritten=" << result.framesWritten << "\n"
                  << "generatedFrames=" << result.generatedFrames << "\n"
                  << "evaluations=" << result.evaluations << "\n"
                  << "progressReports=" << progressReports << "\n"
                  << "probedFrameCount=" << outputProbe.frameCount << "\n"
                  << "probedDecodedFinalFrame=" << (outputProbe.decodedFinalFrame ? "true" : "false") << "\n"
                  << "probedDuration100ns=" << probedDuration100ns << "\n"
                  << "sourceDuration100ns=" << sourceDuration100ns << "\n"
                  << "sourceFrameCount=" << sourceProbe.frameCount << "\n"
                  << "durationDeltaMs=" << durationDeltaMs << "\n"
                  << "ok=" << (result.ok ? "true" : "false") << "\n"
                  << "error=" << ErrorName(result.error) << "\n"
                  << "detail=" << Narrow(result.detail) << "\n";

        if (!result.ok) {
            // A refused device or runtime means no question was asked; anything
            // else is a conversion that ran and failed, which is a real defect.
            const bool unreachable = result.error == FrameGenerationError::Device ||
                                     result.error == FrameGenerationError::Runtime;
            std::cout << "experiment=" << (unreachable ? "unreachable" : "failed") << "\n"
                      << "verdict=FAIL\n";
            exitCode = unreachable ? 2 : 1;
        } else if (!outputProbe.ok) {
            std::cout << "experiment=ran\nverdict=FAIL reason=the converted file could not be probed: "
                      << Narrow(outputProbe.detail) << "\n";
            exitCode = 1;
        } else {
            // One output frame of slack, and no more: the pass emits exactly
            // sourceFrames * multiplier and the container's own frame count is
            // read by demuxing, so a whole frame of disagreement is already the
            // most a correct pass can produce.
            const int64_t expectedFrames = int64_t(latest.sourceFramesRead * effectiveMultiplier);
            const int64_t frameDelta = int64_t(outputProbe.frameCount) - expectedFrames;
            // One SOURCE frame interval, because that is the granularity the
            // source's own length is known to; a generated frame must not
            // change how long the video runs.
            const double durationToleranceMs = result.sourceFps > 0.0 ? 1000.0 / result.sourceFps : 0.0;
            const bool frameCountHeld = std::llabs(frameDelta) <= 1;
            const bool durationHeld = std::abs(durationDeltaMs) <= durationToleranceMs;

            std::cout << "expectedFrameCount=" << expectedFrames << "\n"
                      << "frameCountDelta=" << frameDelta << "\n"
                      << "durationToleranceMs=" << durationToleranceMs << "\n"
                      << "frameCountHeld=" << (frameCountHeld ? "true" : "false") << "\n"
                      << "durationHeld=" << (durationHeld ? "true" : "false") << "\n"
                      << "experiment=ran\n"
                      << "verdict=" << (frameCountHeld && durationHeld ? "PASS" : "FAIL") << "\n";
            if (!frameCountHeld || !durationHeld) exitCode = 1;
        }
    }

    MFShutdown();
    CoUninitialize();
    return exitCode;
}
