#include "MediaPipeline.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>

namespace {

std::wstring FrameRateText(double fps)
{
    std::wostringstream output;
    output << std::fixed << std::setprecision(6) << fps;
    std::wstring value = output.str();
    while (value.size() > 1 && value.back() == L'0') value.pop_back();
    if (!value.empty() && value.back() == L'.') value.pop_back();
    return value;
}

std::filesystem::path ModuleDirectory()
{
    std::wstring value(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size()) return {};
    value.resize(length);
    return std::filesystem::path(value).parent_path();
}

std::filesystem::path FindHelper(const std::filesystem::path& directory,
                                 std::wstring_view name)
{
    const auto candidate = (directory.empty() ? ModuleDirectory() : directory) / name;
    std::error_code error;
    if (!std::filesystem::is_regular_file(candidate, error) || error) return {};
    return candidate;
}

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

std::wstring CommandLine(const std::filesystem::path& executable,
                         const std::vector<std::wstring>& arguments)
{
    std::wstring result = QuoteArgument(executable.wstring());
    for (const auto& argument : arguments) {
        result.push_back(L' ');
        result += QuoteArgument(argument);
    }
    return result;
}

HANDLE CreateKillOnCloseJob()
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

struct ChildProcess {
    HANDLE process{};
    HANDLE job{};
    HANDLE stdinWrite{};

    ~ChildProcess() { Stop(); }

    void Stop()
    {
        if (stdinWrite) { CloseHandle(stdinWrite); stdinWrite = nullptr; }
        if (process) {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                if (job) TerminateJobObject(job, 1); else TerminateProcess(process, 1);
                WaitForSingleObject(process, 2000);
            }
            CloseHandle(process);
            process = nullptr;
        }
        if (job) { CloseHandle(job); job = nullptr; }
    }

    bool Start(const std::filesystem::path& executable,
               const std::vector<std::wstring>& arguments, bool pipeInput)
    {
        Stop();
        HANDLE stdinRead = nullptr;
        if (pipeInput) {
            SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
            if (!CreatePipe(&stdinRead, &stdinWrite, &security, 0)) return false;
            if (!SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0)) {
                CloseHandle(stdinRead); CloseHandle(stdinWrite); stdinWrite = nullptr; return false;
            }
        }
        job = CreateKillOnCloseJob();
        if (!job) {
            if (stdinRead) CloseHandle(stdinRead);
            if (stdinWrite) { CloseHandle(stdinWrite); stdinWrite = nullptr; }
            return false;
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        std::vector<uint8_t> attributes;
        if (pipeInput) {
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = stdinRead;
            startup.StartupInfo.hStdOutput = nullptr;
            startup.StartupInfo.hStdError = nullptr;
            SIZE_T attributeBytes = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
            if (attributeBytes == 0) {
                CloseHandle(stdinRead); Stop(); return false;
            }
            attributes.resize(attributeBytes);
            startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
            HANDLE inherited = stdinRead;
            if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                                   &attributeBytes)) {
                CloseHandle(stdinRead); Stop(); return false;
            }
            if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inherited, sizeof(inherited),
                    nullptr, nullptr)) {
                DeleteProcThreadAttributeList(startup.lpAttributeList);
                CloseHandle(stdinRead); Stop(); return false;
            }
        }
        PROCESS_INFORMATION info{};
        std::wstring command = CommandLine(executable, arguments);
        const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
            pipeInput ? TRUE : FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED |
            (pipeInput ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
            executable.parent_path().c_str(), &startup.StartupInfo, &info);
        if (startup.lpAttributeList) DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (stdinRead) CloseHandle(stdinRead);
        if (!created) { Stop(); return false; }
        process = info.hProcess;
        if (!AssignProcessToJobObject(job, process)) {
            TerminateProcess(process, 1); CloseHandle(info.hThread); Stop(); return false;
        }
        if (ResumeThread(info.hThread) == static_cast<DWORD>(-1)) {
            CloseHandle(info.hThread); Stop(); return false;
        }
        CloseHandle(info.hThread);
        return true;
    }

    DWORD Wait(std::stop_token stop, bool& cancelled)
    {
        cancelled = false;
        if (!process) return static_cast<DWORD>(-1);
        for (;;) {
            const DWORD wait = WaitForSingleObject(process, 25);
            if (wait == WAIT_OBJECT_0) break;
            if (wait != WAIT_TIMEOUT) return static_cast<DWORD>(-1);
            if (stop.stop_requested()) {
                cancelled = true;
                if (job) TerminateJobObject(job, 1); else TerminateProcess(process, 1);
                WaitForSingleObject(process, 2000);
                break;
            }
        }
        DWORD exitCode = static_cast<DWORD>(-1);
        GetExitCodeProcess(process, &exitCode);
        CloseHandle(process); process = nullptr;
        if (job) { CloseHandle(job); job = nullptr; }
        return exitCode;
    }
};

struct CaptureResult {
    bool started{};
    bool cancelled{};
    DWORD exitCode{static_cast<DWORD>(-1)};
    std::string output;
};

CaptureResult RunCapture(const std::filesystem::path& executable,
                         const std::vector<std::wstring>& arguments,
                         std::stop_token stop, size_t limit = 1024 * 1024)
{
    CaptureResult result;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) return result;
    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readPipe); CloseHandle(writePipe); return result;
    }
    HANDLE job = CreateKillOnCloseJob();
    if (!job) { CloseHandle(readPipe); CloseHandle(writePipe); return result; }
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    if (attributeBytes == 0) {
        CloseHandle(readPipe); CloseHandle(writePipe); CloseHandle(job); return result;
    }
    std::vector<std::byte> attributeStorage(attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes)) {
        CloseHandle(readPipe); CloseHandle(writePipe); CloseHandle(job); return result;
    }
    if (!UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   &writePipe, sizeof(writePipe), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attributeList);
        CloseHandle(readPipe); CloseHandle(writePipe); CloseHandle(job); return result;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullptr;
    startup.StartupInfo.hStdOutput = writePipe;
    startup.StartupInfo.hStdError = writePipe;
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION info{};
    std::wstring command = CommandLine(executable, arguments);
    const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(attributeList);
    CloseHandle(writePipe);
    if (!created) { CloseHandle(readPipe); CloseHandle(job); return result; }
    result.started = true;
    if (!AssignProcessToJobObject(job, info.hProcess) ||
        ResumeThread(info.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(info.hProcess, 1);
    }
    CloseHandle(info.hThread);
    std::array<char, 4096> buffer{};
    bool done = false;
    while (!done) {
        DWORD available = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available) {
            DWORD read = 0;
            const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (ReadFile(readPipe, buffer.data(), wanted, &read, nullptr) && read) {
                if (result.output.size() + read > limit) {
                    TerminateJobObject(job, 1);
                    result.output.clear();
                    break;
                }
                result.output.append(buffer.data(), read);
            }
        }
        const DWORD wait = WaitForSingleObject(info.hProcess, 10);
        if (wait == WAIT_OBJECT_0) done = true;
        else if (wait != WAIT_TIMEOUT) { TerminateJobObject(job, 1); done = true; }
        if (stop.stop_requested()) {
            result.cancelled = true;
            TerminateJobObject(job, 1);
            WaitForSingleObject(info.hProcess, 2000);
            done = true;
        }
    }
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || !read)
            break;
        if (result.output.size() + read <= limit) result.output.append(buffer.data(), read);
    }
    GetExitCodeProcess(info.hProcess, &result.exitCode);
    CloseHandle(readPipe); CloseHandle(info.hProcess); CloseHandle(job);
    return result;
}

bool ParseUnsigned(std::string_view value, uint64_t& output)
{
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

} // namespace

std::vector<std::wstring> BuildMaterializeArguments(const MaterializeRequest& request)
{
    std::vector<std::wstring> arguments{
        L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-y",
        L"-i", request.videoUrl,
    };
    if (!request.audioUrl.empty()) {
        arguments.insert(arguments.end(), {L"-i", request.audioUrl});
    }
    arguments.insert(arguments.end(), {L"-map", L"0:v:0"});
    if (!request.audioUrl.empty()) {
        arguments.insert(arguments.end(), {L"-map", L"1:a:0?"});
    } else {
        arguments.insert(arguments.end(), {L"-map", L"0:a:0?"});
    }
    arguments.insert(arguments.end(), {
        L"-c", L"copy", L"-f", L"matroska", request.output.wstring()});
    return arguments;
}

std::vector<std::wstring> BuildEncoderArguments(const EncoderSpec& spec,
                                                const std::filesystem::path& output)
{
    std::vector<std::wstring> arguments{
        L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-y",
        L"-f", L"rawvideo", L"-pix_fmt", L"bgra",
        L"-video_size", std::to_wstring(spec.width) + L"x" + std::to_wstring(spec.height),
        L"-framerate", FrameRateText(spec.fps), L"-i", L"pipe:0", L"-an",
    };
    if (spec.kind == EncoderKind::HevcNvenc) {
        arguments.insert(arguments.end(), {
            L"-c:v", L"hevc_nvenc", L"-preset", L"p7", L"-tune", L"hq",
            L"-rc", L"vbr", L"-cq", L"16", L"-b:v", L"0",
            L"-pix_fmt", L"yuv420p"});
    } else {
        arguments.insert(arguments.end(), {
            L"-c:v", L"libx264", L"-preset", L"slow", L"-crf", L"16",
            L"-pix_fmt", L"yuv420p"});
    }
    arguments.insert(arguments.end(), {L"-f", L"matroska", output.wstring()});
    return arguments;
}

size_t ExpectedBgraFrameBytes(const EncoderSpec& spec)
{
    if (spec.width == 0 || spec.height == 0 || !std::isfinite(spec.fps) || spec.fps <= 0.0)
        return 0;
    constexpr uint64_t bytesPerPixel = 4;
    const uint64_t bytes = uint64_t{spec.width} * spec.height * bytesPerPixel;
    if (bytes > std::numeric_limits<size_t>::max()) return 0;
    return static_cast<size_t>(bytes);
}

bool ShouldRetryWithSoftware(EncoderKind attempted, EncodeError error)
{
    if (attempted != EncoderKind::HevcNvenc) return false;
    return error == EncodeError::StartFailed || error == EncodeError::WriteFailed ||
           error == EncodeError::FinishFailed;
}

MediaMaterializer::MediaMaterializer(std::filesystem::path helperDirectory)
    : helperDirectory_(std::move(helperDirectory)) {}

MaterializeResult MediaMaterializer::Run(const MaterializeRequest& request, std::stop_token stop)
{
    if (request.videoUrl.empty() || request.output.empty())
        return {false, MaterializeError::InvalidRequest, L"Invalid media materialization request."};
    const auto ffmpeg = FindHelper(helperDirectory_, L"ffmpeg.exe");
    if (ffmpeg.empty()) return {false, MaterializeError::HelperMissing, L"FFmpeg is unavailable."};
    ChildProcess process;
    if (!process.Start(ffmpeg, BuildMaterializeArguments(request), false))
        return {false, MaterializeError::StartFailed, L"FFmpeg could not be started."};
    bool cancelled = false;
    const DWORD exitCode = process.Wait(stop, cancelled);
    if (cancelled) return {false, MaterializeError::Cancelled, L"Source preparation was cancelled."};
    std::error_code error;
    if (exitCode != 0 || !std::filesystem::is_regular_file(request.output, error) || error)
        return {false, MaterializeError::ProcessFailed, L"FFmpeg could not prepare the source."};
    return {true, MaterializeError::None, {}};
}

struct RawVideoEncoder::Impl {
    explicit Impl(std::filesystem::path directory) : helperDirectory(std::move(directory)) {}
    std::filesystem::path helperDirectory;
    std::filesystem::path output;
    EncoderSpec spec;
    size_t frameBytes{};
    ChildProcess process;
    bool active{};
};

RawVideoEncoder::RawVideoEncoder(std::filesystem::path helperDirectory)
    : impl_(std::make_unique<Impl>(std::move(helperDirectory))) {}

RawVideoEncoder::~RawVideoEncoder() { Cancel(); }

EncodeError RawVideoEncoder::Start(const EncoderSpec& spec,
                                   const std::filesystem::path& output)
{
    Cancel();
    const size_t bytes = ExpectedBgraFrameBytes(spec);
    if (bytes == 0 || output.empty()) return EncodeError::InvalidSpecification;
    const auto ffmpeg = FindHelper(impl_->helperDirectory, L"ffmpeg.exe");
    if (ffmpeg.empty()) return EncodeError::HelperMissing;
    if (!impl_->process.Start(ffmpeg, BuildEncoderArguments(spec, output), true))
        return EncodeError::StartFailed;
    impl_->spec = spec;
    impl_->output = output;
    impl_->frameBytes = bytes;
    impl_->active = true;
    return EncodeError::None;
}

EncodeError RawVideoEncoder::WriteFrame(std::span<const uint8_t> bgra, std::stop_token stop)
{
    if (!impl_->active || !impl_->process.stdinWrite) return EncodeError::WriteFailed;
    if (bgra.size() != impl_->frameBytes) return EncodeError::InvalidFrame;
    size_t offset = 0;
    while (offset < bgra.size()) {
        if (stop.stop_requested()) { Cancel(); return EncodeError::Cancelled; }
        const DWORD wanted = static_cast<DWORD>(std::min<size_t>(bgra.size() - offset, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(impl_->process.stdinWrite, bgra.data() + offset, wanted, &written, nullptr) ||
            written == 0) {
            Cancel();
            return EncodeError::WriteFailed;
        }
        offset += written;
    }
    return EncodeError::None;
}

EncodeError RawVideoEncoder::Finish(std::stop_token stop)
{
    if (!impl_->active) return EncodeError::FinishFailed;
    if (impl_->process.stdinWrite) {
        CloseHandle(impl_->process.stdinWrite);
        impl_->process.stdinWrite = nullptr;
    }
    bool cancelled = false;
    const DWORD exitCode = impl_->process.Wait(stop, cancelled);
    impl_->active = false;
    if (cancelled) return EncodeError::Cancelled;
    std::error_code error;
    if (exitCode != 0 || !std::filesystem::is_regular_file(impl_->output, error) || error)
        return EncodeError::FinishFailed;
    return EncodeError::None;
}

void RawVideoEncoder::Cancel()
{
    if (!impl_) return;
    impl_->process.Stop();
    impl_->active = false;
}

ProbeResult ProbeMedia(const std::filesystem::path& helperDirectory,
                       const std::filesystem::path& media,
                       std::stop_token stop)
{
    ProbeResult result;
    const auto ffprobe = FindHelper(helperDirectory, L"ffprobe.exe");
    const auto ffmpeg = FindHelper(helperDirectory, L"ffmpeg.exe");
    if (ffprobe.empty() || ffmpeg.empty()) { result.detail = L"FFmpeg tools are unavailable."; return result; }
    const std::vector<std::wstring> probeArguments{
        L"-v", L"error", L"-count_frames", L"-select_streams", L"v:0",
        L"-show_entries", L"stream=width,height,nb_read_frames:format=duration",
        L"-of", L"default=noprint_wrappers=1:nokey=0", media.wstring()};
    const CaptureResult capture = RunCapture(ffprobe, probeArguments, stop);
    if (!capture.started || capture.cancelled || capture.exitCode != 0) {
        result.detail = capture.cancelled ? L"Media validation was cancelled." : L"FFprobe validation failed.";
        return result;
    }
    double durationSeconds = 0.0;
    size_t position = 0;
    while (position < capture.output.size()) {
        const size_t end = capture.output.find('\n', position);
        std::string_view line(capture.output.data() + position,
            (end == std::string::npos ? capture.output.size() : end) - position);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const size_t equals = line.find('=');
        if (equals != std::string_view::npos) {
            const auto key = line.substr(0, equals);
            const auto value = line.substr(equals + 1);
            uint64_t integer = 0;
            if (key == "width" && ParseUnsigned(value, integer)) result.width = static_cast<uint32_t>(integer);
            else if (key == "height" && ParseUnsigned(value, integer)) result.height = static_cast<uint32_t>(integer);
            else if (key == "nb_read_frames" && ParseUnsigned(value, integer)) result.frameCount = integer;
            else if (key == "duration") {
                try { durationSeconds = std::stod(std::string(value)); } catch (...) { durationSeconds = 0.0; }
            }
        }
        if (end == std::string::npos) break;
        position = end + 1;
    }
    result.duration100ns = std::llround(durationSeconds * 10000000.0);
    if (!result.width || !result.height || !result.frameCount || result.duration100ns <= 0) {
        result.detail = L"FFprobe returned incomplete media metadata.";
        return result;
    }
    ChildProcess finalFrame;
    const std::vector<std::wstring> decodeArguments{
        L"-v", L"error", L"-sseof", L"-1", L"-i", media.wstring(),
        L"-map", L"0:v:0", L"-frames:v", L"1", L"-f", L"null", L"NUL"};
    if (!finalFrame.Start(ffmpeg, decodeArguments, false)) {
        result.detail = L"Final-frame validation could not start.";
        return result;
    }
    bool cancelled = false;
    result.decodedFinalFrame = finalFrame.Wait(stop, cancelled) == 0 && !cancelled;
    result.ok = result.decodedFinalFrame;
    if (!result.ok) result.detail = L"The encoded final frame could not be decoded.";
    return result;
}
