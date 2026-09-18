#include "SceneCut.h"
#include "SynchronizedPlayback.h"
#include "TemporalGuides.h"
#include "TestSupport.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 180;
constexpr double kFps = 30.0;

// Smooth textured field so block matching has unambiguous correspondences. `lift` raises the
// whole field, which is how a flash or an exposure step looks to the analysis grid: the
// correspondences survive but the luma histogram moves off its bins.
std::vector<uint8_t> TexturedFrame(int shiftX, int shiftY, double lift = 0.0)
{
    std::vector<uint8_t> bgra(size_t(kWidth) * kHeight * 4u);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const double sx = double(int(x) - shiftX), sy = double(int(y) - shiftY);
            const double v = 0.5 + 0.25 * std::sin(sx * 0.21) * std::cos(sy * 0.17) +
                             0.2 * std::sin(sx * 0.05 + sy * 0.09) + lift;
            const uint8_t luma = uint8_t(std::clamp(v, 0.0, 1.0) * 255.0);
            uint8_t* p = bgra.data() + (size_t(y) * kWidth + x) * 4u;
            p[0] = luma; p[1] = luma; p[2] = luma; p[3] = 255;
        }
    }
    return bgra;
}

std::vector<uint8_t> NoiseFrame(uint32_t seed)
{
    std::vector<uint8_t> bgra(size_t(kWidth) * kHeight * 4u);
    uint32_t state = seed * 2654435761u + 12345u;
    for (size_t i = 0; i < bgra.size(); i += 4) {
        state = state * 1664525u + 1013904223u;
        const uint8_t luma = uint8_t(state >> 24);
        bgra[i] = luma; bgra[i + 1] = luma; bgra[i + 2] = luma; bgra[i + 3] = 255;
    }
    return bgra;
}

FrameIdentity Frame(uint64_t number, uint32_t sourceGeneration = 1, HistoryReset reset = HistoryReset::None)
{
    return FrameIdentity{number, int64_t(double(number) / kFps * 1e7), sourceGeneration, 0, 0, reset};
}

bool Generate(TemporalGuideGenerator& guides, const std::vector<uint8_t>& bgra, const FrameIdentity& frame, GuideFrame& out)
{
    return guides.Generate(bgra.data(), kWidth, kHeight, kWidth, kHeight, kFps, frame, out);
}

struct ChannelStats {
    bool anyNonZero{};
    float minimum{};
    float maximum{};
};

ChannelStats Channel(const GuideFrame& guide, size_t channel)
{
    ChannelStats stats{false, 1e9f, -1e9f};
    for (size_t i = channel; i < guide.guideGridRGBA32F.size(); i += 4) {
        const float v = guide.guideGridRGBA32F[i];
        if (v != 0.0f) stats.anyNonZero = true;
        stats.minimum = std::min(stats.minimum, v);
        stats.maximum = std::max(stats.maximum, v);
    }
    return stats;
}

void guide_controls_neutralize_disabled_guides_test()
{
    const auto first = TexturedFrame(0, 0);
    const auto second = TexturedFrame(7, 3);
    const auto run = [&](GuideControls controls, GuideFrame& out) {
        TemporalGuideGenerator guides;
        guides.SetControls(controls);
        GuideFrame warm;
        CHECK(Generate(guides, first, Frame(0), warm));
        CHECK(Generate(guides, second, Frame(1), out));
        CHECK(out.hasHistory);
        CHECK_EQ(controls, guides.Controls());
    };

    GuideFrame full;
    run(GuideControls{}, full);
    CHECK(Channel(full, 0).anyNonZero || Channel(full, 1).anyNonZero);
    const auto fullDepth = Channel(full, 2);
    CHECK(fullDepth.maximum - fullDepth.minimum > 0.01f);
    // A is unused padding kept only so the grid stays a filterable RGBA32F.
    CHECK(!Channel(full, 3).anyNonZero);

    GuideFrame noMotion;
    run(GuideControls{false, true}, noMotion);
    CHECK(!Channel(noMotion, 0).anyNonZero);
    CHECK(!Channel(noMotion, 1).anyNonZero);
    CHECK_EQ(full.gridW, noMotion.gridW);
    CHECK_EQ(full.gridH, noMotion.gridH);

    GuideFrame flatDepth;
    run(GuideControls{true, false}, flatDepth);
    const auto depth = Channel(flatDepth, 2);
    CHECK_EQ(0.75f, depth.minimum);
    CHECK_EQ(0.75f, depth.maximum);
    CHECK_EQ(full.guideGridRGBA32F.size(), flatDepth.guideGridRGBA32F.size());
}

void guide_generator_reports_reset_reasons_test()
{
    TemporalGuideGenerator guides;
    std::vector<GuideFrame> out(8);
    CHECK_EQ(uint32_t{0}, guides.HistoryGeneration());

    CHECK(Generate(guides, TexturedFrame(0, 0), Frame(0), out[0]));
    CHECK(!out[0].hasHistory);
    CHECK_EQ(HistoryReset::FirstFrame, out[0].id.reset);
    CHECK_EQ(uint32_t{1}, guides.HistoryGeneration());
    CHECK_EQ(uint32_t{1}, out[0].id.historyGeneration);
    CHECK(out[0].id.SameSource(Frame(0)));

    CHECK(Generate(guides, TexturedFrame(2, 1), Frame(1), out[1]));
    CHECK(out[1].hasHistory);
    CHECK_EQ(HistoryReset::None, out[1].id.reset);
    CHECK_EQ(uint32_t{1}, out[1].id.historyGeneration);
    CHECK_EQ(uint64_t{1}, out[1].id.frameNumber);

    // Frame 3 without frame 2: dropped frame, history discarded.
    CHECK(Generate(guides, TexturedFrame(4, 2), Frame(3), out[2]));
    CHECK(!out[2].hasHistory);
    CHECK_EQ(HistoryReset::Drop, out[2].id.reset);
    CHECK_EQ(uint32_t{2}, guides.HistoryGeneration());
    CHECK_EQ(uint32_t{2}, out[2].id.historyGeneration);

    CHECK(Generate(guides, TexturedFrame(6, 3), Frame(4), out[3]));
    CHECK(out[3].hasHistory);

    // Same frame numbers from a new decoder session are a new source.
    CHECK(Generate(guides, TexturedFrame(8, 4), Frame(5, 2), out[4]));
    CHECK(!out[4].hasHistory);
    CHECK_EQ(HistoryReset::SourceChange, out[4].id.reset);
    CHECK_EQ(uint32_t{3}, out[4].id.historyGeneration);

    // Caller-declared reset wins over a clean successor.
    CHECK(Generate(guides, TexturedFrame(10, 5), Frame(6, 2, HistoryReset::Seek), out[5]));
    CHECK(!out[5].hasHistory);
    CHECK_EQ(HistoryReset::Seek, out[5].id.reset);
    CHECK_EQ(uint32_t{4}, out[5].id.historyGeneration);

    // A hard cut is detected from correspondence quality even with clean identity.
    CHECK(Generate(guides, NoiseFrame(1), Frame(7, 2), out[6]));
    CHECK(!out[6].hasHistory);
    CHECK_EQ(HistoryReset::Cut, out[6].id.reset);
    CHECK_EQ(uint32_t{5}, out[6].id.historyGeneration);
    CHECK_EQ(uint64_t{7}, out[6].id.frameNumber);

    // Reset() makes the next frame a first frame regardless of its identity.
    guides.Reset();
    CHECK(Generate(guides, NoiseFrame(1), Frame(8, 2), out[7]));
    CHECK(!out[7].hasHistory);
    CHECK_EQ(HistoryReset::FirstFrame, out[7].id.reset);
    CHECK_EQ(uint32_t{6}, out[7].id.historyGeneration);
}

void guide_generator_reevaluates_a_repeated_frame_without_reset_test()
{
    TemporalGuideGenerator guides;
    GuideFrame first, second, repeat, next;
    CHECK(Generate(guides, TexturedFrame(0, 0), Frame(0), first));
    CHECK(Generate(guides, TexturedFrame(3, 1), Frame(1), second));
    CHECK(second.hasHistory);
    // The offline job re-captures the first frame until the runtime receipt
    // arrives; that must not discard the warm history or count as a reset.
    CHECK(Generate(guides, TexturedFrame(3, 1), Frame(1), repeat));
    CHECK(repeat.hasHistory);
    CHECK_EQ(HistoryReset::None, repeat.id.reset);
    CHECK_EQ(uint32_t{1}, guides.HistoryGeneration());
    CHECK_EQ(second.globalMotionX, repeat.globalMotionX);
    CHECK_EQ(second.globalMotionY, repeat.globalMotionY);
    // The successor after a repeat still continues the history.
    CHECK(Generate(guides, TexturedFrame(6, 2), Frame(2), next));
    CHECK(next.hasHistory);
    CHECK_EQ(HistoryReset::None, next.id.reset);
    // Re-submitting a single-frame source (photo priming) has no history but
    // never declares a reset either.
    TemporalGuideGenerator photo;
    GuideFrame prime, again;
    CHECK(Generate(photo, TexturedFrame(0, 0), Frame(0), prime));
    CHECK(Generate(photo, TexturedFrame(0, 0), Frame(0), again));
    CHECK(!again.hasHistory);
    CHECK_EQ(HistoryReset::None, again.id.reset);
    CHECK_EQ(uint32_t{1}, photo.HistoryGeneration());
}

// The same picture encoded two ways: BGRA, and limited-range NV12 whose Y plane is
// the BT.709 luma of that BGRA. Both must drive the analysis pipeline identically
// once DownsampleLuma maps them onto the same [0,1] luma scale.
// The picture is deliberately coloured, not gray: on gray input the BGRA weights
// sum to 1 and cancel, so any weight triple - or a missing 16/219 range mapping -
// would pass. A hashed 8x8 block texture also gives every cell a distinct SAD
// minimum, where a smooth gradient would let one code of rounding flip the winner.
struct TexturedPixel { uint8_t b, g, r; };

TexturedPixel TexturedColor(uint32_t x, uint32_t y, int shiftX)
{
    const uint32_t sx = uint32_t((int(x) - shiftX + int(kWidth)) % int(kWidth));
    uint32_t h = (sx / 8u) * 0x9E3779B1u ^ (y / 8u) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return {uint8_t(24u + (h % 200u)), uint8_t(24u + ((h >> 8) % 200u)), uint8_t(24u + ((h >> 16) % 200u))};
}

// BT.709 luma of the same pixel, in limited range: exactly what a correct NV12 Y
// plane holds, so the two layouts describe one picture rather than two.
uint8_t LimitedRangeLuma(TexturedPixel pixel)
{
    const double luma = 0.2126 * pixel.r + 0.7152 * pixel.g + 0.0722 * pixel.b;
    return uint8_t(16 + int(std::lround(luma * 219.0 / 255.0)));
}

std::vector<uint8_t> TexturedBgra(int shiftX)
{
    std::vector<uint8_t> bgra(size_t(kWidth) * kHeight * 4u);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const TexturedPixel pixel = TexturedColor(x, y, shiftX);
            uint8_t* p = bgra.data() + (size_t(y) * kWidth + x) * 4u;
            p[0] = pixel.b; p[1] = pixel.g; p[2] = pixel.r; p[3] = 255;
        }
    }
    return bgra;
}

std::vector<uint8_t> TexturedNv12(int shiftX)
{
    std::vector<uint8_t> nv12(size_t(kWidth) * kHeight + size_t(kWidth) * kHeight / 2u, 128);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            nv12[size_t(y) * kWidth + x] = LimitedRangeLuma(TexturedColor(x, y, shiftX));
        }
    }
    return nv12;
}

void generate_treats_nv12_limited_range_luma_like_bgra_test()
{
    const auto bgra0 = TexturedBgra(0), bgra1 = TexturedBgra(5);
    const auto nv120 = TexturedNv12(0), nv121 = TexturedNv12(5);

    TemporalGuideGenerator bgraGuides, nv12Guides;
    GuideFrame bgraOut0, bgraOut1, nv12Out0, nv12Out1;
    CHECK(bgraGuides.Generate(bgra0.data(), kWidth, kHeight, kWidth, kHeight, kFps, Frame(0), bgraOut0));
    CHECK(bgraGuides.Generate(bgra1.data(), kWidth, kHeight, kWidth, kHeight, kFps, Frame(1), bgraOut1));
    CHECK(nv12Guides.Generate(nv120.data(), kWidth, kHeight, kWidth, kHeight, kFps, Frame(0), nv12Out0,
                               SourcePixelLayout::Nv12));
    CHECK(nv12Guides.Generate(nv121.data(), kWidth, kHeight, kWidth, kHeight, kFps, Frame(1), nv12Out1,
                               SourcePixelLayout::Nv12));

    CHECK_EQ(bgraOut0.hasHistory, nv12Out0.hasHistory);
    CHECK_EQ(bgraOut1.hasHistory, nv12Out1.hasHistory);
    CHECK_EQ(int(bgraOut0.id.reset), int(nv12Out0.id.reset));
    CHECK_EQ(int(bgraOut1.id.reset), int(nv12Out1.id.reset));
    CHECK_EQ(bgraOut1.gridW, nv12Out1.gridW);
    CHECK_EQ(bgraOut1.gridH, nv12Out1.gridH);
    CHECK_EQ(bgraOut1.guideGridRGBA32F.size(), nv12Out1.guideGridRGBA32F.size());

    // The two inputs differ by at most one luma code (0.5/219) of rounding. That is
    // enough to flip a cell sitting on the confidence threshold between "rejected" and a
    // displacement, so per-cell motion is compared as a mismatch rate; the depth proxy and
    // the global motion, which have no such threshold, must agree everywhere (the depth
    // proxy is a normalised gradient, so one code of rounding is worth a few hundredths).
    const size_t cells = bgraOut1.guideGridRGBA32F.size() / 4u;
    size_t motionMismatches = 0, movedCells = 0; float worstDepth = 0.0f;
    for (size_t cell = 0; cell < cells; ++cell) {
        const float* a = bgraOut1.guideGridRGBA32F.data() + cell * 4u;
        const float* b = nv12Out1.guideGridRGBA32F.data() + cell * 4u;
        if (std::abs(a[0] - b[0]) > 0.02f || std::abs(a[1] - b[1]) > 0.02f) ++motionMismatches;
        if (std::abs(a[0]) > 0.001f) ++movedCells;
        worstDepth = std::max(worstDepth, std::abs(a[2] - b[2]));
    }
    // Equivalence is only worth asserting if the frames moved at all: two generators
    // that both reported no motion anywhere would otherwise agree perfectly.
    CHECK(movedCells > cells / 10u);
    CHECK(motionMismatches <= cells / 50u);
    CHECK(worstDepth <= 0.05f);
    CHECK(std::abs(bgraOut1.globalMotionX - nv12Out1.globalMotionX) <= 0.5f);
    CHECK(std::abs(bgraOut1.globalMotionY - nv12Out1.globalMotionY) <= 0.5f);
}

void scene_cut_needs_low_histogram_overlap_or_a_large_residual_test()
{
    // Measured corpus values: fast pans (residual 0.10-0.13, overlap >= 0.91)
    // must not cut; real cuts (0.24-0.40, overlap <= 0.47) and the softest
    // observed real cut (0.108 / 0.78) must.
    CHECK(!TemporalGuideGenerator::IsSceneCut(0.125f, 0.915f));
    CHECK(!TemporalGuideGenerator::IsSceneCut(0.098f, 0.60f));
    CHECK(TemporalGuideGenerator::IsSceneCut(0.108f, 0.778f));
    CHECK(TemporalGuideGenerator::IsSceneCut(0.242f, 0.019f));
    CHECK(TemporalGuideGenerator::IsSceneCut(0.374f, 0.468f));
    CHECK(TemporalGuideGenerator::IsSceneCut(0.31f, 0.99f));
    const std::vector<float> dark(100, 0.1f), bright(100, 0.9f), mixed = [] {
        std::vector<float> v(100, 0.1f);
        for (size_t i = 0; i < 50; ++i) v[i] = 0.9f;
        return v;
    }();
    CHECK_EQ(1.0f, TemporalGuideGenerator::LumaHistogramIntersection(dark, dark));
    CHECK_EQ(0.0f, TemporalGuideGenerator::LumaHistogramIntersection(dark, bright));
    CHECK_EQ(0.5f, TemporalGuideGenerator::LumaHistogramIntersection(dark, mixed));
}

// The frame-generation pass reaches the same criterion from two DECODED frames
// and no alignment stage, so its evidence is gathered differently and only one
// arm of the criterion survives the difference. These pin the property that
// difference threatens: a pan must not read as a cut.
void decoded_pair_evidence_separates_a_pan_from_a_cut_test()
{
    constexpr uint32_t w = 160, h = 90;
    const auto fill = [](std::vector<uint8_t>& frame, auto shade) {
        frame.assign(size_t(w) * h * 4u, 0);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t v = shade(x, y);
                uint8_t* p = frame.data() + (size_t(y) * w + x) * 4u;
                p[0] = p[1] = p[2] = v; p[3] = 255;
            }
    };
    // Vertical stripes, which a horizontal shift carries EXACTLY: the luma
    // histogram is identical before and after, which is the whole reason the
    // histogram is in the criterion. A real pan reveals new content at one
    // edge and only approximates this; it approximates it well, which is what
    // the corpus measured as overlap >= 0.91.
    std::vector<uint8_t> shot, panned, other, dimmed;
    fill(shot, [](uint32_t x, uint32_t) { return uint8_t(((x / 4u) % 2u) ? 200 : 40); });
    fill(panned, [](uint32_t x, uint32_t) { return uint8_t(((((x + 20u) % w) / 4u) % 2u) ? 200 : 40); });
    // A different shot: the same geometry, a luma distribution with no overlap.
    fill(other, [](uint32_t x, uint32_t) { return uint8_t(((x / 4u) % 2u) ? 130 : 120); });
    // One step of a slow fade - the distribution slides while the frames stay
    // nearly identical, which histogram overlap alone would call a cut.
    fill(dimmed, [](uint32_t x, uint32_t) { return uint8_t(((x / 4u) % 2u) ? 196 : 36); });

    const auto measure = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        return scene_cut::MeasureDecodedPair(a, b, w, h);
    };

    // The same frame twice is never a cut.
    const auto still = measure(shot, shot);
    CHECK(still.residual == 0.0);
    CHECK(still.histogramOverlap > 0.999);
    CHECK(!scene_cut::IsCutBetweenDecodedFrames(still));

    // A pan moves every pixel - an unaligned residual far above the strong
    // arm's 0.30 - and changes the distribution not at all. This is exactly the
    // case that made the strong arm unusable without a correspondence stage,
    // so the assertion is worth stating both ways.
    const auto pan = measure(shot, panned);
    CHECK(pan.residual > scene_cut::kResidualStrong);
    CHECK(pan.histogramOverlap > 0.999);
    CHECK(!scene_cut::IsCutBetweenDecodedFrames(pan));
    CHECK(scene_cut::Classify(pan.residual, pan.histogramOverlap) == SceneCutStrength::Residual);

    // A cut loses both.
    const auto cut = measure(shot, other);
    CHECK(cut.residual > scene_cut::kResidualWeak);
    CHECK(cut.histogramOverlap < scene_cut::kHistogramOverlap);
    CHECK(scene_cut::IsCutBetweenDecodedFrames(cut));

    // A fade step moves the histogram off its bins without moving the picture.
    // The residual floor is what keeps it interpolating.
    const auto fade = measure(shot, dimmed);
    CHECK(fade.residual <= scene_cut::kResidualWeak);
    CHECK(!scene_cut::IsCutBetweenDecodedFrames(fade));

    // Degenerate inputs answer "not a cut" rather than reading past the end.
    CHECK(!scene_cut::IsCutBetweenDecodedFrames(scene_cut::MeasureDecodedPair({}, {}, w, h)));
    CHECK(!scene_cut::IsCutBetweenDecodedFrames(scene_cut::MeasureDecodedPair(shot, other, 0, 0)));

    // Subsampling must not change the verdict: a 4K-shaped frame is thinned by
    // MeasureDecodedPair and a 160x90 one is not, so run the same content at a
    // size that forces a stride and check the answers still separate.
    constexpr uint32_t bigW = 1920, bigH = 1080;
    std::vector<uint8_t> bigShot(size_t(bigW) * bigH * 4u), bigOther(size_t(bigW) * bigH * 4u);
    for (uint32_t y = 0; y < bigH; ++y)
        for (uint32_t x = 0; x < bigW; ++x) {
            const size_t at = (size_t(y) * bigW + x) * 4u;
            const uint8_t a = uint8_t(((x / 48u) % 2u) ? 200 : 40);
            const uint8_t b = uint8_t(((x / 48u) % 2u) ? 130 : 120);
            bigShot[at] = bigShot[at + 1] = bigShot[at + 2] = a; bigShot[at + 3] = 255;
            bigOther[at] = bigOther[at + 1] = bigOther[at + 2] = b; bigOther[at + 3] = 255;
        }
    CHECK(!scene_cut::IsCutBetweenDecodedFrames(
        scene_cut::MeasureDecodedPair(bigShot, bigShot, bigW, bigH)));
    CHECK(scene_cut::IsCutBetweenDecodedFrames(
        scene_cut::MeasureDecodedPair(bigShot, bigOther, bigW, bigH)));
}

// The debounce is specified in seconds, so these tests run at the frame rate the labelled
// corpus was shot and scored at, where the derived interval is a whole number of frames:
// 0.3 s at 30 fps is exactly 9.
constexpr double kCutFps = 30.0;
constexpr uint32_t kCutWindowFrames = 9;
// Lifting the whole field by this much lands squarely in the weak arm's band: measured
// residual 0.216 with histogram overlap 0.594, i.e. a cut only the histogram test believes in.
constexpr double kFlashLift = 0.22;

bool GenerateAtCutFps(TemporalGuideGenerator& guides, const std::vector<uint8_t>& bgra, uint64_t number,
                      GuideFrame& out, HistoryReset reset = HistoryReset::None)
{
    const FrameIdentity id{number, int64_t(double(number) / kCutFps * 1e7), 1, 0, 0, reset};
    return guides.Generate(bgra.data(), kWidth, kHeight, kWidth, kHeight, kCutFps, id, out);
}

void weak_scene_cuts_are_suppressed_inside_the_minimum_interval_test()
{
    CHECK_EQ(kCutWindowFrames, TemporalGuideGenerator::MinFramesBetweenCuts(kCutFps));
    // The corpus values from the test above, now split by strength: only the softest cut is
    // debounceable, the decisive one is not.
    CHECK_EQ(SceneCutStrength::Histogram, TemporalGuideGenerator::ClassifySceneCut(0.108, 0.778));
    CHECK_EQ(SceneCutStrength::Residual, TemporalGuideGenerator::ClassifySceneCut(0.374, 0.468));
    CHECK_EQ(SceneCutStrength::None, TemporalGuideGenerator::ClassifySceneCut(0.125, 0.915));

    TemporalGuideGenerator guides;
    GuideFrame out;
    CHECK(GenerateAtCutFps(guides, TexturedFrame(0, 0), 0, out));
    CHECK(GenerateAtCutFps(guides, TexturedFrame(3, 1), 1, out));
    CHECK(out.hasHistory);
    CHECK_EQ(SceneCutStrength::None, out.sceneCut);

    // Frame 2 flashes. Nothing has been accepted since the first frame, so the weak arm fires
    // and takes the history with it.
    CHECK(GenerateAtCutFps(guides, TexturedFrame(6, 2, kFlashLift), 2, out));
    CHECK_EQ(SceneCutStrength::Histogram, out.sceneCut);
    CHECK(!out.sceneCutSuppressed);
    CHECK(!out.hasHistory);
    CHECK_EQ(HistoryReset::Cut, out.id.reset);
    CHECK(out.sceneCutResidual > 0.10f && out.sceneCutHistogramOverlap < 0.85f);
    const uint32_t generationAfterCut = guides.HistoryGeneration();

    // Frames 3 and 4 stay in the flashed exposure and pan normally.
    CHECK(GenerateAtCutFps(guides, TexturedFrame(9, 3, kFlashLift), 3, out));
    CHECK(GenerateAtCutFps(guides, TexturedFrame(12, 4, kFlashLift), 4, out));
    CHECK(out.hasHistory);
    CHECK_EQ(SceneCutStrength::None, out.sceneCut);

    // Frame 5 ends the flash: the same weak evidence, three frames into the window. The
    // decision is still reported, but the DLSS history must survive it - a second reset this
    // soon is the flicker the guide warns about, not a second scene.
    CHECK(GenerateAtCutFps(guides, TexturedFrame(15, 5), 5, out));
    CHECK_EQ(SceneCutStrength::Histogram, out.sceneCut);
    CHECK(out.sceneCutSuppressed);
    CHECK(out.hasHistory);
    CHECK_EQ(HistoryReset::None, out.id.reset);
    CHECK_EQ(generationAfterCut, guides.HistoryGeneration());

    // Frames 6..10 are an ordinary pan, so the window elapses without another accepted cut.
    for (uint64_t number = 6; number <= 10; ++number) {
        CHECK(GenerateAtCutFps(guides, TexturedFrame(int(number) * 3, int(number)), number, out));
        CHECK(out.hasHistory);
        CHECK_EQ(SceneCutStrength::None, out.sceneCut);
    }

    // Frame 11 is exactly kCutWindowFrames frames after the accepted cut on frame 2, which is
    // the first interval the window no longer covers, so the same weak evidence is trusted
    // again. One frame earlier it would still be withheld.
    CHECK(GenerateAtCutFps(guides, TexturedFrame(33, 11, kFlashLift), 11, out));
    CHECK_EQ(SceneCutStrength::Histogram, out.sceneCut);
    CHECK(!out.sceneCutSuppressed);
    CHECK(!out.hasHistory);
    CHECK_EQ(HistoryReset::Cut, out.id.reset);
    CHECK_EQ(generationAfterCut + 1, guides.HistoryGeneration());
}

// The defect the window length was changed for. On labelled real footage - corpus clip
// `real-film-cuts`, hard cuts verified frame by frame at 20/47/70/87 - the cut at frame 87
// is 17 frames after the one at 70, 0.57 s at 30 fps. It fires the weak arm (residual
// 0.2711, histogram overlap 0.5294) and the 0.6 s window, 18 frames at this rate,
// discarded it by construction: the neural pass kept its accumulated history across a
// genuine discontinuity and the renderer was handed guides built from the wrong shot. No
// synthetic clip can reach this case, because none of them cuts twice inside 0.6 s.
void a_real_cut_one_frame_inside_the_old_window_is_taken_test()
{
    // The interval under test only means anything while the window is shorter than it.
    CHECK(TemporalGuideGenerator::MinFramesBetweenCuts(kCutFps) <= 17u);

    TemporalGuideGenerator guides;
    GuideFrame out;
    CHECK(GenerateAtCutFps(guides, TexturedFrame(0, 0), 0, out));
    CHECK(GenerateAtCutFps(guides, TexturedFrame(3, 1), 1, out));

    // Frame 2 is the first shot change, and is accepted with nothing before it.
    CHECK(GenerateAtCutFps(guides, TexturedFrame(6, 2, kFlashLift), 2, out));
    CHECK_EQ(SceneCutStrength::Histogram, out.sceneCut);
    CHECK(!out.sceneCutSuppressed);
    CHECK_EQ(HistoryReset::Cut, out.id.reset);
    const uint32_t generationAfterFirstCut = guides.HistoryGeneration();

    // Frames 3..18 are the second shot: sixteen frames of ordinary pan, no decision.
    for (uint64_t number = 3; number <= 18; ++number) {
        CHECK(GenerateAtCutFps(guides, TexturedFrame(int(number) * 3, int(number), kFlashLift),
                               number, out));
        CHECK(out.hasHistory);
        CHECK_EQ(SceneCutStrength::None, out.sceneCut);
    }

    // Frame 19 is the third shot change, 17 frames after the second - one frame short of
    // the old window. It is a scene change, not a transient returning to the shot before
    // it, so the history has to go.
    CHECK(GenerateAtCutFps(guides, TexturedFrame(57, 19), 19, out));
    CHECK_EQ(SceneCutStrength::Histogram, out.sceneCut);
    CHECK(!out.sceneCutSuppressed);
    CHECK(!out.hasHistory);
    CHECK_EQ(HistoryReset::Cut, out.id.reset);
    CHECK_EQ(generationAfterFirstCut + 1, guides.HistoryGeneration());
    CHECK_EQ((SceneCutAccounting{.acceptedWeak = 2}), guides.SceneCuts());
}

void a_strong_scene_cut_fires_inside_the_minimum_interval_test()
{
    TemporalGuideGenerator guides;
    GuideFrame out;
    CHECK(GenerateAtCutFps(guides, TexturedFrame(0, 0), 0, out));
    CHECK(GenerateAtCutFps(guides, TexturedFrame(3, 1), 1, out));
    CHECK(GenerateAtCutFps(guides, TexturedFrame(6, 2, kFlashLift), 2, out));
    CHECK_EQ(HistoryReset::Cut, out.id.reset);
    CHECK(GenerateAtCutFps(guides, TexturedFrame(9, 3, kFlashLift), 3, out));
    CHECK(GenerateAtCutFps(guides, TexturedFrame(12, 4, kFlashLift), 4, out));
    CHECK(out.hasHistory);

    // Frame 5 cuts to black three frames into the window. Correspondence fails outright, and a
    // signal that strong is never withheld - x264/x265 let a decisive scenecut fire inside
    // min-keyint too, and keeping history across a real cut is what NGX asks us to avoid.
    CHECK(GenerateAtCutFps(guides, TexturedFrame(15, 5, -1.0), 5, out));
    CHECK_EQ(SceneCutStrength::Residual, out.sceneCut);
    CHECK(!out.sceneCutSuppressed);
    CHECK(!out.hasHistory);
    CHECK_EQ(HistoryReset::Cut, out.id.reset);
    CHECK(out.sceneCutResidual > 0.30f);
}

void a_declared_reset_rearms_the_scene_cut_interval_test()
{
    TemporalGuideGenerator guides;
    GuideFrame out;
    CHECK(GenerateAtCutFps(guides, TexturedFrame(0, 0), 0, out));
    CHECK(GenerateAtCutFps(guides, TexturedFrame(3, 1), 1, out));
    CHECK(GenerateAtCutFps(guides, TexturedFrame(6, 2, kFlashLift), 2, out));
    CHECK_EQ(SceneCutStrength::Histogram, out.sceneCut);
    CHECK_EQ(HistoryReset::Cut, out.id.reset);

    // A seek on frame 3 behaves as before: history gone, reset reported, nothing classified
    // because there is nothing to compare against.
    CHECK(GenerateAtCutFps(guides, TexturedFrame(0, 0), 3, out, HistoryReset::Seek));
    CHECK(!out.hasHistory);
    CHECK_EQ(HistoryReset::Seek, out.id.reset);
    CHECK_EQ(SceneCutStrength::None, out.sceneCut);
    CHECK(!out.sceneCutSuppressed);
    CHECK(GenerateAtCutFps(guides, TexturedFrame(3, 1), 4, out));
    CHECK(out.hasHistory);

    // Frame 5 is three frames after the accepted cut on frame 2, so the window would still be
    // running - but the seek already discarded the history a suppression would have protected,
    // so the weak arm is armed again and the cut is taken.
    CHECK(GenerateAtCutFps(guides, TexturedFrame(6, 2, kFlashLift), 5, out));
    CHECK_EQ(SceneCutStrength::Histogram, out.sceneCut);
    CHECK(!out.sceneCutSuppressed);
    CHECK(!out.hasHistory);
    CHECK_EQ(HistoryReset::Cut, out.id.reset);
}

void scene_cut_accounting_survives_reset_and_counts_each_arm_test()
{
    TemporalGuideGenerator guides;
    GuideFrame out;
    CHECK(GenerateAtCutFps(guides, TexturedFrame(0, 0), 0, out));
    CHECK(GenerateAtCutFps(guides, TexturedFrame(3, 1), 1, out));
    // Nothing is classified without a previous frame, and a clean pan is not a decision.
    CHECK_EQ(SceneCutAccounting{}, guides.SceneCuts());

    CHECK(GenerateAtCutFps(guides, TexturedFrame(6, 2, kFlashLift), 2, out));
    CHECK_EQ(SceneCutStrength::Histogram, out.sceneCut);
    CHECK(!out.sceneCutSuppressed);
    CHECK_EQ((SceneCutAccounting{.acceptedWeak = 1}), guides.SceneCuts());

    CHECK(GenerateAtCutFps(guides, TexturedFrame(9, 3, kFlashLift), 3, out));
    CHECK(GenerateAtCutFps(guides, TexturedFrame(12, 4, kFlashLift), 4, out));
    // Frame 5 leaves the flash three frames into the window, so the same weak evidence is
    // recognised and withheld. That is the number the corpus work needs: a suppression is
    // either a flicker avoided or a cut missed, and only labelled footage can tell which.
    CHECK(GenerateAtCutFps(guides, TexturedFrame(15, 5), 5, out));
    CHECK(out.sceneCutSuppressed);
    CHECK_EQ((SceneCutAccounting{.acceptedWeak = 1, .suppressed = 1}), guides.SceneCuts());

    for (uint64_t number = 6; number <= 16; ++number)
        CHECK(GenerateAtCutFps(guides, TexturedFrame(int(number) * 3, int(number)), number, out));
    CHECK_EQ((SceneCutAccounting{.acceptedWeak = 1, .suppressed = 1}), guides.SceneCuts());

    CHECK(GenerateAtCutFps(guides, TexturedFrame(51, 17, kFlashLift), 17, out));
    CHECK(!out.sceneCutSuppressed);
    CHECK_EQ((SceneCutAccounting{.acceptedWeak = 2, .suppressed = 1}), guides.SceneCuts());

    // Re-submitting frame 17 re-decides it, this time as a suppression because the cut it
    // is being compared against is its own. The offline job resubmits one frame up to 120
    // times while it waits for the runtime receipt; the tally must not move.
    CHECK(GenerateAtCutFps(guides, TexturedFrame(51, 17, kFlashLift), 17, out));
    CHECK_EQ((SceneCutAccounting{.acceptedWeak = 2, .suppressed = 1}), guides.SceneCuts());

    // Frame 18 cuts to black one frame after an accepted cut. The strong arm ignores the
    // window and is counted apart from the weak one.
    CHECK(GenerateAtCutFps(guides, TexturedFrame(54, 18, -1.0), 18, out));
    CHECK_EQ(SceneCutStrength::Residual, out.sceneCut);
    CHECK(!out.sceneCutSuppressed);
    const SceneCutAccounting tally{.acceptedStrong = 1, .acceptedWeak = 2, .suppressed = 1};
    CHECK_EQ(tally, guides.SceneCuts());
    CHECK_EQ(uint32_t{3}, guides.SceneCuts().Accepted());

    // A seek discards the image history but not the record of what was decided, and the
    // generator keeps counting into the same tally afterwards.
    guides.Reset();
    CHECK(GenerateAtCutFps(guides, TexturedFrame(0, 0), 19, out));
    CHECK_EQ(HistoryReset::FirstFrame, out.id.reset);
    CHECK_EQ(tally, guides.SceneCuts());
    CHECK(GenerateAtCutFps(guides, TexturedFrame(3, 1), 20, out));
    CHECK(GenerateAtCutFps(guides, TexturedFrame(6, 2, kFlashLift), 21, out));
    CHECK(!out.sceneCutSuppressed);
    CHECK_EQ((SceneCutAccounting{.acceptedStrong = 1, .acceptedWeak = 3, .suppressed = 1}),
             guides.SceneCuts());
}


// Aliasing trap. The analysis grid is 128x72 at this size, i.e. 10 source pixels per cell, so
// vertical stripes with a 20 px period repeat exactly every two cells: a 20 px displacement
// explains the static half of the frame exactly as well as standing still does. The other half
// carries non-repeating value noise that really moves, which pulls the whole-frame estimate off
// zero - and once the local search window is centred away from zero, the periodic half offers a
// perfect match that corresponds to no motion at all.
constexpr uint32_t kAliasW = 1280, kAliasH = 720;
constexpr uint32_t kAliasCell = 10;      // kAliasW / analysis grid width
constexpr uint32_t kAliasSplit = 768;    // left of this the content moves, right of it it does not
constexpr int kAliasShift = 30;          // three cells, resolved decisively by the noise half

double AliasHash(int x, int y)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return double(h >> 8) / double(1u << 24);
}

// Smoothed value noise: structure at the analysis-cell scale with no repetition, so the moving
// half has exactly one correct correspondence.
double AliasNoise(double x, double y)
{
    constexpr double spacing = 24.0;
    const double gx = x / spacing, gy = y / spacing;
    const int x0 = int(std::floor(gx)), y0 = int(std::floor(gy));
    const double tx = gx - double(x0), ty = gy - double(y0);
    const double sx = tx * tx * (3.0 - 2.0 * tx), sy = ty * ty * (3.0 - 2.0 * ty);
    const double top = AliasHash(x0, y0) * (1.0 - sx) + AliasHash(x0 + 1, y0) * sx;
    const double bottom = AliasHash(x0, y0 + 1) * (1.0 - sx) + AliasHash(x0 + 1, y0 + 1) * sx;
    return top * (1.0 - sy) + bottom * sy;
}

std::vector<uint8_t> AliasFrame(int shiftX)
{
    std::vector<uint8_t> bgra(size_t(kAliasW) * kAliasH * 4u);
    for (uint32_t y = 0; y < kAliasH; ++y) {
        for (uint32_t x = 0; x < kAliasW; ++x) {
            const double v = x < kAliasSplit
                ? 0.2 + 0.6 * AliasNoise(double(int(x) - shiftX), double(y))
                : 0.5 + 0.18 * std::sin(double(x) * (6.283185307 / 20.0));
            const uint8_t luma = uint8_t(std::clamp(v, 0.0, 1.0) * 255.0);
            uint8_t* p = bgra.data() + (size_t(y) * kAliasW + x) * 4u;
            p[0] = luma; p[1] = luma; p[2] = luma; p[3] = 255;
        }
    }
    return bgra;
}

void flow_rejects_aliased_vectors_on_static_repetitive_content_test()
{
    TemporalGuideGenerator guides;
    GuideFrame first, second;
    const auto a = AliasFrame(0);
    const auto b = AliasFrame(kAliasShift);
    CHECK(guides.Generate(a.data(), kAliasW, kAliasH, kAliasW, kAliasH, kFps, Frame(0), first));
    CHECK(guides.Generate(b.data(), kAliasW, kAliasH, kAliasW, kAliasH, kFps, Frame(1), second));
    CHECK(second.hasHistory);
    // The premise of the trap: the whole-frame estimate lands off zero (measured -20 px, a
    // compromise between the two halves), so zero motion is no longer the cheapest candidate
    // of the static half's search window - it carries the window's distance penalty.
    CHECK(std::abs(second.globalMotionX) >= float(kAliasCell));

    const uint32_t splitCell = kAliasSplit / kAliasCell;
    size_t movingCells = 0, movingCorrect = 0, staticCells = 0, staticMoved = 0;
    float worstStatic = 0.0f;
    for (uint32_t y = 2; y + 2 < second.gridH; ++y) {
        for (uint32_t x = 2; x + 2 < second.gridW; ++x) {
            const size_t o = (size_t(y) * second.gridW + x) * 4u;
            const float vx = second.guideGridRGBA32F[o + 0], vy = second.guideGridRGBA32F[o + 1];
            if (x + 2 < splitCell) {
                ++movingCells;
                // Guide vectors point current -> previous, so content that moved +30 px reads -30.
                if (std::abs(vx + float(kAliasShift)) <= 4.0f && std::abs(vy) <= 4.0f) ++movingCorrect;
            } else if (x > splitCell + 2) {
                ++staticCells;
                const float magnitude = std::abs(vx) + std::abs(vy);
                worstStatic = std::max(worstStatic, magnitude);
                if (magnitude > 1.0f) ++staticMoved;
            }
        }
    }
    CHECK(movingCells > 1000 && staticCells > 500);
    // Real motion still has to survive: the noise half moved and must report it.
    CHECK(movingCorrect * 10 >= movingCells * 9);
    // The static half must report no motion at all. The pre-confidence estimator emitted the
    // aliased 20 px match in 3128 of these 3196 cells, because it only rejected vectors whose
    // absolute residual was high and a perfect periodic match has none.
    if (staticMoved) std::cerr << "static cells with motion: " << staticMoved << '/' << staticCells
                               << " worst=" << worstStatic << " px\n";
    CHECK_EQ(size_t{0}, staticMoved);
}

// Synchronized playback fakes stamp decoder-style identities on a 25 fps
// timeline so range offsets are exact.
constexpr double kSyncFps = 25.0;
constexpr int64_t kSyncFrame100ns = 400000;

class IdentitySource final : public ISynchronizedFrameSource {
public:
    // `skew100ns` mimics a seeked FFmpeg source whose rebased timestamps land
    // a few ticks below the canonical CFR grid; frame numbers stay exact
    // because the decoder rounds them onto the grid.
    explicit IdentitySource(std::vector<uint64_t> frameNumbers, uint64_t totalFrames,
                            int64_t skew100ns = 0)
        : duration(double(totalFrames) / kSyncFps)
    {
        for (const uint64_t number : frameNumbers) {
            VideoFrame frame;
            frame.bgra = {uint8_t(number), 0, 0, 255};
            frame.timestamp100ns = int64_t(number) * kSyncFrame100ns + skew100ns;
            frame.frameNumber = number;
            frame.sourceGeneration = generation;
            frames.push_back(std::move(frame));
        }
    }
    bool Open(const std::filesystem::path&, std::stop_token) override { index = 0; ++generation; return true; }
    void Close() override {}
    VideoReadResult Read(VideoFrame& frame, std::stop_token stop) override
    {
        if (stop.stop_requested()) return VideoReadResult::Cancelled;
        if (index >= frames.size()) return VideoReadResult::EndOfStream;
        frame = frames[index++];
        frame.sourceGeneration = generation;
        return VideoReadResult::FrameReady;
    }
    bool SeekSeconds(double seconds) override
    {
        ++generation;
        lastSeekSeconds = seconds;
        // FFmpeg's input seek lands on the frame containing the requested time,
        // so a timestamp a few ticks below the request is not skipped.
        const int64_t target = std::llround(seconds * 1e7) - kSyncFrame100ns / 2;
        index = 0;
        while (index < frames.size() && frames[index].timestamp100ns < target) ++index;
        return true;
    }
    uint32_t Width() const override { return 1; }
    uint32_t Height() const override { return 1; }
    double FrameRate() const override { return kSyncFps; }
    double DurationSeconds() const override { return duration; }

    std::vector<VideoFrame> frames;
    size_t index{};
    uint32_t generation{};
    double duration{};
    double lastSeekSeconds{-1.0};
};

std::vector<uint64_t> Sequence(uint64_t first, uint64_t count)
{
    std::vector<uint64_t> numbers;
    for (uint64_t i = 0; i < count; ++i) numbers.push_back(first + i);
    return numbers;
}

void synchronized_range_offsets_neural_frames_onto_the_original_timeline_test()
{
    IdentitySource original(Sequence(0, 40), 40);
    IdentitySource neural(Sequence(0, 3), 3);
    SynchronizedPlayback playback(original, neural);
    const SynchronizedRange range{10 * kSyncFrame100ns, 13 * kSyncFrame100ns};
    CHECK(playback.Open(L"o", L"n", {}, range));
    CHECK_EQ(range.start100ns, playback.Range().start100ns);
    CHECK_EQ(range.end100ns, playback.Range().end100ns);
    CHECK(std::abs(original.lastSeekSeconds - 0.4) < 1e-9);

    for (uint64_t expected = 10; expected < 13; ++expected) {
        CHECK_EQ(SynchronizedReadResult::PairReady, playback.ReadNextAvailable({}));
        const auto* pair = playback.CurrentPair();
        CHECK(pair != nullptr);
        if (!pair) return;
        CHECK_EQ(expected, pair->frameNumber);
        CHECK_EQ(expected, pair->original.frameNumber);
        CHECK_EQ(expected, pair->neural.frameNumber);
        CHECK_EQ(int64_t(expected) * kSyncFrame100ns, pair->timestamp100ns);
        CHECK_EQ(int64_t(expected) * kSyncFrame100ns, pair->neural.timestamp100ns);
        CHECK_EQ(uint8_t(expected), pair->original.bgra[0]);
        CHECK_EQ(uint8_t(expected - 10), pair->neural.bgra[0]);
    }
    // The original continues past the range; playback ends at range.end.
    CHECK_EQ(SynchronizedReadResult::EndOfStream, playback.ReadNextAvailable({}));
}

void synchronized_range_ends_on_a_rebased_original_timestamp_test()
{
    // A seek inside a range render rebases the original's timestamps slightly
    // early. The first frame after the range must still end playback instead
    // of pairing against an exhausted neural stream.
    IdentitySource original(Sequence(0, 40), 40, -3);
    IdentitySource neural(Sequence(0, 3), 3);
    SynchronizedPlayback playback(original, neural);
    const SynchronizedRange range{10 * kSyncFrame100ns, 13 * kSyncFrame100ns};
    CHECK(playback.Open(L"o", L"n", {}, range));
    for (uint64_t expected = 10; expected < 13; ++expected) {
        CHECK_EQ(SynchronizedReadResult::PairReady, playback.ReadNextAvailable({}));
        const auto* pair = playback.CurrentPair();
        CHECK(pair != nullptr);
        if (!pair) return;
        CHECK_EQ(expected, pair->frameNumber);
    }
    CHECK_EQ(SynchronizedReadResult::EndOfStream, playback.ReadNextAvailable({}));
}

void synchronized_range_ending_on_the_last_source_frame_completes_test()
{
    // Previewing the tail of a video produces a range whose end is the source
    // duration: no frame after it exists, so the original hits EOF instead of
    // tripping the end test. That must still end playback cleanly.
    IdentitySource original(Sequence(0, 40), 40);
    IdentitySource neural(Sequence(0, 3), 3);
    SynchronizedPlayback playback(original, neural);
    CHECK(playback.Open(L"o", L"n", {}, SynchronizedRange{37 * kSyncFrame100ns, 40 * kSyncFrame100ns}));
    for (uint64_t expected = 37; expected < 40; ++expected) {
        CHECK_EQ(SynchronizedReadResult::PairReady, playback.ReadNextAvailable({}));
        const auto* pair = playback.CurrentPair();
        CHECK(pair != nullptr);
        if (!pair) return;
        CHECK_EQ(expected, pair->frameNumber);
    }
    CHECK_EQ(SynchronizedReadResult::EndOfStream, playback.ReadNextAvailable({}));

    // The same applies to a single last frame, which is what the one-frame
    // preview produces at the end of a source.
    IdentitySource lastOriginal(Sequence(0, 40), 40);
    IdentitySource lastNeural(Sequence(0, 1), 1);
    SynchronizedPlayback lastFrame(lastOriginal, lastNeural);
    CHECK(lastFrame.Open(L"o", L"n", {}, SynchronizedRange{39 * kSyncFrame100ns, 40 * kSyncFrame100ns}));
    CHECK_EQ(SynchronizedReadResult::PairReady, lastFrame.ReadNextAvailable({}));
    CHECK_EQ(SynchronizedReadResult::EndOfStream, lastFrame.ReadNextAvailable({}));
}

void synchronized_range_rejects_a_neural_render_of_the_wrong_length_test()
{
    IdentitySource original(Sequence(0, 40), 40);
    IdentitySource neural(Sequence(0, 5), 5);
    SynchronizedPlayback playback(original, neural);
    CHECK(!playback.Open(L"o", L"n", {}, SynchronizedRange{10 * kSyncFrame100ns, 13 * kSyncFrame100ns}));
    CHECK(!playback.NeuralAvailable());
    CHECK(playback.Open(L"o", L"n", {}, SynchronizedRange{10 * kSyncFrame100ns, 15 * kSyncFrame100ns}));
}

void synchronized_range_seek_clamps_into_the_window_test()
{
    IdentitySource original(Sequence(0, 40), 40);
    IdentitySource neural(Sequence(0, 4), 4);
    SynchronizedPlayback playback(original, neural);
    CHECK(playback.Open(L"o", L"n", {}, SynchronizedRange{10 * kSyncFrame100ns, 14 * kSyncFrame100ns}));

    CHECK(playback.SeekSeconds(0.0, {}));
    const auto* pair = playback.CurrentPair();
    CHECK(pair != nullptr);
    if (pair) {
        CHECK_EQ(uint64_t{10}, pair->frameNumber);
        CHECK_EQ(uint64_t{10}, pair->neural.frameNumber);
    }
    CHECK(std::abs(neural.lastSeekSeconds) < 1e-9);

    // Past the end clamps to the last frame inside the window.
    CHECK(playback.SeekSeconds(5.0, {}));
    pair = playback.CurrentPair();
    CHECK(pair != nullptr);
    if (pair) CHECK_EQ(uint64_t{13}, pair->frameNumber);
    CHECK(std::abs(neural.lastSeekSeconds - 0.12) < 1e-9);

    CHECK(playback.SeekSeconds(0.48, {}));
    pair = playback.CurrentPair();
    if (pair) CHECK_EQ(uint64_t{12}, pair->frameNumber);
    CHECK(std::abs(neural.lastSeekSeconds - 0.08) < 1e-9);
}

void synchronized_playback_reports_a_frame_number_mismatch_test()
{
    IdentitySource original(Sequence(0, 4), 4);
    IdentitySource neural({0, 2, 3}, 4);
    SynchronizedPlayback playback(original, neural);
    CHECK(playback.Open(L"o", L"n", {}));
    CHECK_EQ(SynchronizedReadResult::PairReady, playback.ReadNextAvailable({}));
    // Original frame 1 meets neural frame 2 inside the old one-frame timestamp
    // tolerance; identity refuses to pair them.
    CHECK_EQ(SynchronizedReadResult::OutOfSync, playback.ReadNextAvailable({}));
    const auto* pair = playback.CurrentPair();
    CHECK(pair != nullptr);
    if (pair) CHECK_EQ(uint64_t{0}, pair->frameNumber);
    // The earlier member was dropped, so the streams resynchronize at frame 2.
    CHECK_EQ(SynchronizedReadResult::PairReady, playback.ReadNextAvailable({}));
    pair = playback.CurrentPair();
    if (pair) {
        CHECK_EQ(uint64_t{2}, pair->frameNumber);
        CHECK_EQ(uint64_t{2}, pair->neural.frameNumber);
    }
    CHECK_EQ(SynchronizedReadResult::PairReady, playback.ReadNextAvailable({}));
    pair = playback.CurrentPair();
    if (pair) CHECK_EQ(uint64_t{3}, pair->frameNumber);
}

void identity_of_copies_the_decoded_frame_fields_test()
{
    VideoFrame frame;
    frame.timestamp100ns = 4000000;
    frame.frameNumber = 10;
    frame.sourceGeneration = 3;
    const FrameIdentity id = IdentityOf(frame, 7, 42, HistoryReset::Seek);
    CHECK_EQ(uint64_t{10}, id.frameNumber);
    CHECK_EQ(int64_t{4000000}, id.pts100ns);
    CHECK_EQ(uint32_t{3}, id.sourceGeneration);
    CHECK_EQ(uint32_t{7}, id.historyGeneration);
    CHECK_EQ(uint64_t{42}, id.jobId);
    CHECK_EQ(HistoryReset::Seek, id.reset);
    CHECK(id.SameSource(IdentityOf(frame, 9, 42, HistoryReset::None)));
    CHECK(!id.SameSource(IdentityOf(frame, 7, 43, HistoryReset::Seek)));
}

} // namespace

int main()
{
    guide_controls_neutralize_disabled_guides_test();
    guide_generator_reports_reset_reasons_test();
    guide_generator_reevaluates_a_repeated_frame_without_reset_test();
    generate_treats_nv12_limited_range_luma_like_bgra_test();
    flow_rejects_aliased_vectors_on_static_repetitive_content_test();
    scene_cut_needs_low_histogram_overlap_or_a_large_residual_test();
    decoded_pair_evidence_separates_a_pan_from_a_cut_test();
    weak_scene_cuts_are_suppressed_inside_the_minimum_interval_test();
    a_real_cut_one_frame_inside_the_old_window_is_taken_test();
    a_strong_scene_cut_fires_inside_the_minimum_interval_test();
    a_declared_reset_rearms_the_scene_cut_interval_test();
    scene_cut_accounting_survives_reset_and_counts_each_arm_test();
    synchronized_range_offsets_neural_frames_onto_the_original_timeline_test();
    synchronized_range_ends_on_a_rebased_original_timestamp_test();
    synchronized_range_ending_on_the_last_source_frame_completes_test();
    synchronized_range_rejects_a_neural_render_of_the_wrong_length_test();
    synchronized_range_seek_clamps_into_the_window_test();
    synchronized_playback_reports_a_frame_number_mismatch_test();
    identity_of_copies_the_decoded_frame_fields_test();
    return test_support::failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
