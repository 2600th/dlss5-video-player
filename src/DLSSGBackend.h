#pragma once
#include <d3d12.h>
#include <dxgiformat.h>
#include <cstdint>
#include <string>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_dlssg.h>
#include "NgxSession.h"

// What the runtime answered when asked for DLSS Frame Generation on this
// machine. Every field is measured: nothing here is inferred from the GPU
// name, the driver version or the presence of nvngx_dlssg.dll.
struct DLSSGCapability {
    bool available = false;          // feature created and released cleanly
    uint32_t multiFrameCountMax = 0; // generated frames per source pair the runtime admits (1 => 2x)
    bool hagsEnabled = false;        // HwSchMode == 2
    NVSDK_NGX_Result createResult = NVSDK_NGX_Result_Fail;
    std::wstring detail;             // human-readable reason when !available
};

// Direct NGX DLSS-G (Frame Generation) admission probe.
//
// Frame Generation is normally reached through Streamline, whose sl.dlss_g.dll
// owns the swapchain and drives presentation and pacing. This player holds its
// own swapchain and calls NGX directly, so whether the raw
// NVSDK_NGX_D3D12_CreateFeature path admits NVSDK_NGX_Feature_FrameGeneration
// at all is an open question that only hardware can answer. This class asks it
// and nothing else: it creates the feature, records the NVSDK_NGX_Result, and
// releases it again. There is deliberately no evaluate path here - a feature
// the runtime refuses to create cannot be evaluated, and a feature it admits
// still has to be shown to produce frames, which is a separate measurement.
//
// What it measured on 2026-09-17 (RTX 5090, driver 616.64, Windows 11 26200,
// no Streamline module loaded): the create returns NVSDK_NGX_Result_Success
// at 1920x1080 B8G8R8A8_UNORM and DLSSG.MultiFrameCountMax reads 5 - but only
// with nvngx_dlssg.dll resolvable beside the executable. Without it the same
// create answers 0xbad0000b and the capability block reports
// FrameGeneration.Available=0 with FeatureInitResult=0xbad00004, even though
// NGX locates and logs the driver-store fallback snippet. So raw-NGX DLSS-G
// admission is real on this machine and it is conditional on shipping the
// snippet, exactly as DLSS-SR ships nvngx_dlss.dll.
class DLSSGBackend {
public:
    ~DLSSGBackend();

    // Creates the FrameGeneration feature at the given backbuffer geometry and
    // releases it again. Answers only whether the runtime admits the feature.
    DLSSGCapability Probe(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                          uint32_t width, uint32_t height, DXGI_FORMAT backbufferFormat);
    void Shutdown();

    // The last probe's verdict, false until one succeeds and false again after
    // Shutdown: no caller may read admission out of a probe that did not run.
    bool Available() const { return m_available; }
    // True once NGX itself came up on the probe device and handed over a
    // parameter block, so a caller can tell a runtime refusal - which is a
    // verdict - from NGX never initializing, which is not one.
    bool SessionEstablished() const { return m_sessionLeaseAcquired && m_params != nullptr; }
    NVSDK_NGX_Result LastResult() const { return m_lastResult; }

private:
    bool AcquireSession(ID3D12Device* device);

    ID3D12Device* m_device = nullptr;
    const void* m_sessionKey = nullptr;
    NVSDK_NGX_Parameter* m_params = nullptr;
    NVSDK_NGX_Handle* m_handle = nullptr;
    NVSDK_NGX_Result m_lastResult = NVSDK_NGX_Result_Fail;
    bool m_sessionLeaseAcquired = false;
    bool m_available = false;
};
