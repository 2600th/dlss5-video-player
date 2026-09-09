#include "SynchronizedPlayback.h"
#include "TemporalGuides.h"
#include "TestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 180;
constexpr double kFps = 30.0;

// Smooth textured field so block matching has unambiguous correspondences.
std::vector<uint8_t> TexturedFrame(int shiftX, int shiftY)
{
    std::vector<uint8_t> bgra(size_t(kWidth) * kHeight * 4u);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const double sx = double(int(x) - shiftX), sy = double(int(y) - shiftY);
            const double v = 0.5 + 0.25 * std::sin(sx * 0.21) * std::cos(sy * 0.17) +
                             0.2 * std::sin(sx * 0.05 + sy * 0.09);
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

    GuideFrame noMotion;
    run(GuideControls{false, true, true}, noMotion);
    CHECK(!Channel(noMotion, 0).anyNonZero);
    CHECK(!Channel(noMotion, 1).anyNonZero);
    CHECK_EQ(full.gridW, noMotion.gridW);
    CHECK_EQ(full.gridH, noMotion.gridH);

    GuideFrame noMask;
    run(GuideControls{true, true, false}, noMask);
    CHECK(!Channel(noMask, 3).anyNonZero);
    CHECK(Channel(noMask, 0).anyNonZero || Channel(noMask, 1).anyNonZero);

    GuideFrame flatDepth;
    run(GuideControls{true, false, true}, flatDepth);
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
    flow_rejects_aliased_vectors_on_static_repetitive_content_test();
    scene_cut_needs_low_histogram_overlap_or_a_large_residual_test();
    synchronized_range_offsets_neural_frames_onto_the_original_timeline_test();
    synchronized_range_ends_on_a_rebased_original_timestamp_test();
    synchronized_range_ending_on_the_last_source_frame_completes_test();
    synchronized_range_rejects_a_neural_render_of_the_wrong_length_test();
    synchronized_range_seek_clamps_into_the_window_test();
    synchronized_playback_reports_a_frame_number_mismatch_test();
    identity_of_copies_the_decoded_frame_fields_test();
    return test_support::failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
