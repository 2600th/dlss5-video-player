#pragma once
#include <cstdint>
#include <vector>
#include <utility>
#include "FrameIdentity.h"
#include "GuideControls.h"
#include "PixelLayout.h"

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
    bool Generate(const uint8_t* bgra, uint32_t sourceW, uint32_t sourceH,
                  uint32_t renderW, uint32_t renderH, double targetFps,
                  const FrameIdentity& frame, GuideFrame& out,
                  SourcePixelLayout layout = SourcePixelLayout::Bgra);

    // Scene-cut decision from the post-alignment residual and the luma
    // histogram overlap of two analysis grids. Fast pans keep a high overlap
    // even when the residual is large; a cut loses both.
    static bool IsSceneCut(float globalMatchCost, float histogramIntersection);
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
    GuideControls m_controls;
};
