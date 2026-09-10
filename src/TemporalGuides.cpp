#include "TemporalGuides.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "ParallelFor.h"

namespace {
// Grid work is small in absolute terms, so these keep tiny analysis grids on the calling
// thread rather than paying dispatch for a few hundred cells.
// Waking a parked worker costs single-digit microseconds, so a range has to carry more
// work than that to be worth splitting off. These were tuned by measurement on a 1080p
// grid: smaller values made guide generation slower, not faster.
constexpr size_t kGuideRowGrain = 16;        // grid rows per range
constexpr size_t kGuideCandidateGrain = 64;  // global-search candidates per range
constexpr size_t kGuideLatticeGrain = 2;     // block-match lattice rows per range
}  // namespace

void TemporalGuideGenerator::Reset() {
    m_prevLuma.clear();
    m_lastLuma.clear();
    m_prevDepth.clear();
    m_gridW = m_gridH = 0;
    m_havePrev = false;
    m_firstFrame = true;
    // A declared reset already wiped the DLSS history, so there is nothing left for a
    // suppressed weak cut to protect: re-arm the weak arm instead of making the first
    // real cut after a seek wait out the interval.
    m_framesSinceCut = 0;
    m_haveAcceptedCut = false;
}

void TemporalGuideGenerator::SetControls(const GuideControls& controls) {
    if (controls.depth != m_controls.depth) m_prevDepth.clear();
    m_controls = controls;
}

bool TemporalGuideGenerator::IsRepeat(const FrameIdentity& frame) const {
    return !m_firstFrame && m_havePrev && frame.reset == HistoryReset::None &&
           frame.frameNumber == m_lastFrameNumber && frame.pts100ns == m_lastPts &&
           frame.sourceGeneration == m_lastSourceGeneration;
}

HistoryReset TemporalGuideGenerator::ClassifyReset(const FrameIdentity& frame, uint32_t gw, uint32_t gh) const {
    if (m_firstFrame || !m_havePrev) return HistoryReset::FirstFrame;
    if (frame.reset != HistoryReset::None) return frame.reset;
    if (frame.sourceGeneration != m_lastSourceGeneration) return HistoryReset::SourceChange;
    if (gw != m_gridW || gh != m_gridH) return HistoryReset::SourceChange;
    if (frame.frameNumber == m_lastFrameNumber + 1 || IsRepeat(frame)) return HistoryReset::None;
    // A forward gap means frames were dropped between us and the previous
    // guide; a backwards step means the timeline was re-positioned.
    return frame.frameNumber > m_lastFrameNumber ? HistoryReset::Drop : HistoryReset::Seek;
}

float TemporalGuideGenerator::LumaHistogramIntersection(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.0f;
    constexpr int bins = 32;
    std::array<float, bins> ha{}, hb{};
    for (const float v : a) ++ha[size_t(std::clamp(int(v * bins), 0, bins - 1))];
    for (const float v : b) ++hb[size_t(std::clamp(int(v * bins), 0, bins - 1))];
    float overlap = 0.0f;
    for (int i = 0; i < bins; ++i) overlap += std::min(ha[size_t(i)], hb[size_t(i)]);
    return overlap / float(a.size());
}

// Measured on the benchmark corpus: fast pans reach residual 0.10-0.13 with histogram
// overlap >= 0.91; real cuts show residual 0.24-0.40 with overlap <= 0.47, and the softest
// real cut observed was 0.108 / 0.78. Pairing a residual with a histogram distance under a
// threshold band is x265's --hist-scenecut design; NVIDIA documents no threshold and no
// detection method at all, so none of these numbers can be attributed to them.
constexpr double kCutResidualStrong = 0.30;    // correspondence failed outright
constexpr double kCutResidualWeak = 0.10;      // more than a pan, less than a certainty
constexpr double kCutHistogramOverlap = 0.85;  // luma distribution no longer the same scene
// PySceneDetect's min_scene_len, whose CLI default is 0.6 s (its API default of 15 frames is
// the same thing at 25 fps), applied as the same kind of hard minimum-interval filter its
// FlashFilter uses in SUPPRESS mode. This is the debounce the weak arm was missing: a
// contributor measured 6 fires in 12 frames (#19119-19130) and 11 in 15 frames on another
// clip, and per DLSS Programming Guide 310.6.0 S3.13 over-firing InReset is the documented
// failure mode ("temporal flickering, heavy aliasing or other visual artifacts"), not a
// missed reset. FFmpeg's scdet has no debounce; it also never has to protect an upscaler's
// accumulated history.
constexpr double kMinSecondsBetweenCuts = 0.6;

SceneCutStrength TemporalGuideGenerator::ClassifySceneCut(double residual, double histogramOverlap) {
    if (residual > kCutResidualStrong) return SceneCutStrength::Residual;
    if (residual > kCutResidualWeak && histogramOverlap < kCutHistogramOverlap) return SceneCutStrength::Histogram;
    return SceneCutStrength::None;
}

bool TemporalGuideGenerator::IsSceneCut(float globalMatchCost, float histogramIntersection) {
    return ClassifySceneCut(globalMatchCost, histogramIntersection) != SceneCutStrength::None;
}

uint32_t TemporalGuideGenerator::MinFramesBetweenCuts(double fps) {
    // A non-finite or absurd frame rate still has to leave the weak arm usable, and two
    // frames is the shortest interval that suppresses anything at all.
    if (!std::isfinite(fps) || fps <= 0.0) return 2;
    return std::max<uint32_t>(2, uint32_t(std::lround(kMinSecondsBetweenCuts * fps)));
}

std::pair<uint32_t,uint32_t> TemporalGuideGenerator::AnalysisGrid(uint32_t sourceW, uint32_t sourceH, double targetFps) {
    if (!sourceW || !sourceH) return {0,0};
    // High-frame-rate playback uses a slightly more compact analysis field so guide
    // generation cannot become the reason a 50/60-fps movie misses realtime. The GPU
    // still expands this field to the exact DLSS input resolution.
    const bool highFps = std::isfinite(targetFps) && targetFps >= 45.0;
    const uint32_t maxGridW = highFps ? 128u : 160u;
    const uint32_t divisor = highFps ? 14u : 10u;
    const uint32_t gw = std::clamp(sourceW / divisor, 96u, maxGridW);
    const uint32_t minH = highFps ? 48u : 54u;
    const uint32_t gh = std::max(minH, uint32_t((uint64_t(gw) * sourceH) / sourceW));
    return {gw, gh};
}

float TemporalGuideGenerator::Luma(const uint8_t* p) {
    // BGRA -> Rec.709-ish luma in [0,1].
    return (0.0722f * p[0] + 0.7152f * p[1] + 0.2126f * p[2]) * (1.0f / 255.0f);
}

void TemporalGuideGenerator::DownsampleLuma(const uint8_t* bgra, uint32_t w, uint32_t h,
                                             uint32_t gw, uint32_t gh, std::vector<float>& out) const {
    out.assign(size_t(gw) * gh, 0.0f);
    // Each grid row samples a disjoint band of the source frame, so the rows are
    // independent. The reads stride a whole 4K frame, which makes this more
    // memory-latency bound than its op count suggests.
    ParallelForRanges(size_t(gh), kGuideRowGrain, [&](size_t beginRow, size_t endRow) {
        for (uint32_t gy = uint32_t(beginRow); gy < uint32_t(endRow); ++gy) {
            const uint32_t y0 = uint32_t((uint64_t(gy) * h) / gh);
            const uint32_t y1 = std::max(y0 + 1, uint32_t((uint64_t(gy + 1) * h) / gh));
            for (uint32_t gx = 0; gx < gw; ++gx) {
                const uint32_t x0 = uint32_t((uint64_t(gx) * w) / gw);
                const uint32_t x1 = std::max(x0 + 1, uint32_t((uint64_t(gx + 1) * w) / gw));
                // Four stratified samples are much cheaper than averaging every source pixel.
                const uint32_t xs[2] = { x0, std::min(w - 1, (x0 + x1) / 2) };
                const uint32_t ys[2] = { y0, std::min(h - 1, (y0 + y1) / 2) };
                float s = 0.0f;
                for (uint32_t yy : ys) for (uint32_t xx : xs)
                    s += Luma(bgra + (size_t(yy) * w + xx) * 4u);
                out[size_t(gy) * gw + gx] = s * 0.25f;
            }
        }
    });
}

// Returns the candidate's full cost, penalty included, or +inf if it provably cannot
// beat `best`.
//
// The early-out is exact, not an approximation. Taps are non-negative so the running sad
// only grows, and count never exceeds nine, therefore sad/9 + penalty is a lower bound on
// the final sad/count + penalty. A candidate pruned by it would have failed the strict
// `< best` test anyway, so the selected vector is bit-identical to the exhaustive search.
// Comparing against `best` directly rather than against a precomputed `best - penalty`
// avoids a subtraction whose rounding could prune slightly too eagerly. For the same
// reason the divisor is a real division: sad * (1.0f/9.0f) multiplies by a rounded
// reciprocal and can land an ulp above sad/9, which is enough to prune a candidate that
// should have won. IEEE division is correctly rounded and monotonic in the divisor, so
// sad/9 <= sad/count for every count <= 9.
static float PatchSadCost(const std::vector<float>& cur, const std::vector<float>& prev,
                          int x, int y, int dx, int dy, int w, int h,
                          float penalty, float best) {
    float sad = 0.0f;
    int count = 0;
    for (int py = -1; py <= 1; ++py) {
        const int cy = y + py, oy = cy + dy;
        if (cy >= 0 && cy < h && oy >= 0 && oy < h) {
            for (int px = -1; px <= 1; ++px) {
                const int cx = x + px, ox = cx + dx;
                if (cx < 0 || cx >= w || ox < 0 || ox >= w) continue;
                sad += std::abs(cur[size_t(cy) * w + cx] - prev[size_t(oy) * w + ox]);
                ++count;
            }
        }
        if (sad / 9.0f + penalty >= best)
            return std::numeric_limits<float>::infinity();
    }
    return (count ? sad / float(count) : 10.0f) + penalty;
}

static float SampleBilinear(const std::vector<float>& img, float x, float y, int w, int h) {
    if (x < 0.0f || y < 0.0f || x > float(w - 1) || y > float(h - 1))
        return std::numeric_limits<float>::quiet_NaN();
    const int x0 = std::clamp(int(std::floor(x)), 0, w - 1);
    const int y0 = std::clamp(int(std::floor(y)), 0, h - 1);
    const int x1 = std::min(x0 + 1, w - 1);
    const int y1 = std::min(y0 + 1, h - 1);
    const float tx = x - float(x0), ty = y - float(y0);
    const float a = img[size_t(y0) * w + x0] * (1.0f - tx) + img[size_t(y0) * w + x1] * tx;
    const float b = img[size_t(y1) * w + x0] * (1.0f - tx) + img[size_t(y1) * w + x1] * tx;
    return a * (1.0f - ty) + b * ty;
}

// Same exact early-out as PatchSadCost. This one matters more: each tap costs a bilinear
// fetch, and the refine sweeps twenty-five sub-positions per block.
static float PatchSadSubpixelCost(const std::vector<float>& cur, const std::vector<float>& prev,
                                  int x, int y, float dx, float dy, int w, int h,
                                  float penalty, float best) {
    float sad = 0.0f;
    int count = 0;
    for (int py = -1; py <= 1; ++py) {
        const int cy = y + py;
        if (cy >= 0 && cy < h) {
            for (int px = -1; px <= 1; ++px) {
                const int cx = x + px;
                if (cx < 0 || cx >= w) continue;
                const float pv = SampleBilinear(prev, float(cx) + dx, float(cy) + dy, w, h);
                if (!std::isfinite(pv)) continue;
                sad += std::abs(cur[size_t(cy) * w + cx] - pv);
                ++count;
            }
        }
        if (sad / 9.0f + penalty >= best)
            return std::numeric_limits<float>::infinity();
    }
    return (count ? sad / float(count) : 10.0f) + penalty;
}

// Rejection thresholds, in mean-absolute-luma units of the 3x3 patch cost. Picked from a sweep
// over the benchmark corpus (5 clips, 92 frame pairs) where every emitted vector was scored by
// warping the previous frame's FULL-RESOLUTION pixels, so the score is independent of the cost
// function being thresholded. Mean per-cell warp residual, old algorithm -> this one (with the
// residual of emitting nothing at all in brackets): cuts-motion 0.0666 -> 0.0548 [0.1029],
// text-subtitles 0.0020 -> 0.0015 [0.0015], faces 0.0049 -> 0.0048 [0.0049], fine-detail
// 0.0480 -> 0.0423 [0.0370], highlights-gradients 0.0051 -> 0.0064 [0.0077].
//
// A winner that beats standing still by less than kMotionEvidence does not reduce the residual
// on any clip - on text-subtitles and faces such vectors made it WORSE than not moving, which
// is the whole reason mv-on used to lose to mv-off. Sweeping the threshold, 0.02/0.03/0.04 give
// cuts-motion 0.0556/0.0548/0.0549 and highlights-gradients 0.0061/0.0064/0.0067, so 0.03 is
// the knee: it keeps the low-contrast real motion that a stricter gate drops.
constexpr float kMotionEvidence = 0.03f;
// Between kMotionEvidence and kDecisiveEvidence the evidence exists but is not decisive, and
// only there does a reverse (previous -> current) search pay for itself: gating that band on the
// round trip moved fine-detail 0.0482 -> 0.0440 and cuts-motion 0.0515 -> 0.0513, while running
// it on every cell cost 0.0515 -> 0.0546 because it starts rejecting vectors that were right.
// Restricting it to the band is also what keeps it affordable: ~5% of the solved cells.
constexpr float kDecisiveEvidence = 0.09f;
// Tolerated round-trip disagreement, in analysis cells (one cell is 12 source pixels at 1080p).
// 0.8 absorbs the subpixel refinement's own quarter-cell disagreement but rejects a full cell of
// drift; loosening it to 1.1 halves the gain on fine-detail.
constexpr float kRoundTripCells = 0.8f;

void TemporalGuideGenerator::EstimateFlow(const std::vector<float>& cur, const std::vector<float>& prev,
                                           uint32_t gw, uint32_t gh,
                                           std::vector<float>& flowX, std::vector<float>& flowY,
                                           std::vector<float>& confidence,
                                           float& globalX, float& globalY, float& globalCost) const {
    const int w = int(gw), h = int(gh);
    // First find a coarse whole-frame translation. This is especially valuable for camera pans.
    float bestGlobal = std::numeric_limits<float>::max();
    float zeroGlobal = std::numeric_limits<float>::max();
    int bestGX = 0, bestGY = 0;
    constexpr int globalRadius = 7;
    constexpr int globalSpan = globalRadius * 2 + 1;
    // Score every candidate in parallel, then pick the winner with the original serial
    // scan. Keeping the selection sequential preserves the first-minimum tie-break, so
    // the chosen translation is bit-identical however the work was distributed.
    std::array<float, size_t(globalSpan) * globalSpan> globalCosts{};
    ParallelForRanges(globalCosts.size(), kGuideCandidateGrain,
                      [&](size_t beginCandidate, size_t endCandidate) {
        for (size_t i = beginCandidate; i < endCandidate; ++i) {
            const int dy = int(i) / globalSpan - globalRadius;
            const int dx = int(i) % globalSpan - globalRadius;
            float sad = 0.0f; int n = 0;
            for (int y = 4; y < h - 4; y += 4) {
                const int oy = y + dy; if (oy < 0 || oy >= h) continue;
                for (int x = 4; x < w - 4; x += 4) {
                    const int ox = x + dx; if (ox < 0 || ox >= w) continue;
                    sad += std::abs(cur[size_t(y) * w + x] - prev[size_t(oy) * w + ox]);
                    ++n;
                }
            }
            if (n) sad /= float(n);
            // Mild penalty avoids jumping to large vectors in flat/noisy regions.
            sad += 0.0015f * float(dx * dx + dy * dy);
            globalCosts[i] = sad;
        }
    });
    for (size_t i = 0; i < globalCosts.size(); ++i) {
        const int dy = int(i) / globalSpan - globalRadius;
        const int dx = int(i) % globalSpan - globalRadius;
        const float sad = globalCosts[i];
        if (dx == 0 && dy == 0) zeroGlobal = sad;
        if (sad < bestGlobal) { bestGlobal = sad; bestGX = dx; bestGY = dy; }
    }
    // A small independently moving object on an otherwise flat/static frame can make a
    // whole-frame translation look marginally better than zero. Do not smear that motion
    // over every pixel unless the global shift wins by a meaningful margin. Local block
    // matching below will still recover object motion around the zero/global seed.
    if ((bestGX != 0 || bestGY != 0) && std::isfinite(zeroGlobal) &&
        (zeroGlobal - bestGlobal) < 0.012f) {
        bestGX = bestGY = 0;
        bestGlobal = zeroGlobal;
    }
    globalX = float(bestGX); globalY = float(bestGY); globalCost = bestGlobal;

    // Cells that produce no evidence of motion stay at zero rather than inheriting the global
    // vector: measured on the corpus, seeding them with the global translation is much worse
    // (cuts-motion residual 0.0675 vs 0.0519), because a cell with no correspondence evidence
    // is a cell whose content does not support ANY displacement.
    flowX.assign(size_t(gw) * gh, 0.0f);
    flowY.assign(size_t(gw) * gh, 0.0f);
    confidence.assign(size_t(gw) * gh, 0.0f);
    constexpr int localRadius = 3;
    constexpr int localSpan = 2 * localRadius + 1;
    constexpr int reverseRadius = 2;
    // Solve local flow on a 2x2 lattice, then expand each result to the tiny block.
    // At a 160-wide analysis grid this retains useful object motion while making
    // 30/60 fps playback much less CPU-bound than matching every grid pixel.
    //
    // One body, run over a range of lattice rows. ParallelForRanges keeps it on the
    // calling thread when the grid is too small to be worth splitting, so there is no
    // separate serial copy to keep in sync. Lattice cells are independent: each reads
    // only the two luma grids and writes its own disjoint 2x2 block of the output
    // fields, so the per-cell state below is all the shared state there is.
    const int totalStepsY = (h + 1) / 2;
    ParallelForRanges(size_t(std::max(0, totalStepsY)), kGuideLatticeGrain, [&](size_t beginStep, size_t endStep) {
        for (size_t step = beginStep; step < endStep; ++step) {
            const int y = int(step) * 2;
            for (int x = 0; x < w; x += 2) {
                std::array<float, size_t(localSpan) * localSpan> costs{};
                float best = std::numeric_limits<float>::max();
                int bestIndex = 0;
                for (int oy = -localRadius; oy <= localRadius; ++oy) {
                    for (int ox = -localRadius; ox <= localRadius; ++ox) {
                        const int index = (oy + localRadius) * localSpan + (ox + localRadius);
                        const int dx = bestGX + ox, dy = bestGY + oy;
                        const float penalty = 0.002f * float(ox * ox + oy * oy);
                        // The whole window's costs are kept: the zero-cost null hypothesis and
                        // the peakedness term below read the true cost of every rival, so this
                        // scan must not prune. An infinite bound disables PatchSadCost's
                        // early-out and returns every candidate's exact full cost.
                        const float cost = PatchSadCost(cur, prev, x, y, dx, dy, w, h, penalty,
                                                        std::numeric_limits<float>::infinity());
                        costs[size_t(index)] = cost;
                        if (cost < best) { best = cost; bestIndex = index; }
                    }
                }
                const int bestOX = bestIndex % localSpan - localRadius;
                const int bestOY = bestIndex / localSpan - localRadius;
                const int bx = bestGX + bestOX, by = bestGY + bestOY;

                // Zero motion is the null hypothesis this cell has to beat. It is usually already
                // in the search window (the window is centred on the global vector).
                const bool zeroInWindow = std::abs(bestGX) <= localRadius && std::abs(bestGY) <= localRadius;
                const float zeroCost = zeroInWindow
                    ? costs[size_t((-bestGY + localRadius) * localSpan + (-bestGX + localRadius))]
                    : PatchSadCost(cur, prev, x, y, 0, 0, w, h, 0.0f,
                                   std::numeric_limits<float>::infinity());
                const float evidence = zeroCost - best;

                float fbx = 0.0f, fby = 0.0f, conf = 0.0f;
                if (evidence >= kMotionEvidence) {
                    fbx = float(bx); fby = float(by);
                    // Integer block matching on a compact grid is too quantized after scaling to
                    // 1440p/4K. Refine the winning vector at quarter-grid precision using bilinear
                    // samples of the previous frame. This keeps the CPU implementation self-contained
                    // while giving DLSS materially smoother per-pixel motion. Only accepted cells are
                    // refined, so static content no longer pays for 25 subpixel probes per cell.
                    static constexpr float sub[] = {-0.50f, -0.25f, 0.0f, 0.25f, 0.50f};
                    float refined = best;
                    for (float sy : sub) {
                        for (float sx : sub) {
                            const float dx = float(bx) + sx, dy = float(by) + sy;
                            const float cost = PatchSadSubpixelCost(cur, prev, x, y, dx, dy, w, h,
                                                                    0.0015f * (sx * sx + sy * sy),
                                                                    refined);
                            if (cost < refined) { refined = cost; fbx = dx; fby = dy; }
                        }
                    }
                    best = refined;

                    bool consistent = true;
                    if (evidence < kDecisiveEvidence) {
                        // Forward/backward consistency: from where this cell claims to have come,
                        // search back for where it goes. A winner produced by aliasing or by a
                        // repeating pattern does not survive, because the reverse landscape has its
                        // minimum somewhere else. The reverse seed is the negated forward vector and
                        // the window only +-2 cells: this is a confirmation, not a fresh estimate.
                        const int qx = std::clamp(x + int(std::lround(fbx)), 0, w - 1);
                        const int qy = std::clamp(y + int(std::lround(fby)), 0, h - 1);
                        const int seedX = -int(std::lround(fbx)), seedY = -int(std::lround(fby));
                        float reverseBest = std::numeric_limits<float>::max();
                        int rx = seedX, ry = seedY;
                        for (int oy = -reverseRadius; oy <= reverseRadius; ++oy) {
                            for (int ox = -reverseRadius; ox <= reverseRadius; ++ox) {
                                // The tiny penalty only breaks ties towards the seed; without it a
                                // flat reverse landscape hands back the first candidate scanned.
                                // PatchSadCost's early-out only skips candidates that would fail
                                // this strict comparison anyway, so the winner is unchanged.
                                const float cost = PatchSadCost(prev, cur, qx, qy, seedX + ox, seedY + oy,
                                                                w, h, 0.0005f * float(ox * ox + oy * oy),
                                                                reverseBest);
                                if (cost < reverseBest) { reverseBest = cost; rx = seedX + ox; ry = seedY + oy; }
                            }
                        }
                        const float rtx = fbx + float(rx), rty = fby + float(ry);
                        consistent = std::sqrt(rtx * rtx + rty * rty) <= kRoundTripCells;
                    }

                    if (consistent) {
                        // Confidence from the SAD landscape: how much the winner beats standing still
                        // (absolute evidence) tempered by how much it beats the best *distinct* rival
                        // (peakedness - a periodic or textureless cell has many equally good minima).
                        float second = std::numeric_limits<float>::max();
                        for (int oy = -localRadius; oy <= localRadius; ++oy) {
                            for (int ox = -localRadius; ox <= localRadius; ++ox) {
                                if (std::max(std::abs(ox - bestOX), std::abs(oy - bestOY)) <= 1) continue;
                                second = std::min(second, costs[size_t((oy + localRadius) * localSpan + (ox + localRadius))]);
                            }
                        }
                        const float distinct = second > 1e-4f && std::isfinite(second)
                            ? std::clamp((second - best) / second, 0.0f, 1.0f) : 0.0f;
                        conf = std::clamp(evidence / kDecisiveEvidence, 0.0f, 1.0f) * (0.25f + 0.75f * distinct);
                    } else {
                        fbx = fby = 0.0f;
                    }
                }

                for (int yy = y; yy < std::min(y + 2, h); ++yy) {
                    for (int xx = x; xx < std::min(x + 2, w); ++xx) {
                        const size_t oi = size_t(yy) * gw + xx;
                        flowX[oi] = fbx;
                        flowY[oi] = fby;
                        confidence[oi] = conf;
                    }
                }
            }
        }
    });
}

void TemporalGuideGenerator::MedianFlow(std::vector<float>& x, std::vector<float>& y,
                                         const std::vector<float>& confidence,
                                         uint32_t gw, uint32_t gh) const {
    // The field is piecewise constant over the 2x2 solver lattice, so filter one vector per
    // lattice cell over its 3x3 lattice neighbourhood. Two properties matter:
    //  - only cells that passed rejection vote, so a rejected cell can neither vote nor be
    //    resurrected (letting confident neighbours fill rejected cells is the "global fallback"
    //    that measured worse), and
    //  - the output is a confidence-weighted VECTOR median, i.e. always one of the neighbouring
    //    vectors. A per-axis median can return a combination that no cell ever reported.
    const int w = int(gw), h = int(gh);
    const int lw = (w + 1) / 2, lh = (h + 1) / 2;
    std::vector<float> lx(size_t(lw) * lh), ly(size_t(lw) * lh), lc(size_t(lw) * lh);
    for (int ly0 = 0; ly0 < lh; ++ly0) {
        for (int lx0 = 0; lx0 < lw; ++lx0) {
            const size_t src = size_t(ly0 * 2) * gw + size_t(lx0 * 2);
            const size_t dst = size_t(ly0) * lw + size_t(lx0);
            lx[dst] = x[src]; ly[dst] = y[src]; lc[dst] = confidence[src];
        }
    }
    // Every cell reads only the snapshot above and writes its own disjoint 2x2 block, so
    // lattice rows are independent. ParallelForRanges keeps it on the calling thread when
    // the lattice is too small to be worth splitting, so there is no separate serial copy
    // to keep in sync.
    ParallelForRanges(size_t(std::max(0, lh)), kGuideLatticeGrain, [&](size_t beginRow, size_t endRow) {
        for (int ly0 = int(beginRow); ly0 < int(endRow); ++ly0) {
            std::array<float, 9> cx{}, cy{}, cw{};
            for (int lx0 = 0; lx0 < lw; ++lx0) {
                const size_t centre = size_t(ly0) * lw + size_t(lx0);
                if (lc[centre] <= 0.0f) continue;
                size_t k = 0;
                for (int j = -1; j <= 1; ++j) {
                    const int ny = ly0 + j; if (ny < 0 || ny >= lh) continue;
                    for (int i = -1; i <= 1; ++i) {
                        const int nx = lx0 + i; if (nx < 0 || nx >= lw) continue;
                        const size_t n = size_t(ny) * lw + size_t(nx);
                        if (lc[n] <= 0.0f) continue;
                        cx[k] = lx[n]; cy[k] = ly[n]; cw[k] = lc[n]; ++k;
                    }
                }
                if (k < 3) continue;   // too little support to filter; keep the measured vector
                float bestScore = std::numeric_limits<float>::max();
                size_t bestK = 0;
                for (size_t a = 0; a < k; ++a) {
                    float score = 0.0f;
                    for (size_t b = 0; b < k; ++b)
                        score += cw[b] * (std::abs(cx[a] - cx[b]) + std::abs(cy[a] - cy[b]));
                    if (score < bestScore) { bestScore = score; bestK = a; }
                }
                const float mx = cx[bestK], my = cy[bestK];
                for (int yy = ly0 * 2; yy < std::min(ly0 * 2 + 2, h); ++yy) {
                    for (int xx = lx0 * 2; xx < std::min(lx0 * 2 + 2, w); ++xx) {
                        const size_t oi = size_t(yy) * gw + size_t(xx);
                        x[oi] = mx; y[oi] = my;
                    }
                }
            }
        }
    });
}

void TemporalGuideGenerator::BuildDepthProxy(const std::vector<float>& luma,
                                              const std::vector<float>& flowX, const std::vector<float>& flowY,
                                              uint32_t gw, uint32_t gh,
                                              std::vector<float>& depth) {
    depth.assign(size_t(gw) * gh, 0.75f);

    // Left serial on purpose: a grid of this size finishes well inside the cost of
    // waking helper threads.
    float maxMotion = 1.0f;
    for (size_t i = 0; i < flowX.size(); ++i)
        maxMotion = std::max(maxMotion, std::sqrt(flowX[i] * flowX[i] + flowY[i] * flowY[i]));

    ParallelForRanges(size_t(gh), kGuideRowGrain, [&](size_t beginRow, size_t endRow) {
    for (uint32_t y = uint32_t(beginRow); y < uint32_t(endRow); ++y) {
        for (uint32_t x = 0; x < gw; ++x) {
            const size_t idx = size_t(y) * gw + x;
            const float yn = gh > 1 ? float(y) / float(gh - 1) : 0.5f;
            float grad = 0.0f;
            if (x > 0 && x + 1 < gw) grad += std::abs(luma[idx + 1] - luma[idx - 1]);
            if (y > 0 && y + 1 < gh) grad += std::abs(luma[idx + gw] - luma[idx - gw]);
            const float motion = std::sqrt(flowX[idx] * flowX[idx] + flowY[idx] * flowY[idx]) / maxMotion;
            // This is explicitly a VIDEO DEPTH PROXY, not geometric engine depth.
            // It provides stable segmentation/disocclusion hints when a movie has no Z buffer.
            float d = 0.92f - 0.42f * yn - 0.17f * std::clamp(motion, 0.0f, 1.0f)
                            - 0.10f * std::clamp(grad * 2.0f, 0.0f, 1.0f);
            d = std::clamp(d, 0.08f, 0.97f);
            if (m_prevDepth.size() == depth.size()) d = m_prevDepth[idx] * 0.80f + d * 0.20f;
            depth[idx] = d;
        }
    }
    });
    m_prevDepth = depth;
}

bool TemporalGuideGenerator::Generate(const uint8_t* bgra, uint32_t sourceW, uint32_t sourceH,
                                       uint32_t renderW, uint32_t renderH, double targetFps,
                                       const FrameIdentity& frame, GuideFrame& out) {
    if (!bgra || !sourceW || !sourceH || !renderW || !renderH) return false;

    const auto [gw, gh] = AnalysisGrid(sourceW, sourceH, targetFps);
    if (!gw || !gh) return false;
    const bool repeat = IsRepeat(frame) && gw == m_gridW && gh == m_gridH;
    HistoryReset reset = ClassifyReset(frame, gw, gh);
    if (reset != HistoryReset::None) Reset();
    m_gridW = gw; m_gridH = gh;
    // Advance the weak-arm interval once per distinct frame. A re-evaluation of the frame
    // that was just evaluated is not a new frame, and a reset frame's interval starts at
    // the reset itself, which Reset() has already done above.
    if (reset == HistoryReset::None && !repeat && m_framesSinceCut != UINT32_MAX) ++m_framesSinceCut;

    std::vector<float> cur;
    DownsampleLuma(bgra, sourceW, sourceH, gw, gh, cur);

    std::vector<float> fx(size_t(gw) * gh, 0.0f), fy(size_t(gw) * gh, 0.0f);
    std::vector<float> confidence(size_t(gw) * gh, 0.0f);
    float globalX = 0.0f, globalY = 0.0f;
    // A repeat re-evaluates against the same previous distinct frame; a new
    // frame's reference is whatever was evaluated last.
    const std::vector<float>& reference = repeat ? m_prevLuma : m_lastLuma;
    bool history = reset == HistoryReset::None && m_havePrev && reference.size() == cur.size();
    float globalCost = 0.0f;
    SceneCutStrength cutStrength = SceneCutStrength::None;
    bool cutSuppressed = false;
    float histogramOverlap = 1.0f;
    if (history) {
        EstimateFlow(cur, reference, gw, gh, fx, fy, confidence, globalX, globalY, globalCost);
        // Judge cuts on correspondence quality plus histogram overlap, so fast
        // camera pans are not mistaken for cuts and real cuts never keep history.
        histogramOverlap = LumaHistogramIntersection(cur, reference);
        cutStrength = ClassifySceneCut(globalCost, histogramOverlap);
        // The weak arm alone is not worth a history wipe this soon after the last one: a
        // burst of near-threshold frames inside one transition would otherwise reset DLSS
        // repeatedly, which is exactly the artifact S3.13 warns about. A Residual-strength
        // cut is never suppressed, matching x264/x265, where a decisive scenecut still
        // fires inside min-keyint.
        cutSuppressed = cutStrength == SceneCutStrength::Histogram && m_haveAcceptedCut &&
                        m_framesSinceCut < MinFramesBetweenCuts(targetFps);
        if (cutStrength != SceneCutStrength::None && !cutSuppressed) {
            history = false;
            reset = HistoryReset::Cut;
            std::fill(fx.begin(), fx.end(), 0.0f);
            std::fill(fy.begin(), fy.end(), 0.0f);
            globalX = globalY = 0.0f;
            m_prevDepth.clear();
            m_framesSinceCut = 0;
            m_haveAcceptedCut = true;
        } else {
            MedianFlow(fx, fy, confidence, gw, gh);
        }
    }
    if (reset != HistoryReset::None) ++m_historyGeneration;

    std::vector<float> depthGrid;
    if (m_controls.depth) BuildDepthProxy(cur, fx, fy, gw, gh, depthGrid);
    else depthGrid.assign(size_t(gw) * gh, 0.75f);
    const bool emitMotion = history && m_controls.motionVectors;

    // Keep CPU output compact. A D3D12 MRT pass bilinearly expands this grid to
    // full render-resolution R16G16 motion, and a depth pass writes B into the
    // NGX depth resource.
    out.gridW = gw;
    out.gridH = gh;
    out.guideGridRGBA32F.assign(size_t(gw) * gh * 4u, 0.0f);
    const float gridToRenderX = float(renderW) / float(gw);
    const float gridToRenderY = float(renderH) / float(gh);

    ParallelForRanges(size_t(gh), kGuideRowGrain, [&](size_t beginRow, size_t endRow) {
    for (uint32_t y = uint32_t(beginRow); y < uint32_t(endRow); ++y) {
        for (uint32_t x = 0; x < gw; ++x) {
            const size_t i = size_t(y) * gw + x;
            const size_t o = i * 4u;
            out.guideGridRGBA32F[o + 0] = emitMotion ? fx[i] * gridToRenderX : 0.0f;
            out.guideGridRGBA32F[o + 1] = emitMotion ? fy[i] * gridToRenderY : 0.0f;
            out.guideGridRGBA32F[o + 2] = depthGrid[i];
            // A stays 0: the RGBA32F layout is kept because a 96-bit RGB32F
            // texture has no guaranteed bilinear filtering, which this grid needs.
        }
    }
    });

    out.hasHistory = history;
    out.globalMotionX = globalX * gridToRenderX;
    out.globalMotionY = globalY * gridToRenderY;
    out.globalMatchCost = globalCost;
    out.sceneCut = cutStrength;
    out.sceneCutSuppressed = cutSuppressed;
    out.sceneCutResidual = globalCost;
    out.sceneCutHistogramOverlap = histogramOverlap;
    out.id = frame;
    out.id.historyGeneration = m_historyGeneration;
    out.id.reset = reset;
    if (!repeat) m_prevLuma = std::move(m_lastLuma);
    m_lastLuma = std::move(cur);
    m_havePrev = true;
    m_firstFrame = false;
    m_lastFrameNumber = frame.frameNumber;
    m_lastPts = frame.pts100ns;
    m_lastSourceGeneration = frame.sourceGeneration;
    return true;
}

