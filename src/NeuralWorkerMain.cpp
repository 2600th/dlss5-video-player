#include "NeuralWorker.h"
#include "ReShadeConfig.h"
#include "RuntimePolicy.h"

#include <windows.h>
#include <mfapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kProtocolMagic = 0x3152574Eu; // NWR1
constexpr uint16_t kProtocolVersion = 1;
constexpr uint32_t kMaximumDetailBytes = 4 * 1024;

enum class WireKind : uint16_t { Progress = 1, Result = 2 };

#pragma pack(push, 1)
struct WireHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t payloadBytes;
};

struct WireProgress {
    uint32_t phase;
    uint64_t completedFrames;
    uint64_t totalFrames;
    uint64_t bytes;
    int64_t elapsedMilliseconds;
    int64_t estimatedRemainingMilliseconds;
};

struct WireResult {
    uint8_t ok;
    uint8_t cancelled;
    uint8_t encoder;
    uint8_t feature18ArmedBeforeCapture;
    uint8_t upscalingOff;
    uint8_t inlineInterceptionContract;
    uint8_t feature18Created;
    uint8_t feature18Evaluated;
    uint8_t laterFailure;
    uint8_t reserved[7];
    uint64_t frameCount;
    int64_t duration100ns;
    uint64_t nativeEvaluations;
    uint64_t verifiedNeuralFrames;
    uint64_t highestObservedEvaluation;
    uint32_t detailBytes;
};
#pragma pack(pop)

static_assert(sizeof(WireHeader) == 12);
static_assert(sizeof(WireProgress) == 44);
static_assert(sizeof(WireResult) == 60);

std::filesystem::path ModuleDirectory()
{
    std::wstring value(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!length || length >= value.size()) return {};
    value.resize(length);
    return std::filesystem::path(value).parent_path();
}

bool WriteAll(HANDLE handle, const void* data, size_t bytes)
{
    const auto* cursor = static_cast<const std::byte*>(data);
    while (bytes) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes, MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(handle, cursor, chunk, &written, nullptr) || !written) return false;
        cursor += written;
        bytes -= written;
    }
    return true;
}

class MetadataWriter {
public:
    explicit MetadataWriter(HANDLE handle) : handle_(handle) {}

    bool WriteProgress(const NeuralRenderProgress& progress)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(progress.elapsed).count();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(progress.estimatedRemaining).count();
        const WireProgress wire{static_cast<uint32_t>(progress.phase), progress.completedFrames,
            progress.totalFrames, progress.bytes, elapsed, remaining};
        return Write(WireKind::Progress, &wire, sizeof(wire));
    }

    bool WriteResult(const NeuralRenderResult& result)
    {
        std::wstring detail = result.detail;
        const size_t maxCharacters = kMaximumDetailBytes / sizeof(wchar_t);
        if (detail.size() > maxCharacters) detail.resize(maxCharacters);
        WireResult wire{};
        wire.ok = result.ok ? 1 : 0;
        wire.cancelled = result.cancelled ? 1 : 0;
        wire.encoder = static_cast<uint8_t>(result.encoder);
        wire.feature18ArmedBeforeCapture = result.feature18ArmedBeforeCapture ? 1 : 0;
        wire.upscalingOff = result.evidence.upscalingOff ? 1 : 0;
        wire.inlineInterceptionContract = result.evidence.inlineInterceptionContract ? 1 : 0;
        wire.feature18Created = result.evidence.feature18Created ? 1 : 0;
        wire.feature18Evaluated = result.evidence.feature18Evaluated ? 1 : 0;
        wire.laterFailure = result.evidence.laterFailure ? 1 : 0;
        wire.frameCount = result.frameCount;
        wire.duration100ns = result.duration100ns;
        wire.nativeEvaluations = result.nativeEvaluations;
        wire.verifiedNeuralFrames = result.verifiedNeuralFrames;
        wire.highestObservedEvaluation = result.evidence.highestObservedEvaluation;
        wire.detailBytes = static_cast<uint32_t>(detail.size() * sizeof(wchar_t));
        std::vector<std::byte> payload(sizeof(wire) + wire.detailBytes);
        std::memcpy(payload.data(), &wire, sizeof(wire));
        if (wire.detailBytes) std::memcpy(payload.data() + sizeof(wire), detail.data(), wire.detailBytes);
        return Write(WireKind::Result, payload.data(), static_cast<uint32_t>(payload.size()));
    }

private:
    bool Write(WireKind kind, const void* payload, uint32_t payloadBytes)
    {
        std::lock_guard lock(mutex_);
        const WireHeader header{kProtocolMagic, kProtocolVersion, static_cast<uint16_t>(kind), payloadBytes};
        return WriteAll(handle_, &header, sizeof(header)) &&
            (!payloadBytes || WriteAll(handle_, payload, payloadBytes));
    }

    HANDLE handle_{};
    std::mutex mutex_;
};

bool ParseUnsigned(std::wstring_view text, uint64_t& value)
{
    if (text.empty()) return false;
    value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') return false;
        const uint64_t digit = static_cast<uint64_t>(character - L'0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    return true;
}

bool ParseDouble(std::wstring_view text, double& value)
{
    if (text.empty() || text.size() >= 128) return false;
    std::wstring copy(text);
    wchar_t* end = nullptr;
    value = std::wcstod(copy.c_str(), &end);
    return end == copy.c_str() + copy.size() && std::isfinite(value);
}

struct WorkerArguments {
    HANDLE metadata{};
    NeuralRenderRequest request;
    bool configurationRestarted{};
};

std::optional<WorkerArguments> ParseArguments(int argc, wchar_t** argv)
{
    std::vector<std::wstring_view> values;
    values.reserve(static_cast<size_t>(argc));
    for (int index = 0; index < argc; ++index) values.emplace_back(argv[index]);
    if (!neural_worker_detail::HasValidWorkerArgumentShape(values)) return std::nullopt;
    const bool configurationRestarted = argc == 17;
    std::optional<std::wstring> metadata;
    std::optional<std::wstring> source;
    std::optional<std::wstring> staging;
    std::optional<std::wstring> width;
    std::optional<std::wstring> height;
    std::optional<std::wstring> fps;
    std::optional<std::wstring> duration;
    for (int index = 2; index < 16; index += 2) {
        const std::wstring_view key(argv[index]);
        const std::wstring value(argv[index + 1]);
        auto assign = [&](std::optional<std::wstring>& destination) {
            if (destination.has_value()) return false;
            destination = value;
            return true;
        };
        if (key == L"--metadata-handle") { if (!assign(metadata)) return std::nullopt; }
        else if (key == L"--source") { if (!assign(source)) return std::nullopt; }
        else if (key == L"--staging") { if (!assign(staging)) return std::nullopt; }
        else if (key == L"--width") { if (!assign(width)) return std::nullopt; }
        else if (key == L"--height") { if (!assign(height)) return std::nullopt; }
        else if (key == L"--fps") { if (!assign(fps)) return std::nullopt; }
        else if (key == L"--duration-100ns") { if (!assign(duration)) return std::nullopt; }
        else return std::nullopt;
    }
    if (!metadata || !source || !staging || !width || !height || !fps || !duration ||
        source->empty() || staging->empty()) return std::nullopt;
    uint64_t rawHandle = 0, parsedWidth = 0, parsedHeight = 0, parsedDuration = 0;
    double parsedFps = 0.0;
    if (!ParseUnsigned(*metadata, rawHandle) || !ParseUnsigned(*width, parsedWidth) ||
        !ParseUnsigned(*height, parsedHeight) || !ParseUnsigned(*duration, parsedDuration) ||
        !ParseDouble(*fps, parsedFps) || !rawHandle || parsedWidth > UINT32_MAX ||
        parsedHeight > UINT32_MAX || !parsedWidth || !parsedHeight || parsedDuration == 0 ||
        parsedDuration > INT64_MAX || parsedFps <= 0.0) return std::nullopt;
    WorkerArguments parsed;
    parsed.metadata = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(rawHandle));
    parsed.request.sourcePath = *source;
    parsed.request.stagingVideoPath = *staging;
    parsed.request.width = static_cast<uint32_t>(parsedWidth);
    parsed.request.height = static_cast<uint32_t>(parsedHeight);
    parsed.request.fps = parsedFps;
    parsed.request.durationSeconds = static_cast<double>(parsedDuration) / 10000000.0;
    parsed.configurationRestarted = configurationRestarted;
    return parsed;
}

class MediaFoundationScope {
public:
    bool Start()
    {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return false;
        comInitialized_ = true;
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) return false;
        mediaFoundationStarted_ = true;
        return true;
    }

    ~MediaFoundationScope()
    {
        if (mediaFoundationStarted_) MFShutdown();
        if (comInitialized_) CoUninitialize();
    }

private:
    bool comInitialized_{};
    bool mediaFoundationStarted_{};
};

LRESULT CALLBACK HiddenWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND CreateHiddenRenderWindow()
{
    constexpr wchar_t kClassName[] = L"DLSSVideoPlayerNeuralWorkerWindow";
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = HiddenWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kClassName;
    const ATOM registered = RegisterClassExW(&windowClass);
    if (!registered && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP,
        0, 0, 16, 16, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (window) ShowWindow(window, SW_HIDE);
    return window;
}

void PumpMessagesUntil(HANDLE completed)
{
    for (;;) {
        const DWORD wait = MsgWaitForMultipleObjects(1, &completed, FALSE, 50, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) return;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const auto arguments = ParseArguments(argc, argv);
    if (!arguments) return 2;
    MetadataWriter metadata(arguments->metadata);
    const std::filesystem::path moduleDirectory = ModuleDirectory();
    const ConfigUpdate config = ConfigureNeuralAddon(moduleDirectory / L"ReShade.ini", true);
    if (!config.ok || !config.addonEnabled) {
        NeuralRenderResult failed;
        failed.detail = config.error.empty() ? L"The helper-local neural add-on configuration was not enabled." :
                                               config.error;
        metadata.WriteResult(failed);
        return 0;
    }
    // ReShade reads its INI while its proxy is loaded at process startup. If
    // this invocation repaired the helper-local contract, exit and let the
    // hook-free parent launch one fresh helper. Full exit is essential: the
    // proxy must release its log before the rendering process starts.
    if (config.changed) {
        if (!arguments->configurationRestarted)
            return neural_worker_detail::kConfigurationChangedExitCode;
        NeuralRenderResult failed;
        failed.detail = L"The helper-local neural configuration changed again after restart.";
        metadata.WriteResult(failed);
        return 0;
    }
    // Match the player bootstrap ordering: enter the proxy on the main thread
    // before Media Foundation or a decoder can load the system DXGI path.
    const auto gpu=DetectHighPerformanceGpu();
    (void)gpu;
    MediaFoundationScope mediaFoundation;
    if (!mediaFoundation.Start()) {
        NeuralRenderResult failed;
        failed.detail = L"The helper could not initialize its media runtime.";
        metadata.WriteResult(failed);
        return 0;
    }
    HWND renderWindow = CreateHiddenRenderWindow();
    if (!renderWindow) {
        NeuralRenderResult failed;
        failed.detail = L"The helper could not create its hidden render window.";
        metadata.WriteResult(failed);
        return 0;
    }
    NeuralRenderRequest request = arguments->request;
    request.renderWindow = renderWindow;
    HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completed) {
        DestroyWindow(renderWindow);
        NeuralRenderResult failed;
        failed.detail = L"The helper could not create its render completion event.";
        metadata.WriteResult(failed);
        return 0;
    }
    NeuralRenderResult result;
    std::jthread render([&] {
        OfflineNeuralRenderer renderer;
        result = renderer.Run(request, [&](const NeuralRenderProgress& progress) {
            metadata.WriteProgress(progress);
        });
        SetEvent(completed);
    });
    PumpMessagesUntil(completed);
    render.join();
    CloseHandle(completed);
    DestroyWindow(renderWindow);
    return metadata.WriteResult(result) ? 0 : 3;
}
