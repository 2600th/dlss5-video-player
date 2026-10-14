#pragma once
#include "FrameGenBackend.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

// Vendor-neutral Frame Generation backend: no NGX, no nvngx_dlssg.dll, no NVIDIA
// optical flow. It estimates motion between the previous and current backbuffer
// with a D3D12 compute optical-flow pass (coarse block matching, one thread per
// 8x8 block) and then warps+blends the two frames to synthesize the in-between
// frame at the requested time fraction. The pass's motion/depth guides are
// ignored because this backend derives its own motion, exactly as DLSS-G was
// measured to ignore the tagged motion vectors (see FrameGenerationPass.h).
//
// Block size and search radius are fixed compile-time constants shared with the
// shaders; raising BLOCK buys speed at coarser motion, raising SEARCH widens the
// motion the matcher can lock onto.
class AmdFrameGenBackend final : public IFrameGenBackend {
public:
    AmdFrameGenBackend() = default;
    ~AmdFrameGenBackend() override;

    AmdFrameGenBackend(const AmdFrameGenBackend&) = delete;
    AmdFrameGenBackend& operator=(const AmdFrameGenBackend&) = delete;

    const char* Kind() const override { return "amd-optical-flow"; }

    bool Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, uint32_t width,
                    uint32_t height, DXGI_FORMAT backbufferFormat) override;

    bool Evaluate(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backbuffer,
                  ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* outputInterpolated,
                  uint32_t multiFrameCount, uint32_t multiFrameIndex, bool reset,
                  uint64_t backbufferFrameId = 0, float frameTimeMs = 0.0f) override;

    uint64_t EvaluationCount() const override { return m_evaluations; }
    // Conservative until the matcher is measured on real footage: 1 generated
    // frame per pair => 2x. Higher multipliers feed evenly spaced time fractions
    // through the same single-pass flow, so they are safe to raise later.
    uint32_t MultiFrameCountMax() const override { return m_multiFrameCountMax; }

    uint32_t LastErrorCode() const override { return m_lastError; }
    std::wstring LastDetail() const override { return m_lastDetail; }

    bool Available() const override { return m_initialized; }
    bool FeatureCreated() const override { return m_initialized; }
    bool SessionEstablished() const override { return m_initialized; }

    void Shutdown() override;
    void Abandon() override;

private:
    bool CompileShaders();
    bool CreatePipelines();
    void WriteSrv(uint32_t slot, ID3D12Resource* res, DXGI_FORMAT format);
    void WriteUav(uint32_t slot, ID3D12Resource* res, DXGI_FORMAT format);
    void Fail(uint32_t code, std::wstring detail);

    static constexpr uint32_t kBlock = 8;    // optical-flow block / blend thread tile
    // Block-match radius in pixels: the largest per-frame (adjacent decoded
    // frame) displacement the matcher can lock onto. The interpolation math is
    // correct for any mv it finds, so this is purely a motion-coverage knob -
    // and an expensive one, because candidates go as its square: 4225 offsets
    // per block at 32 against 289 at 8. Halving it to 16 is a 4x saving for
    // content that does not move more than 16 px between frames. It is a
    // single-scale matcher - a future hierarchical pass would widen the
    // coverage without paying the square - and is the number to lower when the
    // conversion is too slow, or raise when generated frames smear on fast
    // action.
    //
    // Measured on an RX 9070 XT, 1920x1080, 744 frames, Release: 15.3 s at
    // kSearch 32 (39 fps of source). Before the search was spread across a
    // thread group and given an early-out it was 79 s for the same file.
    static constexpr uint32_t kSearch = 32;

    ID3D12Device* m_device = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    uint32_t m_multiFrameCountMax = 1;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_flowPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_blendPso;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    uint32_t m_heapInc = 0;

    // Previous backbuffer, kept as history between Evaluate calls.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_prevFrame;
    // Per-block forward flow (curr - prev displacement, in pixels).
    Microsoft::WRL::ComPtr<ID3D12Resource> m_flow;
    // m_flow's resource state as this backend last left it, so the first flow
    // pass after Initialize (which creates it UNORDERED_ACCESS) and every later
    // one (left NON_PIXEL_SHADER_RESOURCE by the blend read) both start from a
    // correct before-state instead of assuming one.
    D3D12_RESOURCE_STATES m_flowState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    uint64_t m_evaluations = 0;
    uint32_t m_lastError = 0;
    std::wstring m_lastDetail;
    bool m_initialized = false;
};
