#include "VideoDecoder.h"
#include "Log.h"
#include <propvarutil.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <vector>
#include <iterator>
#include <cstring>
#include <thread>
#include <utility>
#include <system_error>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static std::wstring Quote(const std::wstring& s) {
    // Windows filenames cannot contain a literal quote character, so this is
    // sufficient for the executable and video paths used by this player.
    return L"\"" + s + L"\"";
}

static bool ParseRate(const std::string& text, double& out) {
    const size_t slash = text.find('/');
    try {
        if (slash == std::string::npos) {
            const double v = std::stod(text);
            if (std::isfinite(v) && v > 0.0) { out = v; return true; }
            return false;
        }
        const double n = std::stod(text.substr(0, slash));
        const double d = std::stod(text.substr(slash + 1));
        if (d == 0.0) return false;
        const double v = n / d;
        if (std::isfinite(v) && v > 0.0) { out = v; return true; }
    } catch (...) {}
    return false;
}

VideoDecoder::~VideoDecoder() { Close(); }

void VideoDecoder::Swap(VideoDecoder& other) noexcept {
    const bool restartThis = other.m_frameQueueEnabled;
    const bool restartOther = m_frameQueueEnabled;
    StopFrameQueue();
    other.StopFrameQueue();
    using std::swap;
    swap(m_backend,other.m_backend);swap(m_reader,other.m_reader);swap(m_path,other.m_path);
    swap(m_width,other.m_width);swap(m_height,other.m_height);swap(m_nativeWidth,other.m_nativeWidth);swap(m_nativeHeight,other.m_nativeHeight);swap(m_stride,other.m_stride);
    swap(m_fps,other.m_fps);swap(m_durationSec,other.m_durationSec);swap(m_displayAspect,other.m_displayAspect);
    swap(m_ffmpegExe,other.m_ffmpegExe);swap(m_ffprobeExe,other.m_ffprobeExe);swap(m_ffmpegProcess,other.m_ffmpegProcess);swap(m_ffmpegStdout,other.m_ffmpegStdout);swap(m_ffmpegJob,other.m_ffmpegJob);
    swap(m_ffmpegFrameIndex,other.m_ffmpegFrameIndex);swap(m_ffmpegSeekBase100ns,other.m_ffmpegSeekBase100ns);swap(m_ffmpegAcceleration,other.m_ffmpegAcceleration);swap(m_sourceKind,other.m_sourceKind);
    swap(m_pendingFrame,other.m_pendingFrame);swap(m_pendingFrameBytes,other.m_pendingFrameBytes);swap(m_lastFrameByte,other.m_lastFrameByte);swap(m_networkStallTimeout,other.m_networkStallTimeout);swap(m_probeTimeout,other.m_probeTimeout);
#ifdef VIDEO_DECODER_TESTING
    swap(m_helperDirectory,other.m_helperDirectory);
    swap(m_failureStage,other.m_failureStage);
#endif
    if(restartThis)StartFrameQueue();
    if(restartOther)other.StartFrameQueue();
}

const wchar_t* VideoDecoder::BackendName() const {
    switch (m_backend) {
    case Backend::FFmpeg: return L"FFmpeg";
    case Backend::MediaFoundation: return L"Media Foundation";
    default: return L"None";
    }
}

void VideoDecoder::Close() {
    StopFrameQueue();
    StopFFmpeg();
    m_reader.Reset();
    m_backend = Backend::None;
}

bool VideoDecoder::Open(const std::wstring& path, MediaSourceKind sourceKind, std::stop_token stop) {
    return OpenImpl(path,sourceKind,stop,true);
}

bool VideoDecoder::OpenSequential(const std::wstring& path, MediaSourceKind sourceKind,
                                  std::stop_token stop) {
    return OpenImpl(path,sourceKind,stop,false);
}

bool VideoDecoder::OpenImpl(const std::wstring& path, MediaSourceKind sourceKind,
                            std::stop_token stop, bool queueFrames) {
    Close();
    m_path = path;
    m_width = m_height = 0;
    m_nativeWidth = m_nativeHeight = 0;
    m_stride = 0;
    m_fps = 30.0;
    m_durationSec = 0.0;
    m_displayAspect = 0.0;
    m_sourceKind = sourceKind;

    LOG("Opening video. Decoder preference: FFmpeg -> Windows Media Foundation");

    // FFmpeg is intentionally preferred. It makes playback independent from
    // optional Microsoft Store codec packs and handles MKV/WebM/AV1/HEVC/etc.
    if (OpenFFmpeg(path, stop,queueFrames?FFmpegAcceleration::Cuda:
                                      FFmpegAcceleration::Software)) {
        m_backend = Backend::FFmpeg;
        if(queueFrames)StartFrameQueue();
        LOG("Video decoder selected: FFmpeg");
        return true;
    }

    if (DecoderPolicyForSource(sourceKind) == DecoderOpenPolicy::FfmpegOnly) {
        LOG("FFmpeg could not open the YouTube stream; Media Foundation fallback is disabled.");
        return false;
    }

    LOG("FFmpeg backend unavailable or rejected the file; trying Media Foundation.");
    if (OpenMediaFoundation(path)) {
        m_backend = Backend::MediaFoundation;
        LOG("Video decoder selected: Windows Media Foundation");
        return true;
    }

    LOG("All video decoder backends failed.");
    return false;
}

std::wstring VideoDecoder::FindTool(const wchar_t* exeName) const {
#ifdef VIDEO_DECODER_TESTING
    if(!m_helperDirectory.empty()){
        const fs::path candidate=fs::path(m_helperDirectory)/exeName;std::error_code ec;
        if(fs::is_regular_file(candidate,ec))return candidate.wstring();
        return L"";
    }
#endif
    wchar_t modulePath[32768]{};
    if (GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)))) {
        const fs::path base = fs::path(modulePath).parent_path();
        const fs::path candidates[] = {
            base / exeName,
            base / L"ffmpeg" / exeName,
            base / L"ffmpeg" / L"bin" / exeName,
            base.parent_path() / L"ffmpeg" / L"bin" / exeName
        };
        for (const auto& c : candidates) {
            std::error_code ec;
            if (fs::is_regular_file(c, ec)) return c.wstring();
        }
    }

    wchar_t found[32768]{};
    const DWORD n = SearchPathW(nullptr, exeName, nullptr,
                                static_cast<DWORD>(std::size(found)), found, nullptr);
    if (n && n < std::size(found)) return found;
    return L"";
}

bool VideoDecoder::RunCapture(const std::wstring& exe, const std::wstring& arguments,
                              std::string& output, DWORD* exitCode,
                              std::stop_token stop, std::chrono::milliseconds timeout) {
    output.clear();
    if (exe.empty()) return false;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nul == INVALID_HANDLE_VALUE) nul = nullptr;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = writePipe;
    si.hStdError = nul;

    PROCESS_INFORMATION pi{};
    std::wstring command = Quote(exe) + L" " + arguments;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(job); job = nullptr;
        }
    }
    const BOOL ok = job && CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr,
                                   TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (nul) CloseHandle(nul);

    if (!ok) {
        CloseHandle(readPipe);
        if (job) CloseHandle(job);
        return false;
    }
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);WaitForSingleObject(pi.hProcess,500);
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(readPipe);CloseHandle(job);return false;
    }
    DWORD resumeResult=static_cast<DWORD>(-1);
#ifdef VIDEO_DECODER_TESTING
    if(m_failureStage!=FailureStage::ProbeResume)
#endif
    {
        resumeResult=ResumeThread(pi.hThread);
    }
    if(resumeResult==static_cast<DWORD>(-1)){
        const DWORD resumeError=GetLastError();
        LOG("ResumeThread(ffprobe) failed winerr="<<resumeError);
        if(!TerminateJobObject(job,ERROR_PROCESS_ABORTED))
            TerminateProcess(pi.hProcess,ERROR_PROCESS_ABORTED);
        WaitForSingleObject(pi.hProcess,500);
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(readPipe);CloseHandle(job);
        if(exitCode)*exitCode=ERROR_PROCESS_ABORTED;
        return false;
    }
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    bool cancelled=false,timedOut=false,pipeError=false;
    char buf[8192];
    for (;;) {
        DWORD available=0;
        if (!PeekNamedPipe(readPipe,nullptr,0,nullptr,&available,nullptr)) {
            const DWORD pipeFailure=GetLastError();
            if(pipeFailure==ERROR_BROKEN_PIPE&&WaitForSingleObject(pi.hProcess,0)==WAIT_OBJECT_0)break;
            pipeError=true;break;
        }
        while(available>0){
            const DWORD want=std::min<DWORD>(available,sizeof(buf));DWORD got=0;
            if(!ReadFile(readPipe,buf,want,&got,nullptr)){pipeError=true;break;}
            if(got==0)break;output.append(buf,buf+got);available-=got;
        }
        if(pipeError)break;
        if(WaitForSingleObject(pi.hProcess,0)==WAIT_OBJECT_0){
            DWORD remaining=0;if(!PeekNamedPipe(readPipe,nullptr,0,nullptr,&remaining,nullptr)||remaining==0)break;
        }
        if(stop.stop_requested()){cancelled=true;break;}
        if(std::chrono::steady_clock::now()>=deadline){timedOut=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if(cancelled||timedOut||pipeError){TerminateJobObject(job,1);WaitForSingleObject(pi.hProcess,500);}
    CloseHandle(readPipe);

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exitCode) *exitCode = code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    return !cancelled&&!timedOut&&!pipeError&&code == 0;
}

bool VideoDecoder::ProbeFFmpeg(const std::wstring& path, std::stop_token stop) {
    std::wstring args =
        L"-v error -select_streams v:0 "
        L"-show_entries stream=width,height,display_aspect_ratio,sample_aspect_ratio,avg_frame_rate,r_frame_rate,duration:format=duration "
        L"-of default=noprint_wrappers=1 " + Quote(path);

    std::string text;
    DWORD code = 0;
    const auto timeout=m_sourceKind==MediaSourceKind::YouTube?m_probeTimeout:std::chrono::milliseconds(30000);
    if (!RunCapture(m_ffprobeExe, args, text, &code, stop, timeout)) {
        LOG("ffprobe failed, exitCode=" << code);
        return false;
    }

    uint32_t width = 0, height = 0;
    double avgRate = 0.0, rawRate = 0.0, duration = 0.0;
    double displayAspect = 0.0, sampleAspect = 1.0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        try {
            if (key == "width") width = static_cast<uint32_t>(std::stoul(value));
            else if (key == "height") height = static_cast<uint32_t>(std::stoul(value));
            else if (key == "display_aspect_ratio" && value != "N/A") {
                const size_t c=value.find(':'); if(c!=std::string::npos){ double a=std::stod(value.substr(0,c)), b=std::stod(value.substr(c+1)); if(b>0) displayAspect=a/b; }
            }
            else if (key == "sample_aspect_ratio" && value != "N/A") {
                const size_t c=value.find(':'); if(c!=std::string::npos){ double a=std::stod(value.substr(0,c)), b=std::stod(value.substr(c+1)); if(b>0) sampleAspect=a/b; }
            }
            else if (key == "avg_frame_rate") ParseRate(value, avgRate);
            else if (key == "r_frame_rate") ParseRate(value, rawRate);
            else if (key == "duration" && value != "N/A" && duration <= 0.0) duration = std::stod(value);
        } catch (...) {}
    }

    if (!width || !height) {
        LOG("ffprobe returned no usable video dimensions.");
        return false;
    }

    m_nativeWidth = width;
    m_nativeHeight = height;
    m_width = width;
    m_height = height;
    m_stride = static_cast<int32_t>(m_width * 4u);
    m_fps = avgRate > 0.0 ? avgRate : (rawRate > 0.0 ? rawRate : 30.0);
    m_durationSec = (std::isfinite(duration) && duration > 0.0) ? duration : 0.0;
    if (std::isfinite(displayAspect) && displayAspect > 0.1) m_displayAspect = displayAspect;
    else m_displayAspect = (double(m_width) * sampleAspect) / double(m_height);

    // Avoid pathological metadata causing gigantic pacing delays/CPU usage.
    m_fps = std::clamp(m_fps, 1.0, 240.0);

    LOG("ffprobe: " << m_width << "x" << m_height << " DAR=" << m_displayAspect << " @ " << m_fps
        << " fps, duration=" << m_durationSec);
    return true;
}

bool VideoDecoder::StartFFmpeg(double seekSeconds, FFmpegAcceleration acceleration) {
    StopFFmpeg();
    seekSeconds = std::max(0.0, seekSeconds);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 4 * 1024 * 1024)) {
        LOG("CreatePipe for ffmpeg failed winerr=" << GetLastError());
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nul == INVALID_HANDLE_VALUE) nul = nullptr;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = writePipe;
    si.hStdError = nul;

    std::wostringstream args;
    args << L"-hide_banner -loglevel error -nostdin -threads 0 ";
    if (acceleration == FFmpegAcceleration::Cuda)
        args << L"-hwaccel cuda -hwaccel_output_format cuda ";
    else if (acceleration == FFmpegAcceleration::D3D11Va)
        args << L"-hwaccel d3d11va -hwaccel_output_format d3d11 ";
    if (seekSeconds > 0.0)
        args << L"-ss " << std::fixed << std::setprecision(6) << seekSeconds << L" ";
    args << L"-i " << Quote(m_path)
         << L" -map 0:v:0 -an -sn -dn ";
    if (acceleration == FFmpegAcceleration::Cuda)
        args << L"-vf scale_cuda=" << m_width << L":" << m_height
             << L":format=nv12:interp_algo=bicubic:passthrough=0,hwdownload,format=nv12,format=bgra ";
    else if (acceleration == FFmpegAcceleration::D3D11Va)
        args << L"-vf hwdownload,format=nv12,scale=" << m_width << L":" << m_height
             << L":flags=bicubic,format=bgra ";
    else if (m_nativeWidth && m_nativeHeight && (m_width != m_nativeWidth || m_height != m_nativeHeight))
        args << L"-vf scale=" << m_width << L":" << m_height << L":flags=bicubic ";
    args << L"-pix_fmt bgra -fps_mode cfr -r "
         << std::fixed << std::setprecision(6) << m_fps
         << L" -f rawvideo pipe:1";

    std::wstring command = Quote(m_ffmpegExe) + L" " + args.str();
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    if(job){JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){CloseHandle(job);job=nullptr;}}
    PROCESS_INFORMATION pi{};
    const BOOL ok = job&&CreateProcessW(m_ffmpegExe.c_str(), mutableCommand.data(), nullptr, nullptr,
                                   TRUE, CREATE_NO_WINDOW|CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (nul) CloseHandle(nul);

    if (!ok) {
        LOG("CreateProcess(ffmpeg) failed winerr=" << GetLastError());
        CloseHandle(readPipe);
        if(job)CloseHandle(job);
        return false;
    }

    if(!AssignProcessToJobObject(job,pi.hProcess)){
        TerminateProcess(pi.hProcess,1);WaitForSingleObject(pi.hProcess,500);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(readPipe);CloseHandle(job);return false;
    }
    DWORD resumeResult=static_cast<DWORD>(-1);
#ifdef VIDEO_DECODER_TESTING
    if(m_failureStage!=FailureStage::DecodeResume)
#endif
    {
        resumeResult=ResumeThread(pi.hThread);
    }
    if(resumeResult==static_cast<DWORD>(-1)){
        const DWORD resumeError=GetLastError();
        LOG("ResumeThread(ffmpeg) failed winerr="<<resumeError);
        if(!TerminateJobObject(job,ERROR_PROCESS_ABORTED))
            TerminateProcess(pi.hProcess,ERROR_PROCESS_ABORTED);
        WaitForSingleObject(pi.hProcess,500);
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(readPipe);CloseHandle(job);
        return false;
    }

    CloseHandle(pi.hThread);
    m_ffmpegProcess = pi.hProcess;
    m_ffmpegStdout = readPipe;
    m_ffmpegJob = job;
    m_ffmpegFrameIndex = 0;
    m_ffmpegSeekBase100ns = static_cast<int64_t>(seekSeconds * 10000000.0);
    m_ffmpegAcceleration = acceleration;
    m_pendingFrame.clear();m_pendingFrameBytes=0;m_lastFrameByte=std::chrono::steady_clock::now();
    const char* accelerationName = acceleration == FFmpegAcceleration::Cuda ? "CUDA decode + GPU scale" :
        acceleration == FFmpegAcceleration::D3D11Va ? "D3D11VA decode" : "software decode";
    LOG("FFmpeg raw BGRA process started with " << accelerationName << ".");
    return true;
}

void VideoDecoder::StopFFmpeg(DWORD waitTimeout) {
    if (m_ffmpegProcess) {
        DWORD code = 0;
        if (GetExitCodeProcess(m_ffmpegProcess, &code) && code == STILL_ACTIVE) {
            if(m_ffmpegJob)TerminateJobObject(m_ffmpegJob,0);else TerminateProcess(m_ffmpegProcess, 0);
            WaitForSingleObject(m_ffmpegProcess, waitTimeout);
        }
        CloseHandle(m_ffmpegProcess);
        m_ffmpegProcess = nullptr;
    }
    if(m_ffmpegJob){CloseHandle(m_ffmpegJob);m_ffmpegJob=nullptr;}
    if (m_ffmpegStdout) {CloseHandle(m_ffmpegStdout);m_ffmpegStdout = nullptr;}
    m_pendingFrame.clear();m_pendingFrameBytes=0;
}

bool VideoDecoder::OpenFFmpeg(const std::wstring& path, std::stop_token stop,
                              FFmpegAcceleration initialAcceleration) {
    m_ffmpegExe = FindTool(L"ffmpeg.exe");
    m_ffprobeExe = FindTool(L"ffprobe.exe");
    if (m_ffmpegExe.empty() || m_ffprobeExe.empty()) {
        LOG("Bundled/system FFmpeg tools not found. ffmpeg=" << (!m_ffmpegExe.empty())
            << " ffprobe=" << (!m_ffprobeExe.empty()));
        return false;
    }

    LOG("FFmpeg executable detected.");
    if (!ProbeFFmpeg(path,stop)||stop.stop_requested()) return false;
    return StartFFmpeg(0.0,initialAcceleration);
}

bool VideoDecoder::TryNextFFmpegAcceleration(DWORD exitCode) {
    if (exitCode == 0 || exitCode == STILL_ACTIVE || m_ffmpegAcceleration == FFmpegAcceleration::Software)
        return false;
    const FFmpegAcceleration next = m_ffmpegAcceleration == FFmpegAcceleration::Cuda ?
        FFmpegAcceleration::D3D11Va : FFmpegAcceleration::Software;
    const double resumeSeconds = static_cast<double>(m_ffmpegSeekBase100ns) * 1e-7 +
        static_cast<double>(m_ffmpegFrameIndex) / std::max(1.0, m_fps);
    LOG("FFmpeg hardware path exited with code " << exitCode << "; trying " <<
        (next == FFmpegAcceleration::D3D11Va ? "D3D11VA" : "software") << " fallback.");
    return StartFFmpeg(resumeSeconds, next);
}

bool VideoDecoder::ReadNextFFmpeg(VideoFrame& out) {
    for(;;){
        const VideoReadResult result=ReadNextFFmpegAvailable(out,{});
        if(result==VideoReadResult::FrameReady)return true;
        if(result!=VideoReadResult::NotReady)return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

VideoReadResult VideoDecoder::ClassifyFFmpegEnd(DWORD exitCode) {
    if (!m_pendingFrameBytes) return VideoReadResult::EndOfStream;
    const double completedSeconds = static_cast<double>(m_ffmpegSeekBase100ns) * 1e-7 +
        static_cast<double>(m_ffmpegFrameIndex) / std::max(1.0, m_fps);
    const double endTolerance = std::max(0.05, 1.5 / std::max(1.0, m_fps));
    if (m_sourceKind == MediaSourceKind::YouTube && exitCode == 0 && m_durationSec > 0.0 &&
        completedSeconds + endTolerance >= m_durationSec) {
        LOG("Discarding an incomplete trailing raw frame after the expected YouTube duration.");
        m_pendingFrameBytes = 0;
        return VideoReadResult::EndOfStream;
    }
    LOG("FFmpeg ended in the middle of a raw video frame. exitCode="<<exitCode
        <<" frameIndex="<<m_ffmpegFrameIndex<<" pendingBytes="<<m_pendingFrameBytes);
    return VideoReadResult::Error;
}

VideoReadResult VideoDecoder::ReadNextFFmpegProcessAvailable(VideoFrame& out,std::stop_token stop) {
    if (!m_ffmpegStdout) return VideoReadResult::EndOfStream;
    const size_t frameBytes = static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4u;
    if (!frameBytes) return VideoReadResult::Error;
    if(stop.stop_requested())return VideoReadResult::Cancelled;
    if(m_pendingFrame.size()!=frameBytes){m_pendingFrame.resize(frameBytes);m_pendingFrameBytes=0;m_lastFrameByte=std::chrono::steady_clock::now();}

    DWORD available=0;
    if(!PeekNamedPipe(m_ffmpegStdout,nullptr,0,nullptr,&available,nullptr)){
        if(GetLastError()==ERROR_BROKEN_PIPE&&m_ffmpegProcess&&WaitForSingleObject(m_ffmpegProcess,0)==WAIT_OBJECT_0){
            DWORD exitCode=1;GetExitCodeProcess(m_ffmpegProcess,&exitCode);
            if(TryNextFFmpegAcceleration(exitCode))return VideoReadResult::NotReady;
            return ClassifyFFmpegEnd(exitCode);
        }
        return VideoReadResult::Error;
    }
    DWORD got=0;
    if(available>0){
        const DWORD want=static_cast<DWORD>(std::min<size_t>({frameBytes-m_pendingFrameBytes,static_cast<size_t>(available),size_t{4u<<20}}));
        if(want&&!ReadFile(m_ffmpegStdout,m_pendingFrame.data()+m_pendingFrameBytes,want,&got,nullptr))return VideoReadResult::Error;
        if(got){m_pendingFrameBytes+=got;m_lastFrameByte=std::chrono::steady_clock::now();}
    }

    if(m_pendingFrameBytes<frameBytes){
        if(stop.stop_requested())return VideoReadResult::Cancelled;
        if(m_sourceKind==MediaSourceKind::YouTube&&std::chrono::steady_clock::now()-m_lastFrameByte>=m_networkStallTimeout){
            LOG("FFmpeg YouTube stream stalled before a complete frame.");StopFFmpeg(0);return VideoReadResult::Stalled;
        }
        if(m_ffmpegProcess&&WaitForSingleObject(m_ffmpegProcess,0)==WAIT_OBJECT_0){
            DWORD remaining=0;
            if(PeekNamedPipe(m_ffmpegStdout,nullptr,0,nullptr,&remaining,nullptr)&&remaining>0)
                return VideoReadResult::NotReady;
            DWORD exitCode=1;GetExitCodeProcess(m_ffmpegProcess,&exitCode);
            if(TryNextFFmpegAcceleration(exitCode))return VideoReadResult::NotReady;
            return ClassifyFFmpegEnd(exitCode);
        }
        return VideoReadResult::NotReady;
    }

    out.bgra.swap(m_pendingFrame);m_pendingFrame.clear();m_pendingFrameBytes=0;

    out.timestamp100ns = m_ffmpegSeekBase100ns +
        static_cast<int64_t>((static_cast<double>(m_ffmpegFrameIndex) / m_fps) * 10000000.0);
    out.discontinuity = (m_ffmpegFrameIndex == 0 && m_ffmpegSeekBase100ns != 0);
    ++m_ffmpegFrameIndex;
    return VideoReadResult::FrameReady;
}

void VideoDecoder::StartFrameQueue() {
    if (m_backend != Backend::FFmpeg || m_frameThread.joinable()) return;
    {
        std::lock_guard lock(m_frameMutex);
        m_frameQueue.clear();
        m_frameTerminal = VideoReadResult::NotReady;
        m_frameQueueEnabled = true;
    }
    try {
        m_frameThread = std::jthread([this](std::stop_token stop) { FrameQueueLoop(stop); });
    } catch (const std::system_error& error) {
        std::lock_guard lock(m_frameMutex);
        m_frameQueueEnabled = false;
        LOG("Decoded-frame queue could not start; using synchronous reads. error=" << error.code().value());
    }
}

void VideoDecoder::StopFrameQueue() {
    if (m_frameThread.joinable()) {
        m_frameThread.request_stop();
        m_frameCv.notify_all();
        m_frameThread.join();
    }
    std::lock_guard lock(m_frameMutex);
    m_frameQueue.clear();
    m_frameTerminal = VideoReadResult::NotReady;
    m_frameQueueEnabled = false;
}

void VideoDecoder::FrameQueueLoop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(m_frameMutex);
            if (!m_frameCv.wait(lock, stop, [this] { return m_frameQueue.size() < FrameQueueCapacity; }))
                break;
        }

        VideoFrame frame;
        const VideoReadResult result = ReadNextFFmpegProcessAvailable(frame, stop);
        if (result == VideoReadResult::FrameReady) {
            std::lock_guard lock(m_frameMutex);
            if (stop.stop_requested()) break;
            m_frameQueue.push_back(std::move(frame));
            m_frameCv.notify_all();
        } else if (result == VideoReadResult::NotReady) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else if (result != VideoReadResult::Cancelled) {
            std::lock_guard lock(m_frameMutex);
            m_frameTerminal = result;
            m_frameCv.notify_all();
            break;
        }
    }
}

VideoReadResult VideoDecoder::ReadNextFFmpegAvailable(VideoFrame& out,std::stop_token stop) {
    {
        std::lock_guard lock(m_frameMutex);
        if (m_frameQueueEnabled) {
            if (stop.stop_requested()) return VideoReadResult::Cancelled;
            if (!m_frameQueue.empty()) {
                out = std::move(m_frameQueue.front());
                m_frameQueue.pop_front();
                m_frameCv.notify_all();
                return VideoReadResult::FrameReady;
            }
            return m_frameTerminal;
        }
    }
    return ReadNextFFmpegProcessAvailable(out,stop);
}

bool VideoDecoder::SetDecodeSize(uint32_t width, uint32_t height) {
    if (m_backend != Backend::FFmpeg || !m_nativeWidth || !m_nativeHeight) return false;
    width = std::max(2u, width & ~1u);
    height = std::max(2u, height & ~1u);
    width = std::min(width, m_nativeWidth & ~1u);
    height = std::min(height, m_nativeHeight & ~1u);
    if (!width || !height) return false;
    if (width == m_width && height == m_height) return true;

    const bool restartQueue=m_frameQueueEnabled;
    if(restartQueue)StopFrameQueue();
    const uint32_t oldW=m_width, oldH=m_height;
    m_width=width; m_height=height; m_stride=static_cast<int32_t>(m_width*4u);
    if (StartFFmpeg(0.0)) {
        if(restartQueue)StartFrameQueue();
        LOG("FFmpeg realtime decode scale: " << m_nativeWidth << "x" << m_nativeHeight << " -> " << m_width << "x" << m_height);
        return true;
    }

    LOG("FFmpeg decode downscale failed; restoring native decode size.");
    m_width=oldW; m_height=oldH; m_stride=static_cast<int32_t>(m_width*4u);
    const bool restored=StartFFmpeg(0.0);
    if(restartQueue&&restored)StartFrameQueue();
    return restored;
}

bool VideoDecoder::OpenMediaFoundation(const std::wstring& path) {
    m_reader.Reset();

    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 4))) return false;
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);

    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), &m_reader);
    if (FAILED(hr)) {
        LOG("MFCreateSourceReaderFromURL failed hr=0x" << std::hex << hr);
        return false;
    }

    m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outType.Get());
    if (FAILED(hr)) {
        // Some systems expose ARGB32 rather than RGB32 through the video processor.
        outType.Reset();
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
        hr = m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outType.Get());
    }
    if (FAILED(hr)) {
        LOG("SetCurrentMediaType(RGB32/ARGB32) failed hr=0x" << std::hex << hr);
        m_reader.Reset();
        return false;
    }

    ComPtr<IMFMediaType> current;
    if (FAILED(m_reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current))) return false;
    MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &m_width, &m_height);
    m_nativeWidth=m_width; m_nativeHeight=m_height;
    m_displayAspect = m_height ? double(m_width)/double(m_height) : 16.0/9.0;
    UINT32 frN = 0, frD = 0;
    if (SUCCEEDED(MFGetAttributeRatio(current.Get(), MF_MT_FRAME_RATE, &frN, &frD)) && frD)
        m_fps = double(frN) / double(frD);

    UINT32 strideU = 0;
    if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideU)))
        m_stride = static_cast<int32_t>(strideU);
    else
        m_stride = static_cast<int32_t>(m_width * 4);

    PROPVARIANT var{};
    PropVariantInit(&var);
    if (SUCCEEDED(m_reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var))) {
        if (var.vt == VT_UI8 || var.vt == VT_I8)
            m_durationSec = static_cast<double>(var.vt == VT_I8 ? var.hVal.QuadPart : static_cast<LONGLONG>(var.uhVal.QuadPart)) / 10000000.0;
    }
    PropVariantClear(&var);

    LOG("Media Foundation: " << m_width << "x" << m_height << " @ " << m_fps << " fps, duration=" << m_durationSec);
    return m_width > 0 && m_height > 0;
}

bool VideoDecoder::ReadNextMediaFoundation(VideoFrame& out) {
    if (!m_reader) return false;

    for (;;) {
        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = m_reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                          &streamIndex, &flags, &timestamp, &sample);
        if (FAILED(hr)) {
            LOG("ReadSample failed hr=0x" << std::hex << hr);
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return false;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            LOG("Media Foundation video media type changed; continuing.");
            continue;
        }
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;

        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buffer->Lock(&data, &maxLen, &curLen))) continue;

        const size_t dstStride = static_cast<size_t>(m_width) * 4u;
        out.bgra.resize(dstStride * m_height);

        int32_t stride = m_stride;
        size_t absStride = static_cast<size_t>(std::abs(stride));
        if (absStride * m_height > curLen) {
            stride = static_cast<int32_t>(dstStride);
            absStride = dstStride;
        }

        const BYTE* firstRow = data;
        if (stride < 0) firstRow = data + absStride * (m_height - 1);

        for (uint32_t y = 0; y < m_height; ++y) {
            const BYTE* src = stride >= 0 ? firstRow + absStride * y : firstRow - absStride * y;
            memcpy(out.bgra.data() + dstStride * y, src, std::min(dstStride, absStride));
        }
        buffer->Unlock();

        out.timestamp100ns = timestamp;
        out.discontinuity = (flags & MF_SOURCE_READERF_STREAMTICK) != 0;
        return true;
    }
}

bool VideoDecoder::ReadNext(VideoFrame& out) {
    if (m_backend == Backend::FFmpeg) return ReadNextFFmpeg(out);
    if (m_backend == Backend::MediaFoundation) return ReadNextMediaFoundation(out);
    return false;
}

VideoReadResult VideoDecoder::ReadNextAvailable(VideoFrame& out,std::stop_token stop) {
    if(m_backend==Backend::FFmpeg)return ReadNextFFmpegAvailable(out,stop);
    if(m_backend==Backend::MediaFoundation)return ReadNextMediaFoundation(out)?VideoReadResult::FrameReady:VideoReadResult::EndOfStream;
    return VideoReadResult::Error;
}

bool VideoDecoder::SeekSeconds(double seconds) {
    seconds = std::clamp(seconds, 0.0, std::max(0.0, m_durationSec));
    if (m_backend == Backend::FFmpeg) {
        const bool restartQueue=m_frameQueueEnabled;
        if(restartQueue)StopFrameQueue();
        const bool started=StartFFmpeg(seconds);
        if(restartQueue&&started)StartFrameQueue();
        return started;
    }
    if (m_backend != Backend::MediaFoundation || !m_reader) return false;

    PROPVARIANT pos{};
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = static_cast<LONGLONG>(seconds * 10000000.0);
    HRESULT hr = m_reader->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);
    if (FAILED(hr)) {
        LOG("Media Foundation seek failed hr=0x" << std::hex << hr);
        return false;
    }
    return true;
}
