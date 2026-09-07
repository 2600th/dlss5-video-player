#include "NeuralPreflight.h"
#include "NeuralWorker.h"
#include "NeuralWorkerProtocol.h"
#include "ReShadeConfig.h"
#include "RuntimePolicy.h"

#include <windows.h>
#include <mfapi.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace neural_worker_protocol;

namespace {

std::filesystem::path ModuleDirectory()
{
    std::wstring value(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!length || length >= value.size()) return {};
    value.resize(length);
    return std::filesystem::path(value).parent_path();
}

class MetadataWriter {
public:
    explicit MetadataWriter(HANDLE handle) : handle_(handle) {}

    bool WriteProgress(const NeuralRenderProgress& progress)
    {
        const WireProgress wire = EncodeProgress(progress);
        std::lock_guard lock(mutex_);
        return WriteMessage(handle_, WireKind::Progress, &wire, sizeof(wire));
    }

    bool WriteResult(const NeuralRenderResult& result)
    {
        const std::vector<std::byte> payload = EncodeResult(result);
        std::lock_guard lock(mutex_);
        return WriteMessage(handle_, WireKind::Result, payload.data(), static_cast<uint32_t>(payload.size()));
    }

    bool WritePreflight(const PreflightPayload& preflight)
    {
        const std::vector<std::byte> payload = EncodePreflight(preflight);
        std::lock_guard lock(mutex_);
        return WriteMessage(handle_, WireKind::Preflight, payload.data(), static_cast<uint32_t>(payload.size()));
    }

private:
    HANDLE handle_{};
    std::mutex mutex_;
};

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

NeuralRenderResult FailedResult(std::wstring detail, uint64_t jobId)
{
    NeuralRenderResult failed;
    failed.failure = NeuralRenderFailure::Preflight;
    failed.jobId = jobId;
    failed.detail = std::move(detail);
    return failed;
}

// Runs `body` on a worker thread while the main thread keeps the hidden
// window's message pump alive for the proxy and DXGI.
template <class Body>
bool RunWithMessagePump(Body&& body)
{
    HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completed) return false;
    std::jthread worker([&] {
        body();
        SetEvent(completed);
    });
    PumpMessagesUntil(completed);
    worker.join();
    CloseHandle(completed);
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::wstring_view> values;
    values.reserve(static_cast<size_t>(argc));
    for (int index = 0; index < argc; ++index) values.emplace_back(argv[index]);
    const auto arguments = neural_worker_detail::ParseWorkerArguments(values);
    if (!arguments) return 2;
    MetadataWriter metadata(arguments->metadata);
    const uint64_t jobId = arguments->request.jobId;
    auto fail = [&](std::wstring detail) {
        if (arguments->preflight) {
            PreflightPayload payload;
            payload.json = BuildPreflightFailureJson(detail);
            metadata.WritePreflight(payload);
        } else {
            metadata.WriteResult(FailedResult(std::move(detail), jobId));
        }
        return 0;
    };
    const std::filesystem::path moduleDirectory = ModuleDirectory();
    const ConfigUpdate config = ConfigureNeuralAddon(moduleDirectory / L"ReShade.ini", true);
    if (!config.ok || !config.addonEnabled) {
        return fail(config.error.empty() ? L"The helper-local neural add-on configuration was not enabled." :
                                           config.error);
    }
    // ReShade reads its INI while its proxy is loaded at process startup. If
    // this invocation repaired the helper-local contract, exit and let the
    // hook-free parent launch one fresh helper. Full exit is essential: the
    // proxy must release its log before the rendering process starts.
    if (config.changed) {
        if (!arguments->configurationRestarted)
            return neural_worker_detail::kConfigurationChangedExitCode;
        return fail(L"The helper-local neural configuration changed again after restart.");
    }
    // Match the player bootstrap ordering: enter the proxy on the main thread
    // before Media Foundation or a decoder can load the system DXGI path.
    const DetectedGpu gpu = DetectHighPerformanceGpu();
    MediaFoundationScope mediaFoundation;
    if (!mediaFoundation.Start()) return fail(L"The helper could not initialize its media runtime.");
    HWND renderWindow = CreateHiddenRenderWindow();
    if (!renderWindow) return fail(L"The helper could not create its hidden render window.");

    if (arguments->preflight) {
        PreflightPayload payload;
        const bool pumped = RunWithMessagePump([&] {
            payload = RunNeuralPreflightProbe(renderWindow, moduleDirectory, gpu);
        });
        DestroyWindow(renderWindow);
        if (!pumped) return fail(L"The helper could not create its preflight completion event.");
        return metadata.WritePreflight(payload) ? 0 : 3;
    }

    NeuralRenderRequest request = arguments->request;
    request.renderWindow = renderWindow;
    NeuralRenderResult result;
    const bool pumped = RunWithMessagePump([&] {
        OfflineNeuralRenderer renderer;
        result = renderer.Run(request, [&](const NeuralRenderProgress& progress) {
            metadata.WriteProgress(progress);
        });
    });
    DestroyWindow(renderWindow);
    if (!pumped) return fail(L"The helper could not create its render completion event.");
    result.jobId = request.jobId;
    return metadata.WriteResult(result) ? 0 : 3;
}
