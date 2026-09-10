#pragma once
#include <cstdint>
#include <vector>
#include <utility>
#include "FrameIdentity.h"
#include "GuideControls.h"
#include "PixelLayout.h"

// Strength of the image evidence behind a scene-cut decision. The two arms are
// kept apart because only the weak one is debounced: NVIDIA's DLSS Programming
// Guide 310.6.0 S3.13 asks for InReset on the first frame after a major
// transition and warns that improper use "can result in temporal flickering,
// heavy aliasing or other visual artifacts", so a false positive is far more
// expensive than a late true positive - but a strong signal must still cut
// immediately, which is also what x264/x265 do (their distance-ramped scenecut
// threshold never blocks a decisive cut).
enum class SceneCutStrength {
    None,       // the frames correspond; history continues
    Histogram,  // weak arm: a moderate residual plus a collapsed luma histogram
    Residual,   // strong arm: correspondence failed outright
};

struct GuideFrame {
    // Compact analysis grid consumed by a GPU expansion pass:
    // R = motion X, G = motion Y (current -> previous, already in DLSS input pixels)
    // B = depth proxy [0,1], A unused (kept only so the texture stays RGBA32F,
    // which is the widest 32-bit float format with guaranteed bilinear filtering).
    std::vector<float> guideGridRGBA32F;
    uint32_t gridW = 0;
    uint32_t gridH = 0;
    bool hasHistory = false;
    float globalMotionX = 0.0f;
    float globalMotionY = 0.0f;
    float globalMatchCost = 0.0f;
    // What the scene-cut classifier saw for this frame, so a live-playback
    // decision is inspectable instead of silent. `sceneCutSuppressed` is true
    // when a weak cut was recognised but withheld by the minimum-interval
    // debounce, in which case the history below continues. With no previous
    // frame to compare against nothing is measured and the pair is reported as
    // the neutral (residual 0, overlap 1) "no evidence" reading.
    SceneCutStrength sceneCut = SceneCutStrength::None;
    bool sceneCutSuppressed = false;
    float sceneCutResidual = 0.0f;
    float sceneCutHistogramOverlap = 1.0f;
    // Source identity of the frame these guides were built from, stamped with
    // the generator's history generation and the reset reason (None when the
    // guides continue the previous frame's history; hasHistory == !reset).
    FrameIdentity id;
};

// Pixel layout of the buffer handed to Generate/DownsampleLuma (see PixelLayout.h). Bgra
// is the live-playback path; Nv12 is the export decoder's frame shape, of which only the
// Y plane is read here.
using SourcePixelLayout = PixelLayout;

class TemporalGuideGenerator {
public:
    static std::pair<uint32_t,uint32_t> AnalysisGrid(uint32_t sourceW, uint32_t sourceH, double targetFps = 30.0);

    // Discards image history; the next frame is stamped HistoryReset::FirstFrame.
    void Reset();
    void SetControls(const GuideControls& controls);
    const GuideControls& Controls() const { return m_controls; }
    // Incremented every time this generator declares a reset.
    uint32_t HistoryGeneration() const { return m_historyGeneration; }

    // Builds guides for `frame`. History is reset when the caller declares a
    // reset (frame.reset != None), the source generation changes, the frame
    // number is not the successor of the previous one, the analysis grid
    // changes, or the image evidence indicates a cut. Re-submitting the frame
    // that was evaluated last (same frameNumber/pts/sourceGeneration) is a
    // re-evaluation: guides are rebuilt against the same previous frame and no
    // reset is declared.
    // A cut whose evidence is only Histogram-strength is withheld when fewer
    // than MinFramesBetweenCuts(targetFps) frames have passed since the last
    // accepted cut; `out.sceneCut*` reports every decision either way.
    bool Generate(const uint8_t* bgra, uint32_t sourceW, uint32_t sourceH,
                  uint32_t renderW, uint32_t renderH, double targetFps,
                  const FrameIdentity& frame, GuideFrame& out,
                  SourcePixelLayout layout = SourcePixelLayout::Bgra);

    // Scene-cut decision from the post-alignment residual and the luma
    // histogram overlap of two analysis grids. Fast pans keep a high overlap
    // even when the residual is large; a cut loses both.
    static bool IsSceneCut(float globalMatchCost, float histogramIntersection);
    // Same decision, split by how strong the evidence is. IsSceneCut() is
    // exactly `ClassifySceneCut(...) != None`; Generate() needs the split
    // because only the weak arm is debounced.
    static SceneCutStrength ClassifySceneCut(double residual, double histogramOverlap);
    // Minimum number of frames Generate() requires between two weak cuts at
    // `fps`. 15 frames at 25 fps, 18 at 30, and never fewer than 2.
    static uint32_t MinFramesBetweenCuts(double fps);
    static float LumaHistogramIntersection(const std::vector<float>& a, const std::vector<float>& b);

private:
    static float Luma(const uint8_t* p);
    static float LumaFromNv12Y(uint8_t y);
    void DownsampleLuma(const uint8_t* bgra, uint32_t w, uint32_t h,
                        uint32_t gw, uint32_t gh, std::vector<float>& out,
                        SourcePixelLayout layout = SourcePixelLayout::Bgra) const;
    // Confidence is per grid cell in [0,1]: 0 means the cell was rejected and carries no
    // motion, higher values mean the winning displacement both beat standing still by a
    // margin and was a distinct minimum of the SAD landscape.
    void EstimateFlow(const std::vector<float>& cur, const std::vector<float>& prev,
                      uint32_t gw, uint32_t gh,
                      std::vector<float>& flowX, std::vector<float>& flowY,
                      std::vector<float>& confidence,
                      float& globalX, float& globalY, float& globalCost) const;
    void MedianFlow(std::vector<float>& x, std::vector<float>& y,
                    const std::vector<float>& confidence,
                    uint32_t gw, uint32_t gh) const;
    void BuildDepthProxy(const std::vector<float>& luma,
                         const std::vector<float>& flowX, const std::vector<float>& flowY,
                         uint32_t gw, uint32_t gh,
                         std::vector<float>& depth);
    HistoryReset ClassifyReset(const FrameIdentity& frame, uint32_t gw, uint32_t gh) const;
    bool IsRepeat(const FrameIdentity& frame) const;

    std::vector<float> m_prevLuma;   // previous distinct frame (flow reference)
    std::vector<float> m_lastLuma;   // most recently evaluated frame
    std::vector<float> m_prevDepth;
    uint32_t m_gridW = 0, m_gridH = 0;
    bool m_havePrev = false;
    bool m_firstFrame = true;
    uint64_t m_lastFrameNumber = 0;
    int64_t m_lastPts = 0;
    uint32_t m_lastSourceGeneration = 0;
    uint32_t m_historyGeneration = 0;
    // Weak-arm debounce state. m_framesSinceCut counts distinct frames since the
    // last cut this generator accepted from image evidence and saturates rather
    // than wrapping; m_haveAcceptedCut is false until there is such a cut, and a
    // declared reset clears it again because the history a suppression would
    // have protected is already gone.
    uint32_t m_framesSinceCut = 0;
    bool m_haveAcceptedCut = false;
    GuideControls m_controls;
};
