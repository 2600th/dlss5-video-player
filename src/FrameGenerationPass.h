#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

// DLSS Frame Generation as an OFFLINE conversion, not a presentation path.
//
// Streamline's DLSS-G owns the swapchain and pairs generation with pacing: it
// decides when each generated frame is shown. This player holds its own
// swapchain and reaches NGX directly, and the raw path it reaches has only the
// generation half - DLSSGBackend::Evaluate hands back an interpolated texture
// and nothing presents it. So the frames are written to a new file at N times
// the source rate while the conversion's progress is shown, and the player then
// opens that file through the ordinary decode path. This is also why the pass
// never upscales: NVIDIA's own pipeline runs Super Resolution first and Frame
// Generation on the upscaled result, so this pass consumes whatever video it is
// handed - the original or an already neural-rendered one - and only changes
// the frame rate.
//
// What the pass is allowed to claim rests on tests/DlssgEvaluateSmoke.cpp,
// measured 2026-09-17 on an RTX 5090 / driver 616.64 / Windows 11 26200. The
// raw evaluate produces a real intermediate frame: a 200x200 square translated
// exactly +200 px between two 1920x1080 frames came back with its horizontal
// centroid at 812.73 against 699.50 in the older frame and 899.50 in the newer,
// soft-edged over a 736..894 span, with all 2073600 pixels written - an
// interpolation, not a cross-fade of the pair, and biased 13.2 px past the
// 799.50 midpoint toward the newer frame.
//
// It is NOT produced from the tagged DLSSG.MVecs. The same pair was evaluated
// with motion in backbuffer pixels, in normalized screen units, and zeroed -
// a buffer claiming nothing moved while the colour pair jumps 200 px - and all
// three produced the same frame (centroids 812.73 / 812.73 / 812.79). This pass
// therefore hands over a zero motion buffer and a flat depth proxy and spends
// nothing on motion estimation, and nothing may claim that a better motion
// estimate buys a better generated frame until a runtime is measured that reads
// those vectors.
//
// Hard prerequisite, measured the same day: nvngx_dlssg.dll must be resolvable
// beside the running executable. Without it CreateFeature(FrameGeneration)
// answers 0xbad0000b even though NGX locates and logs the driver-store copy.

enum class FrameGenerationError { None, InvalidRequest, Source, Device, Runtime, Encoder, Cancelled };

struct FrameGenerationRequest {
    std::filesystem::path source;
    // Where the audio, subtitles and chapters are copied from, when that is a
    // different file from the one the frames come from. Empty means `source`.
    // The player converts the NEURAL RENDER when that view is on screen, and
    // this project writes those carriers video-only (`-an`), so the streams
    // have to come from the original the carrier was rendered from - otherwise
    // the converted file is silent and the pass's own audio check passes by
    // comparing zero streams against zero. It is only ever the original of the
    // same source: the carrier is admissible only when it covers the whole
    // source, so both files have the same length and the copy needs no retime.
    std::filesystem::path streamSource;
    std::filesystem::path output;
    uint32_t multiplier{};        // >= 2; generated frames per source frame is multiplier - 1
    // Default p5; the trade is measured beside EncoderSpec::nvencPreset in
    // MediaPipeline.h, and this pass is where it was measured end to end.
    uint32_t nvencPreset{5};
    // Hold the older frame instead of emitting generated ones when a pair is the
    // same picture twice - animation drawn on twos or threes, a telecine or capture
    // repeat (scene_cut::IsDuplicateDecodedPair). Off by default, because on the
    // runtime measured it buys nothing visible: real-game-motion re-timed onto twos
    // and converted 2x on an RTX 4080 SUPER, the frame DLSS-G generated between two
    // copies of one frame sat 0.025 of a code (mean absolute) from the held frame -
    // the same distance NVENC puts between two encodes of that one frame - while the
    // detector's error on real motion is a held pair where one was wanted
    // (tools/benchmark/duplab.py). FrameGenerationSmoke's seventh argument runs it.
    bool holdDuplicates{false};
};

struct FrameGenerationProgress {
    uint64_t sourceFramesRead{};
    uint64_t framesWritten{};
    uint64_t sourceFramesTotal{};  // 0 when the source duration is unreadable
    double sourceSeconds{};
};

struct FrameGenerationResult {
    bool ok{};
    FrameGenerationError error{FrameGenerationError::None};
    std::wstring detail;
    uint32_t width{}, height{};
    double sourceFps{}, outputFps{};
    // framesWritten is sourceFramesRead * multiplier exactly, which is what
    // keeps the output the same length as the source; generatedFrames is how
    // many of them DLSS-G produced, and the remainder is the source frames
    // themselves plus the held tail described on Run below. evaluations is the
    // backend's own count, so it also includes the history-establishing
    // evaluates whose output is discarded.
    uint64_t framesWritten{}, generatedFrames{}, evaluations{};
    // What the finished file carries beside the generated video. The pass
    // encodes video only and then stream-copies the source's audio, subtitles
    // and chapters onto it, so these are read back off the muxed file rather
    // than assumed: a conversion that ran and reports audioCarried=false is
    // either a silent source, which is valid, or a caller-visible defect.
    bool audioCarried{};
    uint32_t outputAudioStreams{}, outputSubtitleStreams{};
    // Pairs the scene-cut criterion refused to generate across, so a caller can
    // tell "nothing was generated" apart from "this film is cut quickly". Held
    // frames, not generated ones: each cut contributes multiplier-1 repeats and
    // therefore lowers generatedFrames without changing framesWritten.
    uint64_t sceneCuts{};
    // Pairs held because they were duplicates (request.holdDuplicates). Like a cut,
    // each contributes multiplier-1 held frames and no generated ones; unlike a cut,
    // the evaluates still ran, so the runtime's history saw every frame.
    uint64_t duplicateHolds{};
};

class FrameGenerationPass {
public:
    explicit FrameGenerationPass(std::filesystem::path helperDirectory = {});

    // Converts `request.source` into `request.output` at multiplier times its
    // frame rate, at the source geometry, and reports what it produced. The
    // progress callback is optional and runs on the calling thread inside Run,
    // at most once every 250 ms plus a final report, the same shape
    // MediaMaterializer::Run uses.
    //
    // Emission order, per source frame N, is presentation order: source frame
    // N-1 first, then the multiplier-1 frames generated between N-1 and N, then
    // N becomes the next iteration's N-1. The two ends are the cases that order
    // does not cover on its own. The FIRST source frame has no predecessor, so
    // it gets one evaluate with reset=true whose output is discarded - it
    // establishes the temporal history the next evaluate interpolates from, and
    // a frame generated before the first frame would sit between nothing and
    // something. The LAST source frame has no successor, so there is nothing to
    // interpolate toward and its slot is filled by holding it for multiplier-1
    // frames. The count therefore comes out at exactly sourceFrames *
    // multiplier, which is what makes the output the same duration as the
    // source: sourceFrames*multiplier / (sourceFps*multiplier) is
    // sourceFrames/sourceFps.
    //
    // A decoder discontinuity is treated the same way as the first frame: the
    // pair straddling it is not a pair, so the evaluate across it is a reset
    // whose output is discarded and the slot is held instead.
    //
    // A SCENE CUT is treated identically, and is detected here. An earlier
    // version of this comment said no detector should be added and that
    // TemporalGuides - which classifies cuts for Super Resolution - was where a
    // later slice should take the signal from. The second half was right and is
    // what happened: the criterion moved to `src/SceneCut.h`, thresholds and
    // measurements intact, and both consumers now include it. The first half
    // was wrong. Generating across a cut blends two unrelated shots into every
    // slot between them - at 2x on 24 fps film, one morphed frame per edit -
    // and it is the most visible defect this pass can produce.
    //
    // The pass only gets to use the histogram arm of that criterion, because it
    // has no correspondence stage to produce an aligned residual with; see
    // `IsCutBetweenDecodedFrames`. `FrameGenerationResult::sceneCuts` reports
    // how many pairs were refused.
    //
    // The frames are encoded into a video-only staging file beside
    // `request.output`, and `request.output` is then MUXED from that video plus
    // the source's audio, subtitle and chapter streams, stream-copied. Audio
    // is what makes the mux a stage rather than an option: the player opens the
    // converted file through its ordinary decode path and starts audio from the
    // file it loaded, so a video-only output plays silent. A plain stream copy
    // is correct here precisely because the paragraph above holds - the
    // generated video is the same length as the source (measured 8.000 s for
    // both on the 30 fps clip at 4x) - so the source's audio needs no
    // stretching, resampling or offset to line up with it. Nothing in this pass
    // may change the output's length without changing that copy into a retime.
    // A source with no audio still produces a valid output, and on any failure
    // the staging file and the output are both removed rather than left behind.
    FrameGenerationResult Run(const FrameGenerationRequest& request, std::stop_token stop,
                              std::function<void(const FrameGenerationProgress&)> onProgress = {});

private:
    std::filesystem::path helperDirectory_;
};

// What the runtime admits, for a caller that has to choose a multiplier BEFORE
// it can run a conversion. DLSSG.MultiFrameCountMax is the runtime's own number
// (5 on the RTX 5090 / 616.64 measured here, so multipliers up to 6x) and
// frame_rate_policy::PlanFrameGeneration takes it as an input rather than
// assuming one, so this has to be measured and not guessed.
struct FrameGenerationCapability {
    bool available{};
    uint32_t multiFrameCountMax{};  // 0 when the runtime admits no generation at all
    std::wstring detail;
};

// Creates a throwaway D3D12 device on the same adapter the pass would convert
// on, runs DLSSGBackend::Probe at 1920x1080 B8G8R8A8_UNORM - which creates the
// feature and reads the cap - then submits and retires the create before it
// releases the feature, and tears everything down.
// Safe on a worker thread and never throws: any failure returns available=false
// with the hex NVSDK_NGX_Result in detail. The answer cannot change without a
// driver or GPU change, so a caller is expected to ask once and cache it for
// the process lifetime; each call costs an NGX init and a feature create.
FrameGenerationCapability QueryFrameGenerationCapability() noexcept;

namespace frame_generation_detail {

// What a submission whose bounded wait did not see its fence turned out to be.
// The pass used to return straight away, and every resource the submission
// referenced - textures, readbacks, upload heaps, the DLSS-G feature - was
// released on the way out while the GPU could still be executing it.
enum class StalledSubmission {
    Retired,        // slow, not stuck: it finished inside the longer wait
    DeviceRemoved,  // nothing is executing any more, so freeing is safe
    Stuck,          // alive and still busy: leak everything, free nothing
};

// `deviceRemoved` asks the device (GetDeviceRemovedReason); `waitLonger`
// waits again under a longer bound and says whether the fence was reached.
// Removal is asked first, because a removed device's fence never advances and
// a second wait on it would only delay the answer, and again after the wait,
// because a device lost during it is not a stuck one.
template <class DeviceRemoved, class WaitLonger>
StalledSubmission ResolveStalledSubmission(DeviceRemoved&& deviceRemoved, WaitLonger&& waitLonger)
{
    if (deviceRemoved()) return StalledSubmission::DeviceRemoved;
    if (waitLonger()) return StalledSubmission::Retired;
    return deviceRemoved() ? StalledSubmission::DeviceRemoved : StalledSubmission::Stuck;
}

} // namespace frame_generation_detail
