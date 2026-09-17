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
// The STREAM SOURCE, this harness's fifth argument, is here because the
// paragraph above measured the wrong file. The player converts the NEURAL
// RENDER when that view is on screen and this project writes those carriers
// video-only (MediaPipeline's BuildEncoderArguments passes `-an`), so the
// converted file's audio can only be copied from the ORIGINAL the carrier was
// rendered from - and the check that was supposed to catch a silent output
// read its expected counts off the file the FRAMES came from, which for a
// carrier is zero, so it compared 0 against 0 and passed. So when a stream
// source is given, every carriage assertion below takes its expected counts
// from THAT file, and the synthesis stage above is not run: giving the frame
// source a tone of its own is exactly what the player cannot do, and doing it
// here would hand the mux audio on the one file it must not take audio from,
// which is what made this harness blind to the defect.
//
// The phase probe is here because none of those can see WHERE the inserted
// frames sit, which is the only thing the feature is for. The pass shipped
// handing its reset evaluate multiFrameCount=generatedPerSource, the runtime
// answered with the newer source frame byte for byte at every index of the
// first pair, and every assertion above held on that output. So the converted
// file is decoded back to raw gray, each frame is located by its brightness
// centroid on the x axis, and the frames between two source frames must be
// strictly inside that pair, in order, at even fractions of it. See the block
// above DecodeCentroids for the tolerances and what each one rejects.
//
// Exit codes: 0 when the conversion ran and every assertion held, 1 when one of
// them missed (including a decode that could not be read back - an
// unmeasurable conversion is a failure, not a skip), 2 when no question could
// be asked at all - no D3D12 device, a runtime that refused the feature, no
// staged ffmpeg.exe to decode with, or a source that could not be given the
// audio track the carriage assertions need.
//
// Usage: FrameGenerationSmoke [source] [multiplier] [ffmpegBinDir] [output] [streamSource]
#include <windows.h>

#include "FrameGenerationPass.h"
#include "MediaPipeline.h"

#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
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

// ---------------------------------------------------------------------------
// The phase probe.
//
// Everything above this point can hold on a conversion that inserted nothing.
// The pass shipped handing its RESET evaluate multiFrameCount=generatedPerSource,
// and the runtime answered by returning the newer source frame BYTE FOR BYTE at
// every index of the first pair - three identical copies at 4x - and a poisoned
// history in the second pair. The frame count was still sourceFrames *
// multiplier, the duration was still the source's, and the audio still arrived,
// so this harness passed. What it could not see is WHERE the inserted frames
// sit, which is the only thing frame generation is for.
//
// So the converted file is decoded back to 8-bit gray and each frame is located
// by its brightness centroid on the x axis - the one position a moving probe
// makes measurable without a reference decoder.

// Pixels at or below this are the black background of the synthetic clips.
// Counting them would drag every centroid toward the middle of the frame and
// compress the measurement; this is the same floor the phase table in the
// design note was measured with.
constexpr uint8_t kBlackFloor = 24;

// Only this many source frames' worth of output is decoded. The properties
// asserted below are about the FIRST pair (the one the reset bug broke) and the
// steady state immediately after it, not about the whole file, and the 900 s
// budget must not be spent decoding a feature-length clip.
constexpr uint64_t kPhasePrefixSourceFrames = 5;

// A source frame has to come out of the conversion as itself. One pixel of
// centroid is slack a re-encode fits inside with room: measured here, the
// worst passthrough delta over the first five frames is 0.595 px on
// external/test-media/dlaa-smoke.mp4, whose real content goes out through the
// H.264 encoder, and 0.085 px on the lossless textured probe clip. So a
// passthrough frame that has moved a whole pixel was not re-encoded, it was
// rewritten - or the pass wrote a generated frame into a source frame's slot.
constexpr double kPassthroughTolerancePx = 1.0;

// A generated frame this close to an end of its pair is not BETWEEN the pair,
// it is one of them. The copy bug measured phase 1.002 - the newer frame
// itself, just past the far end - so this margin is what rejects it.
constexpr double kEndMarginPx = 2.0;

// The worst deviation the corrected measurement saw over 2x..6x was 0.109 of an
// interval (3x and 6x index 1, a systematic slight-early bias), so 0.15 admits
// the measured behaviour with margin while still rejecting both failures this
// probe exists for: a byte copy sits at phase 1.0, which is 0.75 from the 0.25
// a 4x index 1 owes, and midpoint clustering puts every index at 0.5, which is
// 0.25 from that same 0.25.
constexpr double kPhaseTolerance = 0.15;

// A pair narrower than this carries no horizontal displacement the centroid can
// resolve, and its phase denominator is noise rather than motion: a 2 px end
// margin and a 0.15 phase window only mean anything when the two source frames
// are far apart. Measured here on external/test-media/dlaa-smoke.mp4, which is
// framed statically - its first six source centroids span 0.05 px, four hundred
// times less than the floor - so that clip is a still image AS FAR AS THIS
// PROBE IS CONCERNED and its pairs report themselves skipped by name below.
// The textured registration in CMakeLists.txt is the one that moves 40 px per
// source frame and therefore actually exercises the phase assertions.
constexpr double kMinPairSeparationPx = 8.0;

// One frame's brightness centroid per decoded frame, NaN for a frame with no
// pixel above the floor (which cannot be located at all).
struct CentroidSeries {
    bool ok{};
    std::wstring detail;
    std::vector<double> centroids;
};

// Decodes the first `frames` frames of `media` to raw gray through the staged
// FFmpeg and returns their centroids. rawvideo + gray is one byte per pixel
// with no subsampling to undo, so the frame boundaries are arithmetic and the
// weight of a pixel is the byte itself. The scratch file is written beside the
// converted output and removed before returning.
CentroidSeries DecodeCentroids(const fs::path& ffmpeg, const fs::path& media, uint32_t width,
                               uint32_t height, uint64_t frames, const fs::path& scratch)
{
    std::error_code ignored;
    fs::remove(scratch, ignored);
    // -frames:v bounds the decode to the prefix, so a two-hour clip costs what
    // a two-second one costs.
    const std::vector<std::wstring> arguments{
        L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-y",
        L"-i", media.wstring(), L"-frames:v", std::to_wstring(frames),
        L"-f", L"rawvideo", L"-pix_fmt", L"gray", scratch.wstring()};
    // Wrapped so the scratch file is removed on EVERY path out, the failures
    // included: a 20-frame gray prefix of 720p is 18 MB, nothing downstream
    // reads it, and a failing run is the one most likely to be repeated.
    const auto measure = [&]() -> CentroidSeries {
        CentroidSeries series;
        if (RunTool(ffmpeg, arguments) != 0) {
            series.detail = L"ffmpeg could not decode " + media.filename().wstring() + L" to gray";
            return series;
        }
        std::ifstream stream(scratch, std::ios::binary);
        if (!stream) {
            series.detail = L"the gray decode of " + media.filename().wstring() + L" could not be opened";
            return series;
        }
        const size_t frameBytes = size_t(width) * size_t(height);
        std::vector<uint8_t> frame(frameBytes);
        series.centroids.reserve(size_t(frames));
        for (uint64_t index = 0; index < frames; ++index) {
            if (!stream.read(reinterpret_cast<char*>(frame.data()), std::streamsize(frameBytes))) {
                series.detail = L"the gray decode of " + media.filename().wstring() + L" ran out after " +
                                std::to_wstring(index) + L" of " + std::to_wstring(frames) + L" frames";
                return series;
            }
            double weight = 0.0;
            double moment = 0.0;
            for (uint32_t row = 0; row < height; ++row) {
                const uint8_t* line = frame.data() + size_t(row) * width;
                for (uint32_t x = 0; x < width; ++x) {
                    const uint8_t value = line[x];
                    if (value <= kBlackFloor) continue;
                    weight += double(value);
                    moment += double(value) * double(x);
                }
            }
            series.centroids.push_back(weight > 0.0 ? moment / weight
                                                    : std::numeric_limits<double>::quiet_NaN());
        }
        series.ok = true;
        return series;
    };
    const CentroidSeries series = measure();
    fs::remove(scratch, ignored);
    return series;
}

// One pair's judgement: the two source centroids that bound it, and the
// centroids of the frames the pass inserted between them in index order. Pure
// arithmetic on measured pixels and no I/O, so a synthetic centroid series can
// be pushed through exactly this predicate to show what it rejects.
struct PairVerdict {
    bool measurable{};          // the pair is wide enough for a phase to mean anything
    bool held{};
    std::vector<double> phases; // (c - lo) / (hi - lo), one per generated frame
    double worstDeviation{};    // max |phase - k/multiplier| over the pair
    const char* failure{""};    // the first property that missed
};

PairVerdict JudgePair(double lo, double hi, const std::vector<double>& generated, uint32_t multiplier)
{
    PairVerdict verdict;
    // A source frame that could not be located is a broken measurement, not a
    // pair to skip: say so instead of passing quietly.
    if (std::isnan(lo) || std::isnan(hi)) {
        verdict.measurable = true;
        verdict.failure = "sourceUnlocatable";
        return verdict;
    }
    const double span = hi - lo;
    if (std::abs(span) < kMinPairSeparationPx) return verdict; // measurable stays false
    verdict.measurable = true;
    if (multiplier < 2 || generated.size() + 1 != size_t(multiplier)) {
        verdict.failure = "wrongGeneratedCount";
        return verdict;
    }
    // Direction of travel: the probe is a centroid on the x axis, so a clip
    // moving left has hi < lo. Every property is therefore stated on the PHASE,
    // which runs 0 at the older frame to 1 at the newer one whichever way the
    // content moves, and the end margin is stated on the ordered bounds.
    const double low = std::min(lo, hi);
    const double high = std::max(lo, hi);
    bool held = true;
    const auto miss = [&](const char* reason) {
        held = false;
        if (*verdict.failure == '\0') verdict.failure = reason;
    };
    double previousPhase = 0.0;
    for (size_t index = 0; index < generated.size(); ++index) {
        const double centroid = generated[index];
        const double phase = (centroid - lo) / span;
        verdict.phases.push_back(phase);
        const double ideal = double(index + 1) / double(multiplier);
        const double deviation = std::abs(phase - ideal);
        verdict.worstDeviation = std::max(verdict.worstDeviation, deviation);
        // Strictly inside the pair, with room. This is the property the reset
        // bug broke: a byte-identical copy of the newer source frame measured
        // phase 1.002, which is outside the pair altogether. Written as a
        // negated comparison so a NaN centroid misses rather than passes.
        if (!(centroid >= low + kEndMarginPx && centroid <= high - kEndMarginPx)) {
            miss("notStrictlyInside");
        }
        // Ordered: index k+1 of a pair is later in time than index k, so it has
        // to be further along the pair. Three copies of the same frame, or a
        // pair whose middle frames are interchangeable, fails here.
        if (!(phase > previousPhase)) miss("notIncreasing");
        previousPhase = phase;
        // Evenly spaced. Midpoint clustering - every index sitting near 0.5,
        // which is what a blend rather than an interpolation produces - passes
        // both properties above and fails only here.
        if (!(deviation <= kPhaseTolerance)) miss("phaseDeviation");
    }
    verdict.held = held;
    return verdict;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::cout << std::fixed << std::setprecision(3);

    // Every argument is optional and positional, so the only wrong shape is
    // more of them than there are: say what they are rather than silently
    // ignoring one, because a mistyped fifth argument is a run whose audio
    // came from somewhere other than the file the caller meant.
    if (argc > 6) {
        std::wcerr << L"Usage: FrameGenerationSmoke [source] [multiplier] [ffmpegBinDir] [output] "
                      L"[streamSource]\n"
                      L"  streamSource: the file the audio, subtitles and chapters are expected from -\n"
                      L"                the original a video-only neural carrier was rendered from.\n"
                      L"                Omitted means the streams come from <source> itself.\n";
        return 2;
    }
    const fs::path source = argc > 1 ? fs::path(argv[1]) : fs::path(kDefaultSource);
    const uint32_t multiplier = argc > 2 ? uint32_t(std::max(0L, std::wcstol(argv[2], nullptr, 10)))
                                         : kDefaultMultiplier;
    // FFmpeg's directory. Empty searches beside this executable and then PATH,
    // which is what a staged build gets; the CTest registration passes
    // FFMPEG_STAGED_DIR so a build tree that never staged the helpers can still
    // run it against the repository's own copy.
    const fs::path helpers = argc > 3 ? fs::path(argv[3]) : fs::path();
    const fs::path output = argc > 4 ? fs::path(argv[4]) : DefaultOutput();
    // FrameGenerationRequest::streamSource. Empty leaves the field empty,
    // which is the pass's "the streams come from `source`" and this harness's
    // behaviour before the field existed.
    const fs::path streamSource = argc > 5 ? fs::path(argv[5]) : fs::path();
    std::error_code directoryError;
    fs::create_directories(output.parent_path(), directoryError);

    // The source the conversion is actually run on. Carrying the source's audio
    // is the second question this harness asks, because the player starts audio
    // from the file it loaded and a video-only conversion therefore plays
    // silent - and a SILENT source cannot fail that assertion however broken
    // the mux is. So a clip with no audio track is given one here, before
    // anything else happens, and the conversion runs on that instead.
    const fs::path ffmpeg = FfmpegTool(helpers, L"ffmpeg.exe");
    // FFmpeg is no longer optional here: the phase probe below decodes both
    // files back to raw gray through this binary, and that measurement is the
    // assertion this harness exists for. Missing helpers means no question can
    // be asked, which is `unreachable` - not a pass.
    if (ffmpeg.empty()) {
        std::cout << "source=" << Narrow(source.wstring())
                  << "\nexperiment=unreachable reason=ffmpeg.exe was not found beside this executable or "
                     "in the directory given\n";
        return 2;
    }
    // Which file the streams are expected from, chosen exactly the way
    // FrameGenerationPass chooses it (`request.streamSource.empty() ?
    // request.source : request.streamSource`): the fifth argument when there
    // is one, the frame source otherwise. Taking the expected counts off the
    // frame source when a stream source was given is the defect this harness
    // now covers - a video-only carrier has no streams at all, so the check
    // compared 0 against 0 and a silent conversion passed it.
    const bool separateStreamSource = !streamSource.empty();
    const fs::path expectedStreamFile = separateStreamSource ? streamSource : source;
    const MediaStreamSummary givenStreams = SummarizeMediaStreams(helpers, expectedStreamFile, {});
    fs::path conversionSource = source;
    bool synthesizedAudio = false;
    if (!givenStreams.ok) {
        std::cout << "source=" << Narrow(source.wstring())
                  << "\nstreamSource=" << Narrow(expectedStreamFile.wstring())
                  << "\nexperiment=unreachable reason=the streams expected from "
                  << Narrow(expectedStreamFile.filename().wstring()) << " could not be listed: "
                  << Narrow(givenStreams.detail) << "\n";
        return 2;
    }
    // Synthesis is for the case where the frame source is ALSO the stream
    // source: the only file that can be given the tone is the one being
    // converted. A given stream source is the caller answering the same
    // question with a real file, and synthesising onto the frame source would
    // put audio precisely where the mux must not find it - making the
    // carriage assertion vacuous again in the one case it was written for.
    if (!separateStreamSource && givenStreams.audioStreams == 0) {
        const ProbeResult givenProbe = ProbeMedia(helpers, source, {}, MediaProbeMode::CachedMetadata);
        const double seconds = double(givenProbe.duration100ns) / 10000000.0;
        // Named after the converted output, because two registrations of this
        // harness share one directory and must not overwrite each other's
        // intermediates.
        const fs::path audible =
            output.parent_path() / (output.stem().wstring() + L"-source-with-audio.mkv");
        if (!givenProbe.ok || seconds <= 0.0 ||
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
        request.streamSource = streamSource;
        request.output = output;
        request.multiplier = multiplier;
        // The file the carriage assertions are measured against: without a
        // stream source it is the file being converted - the synthesised copy
        // when the given clip was silent - and with one it is the stream
        // source itself, which is the whole point of the field.
        const fs::path streamFile = separateStreamSource ? streamSource : conversionSource;

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
            SummarizeMediaStreams(helpers, streamFile, stopSource.get_token());
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
                  << "streamSource=" << (separateStreamSource ? Narrow(streamSource.wstring())
                                                              : std::string("(same as source)")) << "\n"
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
            // Every audio stream the STREAM SOURCE had has to be in the
            // converted file, and the pass has to report the same number it
            // actually wrote: a caller that shows "audio carried" from a field
            // the pass filled in without looking is no better than the silent
            // output this harness exists to catch. sourceAudioStreams is
            // asserted non-zero because the assertion is vacuous without it,
            // which is why a silent clip is either given a track above or
            // handed an audio-bearing stream source - and why a stream source
            // that carries none fails here instead of passing quietly.
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

            // ---- where the inserted frames actually sit ----------------
            // Measured from pixels, because none of the four properties above
            // can see it. The pass shipped emitting three byte-identical
            // copies of the newer source frame after every reset and every one
            // of them still held.
            //
            // Output layout, from FrameGenerationPass's write order: the older
            // frame of the pair, then the frames generated inside it. So
            // source frame n lands at output index n * multiplier, and pair n
            // (between source n-1 and source n) owns output indices
            // (n-1) * multiplier + k for k = 1 .. multiplier-1.
            bool passthroughHeld = true;
            bool phaseHeld = true;
            double worstPassthroughDeltaPx = 0.0;
            double worstPhaseDeviation = 0.0;
            uint64_t prefixSourceFrames =
                std::min<uint64_t>(kPhasePrefixSourceFrames, latest.sourceFramesRead);
            // Never ask the decoder for more than the file holds: the frame
            // count above allows one frame of slack, and a short clip could
            // otherwise be read past its end and reported as a decode failure.
            if (effectiveMultiplier >= 2 && outputProbe.frameCount / effectiveMultiplier < prefixSourceFrames) {
                prefixSourceFrames = outputProbe.frameCount / effectiveMultiplier;
            }
            if (effectiveMultiplier < 2) {
                // A conversion that inserted nothing has no phase to measure.
                // This is not a decode failure and not a defect either: the
                // pass clamps to the runtime's own DLSSG.MultiFrameCountMax,
                // and a runtime that admits only 1x is reported, not asserted.
                std::cout << "phaseAssertions=skipped reason=this run's effective multiplier is "
                          << effectiveMultiplier << ", so it inserted no frames to locate\n";
            } else if (prefixSourceFrames < 2) {
                std::cout << "phaseAssertions=skipped reason=the source has fewer than two frames, so it "
                             "has no pair for a generated frame to sit inside\n";
            } else if (result.width == 0 || result.height == 0) {
                phaseHeld = false;
                std::cout << "phaseDecode=failed detail=the conversion reported no geometry, so the raw "
                             "frame size is unknown\n";
            } else {
                const uint64_t prefixOutputFrames = prefixSourceFrames * effectiveMultiplier;
                const std::wstring stem = output.stem().wstring();
                const fs::path outputScratch = output.parent_path() / (stem + L"-phase-output.gray");
                const fs::path sourceScratch = output.parent_path() / (stem + L"-phase-source.gray");
                const CentroidSeries outputSeries = DecodeCentroids(ffmpeg, output, result.width,
                                                                    result.height, prefixOutputFrames,
                                                                    outputScratch);
                // The same file the pass read, so the two series are the same
                // frames: the audio-bearing copy stream-copies the video.
                const CentroidSeries sourceSeries = DecodeCentroids(ffmpeg, conversionSource, result.width,
                                                                    result.height, prefixSourceFrames,
                                                                    sourceScratch);
                if (!outputSeries.ok || !sourceSeries.ok) {
                    // A failed decode is a FAIL and never a skip. The harness
                    // cannot tell a broken conversion from a broken decode, and
                    // the broken conversion is precisely what it is here to
                    // catch, so the unmeasurable case has to be the loud one.
                    phaseHeld = false;
                    std::cout << "phaseDecode=failed detail="
                              << Narrow(outputSeries.ok ? sourceSeries.detail : outputSeries.detail) << "\n";
                } else {
                    // The source frames have to pass through the conversion as
                    // themselves. They are the two ends every phase below is
                    // measured against, so a pass that rewrote them would make
                    // the rest of this measurement meaningless - and index
                    // n * multiplier is a frame the pass copied, not one it
                    // generated, so nothing but the re-encode may move it.
                    for (uint64_t frame = 0; frame < prefixSourceFrames; ++frame) {
                        const double emitted = outputSeries.centroids[size_t(frame * effectiveMultiplier)];
                        const double delta = std::abs(emitted - sourceSeries.centroids[size_t(frame)]);
                        worstPassthroughDeltaPx = std::max(worstPassthroughDeltaPx, delta);
                        std::cout << "passthrough_frame" << frame << "DeltaPx=" << delta << "\n";
                    }
                    // Negated form, so an unlocatable frame (NaN) misses here
                    // rather than passing quietly.
                    passthroughHeld = worstPassthroughDeltaPx <= kPassthroughTolerancePx;

                    double sourceLow = sourceSeries.centroids.front();
                    double sourceHigh = sourceLow;
                    for (const double centroid : sourceSeries.centroids) {
                        sourceLow = std::min(sourceLow, centroid);
                        sourceHigh = std::max(sourceHigh, centroid);
                    }
                    const double sourceSpanPx = sourceHigh - sourceLow;
                    std::cout << "sourceCentroidSpanPx=" << sourceSpanPx << "\n";
                    uint64_t measuredPairs = 0;
                    if (sourceSpanPx < kMinPairSeparationPx) {
                        // A clip whose brightness centroid does not move is a
                        // still image to this probe whatever its frame count
                        // says, and a phase measured against a denominator of
                        // noise would be a number with no content. Said out
                        // loud, with the measurement, so a run that skips
                        // cannot be mistaken for a run that held.
                        std::cout << "phaseAssertions=skipped reason=this clip's brightness centroid moves "
                                  << sourceSpanPx << " px across the measured prefix, under the "
                                  << kMinPairSeparationPx
                                  << " px this probe needs, so it is a still image as far as the probe is "
                                     "concerned\n";
                    } else {
                        // Pair 1 is INCLUDED. It is the first pair after the
                        // reset evaluate, which is exactly the case the
                        // multiFrameCount-on-reset bug broke (measured phases
                        // 1.002 / 1.002 / 1.002 at 4x), so warming up past it
                        // would skip the only interval that ever failed.
                        for (uint64_t pair = 1; pair < prefixSourceFrames; ++pair) {
                            std::vector<double> generated;
                            generated.reserve(size_t(effectiveMultiplier - 1));
                            for (uint64_t index = 1; index < effectiveMultiplier; ++index) {
                                generated.push_back(
                                    outputSeries.centroids[size_t((pair - 1) * effectiveMultiplier + index)]);
                            }
                            const PairVerdict verdict =
                                JudgePair(sourceSeries.centroids[size_t(pair - 1)],
                                          sourceSeries.centroids[size_t(pair)], generated,
                                          uint32_t(effectiveMultiplier));
                            if (!verdict.measurable) {
                                std::cout << "phase_pair" << pair
                                          << "=skipped reason=this pair's two source frames are less than "
                                          << kMinPairSeparationPx << " px apart\n";
                                continue;
                            }
                            ++measuredPairs;
                            for (size_t index = 0; index < verdict.phases.size(); ++index) {
                                std::cout << "phase_pair" << pair << "_index" << (index + 1) << "="
                                          << verdict.phases[index] << "\n";
                            }
                            worstPhaseDeviation = std::max(worstPhaseDeviation, verdict.worstDeviation);
                            if (!verdict.held) {
                                phaseHeld = false;
                                std::cout << "phase_pair" << pair << "Failed=" << verdict.failure << "\n";
                            }
                        }
                        if (measuredPairs == 0) {
                            std::cout << "phaseAssertions=skipped reason=no pair in the measured prefix is "
                                         "wide enough for a phase, although the prefix as a whole moves\n";
                        }
                    }
                    std::cout << "phasePairsMeasured=" << measuredPairs << "\n";
                }
            }

            const bool held = frameCountHeld && durationHeld && audioCarriedHeld && audioSyncHeld &&
                              passthroughHeld && phaseHeld;
            std::cout << "expectedFrameCount=" << expectedFrames << "\n"
                      << "frameCountDelta=" << frameDelta << "\n"
                      << "durationToleranceMs=" << durationToleranceMs << "\n"
                      << "frameCountHeld=" << (frameCountHeld ? "true" : "false") << "\n"
                      << "durationHeld=" << (durationHeld ? "true" : "false") << "\n"
                      << "audioCarriedHeld=" << (audioCarriedHeld ? "true" : "false") << "\n"
                      << "audioSyncHeld=" << (audioSyncHeld ? "true" : "false") << "\n"
                      << "worstPassthroughDeltaPx=" << worstPassthroughDeltaPx << "\n"
                      << "worstPhaseDeviation=" << worstPhaseDeviation << "\n"
                      << "passthroughHeld=" << (passthroughHeld ? "true" : "false") << "\n"
                      << "phaseHeld=" << (phaseHeld ? "true" : "false") << "\n"
                      << "experiment=ran\n"
                      << "verdict=" << (held ? "PASS" : "FAIL") << "\n";
            if (!held) exitCode = 1;
        }
    }

    MFShutdown();
    CoUninitialize();
    return exitCode;
}
