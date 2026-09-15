#include "NeuralPreflightProbe.h"

#include "D3D12Renderer.h"
#include "DLSSBackend.h"
#include "NeuralPreflight.h"
#include "OfflineNeuralRenderer.h"
#include "TemporalGuides.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kProbeWidth = 1280;
constexpr uint32_t kProbeHeight = 720;
constexpr double kProbeFps = 30.0;
constexpr uint32_t kProbeFrameLimit = 120;

// Moving diagonal gradient so the reconstructed guides carry real motion.
void FillProbeFrame(std::vector<uint8_t>& bgra, uint32_t index)
{
    bgra.resize(size_t{kProbeWidth} * kProbeHeight * 4u);
    for (uint32_t y = 0; y < kProbeHeight; ++y) {
        for (uint32_t x = 0; x < kProbeWidth; ++x) {
            const uint32_t phase = (x + y + index * 3u) & 0xFF;
            uint8_t* pixel = bgra.data() + (size_t{y} * kProbeWidth + x) * 4u;
            pixel[0] = static_cast<uint8_t>(phase);
            pixel[1] = static_cast<uint8_t>(255u - phase);
            pixel[2] = static_cast<uint8_t>((x * 255u) / kProbeWidth);
            pixel[3] = 255;
        }
    }
}

std::string ReadWholeFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

neural_worker_protocol::PreflightPayload RunNeuralPreflightProbe(
    HWND renderWindow, const std::filesystem::path& moduleDirectory, const DetectedGpu& gpu)
{
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    // The proxy's log file is chosen by session, not by name: ReShade rotates
    // to ReShade.log1 when another process still holds ReShade.log, and a
    // previous session's file may still be present.

    std::wstring failure;
    uint32_t attempts = 0;
    bool created = false;
    NVSDK_NGX_Result ngxResult = NVSDK_NGX_Result_Fail;
    {
        const auto [gridW, gridH] = TemporalGuideGenerator::AnalysisGrid(kProbeWidth, kProbeHeight, kProbeFps);
        D3D12RendererOwner renderer = MakeD3D12Renderer();
        if (!renderer || !renderer->Initialize(renderWindow, kProbeWidth, kProbeHeight, kProbeWidth, kProbeHeight,
                                               gridW, gridH, DefaultNeuralCarrierQuality())) {
            failure = L"The probe renderer could not be initialized.";
        } else {
            renderer->SetDLSS(true);
            TemporalGuideGenerator guides;
            std::vector<uint8_t> frame;
            GuideFrame guide;
            for (; attempts < kProbeFrameLimit && !renderer->DLSSFeatureCreated(); ++attempts) {
                FillProbeFrame(frame, attempts);
                const FrameIdentity probeFrame{attempts, static_cast<int64_t>(double(attempts) * 1e7 / kProbeFps),
                                               0, 0, 0, attempts == 0 ? HistoryReset::FirstFrame : HistoryReset::None};
                if (!guides.Generate(frame.data(), kProbeWidth, kProbeHeight, kProbeWidth, kProbeHeight, kProbeFps,
                                     probeFrame, guide) ||
                    !renderer->RenderFrame(frame.data(), frame.size(), guide.guideGridRGBA32F.data(),
                                           guide.guideGridRGBA32F.size() * sizeof(float), guide.gridW, guide.gridH,
                                           attempts == 0, static_cast<float>(1000.0 / kProbeFps))) {
                    failure = L"The probe frame could not be evaluated.";
                    ++attempts;
                    break;
                }
            }
            created = renderer->DLSSFeatureCreated();
            ngxResult = renderer->DLSSLastResult();
            if (!created && failure.empty()) failure = L"Feature 18 was not created within the probe budget.";
        }
    }
    const std::string segment = ReadNeuralRuntimeSessionLog(moduleDirectory);
    const NeuralRuntimeEvidence evidence = ParseNeuralRuntimeEvidence(segment);
    const std::filesystem::path logPath = ResolveNeuralRuntimeLogPath(moduleDirectory);
    const NeuralRuntimeBanner banner = ParseNeuralRuntimeBanner(logPath.empty() ? segment : ReadWholeFile(logPath));
    const auto observations = CollectFeature18Observations(segment);
    const auto modules = DescribeRuntimeModules(moduleDirectory, LockedRuntimeFileNames());
    const bool ok = failure.empty() && created && evidence.Valid();
    const NeuralPreflightDiagnosis diagnosis =
        DiagnoseNeuralPreflight(gpu, observations, created, evidence.Valid(), failure);
    if (!ok && failure.empty()) failure = L"Feature 18 runtime evidence did not arm the inline interception contract.";
    // The classified sentence names the code and the action; the generic one
    // is only what is left when nothing could be classified.
    const std::wstring& error = diagnosis.cause == NeuralPreflightCause::None ? failure : diagnosis.detail;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();

    std::string json = "{\"schema\":3,\"ok\":";
    json += ok ? "true" : "false";
    json += ",\"workerVersion\":\"" DLSS_VIDEO_PLAYER_VERSION "\"";
    json += ",\"elapsedMilliseconds\":" + std::to_string(elapsed);
    json += ",\"gpu\":{\"description\":\"" + JsonEscapeWide(gpu.description) + "\",\"vendorId\":" +
            std::to_string(gpu.vendorId) + ",\"deviceId\":" + std::to_string(gpu.deviceId) +
            ",\"dedicatedVideoMemoryMiB\":" + std::to_string(gpu.dedicatedVideoMemoryBytes >> 20) +
            ",\"driverVersion\":\"" + JsonEscapeWide(gpu.driverVersion) + "\"}";
    json += ",\"runtime\":{\"reshade\":\"" + JsonEscape(banner.reshadeVersion) + "\",\"addon\":\"" +
            JsonEscape(banner.addonVersion) + "\",\"addonApi\":\"" + JsonEscape(banner.addonApiVersion) +
            "\",\"renodx\":\"" + JsonEscape(banner.renodxVersion) + "\",\"renodxBuild\":\"" +
            JsonEscape(banner.renodxBuild) + "\",\"dlssnr\":\"" + JsonEscape(banner.dlssnrRuntime) +
            "\",\"activeSettings\":\"" + JsonEscape(banner.activeSettings) + "\"}";
    // The weights the pass evaluates are resolved outside the staged runtime,
    // so the receipt records what the render identity's model-store term
    // covered, and names the driver-version fallback instead of taking it
    // silently.
    const NeuralModelStore modelStore = ResolveNeuralModelStore(gpu.driverVersion);
    json += ",\"modelStore\":" + NeuralModelStoreJson(modelStore);
    json += ",\"modules\":[";
    for (size_t index = 0; index < modules.size(); ++index) {
        const RuntimeModuleReceipt& module = modules[index];
        if (index) json += ',';
        json += "{\"name\":\"" + JsonEscapeWide(module.name) + "\",\"present\":" + (module.present ? "true" : "false") +
                ",\"size\":" + std::to_string(module.sizeBytes) + ",\"fileVersion\":\"" +
                JsonEscapeWide(module.fileVersion) + "\",\"sha256\":\"" + JsonEscape(module.sha256) + "\"}";
    }
    json += "]";
    json += ",\"feature18\":{\"created\":" + std::string(evidence.feature18Created ? "true" : "false") +
            ",\"evaluated\":" + (evidence.feature18Evaluated ? "true" : "false") +
            ",\"armed\":" + (evidence.Valid() ? "true" : "false") +
            ",\"upscalingOff\":" + (evidence.upscalingOff ? "true" : "false") +
            ",\"inlineInterception\":" + (evidence.inlineInterceptionContract ? "true" : "false") +
            ",\"laterFailure\":" + (evidence.laterFailure ? "true" : "false") +
            ",\"highestEvaluation\":" + std::to_string(evidence.highestObservedEvaluation) +
            ",\"probeFrames\":" + std::to_string(attempts) +
            ",\"carrierCreateResult\":\"" + HexResultText(static_cast<uint32_t>(ngxResult)) + "\"" +
            ",\"createResult\":\"" +
            (diagnosis.ngxResult ? HexResultText(*diagnosis.ngxResult) : std::string()) + "\"" +
            ",\"observations\":[";
    for (size_t index = 0; index < observations.size(); ++index) {
        if (index) json += ',';
        json += "{\"failure\":" + std::string(observations[index].failure ? "true" : "false") + ",\"line\":\"" +
                JsonEscape(observations[index].line) + "\"}";
    }
    json += "]}";
    json += ",\"diagnosis\":{\"cause\":\"" + std::string(NeuralPreflightCauseName(diagnosis.cause)) +
            "\",\"detail\":\"" + JsonEscapeWide(diagnosis.detail) + "\"}";
    if (!error.empty()) json += ",\"error\":\"" + JsonEscapeWide(error) + "\"";
    json += "}";

    neural_worker_protocol::PreflightPayload payload;
    payload.ok = ok;
    payload.json = std::move(json);
    return payload;
}
