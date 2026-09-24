#pragma once

#include "MediaSource.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>

// What an HDR source becomes on its way to the model, and on its way to the
// screen (P3.1).
//
// The neural runtime is SDR: feature 18 takes 8-bit sRGB-encoded frames and the
// cache is 8-bit BT.709. An HDR10 (PQ) or HLG stream used to reach it through
// the same `format=bgra` conversion as any SDR video, which converts the matrix
// and nothing else - so PQ code values were read as SDR gamma: measured on a
// generated PQ ramp, 100-nit diffuse white decoded to 130/255 and a 1000-nit
// highlight to 192, the flat, grey picture every HDR report describes, and
// that is what the model was shown. Every HDR source is now tone mapped to SDR
// BT.709 in the decoder, for every consumer: the player, the offline render and the
// live segments all read the same pixels, so a cached render still pairs
// with the original it was made from.
namespace hdr_policy {

enum class HdrSignal { Sdr, Pq, Hlg };

inline HdrSignal SignalOf(const SourceColorDescription& color)
{
    switch (color.transfer) {
        case ColorTransfer::Pq: return HdrSignal::Pq;
        case ColorTransfer::Hlg: return HdrSignal::Hlg;
        default: return HdrSignal::Sdr;
    }
}

// zimg linearises with this many nits at 1.0, and ffmpeg's `tonemap` peak is
// in the same unit. 100 is the reference the ffmpeg recipe (and Jellyfin's
// CPU path, which ships it) uses: on the same ramp 100 nits lands at 158/255
// and 203 nits (BT.2408 reference white) at 194, below the clip, which leaves
// the curve room to roll the highlights off instead of flattening them.
inline constexpr double kToneMapReferenceNits = 100.0;
// A PQ stream that states neither MaxCLL nor a mastering peak. ffmpeg's own
// fallback is 10000 nits, the top of the PQ range, which compresses a
// typical 1000-nit grade into the bottom third of the curve and plays it dark;
// 1000 is what most HDR10 masters are graded to.
inline constexpr double kDefaultPqPeakNits = 1000.0;
// HLG is relative: zimg decodes it for its nominal 1000-nit display.
inline constexpr double kHlgPeakNits = 1000.0;

// The peak the curve maps to SDR white, decided ONCE per source from its static
// metadata. Per-frame peak detection flickers once frame generation doubles the
// frames between two measurements (P3.1 trap 2), and a peak that ffmpeg read
// from each frame's side data would change the pixels of a frame that happened
// to lose it - so the peak is measured by the probe and passed to the filter as
// a number, and it is part of the cache key. MaxCLL is the content's own
// brightest pixel, so it wins over the mastering display's peak, which only
// bounds it. Values outside 100..10000 nits are not a real grade and are
// ignored rather than trusted.
inline double ToneMapPeakNits(HdrSignal signal, double maxContentNits, double masteringPeakNits)
{
    if (signal == HdrSignal::Hlg) return kHlgPeakNits;
    const auto usable = [](double nits) { return std::isfinite(nits) && nits >= 100.0 && nits <= 10000.0; };
    const double peak = usable(maxContentNits) ? maxContentNits
                      : usable(masteringPeakNits) ? masteringPeakNits : kDefaultPqPeakNits;
    // Whole nits: the value is printed into the filter and into the cache key,
    // and a key must not move on a float's last digit.
    return std::round(peak);
}

inline const wchar_t* ZscaleTransferName(HdrSignal signal)
{
    return signal == HdrSignal::Hlg ? L"arib-std-b67" : L"smpte2084";
}

// The ffmpeg filter chain, without a leading comma, that turns a decoded HDR
// frame into SDR BT.709 R'G'B' for the trailing `format=bgra`. Linear light at
// 100 nits to 1.0, BT.2020 -> BT.709 primaries, Hable with no desaturation (the
// operator the brief allows beside BT.2390, which this ffmpeg's `tonemap` does
// not have), then the BT.709 transfer, which zimg applies display-referred -
// the inverse of BT.1886, i.e. what an SDR master of the same shot contains.
// The input transfer and primaries are stated rather than read from the frame,
// so a hardware download that drops the frame's tags cannot change the result.
// The matrix is read from the frame when the stream declared one, and is
// BT.2020 non-constant-luminance - the HDR10 and HLG matrix - when it did not:
// zimg refuses a YUV frame with no matrix at all.
inline std::wstring SdrToneMapFilter(HdrSignal signal, double peakNits, bool matrixDeclared)
{
    wchar_t peak[32]{};
    std::swprintf(peak, 32, L"%.6f", peakNits / kToneMapReferenceNits);
    return std::wstring(L"zscale=tin=") + ZscaleTransferName(signal) +
           (matrixDeclared ? L"" : L":min=bt2020nc") +
           L":pin=bt2020:t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,"
           L"tonemap=tonemap=hable:desat=0:peak=" + peak +
           L",zscale=t=bt709,format=gbrpf32le";
}

// The cache-key term a render of an HDR source carries: its input pixels are
// the tone-mapped ones now, where a render made before saw PQ code values as
// SDR. The peak is in it because it is in the pixels. Empty for SDR, whose
// keys do not move. v1 is ffmpeg's float chain (SdrToneMapFilter); v2 the
// decoder's own integer pass over a 65-point table (HdrToneMap.h), the same
// curve a code value or two apart, whose bytes do not depend on whether the
// GPU or the CPU ran it - so the device is not in the key.
inline std::string ToneMapIdentityTerm(HdrSignal signal, double peakNits, bool integerTable = false)
{
    if (signal == HdrSignal::Sdr) return {};
    return std::string(integerTable ? "|hdr-sdr-hable-v2-lut65-" : "|hdr-sdr-hable-v1-") +
           (signal == HdrSignal::Hlg ? "hlg" : "pq") + "-peak" +
           std::to_string(static_cast<long long>(std::llround(peakNits)));
}

// The PQ presentation decode: the same HDR frame kept HDR, as ST 2084 BT.2020
// R'G'B' in ten bits, for an HDR display to show as it was graded. HLG is
// converted to PQ on the way (zimg's nominal 1000-nit HLG display), because the
// swapchain has one colour space. The output is planar 10-bit for the trailing
// `-pix_fmt x2bgr10le`, which is R10G10B10A2 byte for byte: four bytes a pixel,
// so the pipe, the frame pool and the upload keep the BGRA sizes.
inline std::wstring PqPresentationFilter(HdrSignal signal, bool matrixDeclared)
{
    return std::wstring(L"zscale=tin=") + ZscaleTransferName(signal) +
           (matrixDeclared ? L"" : L":min=bt2020nc") +
           L":pin=bt2020:t=smpte2084:p=bt2020:npl=100,format=gbrp10le";
}

// --- HDR output ----------------------------------------------------------

// BT.2408's reference white, for an SDR frame on an HDR display when Windows does
// not say what its "SDR content brightness" is.
inline constexpr float kReferenceWhiteNits = 203.0f;

// DISPLAYCONFIG_SDR_WHITE_LEVEL::SDRWhiteLevel is a multiplier of 80 nits in
// thousandths (1000 = 80 nits); 0 is "not answered". The Windows slider spans
// 80..480 nits, so anything outside it is not a setting.
inline float SdrWhiteNits(uint32_t sdrWhiteLevel)
{
    if (sdrWhiteLevel == 0) return kReferenceWhiteNits;
    const float nits = static_cast<float>(sdrWhiteLevel) * 80.0f / 1000.0f;
    return std::clamp(nits, 80.0f, 480.0f);
}

struct DesktopRect {
    long left{}, top{}, right{}, bottom{};
};

// The output a window is on: the one holding the largest part of it, the rule
// DXGI's own GetContainingOutput follows, computed here from the desktop
// rectangles so it can be asked again after every move without a swapchain and
// tested without a monitor. -1 when the window is on none of them - minimised
// windows sit at -32000.
inline int OutputUnderWindow(const DesktopRect& window, const DesktopRect* outputs, size_t count)
{
    int best = -1;
    long long bestArea = 0;
    for (size_t index = 0; index < count; ++index) {
        const DesktopRect& output = outputs[index];
        const long long width = static_cast<long long>(std::min(window.right, output.right)) - std::max(window.left, output.left);
        const long long height = static_cast<long long>(std::min(window.bottom, output.bottom)) - std::max(window.top, output.top);
        if (width <= 0 || height <= 0) continue;
        if (width * height > bestArea) {
            bestArea = width * height;
            best = static_cast<int>(index);
        }
    }
    return best;
}

// What the original member is decoded as while it is on screen. PQ - the HDR
// picture - only when all of these hold:
//  * the swapchain is HDR: an SDR one would clip it;
//  * the source is HDR;
//  * the viewer has not asked to compare at SDR, which shows the original
//    exactly as the model saw it, tone mapped;
//  * nothing reads the decoded frame as SDR input: the live upscaler and the
//    guide estimator both take 8-bit SDR;
//  * the view does not compute with the original's values (the Mix, the mask,
//    Difference, 2x2): mixing PQ light with the neural frame's SDR light, or
//    subtracting one from the other, measures the tone map rather than the
//    model, so those views compare at SDR.
struct OriginalPresentation {
    bool hdrSwapchain{};
    bool sourceHdr{};
    bool compareAtSdr{};
    bool sourceFeedsSdrConsumer{};
    bool viewCombinesPixels{};
};

inline bool DecodeOriginalAsPq(const OriginalPresentation& state)
{
    return state.hdrSwapchain && state.sourceHdr && !state.compareAtSdr && !state.sourceFeedsSdrConsumer &&
           !state.viewCombinesPixels;
}

} // namespace hdr_policy
