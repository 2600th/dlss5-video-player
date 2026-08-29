#pragma once
#include <d3d12.h>
#include <cstdint>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

// Direct NGX DLSS-SR host.
// The player deliberately uses the raw NVSDK_NGX_D3D12_CreateFeature / EvaluateFeature_C
// entry points after explicitly populating the parameter block. This makes the
// full DLSS contract visible to ReShade/RenoDX hooks instead of hiding it behind
// only the inline helper wrappers.
class DLSSBackend {
public:
    ~DLSSBackend();

    bool Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                    uint32_t sourceW, uint32_t sourceH,
                    uint32_t outputW, uint32_t outputH,
                    NVSDK_NGX_PerfQuality_Value quality);
    bool EnsureFeature(ID3D12GraphicsCommandList* cmd);
    bool RecreateFeature(ID3D12GraphicsCommandList* cmd);
    bool FeatureCreated() const { return m_handle != nullptr; }
    uint64_t EvaluationCount() const { return m_evaluations; }
    bool Evaluate(ID3D12GraphicsCommandList* cmd,
                  ID3D12Resource* color,
                  ID3D12Resource* output,
                  ID3D12Resource* depth,
                  ID3D12Resource* motion,
                  ID3D12Resource* biasCurrentColor,
                  bool reset,
                  float frameTimeMs,
                  float jitterX,
                  float jitterY);
    void Shutdown();

    bool Available() const { return m_available && m_initialized && m_params != nullptr; }
    uint32_t RenderWidth() const { return m_renderW; }
    uint32_t RenderHeight() const { return m_renderH; }
    uint32_t OutputWidth() const { return m_outputW; }
    uint32_t OutputHeight() const { return m_outputH; }
    NVSDK_NGX_Result LastResult() const { return m_lastResult; }
    bool LastEvaluationUsedC() const { return m_lastEvaluationUsedC; }

private:
    bool CreateFeature(ID3D12GraphicsCommandList* cmd);
    void FillCreateParameters();
    void FillEvaluateParameters(ID3D12Resource* color,
                                ID3D12Resource* output,
                                ID3D12Resource* depth,
                                ID3D12Resource* motion,
                                ID3D12Resource* biasCurrentColor,
                                bool reset,
                                float frameTimeMs,
                                float jitterX,
                                float jitterY);

    ID3D12Device* m_device = nullptr;
    NVSDK_NGX_Parameter* m_params = nullptr;
    NVSDK_NGX_Handle* m_handle = nullptr;
    uint32_t m_renderW = 0, m_renderH = 0;
    uint32_t m_optimalW = 0, m_optimalH = 0;
    uint32_t m_minW = 0, m_minH = 0, m_maxW = 0, m_maxH = 0;
    uint32_t m_outputW = 0, m_outputH = 0;
    NVSDK_NGX_PerfQuality_Value m_quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    NVSDK_NGX_Result m_lastResult = NVSDK_NGX_Result_Fail;
    bool m_initialized = false;
    bool m_available = false;
    uint64_t m_evaluations = 0;
    bool m_lastEvaluationUsedC = true;
};
