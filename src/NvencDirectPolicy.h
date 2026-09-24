#pragma once

#include "MediaPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>

#ifdef DLSS_VIDEO_PLAYER_HAS_NVENC
#include <nvEncodeAPI.h>
#endif

// The pure decisions of the direct NVENC path (NvencDirect.cpp), apart from the
// driver so they can be tested without a GPU.
//
// The direct path encodes the neural capture straight from its D3D12 planes. The
// ffmpeg child it replaces stays the fallback, so the one requirement the path
// is built around is that it writes THE SAME BITS: a render must not become a
// different file, and must not need a new cache key, because the encoder moved
// out of a child process. NVENC is deterministic for a given configuration and
// input, so identity comes down to handing it the configuration hevc_nvenc
// builds from BuildEncoderArguments' options - which is what ApplyHevcNvencOptions
// reproduces, field by field, in the order FFmpeg's nvenc.c applies them - and
// the same pixels, which the capture shaders already produce for both paths.
namespace nvenc_direct {

// Why a render is not a candidate for the direct path. Everything but None keeps
// the ffmpeg child, which is also exactly what every render did before it.
enum class Ineligible {
    None,
    // The benchmark switch said ffmpeg.
    Forced,
    // FFV1 (Lossless) and libx264 are CPU encoders; only hevc_nvenc has a
    // D3D12 twin.
    NotNvenc,
    // An 8-bit BGRA capture. ffmpeg converts it to 4:2:0 with swscale on the
    // CPU, and NVENC's own RGB conversion is a different matrix and filter, so
    // the direct path could not write the same pixels. That is the Standard
    // rung's default (GPU colour conversion off, docs/measurements/
    // gpu-readback-20260914/), which therefore keeps the child.
    PackedCapture,
    // Live sessions write one file per segment through SegmentWriter, whose
    // warm-start machinery is built around one ffmpeg per file. Single-file
    // renders - every cache render and export - are the throughput case.
    Segmented,
};

// Whether the direct path is even tried; nvenc_direct::DriverAvailable and the
// session's own start decide whether it then runs.
constexpr Ineligible Eligibility(EncoderKind kind, EncoderPixelFormat capture, bool segmented,
                                 EncoderPath path)
{
    if (path == EncoderPath::Ffmpeg) return Ineligible::Forced;
    if (kind != EncoderKind::HevcNvenc) return Ineligible::NotNvenc;
    if (capture == EncoderPixelFormat::Bgra) return Ineligible::PackedCapture;
    if (segmented) return Ineligible::Segmented;
    return Ineligible::None;
}

constexpr std::string_view IneligibleName(Ineligible reason)
{
    switch (reason) {
    case Ineligible::None: return "eligible";
    case Ineligible::Forced: return "forced to the ffmpeg child";
    case Ineligible::NotNvenc: return "not an NVENC encode";
    case Ineligible::PackedCapture: return "8-bit BGRA capture, converted by ffmpeg";
    case Ineligible::Segmented: return "segmented live output";
    }
    return "unknown";
}

// ---- FFmpeg's rational arithmetic -------------------------------------------
// The child is told the frame rate as FrameRateText's decimal, and what it
// encodes with is what av_parse_video_rate makes of that text: the decimal
// through av_d2q with a bound of 1001000. That rational is both the rate NVENC
// is configured with (frameRateNum/Den, which its rate control reads) and the
// time base the packet timestamps are in, so the direct path has to arrive at
// the same one - "the fps as a fraction" is not it. Measured on the shipped
// ffmpeg: "29.97003" becomes 979001/32666 and "23.976024" 991001/41333, not
// 30000/1001 and 24000/1001, and frame 12 of the latter is stamped 500 ms where
// 24000/1001 would say 501.
struct Rational {
    int64_t num{};
    int64_t den{};
    friend constexpr bool operator==(const Rational&, const Rational&) = default;
};

// av_reduce: the best approximation of num/den with both terms within `max`.
inline Rational Reduce(int64_t num, int64_t den, int64_t max)
{
    Rational a0{0, 1}, a1{1, 0};
    const bool negative = (num < 0) != (den < 0);
    num = num < 0 ? -num : num;
    den = den < 0 ? -den : den;
    const int64_t gcd = std::gcd(num, den);
    if (gcd) {
        num /= gcd;
        den /= gcd;
    }
    if (num <= max && den <= max) {
        a1 = {num, den};
        den = 0;
    }
    while (den) {
        const uint64_t x = uint64_t(num / den);
        const int64_t nextDen = num - den * int64_t(x);
        const int64_t a2n = int64_t(x) * a1.num + a0.num;
        const int64_t a2d = int64_t(x) * a1.den + a0.den;
        if (a2n > max || a2d > max) {
            uint64_t bounded = x;
            if (a1.num) bounded = uint64_t((max - a0.num) / a1.num);
            if (a1.den) bounded = std::min<uint64_t>(bounded, uint64_t((max - a0.den) / a1.den));
            if (den * (2 * int64_t(bounded) * a1.den + a0.den) > num * a1.den)
                a1 = {int64_t(bounded) * a1.num + a0.num, int64_t(bounded) * a1.den + a0.den};
            break;
        }
        a0 = a1;
        a1 = {a2n, a2d};
        num = den;
        den = nextDen;
    }
    return {negative ? -a1.num : a1.num, a1.den};
}

// av_d2q.
inline Rational DoubleToRational(double value, int64_t max)
{
    if (std::isnan(value)) return {0, 0};
    if (std::fabs(value) > double(std::numeric_limits<int32_t>::max()) + 3.0)
        return {value < 0 ? -1 : 1, 0};
    // Only the exponent is wanted; the mantissa frexp returns is not.
    int exponent = 0;
    (void)std::frexp(value, &exponent);
    exponent = std::max(exponent - 1, 0);
    const int64_t den = int64_t{1} << (62 - exponent);
    Rational result = Reduce(int64_t(std::floor(value * double(den) + 0.5)), den, max);
    if ((!result.num || !result.den) && value != 0.0 && max > 0 && max < std::numeric_limits<int32_t>::max())
        result = Reduce(int64_t(std::floor(value * double(den) + 0.5)), den, std::numeric_limits<int32_t>::max());
    return result;
}

// What `-framerate <FrameRateText(fps)>` becomes inside the child: the same text
// parsed back and reduced as av_parse_video_rate does it.
inline Rational ChildFrameRate(double fps)
{
    const std::wstring text = FrameRateText(fps);
    return DoubleToRational(std::wcstod(text.c_str(), nullptr), 1001000);
}

// av_rescale_q(frame, 1/rate, 1/1000) with AV_ROUND_NEAR_INF: the millisecond
// timestamp Matroska stores for the frame-th frame, which is the only timestamp
// the cache file keeps.
inline int64_t FrameMilliseconds(uint64_t frame, Rational rate)
{
    if (rate.num <= 0 || rate.den <= 0) return 0;
    // a * b / c with b = 1000 * den and c = num, rounded half away from zero.
    const int64_t numerator = int64_t(frame) * 1000 * rate.den;
    const int64_t quotient = numerator / rate.num, remainder = numerator % rate.num;
    return quotient + (2 * remainder >= rate.num ? 1 : 0);
}

// Matroska's DefaultDuration for the rate, in nanoseconds, the way FFmpeg's
// muxer writes it: 1/rate, truncated.
inline uint64_t DefaultDurationNanoseconds(Rational rate)
{
    if (rate.num <= 0 || rate.den <= 0) return 0;
    return uint64_t(rate.den * 1'000'000'000 / rate.num);
}

// ---- Surface accounting -----------------------------------------------------
// The capture copies each frame into one of a pool of NV12/P010 surfaces and
// NVENC reads it from there, so a surface is busy from the capture copy until
// NVENC has coded that frame. The render thread takes a surface for every
// capture and waits when none is free, so the pool must be deep enough that the
// frames holding surfaces never depend on the render thread to finish:
//  * the capture ring's other slots, whose frames the render thread still owns;
//  * the B-frames NVENC holds back for their next anchor (frameIntervalP - 1),
//    whose anchor may be one of those ring frames;
//  * the lookahead, which keeps that many more inputs before it codes one;
//  * the frames whose output the encoder has not locked yet (lockDepth);
// plus the one being captured and two frames of slack so a jittery encode does
// not stall the render thread. Measured need is the same as FFmpeg's own input
// surface count for this configuration, max(4, 4 * frameIntervalP).
inline uint32_t SurfacePoolSize(uint32_t captureSlots, int32_t frameIntervalP, uint32_t lookahead,
                                uint32_t lockDepth)
{
    const uint32_t held = uint32_t(std::max(frameIntervalP, 1) - 1) + lookahead;
    return captureSlots + held + lockDepth + 2u;
}

// Output bitstream buffers the session cycles through. One is attached to every
// submission and stays taken until its picture is locked, so this bounds what is
// submitted and not yet locked: the held-back B-frames and lookahead, the lock
// depth, and the frame being submitted.
inline uint32_t OutputBufferCount(int32_t frameIntervalP, uint32_t lookahead, uint32_t lockDepth)
{
    return uint32_t(std::max(frameIntervalP, 1)) + lookahead + lockDepth + 1u;
}

// Bytes one output bitstream buffer is given. NVENC truncates a picture larger
// than its buffer, so it is sized for the worst case a CQ 14-16 encode can
// produce rather than the average: a whole uncompressed frame of the input
// format, which an HEVC intra picture at these CQs never approaches (the
// largest measured 1080p frame is 1.8 MB at FFV1 lossless), plus a floor for
// the tiny sizes a test renders.
inline uint32_t OutputBufferBytes(uint32_t width, uint32_t height, bool tenBit)
{
    const uint64_t frame = EncoderFrameBytes(tenBit ? EncoderPixelFormat::P010 : EncoderPixelFormat::Nv12,
                                             width, height);
    return uint32_t(std::min<uint64_t>(std::max<uint64_t>(frame, 2u * 1024u * 1024u),
                                       std::numeric_limits<uint32_t>::max() / 2u));
}

#ifdef DLSS_VIDEO_PLAYER_HAS_NVENC

inline const GUID& PresetGuid(uint32_t preset)
{
    switch (std::clamp<uint32_t>(preset, 1u, 7u)) {
    case 1: return NV_ENC_PRESET_P1_GUID;
    case 2: return NV_ENC_PRESET_P2_GUID;
    case 3: return NV_ENC_PRESET_P3_GUID;
    case 4: return NV_ENC_PRESET_P4_GUID;
    case 6: return NV_ENC_PRESET_P6_GUID;
    case 7: return NV_ENC_PRESET_P7_GUID;
    default: break;
    }
    return NV_ENC_PRESET_P5_GUID;
}

// hevc_nvenc's defaults that BuildEncoderArguments does not override, from
// FFmpeg's nvenc_hevc.c and AVCodecContext: g=250, bf=-1 (keep the preset's
// B-frames), refs=0, i_quant_factor=-0.8, b_quant_factor=1.25 with offsets 0
// and 1.25, multipass disabled, no AQ, no lookahead of its own, delay (async
// depth) unbounded, surfaces automatic.
inline constexpr uint32_t kFfmpegGopSize = 250;
inline constexpr uint32_t kFfmpegMaxRegisteredFrames = 64;

struct Plan {
    // What FFmpeg would hand nvEncInitializeEncoder, and the two numbers the
    // direct session sizes itself from.
    uint32_t lookahead{};
    int32_t frameIntervalP{1};
};

// Applies to a preset configuration exactly what hevc_nvenc applies for the
// options BuildEncoderArguments passes it (-preset pN -tune hq -rc vbr -cq Q
// -b:v 0 -maxrate 800M -bufsize 800M -split_encode_mode auto, -profile:v main10
// on the 10-bit rungs, the BT.709 limited-range colour options, Matroska's
// global header), in the order nvenc.c applies them - nvenc_setup_encoder,
// nvenc_recalc_surfaces, nvenc_setup_rate_control, nvenc_setup_hevc_config -
// because several steps read what an earlier one wrote. `config` must hold the
// preset configuration nvEncGetEncodePresetConfigEx returned for the same
// preset and tuning. The D3D12-only bufferFormat is the one field FFmpeg never
// sets, because its sessions are CUDA ones.
inline Plan ApplyHevcNvencOptions(const EncoderSpec& spec, Rational rate,
                                  NV_ENC_INITIALIZE_PARAMS& init, NV_ENC_CONFIG& config)
{
    const bool tenBit = EncoderQualityIsTenBit(spec.quality);
    Plan plan;
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
    init.presetGUID = PresetGuid(spec.nvencPreset);
    init.encodeWidth = spec.width;
    init.encodeHeight = spec.height;
    init.encodeConfig = &config;
    init.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    config.version = NV_ENC_CONFIG_VER;
    // compute_dar: no sample aspect ratio on a rawvideo input, so the display
    // aspect is the frame's own, reduced.
    const Rational dar = Reduce(spec.width, spec.height, 1024 * 1024);
    init.darWidth = uint32_t(dar.num);
    init.darHeight = uint32_t(dar.den);
    init.frameRateNum = uint32_t(rate.num);
    init.frameRateDen = uint32_t(rate.den);
    init.enableEncodeAsync = 0;
    init.enablePTD = 1;
    // "If lookahead isn't set from CLI, use value from preset."
    uint32_t lookahead = config.rcParams.enableLookahead ? config.rcParams.lookaheadDepth : 0u;
    init.splitEncodeMode = NV_ENC_SPLIT_AUTO_MODE;
    // A GOP size was set (the codec default, 250) and max_b_frames is -1, so the
    // preset's frameIntervalP stands and only the length changes.
    config.gopLength = kFfmpegGopSize;

    // nvenc_recalc_surfaces
    const int32_t fip = config.frameIntervalP;
    int64_t surfaces = std::max<int64_t>(4, int64_t(fip) * 2 * 2);
    if (lookahead > 0) surfaces = std::max<int64_t>(1, std::max<int64_t>(surfaces, int64_t(lookahead) + fip + 1 + 4));
    surfaces = std::clamp<int64_t>(surfaces, 1, kFfmpegMaxRegisteredFrames);
    const int64_t asyncDepth = surfaces - 1;

    // nvenc_setup_rate_control
    NV_ENC_RC_PARAMS& rc = config.rcParams;
    // -b:v 0: no average bitrate of its own, so a preset average becomes the ceiling...
    if (rc.averageBitRate > 0) rc.maxBitRate = rc.averageBitRate;
    // ...which -maxrate then replaces.
    constexpr uint32_t kMaxRate = 800'000'000;
    rc.maxBitRate = kMaxRate;
    rc.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    // set_vbr with no qmin/qmax: initial QPs 26 for P, and from the codec's
    // default quant factors 21 for I and 34 for B.
    rc.enableInitialRCQP = 1;
    rc.initialRCQP.qpInterP = 26;
    rc.initialRCQP.qpIntra = uint32_t(std::clamp(int(26 * 0.8 + 0.0 + 0.5), 0, 51));
    rc.initialRCQP.qpInterB = uint32_t(std::clamp(int(26 * 1.25 + 1.25 + 0.5), 0, 51));
    rc.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    rc.vbvBufferSize = kMaxRate;  // -bufsize, discarded again by -cq below
    if (lookahead > 0) {
        const int64_t bound = std::min(surfaces, asyncDepth) - fip - 4;
        if (bound < 0) {
            rc.enableLookahead = 0;
            lookahead = 0;
        } else {
            rc.enableLookahead = 1;
            rc.lookaheadDepth = uint16_t(std::clamp<int64_t>(lookahead, 0, bound));
            rc.disableIadapt = 0;
            rc.disableBadapt = 0;
            lookahead = rc.lookaheadDepth;
        }
    }
    // -cq: 8.8 fixed point, and constant quality discards the average rate and
    // the buffer, honouring only the ceiling.
    const float quality = float(EncoderQualityIsTenBit(spec.quality) ? kHighRungCq : 16u);
    const int fixed = int(quality * 256.0f);
    rc.targetQuality = uint8_t(fixed >> 8);
    rc.targetQualityLSB = uint8_t(fixed & 0xff);
    rc.averageBitRate = 0;
    rc.vbvBufferSize = 0;
    rc.maxBitRate = kMaxRate;

    config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;

    // nvenc_setup_hevc_config
    NV_ENC_CONFIG_HEVC& hevc = config.encodeCodecConfig.hevcConfig;
    NV_ENC_CONFIG_HEVC_VUI_PARAMETERS& vui = hevc.hevcVUIParameters;
    vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    vui.videoFullRangeFlag = 0;
    vui.colourDescriptionPresentFlag = 1;
    vui.videoSignalTypePresentFlag = 1;
    hevc.sliceMode = 3;
    hevc.sliceModeData = 1;
    // Matroska asks for a global header, so the parameter sets go into the
    // codec private data (nvEncGetSequenceParams) and never into a packet.
    hevc.disableSPSPPS = 1;
    hevc.repeatSPSPPS = 0;
    hevc.outputAUD = 0;
    hevc.maxNumRefFramesInDPB = 0;
    hevc.idrPeriod = config.gopLength;
    hevc.outputPictureTimingSEI = 1;
    config.profileGUID = tenBit ? NV_ENC_HEVC_PROFILE_MAIN10_GUID : NV_ENC_HEVC_PROFILE_MAIN_GUID;
    hevc.chromaFormatIDC = 1;
    hevc.inputBitDepth = tenBit ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
    hevc.outputBitDepth = tenBit ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
    hevc.level = 0;  // auto
    hevc.tier = 0;   // main
    hevc.numRefL0 = NV_ENC_NUM_REF_FRAMES_AUTOSELECT;
    hevc.numRefL1 = NV_ENC_NUM_REF_FRAMES_AUTOSELECT;

    init.bufferFormat = tenBit ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
    plan.lookahead = lookahead;
    plan.frameIntervalP = fip;
    return plan;
}

#endif

} // namespace nvenc_direct
