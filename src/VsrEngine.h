#pragma once
#include <d3d12.h>
#include <cstdint>
#include <nvsdk_ngx.h>
#include "VsrPolicy.h"

// RTX Video Super Resolution (NGX feature 16, nvngx_vsr.dll) on the renderer's
// D3D12 device, for the player's RTX VSR comparison view (P2.8, VsrPolicy.h).
//
// It needs no NGX core of its own. The spike for P2.8 created feature 16 through
// the DLSS SDK's nvsdk_ngx_d.lib - the one core this project already links - with
// nvngx_vsr.dll 1.6.0 beside the executable, in the same session and on the same
// command list as a DLSS Super Resolution feature, both evaluating without error
// (RTX 4080 SUPER, driver 610.47; the p28 report has the log). So the engine joins
// the session DLSSBackend opened on the device (ngx_session_detail::Registry) and
// never opens one: the project identity, the log directory and the logging hook
// that session was started with stay DLSSBackend's, and a device NGX never started
// on simply has no VSR, which is the truth on anything that is not an RTX GPU.
//
// Built without -DRTX_VIDEO_SDK the class is still here, with nothing behind it:
// Probe reports NotBuilt and nothing else does anything. With it,
// DLSS_VIDEO_PLAYER_HAS_RTX_VSR compiles the body in VsrEngine.cpp against the
// SDK's feature header, which is read at build time and never committed.
class VsrEngine {
public:
#ifdef DLSS_VIDEO_PLAYER_HAS_RTX_VSR
    static constexpr bool kBuilt = true;
#else
    static constexpr bool kBuilt = false;
#endif
    // Before Probe, a built engine has no session yet, which is what it reports.
    VsrEngine() { m_caps.built = kBuilt; }
    ~VsrEngine();
    VsrEngine(const VsrEngine&) = delete;
    VsrEngine& operator=(const VsrEngine&) = delete;

    // Joins the device's NGX session and reads the feature's capability
    // parameters. Cheap - no feature is created - so a renderer can say at once
    // whether the view is possible. Calling it again re-reads nothing.
    void Probe(ID3D12Device* device);
    // Creates the feature on an open command list. The caller submits and waits
    // for that list before the first Evaluate, as it does for DLSS's.
    bool CreateFeature(ID3D12GraphicsCommandList* cmd);
    bool FeatureCreated() const { return m_handle != nullptr; }
    // `input` is shader-readable (NON_PIXEL_SHADER_RESOURCE), 8-bit SDR RGB;
    // `output` an 8-bit UAV texture in UNORDERED_ACCESS. Both whole, at the sizes
    // given. NGX leaves its own descriptor heaps bound on `cmd`.
    bool Evaluate(ID3D12GraphicsCommandList* cmd, ID3D12Resource* input, uint32_t inputW, uint32_t inputH,
                  ID3D12Resource* output, uint32_t outputW, uint32_t outputH, vsr_policy::Quality quality);
    // Releases the feature and the session lease. Called by the renderer ahead of
    // DLSSBackend's own shutdown, after its queue has drained.
    void Shutdown();

    vsr_policy::Reason Reason() const { return vsr_policy::Decide(m_caps); }
    const vsr_policy::Capabilities& Capabilities() const { return m_caps; }
    // The last NGX result, for the log and the create-failed reason.
    NVSDK_NGX_Result LastResult() const { return m_lastResult; }
    uint64_t EvaluationCount() const { return m_evaluations; }

private:
    vsr_policy::Capabilities m_caps{};
    ID3D12Device* m_device = nullptr;
    const void* m_sessionKey = nullptr;
    bool m_probed = false;
    NVSDK_NGX_Parameter* m_params = nullptr;
    NVSDK_NGX_Handle* m_handle = nullptr;
    NVSDK_NGX_Result m_lastResult = NVSDK_NGX_Result_Fail;
    uint64_t m_evaluations = 0;
};
