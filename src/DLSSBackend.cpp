#include "DLSSBackend.h"
#include "Log.h"
#include <windows.h>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <cmath>

DLSSBackend::~DLSSBackend() { Shutdown(); }

bool DLSSBackend::Initialize(ID3D12Device* device, ID3D12GraphicsCommandList*,
                             uint32_t sourceW, uint32_t sourceH,
                             uint32_t outputW, uint32_t outputH,
                             NVSDK_NGX_PerfQuality_Value quality) {
    m_device = device;
    m_outputW = outputW;
    m_outputH = outputH;
    m_quality = quality;

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path logDir = std::filesystem::path(exePath).parent_path() / L"ngx_logs";
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);

    // Custom engine/project identity is the officially supported NGX route for
    // non-engine samples. It is intentionally stable across runs.
    m_lastResult = NVSDK_NGX_D3D12_Init_with_ProjectID(
        "50f09991-2962-44db-bad7-4be06dbbd1d2",
        NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "DLSSVideoPlayer-10.0",
        logDir.c_str(), device, nullptr, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(m_lastResult)) {
        LOG("NGX Init failed result=0x" << std::hex << m_lastResult);
        return false;
    }
    m_initialized = true;

    m_lastResult = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_params);
    if (NVSDK_NGX_FAILED(m_lastResult) || !m_params) {
        LOG("NGX GetCapabilityParameters failed result=0x" << std::hex << m_lastResult);
        return false;
    }

    int srAvailable = 0;
    NVSDK_NGX_Result availResult = NVSDK_NGX_Parameter_GetI(
        m_params, NVSDK_NGX_Parameter_SuperSampling_Available, &srAvailable);
    if (!NVSDK_NGX_FAILED(availResult) && !srAvailable) {
        LOG("NGX reports SuperSampling.Available=0; native DLSS SR is unavailable on this system/runtime.");
        return false;
    }
    if (!NVSDK_NGX_FAILED(availResult)) {
        LOG("NGX capability: SuperSampling.Available=" << srAvailable);
    }

    float sharpness=0.0f;
    m_lastResult = NGX_DLSS_GET_OPTIMAL_SETTINGS(m_params, outputW, outputH, quality,
        &m_optimalW, &m_optimalH, &m_maxW, &m_maxH, &m_minW, &m_minH, &sharpness);
    if (NVSDK_NGX_FAILED(m_lastResult) || !m_optimalW || !m_optimalH) {
        LOG("NGX optimal settings failed result=0x" << std::hex << m_lastResult
            << "; using 2/3 output dimensions fallback.");
        m_optimalW = std::max(1u, outputW * 2u / 3u);
        m_optimalH = std::max(1u, outputH * 2u / 3u);
        m_minW = m_maxW = m_optimalW;
        m_minH = m_maxH = m_optimalH;
    }

    // Do not pre-upscale a low-resolution movie to the nominal DLSS quality input
    // before invoking DLSS.  Pick the source-equivalent size when the runtime's
    // dynamic-resolution range permits it, otherwise clamp to the advertised range.
    // This preserves the actual reconstruction job for DLSS instead of hiding it
    // behind a bilinear resize first.
    if (quality == NVSDK_NGX_PerfQuality_Value_DLAA) {
        m_renderW = outputW; m_renderH = outputH;
    } else {
        const double outAR = outputH ? double(outputW) / double(outputH) : 1.0;
        const double sourcePixels = std::max(1.0, double(sourceW) * double(sourceH));
        double nativeW = std::sqrt(sourcePixels * outAR);
        double candidateW = std::min(double(m_optimalW), nativeW);
        const double minScale = (m_minW && m_minH) ? std::max(double(m_minW) / outputW, double(m_minH) / outputH) : 0.0;
        const double maxScale = (m_maxW && m_maxH) ? std::min(double(m_maxW) / outputW, double(m_maxH) / outputH) : 1.0;
        double scale = candidateW / double(outputW);
        if (minScale > 0.0 && maxScale >= minScale) scale = std::clamp(scale, minScale, maxScale);
        m_renderW = std::max(2u, uint32_t(std::lround(double(outputW) * scale)) & ~1u);
        m_renderH = std::max(2u, uint32_t(std::lround(double(outputH) * scale)) & ~1u);
    }

    LOG("NGX input policy: source=" << sourceW << "x" << sourceH
        << " optimal=" << m_optimalW << "x" << m_optimalH
        << " range=" << m_minW << "x" << m_minH << ".." << m_maxW << "x" << m_maxH
        << " selected=" << m_renderW << "x" << m_renderH
        << " output=" << outputW << "x" << outputH);

    // Prefer the current transformer preset when the runtime supports it.
    // Unsupported hints are simply ignored by older NGX builds.
    NVSDK_NGX_Parameter_SetI(m_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                             NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_Parameter_SetI(m_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                             NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_Parameter_SetI(m_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                             NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_Parameter_SetI(m_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                             NVSDK_NGX_DLSS_Hint_Render_Preset_K);

    // Feature creation is deliberately deferred until the first rendered frame.
    // At that point D3D12/DXGI and ReShade add-ons are fully initialized, so a
    // generic RenoDX DLSS5 add-on can observe the *actual* CreateFeature call.
    m_available = true;
    LOG("NGX initialized. Deferred raw CreateFeature armed: input=" << m_renderW << "x" << m_renderH
        << " output=" << m_outputW << "x" << m_outputH);
    return true;
}

void DLSSBackend::FillCreateParameters() {
    // Match NVIDIA's official D3D12 helper contract even though we call the raw
    // CreateFeature symbol so ReShade/RenoDX can see it. Single-GPU apps use node 1.
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_CreationNodeMask, 1);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_Width, m_renderW);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_Height, m_renderH);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_OutWidth, m_outputW);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_OutHeight, m_outputH);
    NVSDK_NGX_Parameter_SetI(m_params, NVSDK_NGX_Parameter_PerfQualityValue, m_quality);

    const int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                      NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    // Depth is conventional HW depth: 0 = near, 1 = far, so no DepthInverted.
    // Motion vectors are generated without camera-jitter baked in, so no MVJittered.
    NVSDK_NGX_Parameter_SetI(m_params, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
    NVSDK_NGX_Parameter_SetI(m_params, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
}

bool DLSSBackend::CreateFeature(ID3D12GraphicsCommandList* cmd) {
    if (!m_params || !cmd) return false;
    if (m_handle) return true;

    FillCreateParameters();
    m_lastResult = NVSDK_NGX_D3D12_CreateFeature(
        cmd, NVSDK_NGX_Feature_SuperSampling, m_params, &m_handle);
    if (NVSDK_NGX_FAILED(m_lastResult) || !m_handle) {
        LOG("RAW NGX D3D12 CreateFeature failed result=0x" << std::hex << m_lastResult);
        m_handle = nullptr;
        return false;
    }

    LOG("RAW NGX D3D12 CreateFeature SUCCESS: feature=SuperSampling input="
        << std::dec << m_renderW << "x" << m_renderH << " output="
        << m_outputW << "x" << m_outputH
        << " flags=MVLowRes|AutoExposure; direct hook-visible contract");
    return true;
}

bool DLSSBackend::EnsureFeature(ID3D12GraphicsCommandList* cmd) {
    if (!Available() || !cmd) return false;
    return m_handle ? true : CreateFeature(cmd);
}

bool DLSSBackend::RecreateFeature(ID3D12GraphicsCommandList* cmd) {
    if (!Available() || !cmd) return false;
    if (m_handle) {
        NVSDK_NGX_D3D12_ReleaseFeature(m_handle);
        m_handle = nullptr;
    }
    m_evaluations = 0;
    LOG("Recreating RAW NGX DLSS feature for ReShade/RenoDX hook capture.");
    return CreateFeature(cmd);
}

void DLSSBackend::FillEvaluateParameters(ID3D12Resource* color,
                                         ID3D12Resource* output,
                                         ID3D12Resource* depth,
                                         ID3D12Resource* motion,
                                         ID3D12Resource* biasCurrentColor,
                                         bool reset,
                                         float frameTimeMs,
                                         float jitterX,
                                         float jitterY) {
    // Required DLSS-SR resources.
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_Color, color);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_Output, output);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_Depth, depth);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_MotionVectors, motion);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, biasCurrentColor);
    // The same conservative correspondence-failure mask is also valid as the newer
    // disocclusion/responsivity hints. Supplying all three names makes the temporal
    // contract visible to current NGX consumers without inventing extra image content.
#ifdef NVSDK_NGX_Parameter_DLSS_DisocclusionMask
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_DLSS_DisocclusionMask, biasCurrentColor);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_DLSS_ResponsivityMask, biasCurrentColor);
#endif

    // Required/meaningful temporal constants.
    NVSDK_NGX_Parameter_SetF(m_params, NVSDK_NGX_Parameter_Jitter_Offset_X, jitterX);
    NVSDK_NGX_Parameter_SetF(m_params, NVSDK_NGX_Parameter_Jitter_Offset_Y, jitterY);
    NVSDK_NGX_Parameter_SetF(m_params, NVSDK_NGX_Parameter_Sharpness, 0.0f);
    NVSDK_NGX_Parameter_SetI(m_params, NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);

    // Buffer stores current-pixel -> previous-frame location directly in input pixels.
    NVSDK_NGX_Parameter_SetF(m_params, NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    NVSDK_NGX_Parameter_SetF(m_params, NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);

    // Explicit extents/subrects make the contract unambiguous to both NGX and
    // generic add-ons which inspect the parameter block.
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, m_renderW);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, m_renderH);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, 0);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y, 0);
#ifdef NVSDK_NGX_Parameter_DLSS_DisocclusionMask
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_X, 0);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_Y, 0);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_ResponsivityMask_Subrect_Base_X, 0);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_ResponsivityMask_Subrect_Base_Y, 0);
#endif
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0);

    // SDR video is converted to linear FP16 before DLSS. AutoExposure is enabled,
    // so no external exposure texture is necessary; these values define the color scale.
    NVSDK_NGX_Parameter_SetF(m_params, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    NVSDK_NGX_Parameter_SetF(m_params, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    NVSDK_NGX_Parameter_SetF(m_params, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, frameTimeMs);
    NVSDK_NGX_Parameter_SetI(m_params, NVSDK_NGX_Parameter_DLSS_Indicator_Invert_X_Axis, 0);
    NVSDK_NGX_Parameter_SetI(m_params, NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis, 0);

    // Explicitly clear every optional research/engine guide that is not meaningful for
    // a decoded movie. This mirrors NVIDIA's helper behavior and prevents stale capability
    // parameter values from being interpreted as live resources by NGX or an interceptor.
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_TransparencyMask, nullptr);
#ifdef NVSDK_NGX_Parameter_DLSS_TransparencyLayer
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_DLSS_TransparencyLayer, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs, nullptr);
#endif
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_ExposureTexture, nullptr);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X, 0);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y, 0);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Albedo, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Roughness, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Metallic, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Specular, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Subsurface, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Normals, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_ShadingModelId, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_MaterialId, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Atrrib_8, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Atrrib_9, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Atrrib_10, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Atrrib_11, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Atrrib_12, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Atrrib_13, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Atrrib_14, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Atrrib_15, nullptr);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_TonemapperType, NVSDK_NGX_TONEMAPPER_STRING);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_MotionVectors3D, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_IsParticleMask, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_AnimatedTextureMask, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_DepthHighRes, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_Position_ViewSpace, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_RayTracingHitDistance, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_MotionVectorsReflection, nullptr);
}

bool DLSSBackend::Evaluate(ID3D12GraphicsCommandList* cmd,
                           ID3D12Resource* color,
                           ID3D12Resource* output,
                           ID3D12Resource* depth,
                           ID3D12Resource* motion,
                           ID3D12Resource* biasCurrentColor,
                           bool reset,
                           float frameTimeMs,
                           float jitterX,
                           float jitterY) {
    if (!Available() || !m_handle || !cmd || !color || !output || !depth || !motion || !biasCurrentColor) return false;

    FillEvaluateParameters(color, output, depth, motion, biasCurrentColor, reset, frameTimeMs, jitterX, jitterY);

    // NVIDIA's current nvsdk_ngx_helpers.h finishes the D3D12 DLSS helper with
    // NVSDK_NGX_D3D12_EvaluateFeature_C. The leaked/generic RenoDX NR add-on also
    // explicitly looks for this symbol in several games, so make _C the primary path.
    // If an unusual/older runtime rejects it, retry the legacy non-_C export once.
    m_lastResult = NVSDK_NGX_D3D12_EvaluateFeature_C(cmd, m_handle, m_params, nullptr);
    m_lastEvaluationUsedC = true;
    const char* evalPath = "EvaluateFeature_C";
    if (NVSDK_NGX_FAILED(m_lastResult)) {
        const NVSDK_NGX_Result cResult = m_lastResult;
        LOG("RAW NGX D3D12 EvaluateFeature_C failed result=0x" << std::hex << cResult
            << "; trying legacy EvaluateFeature fallback.");
        m_lastResult = NVSDK_NGX_D3D12_EvaluateFeature(cmd, m_handle, m_params, nullptr);
        m_lastEvaluationUsedC = false;
        evalPath = "EvaluateFeature";
    }
    if (NVSDK_NGX_FAILED(m_lastResult)) {
        LOG("RAW NGX D3D12 " << evalPath << " failed result=0x" << std::hex << m_lastResult);
        return false;
    }

    ++m_evaluations;
    if (m_evaluations == 1 || (m_evaluations % 300) == 0) {
        LOG("RAW NGX " << evalPath << " SUCCESS #" << std::dec << m_evaluations
            << " color=" << m_renderW << "x" << m_renderH
            << " depth=" << m_renderW << "x" << m_renderH
            << " mv=" << m_renderW << "x" << m_renderH
            << " bias=R8 " << m_renderW << "x" << m_renderH
            << " output=" << m_outputW << "x" << m_outputH
            << " jitter=(" << jitterX << "," << jitterY << ") reset=" << (reset?1:0));
    }
    return true;
}

void DLSSBackend::Shutdown() {
    if (m_handle) {
        NVSDK_NGX_D3D12_ReleaseFeature(m_handle);
        m_handle = nullptr;
    }
    if (m_params) {
        NVSDK_NGX_D3D12_DestroyParameters(m_params);
        m_params = nullptr;
    }
    if (m_initialized) {
        NVSDK_NGX_D3D12_Shutdown1(m_device);
        m_initialized = false;
    }
    m_device = nullptr;
    m_available = false;
    m_evaluations = 0;
}
