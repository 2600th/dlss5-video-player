#include "NeuralWorker.h"
#include "NeuralWorkerProtocol.h"
#include "ChildProcess.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace neural_worker_protocol;

namespace {

constexpr std::wstring_view kWorkerMode = L"--neural-worker";
constexpr std::wstring_view kPreflightMode = L"--neural-preflight";
constexpr std::wstring_view kRestartedFlag = L"--configuration-restarted";

std::wstring QuoteArgument(std::wstring_view value)
{
    std::wstring quoted(1, L'"');
    size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(slashes, L'\\');
            quoted.push_back(character);
        }
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring MakeCommandLine(const std::filesystem::path& executable,
                             const std::vector<std::wstring>& arguments)
{
    std::wstring command = QuoteArgument(executable.wstring());
    for (const std::wstring& argument : arguments) {
        command.push_back(L' ');
        command += QuoteArgument(argument);
    }
    return command;
}

HANDLE CreateKillOnCloseJob()
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

void KillAndWait(HANDLE job, HANDLE process)
{
    if (job) TerminateJobObject(job, ERROR_PROCESS_ABORTED);
    // The process can be suspended before assignment fails. Terminating it
    // directly as well prevents that unassigned process from escaping.
    if (process) TerminateProcess(process, ERROR_PROCESS_ABORTED);
    if (process) WaitForSingleObject(process, 2000);
}

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

std::string Narrow(std::wstring_view text)
{
    std::string narrow;
    narrow.reserve(text.size());
    for (const wchar_t character : text) {
        if (character > 0x7F) return {};
        narrow.push_back(static_cast<char>(character));
    }
    return narrow;
}

std::wstring HandleText(HANDLE handle)
{
    return std::to_wstring(reinterpret_cast<uintptr_t>(handle));
}

class MetadataReader {
public:
    MetadataReader(OfflineNeuralRenderer::ProgressCallback progress, NeuralSegmentSink segments)
        : progress_(std::move(progress)), segments_(std::move(segments)) {}

    bool ReadAvailable(HANDLE pipe)
    {
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                const DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE) return true;
                malformed_ = true;
                return false;
            }
            if (!available) return true;
            std::array<std::byte, 4096> chunk{};
            const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(chunk.size()));
            DWORD read = 0;
            if (!ReadFile(pipe, chunk.data(), wanted, &read, nullptr) || read == 0) {
                malformed_ = true;
                return false;
            }
            if (!Push(std::span<const std::byte>(chunk.data(), read))) return false;
        }
    }

    // The same decoder, fed from memory instead of the pipe.
    bool Push(std::span<const std::byte> chunk)
    {
        if (bytes_.size() + chunk.size() > kMaximumPayloadBytes * 2u) {
            malformed_ = true;
            return false;
        }
        bytes_.insert(bytes_.end(), chunk.begin(), chunk.end());
        return Consume();
    }

    bool Complete() const { return !malformed_ && bytes_.empty() && result_.has_value(); }
    bool PreflightComplete() const { return !malformed_ && bytes_.empty() && preflight_.has_value(); }
    bool Malformed() const { return malformed_; }
    const NeuralRenderResult& Result() const { return *result_; }
    const PreflightPayload& Preflight() const { return *preflight_; }

private:
    bool Consume()
    {
        size_t offset = 0;
        while (bytes_.size() - offset >= sizeof(WireHeader)) {
            WireHeader header{};
            std::memcpy(&header, bytes_.data() + offset, sizeof(header));
            if (header.magic != kMagic || header.version != kVersion ||
                header.payloadBytes > kMaximumPayloadBytes ||
                (header.kind != static_cast<uint16_t>(WireKind::Progress) &&
                 header.kind != static_cast<uint16_t>(WireKind::Result) &&
                 header.kind != static_cast<uint16_t>(WireKind::Preflight) &&
                 header.kind != static_cast<uint16_t>(WireKind::Segment))) {
                malformed_ = true;
                return false;
            }
            const size_t messageBytes = sizeof(header) + static_cast<size_t>(header.payloadBytes);
            if (bytes_.size() - offset < messageBytes) break;
            const std::span<const std::byte> payload(bytes_.data() + offset + sizeof(header), header.payloadBytes);
            // A terminal message (result or preflight) must be the last one.
            if (result_.has_value() || preflight_.has_value()) { malformed_ = true; return false; }
            switch (static_cast<WireKind>(header.kind)) {
                case WireKind::Progress: {
                    const auto progress = DecodeProgress(payload);
                    if (!progress) { malformed_ = true; return false; }
                    if (progress_) progress_(*progress);
                    break;
                }
                case WireKind::Result: {
                    auto result = DecodeResult(payload);
                    if (!result) { malformed_ = true; return false; }
                    result_ = std::move(result);
                    break;
                }
                case WireKind::Preflight: {
                    auto preflight = DecodePreflight(payload);
                    if (!preflight) { malformed_ = true; return false; }
                    preflight_ = std::move(preflight);
                    break;
                }
                case WireKind::Segment: {
                    auto segment = DecodeSegment(payload);
                    if (!segment) { malformed_ = true; return false; }
                    // Indices arrive in order. A repeated 0 is the helper
                    // restarting the sequence: every earlier file is gone.
                    if (segment->index == 0) {
                        if (lastSegmentIndex_ && segments_.onRestart) segments_.onRestart();
                    } else if (!lastSegmentIndex_ || segment->index != *lastSegmentIndex_ + 1) {
                        malformed_ = true;
                        return false;
                    }
                    lastSegmentIndex_ = segment->index;
                    if (segments_.onSegment) segments_.onSegment(*segment);
                    break;
                }
            }
            offset += messageBytes;
        }
        if (offset) bytes_.erase(bytes_.begin(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
        return true;
    }

    OfflineNeuralRenderer::ProgressCallback progress_;
    NeuralSegmentSink segments_;
    std::optional<uint64_t> lastSegmentIndex_;
    std::vector<std::byte> bytes_;
    std::optional<NeuralRenderResult> result_;
    std::optional<PreflightPayload> preflight_;
    bool malformed_{};
};

std::wstring ErrorDetail(std::wstring_view operation)
{
    return std::wstring(operation) + L" (Win32 error " + std::to_wstring(GetLastError()) + L").";
}

struct LaunchOutcome {
    bool launched{};
    bool cancelled{};
    DWORD exitCode{static_cast<DWORD>(-1)};
    std::wstring detail;
};

// ReShade truncates its log when the proxy loads and rotates to ReShade.log1
// when a previous file is still held open. Retiring both files before a launch
// keeps the helper on ReShade.log in the normal case; it is best effort only,
// because Windows refuses to delete a file another process holds without
// FILE_SHARE_DELETE. The helper therefore also selects its log by session
// (ResolveNeuralRuntimeLogPath) instead of trusting the name.
void RemoveStaleRuntimeLogs(const std::filesystem::path& runtimeDirectory)
{
    if (runtimeDirectory.empty()) return;
    std::error_code error;
    std::filesystem::remove(runtimeDirectory / L"ReShade.log", error);
    std::filesystem::remove(runtimeDirectory / L"ReShade.log1", error);
}

// Launches the helper with the metadata pipe (and optional inheritable pause
// event), pumps its messages into `reader`, and returns once it exits or the
// caller cancels. The job object kills the whole helper tree on close.
LaunchOutcome LaunchHelper(const std::filesystem::path& executable,
                           const std::function<std::vector<std::wstring>(HANDLE metadata, HANDLE pause)>& arguments,
                           HANDLE pauseEvent, MetadataReader& reader, std::stop_token stop)
{
    LaunchOutcome outcome;
    RemoveStaleRuntimeLogs(executable.parent_path());
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE metadataRead = nullptr;
    HANDLE metadataWrite = nullptr;
    if (!CreatePipe(&metadataRead, &metadataWrite, &security, 0) ||
        !SetHandleInformation(metadataRead, HANDLE_FLAG_INHERIT, 0)) {
        if (metadataRead) CloseHandle(metadataRead);
        if (metadataWrite) CloseHandle(metadataWrite);
        outcome.detail = ErrorDetail(L"Creating the neural helper metadata pipe failed");
        return outcome;
    }
    HANDLE inheritedPause = nullptr;
    if (pauseEvent && !DuplicateHandle(GetCurrentProcess(), pauseEvent, GetCurrentProcess(), &inheritedPause,
                                       SYNCHRONIZE, TRUE, 0)) {
        CloseHandle(metadataRead); CloseHandle(metadataWrite);
        outcome.detail = ErrorDetail(L"Sharing the neural helper pause event failed");
        return outcome;
    }
    HANDLE job = CreateKillOnCloseJob();
    if (!job) {
        CloseHandle(metadataRead); CloseHandle(metadataWrite);
        if (inheritedPause) CloseHandle(inheritedPause);
        outcome.detail = ErrorDetail(L"Creating the neural helper job failed");
        return outcome;
    }

    std::array<HANDLE, 2> inherited{metadataWrite, inheritedPause};
    const DWORD inheritedCount = inheritedPause ? 2 : 1;
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::byte> attributes(attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    const bool attributeListInitialized = attributeBytes &&
        InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes) != FALSE;
    if (!attributeListInitialized || !UpdateProcThreadAttribute(attributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), inheritedCount * sizeof(HANDLE), nullptr, nullptr)) {
        if (attributeListInitialized) DeleteProcThreadAttributeList(attributeList);
        CloseHandle(job); CloseHandle(metadataRead); CloseHandle(metadataWrite);
        if (inheritedPause) CloseHandle(inheritedPause);
        outcome.detail = ErrorDetail(L"Restricting neural helper handle inheritance failed");
        return outcome;
    }

    std::wstring command = MakeCommandLine(executable, arguments(metadataWrite, inheritedPause));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION process{};
    const ChildProcessErrorModeScope quietLaunchFailures;
    const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
        executable.parent_path().c_str(), &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributeList);
    CloseHandle(metadataWrite);
    if (inheritedPause) CloseHandle(inheritedPause);
    if (!created) {
        CloseHandle(job); CloseHandle(metadataRead);
        outcome.detail = ErrorDetail(L"Starting the isolated neural helper failed");
        return outcome;
    }
    const bool assigned = AssignProcessToJobObject(job, process.hProcess) != FALSE;
    const DWORD resumed = assigned ? ResumeThread(process.hThread) : static_cast<DWORD>(-1);
    CloseHandle(process.hThread);
    if (!assigned || resumed == static_cast<DWORD>(-1)) {
        KillAndWait(job, process.hProcess);
        CloseHandle(process.hProcess); CloseHandle(job); CloseHandle(metadataRead);
        outcome.detail = assigned ? ErrorDetail(L"Resuming the isolated neural helper failed") :
                                    L"The isolated neural helper could not be assigned to its job.";
        return outcome;
    }
    outcome.launched = true;

    for (;;) {
        if (!reader.ReadAvailable(metadataRead)) break;
        const DWORD wait = WaitForSingleObject(process.hProcess, 20);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_TIMEOUT) break;
        if (stop.stop_requested()) {
            outcome.cancelled = true;
            KillAndWait(job, process.hProcess);
            break;
        }
    }
    reader.ReadAvailable(metadataRead);
    GetExitCodeProcess(process.hProcess, &outcome.exitCode);
    // The old process is signalled and all its handles are closed here. Waiting
    // for full exit releases ReShade.log before the next proxy loads; overlapping
    // helpers otherwise put the real evidence in ReShade.log1.
    CloseHandle(metadataRead); CloseHandle(process.hProcess); CloseHandle(job);
    return outcome;
}

bool ValidRequest(const NeuralRenderRequest& request)
{
    if (request.sourcePath.empty() || request.stagingVideoPath.empty() || !request.width || !request.height ||
        !std::isfinite(request.fps) || request.fps <= 0.0 || !std::isfinite(request.durationSeconds) ||
        request.durationSeconds <= 0.0) return false;
    if (request.range.start100ns < 0 || request.range.end100ns < 0) return false;
    if (request.range.end100ns && request.range.end100ns <= request.range.start100ns) return false;
    return true;
}

NeuralRenderResult RunNeuralWorkerAttempt(const std::filesystem::path& executable,
                                          const NeuralRenderRequest& request,
                                          const OfflineNeuralRenderer::ProgressCallback& progress,
                                          const NeuralSegmentSink& segments,
                                          std::stop_token stop, bool configurationRestarted)
{
    NeuralRenderResult result;
    result.jobId = request.jobId;
    if (stop.stop_requested()) {
        result.cancelled = true;
        result.failure = NeuralRenderFailure::Cancelled;
        result.detail = L"Neural rendering was cancelled before the helper started.";
        return result;
    }
    MetadataReader reader(progress, segments);
    const LaunchOutcome launch = LaunchHelper(executable,
        [&](HANDLE metadata, HANDLE pause) {
            return neural_worker_detail::BuildWorkerArguments(request, metadata, pause, configurationRestarted);
        }, request.pauseEvent, reader, stop);
    if (!launch.launched) {
        result.failure = NeuralRenderFailure::Protocol;
        result.detail = launch.detail;
        return result;
    }
    if (launch.cancelled || stop.stop_requested()) {
        result.cancelled = true;
        result.failure = NeuralRenderFailure::Cancelled;
        result.detail = L"Neural rendering was cancelled.";
        return result;
    }
    if (launch.exitCode == neural_worker_detail::kConfigurationChangedExitCode &&
        !configurationRestarted && !reader.Malformed()) {
        return RunNeuralWorkerAttempt(executable, request, progress, segments, stop, true);
    }
    if (launch.exitCode != 0) {
        result.failure = NeuralRenderFailure::WorkerCrashed;
        result.detail = L"The isolated neural helper exited with code " + std::to_wstring(launch.exitCode) +
                        L" before producing a result.";
        return result;
    }
    if (!reader.Complete()) {
        result.failure = NeuralRenderFailure::Protocol;
        result.detail = reader.Malformed() ? L"The isolated neural helper returned malformed metadata." :
                                             L"The isolated neural helper returned incomplete metadata.";
        return result;
    }
    result = reader.Result();
    if (result.jobId != request.jobId) {
        NeuralRenderResult mismatch;
        mismatch.jobId = request.jobId;
        mismatch.failure = NeuralRenderFailure::Identity;
        mismatch.detail = L"The isolated neural helper reported a result for a different job.";
        return mismatch;
    }
    return result;
}

} // namespace

std::vector<std::wstring> neural_worker_detail::BuildWorkerArguments(
    const NeuralRenderRequest& request, HANDLE metadata, HANDLE pauseEvent, bool configurationRestarted)
{
    std::vector<std::wstring> arguments{
        std::wstring(kWorkerMode), L"--metadata-handle", HandleText(metadata),
        L"--source", request.sourcePath.wstring(), L"--staging", request.stagingVideoPath.wstring(),
        L"--width", std::to_wstring(request.width), L"--height", std::to_wstring(request.height),
        L"--fps", std::to_wstring(request.fps), L"--duration-100ns",
        std::to_wstring(static_cast<int64_t>(std::llround(request.durationSeconds * 10000000.0))),
        L"--job-id", std::to_wstring(request.jobId),
        L"--range-start-100ns", std::to_wstring(request.range.start100ns),
        L"--range-end-100ns", std::to_wstring(request.range.end100ns),
        L"--preroll-frames", std::to_wstring(request.prerollFrames),
        L"--frame-retry-limit", std::to_wstring(request.frameRetryLimit)};
    const std::string guides = CanonicalGuideControls(request.guides);
    arguments.emplace_back(L"--guides");
    arguments.emplace_back(guides.begin(), guides.end());
    if (request.segmentFrames) {
        arguments.emplace_back(L"--segment-frames");
        arguments.emplace_back(std::to_wstring(request.segmentFrames));
    }
    // Absent means the CPU conversion inside ffmpeg, which is what every earlier
    // helper did, so an older parent and a newer helper still agree.
    if (request.gpuColorConversion) {
        arguments.emplace_back(L"--gpu-color-conversion");
        arguments.emplace_back(L"1");
    }
    // Absent means preset 7 (slowest/highest quality), so an older parent and a
    // newer helper still agree.
    if (request.nvencPreset != 7) {
        arguments.emplace_back(L"--nvenc-preset");
        arguments.emplace_back(std::to_wstring(request.nvencPreset));
    }
    // Absent means GPU source conversion (NV12 decode + GPU convert to BGRA), so an
    // older parent and a newer helper still agree.
    if (!request.gpuSourceConversion) {
        arguments.emplace_back(L"--gpu-source-conversion");
        arguments.emplace_back(L"0");
    }
    if (pauseEvent) {
        arguments.emplace_back(L"--pause-event");
        arguments.emplace_back(HandleText(pauseEvent));
    }
    if (configurationRestarted) arguments.emplace_back(kRestartedFlag);
    return arguments;
}

std::vector<std::wstring> neural_worker_detail::BuildPreflightArguments(HANDLE metadata, bool configurationRestarted)
{
    std::vector<std::wstring> arguments{std::wstring(kPreflightMode), L"--metadata-handle", HandleText(metadata)};
    if (configurationRestarted) arguments.emplace_back(kRestartedFlag);
    return arguments;
}

std::optional<neural_worker_detail::WorkerArguments> neural_worker_detail::ParseWorkerArguments(
    std::span<const std::wstring_view> arguments)
{
    if (arguments.size() < 2) return std::nullopt;
    WorkerArguments parsed;
    if (arguments[1] == kPreflightMode) parsed.preflight = true;
    else if (arguments[1] != kWorkerMode) return std::nullopt;
    size_t end = arguments.size();
    if (end > 2 && arguments[end - 1] == kRestartedFlag) {
        parsed.configurationRestarted = true;
        --end;
    }
    if (((end - 2) % 2) != 0) return std::nullopt;

    enum Key { Metadata, Source, Staging, Width, Height, Fps, Duration, JobId, RangeStart, RangeEnd, Preroll,
               RetryLimit, Guides, SegmentFrames, PauseEvent, GpuColorConversion, NvencPreset,
               GpuSourceConversion, KeyCount };
    constexpr std::array<std::wstring_view, KeyCount> names{
        L"--metadata-handle", L"--source", L"--staging", L"--width", L"--height", L"--fps", L"--duration-100ns",
        L"--job-id", L"--range-start-100ns", L"--range-end-100ns", L"--preroll-frames", L"--frame-retry-limit",
        L"--guides", L"--segment-frames", L"--pause-event", L"--gpu-color-conversion", L"--nvenc-preset",
        L"--gpu-source-conversion"};
    std::array<std::optional<std::wstring_view>, KeyCount> values{};
    for (size_t index = 2; index < end; index += 2) {
        const auto found = std::find(names.begin(), names.end(), arguments[index]);
        if (found == names.end()) return std::nullopt;
        auto& slot = values[static_cast<size_t>(found - names.begin())];
        if (slot.has_value()) return std::nullopt;
        slot = arguments[index + 1];
    }
    uint64_t rawHandle = 0;
    if (!values[Metadata] || !ParseUnsigned(*values[Metadata], rawHandle) || !rawHandle) return std::nullopt;
    parsed.metadata = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(rawHandle));
    if (parsed.preflight) {
        for (size_t key = 0; key < KeyCount; ++key) {
            if (key != Metadata && values[key].has_value()) return std::nullopt;
        }
        return parsed;
    }
    constexpr std::array<Key, 12> required{Source, Staging, Width, Height, Fps, Duration, JobId, RangeStart,
                                           RangeEnd, Preroll, RetryLimit, Guides};
    for (const Key key : required) if (!values[key]) return std::nullopt;
    if (values[Source]->empty() || values[Staging]->empty()) return std::nullopt;
    uint64_t width = 0, height = 0, duration = 0, jobId = 0, rangeStart = 0, rangeEnd = 0, preroll = 0,
             retryLimit = 0;
    double fps = 0.0;
    if (!ParseUnsigned(*values[Width], width) || !ParseUnsigned(*values[Height], height) ||
        !ParseUnsigned(*values[Duration], duration) || !ParseDouble(*values[Fps], fps) ||
        !ParseUnsigned(*values[JobId], jobId) || !ParseUnsigned(*values[RangeStart], rangeStart) ||
        !ParseUnsigned(*values[RangeEnd], rangeEnd) || !ParseUnsigned(*values[Preroll], preroll) ||
        !ParseUnsigned(*values[RetryLimit], retryLimit) || !width || !height || width > UINT32_MAX ||
        height > UINT32_MAX || !duration || duration > INT64_MAX || fps <= 0.0 || rangeStart > INT64_MAX ||
        rangeEnd > INT64_MAX || preroll > UINT32_MAX || retryLimit > UINT32_MAX) return std::nullopt;
    const auto guides = ParseGuideControls(Narrow(*values[Guides]));
    if (!guides) return std::nullopt;
    NeuralRenderRequest& request = parsed.request;
    request.sourcePath = *values[Source];
    request.stagingVideoPath = *values[Staging];
    request.width = static_cast<uint32_t>(width);
    request.height = static_cast<uint32_t>(height);
    request.fps = fps;
    request.durationSeconds = static_cast<double>(duration) / 10000000.0;
    request.jobId = jobId;
    request.range = {static_cast<int64_t>(rangeStart), static_cast<int64_t>(rangeEnd)};
    request.prerollFrames = static_cast<uint32_t>(preroll);
    request.frameRetryLimit = static_cast<uint32_t>(retryLimit);
    request.guides = *guides;
    // Optional: absent means one output file for the whole range.
    if (values[SegmentFrames]) {
        uint64_t segmentFrames = 0;
        if (!ParseUnsigned(*values[SegmentFrames], segmentFrames) || segmentFrames > UINT32_MAX)
            return std::nullopt;
        request.segmentFrames = static_cast<uint32_t>(segmentFrames);
    }
    if (values[GpuColorConversion]) {
        uint64_t enabled = 0;
        if (!ParseUnsigned(*values[GpuColorConversion], enabled) || enabled > 1) return std::nullopt;
        request.gpuColorConversion = enabled != 0;
    }
    if (values[GpuSourceConversion]) {
        uint64_t enabled = 0;
        if (!ParseUnsigned(*values[GpuSourceConversion], enabled) || enabled > 1) return std::nullopt;
        request.gpuSourceConversion = enabled != 0;
    }
    if (values[NvencPreset]) {
        uint64_t preset = 0;
        if (!ParseUnsigned(*values[NvencPreset], preset) || !preset || preset > 7) return std::nullopt;
        request.nvencPreset = static_cast<uint32_t>(preset);
    }
    if (values[PauseEvent]) {
        uint64_t rawPause = 0;
        if (!ParseUnsigned(*values[PauseEvent], rawPause) || !rawPause) return std::nullopt;
        request.pauseEvent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(rawPause));
    }
    if (!ValidRequest(request)) return std::nullopt;
    return parsed;
}

NeuralRenderResult RunNeuralWorker(const std::filesystem::path& executable,
                                   const NeuralRenderRequest& request,
                                   OfflineNeuralRenderer::ProgressCallback progress,
                                   std::stop_token stop, const NeuralSegmentSink& segments,
                                   uint32_t crashRelaunchLimit)
{
    NeuralRenderResult result;
    result.jobId = request.jobId;
    std::error_code fileError;
    if (executable.empty() || !std::filesystem::is_regular_file(executable, fileError) || fileError) {
        result.failure = NeuralRenderFailure::Protocol;
        result.detail = L"The isolated neural helper executable is unavailable.";
        return result;
    }
    if (!ValidRequest(request)) {
        result.failure = NeuralRenderFailure::Protocol;
        result.detail = L"Invalid neural helper request.";
        return result;
    }
    // Bounded from-zero relaunch: a crashed helper or a removed device leaves
    // no temporal history to resume, so the whole sequence restarts in a fresh
    // process. Frames are never spliced across helper instances.
    for (uint32_t attempt = 0;; ++attempt) {
        // A relaunch renders the range again from frame zero, so every segment
        // the previous helper published names a file that is about to be
        // rewritten.
        if (attempt && segments.onRestart) segments.onRestart();
        result = RunNeuralWorkerAttempt(executable, request, progress, segments, stop, false);
        const bool relaunchable = !result.ok && !result.cancelled &&
            (result.failure == NeuralRenderFailure::WorkerCrashed ||
             result.failure == NeuralRenderFailure::DeviceRemoved ||
             result.failure == NeuralRenderFailure::GpuStall);
        if (!relaunchable) return result;
        if (attempt >= crashRelaunchLimit) {
            result.failure = NeuralRenderFailure::RetryExhausted;
            result.detail = L"The neural helper did not recover after " + std::to_wstring(attempt + 1) +
                            L" attempt(s): " + result.detail;
            return result;
        }
        if (progress) {
            NeuralRenderProgress recovering{};
            recovering.phase = NeuralRenderPhase::Recovering;
            recovering.recovering = result.failure;
            recovering.retries = attempt + 1;
            progress(recovering);
        }
    }
}

neural_worker_detail::MetadataStreamOutcome neural_worker_detail::DecodeMetadataStream(
    std::span<const std::byte> bytes)
{
    MetadataStreamOutcome outcome;
    NeuralSegmentSink sink;
    sink.onSegment = [&](const NeuralRenderSegment& segment) { outcome.segments.push_back(segment); };
    sink.onRestart = [&] { ++outcome.restarts; outcome.segments.clear(); };
    MetadataReader reader([&](const NeuralRenderProgress&) { ++outcome.progressUpdates; }, sink);
    // Deliberately fragmented: the pipe delivers arbitrary chunks and a message
    // header can straddle two reads.
    constexpr size_t chunkBytes = 7;
    for (size_t offset = 0; offset < bytes.size(); offset += chunkBytes) {
        if (!reader.Push(bytes.subspan(offset, std::min(chunkBytes, bytes.size() - offset)))) break;
    }
    outcome.malformed = reader.Malformed();
    outcome.complete = reader.Complete();
    return outcome;
}

NeuralPreflightResult RunNeuralPreflight(const std::filesystem::path& executable, std::stop_token stop)
{
    NeuralPreflightResult result;
    std::error_code fileError;
    if (executable.empty() || !std::filesystem::is_regular_file(executable, fileError) || fileError) {
        result.detail = L"The isolated neural helper executable is unavailable.";
        return result;
    }
    for (bool restarted = false;; restarted = true) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            result.detail = L"Neural preflight was cancelled.";
            return result;
        }
        MetadataReader reader({}, {});
        const LaunchOutcome launch = LaunchHelper(executable,
            [&](HANDLE metadata, HANDLE) { return neural_worker_detail::BuildPreflightArguments(metadata, restarted); },
            nullptr, reader, stop);
        if (!launch.launched) { result.detail = launch.detail; return result; }
        if (launch.cancelled || stop.stop_requested()) {
            result.cancelled = true;
            result.detail = L"Neural preflight was cancelled.";
            return result;
        }
        if (launch.exitCode == neural_worker_detail::kConfigurationChangedExitCode && !restarted &&
            !reader.Malformed()) continue;
        if (launch.exitCode != 0) {
            result.detail = L"The neural preflight helper exited with code " + std::to_wstring(launch.exitCode) + L".";
            return result;
        }
        if (!reader.PreflightComplete()) {
            result.detail = reader.Malformed() ? L"The neural preflight helper returned malformed metadata." :
                                                 L"The neural preflight helper returned no receipt.";
            return result;
        }
        result.ok = reader.Preflight().ok;
        result.json = reader.Preflight().json;
        if (!result.ok) result.detail = L"The neural runtime preflight did not arm feature 18.";
        return result;
    }
}

std::wstring NeuralRuntimeLease::MutexName(const std::filesystem::path& runtimeDirectory)
{
    // FNV-1a over the normalized, lower-cased directory: every process must
    // derive the same name for the same runtime, and the name must stay well
    // inside the kernel object-name length limit for long paths.
    std::wstring path = runtimeDirectory.lexically_normal().wstring();
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t character : path) {
        if (character >= L'A' && character <= L'Z') character = wchar_t(character - L'A' + L'a');
        if (character == L'/') character = L'\\';
        hash = (hash ^ static_cast<uint64_t>(character)) * 1099511628211ull;
    }
    std::wstring name = L"Local\\DLSSVideoPlayer.neural-runtime.";
    for (int shift = 60; shift >= 0; shift -= 4) name.push_back(L"0123456789abcdef"[(hash >> shift) & 0xF]);
    return name;
}

NeuralRuntimeLease::NeuralRuntimeLease(const std::filesystem::path& runtimeDirectory,
                                       std::chrono::milliseconds wait)
{
    if (runtimeDirectory.empty()) return;
    const std::wstring name = MutexName(runtimeDirectory);
    mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!mutex_) return;
    const DWORD milliseconds = wait.count() <= 0
        ? 0u : static_cast<DWORD>(std::min<long long>(wait.count(), INFINITE - 1));
    // WAIT_ABANDONED means the previous holder died without releasing it: the
    // runtime is ours, and its stale log is retired before the next launch.
    const DWORD result = WaitForSingleObject(mutex_, milliseconds);
    held_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
}

NeuralRuntimeLease::~NeuralRuntimeLease()
{
    if (mutex_ && held_) ReleaseMutex(mutex_);
    if (mutex_) CloseHandle(mutex_);
}
