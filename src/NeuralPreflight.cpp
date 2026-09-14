#include "NeuralPreflight.h"

#include "NeuralCache.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "version.lib")

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

// Lower-cased input only: every caller scans a LowerAscii copy of the log.
int HexNibble(char character) noexcept
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

std::wstring HexResultTextWide(uint32_t value)
{
    const std::string text = HexResultText(value);
    return std::wstring(text.begin(), text.end());
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

std::optional<uint32_t> ParseFeature18CreateResult(std::span<const Feature18Observation> observations)
{
    constexpr std::string_view marker = "feature 18 create failed with 0x";
    std::optional<uint32_t> result;
    for (const Feature18Observation& observation : observations) {
        const std::string line = LowerAscii(observation.line);
        for (size_t at = line.find(marker); at != std::string::npos; at = line.find(marker, at + marker.size())) {
            const size_t start = at + marker.size();
            size_t end = start;
            while (end < line.size() && HexNibble(line[end]) >= 0) ++end;
            // A 32-bit result and nothing else: an empty or over-long run is
            // some other number that happens to follow the marker.
            if (end == start || end - start > 8) continue;
            uint32_t value = 0;
            for (size_t index = start; index < end; ++index) {
                value = (value << 4) | static_cast<uint32_t>(HexNibble(line[index]));
            }
            result = value;
        }
    }
    return result;
}

const char* NeuralPreflightCauseName(NeuralPreflightCause cause) noexcept
{
    const size_t index = static_cast<size_t>(cause);
    // Every entry is a string literal, so data() is null-terminated.
    return index < kNeuralPreflightCauseNames.size() ? kNeuralPreflightCauseNames[index].data() : "none";
}

NeuralPreflightDiagnosis DiagnoseNeuralPreflight(const DetectedGpu& gpu,
                                                 std::span<const Feature18Observation> observations,
                                                 bool created,
                                                 bool evidenceValid,
                                                 std::wstring_view probeFailure)
{
    constexpr uint32_t kFeatureNotSupported = 0xbad00001u;  // NVSDK_NGX_Result_FAIL_FeatureNotSupported
    constexpr uint32_t kPlatformError = 0xbad00002u;        // NVSDK_NGX_Result_FAIL_PlatformError
    constexpr uint32_t kOutOfGpuMemory = 0xbad0000du;       // NVSDK_NGX_Result_FAIL_OutOfGPUMemory

    NeuralPreflightDiagnosis diagnosis;
    diagnosis.ngxResult = ParseFeature18CreateResult(observations);
    // Nothing refused the feature in the end: a code from a create the runtime
    // went on to satisfy is history, not a verdict.
    if (created && evidenceValid && probeFailure.empty()) return diagnosis;

    // ClassifyNeuralDriver, with the parsed number kept for the message.
    const std::optional<NvidiaDriverVersion> driver = ParseNvidiaDriverVersion(gpu.driverVersion);
    const bool belowFloor = driver && *driver < kNeuralDriverFloor;
    const std::wstring recommended = FormatNvidiaDriverVersion(kNeuralDriverRecommended);
    const auto blameDriver = [&] {
        diagnosis.cause = NeuralPreflightCause::DriverBelowFloor;
        diagnosis.detail = L"NVIDIA driver " + FormatNvidiaDriverVersion(*driver) + L" is below the " +
                           FormatNvidiaDriverVersion(kNeuralDriverFloor) +
                           L" minimum for neural rendering. Update to " + recommended + L" or newer, then try again.";
    };

    if (diagnosis.ngxResult == kFeatureNotSupported) {
        diagnosis.cause = NeuralPreflightCause::ArchitectureUnsupported;
        diagnosis.detail = L"The neural runtime refused feature 18 with " + HexResultTextWide(kFeatureNotSupported) +
                           L" (feature not supported): this GPU architecture is not served by the neural runtime, "
                           L"so neural rendering is unavailable on it.";
    } else if (diagnosis.ngxResult == kOutOfGpuMemory) {
        diagnosis.cause = NeuralPreflightCause::OutOfVideoMemory;
        diagnosis.detail = L"The neural runtime ran out of GPU memory creating feature 18 (" +
                           HexResultTextWide(kOutOfGpuMemory) +
                           L"). Choose a lower source resolution or close other GPU applications, then try again.";
    } else if (diagnosis.ngxResult == kPlatformError) {
        // The driver's NGX core, not the runtime, declined to service the
        // feature. An out-of-date driver is the cause that can be acted on.
        if (belowFloor) {
            blameDriver();
        } else {
            diagnosis.cause = NeuralPreflightCause::PlatformRefusal;
            diagnosis.detail = L"The neural runtime refused feature 18 with " + HexResultTextWide(kPlatformError) +
                               L" (platform error). Update the NVIDIA driver to " + recommended +
                               L" or newer and close other DLSS injectors or overlays, then try again.";
        }
    } else if (belowFloor) {
        blameDriver();
    } else if (!probeFailure.empty()) {
        diagnosis.cause = NeuralPreflightCause::ProbeFailed;
        diagnosis.detail = probeFailure;
    } else {
        // The early return above leaves only an incomplete evidence chain.
        diagnosis.cause = NeuralPreflightCause::EvidenceIncomplete;
        diagnosis.detail = L"The neural runtime did not arm the inline interception contract for feature 18. "
                           L"Close other DLSS injectors or overlays and try again; if it repeats, reinstall the "
                           L"neural runtime.";
    }
    return diagnosis;
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
            if (const auto digest = Sha256FileCached(path)) module.sha256 = *digest;
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
    return "{\"schema\":2,\"ok\":false,\"error\":\"" + JsonEscapeWide(detail) + "\"}";
}

// NGX results are always written as eight lower-case hex digits ("0xbad00002")
// so a receipt and a log line can be compared literally.
std::string HexResultText(uint32_t value)
{
    std::string text = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) text.push_back("0123456789abcdef"[(value >> shift) & 0xFu]);
    return text;
}

