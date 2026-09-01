#pragma once
#include <cstdint>
#include <vector>
#include <utility>

struct GuideFrame {
    // Compact analysis grid consumed by a GPU expansion pass:
    // R = motion X, G = motion Y (current -> previous, already in DLSS input pixels)
    // B = depth proxy [0,1], A = BiasCurrentColor/disocclusion mask [0,1].
    std::vector<float> guideGridRGBA32F;
    uint32_t gridW = 0;
    uint32_t gridH = 0;
    bool hasHistory = false;
    float globalMotionX = 0.0f;
    float globalMotionY = 0.0f;
    float globalMatchCost = 0.0f;
};

class TemporalGuideGenerator {
public:
    enum class DepthMode { Flat, Estimated };

    static std::pair<uint32_t,uint32_t> AnalysisGrid(uint32_t sourceW, uint32_t sourceH, double targetFps = 30.0);

    void Reset();
    void SetDepthMode(DepthMode mode) { m_depthMode = mode; }
    DepthMode GetDepthMode() const { return m_depthMode; }

    bool Generate(const uint8_t* bgra, uint32_t sourceW, uint32_t sourceH,
                  uint32_t renderW, uint32_t renderH, double targetFps, bool reset,
                  GuideFrame& out);

private:
    static float Luma(const uint8_t* p);
    void DownsampleLuma(const uint8_t* bgra, uint32_t w, uint32_t h,
                        uint32_t gw, uint32_t gh, std::vector<float>& out) const;
    void EstimateFlow(const std::vector<float>& cur, const std::vector<float>& prev,
                      uint32_t gw, uint32_t gh,
                      std::vector<float>& flowX, std::vector<float>& flowY,
                      std::vector<float>& mismatch,
                      float& globalX, float& globalY, float& globalCost) const;
    void MedianFlow(std::vector<float>& x, std::vector<float>& y,
                    uint32_t gw, uint32_t gh) const;
    void BuildDepthProxy(const std::vector<float>& luma,
                         const std::vector<float>& flowX, const std::vector<float>& flowY,
                         uint32_t gw, uint32_t gh,
                         std::vector<float>& depth);

    std::vector<float> m_prevLuma;
    std::vector<float> m_prevDepth;
    uint32_t m_gridW = 0, m_gridH = 0;
    bool m_havePrev = false;
    DepthMode m_depthMode = DepthMode::Estimated;
};
