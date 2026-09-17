#include "DLSSGBackend.h"
#include "Log.h"
#include "NeuralPreflight.h"
#include <windows.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#pragma comment(lib, "advapi32.lib")

namespace {

// HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\HwSchMode is what the
// Windows "Hardware-accelerated GPU scheduling" switch writes: 2 with it on, 1
// with it off, and nothing at all where the switch was never touched - which
// is this machine (Windows 11 26200, driver 616.64), where the value does not
// exist and the runtime admitted Frame Generation regardless. So the value is
// measured and reported, an absent one is reported as absent rather than as
// off, and neither is ever gated on: the runtime is the only thing entitled
// to refuse, and a refusal HAGS would explain is a different finding from one
// it would not.
std::optional<uint32_t> HardwareScheduledGpuMode()
{
    DWORD mode = 0;
    DWORD size = sizeof(mode);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                     L"HwSchMode", RRF_RT_REG_DWORD, nullptr, &mode, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return mode;
}

// The measured HAGS state as one clause of a capability detail.
std::wstring HardwareSchedulingSentence(std::optional<uint32_t> mode)
{
    if (!mode) {
        return L"the HwSchMode value is absent from HKLM\\SYSTEM\\CurrentControlSet\\Control\\"
               L"GraphicsDrivers, so hardware-accelerated GPU scheduling is unconfirmed";
    }
    return L"HwSchMode=" + std::to_wstring(*mode) + L" so hardware-accelerated GPU scheduling is off";
}

void Append(std::wstring& detail, const std::wstring& sentence)
{
    if (!detail.empty()) detail += L"; ";
    detail += sentence;
}

} // namespace

DLSSGBackend::~DLSSGBackend() { Shutdown(); }

bool DLSSGBackend::AcquireSession(ID3D12Device* device)
{
    if (m_sessionLeaseAcquired && m_device == device && m_params) return true;
    if (m_device != device) Shutdown();
    m_device = device;

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path logDir = std::filesystem::path(exePath).parent_path() / L"ngx_logs";
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);

    // IUnknown identity, as the SR backend does it, so the lease below keys on
    // the same value for the same device no matter which D3D12 interface
    // pointer each backend was handed.
    IUnknown* deviceIdentity = nullptr;
    if (FAILED(device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&deviceIdentity)))) {
        m_lastResult = NVSDK_NGX_Result_Fail;
        LOG("NGX device identity lookup failed for the DLSS-G probe.");
        m_device = nullptr;
        return false;
    }
    m_sessionKey = deviceIdentity;
    deviceIdentity->Release();

    // The SR backend's callback copies only lines that name a problem, because
    // its ~170 lines of startup chatter drown the player's own log every
    // session. This one copies everything: the raw-NGX Frame Generation path is
    // undocumented, so when it refuses, the surrounding lines are the only
    // statement of why, and reading that statement is the entire purpose of
    // this class.
    static NVSDK_NGX_FeatureCommonInfo commonInfo{};
    commonInfo.LoggingInfo.LoggingCallback = [](const char* message, NVSDK_NGX_Logging_Level,
                                                NVSDK_NGX_Feature component) {
        std::string_view text = message ? message : "";
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
        if (text.empty()) return;
        LOG("[NGX feature " << int(component) << "] " << text);
    };
    commonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;

    // Same project identity the SR backend registers, so the driver sees one
    // application asking for both features rather than two. The lease means
    // whichever backend reaches a device second reuses the first one's init
    // instead of initializing NGX twice on it.
    const bool sessionAcquired = ngx_session_detail::ProcessRegistry().Acquire(
        m_sessionKey,
        [&] {
            m_lastResult = NVSDK_NGX_D3D12_Init_with_ProjectID(
                "50f09991-2962-44db-bad7-4be06dbbd1d2",
                NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                "DLSSVideoPlayer-10.0",
                logDir.c_str(), device, &commonInfo, NVSDK_NGX_Version_API);
            return !NVSDK_NGX_FAILED(m_lastResult);
        });
    if (!sessionAcquired) {
        LOG("NGX Init failed for the DLSS-G probe result=0x" << std::hex << m_lastResult);
        m_sessionKey = nullptr;
        m_device = nullptr;
        return false;
    }
    m_sessionLeaseAcquired = true;

    m_lastResult = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_params);
    if (NVSDK_NGX_FAILED(m_lastResult) || !m_params) {
        LOG("NGX GetCapabilityParameters failed for the DLSS-G probe result=0x" << std::hex << m_lastResult);
        m_params = nullptr;
        return false;
    }
    return true;
}

DLSSGCapability DLSSGBackend::Probe(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                                    uint32_t width, uint32_t height, DXGI_FORMAT backbufferFormat)
{
    DLSSGCapability capability;
    m_available = false;

    const std::optional<uint32_t> hwSchMode = HardwareScheduledGpuMode();
    capability.hagsEnabled = hwSchMode == 2u;

    if (!device || !cmd || !width || !height) {
        m_lastResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
        capability.createResult = m_lastResult;
        capability.detail = L"The probe needs a device, a command list open for recording and a non-empty "
                            L"backbuffer geometry.";
        return capability;
    }
    if (!AcquireSession(device)) {
        capability.createResult = m_lastResult;
        capability.detail = L"NGX D3D12 initialization refused: " + HexResultTextWide(uint32_t(m_lastResult));
        return capability;
    }

    int advertised = 0;
    const NVSDK_NGX_Result advertisedRead =
        NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_FrameGeneration_Available, &advertised);
    int featureInitResult = 0;
    NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &featureInitResult);
    int needsUpdatedDriver = 0;
    NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needsUpdatedDriver);
    int minDriverMajor = 0, minDriverMinor = 0;
    NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &minDriverMajor);
    NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &minDriverMinor);

    // DLSSG.MultiFrameCountMax is the runtime's own count and the only key in
    // the vendored headers that carries it: nvsdk_ngx_defs_dlssg.h publishes it
    // as an output capability read off the capability parameter block, where 1
    // or absent means no multiframe. A runtime that does not publish it has
    // still admitted one generated frame per real pair if the create below
    // succeeds, which is the 2x case, so that success fills in 1 and no larger
    // number is ever assumed.
    unsigned int multiFrameCountMax = 0;
    if (!NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetUI(m_params, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,
                                                    &multiFrameCountMax))) {
        capability.multiFrameCountMax = multiFrameCountMax;
    }

    LOG("NGX DLSS-G capability keys: FrameGeneration.Available=" << std::dec << advertised
        << " (read=0x" << std::hex << advertisedRead << std::dec << ") FeatureInitResult=0x"
        << std::hex << uint32_t(featureInitResult) << std::dec
        << " NeedsUpdatedDriver=" << needsUpdatedDriver
        << " MinDriverVersion=" << minDriverMajor << "." << minDriverMinor
        << " MultiFrameCountMax=" << capability.multiFrameCountMax
        << " HwSchMode=" << (hwSchMode ? std::to_string(*hwSchMode) : std::string("absent")));

    // The DLFG-specific geometry keys, which the runtime prefers over the
    // generic Width/Height pair whenever they are set
    // (nvsdk_ngx_defs_dlssg.h), are populated here as well as in the create
    // params below, so the create does not depend on which of the two pairs
    // NGX_D3D12_CREATE_DLSSG happens to write.
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_DLSSG_Parameter_Width, width);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_DLSSG_Parameter_Height, height);

    NVSDK_NGX_DLSSG_Create_Params createParams{};
    createParams.Width = width;
    createParams.Height = height;
    createParams.NativeBackbufferFormat = static_cast<unsigned int>(backbufferFormat);
    // A player that owns its swapchain presents at one geometry, so the
    // internal (render) size is the backbuffer size and never moves: dynamic
    // resolution is off and the runtime is not asked to admit a range.
    createParams.RenderWidth = width;
    createParams.RenderHeight = height;
    createParams.DynamicResolutionScaling = false;

    // NGX_D3D12_CREATE_DLSSG sets the node masks and the DLSSG keys and then
    // calls the raw NVSDK_NGX_D3D12_CreateFeature with
    // NVSDK_NGX_Feature_FrameGeneration, which is the entry point in question.
    m_lastResult = NGX_D3D12_CREATE_DLSSG(cmd, 1, 1, &m_handle, m_params, &createParams);
    capability.createResult = m_lastResult;
    if (NVSDK_NGX_FAILED(m_lastResult) || !m_handle) {
        m_handle = nullptr;
        Append(capability.detail,
               L"CreateFeature(FrameGeneration) refused: " + HexResultTextWide(uint32_t(m_lastResult)));
        if (NVSDK_NGX_FAILED(advertisedRead)) {
            Append(capability.detail, L"the runtime published no FrameGeneration.Available key");
        } else if (!advertised) {
            Append(capability.detail, L"FrameGeneration.Available=0, FeatureInitResult=" +
                                          HexResultTextWide(uint32_t(featureInitResult)));
        }
        if (needsUpdatedDriver) {
            Append(capability.detail, L"FrameGeneration.NeedsUpdatedDriver=1, the runtime asks for driver " +
                                          std::to_wstring(minDriverMajor) + L"." +
                                          std::to_wstring(minDriverMinor) + L" or newer");
        }
        // Measured here on 2026-09-17 (RTX 5090, driver 616.64): this exact
        // pair - 0xbad0000b with FrameGeneration.Available=0 and
        // FeatureInitResult=0xbad00004 - is what the runtime answers when
        // nvngx_dlssg.dll is not resolvable beside the executable, and staging
        // the vendored one beside it turns the same create into Success. The
        // driver-store copy is found and logged as a fallback but was measured
        // not to serve this path, so the snippet's absence is named here
        // rather than left for a caller to guess at.
        if (!GetModuleHandleW(L"nvngx_dlssg.dll")) {
            Append(capability.detail,
                   L"no nvngx_dlssg.dll is loaded in this process; the DLSS-G snippet has to be resolvable beside "
                   L"the executable, the driver-store copy alone was measured not to serve this path");
        }
        if (!capability.hagsEnabled) Append(capability.detail, HardwareSchedulingSentence(hwSchMode));
        LOG("RAW NGX D3D12 CreateFeature(FrameGeneration) failed result=0x" << std::hex << m_lastResult
            << std::dec << " at " << width << "x" << height << " format=" << int(backbufferFormat));
        return capability;
    }

    // Released at once: this class answers admission and holds nothing. A
    // feature kept alive here would own runtime memory for the rest of the
    // process with no evaluate path to justify it, and the create was recorded
    // on a command list this class does not own and cannot flush.
    const NVSDK_NGX_Result releaseResult = NVSDK_NGX_D3D12_ReleaseFeature(m_handle);
    m_handle = nullptr;
    if (NVSDK_NGX_FAILED(releaseResult)) {
        m_lastResult = releaseResult;
        capability.createResult = releaseResult;
        Append(capability.detail, L"CreateFeature(FrameGeneration) succeeded but ReleaseFeature refused: " +
                                      HexResultTextWide(uint32_t(releaseResult)));
        LOG("RAW NGX D3D12 ReleaseFeature(FrameGeneration) failed result=0x" << std::hex << releaseResult);
        return capability;
    }

    if (!capability.multiFrameCountMax) capability.multiFrameCountMax = 1;
    if (!capability.hagsEnabled) {
        Append(capability.detail, HardwareSchedulingSentence(hwSchMode) +
                                      L"; the runtime admitted the feature anyway");
    }
    capability.available = true;
    m_available = true;
    LOG("RAW NGX D3D12 CreateFeature(FrameGeneration) SUCCESS at " << std::dec << width << "x" << height
        << " format=" << int(backbufferFormat) << " multiFrameCountMax=" << capability.multiFrameCountMax
        << "; released cleanly");
    return capability;
}

void DLSSGBackend::Shutdown()
{
    if (m_handle) {
        NVSDK_NGX_D3D12_ReleaseFeature(m_handle);
        m_handle = nullptr;
    }
    if (m_params) {
        NVSDK_NGX_D3D12_DestroyParameters(m_params);
        m_params = nullptr;
    }
    if (m_sessionLeaseAcquired) {
        ngx_session_detail::ProcessRegistry().Release(
            m_sessionKey,
            [&] { NVSDK_NGX_D3D12_Shutdown1(m_device); });
        m_sessionLeaseAcquired = false;
    }
    m_sessionKey = nullptr;
    m_device = nullptr;
    m_available = false;
}
