#include "DLSSGBackend.h"
#include "Log.h"
#include "NeuralPreflight.h"
#include <windows.h>
#include <cfloat>
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

// The create both entry points share, so admission and production are answered
// about the same call: one differing parameter between them would make the
// probe's verdict say nothing about the feature Initialize goes on to hold.
NVSDK_NGX_Result DLSSGBackend::CreateFeature(ID3D12GraphicsCommandList* cmd,
                                             uint32_t width, uint32_t height, DXGI_FORMAT backbufferFormat)
{
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
    const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSSG(cmd, 1, 1, &m_handle, m_params, &createParams);
    if (NVSDK_NGX_FAILED(result)) m_handle = nullptr;
    return result;
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

    m_lastResult = CreateFeature(cmd, width, height, backbufferFormat);
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

    // Released at once: this entry point answers admission and holds nothing.
    // Initialize is the one that keeps a feature, and it takes a command list
    // whose submission the caller commits to; the create here was recorded on
    // a list this class does not own and cannot flush.
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

bool DLSSGBackend::Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                              uint32_t width, uint32_t height, DXGI_FORMAT backbufferFormat)
{
    if (!device || !cmd || !width || !height) {
        m_lastResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
        return false;
    }

    // A live feature at this geometry already is the feature this call would
    // create, and re-creating it would discard the temporal history it holds -
    // which is the whole reason it is kept - for nothing.
    if (m_handle && m_device == device && m_width == width && m_height == height &&
        m_backbufferFormat == backbufferFormat) {
        return true;
    }
    // Geometry and buffer format are the changes NVIDIA's DLSS Programming
    // Guide 310.6.0 (S3.2 step 6) does require a re-create for. Every command
    // list that referenced the outgoing feature in Evaluate must already have
    // retired (S5.5), which only the caller can know, so this release trusts
    // the caller the same way the SR backend's recreate path does.
    if (m_handle) {
        NVSDK_NGX_D3D12_ReleaseFeature(m_handle);
        m_handle = nullptr;
    }
    m_available = false;
    m_evaluations = 0;

    if (!AcquireSession(device)) return false;

    // The runtime's own ceiling on generated frames per real pair, read the way
    // Probe reads it. An absent key means no multiframe, which is still one
    // generated frame per pair once the create below succeeds, so it floors at
    // 1 and Evaluate refuses anything above whatever this says.
    unsigned int multiFrameCountMax = 0;
    m_multiFrameCountMax =
        NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetUI(m_params, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,
                                                   &multiFrameCountMax)) || multiFrameCountMax == 0
            ? 1u
            : multiFrameCountMax;

    m_lastResult = CreateFeature(cmd, width, height, backbufferFormat);
    if (NVSDK_NGX_FAILED(m_lastResult) || !m_handle) {
        m_handle = nullptr;
        LOG("RAW NGX D3D12 CreateFeature(FrameGeneration) for evaluation failed result=0x" << std::hex
            << m_lastResult << std::dec << " at " << width << "x" << height
            << " format=" << int(backbufferFormat)
            << "; DLSSGBackend::Probe reports why a create is refused");
        return false;
    }

    m_width = width;
    m_height = height;
    m_backbufferFormat = backbufferFormat;
    m_available = true;
    LOG("RAW NGX D3D12 CreateFeature(FrameGeneration) KEPT at " << std::dec << width << "x" << height
        << " format=" << int(backbufferFormat) << " multiFrameCountMax=" << m_multiFrameCountMax
        << "; the create work is on the caller's command list and has to retire before the first evaluate");
    return true;
}

// Everything in the optional evaluate block that a video source can honestly
// state. Each value is a claim about the input, and a claim the source cannot
// support - a projection it has no camera for, a jitter phase nothing applied -
// would be answered by the runtime resolving the frame against geometry that
// never existed.
NVSDK_NGX_DLSSG_Opt_Eval_Params DLSSGBackend::VideoEvalConstants(uint32_t multiFrameCount,
                                                                 uint32_t multiFrameIndex, bool reset) const
{
    NVSDK_NGX_DLSSG_Opt_Eval_Params constants{};
    constants.multiFrameCount = multiFrameCount;
    constants.multiFrameIndex = multiFrameIndex;
    constants.reset = reset;
    // Reset is the caller's statement, never the runtime's: automode is not in
    // play on the raw path, so nothing here overrode the flag.
    constants.automodeOverrideReset = false;

    // A decoded frame has no camera, so there is no view, projection or lens
    // transform to declare and every matrix is the identity. clipToPrevClip and
    // prevClipToClip identity says the camera did not move between the two
    // frames, which is exactly true of a video source: all apparent motion
    // belongs to the content and is already in the MV buffer, which is what
    // cameraMotionIncluded below declares. A made-up projection instead would
    // have the runtime unproject the flat depth proxy into a scene that does
    // not exist, and clipToLensClip identity says the colour is undistorted -
    // the same statement the null distortion field in Evaluate makes.
    const auto setIdentity = [](float matrix[4][4]) {
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) matrix[row][column] = row == column ? 1.0f : 0.0f;
        }
    };
    setIdentity(constants.cameraViewToClip);
    setIdentity(constants.clipToCameraView);
    setIdentity(constants.clipToLensClip);
    setIdentity(constants.clipToPrevClip);
    setIdentity(constants.prevClipToClip);

    // No temporal AA jitter: decoded frames arrive on a fixed sample grid, so
    // there is no sub-pixel phase to report. This is the position
    // DLSSBackend::FillEvaluateParameters takes for SR, for the same reason.
    constants.jitterOffset[0] = 0.0f;
    constants.jitterOffset[1] = 0.0f;

    // DLSSG.MvecScale{X,Y} exist to normalize the buffer into [-1,1]
    // (nvsdk_ngx_params_dlssg.h), so a buffer written in backbuffer pixels is
    // divided by the backbuffer extent and one already written in normalized
    // screen units passes through at 1.0. The units are the caller's
    // declaration via SetMotionVectorUnits; the scale is derived here so the
    // two can never disagree.
    const bool pixelUnits = m_mvecUnits == MotionVectorUnits::BackbufferPixels;
    constants.mvecScale[0] = pixelUnits && m_width ? 1.0f / float(m_width) : 1.0f;
    constants.mvecScale[1] = pixelUnits && m_height ? 1.0f / float(m_height) : 1.0f;

    // -FLT_MAX is unreachable by any displacement an R16G16_FLOAT buffer can
    // hold, so nothing in the buffer is treated as invalid. The sentinel cannot
    // be 0: a video producer writes zero for "this pixel did not move", which
    // is most of a static shot, and calling that invalid would throw away the
    // one thing the buffer is certain about.
    constants.motionVectorsInvalidValue = -FLT_MAX;
    // Per-pixel vectors straight from the estimator, not spread over silhouette
    // edges by a dilation pass the video path does not run.
    constants.motionVectorsDilated = false;

    constants.cameraMotionIncluded = true;
    // A frame is a flat image on a sample grid, not a perspective view of a
    // scene, so the projection it was produced under is orthographic.
    constants.orthoProjection = true;
    // The depth handed in is a flat proxy written near-to-far as a plain [0,1]
    // value, so it is not reversed-Z. depthInverted also selects the runtime's
    // linearization (nvsdk_ngx_defs_dlssg.h: lin = 1/(1-depth) when false), and
    // false is the branch that stays finite for a proxy written below 1.0.
    constants.depthInverted = false;
    // The backbuffer this path hands over is the player's SDR present surface.
    constants.colorBuffersHDR = false;
    // Every evaluate here is driven by a real decoded frame. There is no paused
    // menu and no cut-scene player to declare, and saying otherwise would ask
    // the runtime to stop generating exactly when the video is playing.
    constants.notRenderingGameFrames = false;
    // Fullscreen menu detection looks for a game menu that replaced the scene.
    // This player composites its own UI after this pass, so there is no such
    // menu in the backbuffer and the heuristic has only false positives to find
    // in ordinary video content.
    constants.menuDetectionEnabled = false;

    // Explicit full-frame extents on every resource this path actually hands
    // over, the way DLSSBackend states its subrects: with EvalFlags left at its
    // default the runtime writes interpolated pixels inside the backbuffer
    // extent and uninterpolated ones outside it, so declaring the extent as the
    // whole frame is what says there is no "outside" here. The extents of the
    // resources Evaluate leaves null stay zero.
    const NVSDK_NGX_Dimensions frame{m_width, m_height};
    constants.backbufferSubrectSize = frame;
    constants.mvecsSubrectSize = frame;
    constants.depthSubrectSize = frame;
    constants.outputInterpSubrectSize = frame;
    return constants;
}

bool DLSSGBackend::Evaluate(ID3D12GraphicsCommandList* cmd,
                            ID3D12Resource* backbuffer,
                            ID3D12Resource* motion,
                            ID3D12Resource* depth,
                            ID3D12Resource* outputInterpolated,
                            uint32_t multiFrameCount,
                            uint32_t multiFrameIndex,
                            bool reset,
                            uint64_t backbufferFrameId)
{
    if (!Available() || !m_handle || !cmd || !backbuffer || !motion || !depth || !outputInterpolated) return false;
    // The index is 1-based and bounded by the count, and the count by what the
    // runtime published. Out of range is a caller bug, refused here rather than
    // handed to the runtime to interpret.
    if (!multiFrameCount || !multiFrameIndex || multiFrameIndex > multiFrameCount ||
        multiFrameCount > m_multiFrameCountMax) {
        m_lastResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
        LOG("DLSS-G evaluate refused: multiFrameIndex=" << std::dec << multiFrameIndex << " of count="
            << multiFrameCount << " is outside 1.." << m_multiFrameCountMax);
        return false;
    }

    NVSDK_NGX_D3D12_DLSSG_Eval_Params evalParams{};
    evalParams.pBackbuffer = backbuffer;
    evalParams.pMVecs = motion;
    evalParams.pDepth = depth;
    evalParams.pOutputInterpFrame = outputInterpolated;
    // Null because a decoded video frame has nothing to put in them, not
    // because they were forgotten: the frame is one flat composited layer, so
    // there is no HUD-less copy of it to hand over, no separate UI colour or
    // alpha plane, and no post-process lens distortion for a bidirectional
    // distortion field to undo. pOutputRealFrame is null because this path
    // presents the decoded frame itself and will not have the runtime draw
    // debug text into it, and pOutputDisableInterpolation is null because the
    // hint it carries only matters to a presentation path that could act on it,
    // which is a later measurement than this one.
    evalParams.pHudless = nullptr;
    evalParams.pUI = nullptr;
    evalParams.pUIAlpha = nullptr;
    evalParams.pBidirectionalDistortionField = nullptr;
    evalParams.pOutputRealFrame = nullptr;
    evalParams.pOutputDisableInterpolation = nullptr;

    // DLSSG.BackbufferFrameID, set before the helper writes the rest of the
    // block so it travels with this evaluate. It is not in
    // NVSDK_NGX_DLSSG_Opt_Eval_Params - the vendored struct has no field for it
    // - so it is written straight onto the parameter block the way the create
    // path writes DLSSG.Width/Height. Left unset at 0: a runtime that reads the
    // key is better off seeing it absent than seeing a counter that restarted.
    if (backbufferFrameId != 0) {
        NVSDK_NGX_Parameter_SetULL(m_params, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,
                                   backbufferFrameId);
    }

    NVSDK_NGX_DLSSG_Opt_Eval_Params constants = VideoEvalConstants(multiFrameCount, multiFrameIndex, reset);

    // NGX_D3D12_EVALUATE_DLSSG writes every DLSSG.* key from the two structs -
    // including the optional resources above, which it sets to null explicitly
    // rather than leaving whatever the last evaluate put there - and finishes
    // with the raw NVSDK_NGX_D3D12_EvaluateFeature_C, which is the same entry
    // point the SR path uses.
    m_lastResult = NGX_D3D12_EVALUATE_DLSSG(cmd, m_handle, m_params, &evalParams, &constants);
    if (NVSDK_NGX_FAILED(m_lastResult)) {
        LOG("RAW NGX D3D12 EvaluateFeature(FrameGeneration) failed result=0x" << std::hex << m_lastResult
            << std::dec << " frame " << multiFrameIndex << "/" << multiFrameCount
            << " reset=" << (reset ? 1 : 0));
        return false;
    }

    ++m_evaluations;
    if (m_evaluations <= 2 || (m_evaluations % 300) == 0) {
        LOG("RAW NGX D3D12 EvaluateFeature(FrameGeneration) SUCCESS #" << std::dec << m_evaluations
            << " at " << m_width << "x" << m_height << " frame " << multiFrameIndex << "/" << multiFrameCount
            << " mvecScale=(" << constants.mvecScale[0] << "," << constants.mvecScale[1] << ")"
            << " reset=" << (reset ? 1 : 0));
    }
    return true;
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
    m_width = 0;
    m_height = 0;
    m_backbufferFormat = DXGI_FORMAT_UNKNOWN;
    m_multiFrameCountMax = 0;
    m_evaluations = 0;
    m_available = false;
}
