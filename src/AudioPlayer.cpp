#include "AudioPlayer.h"
#include "PlatformPaths.h"
#include "HardErrorSuppression.h"
#include "KillOnCloseJob.h"
#include "MediaTools.h"
#include "Log.h"
#include <filesystem>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <iterator>
#include <chrono>
#include <cstddef>
#include <thread>

namespace fs = std::filesystem;
static std::wstring Q(const std::wstring& s) { return L"\"" + s + L"\""; }

AudioPlayer::~AudioPlayer() { Stop(); }

AudioPlayer::ReaderState::~ReaderState()
{
    if (stdoutPipe && !CloseHandle(stdoutPipe)) LOG("Audio: CloseHandle(stdout) failed winerr=" << GetLastError());
    if (stderrPipe && !CloseHandle(stderrPipe)) LOG("Audio: CloseHandle(stderr) failed winerr=" << GetLastError());
    if (process && !CloseHandle(process)) LOG("Audio: CloseHandle(process) failed winerr=" << GetLastError());
    if (job && !CloseHandle(job)) LOG("Audio: CloseHandle(job) failed winerr=" << GetLastError());
    renderer.reset();
    if (completed && !CloseHandle(completed)) LOG("Audio: CloseHandle(completed) failed winerr=" << GetLastError());
}

std::wstring AudioPlayer::FindTool(const wchar_t* name) const {
    return media_tools::FindTool(m_settings.helperDirectory, name, media_tools::Fallback::SearchPath).wstring();
}

namespace {

// A pipe for a helper's stderr: the read end stays here, the write end is
// inherited. False leaves both null, and the caller sends stderr to NUL.
bool CreateStderrPipe(SECURITY_ATTRIBUTES& sa, HANDLE& readEnd, HANDLE& writeEnd)
{
    readEnd = writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 64 * 1024)) { readEnd = writeEnd = nullptr; return false; }
    if (!SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readEnd); CloseHandle(writeEnd); readEnd = writeEnd = nullptr; return false;
    }
    return true;
}

// Reads only what is already in the pipe, so it can never block the caller -
// which is the reader thread, whose next job is feeding the endpoint.
void DrainAvailable(HANDLE pipe, audio_stderr::Tail& tail)
{
    if (!pipe) return;
    char buffer[4096];
    for (int round = 0; round < 64; ++round) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || !available) return;
        DWORD got = 0;
        if (!ReadFile(pipe, buffer, std::min<DWORD>(available, DWORD(sizeof(buffer))), &got, nullptr) || !got)
            return;
        tail.Append(buffer, got);
    }
}

// Runs a helper and returns its standard output. Bounded in both directions:
// a helper that never exits is killed with its job, and one that floods the
// pipe is cut off. A track list is a few hundred bytes.
//
// Both pipes are polled rather than read blocking: a blocking read of stdout
// never returned from a helper that hung, and could not be combined with
// draining stderr, which a helper blocks on once its pipe fills.
bool CaptureHelperOutput(const std::wstring& exe, const std::wstring& arguments, std::string& out,
                         audio_stderr::Tail& errors)
{
    constexpr size_t kOutputLimit = 1u << 20;
    constexpr DWORD kTimeoutMs = 10000;
    out.clear();

    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 64 * 1024)) return false;
    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readPipe); CloseHandle(writePipe); return false;
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nul == INVALID_HANDLE_VALUE) nul = nullptr;
    HANDLE errorRead = nullptr, errorWrite = nullptr;
    CreateStderrPipe(sa, errorRead, errorWrite);

    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul; si.hStdOutput = writePipe; si.hStdError = errorWrite ? errorWrite : nul;
    std::wstring command = Q(exe) + L" " + arguments;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    HANDLE job = CreateKillOnCloseJob();
    PROCESS_INFORMATION pi{};
    const ScopedHardErrorSuppression noHardErrorDialog;
    const BOOL started = job && CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
                                               CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (errorWrite) CloseHandle(errorWrite);
    if (nul) CloseHandle(nul);
    if (!started) { CloseHandle(readPipe); if (errorRead) CloseHandle(errorRead); if (job) CloseHandle(job); return false; }
    if (!AssignProcessToJobObject(job, pi.hProcess) || ResumeThread(pi.hThread) == DWORD(-1)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(readPipe); CloseHandle(job);
        if (errorRead) CloseHandle(errorRead);
        return false;
    }
    CloseHandle(pi.hThread);

    const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
    char buffer[8192];
    for (;;) {
        DrainAvailable(errorRead, errors);
        DWORD available = 0;
        // Fails once the helper has closed stdout and everything is read.
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)) break;
        if (!available) {
            if (GetTickCount64() >= deadline) break;
            Sleep(1);
            continue;
        }
        DWORD got = 0;
        if (!ReadFile(readPipe, buffer, std::min<DWORD>(available, DWORD(sizeof(buffer))), &got, nullptr) || !got)
            break;
        if (out.size() + got > kOutputLimit) break;
        out.append(buffer, got);
    }
    CloseHandle(readPipe);
    const ULONGLONG now = GetTickCount64();
    const DWORD waited = WaitForSingleObject(pi.hProcess, now < deadline ? DWORD(deadline - now) : 0);
    DrainAvailable(errorRead, errors);
    if (errorRead) CloseHandle(errorRead);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    // Closing the kill-on-close job reaps a helper that ignored the deadline.
    CloseHandle(job);
    return waited == WAIT_OBJECT_0 && code == 0;
}

} // namespace

void AudioPlayer::ProbeAudioTracks(const std::wstring& videoPath) {
    if (videoPath == m_tracksPath) return;
    m_tracksPath = videoPath;
    m_tracks.clear();
    m_probedTracks.clear();
    m_tracksProbed = false;
    m_selectedTrack = 0;

    const std::wstring ffprobe = FindTool(L"ffprobe.exe");
    if (ffprobe.empty()) {
        LOG("Audio: ffprobe was not found, so the track list is unavailable; playing the first stream.");
        return;
    }
    std::wstring inputOptions;
    if (_wcsnicmp(videoPath.c_str(), L"https://", 8) == 0 || _wcsnicmp(videoPath.c_str(), L"http://", 7) == 0)
        inputOptions = L"-tls_verify 1 -protocol_whitelist https,tls,tcp ";

    std::string text;
    audio_stderr::Tail errors;
    if (!CaptureHelperOutput(ffprobe,
            L"-v error -select_streams a "
            L"-show_entries stream=index,codec_name,channels,sample_rate:stream_tags=language,title:"
            L"stream_disposition=default,comment,visual_impaired,descriptions,hearing_impaired "
            L"-of default=noprint_wrappers=0 " + inputOptions + L"-i " + Q(videoPath), text, errors)) {
        LOG("Audio: the track list could not be read; playing the first stream.");
        if (!errors.Empty()) LOG("Audio: ffprobe said: " << errors.Line());
        return;
    }

    // [STREAM] wrappers rather than a flat list: without them ffprobe runs
    // consecutive streams' entries together with no separator, and a track
    // missing an optional tag would silently absorb the next one's.
    std::vector<audio_track::Track> tracks;
    audio_track::Track current;
    bool inStream = false;
    std::istringstream lines(text);
    std::string line;
    const auto flag = [](const std::string& value) { return value == "1"; };
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[STREAM]") { current = {}; current.audioIndex = int(tracks.size()); inStream = true; continue; }
        if (line == "[/STREAM]") { if (inStream) tracks.push_back(current); inStream = false; continue; }
        if (!inStream) continue;
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals), value = line.substr(equals + 1);
        if (key == "codec_name") current.codec = value;
        else if (key == "channels") { try { current.channels = std::stoi(value); } catch (...) {} }
        else if (key == "sample_rate") { try { current.sampleRate = std::stoi(value); } catch (...) {} }
        else if (key == "TAG:language") current.language = value;
        else if (key == "TAG:title") current.title = value;
        else if (key == "DISPOSITION:default") current.isDefault = flag(value);
        else if (key == "DISPOSITION:comment") current.comment = flag(value);
        else if (key == "DISPOSITION:visual_impaired") current.visualImpaired = flag(value);
        else if (key == "DISPOSITION:descriptions") current.descriptions = flag(value);
        else if (key == "DISPOSITION:hearing_impaired") current.hearingImpaired = flag(value);
    }

    m_probedTracks = tracks;
    m_tracksProbed = true;
    // One track needs no menu and no decision; the list stays empty so
    // everything downstream takes the path it always did.
    if (tracks.size() < 2) return;
    m_tracks = std::move(tracks);
    const size_t chosen = audio_track::SelectDefault(m_tracks);
    m_selectedTrack = chosen == audio_track::kNoTrack ? 0 : m_tracks[chosen].audioIndex;
    LOG("Audio: " << m_tracks.size() << " tracks; opening on "
        << audio_track::Describe(m_tracks[size_t(m_selectedTrack)]) << '.');
}

bool AudioPlayer::SelectAudioTrack(int audioIndex) {
    if (audioIndex < 0 || size_t(audioIndex) >= m_tracks.size()) return false;
    if (audioIndex == m_selectedTrack) return true;
    m_selectedTrack = audioIndex;
    LOG("Audio: switching to " << audio_track::Describe(m_tracks[size_t(audioIndex)]) << '.');
    // A track change is a stream change, so the child has to be respawned.
    // Resuming where the clock is keeps the switch where the viewer was.
    const double resumeAt = m_path.empty() ? 0.0 : std::max(0.0, m_lastKnownPosition.load());
    return Seek(resumeAt);
}

bool AudioPlayer::Start(const std::wstring& videoPath, double seekSeconds, AudioStartState state) {
    Stop();
    m_seekBaseSec = std::max(0.0, seekSeconds);
    { std::lock_guard<std::mutex> lock(m_clockMutex); audio_clock::Reset(m_clock); audio_clock::Reset(m_continuity); m_clockStalled = false; }
    // Where this start begins is the newest position there is. The clock only
    // refreshes this while it is being read, which it is not while paused, so
    // a paused seek from 60 s to 10 s left 60 behind: a track change or a
    // device restart then resumed audio at 60, and on resume the audio clock
    // dragged video forward 50 s through a drop loop that froze the UI.
    m_lastKnownPosition.store(m_seekBaseSec);
    m_path = videoPath;
    m_ffmpeg = FindFFmpeg();
    if (m_ffmpeg.empty()) { LOG("Audio: ffmpeg.exe not found."); return false; }
    ProbeAudioTracks(videoPath);

    auto reader=std::make_shared<ReaderState>();
    reader->disableAudioDevice=m_settings.faults.disableAudioDevice;
    reader->paused=state==AudioStartState::Paused;
    reader->completed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!reader->completed){LOG("Audio: CreateEvent(reader completion) failed winerr="<<GetLastError());return false;}

    // The endpoint is opened first so ffmpeg can be told to produce exactly
    // the mix format, rather than the fixed 16-bit 48 kHz waveOut was opened
    // at and Windows then converted again.
    WasapiRenderer::Format format{};
    bool bitstream=false;
    if(!reader->disableAudioDevice){
        // Passthrough first when it applies; anything short of an open
        // exclusive stream falls through to the PCM open below, never to
        // silence.
        reader->renderer=OpenPassthrough();
        bitstream=reader->renderer!=nullptr;
        if(!reader->renderer)reader->renderer=std::make_unique<WasapiRenderer>();
        if(!bitstream&&!reader->renderer->Open()){const bool noEndpoint=reader->renderer->NoEndpoint();reader->renderer.reset();LOG("Audio: the render endpoint could not be opened.");AwaitEndpoint(noEndpoint);return false;}
        format=reader->renderer->CurrentFormat();
        reader->sampleRate=format.sampleRate;
        reader->renderer->SetVolume(m_volume);
        // Started only when playing: a paused stream that was started would
        // run the endpoint dry and advance its clock over silence.
        if(!reader->paused&&!reader->renderer->Start()){LOG("Audio: the endpoint refused to start.");AwaitEndpoint(false);return false;}
    }else m_passthroughStatus={};
    if (!StartProcess(seekSeconds,reader,format,bitstream)) return false;
    m_reader=reader;
    try{m_thread=std::thread(&AudioPlayer::ReaderThread,reader);}catch(...){StopProcess(reader);m_reader.reset();throw;}
    return true;
}

std::unique_ptr<WasapiRenderer> AudioPlayer::OpenPassthrough() {
    using namespace audio_passthrough;
    const Status previous=m_passthroughStatus;
    m_passthroughStatus={};
    if(!m_passthroughEnabled)return nullptr;
    // The selected track as ffprobe described it. Nothing described - no
    // ffprobe, or a stream it could not read - is not a track that can be
    // passed through, and says so rather than guessing at a codec.
    const audio_track::Track* track=nullptr;
    for(const auto& candidate:m_probedTracks)
        if(candidate.audioIndex==m_selectedTrack){track=&candidate;break;}
    // A source ffprobe read and found no audio in has nothing to pass
    // through and nothing to explain: it plays silent, as it always did.
    if(!track&&m_tracksProbed&&m_probedTracks.empty())return nullptr;
    m_passthroughStatus.state=State::NotApplicable;
    if(track){
        m_passthroughStatus.trackCodec=track->codec;
        m_passthroughStatus.codec=CodecFromName(track->codec);
    }
    const std::optional<Link> link=track?PlanLink(m_passthroughStatus.codec,uint32_t(std::max(0,track->sampleRate)),track->channels)
                                         :std::nullopt;
    if(!link){
        if(!(previous==m_passthroughStatus))
            LOG("Audio: passthrough is on, but the track is "<<(track?track->codec:std::string("unknown"))
                <<(track?" at "+std::to_string(track->sampleRate)+" Hz":std::string())
                <<", which is not AC-3, E-AC-3 or DTS at a rate IEC 61937 carries; playing PCM.");
        return nullptr;
    }
    auto renderer=std::make_unique<WasapiRenderer>();
    switch(renderer->OpenPassthrough(*link)){
        case WasapiRenderer::PassthroughOpen::Opened:
            m_passthroughStatus.state=State::Active;
            return renderer;
        case WasapiRenderer::PassthroughOpen::Refused:
            m_passthroughStatus.state=State::Refused;
            break;
        case WasapiRenderer::PassthroughOpen::Failed:
            m_passthroughStatus.state=State::Unavailable;
            break;
    }
    // Closed here, before the PCM open, so the exclusive client is gone
    // before a shared one asks for the same endpoint.
    renderer.reset();
    return nullptr;
}

void AudioPlayer::NoteDisplayModeChanged() {
    const double now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    audio_passthrough::NoteDisplayChange(m_displayReopen,now,m_passthroughStatus.state);
}

bool AudioPlayer::StartProcess(double seekSeconds,const std::shared_ptr<ReaderState>& state,
                               const WasapiRenderer::Format& format, bool bitstream) {
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 1024 * 1024)) return false;
    if(!SetHandleInformation(readPipe,HANDLE_FLAG_INHERIT,0)){CloseHandle(readPipe);CloseHandle(writePipe);return false;}
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nul == INVALID_HANDLE_VALUE) nul = nullptr;
    HANDLE errorRead = nullptr, errorWrite = nullptr;
    if (!CreateStderrPipe(sa, errorRead, errorWrite))
        LOG("Audio: no pipe for ffmpeg's stderr winerr=" << GetLastError() << "; its errors go unrecorded.");

    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul; si.hStdOutput = writePipe; si.hStdError = errorWrite ? errorWrite : nul;
    std::wostringstream args;
    args << L"-hide_banner -loglevel error -nostdin ";
    if (seekSeconds > 0.0) args << L"-ss " << std::fixed << std::setprecision(6) << seekSeconds << L" ";
    // A resolved YouTube audio stream is https and nothing else: verify the
    // certificate explicitly and refuse every protocol the URL cannot need,
    // the same input options VideoDecoder and MediaPipeline put ahead of theirs.
    if (_wcsnicmp(m_path.c_str(), L"https://", 8) == 0 || _wcsnicmp(m_path.c_str(), L"http://", 7) == 0)
        args << L"-tls_verify 1 -protocol_whitelist https,tls,tcp ";
    // Matching the endpoint's mix format means neither ffmpeg nor the Windows
    // mixer resamples or requantizes: the samples the decoder produces are the
    // samples the endpoint is handed. With the device disabled there is no
    // format to match, so the old fixed one keeps that path unchanged.
    const uint32_t rate = format.Valid() ? format.sampleRate : 48000u;
    const uint16_t channels = format.Valid() ? format.channels : uint16_t{2};
    const bool asFloat = format.Valid();
    // The chosen stream, not simply the first: a rip that lists the
    // director's commentary first used to play the commentary. Still
    // optional, so a video-only source produces an empty output rather than
    // a failure.
    args << L"-i " << Q(m_path) << L" -map 0:a:" << m_selectedTrack << L"? -vn -sn -dn";
    // Passthrough: the track as it is, wrapped in IEC 61937 bursts at the
    // rate the endpoint was opened at. The spdif muxer sends a DTS-HD track's
    // core, which is what -dtshd_rate's default of 0 asks for.
    if (bitstream) args << L" -c:a copy -f spdif pipe:1";
    else args << L" -ac " << channels << L" -ar " << rate
              << (asFloat ? L" -c:a pcm_f32le -f f32le pipe:1" : L" -c:a pcm_s16le -f s16le pipe:1");
    std::wstring cmd = Q(m_ffmpeg) + L" " + args.str();
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end()); mutableCmd.push_back(L'\0');
    HANDLE job=CreateKillOnCloseJob();
    PROCESS_INFORMATION pi{};
    const ScopedHardErrorSuppression noHardErrorDialog;
    BOOL ok = job&&CreateProcessW(m_ffmpeg.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW|CREATE_SUSPENDED,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe); if (errorWrite) CloseHandle(errorWrite); if (nul) CloseHandle(nul);
    if (!ok) { const DWORD error=GetLastError(); CloseHandle(readPipe);if(errorRead)CloseHandle(errorRead);if(job)CloseHandle(job); LOG("Audio: CreateProcess(ffmpeg) failed winerr=" << error); return false; }
    if(!AssignProcessToJobObject(job,pi.hProcess)){if(!TerminateProcess(pi.hProcess,1))LOG("Audio: failed to terminate unassigned child winerr="<<GetLastError());const DWORD waited=WaitForSingleObject(pi.hProcess,500);if(waited!=WAIT_OBJECT_0)LOG("Audio: unassigned child did not exit within bound result="<<waited);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(readPipe);if(errorRead)CloseHandle(errorRead);CloseHandle(job);return false;}
    if(ResumeThread(pi.hThread)==DWORD(-1)){LOG("Audio: ResumeThread failed winerr="<<GetLastError());if(!TerminateJobObject(job,1))LOG("Audio: failed to terminate suspended job winerr="<<GetLastError());WaitForSingleObject(pi.hProcess,500);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(readPipe);if(errorRead)CloseHandle(errorRead);CloseHandle(job);return false;}
    CloseHandle(pi.hThread);state->process=pi.hProcess;state->stdoutPipe=readPipe;state->stderrPipe=errorRead;state->job=job;
    LOG("Audio: FFmpeg " << (bitstream ? "IEC 61937 passthrough" : "PCM") << " path started at " << seekSeconds << " s.");
    return true;
}

void AudioPlayer::StopProcess(const std::shared_ptr<ReaderState>& state) {
    // Closing a kill-on-close job is the fallback when the explicit job
    // termination API fails. The process handle remains owned until the reader
    // has exited, so no handle used by ThreadMain is closed concurrently.
    if(state->job){
        BOOL terminated=FALSE;
        if(!m_settings.faults.failTerminateJob)
            terminated=TerminateJobObject(state->job,0);
        if(!terminated)LOG("Audio: job termination fallback engaged winerr="<<GetLastError());
        if(!CloseHandle(state->job))LOG("Audio: CloseHandle(job) failed winerr="<<GetLastError());
        state->job=nullptr;
    }
    if(state->process){
        DWORD waitResult=WAIT_TIMEOUT;
        if(!m_settings.faults.failInitialProcessWait)
            waitResult=WaitForSingleObject(state->process,200);
        if(waitResult!=WAIT_OBJECT_0){
            if(waitResult==WAIT_FAILED)LOG("Audio: process wait failed winerr="<<GetLastError());
            DWORD code=STILL_ACTIVE;
            BOOL queried=FALSE;
            if(!m_settings.faults.failGetExitCodeProcess)
                queried=GetExitCodeProcess(state->process,&code);
            if(!queried)LOG("Audio: GetExitCodeProcess failed winerr="<<GetLastError());
            if(!queried||code==STILL_ACTIVE){
                if(!TerminateProcess(state->process,1))LOG("Audio: owned-process fallback termination failed winerr="<<GetLastError());
            }
            DWORD finalWait=WAIT_TIMEOUT;
            if(!m_settings.faults.failFinalProcessWait)
                finalWait=WaitForSingleObject(state->process,200);
            if(finalWait!=WAIT_OBJECT_0)LOG("Audio: owned child did not exit within final bound result="<<finalWait);
        }
    }
}

void AudioPlayer::ReaderThread(std::shared_ptr<ReaderState> state) noexcept
{
    try { ThreadMain(state); }
    catch (...) { LOG("Audio: reader thread stopped after an unexpected exception."); }
    // The child's exit code was never read and its stderr went to NUL, so a
    // failed decode produced no diagnostic at all - and because the position
    // it stopped advancing was still served as the master clock, the symptom
    // was frozen video rather than missing sound. A reader that ends while
    // nobody asked it to is worth a line whatever the cause, and one that
    // ends badly is worth what ffmpeg said about it.
    DrainAvailable(state->stderrPipe, state->stderrTail);
    if (!state->stop && state->process) {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(state->process, &exitCode))
            LOG("Audio: ffmpeg exit code unavailable winerr=" << GetLastError());
        else if (exitCode == STILL_ACTIVE)
            LOG("Audio: reader ended while ffmpeg was still running; there will be no sound from here.");
        else if (exitCode == DWORD(-22))
            // EINVAL, and on this command line it means ffmpeg mapped no audio
            // stream: `-map 0:a:0?` is optional, so a video-only source leaves
            // the output with nothing in it. Worth saying plainly - the old
            // wording reported a normal silent film as a failure, in a number.
            LOG("Audio: the source has no audio track (ffmpeg mapped no stream); playing silent.");
        else if (exitCode != 0)
            LOG("Audio: ffmpeg exited with code " << exitCode << "; there will be no sound from here.");
        if (audio_stderr::ReportOnExit(exitCode) && !state->stderrTail.Empty())
            LOG("Audio: ffmpeg said: " << state->stderrTail.Line());
    }
    if(state->completed&&!SetEvent(state->completed))LOG("Audio: SetEvent(reader completion) failed winerr="<<GetLastError());
}

void AudioPlayer::ThreadMain(const std::shared_ptr<ReaderState>& state) {
    // Event-driven: the endpoint signals when it wants more and says how much,
    // so this fills exactly the space available instead of pushing fixed 16 KB
    // buffers at it and sleeping. The old ring carried 682 ms of queue for no
    // benefit the clock could see - waveOutGetPosition reports samples PLAYED
    // either way - and the depth is now whatever the engine period is, which
    // the log records at Open.
    WasapiRenderer* const renderer = state->renderer.get();
    const uint32_t bytesPerFrame = renderer ? renderer->CurrentFormat().BytesPerFrame() : 4;
    const bool exclusive = renderer && renderer->Exclusive();

    // Whole frames only: the endpoint is handed frames, and the pipe delivers
    // arbitrary byte counts, so a partial frame has to be carried over.
    std::vector<std::byte> pending;
    std::vector<std::byte> chunk;

    // With no endpoint the pipe is still drained, so the process and job
    // teardown paths behave exactly as they do with one.
    constexpr DWORD kNoDeviceSliceBytes = 16384;

    while (!state->stop) {
        // Every round, paused or not, so a child writing errors can never
        // fill the pipe and stall on it.
        DrainAvailable(state->stderrPipe, state->stderrTail);
        uint32_t framesWanted = 0;
        if (renderer) {
            // A paused stream never signals, so the wait times out and the
            // loop comes back to check the stop flag.
            if (!renderer->WaitForSpace(20, framesWanted)) {
                if (renderer->DeviceLost()) { state->deviceLost = true; break; }
                LOG("Audio: the endpoint stopped accepting frames; there will be no sound from here.");
                break;
            }
            if (state->paused) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
            if (!framesWanted) continue;
        }

        const size_t wantedBytes = renderer ? size_t(framesWanted) * bytesPerFrame
                                            : size_t(kNoDeviceSliceBytes);
        chunk.assign(pending.begin(), pending.end());
        pending.clear();

        bool ended = false;
        while (!state->stop && chunk.size() < wantedBytes) {
            DWORD available = 0;
            if (!PeekNamedPipe(state->stdoutPipe, nullptr, 0, nullptr, &available, nullptr)) {
                const DWORD error = GetLastError();
                if (error != ERROR_BROKEN_PIPE) LOG("Audio: PeekNamedPipe failed winerr=" << error);
                ended = true; break;
            }
            if (!available) {
                if (state->process && WaitForSingleObject(state->process, 0) == WAIT_OBJECT_0) { ended = true; break; }
                // Nothing to render yet. Handing the endpoint silence here
                // would advance the clock over audio that has not arrived.
                break;
            }
            const size_t offset = chunk.size();
            const DWORD want = static_cast<DWORD>(std::min<size_t>(wantedBytes - offset, available));
            chunk.resize(offset + want);
            DWORD got = 0;
            if (!ReadFile(state->stdoutPipe, chunk.data() + offset, want, &got, nullptr)) {
                const DWORD error = GetLastError();
                if (error != ERROR_OPERATION_ABORTED && error != ERROR_BROKEN_PIPE)
                    LOG("Audio: ReadFile failed winerr=" << error);
                chunk.resize(offset);
                ended = true; break;
            }
            chunk.resize(offset + got);
            if (!got) { ended = true; break; }
        }

        if (state->stop) break;
        if (!chunk.empty()) state->hasAudioData = true;

        if (!renderer) {
            if (ended && chunk.empty()) break;
            if (chunk.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // Never more than the endpoint asked for: frames kept back from a
        // refused write can outnumber the space the next wait reports.
        const uint32_t wholeFrames = std::min(static_cast<uint32_t>(chunk.size() / bytesPerFrame), framesWanted);
        size_t consumedBytes = 0;
        // An exclusive stream plays its buffer again if an event goes
        // unanswered, so one with nothing to give still gets null data.
        if (!wholeFrames && exclusive && !ended) {
            if (renderer->WriteFiller() == WasapiRenderer::WriteResult::Failed) {
                if (renderer->DeviceLost()) { state->deviceLost = true; break; }
                LOG("Audio: writing to the endpoint failed; there will be no sound from here.");
                break;
            }
        }
        if (wholeFrames) {
            const auto written = renderer->Write(chunk.data(), wholeFrames);
            if (written == WasapiRenderer::WriteResult::Failed) {
                if (renderer->DeviceLost()) { state->deviceLost = true; break; }
                LOG("Audio: writing to the endpoint failed; there will be no sound from here.");
                break;
            }
            // Refused: a pause landed between the paused check above and the
            // write. The frames are kept for the resume rather than dropped.
            if (written == WasapiRenderer::WriteResult::Written) {
                consumedBytes = size_t(wholeFrames) * bytesPerFrame;
                if (!state->paused) ++state->submittedBuffers;
            }
        }
        // What was not taken - the tail of a partial frame, or frames the
        // endpoint refused or had no room for - waits for the next round
        // rather than being rendered as a fraction of a sample or lost.
        pending.assign(chunk.begin() + static_cast<ptrdiff_t>(consumedBytes), chunk.end());

        // Nothing whole left to write after the end. A trailing partial frame
        // can never be completed, so it must not keep the loop alive.
        if (ended && pending.size() < bytesPerFrame) break;
        if (!consumedBytes) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // On natural EOF the queued frames are left to play out rather than reset.
    // Resetting here would snap the played-frame count to zero and make the
    // audio-master clock jump backwards during the last video frames.
    // Stop() and Seek() perform their own reset, so only cancellation
    // discards queued audio.
    if (!state->stop && renderer && !state->deviceLost && exclusive) {
        // An exclusive device keeps playing its buffer whether or not it is
        // new: left running, it would repeat the last bursts until Stop. Two
        // periods of null data let the last of the film out, then it stops,
        // and the clock stands at the end of the film as it does in PCM.
        for (int period = 0; period < 3 && !state->stop; ++period) {
            uint32_t wanted = 0;
            if (!renderer->WaitForSpace(50, wanted)) break;
            if (wanted && renderer->WriteFiller() == WasapiRenderer::WriteResult::Failed) break;
        }
        if (!state->stop) renderer->Stop();
    } else if (!state->stop && renderer && !state->deviceLost) {
        uint64_t played = 0, previous = ~uint64_t{0};
        // Bounded: at most the endpoint buffer plus slack, polled until the
        // count stops moving.
        for (int idle = 0; idle < 200 && !state->stop; ++idle) {
            if (!renderer->PlayedFrames(played)) break;
            if (played == previous) break;
            previous = played;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

double AudioPlayer::PositionSeconds() const {
    const auto state=m_reader;
    if (!state || !state->renderer || !state->sampleRate || !state->hasAudioData.load()) return -1.0;
    uint64_t played = 0;
    if (!state->renderer->PlayedFrames(played)) return -1.0;
    const double position = m_seekBaseSec + double(played) / double(state->sampleRate);
    // hasAudioData is set once and cleared only by Stop(), so a reader thread
    // that has ended - pipe EOF, a dead ffmpeg child, a write failing after a
    // device change - leaves the queued frames to drain and this
    // position frozen. Returning it anyway stopped video for the rest of the
    // file, because the presentation gate holds every frame until the clock
    // reaches its due time. A stalled clock is carried forward at wall-clock
    // rate instead, and slewed back to the audio when it moves again; see
    // audio_clock::Present.
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(m_clockMutex);
    const bool playing = !state->paused.load();
    const bool usable = audio_clock::Usable(m_clock, position, now, playing);
    const double presented = audio_clock::Present(m_continuity, position, usable, now, playing);
    // What a restart resumes from: where the viewer is, which during a stall
    // is the carried clock rather than the frozen audio.
    m_lastKnownPosition.store(presented);
    if (usable && m_clockStalled) {
        m_clockStalled = false;
        LOG("Audio: clock advancing again at " << position << "s; slewing the master clock back from "
            << presented << "s.");
    } else if (!usable && !m_clockStalled) {
        m_clockStalled = true;
        LOG("Audio: clock frozen at " << position << "s for over " << audio_clock::kStallSeconds
            << "s of playback; carrying it forward on the steady clock. The helper has most likely "
               "ended, the stream has underrun, or the output device has gone away.");
    }
    return presented;
}

uint64_t AudioPlayer::SubmittedBuffers() const
{
    const auto state=m_reader;
    return state?state->submittedBuffers.load():0;
}

bool AudioPlayer::Active() const {const auto state=m_reader;return state&&state->renderer!=nullptr;}
bool AudioPlayer::Paused() const {const auto state=m_reader;return state&&state->paused.load();}

void AudioPlayer::Pause(bool paused) {
    const auto state=m_reader;if(!state)return;
    {
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lock(m_clockMutex);
        audio_clock::PauseChanged(m_clock, m_continuity, now);
    }
    state->paused = paused;
    if (!state->renderer) return;
    // Pausing decays the last frame to silence and lets the queue play out
    // before the clock stops, so the endpoint is never cut mid-waveform.
    // Nothing is discarded - the frames drain instead of being thrown away -
    // so the position ends up at the end of what was queued rather than in the
    // middle of it, and resuming opens with a matching ramp.
    if(!(paused?state->renderer->FadeOutAndStop():state->renderer->Start()))
        LOG("Audio: pause/resume was refused by the endpoint.");
}

void AudioPlayer::SetVolume(float volume01) {
    m_volume = std::clamp(volume01, 0.0f, 1.0f);
    const auto state=m_reader;if(!state||!state->renderer)return;
    state->renderer->SetVolume(m_volume);
}

bool AudioPlayer::DeliverDefaultEndpointChange(const std::wstring& newDeviceId) {
    const auto state = m_reader;
    if (!state || !state->renderer) return false;
    state->renderer->OnDefaultEndpointChanged(eRender, eConsole, newDeviceId.c_str());
    return true;
}

void AudioPlayer::AwaitEndpoint(bool noEndpoint) {
    m_endpointArrival = std::make_unique<RenderEndpointArrival>();
    if (m_endpointArrival->Watch(noEndpoint))
        LOG("Audio: waiting for a render endpoint to appear; sound resumes when one does.");
}

bool AudioPlayer::ServiceDeviceChanges(double playerPositionSeconds, bool playerPlaying) {
    const auto state = m_reader;
    if (!state) {
        // Nothing to restart from but the player's own position: the clock
        // went with the endpoint.
        if (!m_endpointArrival || playerPositionSeconds < 0.0 || m_path.empty()) return false;
        if (!m_endpointArrival->Arrived()) return false;
        LOG("Audio: a render endpoint appeared; starting audio at " << playerPositionSeconds << " s.");
        // A failed start watches again, so a notification that came before
        // the endpoint was ready is followed by the one that says it is.
        return Start(m_path, playerPositionSeconds,
                     playerPlaying ? AudioStartState::Playing : AudioStartState::Paused);
    }
    // A display-mode change has retrained the HDMI link under an exclusive
    // bitstream, and the receiver is no longer locked to it; see
    // audio_passthrough::NoteDisplayChange. Every call still succeeds, so
    // nothing below would notice.
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (audio_passthrough::ReopenDue(m_displayReopen, now) &&
        m_passthroughStatus.state == audio_passthrough::State::Active) {
        const double resumeAt = m_lastKnownPosition.load();
        LOG("Audio: the display changed mode; reopening the passthrough stream at " << resumeAt
            << " s so the receiver locks onto it again.");
        if (Seek(resumeAt)) return true;
        LOG("Audio: could not reopen after the display change; playback continues without sound.");
        return false;
    }
    // Two sources, and both are needed. The reader latches a loss it ran
    // into - a call to the endpoint that failed. The renderer latches one the
    // OS reported through the endpoint notifications, and those arrive while
    // every call is still succeeding: a default-device change leaves the old
    // endpoint working perfectly, so the reader would never see it.
    //
    // exchange on the reader's latch, so two callers in the same frame cannot
    // both restart. The renderer's own latch goes away with the renderer when
    // the restart replaces it.
    const bool readerSawLoss = state->deviceLost.exchange(false);
    const bool endpointReportedLoss = state->renderer && state->renderer->DeviceLost();
    if (!readerSawLoss && !endpointReportedLoss) return false;
    const double resumeAt = m_lastKnownPosition.load();
    LOG("Audio: the render endpoint went away; restarting on the current default at "
        << resumeAt << " s.");
    // A full restart rather than splicing into the new device: its mix format
    // may differ from the old one, and ffmpeg is producing the old format.
    if (Seek(resumeAt)) return true;
    LOG("Audio: could not restart on the new endpoint; playback continues without sound.");
    return false;
}

bool AudioPlayer::Seek(double seconds) {
    if (m_path.empty()) return false;
    const bool wasPaused = Paused();
    const float vol = m_volume;
    std::wstring path = m_path;
    Stop();
    m_volume = vol;
    return Start(path,std::max(0.0,seconds),wasPaused?AudioStartState::Paused:AudioStartState::Playing);
}

void AudioPlayer::Stop() {
    const auto state=m_reader;
    m_endpointArrival.reset();
    { std::lock_guard<std::mutex> lock(m_clockMutex); audio_clock::Reset(m_clock); audio_clock::Reset(m_continuity); m_clockStalled = false; }
    if(!state){if(m_thread.joinable())m_thread.detach();return;}
    state->stop = true;
    state->hasAudioData = false;
    state->paused = false;

    // Ramp the endpoint down to silence before stopping it, then stop the
    // owned writer/process tree. A stop is a seek's first half as well as a
    // stop, so cutting here is the click a viewer hears on every seek.
    // The renderer is destroyed with the reader state, so the queue does not
    // need discarding afterwards - it has already been played out.
    // ReaderState keeps every handle alive if a failed wait forces a detach;
    // the availability-driven reader then retires and closes them.
    if (state->renderer && !state->renderer->FadeOutAndStop())
        LOG("Audio: the endpoint refused a ramped stop.");
    StopProcess(state);
    if (m_thread.joinable()) {
        HANDLE readerThread=reinterpret_cast<HANDLE>(m_thread.native_handle());
        if(!CancelSynchronousIo(readerThread)){const DWORD error=GetLastError();if(error!=ERROR_NOT_FOUND)LOG("Audio: CancelSynchronousIo failed winerr="<<error);}
        DWORD readerWait=WAIT_TIMEOUT;
        if(!m_settings.faults.failInitialReaderWait)
            readerWait=WaitForSingleObject(state->completed,200);
        if(readerWait!=WAIT_OBJECT_0){
            LOG("Audio: reader required final bounded cancellation result="<<readerWait);StopProcess(state);
            if(!CancelSynchronousIo(readerThread)){const DWORD error=GetLastError();if(error!=ERROR_NOT_FOUND)LOG("Audio: final CancelSynchronousIo failed winerr="<<error);}
            readerWait=WAIT_TIMEOUT;
            if(!m_settings.faults.failFinalReaderWait)
                readerWait=WaitForSingleObject(state->completed,200);
        }
        if(readerWait==WAIT_OBJECT_0)m_thread.join();
        else{LOG("Audio: retiring reader state after bounded wait failure.");m_thread.detach();}
    }
    m_reader.reset();
}
