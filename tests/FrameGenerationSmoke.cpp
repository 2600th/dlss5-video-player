// Opt-in hardware experiment: registered under the `gpu` CTest label, which the
// portable suite excludes (`ctest -LE gpu`) and an RTX machine opts into with
// `ctest -L gpu`.
//
// DlssgProbeSmoke established that the raw NGX path ADMITS
// NVSDK_NGX_Feature_FrameGeneration on this machine; DlssgEvaluateSmoke
// established that it PRODUCES a real intermediate frame, using a synthetic
// pair with exactly known ground truth. Neither of those is a converted file.
// This program asks the questions left before the player can offer the
// feature: does a real clip come out the other end of FrameGenerationPass as a
// playable file at N times the rate, THE SAME LENGTH, and still CARRYING ITS
// SOUND.
//
// Length is the whole point, which is why it is asserted rather than printed.
// Frame generation must change how many frames a video has and nothing else:
// a pass that drops the tail, double-counts a pair or emits a frame count that
// is not the multiple it claims produces a file that plays at the wrong speed
// or ends early, and every one of those looks like a successful conversion in
// a log. So the first two assertions are the frame count against
// sourceFramesRead * multiplier and the probed duration against the source's,
// and a miss on either fails the test.
//
// Audio is here because those two assertions cannot see it, and the first
// shipped version of the pass encoded video only: the player hands the
// converted file to its ordinary load path, which starts audio from the file
// it loaded, so the conversion played SILENT while every number in this report
// looked right. So the output's audio and subtitle stream counts are asserted
// against the source's, and the output's audio extent against its video's - a
// mux that stretched or truncated the audio keeps the counts and fails the
// extent. A silent source cannot fail any of that, so one is given a
// synthesised tone before the conversion rather than being allowed to make the
// check vacuous.
//
// Exit codes: 0 when the conversion ran and every assertion held, 1 when one of
// them missed, 2 when no question could be asked at all - no D3D12 device, a
// runtime that refused the feature, or a source that could not be given the
// audio track the carriage assertions need.
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
#include <system_error>
#include <vector>

#include <process.h>

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

fs::path ModuleDirectory()
{
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    return fs::path(module).parent_path();
}

fs::path DefaultOutput() { return ModuleDirectory() / L"frame-generation-smoke.mkv"; }

// The same two places the pass itself looks: the directory the harness was
// handed (CTest passes FFMPEG_STAGED_DIR), and otherwise beside this
// executable, which is what a staged build has.
fs::path FfmpegTool(const fs::path& helpers, const wchar_t* name)
{
    const fs::path directory = helpers.empty() ? ModuleDirectory() : helpers;
    const fs::path candidate = directory / name;
    std::error_code error;
    return fs::is_regular_file(candidate, error) && !error ? candidate : fs::path();
}

int RunTool(const fs::path& tool, const std::vector<std::wstring>& arguments)
{
    std::vector<const wchar_t*> argv;
    argv.reserve(arguments.size() + 2);
    const std::wstring executable = tool.wstring();
    argv.push_back(executable.c_str());
    for (const std::wstring& argument : arguments) argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    return int(_wspawnv(_P_WAIT, executable.c_str(), argv.data()));
}

std::wstring SecondsText(double seconds)
{
    wchar_t text[64]{};
    swprintf_s(text, L"%.3f", seconds);
    return text;
}

// A source with sound, built from one without it. A conversion of a silent
// clip cannot fail the audio assertions below no matter how badly the mux is
// broken, so when the clip the harness was given has no audio track this
// stage makes one: the clip's own video, stream-copied so the conversion still
// sees the same frames, plus a synthesised 440 Hz tone the length of the
// video. The tone is encoded to AAC rather than left as raw PCM because AAC is
// what a real source carries and what the pass's stream copy has to move.
bool SynthesizeAudibleSource(const fs::path& ffmpeg, const fs::path& source, double seconds,
                             const fs::path& output)
{
    std::error_code ignored;
    fs::remove(output, ignored);
    const std::vector<std::wstring> arguments{
        L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-y",
        L"-i", source.wstring(),
        L"-f", L"lavfi", L"-i", L"sine=frequency=440:duration=" + SecondsText(seconds),
        L"-map", L"0:v:0", L"-map", L"1:a:0", L"-c:v", L"copy", L"-c:a", L"aac",
        L"-shortest", L"-f", L"matroska", output.wstring()};
    if (RunTool(ffmpeg, arguments) != 0) return false;
    return fs::is_regular_file(output, ignored) && !ignored;
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
    std::error_code directoryError;
    fs::create_directories(output.parent_path(), directoryError);

    // The source the conversion is actually run on. Carrying the source's audio
    // is the second question this harness asks, because the player starts audio
    // from the file it loaded and a video-only conversion therefore plays
    // silent - and a SILENT source cannot fail that assertion however broken
    // the mux is. So a clip with no audio track is given one here, before
    // anything else happens, and the conversion runs on that instead.
    const fs::path ffmpeg = FfmpegTool(helpers, L"ffmpeg.exe");
    const MediaStreamSummary givenStreams = SummarizeMediaStreams(helpers, source, {});
    fs::path conversionSource = source;
    bool synthesizedAudio = false;
    if (!givenStreams.ok) {
        std::cout << "source=" << Narrow(source.wstring())
                  << "\nexperiment=unreachable reason=the source's streams could not be listed: "
                  << Narrow(givenStreams.detail) << "\n";
        return 2;
    }
    if (givenStreams.audioStreams == 0) {
        const ProbeResult givenProbe = ProbeMedia(helpers, source, {}, MediaProbeMode::CachedMetadata);
        const double seconds = double(givenProbe.duration100ns) / 10000000.0;
        const fs::path audible = output.parent_path() / L"frame-generation-smoke-source-with-audio.mkv";
        if (ffmpeg.empty() || !givenProbe.ok || seconds <= 0.0 ||
            !SynthesizeAudibleSource(ffmpeg, source, seconds, audible)) {
            std::cout << "source=" << Narrow(source.wstring())
                      << "\nexperiment=unreachable reason=this silent source could not be given an audio "
                         "track to carry\n";
            return 2;
        }
        conversionSource = audible;
        synthesizedAudio = true;
    }

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
        request.source = conversionSource;
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
        const ProbeResult sourceProbe = ProbeMedia(helpers, conversionSource, stopSource.get_token());
        const ProbeResult outputProbe = result.ok ? ProbeMedia(helpers, output, stopSource.get_token())
                                                  : ProbeResult{};
        // Stream counts on both sides, and where the converted file's audio
        // actually stops: what has to hold is that every audio stream the
        // source had arrived, and that the audio it arrived with still ends
        // with the video. A mux that stretched, resampled or truncated the
        // audio keeps the stream count and fails the second half.
        const MediaStreamSummary sourceStreams =
            SummarizeMediaStreams(helpers, conversionSource, stopSource.get_token());
        const MediaStreamSummary outputStreams = result.ok
            ? SummarizeMediaStreams(helpers, output, stopSource.get_token(),
                                    MediaStreamMode::WithAudioEnd)
            : MediaStreamSummary{};
        const double audioVideoDeltaMs =
            double(outputStreams.audioEnd100ns - ProbedDuration(outputProbe)) / 10000.0;
        const int64_t sourceDuration100ns = ProbedDuration(sourceProbe);
        const int64_t probedDuration100ns = ProbedDuration(outputProbe);
        const double durationDeltaMs = double(probedDuration100ns - sourceDuration100ns) / 10000.0;
        // What the pass actually ran at, which is the requested multiplier only
        // when the runtime admitted it; a clamp says so in detail and shows up
        // here as a smaller number.
        const uint64_t effectiveMultiplier =
            result.sourceFps > 0.0 ? uint64_t(std::llround(result.outputFps / result.sourceFps)) : 0;

        std::cout << "source=" << Narrow(source.wstring()) << "\n"
                  << "conversionSource=" << Narrow(conversionSource.wstring()) << "\n"
                  << "sourceAudioSynthesized=" << (synthesizedAudio ? "true" : "false") << "\n"
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
                  << "sourceAudioStreams=" << sourceStreams.audioStreams << "\n"
                  << "sourceSubtitleStreams=" << sourceStreams.subtitleStreams << "\n"
                  << "outputAudioStreams=" << outputStreams.audioStreams << "\n"
                  << "outputSubtitleStreams=" << outputStreams.subtitleStreams << "\n"
                  << "resultAudioCarried=" << (result.audioCarried ? "true" : "false") << "\n"
                  << "resultOutputAudioStreams=" << result.outputAudioStreams << "\n"
                  << "resultOutputSubtitleStreams=" << result.outputSubtitleStreams << "\n"
                  << "outputAudioEnd100ns=" << outputStreams.audioEnd100ns << "\n"
                  << "audioVideoDeltaMs=" << audioVideoDeltaMs << "\n"
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
        } else if (!sourceStreams.ok || !outputStreams.ok) {
            std::cout << "experiment=ran\nverdict=FAIL reason=the streams could not be counted: "
                      << Narrow(sourceStreams.ok ? outputStreams.detail : sourceStreams.detail) << "\n";
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
            // Every audio stream the source had has to be in the converted
            // file, and the pass has to report the same number it actually
            // wrote: a caller that shows "audio carried" from a field the pass
            // filled in without looking is no better than the silent output
            // this harness exists to catch. sourceAudioStreams is asserted
            // non-zero because the assertion is vacuous without it, which is
            // why a silent clip was given a track above.
            const bool audioCarriedHeld = sourceStreams.audioStreams > 0 &&
                                          outputStreams.audioStreams == sourceStreams.audioStreams &&
                                          result.outputAudioStreams == outputStreams.audioStreams &&
                                          result.audioCarried;
            // The same tolerance the video's own length gets, and for the same
            // reason: the audio was stream-copied onto a video of the same
            // length, so it must still end where the video ends. A stretch, a
            // resample or a truncation shows up here and nowhere else.
            const bool audioSyncHeld = outputStreams.audioEnd100ns > 0 &&
                                       std::abs(audioVideoDeltaMs) <= durationToleranceMs;

            const bool held = frameCountHeld && durationHeld && audioCarriedHeld && audioSyncHeld;
            std::cout << "expectedFrameCount=" << expectedFrames << "\n"
                      << "frameCountDelta=" << frameDelta << "\n"
                      << "durationToleranceMs=" << durationToleranceMs << "\n"
                      << "frameCountHeld=" << (frameCountHeld ? "true" : "false") << "\n"
                      << "durationHeld=" << (durationHeld ? "true" : "false") << "\n"
                      << "audioCarriedHeld=" << (audioCarriedHeld ? "true" : "false") << "\n"
                      << "audioSyncHeld=" << (audioSyncHeld ? "true" : "false") << "\n"
                      << "experiment=ran\n"
                      << "verdict=" << (held ? "PASS" : "FAIL") << "\n";
            if (!held) exitCode = 1;
        }
    }

    MFShutdown();
    CoUninitialize();
    return exitCode;
}
