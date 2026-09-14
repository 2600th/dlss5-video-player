#include "Log.h"
#include "NeuralPreflight.h"
#include "NeuralPreflightProbe.h"
#include "NeuralWorker.h"
#include "NeuralWorkerProtocol.h"
#include "ReShadeConfig.h"
#include "ResidentWorkerLoop.h"
#include "RuntimeLock.h"
#include "RuntimePolicy.h"

#include <windows.h>
#include <mfapi.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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

    // Answers Hello. Empty payload: it says only that this helper exists and
    // that its runtime came up, which is all the parent asked.
    bool WriteReady()
    {
        std::lock_guard lock(mutex_);
        return WriteMessage(handle_, WireKind::Ready, nullptr, 0);
    }

    // A VRAM point sample. Non-terminal, so it may be written with no job in
    // flight: the idle one is, and waits in the pipe for the next job's reader.
    bool WriteMemory(const MemorySample& sample)
    {
        const WireMemory wire = EncodeMemory(sample);
        std::lock_guard lock(mutex_);
        return WriteMessage(handle_, WireKind::Memory, &wire, sizeof(wire));
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

// ASCII-only narrowing for the log, which is a narrow stream. Every string
// that reaches it here is a fixed English diagnostic or a policy name, so a
// non-ASCII character is a bug rather than a translation.
std::string WideToNarrow(std::wstring_view value)
{
    std::string narrow;
    narrow.reserve(value.size());
    for (const wchar_t character : value)
        narrow.push_back(character < 128 ? static_cast<char>(character) : '?');
    return narrow;
}

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
// window's message pump alive for the proxy and DXGI. A resident helper wraps
// its whole session in one of these rather than one per job: the window and the
// device that outlive a job need the pump to outlive it too.
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

// One render, reported exactly as a single-shot helper reports it: progress and
// finalized segments while it runs, the cold-start timeline as soon as there is
// something to show, then the caller writes the result.
NeuralRenderResult RunOneJob(OfflineNeuralRenderer& renderer, MetadataWriter& metadata,
                             const NeuralRenderRequest& request,
                             const NeuralColdStartTimeline& helperPhases,
                             resident_helper::IdleVramPolicy idleVramPolicy,
                             std::stop_token stop)
{
    // Segment messages are written from the renderer's finalize thread; the
    // writer's lock keeps them from interleaving with progress messages.
    const int64_t frameDuration = static_cast<int64_t>(std::llround(10000000.0 / request.fps));
    NeuralSegmentSink segments;
    if (request.segmentFrames) {
        segments.onSegment = [&](const NeuralRenderSegment& segment) {
            metadata.WriteSegment(segment, frameDuration);
        };
    }
    NeuralRenderResult result = renderer.Run(request, [&](const NeuralRenderProgress& progress) {
        metadata.WriteProgress(progress);
    }, stop, segments, [&](const NeuralColdStartTimeline& rendered) {
        // The helper's two bootstrap phases and the renderer's three are one
        // timeline; the parent is told once per job, as soon as there is
        // something to show, so a cancel that kills this process before it can
        // write a result does not take the breakdown with it.
        //
        // `helperPhases` is empty for every job after the first one a resident
        // helper serves: the process start a reused job did not pay is not a
        // phase it may claim. A job whose every phase is absent sends no
        // timeline at all, because a timeline with nothing present is not a
        // measurement of anything.
        NeuralColdStartTimeline merged = helperPhases;
        merged.Merge(rendered);
        if (!merged.Empty()) metadata.WriteTimeline(merged);
    });
    result.jobId = request.jobId;
    // Sampled with the render finished and the device quiet, which is the
    // moment a helper that stays resident starts parking whatever it did not
    // give back. Written ahead of the result so a parent that judges the
    // result and stops reading still has it, and logged as well so a session
    // that nobody was pumping still leaves the number behind.
    const OfflineNeuralRenderer::MemoryFootprint parked = renderer.SampleMemoryFootprint();
    metadata.WriteMemory(MemorySample{MemoryStage::PostJob, idleVramPolicy, parked.featureArmed,
                                      parked.localVramMiB});
    LOG("Neural helper post-job VRAM: job=" << request.jobId
        << " localVramMiB=" << parked.localVramMiB
        << " featureArmed=" << (parked.featureArmed ? 1 : 0)
        << " idleVramPolicy=" << WideToNarrow(resident_helper::IdleVramPolicyName(idleVramPolicy)));
    return result;
}

// (size, last write time) of every file the embedded runtime lock names, as
// this process found them.
//
// The parent verifies the lock's hashes per job under its lease, so this is not
// that check repeated. It is the different question only a reused process has
// to ask: are the files still the ones THIS process mapped? The DLLs are loaded
// once, at startup, so a runtime replaced behind a resident helper would keep
// rendering with the old code under a cache key that claims the new digest -
// and the only cure is to exit and let the next job load the new files.
// Re-hashing 226 MB per job would cost more than the launch residency saves and
// would answer a question the parent already answered.
struct RuntimeStamps {
    std::vector<std::pair<uint64_t, int64_t>> files;
    friend bool operator==(const RuntimeStamps&, const RuntimeStamps&) = default;
};

RuntimeStamps SnapshotRuntimeStamps(const std::filesystem::path& directory)
{
    RuntimeStamps stamps;
    const RuntimeLock& lock = EmbeddedRuntimeLock();
    stamps.files.reserve(lock.entries.size());
    for (const RuntimeLockEntry& entry : lock.entries) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW((directory / entry.destination).c_str(), GetFileExInfoStandard, &data)) {
            // Absent now and absent at startup compare equal; a file that is
            // missing at all is the lock check's verdict to give, not this one's.
            stamps.files.emplace_back(0, 0);
            continue;
        }
        stamps.files.emplace_back(
            (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
            static_cast<int64_t>((static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                                 data.ftLastWriteTime.dwLowDateTime));
    }
    return stamps;
}

const char* ResidencyName(OfflineNeuralRenderer::Residency residency)
{
    switch (residency) {
        case OfflineNeuralRenderer::Residency::Initialized: return "initialized";
        case OfflineNeuralRenderer::Residency::FeatureReused: return "reuse";
        case OfflineNeuralRenderer::Residency::FeatureRecreated: return "feature-recreated";
    }
    return "unknown";
}

const char* ResidentExitName(resident_worker::ResidentExit exit)
{
    switch (exit) {
        case resident_worker::ResidentExit::Shutdown: return "shutdown";
        case resident_worker::ResidentExit::Idle: return "idle";
        case resident_worker::ResidentExit::ParentExited: return "parent-exited";
        case resident_worker::ResidentExit::Closed: return "command-pipe-closed";
        case resident_worker::ResidentExit::Malformed: return "malformed-command";
        case resident_worker::ResidentExit::WriteFailed: return "metadata-write-failed";
        case resident_worker::ResidentExit::JobInvalidated: return "job-invalidated";
    }
    return "unknown";
}

// Serves jobs on the parent's command channel until one of them invalidates the
// process, the parent asks it to stop, the parent dies or the idle budget runs
// out. Everything the parent cannot see into - the device, the NGX instance and
// its feature, the encoder's helper directory - lives in `renderer_` and
// survives between jobs; everything the parent CAN change behind a running
// process is re-checked per job, because a reused process must not assume the
// runtime it loaded or the settings its proxy read are still the ones on disk.
//
// `idleVramPolicy_` is the one thing here that is fixed at launch and never
// re-read: it decides what happens to the feature workset between jobs, and a
// process that changed arms mid-life would report two idle samples that cannot
// be compared with each other or with anything else.
class ResidentRunner {
public:
    ResidentRunner(MetadataWriter& metadata, HWND window, std::filesystem::path moduleDirectory,
                   NeuralColdStartTimeline helperPhases, RuntimeStamps runtime, std::string settings,
                   resident_helper::IdleVramPolicy idleVramPolicy)
        : metadata_(metadata), window_(window), moduleDirectory_(std::move(moduleDirectory)),
          helperPhases_(std::move(helperPhases)), runtime_(std::move(runtime)),
          settings_(std::move(settings)), idleVramPolicy_(idleVramPolicy) {}

    bool Ready() { return metadata_.WriteReady(); }

    // A job the protocol refuses. Reported as a failed result so the parent can
    // fail that render rather than waiting for one that will never arrive, and
    // the helper stays: a malformed job is the parent's mistake, not evidence
    // that this process is broken.
    bool Refuse(std::wstring_view detail)
    {
        NeuralRenderResult refused;
        refused.failure = NeuralRenderFailure::Protocol;
        refused.detail = std::wstring(detail);
        LOG("Resident helper refused a job: " << WideToNarrow(detail));
        return metadata_.WriteResult(refused);
    }

    resident_worker::JobOutcome Job(std::span<const std::wstring> argv, std::stop_token stop)
    {
        std::vector<std::wstring_view> values(argv.begin(), argv.end());
        const auto parsed = neural_worker_detail::ParseWorkerArguments(values);
        // ParseWorkerArguments is the single definition of what a job is and the
        // single place that validates one; residency changed how a job arrives,
        // not what a job means. A vector it refuses, one that asks for a probe,
        // and one that asks for residency of its own are all protocol failures
        // rather than jobs to attempt with whatever decoded.
        if (!parsed || parsed->preflight || parsed->command) {
            return Refuse(L"The helper rejected the job's argument vector.")
                ? resident_worker::JobOutcome::Refused
                : resident_worker::JobOutcome::WriteFailed;
        }
        if (const std::wstring invalid = CheckSessionInvariants(); !invalid.empty()) {
            LOG("Resident helper cannot serve job " << parsed->request.jobId << ": "
                << WideToNarrow(invalid));
            const NeuralRenderResult failed = FailedResult(invalid, parsed->request.jobId);
            return metadata_.WriteResult(failed) ? resident_worker::JobOutcome::Invalidated
                                                 : resident_worker::JobOutcome::WriteFailed;
        }
        NeuralRenderRequest request = parsed->request;
        request.renderWindow = window_;
        // Only the first job a resident helper serves paid the process start.
        const NeuralColdStartTimeline base =
            served_ ? NeuralColdStartTimeline{} : helperPhases_;
        NeuralRenderResult result = RunOneJob(renderer_, metadata_, request, base, idleVramPolicy_, stop);
        ++served_;
        LOG("Resident helper served job " << request.jobId << " (" << served_ << " this process) as "
            << ResidencyName(renderer_.LastResidency()) << ": ok=" << (result.ok ? 1 : 0)
            << " cancelled=" << (result.cancelled ? 1 : 0) << " frames=" << result.frameCount
            << " peakVramMiB=" << result.timing.peakLocalVramMiB);
        if (!metadata_.WriteResult(result)) return resident_worker::JobOutcome::WriteFailed;
        if (result.cancelled) return resident_worker::JobOutcome::Cancelled;
        // A failed job leaves this process unfit to serve another. The session
        // log's failure lines are scanned session-wide, so one job that logged
        // an NR failure would fail every later job in this process for a reason
        // that is not theirs; a removed device or a stalled GPU leaves state no
        // reset here can describe. The parent relaunches, exactly as it does
        // after the idle exit.
        if (!result.ok) return resident_worker::JobOutcome::Invalidated;
        if (!renderer_.ReusableForAnotherJob()) {
            LOG("Resident helper retiring: its session log has outgrown the evidence read.");
            return resident_worker::JobOutcome::Invalidated;
        }
        return resident_worker::JobOutcome::Completed;
    }

    // The idle grace elapsed with no new job. Under KeepFeature this only
    // measures what the process is parking; under FreeFeature it hands the
    // feature-18 workset back first, so the number describes what the policy
    // actually left behind rather than what it intended to.
    //
    // Both arms report the same way, on the metadata pipe and in the log. The
    // pipe copy reaches the next job's receipt, which is the job that pays for
    // whatever was given back; the log copy is what a helper that exits on the
    // idle timeout instead leaves behind, because nobody is pumping the pipe
    // between jobs.
    void Idle()
    {
        const std::string policyName = WideToNarrow(resident_helper::IdleVramPolicyName(idleVramPolicy_));
        OfflineNeuralRenderer::MemoryFootprint parked;
        if (idleVramPolicy_ == resident_helper::IdleVramPolicy::FreeFeature) {
            const OfflineNeuralRenderer::IdleFeatureRelease observed =
                renderer_.ReleaseIdleFeatureMemory();
            parked = observed.after;
            const uint64_t freed = observed.before.localVramMiB > observed.after.localVramMiB
                ? observed.before.localVramMiB - observed.after.localVramMiB : 0;
            // `observed=` is what the adapter said, not a claim that the
            // release worked: DLSS is documented to keep feature memory, and a
            // runtime that declines to give it back has to be recorded as
            // having declined rather than reported as a success.
            LOG("Resident helper idle VRAM: policy=" << policyName
                << " beforeMiB=" << observed.before.localVramMiB
                << " localVramMiB=" << observed.after.localVramMiB
                << " freedMiB=" << freed
                << " released=" << (observed.released ? 1 : 0)
                << " featureArmed=" << (parked.featureArmed ? 1 : 0)
                << " observed=" << (freed ? "freed" : "not-freed"));
        } else {
            parked = renderer_.SampleMemoryFootprint();
            LOG("Resident helper idle VRAM: policy=" << policyName
                << " localVramMiB=" << parked.localVramMiB
                << " featureArmed=" << (parked.featureArmed ? 1 : 0));
        }
        metadata_.WriteMemory(MemorySample{MemoryStage::Idle, idleVramPolicy_, parked.featureArmed,
                                           parked.localVramMiB});
    }

private:
    // What the parent may have changed since this process started. Run per job
    // under the parent's lease, which is also when the parent rewrote the
    // settings and re-verified the lock.
    std::wstring CheckSessionInvariants() const
    {
        const std::filesystem::path ini = moduleDirectory_ / L"ReShade.ini";
        const ConfigUpdate config = ConfigureNeuralAddon(ini, true);
        if (!config.ok || !config.addonEnabled) {
            return config.error.empty()
                ? std::wstring(L"The helper-local neural add-on configuration was not enabled.")
                : config.error;
        }
        // ReShade read this file when its proxy loaded, before the entry point
        // ran, so a repair now cannot reach the running session.
        if (config.changed) {
            return L"The helper-local neural add-on contract had to be repaired, which a resident "
                   L"helper cannot adopt: its proxy read the file at process start.";
        }
        std::wstring error;
        const auto settings = ReadNeuralAddonSettingsSnapshot(ini, &error);
        if (!settings) {
            return error.empty() ? std::wstring(L"The helper could not read its neural settings.")
                                 : error;
        }
        // Same reason, one level up: the settings themselves are read once by
        // the add-on, so a render that wants different ones needs a new process.
        // The parent's residency key covers this; reaching it means that key is
        // missing an input, and a wrong render is worse than a refused one.
        if (*settings != settings_) {
            return L"The neural settings changed under a resident helper, which read them when its "
                   L"proxy loaded.";
        }
        if (SnapshotRuntimeStamps(moduleDirectory_) != runtime_) {
            return L"The staged neural runtime changed under a resident helper, which has the "
                   L"previous files mapped.";
        }
        return {};
    }

    MetadataWriter& metadata_;
    HWND window_{};
    std::filesystem::path moduleDirectory_;
    NeuralColdStartTimeline helperPhases_;
    RuntimeStamps runtime_;
    std::string settings_;
    // Fixed at launch: exactly one arm is in force per helper process.
    resident_helper::IdleVramPolicy idleVramPolicy_{resident_helper::kDefaultIdleVramPolicy};
    // The device, the NGX instance, its feature-18 workset and the encoder's
    // helper lookup, kept for the process. Constructed here and destroyed with
    // this runner, which the session tears down before the render window its
    // swapchain is attached to.
    OfflineNeuralRenderer renderer_;
    uint64_t served_{};
};

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

    // Resident mode. The command handle's presence is the whole switch: without
    // it this helper does exactly what every helper before it did, one job from
    // argv and then exit, which is the path the benchmark harness drives.
    if (arguments->command) {
        std::wstring settingsError;
        const auto settings = ReadNeuralAddonSettingsSnapshot(moduleDirectory / L"ReShade.ini",
                                                              &settingsError);
        if (!settings) {
            return fail(settingsError.empty()
                ? std::wstring(L"The helper could not read its neural settings.") : settingsError);
        }
        LOG("Resident helper session starting with idleVramPolicy="
            << WideToNarrow(resident_helper::IdleVramPolicyName(arguments->idleVramPolicy)));
        int exitCode = 0;
        // One pump for the whole session: the window and the device outlive any
        // single job, so the thread that owns them has to keep pumping between
        // jobs as well as during them.
        const bool pumped = RunWithMessagePump([&] {
            ResidentRunner runner(metadata, renderWindow, moduleDirectory, helperPhases,
                                  SnapshotRuntimeStamps(moduleDirectory), *settings,
                                  arguments->idleVramPolicy);
            resident_worker::CommandChannel channel(arguments->command, arguments->parentProcess);
            const resident_worker::ResidentExit reason =
                resident_worker::RunResidentLoop(channel, runner);
            LOG("Resident helper session ended: " << ResidentExitName(reason));
            switch (reason) {
                case resident_worker::ResidentExit::WriteFailed: exitCode = 3; break;
                case resident_worker::ResidentExit::Malformed: exitCode = 4; break;
                default: exitCode = 0; break;
            }
        });
        // After the runner, so the device and its swapchain are gone first.
        DestroyWindow(renderWindow);
        if (!pumped) return fail(L"The helper could not create its session completion event.");
        return exitCode;
    }

    NeuralRenderRequest request = arguments->request;
    request.renderWindow = renderWindow;
    NeuralRenderResult result;
    const bool pumped = RunWithMessagePump([&] {
        OfflineNeuralRenderer renderer;
        // A single-shot helper exits rather than idling, so its policy is the
        // default whatever the parent thinks: it keeps the feature until the
        // process goes, and the post-job sample says what that was worth.
        result = RunOneJob(renderer, metadata, request, helperPhases,
                           resident_helper::kDefaultIdleVramPolicy, {});
    });
    DestroyWindow(renderWindow);
    if (!pumped) return fail(L"The helper could not create its render completion event.");
    return metadata.WriteResult(result) ? 0 : 3;
}
