#include "MediaPipeline.h"
#include "HardErrorSuppression.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <functional>
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

struct ExportFormat {
    bool mkv{}, mp4{}, gif{}, png{}, jpeg{};
    bool Valid() const { return mkv || mp4 || gif || png || jpeg; }
};

ExportFormat ExportFormatFor(const std::filesystem::path& output)
{
    const auto extension = output.extension().wstring();
    const auto is = [&](const wchar_t* candidate) { return _wcsicmp(extension.c_str(), candidate) == 0; };
    return {is(L".mkv"), is(L".mp4"), is(L".gif"), is(L".png"), is(L".jpg") || is(L".jpeg")};
}

bool ValidRangeSeconds(double value) { return std::isfinite(value) && value >= 0.0; }

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
    const std::filesystem::path base = directory.empty() ? ModuleDirectory() : directory;
    // The neural helper is deliberately packaged below the hook-free player.
    // Its FFmpeg tools remain the single shared copies beside that parent,
    // never an arbitrary executable found through PATH.
    if (base.filename() == L"neural-runtime") {
        const auto shared = base.parent_path() / name;
        std::error_code error;
        if (std::filesystem::is_regular_file(shared, error) && !error) return shared;
        return {};
    }
    const auto candidate = base / name;
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
            // The caller's frame queue provides the real buffering; a larger pipe just
            // holds nonpaged pool.
            if (!CreatePipe(&stdinRead, &stdinWrite, &security,
                            static_cast<DWORD>(kChildStdinPipeBytes))) return false;
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
        const ScopedHardErrorSuppression noHardErrorDialog;
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

class OwnedHandle {
public:
    explicit OwnedHandle(HANDLE value=nullptr):value_(value){}
    ~OwnedHandle(){if(value_)CloseHandle(value_);}
    OwnedHandle(const OwnedHandle&)=delete;
    OwnedHandle& operator=(const OwnedHandle&)=delete;
    HANDLE get()const{return value_;}
private:
    HANDLE value_{};
};

struct CaptureResult {
    bool started{};
    bool cancelled{};
    DWORD exitCode{static_cast<DWORD>(-1)};
    std::string output;
};

CaptureResult RunCapture(const std::filesystem::path& executable,
                         const std::vector<std::wstring>& arguments,
                         std::stop_token stop, size_t limit = 1024 * 1024,
                         const std::function<void(std::string_view)>& consume = {})
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
    const ScopedHardErrorSuppression noHardErrorDialog;
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
    bool captureOverflowed = false;
    while (!done) {
        DWORD available = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available) {
            DWORD read = 0;
            const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (ReadFile(readPipe, buffer.data(), wanted, &read, nullptr) && read) {
                if (consume) consume(std::string_view(buffer.data(), read));
                else if (result.output.size() + read > limit) {
                    captureOverflowed = true;
                    TerminateJobObject(job, 1);
                    result.output.clear();
                    break;
                }
                else result.output.append(buffer.data(), read);
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
        if (consume) consume(std::string_view(buffer.data(), read));
        else if (!captureOverflowed) {
            if (result.output.size() + read <= limit) result.output.append(buffer.data(), read);
            else {
                captureOverflowed = true;
                result.output.clear();
            }
        }
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

std::wstring MaterializeDiagnostic(std::string_view output, const MaterializeRequest& request)
{
    if (output.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, output.data(),
                                       static_cast<int>(output.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring text(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, output.data(),
                        static_cast<int>(output.size()), text.data(), size);
    // Remove exact inputs first (including any whitespace), then URLs emitted
    // after redirects. Redact before shortening so signed query strings cannot
    // survive a truncated URL match.
    for (const auto& url : {request.videoUrl, request.audioUrl}) {
        if (url.empty()) continue;
        size_t position = 0;
        while ((position = text.find(url, position)) != std::wstring::npos) {
            text.replace(position, url.size(), L"[input]");
            position += 7;
        }
    }
    for (size_t position = 0; position < text.size();) {
        if (_wcsnicmp(text.c_str() + position, L"https://", 8) == 0 ||
            _wcsnicmp(text.c_str() + position, L"http://", 7) == 0) {
            const size_t end = text.find_first_of(L" \t\r\n\"'", position);
            text.replace(position, (end == std::wstring::npos ? text.size() : end) - position, L"[url]");
            position += 5;
        } else ++position;
    }
    if (text.size() > 2048) text.resize(2048);
    return text;
}

} // namespace

namespace media_pipeline_detail {

MediaProgressReader::MediaProgressReader(std::function<void(const MediaDownloadProgress&)> onProgress,
                                         std::function<void(std::string_view)> onDiagnostic)
    : onProgress_(std::move(onProgress)), onDiagnostic_(std::move(onDiagnostic)) {}

void MediaProgressReader::Consume(std::string_view chunk, Clock::time_point now)
{
    for (const char character : chunk) {
        if (character == '\n') EndLine(now);
        // A progress line is a few dozen bytes at most. Bounding the buffer the
        // way ProbeMedia bounds its own stops a newline-free error flood from
        // growing it without limit, and dropping the remainder of such a line
        // matches what the capture did with an oversized diagnostic before.
        else if (partial_.size() < 64 * 1024) partial_.push_back(character);
        else oversizedLine_ = true;
    }
}

void MediaProgressReader::Finish(Clock::time_point now)
{
    if (!partial_.empty()) EndLine(now);
    Report(now, true);
}

void MediaProgressReader::EndLine(Clock::time_point now)
{
    if (!oversizedLine_) {
        std::string_view line(partial_);
        // FFmpeg writes the stream in text mode, so the newline arrives as CRLF.
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        ReadLine(line, now);
    }
    partial_.clear();
    oversizedLine_ = false;
}

void MediaProgressReader::ReadLine(std::string_view line, Clock::time_point now)
{
    const size_t equals = line.find('=');
    const std::string_view key = equals == std::string_view::npos
        ? std::string_view{} : line.substr(0, equals);
    // Progress keys are lowercase identifiers. Anything else on this pipe is
    // FFmpeg's own error text, which the caller still needs verbatim, so the
    // parser hands it straight back instead of swallowing it.
    const bool progressKey = !key.empty() && std::ranges::all_of(key, [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_';
    });
    if (!progressKey) {
        if (onDiagnostic_) onDiagnostic_(line);
        return;
    }
    const std::string_view value = line.substr(equals + 1);
    uint64_t integer = 0;
    if (key == "total_size") {
        // Every value may legitimately read "N/A" until the muxer has written
        // something; an unparsable one leaves the last known figure standing.
        if (ParseUnsigned(value, integer)) { latest_.bytes = integer; pending_ = true; }
    } else if (key == "out_time_us" || key == "out_time_ms") {
        // out_time_ms is the older spelling of the same field and also carries
        // microseconds: a long-standing FFmpeg misnomer, not a different unit.
        if (ParseUnsigned(value, integer)) {
            latest_.seconds = static_cast<double>(integer) / 1000000.0;
            pending_ = true;
        }
    } else if (key == "progress") {
        // Each block is terminated by progress=continue, or progress=end for the
        // last one, which is reported whatever the rate limit would have said.
        Report(now, value == "end");
    }
}

void MediaProgressReader::Report(Clock::time_point now, bool force)
{
    if (!pending_ || !onProgress_) return;
    if (!force && reported_ && now - lastReport_ < kMediaProgressInterval) return;
    pending_ = false;
    reported_ = true;
    lastReport_ = now;
    onProgress_(latest_);
}

} // namespace media_pipeline_detail

std::vector<std::wstring> BuildMaterializeArguments(const MaterializeRequest& request)
{
    // -progress makes FFmpeg write a machine-readable key/value block to stdout
    // about twice a second, which is the only stable way to see a stream copy
    // advance: the human-readable stderr line is carriage-return separated and
    // reformatted between builds. It is a global option, so it belongs here with
    // the others, ahead of the first input.
    std::vector<std::wstring> arguments{
        L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-progress", L"pipe:1",
        L"-y", L"-xerror",
    };
    const auto appendInput = [&](const std::wstring& input) {
        if (_wcsnicmp(input.c_str(), L"http://", 7) == 0 ||
            _wcsnicmp(input.c_str(), L"https://", 8) == 0) {
            arguments.insert(arguments.end(), {
                L"-rw_timeout", L"10000000", L"-reconnect", L"1", L"-reconnect_on_network_error", L"1",
                L"-reconnect_on_http_error", L"429,5xx", L"-reconnect_delay_max", L"2",
                L"-reconnect_max_retries", L"3", L"-reconnect_delay_total_max", L"8", L"-respect_retry_after", L"0"});
        }
        arguments.insert(arguments.end(), {L"-i", input});
    };
    appendInput(request.videoUrl);
    if (!request.audioUrl.empty()) {
        appendInput(request.audioUrl);
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
        L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-y"};
    if (spec.kind == EncoderKind::HevcNvenc) {
        // hevc_nvenc creates its CUDA context when the first frame reaches the encoder,
        // not at spawn. A segmented job spawns one ffmpeg per 2 s segment, so that cost
        // landed inside every segment's first frames: measured 126 ms of stdin not being
        // drained, of which the render loop absorbed ~55 ms as a stall. Creating the
        // device up front moves the context creation to the spawn, which the segment
        // writer already does a whole segment ahead; the remaining ~59 ms is the NVENC
        // session itself. The option fails only where hevc_nvenc would fail too.
        arguments.insert(arguments.end(), {L"-init_hw_device", L"cuda=cu:0"});
    }
    arguments.insert(arguments.end(), {
        L"-f", L"rawvideo",
        L"-pix_fmt", spec.pixelFormat == EncoderPixelFormat::Nv12 ? L"nv12" : L"bgra",
        L"-video_size", std::to_wstring(spec.width) + L"x" + std::to_wstring(spec.height),
        L"-framerate", FrameRateText(spec.fps), L"-i", L"pipe:0", L"-an",
    });
    const bool nv12 = spec.pixelFormat == EncoderPixelFormat::Nv12;
    if (spec.kind == EncoderKind::HevcNvenc) {
        arguments.insert(arguments.end(), {
            L"-c:v", L"hevc_nvenc", L"-preset",
            L"p" + std::to_wstring(std::clamp<uint32_t>(spec.nvencPreset, 1u, 7u)), L"-tune", L"hq",
            L"-rc", L"vbr", L"-cq", L"16", L"-b:v", L"0",
            // The export loop measured 3.7 ms/frame of encoder back pressure at
            // 2578x1080 once decode moved to NVDEC: one NVENC session at p7 caps near
            // 150 fps. Split-frame encoding stripes each frame across every NVENC
            // engine the GPU has. `auto` and not `forced`: ffmpeg hands the value
            // straight to nvEncInitializeEncoder with no capability gate, so a driver
            // or GPU that refuses forced split encode fails encoder open, which this
            // player only sees as a broken pipe on the first frame - and that retries
            // the whole render on libx264, silently turning an HEVC export into H.264.
            // `auto` lets the encoder enable striping where it is supported and
            // costs only the preset/tune pairs NVENC declines to stripe.
            L"-split_encode_mode", L"auto",
            // NVENC takes NV12 as it stands, so an already-converted capture reaches the
            // encoder without ffmpeg touching a single pixel.
            L"-pix_fmt", nv12 ? L"nv12" : L"yuv420p"});
    } else {
        arguments.insert(arguments.end(), {
            L"-c:v", L"libx264", L"-preset", L"slow", L"-crf", L"16",
            // x264 has no NV12 input, but deinterleaving one plane is far cheaper than
            // converting a whole BGRA frame.
            L"-pix_fmt", (!nv12 && (spec.width % 2 || spec.height % 2)) ? L"yuv444p" : L"yuv420p"});
    }
    // Colorimetry is stated on BOTH paths, and the BGRA path is additionally told
    // which matrix to convert with. Until 2026-09-15 only the GPU-converted path said
    // anything, on the reasoning that only there does the player pick the matrix - but
    // the consequence was that every default-path render shipped untagged, and ffmpeg's
    // own conversion picks BT.601 for an untagged rawvideo input. A BT.709 source
    // therefore became a file that declared nothing and carried 601 pixels, which any
    // consumer assuming 709 for HD decodes wrongly.
    //
    // Note the order of the two fixes: labelling alone would be worse than the defect.
    // Tagging 601 pixels as BT.709 turns an ambiguous file into a confidently wrong
    // one, so the BGRA path gets an explicit conversion matrix and range as well, which
    // is also what makes the two paths comparable at last. The NV12 path takes no
    // filter: its pixels are already BT.709 limited range from the capture shader, and
    // a scale filter there would undo the whole point of converting on the GPU.
    if (!nv12) {
        arguments.insert(arguments.end(), {L"-vf", L"scale=out_color_matrix=bt709:out_range=tv"});
    }
    arguments.insert(arguments.end(), {
        L"-colorspace", L"bt709", L"-color_primaries", L"bt709",
        L"-color_trc", L"bt709", L"-color_range", L"tv"});
    arguments.insert(arguments.end(), {L"-f", L"matroska", output.wstring()});
    return arguments;
}

std::vector<std::wstring> BuildCachedExportArguments(const CachedExportRequest& request,
                                                     const std::filesystem::path& staging,
                                                     bool oddDimensions)
{
    const ExportFormat format = ExportFormatFor(request.output);
    std::vector<std::wstring> arguments{
        // -y applies only to the exclusively reserved staging file we own.
        L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-xerror", L"-y",
        L"-i", request.neuralVideo.wstring()};
    if (format.mkv || format.mp4) {
        // The neural video already covers only the rendered range, so the
        // source is trimmed to line its audio, subtitles and chapters up with it.
        // Input -ss seeks near the start and rebases the source to zero, but a
        // stream copy keeps the pre-roll between the container seek point and
        // the target as negative timestamps, which the muxer would then shift
        // onto the video. Output -ss 0 discards that pre-roll exactly for both
        // copied and re-encoded streams; the neural video begins at zero and
        // loses nothing. Output -t bounds the source to the rendered duration.
        const bool seek = request.rangeStartSeconds > 0.0;
        if (seek) arguments.insert(arguments.end(), {L"-ss", FrameRateText(request.rangeStartSeconds)});
        arguments.insert(arguments.end(), {L"-i", request.sourceMedia.wstring(),
            L"-map", L"0:v:0", L"-map", L"1:a?", L"-map", L"1:s?",
            L"-map_metadata", L"1", L"-map_chapters", L"1"});
        if (seek) arguments.insert(arguments.end(), {L"-ss", L"0"});
        if (request.rangeDurationSeconds > 0.0)
            arguments.insert(arguments.end(), {L"-t", FrameRateText(request.rangeDurationSeconds)});
        if (format.mkv) arguments.insert(arguments.end(), {L"-map", L"1:t?", L"-c", L"copy", L"-f", L"matroska"});
        else {
            arguments.insert(arguments.end(), {L"-c:v", L"libx264", L"-preset", L"medium", L"-crf", L"18"});
            if (oddDimensions) arguments.insert(arguments.end(), {L"-pix_fmt", L"yuv444p"});
            else arguments.insert(arguments.end(), {L"-vf", L"pad=ceil(iw/2)*2:ceil(ih/2)*2", L"-pix_fmt", L"yuv420p"});
            arguments.insert(arguments.end(), {L"-c:a", L"aac", L"-b:a", L"192k", L"-c:s", L"mov_text", L"-movflags", L"+faststart", L"-f", L"mp4"});
        }
    } else if (format.gif) {
        arguments.insert(arguments.end(), {L"-filter_complex",
            // Two-centisecond frames avoid the short-delay clamping performed
            // by common GIF viewers. fps retains holds through the final delay.
            L"[0:v:0]fps=50,split[a][b];[a]palettegen[p];[b][p]paletteuse=dither=sierra2_4a[v]",
            L"-map", L"[v]", L"-an", L"-loop", L"0", L"-fps_mode", L"passthrough", L"-f", L"gif"});
    } else {
        arguments.insert(arguments.end(), {L"-map", L"0:v:0", L"-frames:v", L"1", L"-an",
            L"-c:v", format.png ? L"png" : L"mjpeg", L"-pix_fmt", format.png ? L"rgb24" : L"yuvj444p"});
        if (format.jpeg) arguments.insert(arguments.end(), {L"-q:v", L"2"});
        arguments.insert(arguments.end(), {L"-f", L"image2", L"-update", L"1"});
    }
    arguments.push_back(staging.wstring());
    return arguments;
}

size_t ExpectedFrameBytes(const EncoderSpec& spec)
{
    if (spec.width == 0 || spec.height == 0 || !std::isfinite(spec.fps) || spec.fps <= 0.0)
        return 0;
    // The size the frames actually arrive at, which is 1.5 bytes per pixel once the
    // capture is converted on the GPU. Assuming BGRA here rejected every NV12 frame
    // the capture handed over as the wrong size.
    if (spec.pixelFormat == EncoderPixelFormat::Nv12 && (spec.width % 2 || spec.height % 2))
        return 0;
    const uint64_t bytes = EncoderFrameBytes(spec.pixelFormat, spec.width, spec.height);
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

MaterializeResult MediaMaterializer::Run(const MaterializeRequest& request, std::stop_token stop,
                                         std::function<void(const MediaDownloadProgress&)> onProgress)
{
    if (stop.stop_requested())
        return {false, MaterializeError::Cancelled, L"Source preparation was cancelled."};
    if (request.videoUrl.empty() || request.output.empty() ||
        !std::isfinite(request.expectedDurationSeconds) || request.expectedDurationSeconds < 0.0)
        return {false, MaterializeError::InvalidRequest, L"Invalid media materialization request."};
    const auto ffmpeg = FindHelper(helperDirectory_, L"ffmpeg.exe");
    if (ffmpeg.empty()) return {false, MaterializeError::HelperMissing, L"FFmpeg is unavailable."};
    // One pipe carries both the progress stream and FFmpeg's errors, so the
    // reader feeds the caller's callback and hands every other line back here.
    // The diagnostic keeps the capture's own limit semantics: once the collected
    // text passes it the text is discarded rather than grown without bound.
    constexpr size_t diagnosticLimit = 64 * 1024;
    std::string diagnostic;
    bool diagnosticOverflowed = false;
    media_pipeline_detail::MediaProgressReader reader(std::move(onProgress),
        [&](std::string_view line) {
            if (diagnosticOverflowed) return;
            if (diagnostic.size() + line.size() + 1 > diagnosticLimit) {
                diagnosticOverflowed = true;
                diagnostic.clear();
                return;
            }
            diagnostic.append(line);
            diagnostic.push_back('\n');
        });
    const CaptureResult capture = RunCapture(ffmpeg, BuildMaterializeArguments(request), stop,
        diagnosticLimit, [&](std::string_view chunk) {
            reader.Consume(chunk, std::chrono::steady_clock::now());
        });
    reader.Finish(std::chrono::steady_clock::now());
    if (!capture.started)
        return {false, MaterializeError::StartFailed, L"FFmpeg could not be started."};
    if (capture.cancelled) return {false, MaterializeError::Cancelled, L"Source preparation was cancelled."};
    std::error_code error;
    if (capture.exitCode != 0 || !std::filesystem::is_regular_file(request.output, error) || error) {
        std::wstring detail = L"FFmpeg could not prepare the source (exit " + std::to_wstring(capture.exitCode) + L").";
        const auto diagnosticText = MaterializeDiagnostic(diagnostic, request);
        if (!diagnosticText.empty()) detail += L"\n" + diagnosticText;
        return {false, MaterializeError::ProcessFailed, std::move(detail)};
    }
    if (request.expectedDurationSeconds > 0.0) {
        const ProbeResult probe = ProbeMedia(helperDirectory_, request.output, stop);
        if (stop.stop_requested())
            return {false, MaterializeError::Cancelled, L"Source preparation was cancelled."};
        // yt-dlp may round duration to a whole second. Keep the allowance fixed:
        // a percentage would conceal increasingly large gaps on longer videos.
        if (!probe.ok || probe.videoDuration100ns <= 0 ||
            double(probe.videoDuration100ns) / 10000000.0 + 1.0 < request.expectedDurationSeconds)
            return {false, MaterializeError::ProcessFailed,
                L"The prepared source video is incomplete (expected " +
                FrameRateText(request.expectedDurationSeconds) + L" seconds, measured " +
                FrameRateText(double(probe.videoDuration100ns) / 10000000.0) + L" seconds)."};
    }
    return {true, MaterializeError::None, {}};
}

CachedVideoExporter::CachedVideoExporter(std::filesystem::path helperDirectory)
    : helperDirectory_(std::move(helperDirectory)) {}

MaterializeResult CachedVideoExporter::Run(const CachedExportRequest& request, std::stop_token stop)
{
    const auto cancelled = [] {
        return MaterializeResult{false, MaterializeError::Cancelled, L"Cached video export was cancelled."};
    };
    if (stop.stop_requested()) return cancelled();
    const ExportFormat format = ExportFormatFor(request.output);
    if (request.neuralVideo.empty() || request.sourceMedia.empty() || request.output.empty() || !format.Valid())
        return {false, MaterializeError::InvalidRequest, L"Choose a new MKV, MP4, GIF, PNG or JPEG file."};
    if (!ValidRangeSeconds(request.rangeStartSeconds) || !ValidRangeSeconds(request.rangeDurationSeconds))
        return {false, MaterializeError::InvalidRequest, L"The export range is invalid."};
    std::error_code error;
    const auto neuralVideo = std::filesystem::absolute(request.neuralVideo, error);
    if (error)
        return {false, MaterializeError::InvalidRequest, L"The cached video path is unavailable."};
    const auto sourceMedia = std::filesystem::absolute(request.sourceMedia, error);
    if (error)
        return {false, MaterializeError::InvalidRequest, L"The original source path is unavailable."};
    for (const auto& input : {neuralVideo, sourceMedia}) {
        if (!std::filesystem::is_regular_file(input, error) || error)
            return {false, MaterializeError::InvalidRequest, L"The cached video or original source is unavailable."};
    }
    const auto output = std::filesystem::absolute(request.output, error);
    if (error || !std::filesystem::is_directory(output.parent_path(), error) || error)
        return {false, MaterializeError::InvalidRequest, L"The export folder is unavailable."};
    // This also rejects input aliases, directories and reparse points. The final
    // no-replace rename below closes the race with a file created during export.
    if (GetFileAttributesW(output.c_str()) != INVALID_FILE_ATTRIBUTES)
        return {false, MaterializeError::InvalidRequest, L"The export file already exists. Choose a new filename."};
    const auto ffmpeg = FindHelper(helperDirectory_, L"ffmpeg.exe");
    if (ffmpeg.empty()) return {false, MaterializeError::HelperMissing, L"FFmpeg is unavailable."};
    bool oddDimensions = false;
    if (format.mp4) {
        const ProbeResult neuralMetadata = ProbeMedia(helperDirectory_, neuralVideo, stop,
                                                       MediaProbeMode::CachedMetadata);
        if (!neuralMetadata.ok)
            return {false, MaterializeError::ProcessFailed, L"The cached neural media could not be inspected."};
        oddDimensions = neuralMetadata.width % 2 || neuralMetadata.height % 2;
    }

    struct StagingFile {
        std::filesystem::path path;
        ~StagingFile() { if (!path.empty()) { std::error_code ignored; std::filesystem::remove(path, ignored); } }
    } staging;
    static std::atomic_uint64_t sequence{};
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = output.parent_path() / (L".dlss-export-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
            L"-" + std::to_wstring(sequence.fetch_add(1)) + L".tmp");
        HANDLE file = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            staging.path = candidate;
            break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    if (staging.path.empty())
        return {false, MaterializeError::ProcessFailed, L"A temporary export file could not be created in the selected folder."};
    if (stop.stop_requested()) return cancelled();
    CachedExportRequest resolved = request;
    resolved.neuralVideo = neuralVideo;
    resolved.sourceMedia = sourceMedia;
    const auto arguments = BuildCachedExportArguments(resolved, staging.path, oddDimensions);
    // Attachments (such as subtitle fonts) travel with the source subtitles.
    // Unsupported codecs fail the entire export; no subtitle is burned in.
    const CaptureResult capture = RunCapture(ffmpeg, arguments, stop, 64 * 1024);
    if (capture.cancelled || stop.stop_requested()) return cancelled();
    if (!capture.started)
        return {false, MaterializeError::StartFailed, L"FFmpeg could not be started."};
    const auto bytes = std::filesystem::file_size(staging.path, error);
    if (capture.exitCode != 0 || error || bytes == 0) {
        std::wstring detail = L"FFmpeg could not export this media in the selected format.";
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, capture.output.data(),
                                               static_cast<int>(capture.output.size()), nullptr, 0);
        if (length > 0) {
            std::wstring diagnostic(static_cast<size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, capture.output.data(),
                               static_cast<int>(capture.output.size()), diagnostic.data(), length);
            detail += L"\n" + diagnostic;
        }
        return {false, MaterializeError::ProcessFailed, std::move(detail)};
    }
    if (stop.stop_requested()) return cancelled();
    if (!MoveFileExW(staging.path.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH))
        return {false, MaterializeError::ProcessFailed,
            L"The completed export could not be saved. Check folder access and choose a filename that does not already exist."};
    staging.path.clear();
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
    const size_t bytes = ExpectedFrameBytes(spec);
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
    HANDLE interruptJob=nullptr;
    if(!impl_->process.job||!DuplicateHandle(GetCurrentProcess(),impl_->process.job,
        GetCurrentProcess(),&interruptJob,0,FALSE,DUPLICATE_SAME_ACCESS))
        return EncodeError::WriteFailed;
    OwnedHandle interruptHandle(interruptJob);
    std::stop_callback interrupt(stop,[job=interruptHandle.get()]{
        TerminateJobObject(job,ERROR_CANCELLED);
    });
    // When the job is terminated under a blocked WriteFile, the kernel completes that
    // write as a success for every byte it had accepted rather than failing it, so the
    // stop has to be re-checked after each write, not only before. With a single
    // remaining chunk the loop would otherwise report None for a frame nobody read.
    size_t offset = 0;
    while (offset < bgra.size()) {
        if (stop.stop_requested()) { Cancel(); return EncodeError::Cancelled; }
        const DWORD wanted = static_cast<DWORD>(std::min<size_t>(bgra.size() - offset, 16 * 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(impl_->process.stdinWrite, bgra.data() + offset, wanted, &written, nullptr) ||
            written == 0) {
            Cancel();
            return stop.stop_requested() ? EncodeError::Cancelled : EncodeError::WriteFailed;
        }
        offset += written;
    }
    if (stop.stop_requested()) { Cancel(); return EncodeError::Cancelled; }
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

namespace {

std::string NarrowUtf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string text(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(),
                        size, nullptr, nullptr);
    return text;
}

// One `file` directive per part. FFmpeg reads the list as UTF-8 and treats the
// quoted value literally, so only the quote itself needs escaping; forward
// slashes avoid any backslash-escape ambiguity.
std::string ConcatListText(std::span<const std::filesystem::path> parts)
{
    std::string text;
    for (const auto& part : parts) {
        std::error_code error;
        std::wstring absolute = std::filesystem::absolute(part, error).wstring();
        if (error || absolute.empty()) return {};
        for (wchar_t& character : absolute) if (character == L'\\') character = L'/';
        const std::string narrow = NarrowUtf8(absolute);
        if (narrow.empty()) return {};
        text += "file '";
        for (const char character : narrow) {
            if (character == '\'') text += "'\\''";
            else text.push_back(character);
        }
        text += "'\n";
    }
    return text;
}

bool WriteWholeFile(const std::filesystem::path& path, std::string_view text)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    size_t offset = 0;
    bool ok = true;
    while (ok && offset < text.size()) {
        const DWORD wanted = static_cast<DWORD>(std::min<size_t>(text.size() - offset, 1024 * 1024));
        DWORD written = 0;
        ok = WriteFile(file, text.data() + offset, wanted, &written, nullptr) && written != 0;
        offset += written;
    }
    CloseHandle(file);
    return ok;
}

} // namespace

EncodeError ConcatenateMedia(const std::filesystem::path& helperDirectory,
                             std::span<const std::filesystem::path> parts,
                             const std::filesystem::path& output,
                             std::stop_token stop)
{
    if (parts.empty() || output.empty()) return EncodeError::InvalidSpecification;
    std::error_code error;
    for (const auto& part : parts) {
        if (part.empty() || !std::filesystem::is_regular_file(part, error) || error)
            return EncodeError::InvalidSpecification;
    }
    const auto ffmpeg = FindHelper(helperDirectory, L"ffmpeg.exe");
    if (ffmpeg.empty()) return EncodeError::HelperMissing;
    if (stop.stop_requested()) return EncodeError::Cancelled;
    const std::string text = ConcatListText(parts);
    if (text.empty()) return EncodeError::InvalidSpecification;
    // Beside the output: the staging directory is ours and stays writable.
    const std::filesystem::path list = output.parent_path() / (output.filename().wstring() + L".concat.txt");
    if (!WriteWholeFile(list, text)) return EncodeError::StartFailed;
    const std::vector<std::wstring> arguments{
        L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-y",
        L"-f", L"concat", L"-safe", L"0", L"-i", list.wstring(),
        L"-c", L"copy", L"-f", L"matroska", output.wstring()};
    const CaptureResult capture = RunCapture(ffmpeg, arguments, stop, 64 * 1024);
    std::filesystem::remove(list, error);
    if (!capture.started) return EncodeError::StartFailed;
    if (capture.cancelled || stop.stop_requested()) return EncodeError::Cancelled;
    if (capture.exitCode != 0 || !std::filesystem::is_regular_file(output, error) || error)
        return EncodeError::FinishFailed;
    return EncodeError::None;
}

ProbeResult ProbeMedia(const std::filesystem::path& helperDirectory,
                       const std::filesystem::path& media,
                       std::stop_token stop, MediaProbeMode mode)
{
    ProbeResult result;
    const bool fullValidation=mode==MediaProbeMode::FullValidation;
    const auto ffprobe = FindHelper(helperDirectory, L"ffprobe.exe");
    const auto ffmpeg = FindHelper(helperDirectory, L"ffmpeg.exe");
    if (ffprobe.empty() || (fullValidation&&ffmpeg.empty())) { result.detail = L"FFmpeg tools are unavailable."; return result; }
    std::vector<std::wstring> probeArguments{
        L"-v", L"error", L"-select_streams", L"v:0",
        L"-show_entries", fullValidation?L"frame=best_effort_timestamp_time,duration_time,pkt_duration_time:stream=width,height,nb_read_frames:format=duration":L"stream=width,height:format=duration",
        L"-of", L"default=noprint_wrappers=1:nokey=0", media.wstring()};
    if(fullValidation)probeArguments.insert(probeArguments.begin(),L"-count_frames");
    double durationSeconds = 0.0;
    double firstVideoTimestamp = std::numeric_limits<double>::infinity();
    double videoEnd = -std::numeric_limits<double>::infinity();
    double frameTimestamp = std::numeric_limits<double>::quiet_NaN();
    const auto parseLine = [&](std::string_view line) {
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
            else if (key == "best_effort_timestamp_time") {
                try { frameTimestamp = std::stod(std::string(value)); }
                catch (...) { frameTimestamp = std::numeric_limits<double>::quiet_NaN(); }
                if (std::isfinite(frameTimestamp)) {
                    firstVideoTimestamp = std::min(firstVideoTimestamp, frameTimestamp);
                    videoEnd = std::max(videoEnd, frameTimestamp);
                }
            }
            else if ((key == "duration_time" || key == "pkt_duration_time") && std::isfinite(frameTimestamp)) {
                double frameDuration = 0.0;
                try { frameDuration = std::stod(std::string(value)); } catch (...) {}
                if (std::isfinite(frameDuration) && frameDuration > 0.0)
                    videoEnd = std::max(videoEnd, frameTimestamp + frameDuration);
            }
        }
    };
    // Stream frame metadata so a full-length video does not need a proportional
    // memory buffer or hit the bounded diagnostic capture limit.
    std::string pending;
    bool oversizedLine = false;
    const CaptureResult capture = RunCapture(ffprobe, probeArguments, stop, 64 * 1024,
        [&](std::string_view chunk) {
            for (const char character : chunk) {
                if (character == '\n') {
                    if (!oversizedLine) parseLine(pending);
                    pending.clear(); oversizedLine = false;
                } else if (pending.size() < 64 * 1024) pending.push_back(character);
                else oversizedLine = true;
            }
        });
    if (!pending.empty() && !oversizedLine) parseLine(pending);
    if (!capture.started || capture.cancelled || capture.exitCode != 0) {
        result.detail = capture.cancelled ? L"Media validation was cancelled." : L"FFprobe validation failed.";
        return result;
    }
    const double videoSeconds = videoEnd - firstVideoTimestamp;
    if (std::isfinite(videoSeconds) && videoSeconds > 0.0 && videoSeconds < double(INT64_MAX) / 10000000.0)
        result.videoDuration100ns = std::llround(videoSeconds * 10000000.0);
    if(std::isfinite(durationSeconds)&&durationSeconds>0.0&&durationSeconds<double(INT64_MAX)/10000000.0)
        result.duration100ns = std::llround(durationSeconds * 10000000.0);
    if (!result.width || !result.height || (fullValidation&&!result.frameCount) || result.duration100ns <= 0) {
        result.detail = L"FFprobe returned incomplete media metadata.";
        return result;
    }
    // Only for entries already checked against their persisted content hash and
    // complete manifest. Full frame/end-of-stream validation stays on promotion.
    if(!fullValidation){result.ok=true;return result;}
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
