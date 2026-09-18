#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

// The scene-cut criterion, as one place, for the two consumers that need the
// same answer from different evidence.
//
// `TemporalGuideGenerator` asks it about a pair it has ALIGNED: its residual is
// a post-alignment global match cost, so a pan that moved the whole frame still
// scores low. `FrameGenerationPass` asks it about a pair it has only decoded -
// there is no correspondence stage in an offline conversion - so its residual
// is the raw difference and a pan scores high. That difference is why the two
// arms below are separable, and why the pass takes only one of them; see
// `MeasureDecodedPair`.
//
// This file is the same shape as FlowGate.h and for the same reason: a
// criterion with measured constants belongs in one header that every consumer
// includes, not copied into each of them with the numbers drifting apart.
namespace scene_cut {

// Strength of the image evidence behind a scene-cut decision. The two arms are
// kept apart because only the weak one is debounced: NVIDIA's DLSS Programming
// Guide 310.6.0 S3.13 asks for InReset on the first frame after a major
// transition and warns that improper use "can result in temporal flickering,
// heavy aliasing or other visual artifacts", so a false positive is far more
// expensive than a late true positive - but a strong signal must still cut
// immediately, which is also what x264/x265 do (their distance-ramped scenecut
// threshold never blocks a decisive cut).
enum class Strength {
    None,       // the frames correspond; history continues
    Histogram,  // weak arm: a moderate residual plus a collapsed luma histogram
    Residual,   // strong arm: correspondence failed outright
};

// Measured on the benchmark corpus: fast pans reach residual 0.10-0.13 with histogram
// overlap >= 0.91; real cuts show residual 0.24-0.40 with overlap <= 0.47, and the softest
// real cut observed was 0.108 / 0.78. Pairing a residual with a histogram distance under a
// threshold band is x265's --hist-scenecut design; NVIDIA documents no threshold and no
// detection method at all, so none of these numbers can be attributed to them.
inline constexpr double kResidualStrong = 0.30;    // correspondence failed outright
inline constexpr double kResidualWeak = 0.10;      // more than a pan, less than a certainty
inline constexpr double kHistogramOverlap = 0.85;  // luma distribution no longer the same scene

// The weak arm's minimum interval. It exists because a transient - a flash, an exposure
// step - fires the weak arm twice, once going in and once coming back out, and per DLSS
// Programming Guide 310.6.0 S3.13 over-firing InReset is the documented failure mode
// ("temporal flickering, heavy aliasing or other visual artifacts"), not a missed reset.
// A contributor measured 6 fires in 12 frames (#19119-19130) and 11 in 15 frames on
// another clip. FFmpeg's scdet has no debounce; it also never has to protect an
// upscaler's accumulated history.
//
// It was PySceneDetect's min_scene_len CLI default of 0.6 s, which is 18 frames at 30 fps
// and long enough to discard a real discontinuity: on a labelled capture a hard cut
// verified frame by frame, 17 frames after the previous one, fired the weak arm
// (residual 0.2711, overlap 0.5294) and was withheld by construction. The labelled
// corpus brackets the window from both sides, and the bracket is wide:
//   * the only transient in it returns 4 frames after the cut that opened it
//     (residual 0.2537, overlap 0.4324), so the window must exceed 4 frames;
//   * the shortest span between two labelled discontinuities is 17 frames, so the
//     window must not exceed that.
// That 17-frame span is bounded above by the capture's own scene change rather than by
// a film edit, so it is evidence that a reset must follow a discontinuity 17 frames
// after its predecessor - not evidence about how fast footage is cut.
// Nothing else in the corpus changes anywhere in between - the dissolves never reach the
// weak arm at all, so they are protected by kResidualWeak and not by this window.
// Note that the transient's return has the *lower* histogram overlap of the two, so no
// pair of evidence thresholds orders the two cases: the interval is the only thing that
// separates them, which is why this is still a plain minimum interval and not a
// strength-conditional rule. 0.3 s sits between the two bounds with roughly equal
// multiplicative margin on each side (9 frames at 30 fps: 2.25x the observed transient,
// 0.53x the shortest labelled span). The cost is that the burst defence is now 9 frames
// wide rather than 18, so a transition that keeps firing for longer than that produces a
// second reset where it used to produce one; no clip in the corpus does.
inline constexpr double kMinSecondsBetweenCuts = 0.3;

inline constexpr Strength Classify(double residual, double histogramOverlap) noexcept
{
    if (residual > kResidualStrong) return Strength::Residual;
    if (residual > kResidualWeak && histogramOverlap < kHistogramOverlap) return Strength::Histogram;
    return Strength::None;
}

inline uint32_t MinFramesBetweenCuts(double fps) noexcept
{
    if (!(fps > 0.0) || !std::isfinite(fps)) return 2;
    return std::max<uint32_t>(2, uint32_t(std::lround(kMinSecondsBetweenCuts * fps)));
}

// Bins for the luma histogram both consumers compare. 32 is what the corpus
// above was measured with; changing it moves every overlap number with it.
inline constexpr int kHistogramBins = 32;

// Overlap of two luma distributions in [0,1], 1 meaning identical. Normalised
// by sample count, so the two sides may be sampled at different densities and
// a subsampled frame answers the same as a whole one.
inline float HistogramIntersection(std::span<const float> a, std::span<const float> b) noexcept
{
    if (a.empty() || a.size() != b.size()) return 0.0f;
    std::array<float, kHistogramBins> ha{}, hb{};
    for (const float v : a) ++ha[size_t(std::clamp(int(v * kHistogramBins), 0, kHistogramBins - 1))];
    for (const float v : b) ++hb[size_t(std::clamp(int(v * kHistogramBins), 0, kHistogramBins - 1))];
    float overlap = 0.0f;
    for (int i = 0; i < kHistogramBins; ++i) overlap += std::min(ha[size_t(i)], hb[size_t(i)]);
    return overlap / float(a.size());
}

// BGRA -> Rec.709-ish luma in [0,1], the same scale the thresholds were
// measured on.
inline float LumaFromBgra(const uint8_t* p) noexcept
{
    return (0.0722f * p[0] + 0.7152f * p[1] + 0.2126f * p[2]) * (1.0f / 255.0f);
}

// What a caller with no alignment stage can measure about a decoded pair.
struct PairEvidence {
    // Mean absolute luma difference with the frames left where they are. This
    // is NOT the aligned residual the strong arm was measured against: it is
    // bounded below by that one, and a pan inflates it without limit.
    //
    // Defaulted to the reading two IDENTICAL frames give, so a pair that could
    // not be measured at all answers "not a cut". A measurement that did not
    // happen is not evidence of a cut, and the failure directions are not
    // symmetric: defaulting the other way would hold the picture on every frame
    // of a conversion whose geometry this function could not read.
    double residual = 0.0;
    double histogramOverlap = 1.0;
};

// At most this many samples per frame. The two quantities above are a mean and
// a normalised histogram, so a regular stride estimates both without bias, and
// 320x180 of a 2560x1440 frame is 1/64th of the reads for an answer whose
// sampling error is far under the margin between a pan and a cut. Cheap enough
// that the frame-generation pass, which is bounded by its encoder, absorbs it.
inline constexpr size_t kMaxPairSamples = 320 * 180;

// Measures a decoded BGRA pair of the same geometry. Tightly packed rows.
inline PairEvidence MeasureDecodedPair(std::span<const uint8_t> previous,
                                       std::span<const uint8_t> current,
                                       uint32_t width, uint32_t height)
{
    PairEvidence evidence{};
    const size_t pixels = size_t(width) * size_t(height);
    if (!width || !height || previous.size() < pixels * 4 || current.size() < pixels * 4)
        return evidence;
    // One step for both axes, so the sample grid keeps the frame's aspect and
    // no axis is favoured by the thinning.
    uint32_t step = 1;
    while ((size_t(width / step + 1) * size_t(height / step + 1)) > kMaxPairSamples) ++step;
    std::array<float, kHistogramBins> ha{}, hb{};
    double sum = 0.0;
    size_t samples = 0;
    for (uint32_t y = 0; y < height; y += step) {
        const uint8_t* rowA = previous.data() + size_t(y) * width * 4u;
        const uint8_t* rowB = current.data() + size_t(y) * width * 4u;
        for (uint32_t x = 0; x < width; x += step) {
            const float a = LumaFromBgra(rowA + size_t(x) * 4u);
            const float b = LumaFromBgra(rowB + size_t(x) * 4u);
            ++ha[size_t(std::clamp(int(a * kHistogramBins), 0, kHistogramBins - 1))];
            ++hb[size_t(std::clamp(int(b * kHistogramBins), 0, kHistogramBins - 1))];
            sum += std::abs(double(a) - double(b));
            ++samples;
        }
    }
    if (!samples) return evidence;
    float overlap = 0.0f;
    for (int i = 0; i < kHistogramBins; ++i) overlap += std::min(ha[size_t(i)], hb[size_t(i)]);
    evidence.residual = sum / double(samples);
    evidence.histogramOverlap = double(overlap) / double(samples);
    return evidence;
}

// The cut decision for a pair nobody aligned.
//
// ONLY the histogram arm. The strong arm is a statement about correspondence
// having failed, and correspondence is exactly what an offline conversion never
// computes: an unaligned residual is bounded below by the aligned one, so a
// fast pan - measured at 0.10-0.13 ALIGNED, and far above kResidualStrong
// unaligned - would trip the strong arm on every frame of the pan and hold the
// picture through the one shot that most needs interpolating. The histogram is
// what separates the two cases and is the reason it is in the criterion at all:
// a pan carries its luma distribution with it (overlap >= 0.91 on the corpus)
// and a cut does not (<= 0.47, softest 0.78).
//
// kResidualWeak is kept as a floor rather than dropped. Histogram overlap alone
// calls a slow fade a cut - the distribution slides while consecutive frames
// stay nearly identical - and holding frames through a fade is the artifact
// this detector exists to avoid, in the one place where interpolation is
// perfectly well defined.
inline bool IsCutBetweenDecodedFrames(const PairEvidence& evidence) noexcept
{
    return evidence.residual > kResidualWeak && evidence.histogramOverlap < kHistogramOverlap;
}

} // namespace scene_cut
