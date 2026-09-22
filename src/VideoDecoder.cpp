#include "VideoDecoder.h"
#include "PlatformPaths.h"
#include "HardErrorSuppression.h"
#include "FrameRatePolicy.h"
#include "VariableFrameRatePolicy.h"
#include "Log.h"
#include <propvarutil.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <charconv>
#include <vector>
#include <iterator>
#include <cstring>
#include <thread>
#include <utility>
#include <system_error>
#include <string_view>
#include <map>
#include <memory>
#include <mutex>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

#include "PrecisionSleeper.h"

static std::wstring Quote(const std::wstring& s) {
    // Windows filenames cannot contain a literal quote character, so this is
    // sufficient for the executable and video paths used by this player.
    return L"\"" + s + L"\"";
}

// Bytes of helper stdout a probe may hand back before it is treated as broken:
// the same bound MediaPipeline's capture keeps.
static constexpr size_t kCaptureLimit = 1024 * 1024;

// Input-side options for a resolved YouTube stream, placed ahead of its -i. The
// certificate is verified explicitly rather than by the build's default, which
// the next ffmpeg pin may change, and the protocol whitelist is the minimal set
// a direct or HLS googlevideo https stream reaches (https -> tls -> tcp):
// anything else - file, http, concat, data - is refused by the child itself.
// Empty for a local file, which a whitelist without "file" would refuse.
// Conditioned on the PATH being a URL, not on the kind alone. The whitelist
// exists to bound what a resolved googlevideo address may reach; a filesystem
// path is not a protocol it constrains, and applying it to one only makes
// ffmpeg refuse the open - which is what stopped the prepared-network path
// from being testable against a local clip at all. AudioPlayer::StartProcess
// has always made exactly this check on exactly this option set. A URL still
// gets the identical string it got before.
static std::wstring NetworkInputOptions(MediaSourceKind kind, const std::wstring& path) {
    const bool networkUrl = _wcsnicmp(path.c_str(), L"https://", 8) == 0 ||
                            _wcsnicmp(path.c_str(), L"http://", 7) == 0;
    return (kind == MediaSourceKind::YouTube && networkUrl)
        ? std::wstring(L"-tls_verify 1 -protocol_whitelist https,tls,tcp ") : std::wstring();
}

static double ElapsedMs(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-since).count();
}

// Index of the first CFR frame at or after a position: exactly the frame
// ffmpeg's accurate input seek keeps. A position within a thousandth of a
// frame period of a boundary is that boundary, so the arithmetic agrees with
// the child for the frame-aligned seeks the player actually issues.
static int64_t FirstFrameAtOrAfter(double seconds,double fps) {
    const double index=std::max(0.0,seconds)*std::max(1.0,fps);
    const double nearest=std::round(index);
    if(std::abs(index-nearest)<1e-3)return static_cast<int64_t>(nearest);
    return static_cast<int64_t>(std::ceil(index));
}

static double SnapToFrameGrid(double seconds,double fps) {
    return static_cast<double>(FirstFrameAtOrAfter(seconds,fps))/std::max(1.0,fps);
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

static bool ParseDurationTag(std::string_view text, double& out) {
    const size_t first = text.find(':');
    const size_t second = first == std::string_view::npos ? first : text.find(':', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos) return false;
    const auto parseUnsigned = [](std::string_view value, uint64_t& number) {
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
    };
    uint64_t hours = 0, minutes = 0;
    if (!parseUnsigned(text.substr(0, first), hours) ||
        !parseUnsigned(text.substr(first + 1, second - first - 1), minutes) || minutes >= 60)
        return false;
    const auto secondsText = text.substr(second + 1);
    double seconds = 0.0;
    const auto parsed = std::from_chars(secondsText.data(), secondsText.data() + secondsText.size(), seconds);
    const double total = double(hours) * 3600.0 + double(minutes) * 60.0 + seconds;
    if (parsed.ec != std::errc{} || parsed.ptr != secondsText.data() + secondsText.size() ||
        !std::isfinite(seconds) || seconds < 0.0 || seconds >= 60.0 ||
        !(total > 0.0 && total < double(INT64_MAX) / 10000000.0)) return false;
    out = total;
    return true;
}

VideoDecoder::~VideoDecoder() { Close(); }

void VideoDecoder::Swap(VideoDecoder& other) noexcept {
    const bool restartThis = other.m_frameQueueEnabled;
    const bool restartOther = m_frameQueueEnabled;
    StopFrameQueue();
    other.StopFrameQueue();
    using std::swap;
    swap(m_backend,other.m_backend);swap(m_reader,other.m_reader);swap(m_path,other.m_path);
    // Everything the probe (or KnownMedia, or Media Foundation) said about the
    // stream, and the layout it decodes to, travels as one value: the colour
    // description, the acceleration memo key and the NV12 decision used to be
    // enumerated here one by one and were the ones left behind.
    swap(m_source,other.m_source);
    swap(m_ffmpegExe,other.m_ffmpegExe);swap(m_ffprobeExe,other.m_ffprobeExe);swap(m_ffmpegProcess,other.m_ffmpegProcess);swap(m_ffmpegStdout,other.m_ffmpegStdout);swap(m_ffmpegJob,other.m_ffmpegJob);
    swap(m_ffmpegEmittedFrames,other.m_ffmpegEmittedFrames);swap(m_ffmpegSeekBase100ns,other.m_ffmpegSeekBase100ns);swap(m_ffmpegAcceleration,other.m_ffmpegAcceleration);swap(m_sourceKind,other.m_sourceKind);
    swap(m_ffmpegSpawnFirstFrame,other.m_ffmpegSpawnFirstFrame);swap(m_ffmpegFirstSourceFrame,other.m_ffmpegFirstSourceFrame);
    swap(m_restartFirstFrameMs,other.m_restartFirstFrameMs);swap(m_drainMsPerFrame,other.m_drainMsPerFrame);
    swap(m_seekTiming,other.m_seekTiming);swap(m_seekStart,other.m_seekStart);swap(m_seekTargetSeconds,other.m_seekTargetSeconds);swap(m_seekTimingPending,other.m_seekTimingPending);
    swap(m_sourceGeneration,other.m_sourceGeneration);swap(m_restartDiscontinuity,other.m_restartDiscontinuity);
    // Stopping both queues silences each decoder's own reader, but an external
    // consumer can still be returning a spent buffer to either side, and
    // vector::swap moves the buffers themselves rather than their bytes.
    if(this!=&other){
        std::scoped_lock poolLock(m_bufferPoolMutex,other.m_bufferPoolMutex);
        swap(m_bufferPool,other.m_bufferPool);
    }
    swap(m_pendingFrame,other.m_pendingFrame);swap(m_pendingFrameBytes,other.m_pendingFrameBytes);swap(m_lastFrameByte,other.m_lastFrameByte);swap(m_networkStallTimeout,other.m_networkStallTimeout);swap(m_probeTimeout,other.m_probeTimeout);
    swap(m_helperDirectory,other.m_helperDirectory);
    swap(m_failureStage,other.m_failureStage);
    swap(m_accelerationMemo,other.m_accelerationMemo);
    if(restartThis)StartFrameQueue();
    if(restartOther)other.StartFrameQueue();
}

void VideoDecoder::Close() {
    StopFrameQueue();
    StopFFmpeg();
    {
        std::lock_guard lock(m_bufferPoolMutex);
        m_bufferPool.clear();
    }
    m_reader.Reset();
    m_backend = Backend::None;
}

bool VideoDecoder::Open(const std::wstring& path, MediaSourceKind sourceKind, std::stop_token stop,
                        bool preferNv12) {
    return OpenImpl(path, sourceKind, stop, true, FFmpegAcceleration::Cuda, false, true, nullptr,
                    preferNv12);
}

bool VideoDecoder::OpenKnown(const std::wstring& path, const KnownMedia& media,
                             MediaSourceKind sourceKind, std::stop_token stop, bool preferNv12) {
    if (!media.Valid()) return Open(path, sourceKind, stop, preferNv12);
    return OpenImpl(path, sourceKind, stop, true, FFmpegAcceleration::Cuda, false, true, &media,
                    preferNv12);
}

bool VideoDecoder::OpenMetadata(const std::wstring& path, MediaSourceKind sourceKind,
                                std::stop_token stop) {
    Close();
    m_path = path;
    m_source = {};
    m_sourceKind = sourceKind;
    ++m_sourceGeneration;
    m_ffprobeExe = FindTool(L"ffprobe.exe");
    if (!m_ffprobeExe.empty() && ProbeFFmpeg(path, stop) && !stop.stop_requested()) return true;
    if (stop.stop_requested()) return false;
    // A container ffprobe cannot describe is still worth one Media Foundation
    // question; its reader answers from the file, without a child process.
    if (DecoderPolicyForSource(sourceKind) == DecoderOpenPolicy::FfmpegOnly) return false;
    if (!OpenMediaFoundation(path)) return false;
    m_backend = Backend::MediaFoundation;
    return true;
}

bool VideoDecoder::OpenSequential(const std::wstring& path, MediaSourceKind sourceKind,
                                  std::stop_token stop, bool preferNv12) {
    // NVDEC (this decode) and NVENC/D3D12 (the export encode and any render) are
    // separate engines and the GPU sits idle during export, so software decode was
    // only burning CPU time for nothing; request CUDA and let the existing
    // Cuda->D3D11Va->Software fallback in StartFFmpeg/TryNextFFmpegAcceleration
    // downgrade per codec as needed. The background frame queue still overlaps
    // decode with GPU rendering/encoding.
    return OpenImpl(path, sourceKind, stop, true, FFmpegAcceleration::Cuda, true, preferNv12);
}

bool VideoDecoder::OpenImpl(const std::wstring& path, MediaSourceKind sourceKind,
                            std::stop_token stop, bool queueFrames,
                            FFmpegAcceleration acceleration, bool sequential,
                            bool sequentialNv12, const KnownMedia* known, bool playbackNv12) {
    Close();
    m_path = path;
    m_source = {};
    m_sourceKind = sourceKind;
    // Set for the whole session here; OpenFFmpeg turns it into m_source.layout
    // once the probe knows the geometry, and it is untouched by any acceleration
    // fallback or seek restart afterwards - so a background export's frames
    // stay NV12-or-Bgra for as long as this decoder instance is open.
    m_source.sequentialOpen = sequential;
    m_source.sequentialNv12 = sequentialNv12;
    m_source.nv12Requested = playbackNv12 || (sequential && sequentialNv12);
    m_source.playbackNv12Requested = playbackNv12;
    ++m_sourceGeneration;

    LOG("Opening video. Decoder preference: FFmpeg -> Windows Media Foundation");

    // FFmpeg is intentionally preferred. It makes playback independent from
    // optional Microsoft Store codec packs and handles MKV/WebM/AV1/HEVC/etc.
    if (OpenFFmpeg(path, stop, acceleration, known)) {
        m_backend = Backend::FFmpeg;
        if (queueFrames) StartFrameQueue();
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
        // A failed OpenFFmpeg can leave m_source.layout at whatever its probe decided
        // before StartFFmpeg itself failed; Media Foundation's reader always
        // hands out BGRA, so the layout is pinned back here regardless.
        m_source.layout = VideoPixelLayout::Bgra;
        LOG("Video decoder selected: Windows Media Foundation");
        return true;
    }

    LOG("All video decoder backends failed.");
    return false;
}

std::wstring VideoDecoder::FindTool(const wchar_t* exeName) const {
    if(!m_helperDirectory.empty()){
        const fs::path candidate=fs::path(m_helperDirectory)/exeName;std::error_code ec;
        if(fs::is_regular_file(candidate,ec))return candidate.wstring();
        return L"";
    }
    if (const auto moduleDirectory = platform_paths::ModuleDirectory()) {
        const fs::path base = *moduleDirectory;
        // neural-runtime is a contained helper package: use only the explicit
        // parent copy shared with the player, never an unrelated PATH tool.
        if (base.filename() == L"neural-runtime") {
            const fs::path shared = base.parent_path() / exeName;
            std::error_code ec;
            return fs::is_regular_file(shared, ec) ? shared.wstring() : L"";
        }
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
    const ScopedHardErrorSuppression noHardErrorDialog;
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
    if(m_failureStage!=FailureStage::ProbeResume)
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
    bool cancelled=false,timedOut=false,pipeError=false,overflowed=false;
    char buf[8192];
    for (;;) {
        DWORD available=0;
        if (!PeekNamedPipe(readPipe,nullptr,0,nullptr,&available,nullptr)) {
            const DWORD pipeFailure=GetLastError();
            if(pipeFailure!=ERROR_BROKEN_PIPE){pipeError=true;break;}
            // A successful, empty orientation probe can close stdout just
            // before its process exits. Await that exit within the same
            // cancellable deadline instead of treating EOF as a pipe error.
            if(WaitForSingleObject(pi.hProcess,0)==WAIT_OBJECT_0)break;
            available=0;
        }
        while(available>0){
            const DWORD want=std::min<DWORD>(available,sizeof(buf));DWORD got=0;
            if(!ReadFile(readPipe,buf,want,&got,nullptr)){pipeError=true;break;}
            if(got==0)break;
            // A probe's answer is a few hundred bytes; the cap is against a child
            // that streams something else at us, not against any real answer.
            if(output.size()+got>kCaptureLimit){overflowed=true;break;}
            output.append(buf,buf+got);available-=got;
        }
        if(pipeError||overflowed)break;
        if(WaitForSingleObject(pi.hProcess,0)==WAIT_OBJECT_0){
            DWORD remaining=0;if(!PeekNamedPipe(readPipe,nullptr,0,nullptr,&remaining,nullptr)||remaining==0)break;
        }
        if(stop.stop_requested()){cancelled=true;break;}
        if(std::chrono::steady_clock::now()>=deadline){timedOut=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if(overflowed)LOG("ffprobe output exceeded "<<(kCaptureLimit>>20)<<" MiB; discarding it.");
    if(cancelled||timedOut||pipeError||overflowed){TerminateJobObject(job,1);WaitForSingleObject(pi.hProcess,500);}
    CloseHandle(readPipe);

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exitCode) *exitCode = code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    return !cancelled&&!timedOut&&!pipeError&&!overflowed&&code == 0;
}

bool VideoDecoder::ProbeFFmpeg(const std::wstring& path, std::stop_token stop) {
    // Options that precede -i: a resolved stream is https and nothing else, so
    // the child is told to verify the certificate and to refuse every protocol
    // the URL cannot legitimately need. Absent for a local file, which the
    // whitelist would refuse.
    const std::wstring inputOptions=NetworkInputOptions(m_sourceKind,path);
    std::wstring args =
        L"-v error -select_streams v:0 "
        L"-show_entries stream=width,height,codec_name,pix_fmt,color_space,color_range,color_primaries,color_transfer,display_aspect_ratio,sample_aspect_ratio,avg_frame_rate,r_frame_rate,duration:stream_tags=DURATION:format=duration,format_name "
        L"-of default=noprint_wrappers=1 " + inputOptions + L"-i " + Quote(path);

    std::string text;
    DWORD code = 0;
    const auto timeout=m_probeTimeout;
    if (!RunCapture(m_ffprobeExe, args, text, &code, stop, timeout)) {
        LOG("ffprobe failed, exitCode=" << code);
        return false;
    }

    uint32_t width = 0, height = 0;
    double avgRate = 0.0, rawRate = 0.0, duration = 0.0;
    double videoDuration = 0.0;
    std::string format,codecName,pixelFormat;
    // ffprobe prints "unknown" for a colour entry the stream does not declare;
    // these stay empty for that, which is what makes Unspecified reachable.
    std::string colorSpace,colorRange,colorPrimaries,colorTransfer;
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
            else if (key == "format_name") format = value;
            else if (key == "codec_name" && value != "N/A") codecName = value;
            else if (key == "pix_fmt" && value != "N/A") pixelFormat = value;
            else if (key == "color_space" && value != "N/A" && value != "unknown") colorSpace = value;
            else if (key == "color_range" && value != "N/A" && value != "unknown") colorRange = value;
            else if (key == "color_primaries" && value != "N/A" && value != "unknown") colorPrimaries = value;
            else if (key == "color_transfer" && value != "N/A" && value != "unknown") colorTransfer = value;
            else if (key == "height") height = static_cast<uint32_t>(std::stoul(value));
            else if (key == "display_aspect_ratio" && value != "N/A") {
                const size_t c=value.find(':'); if(c!=std::string::npos){ double a=std::stod(value.substr(0,c)), b=std::stod(value.substr(c+1)); if(b>0) displayAspect=a/b; }
            }
            else if (key == "sample_aspect_ratio" && value != "N/A") {
                const size_t c=value.find(':'); if(c!=std::string::npos){ double a=std::stod(value.substr(0,c)), b=std::stod(value.substr(c+1)); if(b>0) sampleAspect=a/b; }
            }
            else if (key == "avg_frame_rate") ParseRate(value, avgRate);
            else if (key == "r_frame_rate") ParseRate(value, rawRate);
            else if (key == "TAG:DURATION") ParseDurationTag(value, videoDuration);
            else if (key == "duration" && value != "N/A" && duration <= 0.0) duration = std::stod(value);
        } catch (...) {}
    }
    // ffprobe prints stream entries in its own order, so the key is composed
    // once both fields are in hand.
    m_source.hardwareProfile = codecName.empty() ? pixelFormat : (pixelFormat.empty() ? codecName : codecName + "/" + pixelFormat);
    // Same reason as the key above: the four colour entries arrive in whatever
    // order ffprobe chose, so the description is composed once all four are in
    // hand. An entry ffprobe printed as "unknown" left its string empty and maps
    // to Unspecified; anything declared but outside the sets below maps to Other,
    // so a refusal can tell "says BT.2020" from "says nothing".
    m_source.color.matrix = colorSpace.empty() ? ColorMatrix::Unspecified :
        colorSpace == "bt709" ? ColorMatrix::Bt709 :
        (colorSpace == "bt470bg" || colorSpace == "smpte170m") ? ColorMatrix::Bt601 : ColorMatrix::Other;
    m_source.color.range = colorRange == "tv" ? ColorRange::Limited :
        colorRange == "pc" ? ColorRange::Full : ColorRange::Unspecified;
    m_source.color.primaries = colorPrimaries.empty() ? ColorPrimaries::Unspecified :
        colorPrimaries == "bt709" ? ColorPrimaries::Bt709 :
        colorPrimaries == "bt470bg" ? ColorPrimaries::Bt470bg :
        colorPrimaries == "smpte170m" ? ColorPrimaries::Smpte170m : ColorPrimaries::Other;
    m_source.color.transfer = colorTransfer.empty() ? ColorTransfer::Unspecified :
        colorTransfer == "bt709" ? ColorTransfer::Bt709 :
        colorTransfer == "smpte170m" ? ColorTransfer::Smpte170m : ColorTransfer::Other;
    // Kept verbatim for the refusal log line: "other" is a diagnosis nobody can
    // act on, "bt2020nc" is.
    const auto tag=[](const std::string& value){return value.empty()?std::string("unspecified"):value;};
    m_source.colorTags = "matrix=" + tag(colorSpace) + " range=" + tag(colorRange) +
                  " primaries=" + tag(colorPrimaries) + " transfer=" + tag(colorTransfer);

    if (!width || !height) {
        LOG("ffprobe returned no usable video dimensions.");
        return false;
    }

    m_source.nativeWidth = width;
    m_source.nativeHeight = height;
    m_source.width = width;
    m_source.height = height;
    m_source.stride = static_cast<int32_t>(m_source.width * 4u);
    m_source.fps = avgRate > 0.0 ? avgRate : (rawRate > 0.0 ? rawRate : 30.0);
    // Both rates as probed, before the overrides and the clamp below touch fps:
    // a consumer asking whether the cadence is fixed needs the pair, and a
    // consumer asking whether the source stated a rate at all needs to see the
    // 30.0 above as the fabrication it is. ParseRate writes only finite
    // positive values, so an unreported or unparsable entry leaves 0.0 here.
    m_source.avgFrameRate = avgRate;
    m_source.nominalFrameRate = rawRate;
    // Matroska stores per-track duration here; its container may include longer audio.
    m_source.durationSec = videoDuration > 0.0 ? videoDuration :
        ((std::isfinite(duration) && duration > 0.0) ? duration : 0.0);
    m_source.gif = format == "gif";
    m_source.stillImage = format == "image2" || format == "png_pipe" || format == "jpeg_pipe" ||
        format == "bmp_pipe" || format == "tiff_pipe" || format == "webp_pipe";
    // A photo has one frame, with a finite carrier duration for the existing
    // neural cache. GIF delays are centiseconds: a 100 Hz carrier preserves
    // every delay instead of retiming variable-delay animation to its average.
    if (m_source.stillImage) {
        m_source.fps = 1.0; m_source.durationSec = 1.0;
        // JPEG EXIF orientation is frame side data, absent from stream metadata.
        // FFmpeg autorotates its output; expose matching row geometry to the GPU.
        std::string orientation;
        if (!RunCapture(m_ffprobeExe, L"-v error -select_streams v:0 -read_intervals \"%+#1\" "
            L"-show_entries frame_side_data=rotation -of default=noprint_wrappers=1 " + inputOptions + L"-i " + Quote(path),
            orientation, &code, stop, timeout)) return false;
        std::istringstream rotations(orientation);
        while (std::getline(rotations, line)) {
            if (line.rfind("rotation=", 0) != 0) continue;
            try {
                const double angle = std::stod(line.substr(9));
                if (std::isfinite(angle) && std::abs(std::fmod(std::abs(angle), 180.0) - 90.0) < 0.5) {
                    std::swap(m_source.width, m_source.height);std::swap(m_source.nativeWidth, m_source.nativeHeight);
                    m_source.stride = static_cast<int32_t>(m_source.width * 4u);
                    if (displayAspect > 0.1) displayAspect = 1.0 / displayAspect;
                    if (sampleAspect > 0.0) sampleAspect = 1.0 / sampleAspect;
                }
            } catch (...) { return false; }
            break;
        }
    }
    else if (m_source.gif) m_source.fps = 100.0;
    if (std::isfinite(displayAspect) && displayAspect > 0.1) m_source.displayAspect = displayAspect;
    else m_source.displayAspect = (double(m_source.width) * sampleAspect) / double(m_source.height);

    // Avoid pathological metadata causing gigantic pacing delays/CPU usage.
    m_source.fps = std::clamp(m_source.fps, 1.0, 240.0);

    ProbePacketSpacing(path, inputOptions, stop);

    LOG("ffprobe: " << m_source.width << "x" << m_source.height << " DAR=" << m_source.displayAspect << " @ " << m_source.fps
        << " fps, duration=" << m_source.durationSec << ", " << m_source.colorTags);
    return true;
}

// Samples the first packets' presentation timestamps and asks whether they are
// evenly spaced. See VariableFrameRatePolicy.h for why the two rates the
// container declares cannot answer that.
//
// Local files only. The sources whose declared rates lie are captures and
// phone video, which arrive as files; a stream has been transcoded by the
// service and its container rebuilt, and probing one costs a fresh connection
// and range request on every open for an answer that is not in doubt. A still
// image has no spacing at all.
//
// Best effort throughout: a probe that fails leaves spacingDecided false and
// the declared rates are all ConstantFrameRate() has, which is where it
// started.
void VideoDecoder::ProbePacketSpacing(const std::wstring& path, const std::wstring& inputOptions,
                                      std::stop_token stop) {
    m_source.spacingDecided = false;
    m_source.spacingConstant = true;
    if (m_source.stillImage || m_sourceKind != MediaSourceKind::LocalFile) return;
    if (m_ffprobeExe.empty()) return;

    const std::wstring args =
        L"-v error -select_streams v:0 -read_intervals \"%+#" +
        std::to_wstring(variable_frame_rate::kRecommendedSamples) +
        L"\" -show_entries packet=pts_time -of default=noprint_wrappers=1 " +
        inputOptions + L"-i " + Quote(path);

    std::string text;
    DWORD code = 0;
    if (!RunCapture(m_ffprobeExe, args, text, &code, stop, m_probeTimeout)) {
        LOG("ffprobe: the packet-spacing probe did not run (exitCode=" << code
            << "); frame-rate constancy falls back to the two declared rates.");
        return;
    }

    std::vector<double> times;
    times.reserve(variable_frame_rate::kRecommendedSamples);
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("pts_time=", 0) != 0) continue;
        const std::string value = line.substr(9);
        // "N/A" for a packet the demuxer could not timestamp. One of those in
        // the middle would look like a doubled interval, so the sample stops
        // rather than inventing a gap.
        double time = 0.0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), time);
        if (parsed.ec != std::errc{} || !std::isfinite(time)) break;
        times.push_back(time);
    }

    const auto verdict = variable_frame_rate::Classify(times);
    if (!verdict.decided) {
        LOG("ffprobe: " << times.size() << " packet timestamps is too few to judge the spacing; "
            "frame-rate constancy falls back to the two declared rates.");
        return;
    }
    m_source.spacingDecided = true;
    m_source.spacingConstant = verdict.constant;
    LOG("ffprobe: packet spacing over " << verdict.intervals << " intervals is "
        << (verdict.constant ? "constant" : "variable") << " (median "
        << (verdict.medianIntervalSeconds * 1000.0) << " ms, " << verdict.deviatingIntervals
        << " outside tolerance, " << verdict.reorderedIntervals << " reordered).");
}

bool VideoDecoder::ConstantFrameRate() const {
    // A single frame has no spacing to be constant, and ffprobe hands a still
    // image the image2 demuxer's own 25/1 for both rates, which would otherwise
    // read as a perfectly constant 25 fps cadence.
    if (m_source.stillImage) return false;
    // Evidence beats declaration. When the packet timestamps were sampled and
    // had enough of a story to tell, they are the answer: the two rates below
    // are wrong in both directions on exactly the sources this question is
    // asked about. They remain the fallback for a source whose packets could
    // not be walked - a stream, a probe that failed, too few frames.
    if (m_source.spacingDecided) return m_source.spacingConstant;
    const double avg = m_source.avgFrameRate, nominal = m_source.nominalFrameRate;
    // One rate on its own corroborates nothing, so an unknown or half-reported
    // rate is not constant rather than assumed constant: the consumer that
    // cares is choosing whether to retime the source, and retiming a VFR
    // recording as if it were CFR is the failure this answer exists to prevent.
    if (avg <= 0.0 || nominal <= 0.0) return false;
    return std::abs(avg - nominal) <= frame_rate_policy::kRateTolerance * std::max(avg, nominal);
}

namespace {
constexpr unsigned kCudaUnavailable=1u,kD3d11Unavailable=2u;
} // namespace

// A hardware path that cannot even start is a property of the codec plus this
// build's ffmpeg, not of the file: remembering it per codec keeps every later
// open and every seek from paying for the same dead process launches, while an
// unsupported codec never downgrades the ones the GPU does handle.
class AccelerationMemo {
public:
    unsigned Unavailable(const std::string& profile) const
    {
        std::scoped_lock lock(mutex_);
        const auto found=paths_.find(Key(profile));
        return found==paths_.end()?0u:found->second;
    }

    void Remember(const std::string& profile,unsigned path)
    {
        const std::string key=Key(profile);
        bool added=false;
        {
            std::scoped_lock lock(mutex_);
            unsigned& paths=paths_[key];
            added=(paths&path)==0;
            paths|=path;
        }
        if(added)
            LOG("Hardware decode path " << (path==kCudaUnavailable?"cuda":"d3d11va")
                << " is unavailable for " << key << "; later opens and seeks skip it.");
    }

private:
    static std::string Key(const std::string& profile)
    {
        return profile.empty()?std::string("unknown"):profile;
    }

    mutable std::mutex mutex_;
    std::map<std::string,unsigned> paths_;
};

std::shared_ptr<AccelerationMemo> MakeAccelerationMemo()
{
    return std::make_shared<AccelerationMemo>();
}

namespace {
AccelerationMemo& ProcessAccelerationMemo()
{
    static AccelerationMemo memo;
    return memo;
}
} // namespace

AccelerationMemo& VideoDecoder::AccelerationMemory() const
{
    return m_accelerationMemo?*m_accelerationMemo:ProcessAccelerationMemo();
}

// The job object carries KILL_ON_JOB_CLOSE, so closing it is what makes the
// child die; waiting only makes that death observable to the caller.
static void StopFFmpegChild(HANDLE process,HANDLE job,HANDLE stdoutRead,DWORD waitTimeout) {
    if (process) {
        DWORD code = 0;
        if (GetExitCodeProcess(process, &code) && code == STILL_ACTIVE) {
            if(job)TerminateJobObject(job,0);else TerminateProcess(process, 0);
            WaitForSingleObject(process, waitTimeout);
        }
        CloseHandle(process);
    }
    if(job)CloseHandle(job);
    if(stdoutRead)CloseHandle(stdoutRead);
}

bool VideoDecoder::StartFFmpeg(double seekSeconds, std::optional<FFmpegAcceleration> requested) {
    FFmpegAcceleration acceleration=requested.value_or(m_ffmpegAcceleration);
    const unsigned unavailable=AccelerationMemory().Unavailable(m_source.hardwareProfile);
    if(acceleration==FFmpegAcceleration::Cuda&&(unavailable&kCudaUnavailable))
        acceleration=FFmpegAcceleration::D3D11Va;
    if(acceleration==FFmpegAcceleration::D3D11Va&&(unavailable&kD3d11Unavailable))
        acceleration=FFmpegAcceleration::Software;
    // Teardown is 38ms of a measured 265ms restart seek and the replacement
    // child does not need the old one gone: hand the old child to a guard that
    // kills it once the new one is spawned, so its death overlaps the new
    // child's container reopen instead of delaying it.
    struct HandedOverChild {
        VideoDecoder* owner;HANDLE process,job,stdoutRead;
        ~HandedOverChild(){
            const auto started=std::chrono::steady_clock::now();
            StopFFmpegChild(process,job,stdoutRead,0);
            if(owner->m_seekTimingPending){
                std::scoped_lock timingLock(owner->m_seekTimingMutex);
                owner->m_seekTiming.teardownMs=ElapsedMs(started);
            }
        }
    } handedOver{this,m_ffmpegProcess,m_ffmpegJob,m_ffmpegStdout};
    m_ffmpegProcess=nullptr;m_ffmpegJob=nullptr;m_ffmpegStdout=nullptr;
    m_pendingFrame.clear();m_pendingFrameBytes=0;
    seekSeconds = std::max(0.0, seekSeconds);
    // Snap to the constant-frame-rate grid the child is forced to emit. An
    // unaligned -ss leaves ffmpeg's resampler choosing between two neighbouring
    // source frames for its first output frame and duplicating one soon after,
    // which both offsets the reconstructed timeline by up to half a frame and
    // makes the first frame of a seek ambiguous.
    if(!m_source.stillImage)seekSeconds=SnapToFrameGrid(seekSeconds,m_source.fps);
    if (m_source.stillImage || m_source.gif) acceleration = FFmpegAcceleration::Software;
    if (m_source.stillImage) seekSeconds = 0.0;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    // A pipe smaller than one frame forces the child to block halfway through
    // every frame and wait for our copy before it can decode the next one, at a
    // measured 31ms per 1080p frame against 16ms of decode. Two frames of slack
    // let decode and copy overlap, which is what makes walking past frames a
    // cheaper answer than a restart. The read-ahead stays bounded: this is the
    // only buffer the child gets, plus the four frame queue.
    //
    // The ceiling has to cover two frames of the largest source the player
    // accepts, or the slack silently collapses on exactly the sources that need
    // it most. A 16 MiB ceiling gave 1080p BGRA its two frames (15.82 MiB) and
    // left 1440p with 1.14 (28.12 MiB asked, 16 granted): measured on a 2560x1440
    // YouTube source, that cost 98 partial reads per frame instead of one,
    // 5.23 ms of pipe read per frame instead of 1.41, and 28.85 fps against a
    // 30 fps source. 2160p BGRA - the largest geometry Super Resolution output
    // offers - needs 63.28 MiB, so 64 covers every supported case.
    HANDLE readPipe = nullptr, writePipe = nullptr;
    const DWORD pipeBytes=static_cast<DWORD>(std::clamp<size_t>(
        2u*FrameBytes(m_source.layout,m_source.width,m_source.height),
        4u<<20,64u<<20));
    // Sizing and piping have to agree on the layout: a pipe sized for NV12 cannot
    // hold a BGRA frame, and the symptom is the partial-read storm this comment
    // describes rather than an error anywhere.
    LOG("FFmpeg pipe " << (pipeBytes >> 20) << " MiB for a "
        << (m_source.layout == VideoPixelLayout::Nv12 ? "NV12" : "BGRA") << " "
        << m_source.width << "x" << m_source.height << " frame of "
        << FrameBytes(m_source.layout, m_source.width, m_source.height) << " bytes ("
        << (double(pipeBytes) / double(std::max<size_t>(1, FrameBytes(m_source.layout, m_source.width, m_source.height))))
        << " frames).");
    if (!CreatePipe(&readPipe, &writePipe, &sa, pipeBytes)) {
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
    if (m_source.gif) args << L"-ignore_loop 1 ";
    args << NetworkInputOptions(m_sourceKind,m_path) << L"-i " << Quote(m_path)
         << L" -map 0:v:0 -an -sn -dn ";
    // NV12 (the export's session layout, chosen in OpenFFmpeg) is already the
    // decoder's working format up to hwdownload, so it only takes dropping the
    // trailing `format=bgra` conversion and asking for `-pix_fmt nv12` below -
    // 11.1 MB BGRA -> 4.2 MB NV12 per 2578x1080 frame over the pipe.
    const bool nv12 = m_source.layout == VideoPixelLayout::Nv12;
    if (acceleration == FFmpegAcceleration::Cuda) {
        args << L"-vf scale_cuda=" << m_source.width << L":" << m_source.height
             << L":format=nv12:interp_algo=bicubic:passthrough=0,hwdownload,format=nv12";
        if (!nv12) args << L",format=bgra";
        args << L" ";
    } else if (acceleration == FFmpegAcceleration::D3D11Va) {
        args << L"-vf hwdownload,format=nv12,scale=" << m_source.width << L":" << m_source.height
             << L":flags=bicubic";
        if (!nv12) args << L",format=bgra";
        args << L" ";
    } else if (m_source.nativeWidth && m_source.nativeHeight && (m_source.width != m_source.nativeWidth || m_source.height != m_source.nativeHeight))
        args << L"-vf scale=" << m_source.width << L":" << m_source.height << L":flags=bicubic ";
    if (m_source.stillImage) args << L"-frames:v 1 ";
    if (m_source.gif && m_source.durationSec > seekSeconds)
        args << L"-t " << std::fixed << std::setprecision(6) << (m_source.durationSec - seekSeconds) << L" ";
    args << L"-pix_fmt " << (nv12 ? L"nv12" : L"bgra") << L" -fps_mode cfr -r "
         << std::fixed << std::setprecision(6) << m_source.fps
         << L" -f rawvideo pipe:1";

    std::wstring command = Quote(m_ffmpegExe) + L" " + args.str();
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    if(job){JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){CloseHandle(job);job=nullptr;}}
    PROCESS_INFORMATION pi{};
    const auto spawnStarted=std::chrono::steady_clock::now();
    const ScopedHardErrorSuppression noHardErrorDialog;
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
    if(m_failureStage!=FailureStage::DecodeResume)
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

    const double spawnMs=ElapsedMs(spawnStarted);
    CloseHandle(pi.hThread);
    m_ffmpegProcess = pi.hProcess;
    m_ffmpegStdout = readPipe;
    m_ffmpegJob = job;
    m_ffmpegEmittedFrames = 0;
    m_ffmpegSeekBase100ns = static_cast<int64_t>(seekSeconds * 10000000.0);
    m_ffmpegSpawnFirstFrame = m_ffmpegFirstSourceFrame = FirstFrameAtOrAfter(seekSeconds,m_source.fps);
    m_ffmpegAcceleration = acceleration;
    m_pendingFrame.clear();m_pendingFrameBytes=0;m_lastFrameByte=std::chrono::steady_clock::now();
    m_restartDiscontinuity = false;
    if(m_seekTimingPending){
        std::scoped_lock timingLock(m_seekTimingMutex);
        m_seekTiming.spawnMs=spawnMs;
    }
    const char* accelerationName = acceleration == FFmpegAcceleration::Cuda ? "CUDA decode + GPU scale" :
        acceleration == FFmpegAcceleration::D3D11Va ? "D3D11VA decode" : "software decode";
    LOG("FFmpeg raw " << (nv12 ? "NV12" : "BGRA") << " process started with " << accelerationName << ".");
    return true;
}

void VideoDecoder::StopFFmpeg(DWORD waitTimeout) {
    StopFFmpegChild(m_ffmpegProcess,m_ffmpegJob,m_ffmpegStdout,waitTimeout);
    m_ffmpegProcess=nullptr;m_ffmpegJob=nullptr;m_ffmpegStdout=nullptr;
    m_pendingFrame.clear();m_pendingFrameBytes=0;
}

bool VideoDecoder::OpenFFmpeg(const std::wstring& path, std::stop_token stop,
                              FFmpegAcceleration initialAcceleration,
                              const KnownMedia* known) {
    m_ffmpegExe = FindTool(L"ffmpeg.exe");
    m_ffprobeExe = known ? std::wstring{} : FindTool(L"ffprobe.exe");
    if (m_ffmpegExe.empty() || (!known && m_ffprobeExe.empty())) {
        LOG("Bundled/system FFmpeg tools not found. ffmpeg=" << (!m_ffmpegExe.empty())
            << " ffprobe=" << (!m_ffprobeExe.empty()));
        return false;
    }

    LOG("FFmpeg executable detected.");
    if (known) {
        // A file this process produced beside one already probed: same encoder,
        // same geometry, same frame rate. Probing it again would spend a child
        // process on an answer already in hand.
        m_source.width = m_source.nativeWidth = known->width;
        m_source.height = m_source.nativeHeight = known->height;
        m_source.fps = known->fps;
        // The sibling was encoded by StartFFmpeg with -fps_mode cfr -r fps, so
        // this rate is both the source's own and genuinely constant; without it
        // a no-probe open would report an unknown rate for a file whose rate
        // this process chose.
        m_source.avgFrameRate = m_source.nominalFrameRate = known->fps;
        m_source.durationSec = known->durationSec;
        m_source.hardwareProfile = known->hardwareProfile;
        // Declared by the caller off a file it probed, not assumed here. Empty
        // stays empty, which lands on the same refusal as any other undeclared
        // stream.
        m_source.color = known->color;
        // A known open runs no probe, so it has no verbatim ffprobe strings to
        // quote. Render the mapped description instead: the accept/refuse line
        // below is the only place either decision is visible, and a blank one
        // is the sort of log that sends the next reader to a debugger.
        const char* matrix = m_source.color.matrix == ColorMatrix::Bt709 ? "bt709" :
            m_source.color.matrix == ColorMatrix::Bt601 ? "bt601" :
            m_source.color.matrix == ColorMatrix::Other ? "other" : "unspecified";
        const char* range = m_source.color.range == ColorRange::Limited ? "tv" :
            m_source.color.range == ColorRange::Full ? "pc" : "unspecified";
        m_source.colorTags = std::string("matrix=") + matrix + " range=" + range +
                             " (declared by the sibling this file was opened from)";
        m_source.displayAspect = double(m_source.width) / double(m_source.height);
    } else if (!ProbeFFmpeg(path,stop) || stop.stop_requested()) {
        return false;
    }
    // NV12 needs even plane dimensions (the UV plane is half-resolution in
    // both axes); odd geometry stays BGRA even for a sequential/export open,
    // and so does a caller that opted out of NV12 via preferNv12=false.
    //
    // It also needs the source to have DECLARED a colour description the GPU
    // conversion implements. That conversion is a matrix plus a range mapping,
    // and an undeclared stream states neither: handing one over as NV12 is what
    // used to decode a BT.601 or full-range source under BT.709 limited-range
    // coefficients with nothing in the log to say so. Undeclared is not BT.709 -
    // it is BGRA, ffmpeg converts it on the CPU from the tags it can see, and
    // that costs pipe bandwidth rather than colour. A `known` open skips the
    // probe entirely and so declares nothing, which lands on the same refusal.
    //
    // This is also what makes the renderer's matching refusal unreachable: the
    // decoder is the only thing that ever asks for an NV12 source, and it asks
    // only for a description SourceNv12ConversionFor already accepted.
    // Decided once here, from the probed geometry, and left alone by every
    // later StartFFmpeg call (acceleration fallback, seek restart) this session.
    const bool wantNv12 = m_source.nv12Requested;
    // A PLAYBACK open takes a narrower gate than an export one. The player's
    // comparison reference is a BGRA-only texture, so a source it decodes to
    // NV12 has to be convertible back on the CPU for that one upload, and
    // exactly one inverse is implemented and tested (BT.709 limited - see
    // Nv12ToBgraBt709Limited in main.cpp). Everything else decodes to BGRA the
    // way it always has rather than reaching a conversion nobody wrote. The
    // export path is unaffected: it has no comparison reference and keeps all
    // four conversions the GPU pass implements.
    const bool playbackConvertible =
        !m_source.playbackNv12Requested ||
        SourceNv12ConversionFor(m_source.color) == SourceNv12Conversion::Bt709Limited;
    const bool evenGeometry = m_source.width % 2 == 0 && m_source.height % 2 == 0;
    const bool convertible = SourceNv12ConversionFor(m_source.color) != SourceNv12Conversion::Unsupported;
    // Accepted and refused are deliberately one grep away from each other - same
    // "GPU source conversion" prefix, same four tags named either way - so a reader
    // of one render's log gets either the path it took or the reason it did not.
    if (wantNv12 && evenGeometry) {
        if (convertible)
            LOG("GPU source conversion accepted: " << m_source.colorTags
                << "; decoding to NV12 and converting it on the GPU.");
        else
            LOG("GPU source conversion refused: " << m_source.colorTags
                << "; no conversion implements that description, so the source decodes to "
                   "BGRA and ffmpeg converts it on the CPU instead.");
    }
    m_source.layout = (wantNv12 && evenGeometry && convertible && playbackConvertible)
        ? VideoPixelLayout::Nv12 : VideoPixelLayout::Bgra;
    return StartFFmpeg(0.0,initialAcceleration);
}

// Position on the current timeline of the next frame the child will emit. The
// child emits headerless rawvideo, so this arithmetic is the only position the
// decoder has, and it has to survive a seek that keeps the child running.
double VideoDecoder::FFmpegHeadSeconds() const {
    const int64_t timelineFrame=m_ffmpegSpawnFirstFrame+
        static_cast<int64_t>(m_ffmpegEmittedFrames)-m_ffmpegFirstSourceFrame;
    return static_cast<double>(m_ffmpegSeekBase100ns)*1e-7+
        static_cast<double>(timelineFrame)/std::max(1.0,m_source.fps);
}

bool VideoDecoder::TryNextFFmpegAcceleration(DWORD exitCode) {
    if (exitCode == 0 || exitCode == STILL_ACTIVE || m_ffmpegAcceleration == FFmpegAcceleration::Software)
        return false;
    // A path that dies before its first frame cannot decode this codec here, so
    // later opens skip it. One that fails after delivering frames is a stream or
    // position problem and must not disqualify the hardware for everything else.
    if(m_ffmpegEmittedFrames==0)
        AccelerationMemory().Remember(m_source.hardwareProfile,
            m_ffmpegAcceleration==FFmpegAcceleration::Cuda?kCudaUnavailable:kD3d11Unavailable);
    const FFmpegAcceleration next = m_ffmpegAcceleration == FFmpegAcceleration::Cuda ?
        FFmpegAcceleration::D3D11Va : FFmpegAcceleration::Software;
    const double resumeSeconds = FFmpegHeadSeconds();
    LOG("FFmpeg hardware path exited with code " << exitCode << "; trying " <<
        (next == FFmpegAcceleration::D3D11Va ? "D3D11VA" : "software") << " fallback.");
    if (!StartFFmpeg(resumeSeconds, next)) return false;
    // The resumed process starts a fresh timeline segment: frames already
    // handed out belong to the previous decoder session and must not pair or
    // share temporal history with the first resumed frame.
    ++m_sourceGeneration;
    m_restartDiscontinuity = true;
    return true;
}

void VideoDecoder::RecycleFrameBuffer(std::vector<uint8_t>&& buffer) {
    if (buffer.empty()) return;
    std::lock_guard lock(m_bufferPoolMutex);
    if (m_bufferPool.size() >= FrameBufferPoolCapacity) return;
    m_bufferPool.push_back(std::move(buffer));
}

std::vector<uint8_t> VideoDecoder::TakeRecycledBuffer(size_t frameBytes) {
    std::lock_guard lock(m_bufferPoolMutex);
    // A resolution change leaves buffers of the previous size behind; they are
    // dropped as they are reached rather than searched for and kept.
    while (!m_bufferPool.empty()) {
        std::vector<uint8_t> buffer = std::move(m_bufferPool.back());
        m_bufferPool.pop_back();
        if (buffer.size() == frameBytes) return buffer;
    }
    return {};
}

bool VideoDecoder::ReadNextFFmpeg(VideoFrame& out) {
    return ReadNextBlocking(out, {}) == VideoReadResult::FrameReady;
}

VideoReadResult VideoDecoder::ClassifyFFmpegEnd(DWORD exitCode) {
    if (!m_pendingFrameBytes) return VideoReadResult::EndOfStream;
    const double completedSeconds = FFmpegHeadSeconds();
    const double endTolerance = std::max(0.05, 1.5 / std::max(1.0, m_source.fps));
    if (m_sourceKind == MediaSourceKind::YouTube && exitCode == 0 && m_source.durationSec > 0.0 &&
        completedSeconds + endTolerance >= m_source.durationSec) {
        LOG("Discarding an incomplete trailing raw frame after the expected YouTube duration.");
        m_pendingFrameBytes = 0;
        return VideoReadResult::EndOfStream;
    }
    LOG("FFmpeg ended in the middle of a raw video frame. exitCode="<<exitCode
        <<" emittedFrames="<<m_ffmpegEmittedFrames<<" pendingBytes="<<m_pendingFrameBytes);
    return VideoReadResult::Error;
}

VideoReadResult VideoDecoder::ReadNextFFmpegProcessAvailable(VideoFrame& out,std::stop_token stop,bool block) {
    if (!m_ffmpegStdout) return VideoReadResult::EndOfStream;
    const size_t frameBytes = FrameBytes(m_source.layout, m_source.width, m_source.height);
    if (!frameBytes) return VideoReadResult::Error;
  for(;;){
    if(stop.stop_requested())return VideoReadResult::Cancelled;
    if(m_pendingFrame.size()!=frameBytes){
        m_pendingFrame=TakeRecycledBuffer(frameBytes);
        if(m_pendingFrame.size()!=frameBytes){m_pendingFrame.resize(frameBytes);++m_frameBufferFills;}
        m_pendingFrameBytes=0;m_lastFrameByte=std::chrono::steady_clock::now();
    }

    // Drain whatever the child has produced, rather than one chunk per call.
    // Returning after a single 4 MiB read made the caller sleep once per chunk:
    // an 8.3 MB 1080p frame cost three polls and two sleeps, which measured as
    // 12.7 ms of the 15.2 ms per frame, and a 33 MB 4K frame cost eight. The
    // loop never blocks - it stops as soon as the pipe is empty - so the stall,
    // cancellation and child-exit paths below are reached exactly as before.
    DWORD available=0;
    for(;;){
        if(!PeekNamedPipe(m_ffmpegStdout,nullptr,0,nullptr,&available,nullptr)){
            if(GetLastError()!=ERROR_BROKEN_PIPE)return VideoReadResult::Error;
            // A child can close stdout just before its process handle becomes signaled.
            // Treat that short interval as an empty pipe so the existing nonblocking
            // exit/fallback path below observes the eventual exit code.
            available=0;
        }
        if(!available&&block&&m_pendingFrameBytes<frameBytes){
            // Local files: wait in the kernel for the next byte instead of peek-sleep
            // polling for it. A byte-mode pipe ReadFile wakes as soon as >=1 byte is
            // available, so this is strictly "peek->sleep->peek" replaced by "block".
            const DWORD want=static_cast<DWORD>(std::min<size_t>(frameBytes-m_pendingFrameBytes,size_t{16u<<20}));
            DWORD got=0;
            const auto blockStart=std::chrono::steady_clock::now();
            ++m_frameReadCalls;
            const BOOL ok=ReadFile(m_ffmpegStdout,m_pendingFrame.data()+m_pendingFrameBytes,want,&got,nullptr);
            const DWORD err=ok?ERROR_SUCCESS:GetLastError();
            m_frameBlockedNanos+=std::chrono::steady_clock::now()-blockStart;
            if(!ok){
                // CancelSynchronousIo (StopFrameQueue) unparks this as ERROR_OPERATION_ABORTED;
                // any other failure racing a stop is reported as the cancel too, since the
                // decoder is being torn down and Error would misclassify it.
                if(err==ERROR_OPERATION_ABORTED||stop.stop_requested()){
                    // A cancelled read can still have copied bytes into the buffer, and
                    // they are gone from the pipe. Frames here are delimited by byte
                    // count alone, so dropping them would shift every later frame by
                    // that many bytes for the rest of the session (SeekSeconds reuses
                    // the running child and its pipe).
                    m_pendingFrameBytes+=got;
                    return VideoReadResult::Cancelled;
                }
                // The child closing stdout surfaces here as ERROR_BROKEN_PIPE on this pipe
                // type (not a TRUE/got==0 return); let the child-exit classification below
                // decide what that means instead of reporting it as a read Error.
                if(err!=ERROR_BROKEN_PIPE)return VideoReadResult::Error;
                break;
            }
            if(!got){
                if(stop.stop_requested())return VideoReadResult::Cancelled;
                break;
            }
            m_pendingFrameBytes+=got;m_lastFrameByte=std::chrono::steady_clock::now();
            if(m_seekTimingPending){
                std::scoped_lock timingLock(m_seekTimingMutex);
                if(m_seekTiming.firstByteMs<0.0)m_seekTiming.firstByteMs=ElapsedMs(m_seekStart);
            }
            if(stop.stop_requested())return VideoReadResult::Cancelled;
            continue;
        }
        if(!available||m_pendingFrameBytes>=frameBytes)break;
        const DWORD want=static_cast<DWORD>(std::min<size_t>({frameBytes-m_pendingFrameBytes,static_cast<size_t>(available),size_t{16u<<20}}));
        DWORD got=0;
        ++m_frameReadCalls;
        if(!ReadFile(m_ffmpegStdout,m_pendingFrame.data()+m_pendingFrameBytes,want,&got,nullptr))return VideoReadResult::Error;
        if(!got)break;
        m_pendingFrameBytes+=got;m_lastFrameByte=std::chrono::steady_clock::now();
        if(m_seekTimingPending){
            std::scoped_lock timingLock(m_seekTimingMutex);
            if(m_seekTiming.firstByteMs<0.0)m_seekTiming.firstByteMs=ElapsedMs(m_seekStart);
        }
        if(stop.stop_requested())return VideoReadResult::Cancelled;
    }

    if(m_pendingFrameBytes<frameBytes){
        if(stop.stop_requested())return VideoReadResult::Cancelled;
        if(m_sourceKind==MediaSourceKind::YouTube&&std::chrono::steady_clock::now()-m_lastFrameByte>=m_networkStallTimeout){
            LOG("FFmpeg YouTube stream stalled before a complete frame.");StopFFmpeg(0);
            PublishSeekTiming(false);return VideoReadResult::Stalled;
        }
        if(m_ffmpegProcess&&WaitForSingleObject(m_ffmpegProcess,0)==WAIT_OBJECT_0){
            DWORD remaining=0;
            if(PeekNamedPipe(m_ffmpegStdout,nullptr,0,nullptr,&remaining,nullptr)&&remaining>0)
                return VideoReadResult::NotReady;
            DWORD exitCode=1;GetExitCodeProcess(m_ffmpegProcess,&exitCode);
            if(TryNextFFmpegAcceleration(exitCode))return VideoReadResult::NotReady;
            const VideoReadResult end=ClassifyFFmpegEnd(exitCode);
            PublishSeekTiming(false);
            return end;
        }
        return VideoReadResult::NotReady;
    }

    const int64_t sourceFrame=m_ffmpegSpawnFirstFrame+static_cast<int64_t>(m_ffmpegEmittedFrames);
    ++m_ffmpegEmittedFrames;
    m_pendingFrameBytes=0;
    // A forward seek that kept this child walks past the frames before its
    // target right here, on whichever thread is already reading the pipe: the
    // seek call itself never waits for them and the caller never sees them.
    // The buffer stays put, so skipping costs nothing but the pipe read.
    if(sourceFrame<m_ffmpegFirstSourceFrame)continue;

    out.bgra.swap(m_pendingFrame);
    out.layout = m_source.layout;
    RecycleFrameBuffer(std::move(m_pendingFrame));
    m_pendingFrame.clear();
    const int64_t timelineFrame=sourceFrame-m_ffmpegFirstSourceFrame;
    out.timestamp100ns = m_ffmpegSeekBase100ns +
        static_cast<int64_t>((static_cast<double>(timelineFrame) / m_source.fps) * 10000000.0);
    out.discontinuity = (timelineFrame == 0 && (m_ffmpegSeekBase100ns != 0 || m_restartDiscontinuity));
    out.frameNumber = static_cast<uint64_t>(std::llround(static_cast<double>(out.timestamp100ns) * m_source.fps * 1e-7));
    out.sourceGeneration = m_sourceGeneration;
    m_restartDiscontinuity = false;
    PublishSeekTiming(true);
    return VideoReadResult::FrameReady;
  }
}

void VideoDecoder::StartFrameQueue(QueueBuffer buffered) {
    if (m_backend != Backend::FFmpeg || m_frameThread.joinable()) return;
    {
        std::lock_guard lock(m_frameMutex);
        if(buffered==QueueBuffer::Discard){
            m_frameQueue.clear();
            m_frameTerminal = VideoReadResult::NotReady;
        }
        m_frameQueueEnabled = true;
    }
    try {
        m_frameThread = std::jthread([this](std::stop_token stop) { FrameQueueThread(stop); });
    } catch (const std::system_error& error) {
        std::lock_guard lock(m_frameMutex);
        m_frameQueueEnabled = false;
        LOG("Decoded-frame queue could not start; using synchronous reads. error=" << error.code().value());
    }
}

// The try covers the thread body, not only its creation: a frame buffer is
// 31 MiB at 4K and its allocation can fail, and an exception that leaves the
// thread terminates the process. A reader parked on the queue is told Error so
// it unwedges - unless a stop is already in flight, in which case StopFrameQueue
// has published its own answer and this thread's failure is moot.
void VideoDecoder::FrameQueueThread(std::stop_token stop) noexcept {
    try { FrameQueueLoop(stop); }
    catch (...) {
        LOG("Decoder frame queue stopped after an unexpected exception.");
        std::lock_guard lock(m_frameMutex);
        if (!stop.stop_requested()) m_frameTerminal = VideoReadResult::Error;
        m_frameCv.notify_all();
    }
}

void VideoDecoder::StopFrameQueue(QueueBuffer buffered) {
    // Publish the shutdown and wake blocked readers BEFORE the queue thread goes away.
    // FrameQueueLoop exits on its own stop token without setting a terminal state, so a
    // reader parked in ReadNextBlocking (the UI message pump, for local files) needs
    // this to learn that the answer is Cancelled rather than wait on a predicate that
    // can never become true again. A seek (QueueBuffer::Keep) starts a replacement
    // thread right away and judges child reuse against the terminal state, so only a
    // permanent stop marks Cancelled here.
    {
        std::lock_guard lock(m_frameMutex);
        m_frameQueueEnabled = false;
        if (buffered == QueueBuffer::Discard) m_frameTerminal = VideoReadResult::Cancelled;
    }
    m_frameCv.notify_all();
    if (m_frameThread.joinable()) {
        m_frameThread.request_stop();
        m_frameCv.notify_all();
        // A LocalFile queue thread can be parked in the blocking ReadFile added for
        // local sources, which the stop token alone never wakes. Cancel its pending I/O
        // and re-check: CancelSynchronousIo can race a thread that has not entered
        // ReadFile yet (it then returns FALSE/ERROR_NOT_FOUND, harmlessly), so keep
        // cancelling until a wait on the thread itself stops timing out.
        DWORD waitResult;
        do {
            CancelSynchronousIo(m_frameThread.native_handle());
            waitResult = WaitForSingleObject(m_frameThread.native_handle(), 1);
        } while (waitResult == WAIT_TIMEOUT);
        m_frameThread.join();
    }
    std::lock_guard lock(m_frameMutex);
    if (buffered == QueueBuffer::Discard) {
        m_frameQueue.clear();
        // m_frameTerminal deliberately keeps the Cancelled state. StartFrameQueue resets it.
        // Clearing it back to NotReady here would re-arm the wedge for a reader that has not
        // observed the shutdown yet.
    }
}

void VideoDecoder::FrameQueueLoop(std::stop_token stop) {
    // Where this thread's time goes, reported once when it exits. The export loop
    // measures only its own wait for a frame; splitting the producer side into
    // pipe reads, empty-pipe polls and full-queue waits says whether the child,
    // the copy out of the pipe, or the consumer is the one setting the pace.
    using Clock = std::chrono::steady_clock;
    Clock::duration readNanos{}, pollNanos{}, sleepNanos{}, spaceNanos{};
    uint64_t frames = 0, polls = 0, spaceWaits = 0;
    const auto loopStart = Clock::now();
    m_frameBufferFills = 0;
    m_frameBlockedNanos = Clock::duration{};
    m_frameReadCalls = 0;
    // Local files can wait in the kernel for the next pipe byte instead of polling for
    // it; network sources keep polling because their stall detection lives in the
    // periodic NotReady return from ReadNextFFmpegProcessAvailable.
    const bool block = (m_sourceKind == MediaSourceKind::LocalFile);
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(m_frameMutex);
            if (m_frameQueue.size() >= FrameQueueCapacity) {
                ++spaceWaits;
                const auto waited = Clock::now();
                const bool ok = m_frameCv.wait(lock, stop, [this] { return m_frameQueue.size() < FrameQueueCapacity; });
                spaceNanos += Clock::now() - waited;
                if (!ok) break;
            }
        }

        VideoFrame frame;
        const auto readStart = Clock::now();
        const VideoReadResult result = ReadNextFFmpegProcessAvailable(frame, stop, block);
        const auto readEnd = Clock::now();
        if (result == VideoReadResult::FrameReady) {
            ++frames;readNanos += readEnd - readStart;
            std::lock_guard lock(m_frameMutex);
            // Keep the frame even when a stop lands between reading and
            // queueing it: it has already been taken out of the pipe and
            // counted, so dropping it would leave the position bookkeeping
            // claiming a frame the caller never got, and a seek that reuses
            // this child would then hand out the frame before its target.
            m_frameQueue.push_back(std::move(frame));
            m_frameCv.notify_all();
            if (stop.stop_requested()) break;
        } else if (result == VideoReadResult::NotReady) {
            ++polls;pollNanos += readEnd - readStart;
            SleepPreciseMs(1);
            sleepNanos += Clock::now() - readEnd;
        } else if (result != VideoReadResult::Cancelled) {
            std::lock_guard lock(m_frameMutex);
            m_frameTerminal = result;
            m_frameCv.notify_all();
            break;
        }
    }
    if (frames) {
        const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
        const double wall = ms(Clock::now() - loopStart);
        // With blocking reads (LocalFile), readNanos includes the in-kernel wait for the
        // child; m_frameBlockedNanos isolates that wait so pipeRead keeps meaning "time
        // actually copying bytes out of the pipe". Clamp: a blocked wait can land on an
        // iteration whose call did not end in FrameReady, so it is not always <= readNanos.
        const Clock::duration blocked = std::min(m_frameBlockedNanos, readNanos);
        LOG("Decoder frame queue: frames=" << frames << " wallMs=" << std::fixed << std::setprecision(1) << wall
            << " perFrameMs: pipeRead=" << std::setprecision(3) << ms(readNanos - blocked) / double(frames)
            << " blockedWait=" << ms(m_frameBlockedNanos) / double(frames)
            << " emptyPoll=" << ms(pollNanos) / double(frames) << " emptySleep=" << ms(sleepNanos) / double(frames)
            << " queueFullWait=" << ms(spaceNanos) / double(frames)
            << " other=" << (wall - ms(readNanos + pollNanos + sleepNanos + spaceNanos)) / double(frames)
            << " reads=" << m_frameReadCalls << " polls=" << polls << " queueFullWaits=" << spaceWaits << " zeroFills=" << m_frameBufferFills);
    }
}

VideoReadResult VideoDecoder::ReadNextFFmpegAvailable(VideoFrame& out,std::stop_token stop) {
    {
        std::lock_guard lock(m_frameMutex);
        if (m_frameQueueEnabled) {
            if (stop.stop_requested()) return VideoReadResult::Cancelled;
            if (!m_frameQueue.empty()) {
                RecycleFrameBuffer(std::move(out.bgra));
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
    MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &m_source.width, &m_source.height);
    m_source.nativeWidth=m_source.width; m_source.nativeHeight=m_source.height;
    m_source.displayAspect = m_source.height ? double(m_source.width)/double(m_source.height) : 16.0/9.0;
    UINT32 frN = 0, frD = 0;
    if (SUCCEEDED(MFGetAttributeRatio(current.Get(), MF_MT_FRAME_RATE, &frN, &frD)) && frD) {
        m_source.fps = double(frN) / double(frD);
        // A rate the reader stated, so FrameRateKnown() is true here rather than
        // reporting the 30.0 default as the source's own. MF exposes one nominal
        // rate and no frames-over-duration average, so there is no second rate to
        // corroborate it with and ConstantFrameRate() stays false.
        m_source.avgFrameRate = m_source.fps;
    }

    UINT32 strideU = 0;
    if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideU)))
        m_source.stride = static_cast<int32_t>(strideU);
    else
        m_source.stride = static_cast<int32_t>(m_source.width * 4);

    PROPVARIANT var{};
    PropVariantInit(&var);
    if (SUCCEEDED(m_reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var))) {
        if (var.vt == VT_UI8 || var.vt == VT_I8)
            m_source.durationSec = static_cast<double>(var.vt == VT_I8 ? var.hVal.QuadPart : static_cast<LONGLONG>(var.uhVal.QuadPart)) / 10000000.0;
    }
    PropVariantClear(&var);

    LOG("Media Foundation: " << m_source.width << "x" << m_source.height << " @ " << m_source.fps << " fps, duration=" << m_source.durationSec);
    return m_source.width > 0 && m_source.height > 0;
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

        const size_t dstStride = static_cast<size_t>(m_source.width) * 4u;
        out.bgra.resize(dstStride * m_source.height);

        int32_t stride = m_source.stride;
        size_t absStride = static_cast<size_t>(std::abs(stride));
        if (absStride * m_source.height > curLen) {
            stride = static_cast<int32_t>(dstStride);
            absStride = dstStride;
        }

        const BYTE* firstRow = data;
        if (stride < 0) firstRow = data + absStride * (m_source.height - 1);

        for (uint32_t y = 0; y < m_source.height; ++y) {
            const BYTE* src = stride >= 0 ? firstRow + absStride * y : firstRow - absStride * y;
            memcpy(out.bgra.data() + dstStride * y, src, std::min(dstStride, absStride));
        }
        buffer->Unlock();

        out.timestamp100ns = timestamp;
        out.discontinuity = (flags & MF_SOURCE_READERF_STREAMTICK) != 0;
        out.frameNumber = static_cast<uint64_t>(std::llround(static_cast<double>(timestamp) * m_source.fps * 1e-7));
        out.sourceGeneration = m_sourceGeneration;
        out.layout = VideoPixelLayout::Bgra; // Media Foundation's reader only ever hands out BGRA.
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

VideoReadResult VideoDecoder::ReadNextBlocking(VideoFrame& out, std::stop_token stop) {
    if (m_backend == Backend::FFmpeg) {
        {
            std::unique_lock lock(m_frameMutex);
            // Wait in slices rather than indefinitely so a queue thread that disappears
            // without publishing a terminal state cannot wedge the caller. The terminal
            // state is consulted before the enabled flag: a shutdown publishes both
            // together, and the answer to a reader parked across Close() is Cancelled,
            // not whatever the raw pipe below happens to say mid-teardown.
            for (;;) {
                if (stop.stop_requested()) return VideoReadResult::Cancelled;
                if (!m_frameQueue.empty()) {
                    out = std::move(m_frameQueue.front());
                    m_frameQueue.pop_front();
                    m_frameCv.notify_all();
                    return VideoReadResult::FrameReady;
                }
                if (m_frameTerminal != VideoReadResult::NotReady) return m_frameTerminal;
                if (!m_frameQueueEnabled) break;
                (void)m_frameCv.wait_for(lock, stop, std::chrono::milliseconds(50), [this] {
                    return !m_frameQueue.empty() ||
                           m_frameTerminal != VideoReadResult::NotReady ||
                           !m_frameQueueEnabled;
                });
            }
            // A permanent stop clears m_frameQueueEnabled and publishes the terminal
            // state in the same critical section, so a reader that leaves the loop on
            // the flag alone drops that state on the floor. It would then fall into the
            // synchronous path below against a child that Close is already tearing down,
            // and report the closed pipe as EndOfStream instead of Cancelled.
            if (m_frameTerminal != VideoReadResult::NotReady) return m_frameTerminal;
        }
        for (;;) {
            const VideoReadResult result = ReadNextFFmpegProcessAvailable(out, stop);
            if (result != VideoReadResult::NotReady) return result;
            if (stop.stop_requested()) return VideoReadResult::Cancelled;
            // A yield() spin here pegs a core. This fallback only runs when the queue
            // thread could not be created, but it must still idle politely.
            SleepPreciseMs(2);
        }
    }
    if (m_backend == Backend::MediaFoundation) {
        return ReadNextMediaFoundation(out) ? VideoReadResult::FrameReady : VideoReadResult::EndOfStream;
    }
    return VideoReadResult::Error;
}

VideoDecoder::SeekTiming VideoDecoder::LastSeekTiming() const {
    std::scoped_lock timingLock(m_seekTimingMutex);
    return m_seekTiming;
}

// One line per seek, emitted once the seek's outcome is known: the four parts a
// seek can spend time in are the only way to tell a slow container reopen from
// a slow process launch, and the reuse decision below is judged against them.
void VideoDecoder::PublishSeekTiming(bool frameDelivered) {
    if(!m_seekTimingPending)return;
    m_seekTimingPending=false;
    SeekTiming timing;
    {
        std::scoped_lock timingLock(m_seekTimingMutex);
        if(frameDelivered&&m_seekTiming.firstFrameMs<0.0)
            m_seekTiming.firstFrameMs=ElapsedMs(m_seekStart);
        timing=m_seekTiming;
    }
    // A restart's real cost is the budget every later forward seek is judged
    // against, so learn it from the restarts this source actually pays for
    // instead of assuming one number for every codec and resolution. Cheap
    // restarts have to count quickly and expensive ones slowly: keeping a child
    // suppresses the very restarts that would correct an overestimate, and an
    // overestimate is what makes an all-intra source walk past frames it should
    // have restarted for.
    if(!timing.reusedChild&&timing.firstFrameMs>0.0)
        m_restartFirstFrameMs=timing.firstFrameMs<m_restartFirstFrameMs
            ?0.3*m_restartFirstFrameMs+0.7*timing.firstFrameMs
            :0.8*m_restartFirstFrameMs+0.2*timing.firstFrameMs;
    // Likewise the price of walking past a frame: a reused seek that had to
    // wait for the child measures exactly that.
    if(timing.reusedChild&&timing.drainedFrames&&timing.firstFrameMs>0.0)
        m_drainMsPerFrame=0.7*m_drainMsPerFrame+
            0.3*timing.firstFrameMs/static_cast<double>(timing.drainedFrames);
    LOG("Seek timing: target="<<std::fixed<<std::setprecision(3)<<m_seekTargetSeconds
        <<"s mode="<<(timing.reusedChild?"reuse":"restart")
        <<" teardownMs="<<timing.teardownMs<<" spawnMs="<<timing.spawnMs
        <<" firstByteMs="<<timing.firstByteMs<<" firstFrameMs="<<timing.firstFrameMs
        <<" callMs="<<timing.callMs<<" forwardFrames="<<timing.forwardFrames
        <<" drainedFrames="<<timing.drainedFrames);
}

// A forward seek only costs the running child the frames it has to walk past.
// Keeping it skips the expensive parts of a restart - process teardown, the
// container reopen, hwaccel init and the redecode from the preceding keyframe -
// so any target the child reaches for less than a restart's measured cost is
// served here. The frames walked past must never reach the caller, and the
// frame the caller does get must be the one a restart would have handed out.
VideoDecoder::SeekReuse VideoDecoder::ReuseRunningChildForSeek(double seconds)
{
    if(!m_ffmpegProcess||!m_ffmpegStdout||m_source.stillImage||m_source.gif)return SeekReuse::Restart;
    size_t buffered=0;
    {
        std::lock_guard lock(m_frameMutex);
        // A child that already ended cannot walk anywhere.
        if(m_frameTerminal!=VideoReadResult::NotReady)return SeekReuse::Restart;
        buffered=m_frameQueue.size();
    }
    const int64_t target=FirstFrameAtOrAfter(seconds,m_source.fps);
    // Everything the child has emitted was either handed out or is still
    // buffered, so this is the next frame the caller can be given.
    const int64_t nextDeliverable=m_ffmpegSpawnFirstFrame+
        static_cast<int64_t>(m_ffmpegEmittedFrames)-static_cast<int64_t>(buffered);
    const int64_t skip=target-nextDeliverable;
    {
        std::scoped_lock timingLock(m_seekTimingMutex);
        m_seekTiming.forwardFrames=skip;
    }
    // Rewinding is what keyframes are for: only a restart can go backwards.
    if(skip<0)return SeekReuse::Restart;

    // Frames already decoded cost nothing to throw away, so only the ones the
    // child still has to produce are weighed against a restart - plus the
    // target frame itself, which is normally not decoded yet either.
    const size_t fromQueue=static_cast<size_t>(std::min<int64_t>(skip,static_cast<int64_t>(buffered)));
    const int64_t pipeFrames=skip-static_cast<int64_t>(fromQueue);
    if(static_cast<double>(pipeFrames+1)*m_drainMsPerFrame>=m_restartFirstFrameMs)
        return SeekReuse::Restart;
    ++m_sourceGeneration;
    m_ffmpegSeekBase100ns=static_cast<int64_t>(seconds*10000000.0);
    // The read path hands out nothing below this and labels everything relative
    // to it, so the remaining frames before the target are walked past there,
    // asynchronously, instead of on the seeking thread.
    m_ffmpegFirstSourceFrame=target;
    m_restartDiscontinuity=false;
    size_t relabeled=0;
    {
        std::lock_guard lock(m_frameMutex);
        // Buffered frames were already labelled on the old timeline. Those
        // before the target go, and the rest are exactly the frames a restart
        // would have emitted first: relabel them, because everything downstream
        // pairs and paces on these numbers and must not be able to tell a
        // reused seek from a restarted one.
        m_frameQueue.erase(m_frameQueue.begin(),m_frameQueue.begin()+static_cast<std::ptrdiff_t>(fromQueue));
        for(VideoFrame& frame:m_frameQueue){
            const int64_t timelineFrame=static_cast<int64_t>(relabeled);
            frame.timestamp100ns=m_ffmpegSeekBase100ns+
                static_cast<int64_t>((static_cast<double>(timelineFrame)/m_source.fps)*10000000.0);
            frame.discontinuity=(timelineFrame==0&&m_ffmpegSeekBase100ns!=0);
            frame.frameNumber=static_cast<uint64_t>(std::llround(static_cast<double>(frame.timestamp100ns)*m_source.fps*1e-7));
            frame.sourceGeneration=m_sourceGeneration;
            ++relabeled;
        }
    }
    {
        std::scoped_lock timingLock(m_seekTimingMutex);
        m_seekTiming.reusedChild=true;
        m_seekTiming.drainedFrames=static_cast<uint64_t>(pipeFrames);
    }
    m_seekReusedBuffered=relabeled!=0;
    return SeekReuse::Reused;
}

bool VideoDecoder::SeekSeconds(double seconds) {
    seconds = std::clamp(seconds, 0.0, std::max(0.0, m_source.durationSec));
    if (m_backend == Backend::FFmpeg) {
        const auto seekStarted=std::chrono::steady_clock::now();
        const bool restartQueue=m_frameQueueEnabled;
        // The queue thread is the other reader of the pipe and of the position
        // bookkeeping, so join it before deciding how to serve this seek.
        if(restartQueue)StopFrameQueue(QueueBuffer::Keep);
        // Every child is started on the frame grid, so the target is that grid
        // position: a reused seek then rebases onto exactly the timeline a
        // restarted one would have produced.
        const double aligned=m_source.stillImage?seconds:SnapToFrameGrid(seconds,m_source.fps);
        m_seekStart=seekStarted;m_seekTargetSeconds=aligned;
        {std::scoped_lock timingLock(m_seekTimingMutex);m_seekTiming=SeekTiming{};}
        m_seekTimingPending=true;
        const SeekReuse reuse=ReuseRunningChildForSeek(aligned);
        bool started=true;
        if(reuse==SeekReuse::Restart){
            {std::lock_guard lock(m_frameMutex);m_frameQueue.clear();m_frameTerminal=VideoReadResult::NotReady;}
            started=StartFFmpeg(aligned);
            if(started)++m_sourceGeneration;
        }
        {std::scoped_lock timingLock(m_seekTimingMutex);m_seekTiming.callMs=ElapsedMs(seekStarted);}
        if(!started){m_seekTimingPending=false;return false;}
        // A reused child whose target frame was already decoded has finished the
        // seek right here: no later read will complete the timing line. Publish
        // before the queue thread exists again, so nothing races for it.
        if(reuse==SeekReuse::Reused&&m_seekReusedBuffered)PublishSeekTiming(true);
        if(restartQueue)StartFrameQueue(QueueBuffer::Keep);
        return true;
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
    ++m_sourceGeneration;
    return true;
}
