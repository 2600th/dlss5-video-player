#include "NeuralPreflight.h"
#include "NeuralPreflightProbe.h"
#include "NeuralWorker.h"
#include "NeuralWorkerProtocol.h"
#include "ReShadeConfig.h"
#include "RuntimePolicy.h"

#include <windows.h>
#include <mfapi.h>

#include <chrono>
#include <cmath>
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

// Process creation to now. The loader window - the antivirus scan of the
// runtime tree and the ReShade proxy's own load, since the proxy is this
// executable's dxgi import and is resolved before the entry point - cannot be
// timed from inside the process any other way. The kernel's creation stamp and
// the system clock are the only pair that spans it, and both are the same
// clock, so the difference is a real interval even though neither end is
// monotonic.
std::optional<std::chrono::microseconds> ElapsedSinceProcessStart()
{
    FILETIME creation{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exited, &kernel, &user)) return std::nullopt;
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    const auto packed = [](const FILETIME& value) {
        return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    const uint64_t started = packed(creation);
    const uint64_t current = packed(now);
    if (current < started) return std::nullopt;
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::duration<int64_t, std::ratio<1, 10000000>>(static_cast<int64_t>(current - started)));
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

    bool WriteSegment(const NeuralRenderSegment& segment, int64_t frameDuration100ns)
    {
        const std::vector<std::byte> payload = EncodeSegment(segment, frameDuration100ns);
        std::lock_guard lock(mutex_);
        return WriteMessage(handle_, WireKind::Segment, payload.data(), static_cast<uint32_t>(payload.size()));
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

    bool WriteTimeline(const NeuralColdStartTimeline& timeline)
    {
        const WireTimeline wire = EncodeTimeline(timeline);
        std::lock_guard lock(mutex_);
        return WriteMessage(handle_, WireKind::Timeline, &wire, sizeof(wire));
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
    const auto entered = std::chrono::steady_clock::now();
    const auto loaderWindow = ElapsedSinceProcessStart();
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
    NeuralColdStartTimeline helperPhases;
    if (loaderWindow) helperPhases.Record(NeuralColdStartPhase::HelperStart, *loaderWindow);
    // Everything from the entry point to a render window: the add-on contract
    // check, the adapter query, Media Foundation and the window itself. The
    // helper cannot report a boundary before this one, so a probe or a repair
    // exit reports nothing rather than a partial cold start.
    helperPhases.Record(NeuralColdStartPhase::RuntimeReady,
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - entered));

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
    // Segment messages are written from the renderer's finalize thread; the
    // writer's lock keeps them from interleaving with progress messages.
    const int64_t frameDuration = static_cast<int64_t>(std::llround(10000000.0 / request.fps));
    NeuralSegmentSink segments;
    if (request.segmentFrames) {
        segments.onSegment = [&](const NeuralRenderSegment& segment) {
            metadata.WriteSegment(segment, frameDuration);
        };
    }
    const bool pumped = RunWithMessagePump([&] {
        OfflineNeuralRenderer renderer;
        result = renderer.Run(request, [&](const NeuralRenderProgress& progress) {
            metadata.WriteProgress(progress);
        }, {}, segments, [&](const NeuralColdStartTimeline& rendered) {
            // The helper's two bootstrap phases and the renderer's three are
            // one timeline; the parent is told once, as soon as there is
            // something to show, so a cancel that kills this process before it
            // can write a result does not take the breakdown with it.
            NeuralColdStartTimeline merged = helperPhases;
            merged.Merge(rendered);
            metadata.WriteTimeline(merged);
        });
    });
    DestroyWindow(renderWindow);
    if (!pumped) return fail(L"The helper could not create its render completion event.");
    result.jobId = request.jobId;
    return metadata.WriteResult(result) ? 0 : 3;
}
