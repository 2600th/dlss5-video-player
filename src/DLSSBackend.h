#pragma once
#include <d3d12.h>
#include <cstdint>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include "NgxSession.h"

constexpr NVSDK_NGX_PerfQuality_Value DefaultNeuralCarrierQuality() noexcept
{
    // DLAA preserves the hook-visible NGX evaluation contract at 1:1 resolution,
    // so neural rendering can run without enabling spatial DLSS upscaling.
    return NVSDK_NGX_PerfQuality_Value_DLAA;
}

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
                    NVSDK_NGX_PerfQuality_Value quality, bool preserveSource = false);
    bool EnsureFeature(ID3D12GraphicsCommandList* cmd);
    bool RecreateFeature(ID3D12GraphicsCommandList* cmd);
    // Hands the feature back after asking the runtime to free its memory with
    // it (NVSDK_NGX_Parameter_FreeMemOnReleaseFeature). For an idle helper
    // only: everything else this backend holds - the NGX session, the
    // parameter block, the negotiated sizes - stays, so the next
    // EnsureFeature re-creates the feature without re-initializing NGX.
    // False when there was no feature to hand back. Whether the runtime
    // honours the request is not observable here; the caller measures the
    // adapter on both sides of the call instead.
    bool ReleaseFeatureFreeingMemory();
    bool FeatureCreated() const { return m_handle != nullptr; }
    uint64_t EvaluationCount() const { return m_evaluations; }
    bool Evaluate(ID3D12GraphicsCommandList* cmd,
                  ID3D12Resource* color,
                  ID3D12Resource* output,
                  ID3D12Resource* depth,
                  ID3D12Resource* motion,
                  bool reset,
                  float frameTimeMs);
    void Shutdown();
    // A 1x1 exposure the evaluate reads instead of metering its own (ExposurePolicy.h).
    // Set before the feature is created: with one, the feature is created without
    // AutoExposure and every evaluate binds it; without one - the default - nothing
    // changes. The resource must be in NON_PIXEL_SHADER_RESOURCE at evaluate time.
    void SetExposureTexture(ID3D12Resource* exposure) { m_exposure = exposure; }

    bool Available() const { return m_available && m_initialized && m_params != nullptr; }
    uint32_t RenderWidth() const { return m_renderW; }
    uint32_t RenderHeight() const { return m_renderH; }
    // The output the runtime accepted, which may be smaller than the one asked
    // for when the source could not reach it.
    uint32_t OutputWidth() const { return m_outputW; }
    uint32_t OutputHeight() const { return m_outputH; }
    // Set when no output this source can reach was found, so the caller can say
    // that rather than reporting DLSS as simply unavailable.
    bool SourceOutsideSupportedRange() const { return m_sourceOutsideRange; }
    NVSDK_NGX_Result LastResult() const { return m_lastResult; }

private:
    bool CreateFeature(ID3D12GraphicsCommandList* cmd);
    void FillCreateParameters();
    void FillEvaluateParameters(ID3D12Resource* color,
                                ID3D12Resource* output,
                                ID3D12Resource* depth,
                                ID3D12Resource* motion,
                                bool reset,
                                float frameTimeMs);

    ID3D12Device* m_device = nullptr;
    const void* m_sessionKey = nullptr;
    NVSDK_NGX_Parameter* m_params = nullptr;
    NVSDK_NGX_Handle* m_handle = nullptr;
    uint32_t m_renderW = 0, m_renderH = 0;
    uint32_t m_optimalW = 0, m_optimalH = 0;
    uint32_t m_minW = 0, m_minH = 0, m_maxW = 0, m_maxH = 0;
    uint32_t m_outputW = 0, m_outputH = 0;
    NVSDK_NGX_PerfQuality_Value m_quality = DefaultNeuralCarrierQuality();
    NVSDK_NGX_Result m_lastResult = NVSDK_NGX_Result_Fail;
    bool m_initialized = false;
    bool m_sessionLeaseAcquired = false;
    bool m_available = false;
    bool m_sourceOutsideRange = false;
    ngx_session_detail::FeatureCreateGate m_featureCreateGate;
    uint64_t m_evaluations = 0;
    ID3D12Resource* m_exposure = nullptr;  // owned by the renderer
};
