#include "NeuralWorker.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kProtocolMagic = 0x3152574Eu; // NWR1
constexpr uint16_t kProtocolVersion = 1;
constexpr uint32_t kMaximumPayloadBytes = 16 * 1024;
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

bool IsBooleanByte(uint8_t value)
{
    return value == 0 || value == 1;
}

bool IsKnownPhase(uint32_t phase)
{
    return phase <= static_cast<uint32_t>(NeuralRenderPhase::Ready);
}

bool IsKnownEncoder(uint8_t encoder)
{
    return encoder == static_cast<uint8_t>(EncoderKind::HevcNvenc) ||
        encoder == static_cast<uint8_t>(EncoderKind::H264Software);
}

bool IsZeroed(std::span<const uint8_t> bytes)
{
    return std::all_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value == 0; });
}

class MetadataReader {
public:
    explicit MetadataReader(OfflineNeuralRenderer::ProgressCallback progress)
        : progress_(std::move(progress)) {}

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
            if (bytes_.size() + read > kMaximumPayloadBytes * 2u) {
                malformed_ = true;
                return false;
            }
            bytes_.insert(bytes_.end(), chunk.begin(), chunk.begin() + read);
            if (!Consume()) return false;
        }
    }

    bool Complete() const
    {
        return !malformed_ && bytes_.empty() && result_.has_value();
    }

    bool Malformed() const { return malformed_; }
    const NeuralRenderResult& Result() const { return *result_; }

private:
    bool Consume()
    {
        size_t offset = 0;
        while (bytes_.size() - offset >= sizeof(WireHeader)) {
            WireHeader header{};
            std::memcpy(&header, bytes_.data() + offset, sizeof(header));
            if (header.magic != kProtocolMagic || header.version != kProtocolVersion ||
                header.payloadBytes > kMaximumPayloadBytes ||
                (header.kind != static_cast<uint16_t>(WireKind::Progress) &&
                 header.kind != static_cast<uint16_t>(WireKind::Result))) {
                malformed_ = true;
                return false;
            }
            const size_t messageBytes = sizeof(header) + static_cast<size_t>(header.payloadBytes);
            if (bytes_.size() - offset < messageBytes) break;
            const std::byte* payload = bytes_.data() + offset + sizeof(header);
            if (header.kind == static_cast<uint16_t>(WireKind::Progress)) {
                if (result_.has_value() || !ConsumeProgress(payload, header.payloadBytes)) {
                    malformed_ = true;
                    return false;
                }
            } else if (result_.has_value() || !ConsumeResult(payload, header.payloadBytes)) {
                malformed_ = true;
                return false;
            }
            offset += messageBytes;
        }
        if (offset) bytes_.erase(bytes_.begin(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
        return true;
    }

    bool ConsumeProgress(const std::byte* payload, uint32_t bytes)
    {
        if (bytes != sizeof(WireProgress)) return false;
        WireProgress wire{};
        std::memcpy(&wire, payload, sizeof(wire));
        if (!IsKnownPhase(wire.phase) || wire.completedFrames > wire.totalFrames ||
            wire.elapsedMilliseconds < 0 || wire.estimatedRemainingMilliseconds < 0) return false;
        if (progress_) {
            progress_({static_cast<NeuralRenderPhase>(wire.phase), wire.completedFrames, wire.totalFrames,
                wire.bytes, std::chrono::milliseconds(wire.elapsedMilliseconds),
                std::chrono::milliseconds(wire.estimatedRemainingMilliseconds)});
        }
        return true;
    }

    bool ConsumeResult(const std::byte* payload, uint32_t bytes)
    {
        if (bytes < sizeof(WireResult)) return false;
        WireResult wire{};
        std::memcpy(&wire, payload, sizeof(wire));
        if (!IsBooleanByte(wire.ok) || !IsBooleanByte(wire.cancelled) || !IsKnownEncoder(wire.encoder) ||
            !IsBooleanByte(wire.feature18ArmedBeforeCapture) || !IsBooleanByte(wire.upscalingOff) ||
            !IsBooleanByte(wire.inlineInterceptionContract) || !IsBooleanByte(wire.feature18Created) ||
            !IsBooleanByte(wire.feature18Evaluated) || !IsBooleanByte(wire.laterFailure) ||
            !IsZeroed(wire.reserved) || wire.detailBytes > kMaximumDetailBytes ||
            (wire.detailBytes % sizeof(wchar_t)) != 0 ||
            bytes != sizeof(WireResult) + wire.detailBytes) return false;

        NeuralRenderResult result;
        result.ok = wire.ok != 0;
        result.cancelled = wire.cancelled != 0;
        result.encoder = static_cast<EncoderKind>(wire.encoder);
        result.frameCount = wire.frameCount;
        result.duration100ns = wire.duration100ns;
        result.nativeEvaluations = wire.nativeEvaluations;
        result.verifiedNeuralFrames = wire.verifiedNeuralFrames;
        result.feature18ArmedBeforeCapture = wire.feature18ArmedBeforeCapture != 0;
        result.evidence = {wire.upscalingOff != 0, wire.inlineInterceptionContract != 0,
            wire.feature18Created != 0, wire.feature18Evaluated != 0, wire.laterFailure != 0,
            wire.highestObservedEvaluation};
        if (wire.detailBytes) {
            const auto* detail = reinterpret_cast<const wchar_t*>(payload + sizeof(WireResult));
            result.detail.assign(detail, detail + wire.detailBytes / sizeof(wchar_t));
            if (result.detail.find(L'\0') != std::wstring::npos) return false;
        }
        if (result.ok && (result.cancelled || !result.frameCount || result.duration100ns <= 0 ||
                          !result.nativeEvaluations || result.verifiedNeuralFrames < result.frameCount ||
                          !result.feature18ArmedBeforeCapture || !result.evidence.Valid())) return false;
        if (result.cancelled && result.ok) return false;
        result_ = std::move(result);
        return true;
    }

    OfflineNeuralRenderer::ProgressCallback progress_;
    std::vector<std::byte> bytes_;
    std::optional<NeuralRenderResult> result_;
    bool malformed_{};
};

std::wstring ErrorDetail(std::wstring_view operation)
{
    return std::wstring(operation) + L" (Win32 error " + std::to_wstring(GetLastError()) + L").";
}

} // namespace

bool neural_worker_detail::HasValidWorkerArgumentShape(std::span<const std::wstring_view> arguments)
{
    if ((arguments.size() != 16 && arguments.size() != 17) ||
        arguments[1] != L"--neural-worker" ||
        (arguments.size() == 17 && arguments[16] != L"--configuration-restarted")) return false;
    constexpr std::array<std::wstring_view, 7> expected{
        L"--metadata-handle", L"--source", L"--staging", L"--width", L"--height", L"--fps",
        L"--duration-100ns"};
    std::array<bool, expected.size()> seen{};
    for (size_t index = 2; index < 16; index += 2) {
        if (arguments[index + 1].empty()) return false;
        const auto it = std::find(expected.begin(), expected.end(), arguments[index]);
        if (it == expected.end()) return false;
        const size_t expectedIndex = static_cast<size_t>(it - expected.begin());
        if (seen[expectedIndex]) return false;
        seen[expectedIndex] = true;
    }
    return std::all_of(seen.begin(), seen.end(), [](bool value) { return value; });
}

static NeuralRenderResult RunNeuralWorkerAttempt(const std::filesystem::path& executable,
                                   const NeuralRenderRequest& request,
                                   OfflineNeuralRenderer::ProgressCallback progress,
                                   std::stop_token stop, bool configurationRestarted)
{
    NeuralRenderResult result;
    std::error_code fileError;
    if (executable.empty() || !std::filesystem::is_regular_file(executable, fileError) || fileError) {
        result.detail = L"The isolated neural helper executable is unavailable.";
        return result;
    }
    if (request.sourcePath.empty() || request.stagingVideoPath.empty() || !request.width || !request.height ||
        !std::isfinite(request.fps) || request.fps <= 0.0 || !std::isfinite(request.durationSeconds) ||
        request.durationSeconds <= 0.0) {
        result.detail = L"Invalid neural helper request.";
        return result;
    }
    if (stop.stop_requested()) {
        result.cancelled = true;
        result.detail = L"Neural rendering was cancelled before the helper started.";
        return result;
    }

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE metadataRead = nullptr;
    HANDLE metadataWrite = nullptr;
    if (!CreatePipe(&metadataRead, &metadataWrite, &security, 0) ||
        !SetHandleInformation(metadataRead, HANDLE_FLAG_INHERIT, 0)) {
        if (metadataRead) CloseHandle(metadataRead);
        if (metadataWrite) CloseHandle(metadataWrite);
        result.detail = ErrorDetail(L"Creating the neural helper metadata pipe failed");
        return result;
    }
    HANDLE job = CreateKillOnCloseJob();
    if (!job) {
        CloseHandle(metadataRead); CloseHandle(metadataWrite);
        result.detail = ErrorDetail(L"Creating the neural helper job failed");
        return result;
    }

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::byte> attributes(attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    const bool attributeListInitialized = attributeBytes &&
        InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes) != FALSE;
    if (!attributeListInitialized || !UpdateProcThreadAttribute(attributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &metadataWrite, sizeof(metadataWrite), nullptr, nullptr)) {
        if (attributeListInitialized) DeleteProcThreadAttributeList(attributeList);
        CloseHandle(job); CloseHandle(metadataRead); CloseHandle(metadataWrite);
        result.detail = ErrorDetail(L"Restricting neural helper handle inheritance failed");
        return result;
    }

    std::vector<std::wstring> arguments{
        L"--neural-worker", L"--metadata-handle", std::to_wstring(reinterpret_cast<uintptr_t>(metadataWrite)),
        L"--source", request.sourcePath.wstring(), L"--staging", request.stagingVideoPath.wstring(),
        L"--width", std::to_wstring(request.width), L"--height", std::to_wstring(request.height),
        L"--fps", std::to_wstring(request.fps), L"--duration-100ns",
        std::to_wstring(static_cast<int64_t>(std::llround(request.durationSeconds * 10000000.0)))};
    if (configurationRestarted) arguments.emplace_back(L"--configuration-restarted");
    std::wstring command = MakeCommandLine(executable, arguments);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
        executable.parent_path().c_str(), &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributeList);
    CloseHandle(metadataWrite);
    metadataWrite = nullptr;
    if (!created) {
        CloseHandle(job); CloseHandle(metadataRead);
        result.detail = ErrorDetail(L"Starting the isolated neural helper failed");
        return result;
    }
    const bool assigned = AssignProcessToJobObject(job, process.hProcess) != FALSE;
    const DWORD resumed = assigned ? ResumeThread(process.hThread) : static_cast<DWORD>(-1);
    CloseHandle(process.hThread);
    if (!assigned || resumed == static_cast<DWORD>(-1)) {
        KillAndWait(job, process.hProcess);
        CloseHandle(process.hProcess); CloseHandle(job); CloseHandle(metadataRead);
        result.detail = assigned ? ErrorDetail(L"Resuming the isolated neural helper failed") :
                                  L"The isolated neural helper could not be assigned to its job.";
        return result;
    }

    MetadataReader reader(progress);
    bool cancelled = false;
    for (;;) {
        if (!reader.ReadAvailable(metadataRead)) break;
        const DWORD wait = WaitForSingleObject(process.hProcess, 20);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_TIMEOUT) break;
        if (stop.stop_requested()) {
            cancelled = true;
            KillAndWait(job, process.hProcess);
            break;
        }
    }
    reader.ReadAvailable(metadataRead);
    DWORD exitCode = static_cast<DWORD>(-1);
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(metadataRead); CloseHandle(process.hProcess); CloseHandle(job);

    if (cancelled || stop.stop_requested()) {
        result.cancelled = true;
        result.detail = L"Neural rendering was cancelled.";
        return result;
    }
    // The old process is signalled and all its handles are closed here. Waiting
    // for full exit releases ReShade.log before the next proxy loads; overlapping
    // helpers otherwise put the real evidence in ReShade.log1.
    if (exitCode == neural_worker_detail::kConfigurationChangedExitCode &&
        !configurationRestarted && !reader.Malformed()) {
        return RunNeuralWorkerAttempt(executable, request, std::move(progress), stop, true);
    }
    if (exitCode != 0) {
        result.detail = L"The isolated neural helper failed before producing a result.";
        return result;
    }
    if (!reader.Complete()) {
        result.detail = reader.Malformed() ? L"The isolated neural helper returned malformed metadata." :
                                             L"The isolated neural helper returned incomplete metadata.";
        return result;
    }
    return reader.Result();
}

NeuralRenderResult RunNeuralWorker(const std::filesystem::path& executable,
                                   const NeuralRenderRequest& request,
                                   OfflineNeuralRenderer::ProgressCallback progress,
                                   std::stop_token stop)
{
    return RunNeuralWorkerAttempt(executable, request, std::move(progress), stop, false);
}
