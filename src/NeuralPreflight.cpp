#include "NeuralPreflight.h"

#include "NeuralCache.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifndef OFFLINE_NEURAL_RENDERER_TESTING
#include "D3D12Renderer.h"
#include "DLSSBackend.h"
#include "TemporalGuides.h"
#pragma comment(lib, "version.lib")
#endif

namespace {

constexpr std::array<std::wstring_view, 12> kLockedRuntimeFiles{
    L"nvngx_dlssnr.dll", L"nvngx_dlss.dll", L"dxgi.dll", L"renodx-dlss5.addon64", L"sl.common.dll",
    L"sl.dlss.dll", L"sl.dlss_g.dll", L"sl.dlss_nr.dll", L"sl.interposer.dll", L"sl.nis.dll", L"sl.pcl.dll",
    L"sl.reflex.dll"};

// Returns the text between `prefix` and the first `terminator` character
// after it, searched on the original (case-preserved) log.
std::string Between(std::string_view text, std::string_view prefix, std::string_view terminators)
{
    const size_t start = text.find(prefix);
    if (start == std::string_view::npos) return {};
    const size_t valueStart = start + prefix.size();
    const size_t end = text.find_first_of(terminators, valueStart);
    return std::string(text.substr(valueStart, end == std::string_view::npos ? std::string_view::npos : end - valueStart));
}

std::string Trim(std::string value)
{
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) value.pop_back();
    size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) ++first;
    return value.substr(first);
}

std::string_view LineContaining(std::string_view text, size_t position)
{
    const size_t lineStart = text.rfind('\n', position);
    const size_t lineEnd = text.find('\n', position);
    const size_t begin = lineStart == std::string_view::npos ? 0 : lineStart + 1;
    return text.substr(begin, (lineEnd == std::string_view::npos ? text.size() : lineEnd) - begin);
}

std::string LowerAscii(std::string_view value)
{
    std::string result(value);
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') character = char(character - 'A' + 'a');
    }
    return result;
}

std::string Utf8(std::wstring_view text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(std::max(length, 0)), '\0');
    if (length > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

std::wstring FileVersionText(const std::filesystem::path& path)
{
#ifdef OFFLINE_NEURAL_RENDERER_TESTING
    (void)path;
    return {};
#else
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return {};
    std::vector<std::byte> block(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoBytes = 0;
    if (!VerQueryValueW(block.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &infoBytes) || !info ||
        infoBytes < sizeof(VS_FIXEDFILEINFO)) return {};
    return std::to_wstring(HIWORD(info->dwFileVersionMS)) + L'.' + std::to_wstring(LOWORD(info->dwFileVersionMS)) +
           L'.' + std::to_wstring(HIWORD(info->dwFileVersionLS)) + L'.' + std::to_wstring(LOWORD(info->dwFileVersionLS));
#endif
}

} // namespace

std::span<const std::wstring_view> LockedRuntimeFileNames()
{
    return kLockedRuntimeFiles;
}

NeuralRuntimeBanner ParseNeuralRuntimeBanner(std::string_view reshadeLog)
{
    NeuralRuntimeBanner banner;
    banner.reshadeVersion = Between(reshadeLog, "ReShade version '", "'");
    const size_t addon = reshadeLog.find("Registered add-on \"DLSS 5 Neural Rendering\" v");
    if (addon != std::string_view::npos) {
        const std::string_view line = LineContaining(reshadeLog, addon);
        banner.addonVersion = Between(line, "\" v", " ");
        banner.addonApiVersion = Trim(Between(line, "ReShade API version ", ".\r\n"));
    }
    const size_t renodx = reshadeLog.find("RenoDX DLSS5 Generic v");
    if (renodx != std::string_view::npos) {
        const std::string_view line = LineContaining(reshadeLog, renodx);
        banner.renodxVersion = Between(line, "RenoDX DLSS5 Generic v", " ");
        banner.renodxBuild = Between(line, "(build ", ")");
    }
    const size_t dlssnr = reshadeLog.find("DLSSNR ");
    if (dlssnr != std::string_view::npos) {
        const std::string_view line = LineContaining(reshadeLog, dlssnr);
        const std::string value = Between(line, "DLSSNR ", " ");
        if (!value.empty() && value[0] >= '0' && value[0] <= '9') banner.dlssnrRuntime = value;
    }
    const size_t active = reshadeLog.find("active settings: ");
    if (active != std::string_view::npos) {
        const std::string_view line = LineContaining(reshadeLog, active);
        banner.activeSettings = Trim(Between(line, "active settings: ", "\r\n"));
    }
    return banner;
}

std::vector<Feature18Observation> CollectFeature18Observations(std::string_view reshadeLogSegment)
{
    constexpr std::array<std::string_view, 7> markers{
        "feature 18 created", "feature 18 create failed", "feature 18 evaluation succeeded",
        "feature 18 evaluation failed", "feature 18 evaluate raised an exception", "nr skipped:",
        "nr declined an evaluate:"};
    std::vector<Feature18Observation> observations;
    const std::string lower = LowerAscii(reshadeLogSegment);
    size_t lineStart = 0;
    while (lineStart < lower.size()) {
        size_t lineEnd = lower.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = lower.size();
        const std::string_view line(lower.data() + lineStart, lineEnd - lineStart);
        for (const std::string_view marker : markers) {
            if (line.find(marker) == std::string_view::npos) continue;
            Feature18Observation observation;
            observation.line = Trim(std::string(reshadeLogSegment.substr(lineStart, lineEnd - lineStart)));
            observation.failure = marker != "feature 18 created" && marker != "feature 18 evaluation succeeded";
            observations.push_back(std::move(observation));
            break;
        }
        lineStart = lineEnd + 1;
    }
    return observations;
}

std::vector<RuntimeModuleReceipt> DescribeRuntimeModules(const std::filesystem::path& directory,
                                                         std::span<const std::wstring_view> names)
{
    std::vector<RuntimeModuleReceipt> modules;
    modules.reserve(names.size());
    for (const std::wstring_view name : names) {
        RuntimeModuleReceipt module;
        module.name = std::wstring(name);
        const std::filesystem::path path = directory / name;
        std::error_code error;
        module.present = std::filesystem::is_regular_file(path, error) && !error;
        if (module.present) {
            module.sizeBytes = std::filesystem::file_size(path, error);
            if (error) module.sizeBytes = 0;
            module.fileVersion = FileVersionText(path);
            if (const auto digest = Sha256File(path)) module.sha256 = *digest;
        }
        modules.push_back(std::move(module));
    }
    return modules;
}

std::string JsonEscape(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (const unsigned char character : text) {
        switch (character) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (character < 0x20) {
                    constexpr char digits[] = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped += digits[character >> 4];
                    escaped += digits[character & 0xF];
                } else {
                    escaped += static_cast<char>(character);
                }
        }
    }
    return escaped;
}

std::string JsonEscapeWide(std::wstring_view text)
{
    return JsonEscape(Utf8(text));
}

std::string BuildPreflightFailureJson(std::wstring_view detail)
{
    return "{\"schema\":1,\"ok\":false,\"error\":\"" + JsonEscapeWide(detail) + "\"}";
}

#ifndef OFFLINE_NEURAL_RENDERER_TESTING
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

std::string HexResult(long value)
{
    char text[24];
    std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(value));
    return text;
}

} // namespace

neural_worker_protocol::PreflightPayload RunNeuralPreflightProbe(
    HWND renderWindow, const std::filesystem::path& moduleDirectory, const DetectedGpu& gpu)
{
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const std::filesystem::path logPath = moduleDirectory / L"ReShade.log";
    std::error_code error;
    uintmax_t logOffset = std::filesystem::file_size(logPath, error);
    if (error) logOffset = 0;

    std::string failure;
    uint32_t attempts = 0;
    bool created = false;
    NVSDK_NGX_Result ngxResult = NVSDK_NGX_Result_Fail;
    {
        const auto [gridW, gridH] = TemporalGuideGenerator::AnalysisGrid(kProbeWidth, kProbeHeight, kProbeFps);
        D3D12RendererOwner renderer = MakeD3D12Renderer();
        if (!renderer || !renderer->Initialize(renderWindow, kProbeWidth, kProbeHeight, kProbeWidth, kProbeHeight,
                                               gridW, gridH, DefaultNeuralCarrierQuality())) {
            failure = "The probe renderer could not be initialized.";
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
                    failure = "The probe frame could not be evaluated.";
                    ++attempts;
                    break;
                }
            }
            created = renderer->DLSSFeatureCreated();
            ngxResult = renderer->DLSSLastResult();
            if (!created && failure.empty()) failure = "Feature 18 was not created within the probe budget.";
        }
    }
    const std::string segment = ReadNeuralRuntimeLogSegment(logPath, logOffset);
    const NeuralRuntimeEvidence evidence = ParseNeuralRuntimeEvidence(segment);
    const NeuralRuntimeBanner banner = ParseNeuralRuntimeBanner(ReadWholeFile(logPath));
    const auto observations = CollectFeature18Observations(segment);
    const auto modules = DescribeRuntimeModules(moduleDirectory, LockedRuntimeFileNames());
    const bool ok = failure.empty() && created && evidence.Valid();
    if (!ok && failure.empty()) failure = "Feature 18 runtime evidence did not arm the inline interception contract.";
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();

    std::string json = "{\"schema\":1,\"ok\":";
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
            ",\"ngxCreateResult\":\"" + HexResult(static_cast<long>(ngxResult)) + "\"" +
            ",\"observations\":[";
    for (size_t index = 0; index < observations.size(); ++index) {
        if (index) json += ',';
        json += "{\"failure\":" + std::string(observations[index].failure ? "true" : "false") + ",\"line\":\"" +
                JsonEscape(observations[index].line) + "\"}";
    }
    json += "]}";
    if (!failure.empty()) json += ",\"error\":\"" + JsonEscape(failure) + "\"";
    json += "}";

    neural_worker_protocol::PreflightPayload payload;
    payload.ok = ok;
    payload.json = std::move(json);
    return payload;
}
#endif
