#include "AudioPlayer.h"
#include "Log.h"
#include <filesystem>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <iterator>
#include <chrono>

namespace fs = std::filesystem;
static std::wstring Q(const std::wstring& s) { return L"\"" + s + L"\""; }

AudioPlayer::~AudioPlayer() { Stop(); }

AudioPlayer::ReaderState::~ReaderState()
{
    if (stdoutPipe && !CloseHandle(stdoutPipe)) LOG("Audio: CloseHandle(stdout) failed winerr=" << GetLastError());
    if (process && !CloseHandle(process)) LOG("Audio: CloseHandle(process) failed winerr=" << GetLastError());
    if (job && !CloseHandle(job)) LOG("Audio: CloseHandle(job) failed winerr=" << GetLastError());
    if (waveOut) {
        const MMRESULT closed = waveOutClose(waveOut);
        if (closed != MMSYSERR_NOERROR) LOG("Audio: waveOutClose failed result=" << closed);
    }
    if (completed && !CloseHandle(completed)) LOG("Audio: CloseHandle(completed) failed winerr=" << GetLastError());
}

std::wstring AudioPlayer::FindFFmpeg() const {
#ifdef AUDIO_PLAYER_TESTING
    if(!m_helperDirectory.empty()){
        const fs::path candidate=fs::path(m_helperDirectory)/L"ffmpeg.exe";std::error_code error;
        return fs::is_regular_file(candidate,error)?candidate.wstring():std::wstring{};
    }
#endif
    wchar_t modulePath[32768]{};
    if (GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)))) {
        fs::path base = fs::path(modulePath).parent_path();
        const fs::path cands[] = { base / L"ffmpeg.exe", base / L"ffmpeg" / L"bin" / L"ffmpeg.exe",
                                   base.parent_path() / L"ffmpeg" / L"bin" / L"ffmpeg.exe" };
        for (const auto& p : cands) { std::error_code ec; if (fs::is_regular_file(p, ec)) return p.wstring(); }
    }
    wchar_t found[32768]{};
    DWORD n = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(std::size(found)), found, nullptr);
    return (n && n < std::size(found)) ? std::wstring(found) : std::wstring();
}

bool AudioPlayer::Start(const std::wstring& videoPath, double seekSeconds, AudioStartState state) {
    Stop();
    m_seekBaseSec = std::max(0.0, seekSeconds);
    m_path = videoPath;
    m_ffmpeg = FindFFmpeg();
    if (m_ffmpeg.empty()) { LOG("Audio: ffmpeg.exe not found."); return false; }

    auto reader=std::make_shared<ReaderState>();
    reader->disableWaveOut=m_disableWaveOut;
    reader->paused=state==AudioStartState::Paused;
    reader->completed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!reader->completed){LOG("Audio: CreateEvent(reader completion) failed winerr="<<GetLastError());return false;}

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 48000;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    if(!reader->disableWaveOut){
        if (waveOutOpen(&reader->waveOut, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            reader->waveOut = nullptr;LOG("Audio: waveOutOpen failed.");return false;
        }
        DWORD volume=DWORD(m_volume*65535.0f+0.5f);
        if(waveOutSetVolume(reader->waveOut,MAKELONG(volume,volume))!=MMSYSERR_NOERROR)LOG("Audio: initial volume failed.");
        if(reader->paused&&waveOutPause(reader->waveOut)!=MMSYSERR_NOERROR){LOG("Audio: initial pause failed.");return false;}
    }
    if (!StartProcess(seekSeconds,reader)) return false;
    m_reader=reader;
    try{m_thread=std::thread(&AudioPlayer::ReaderThread,reader);}catch(...){StopProcess(reader);m_reader.reset();throw;}
    return true;
}

bool AudioPlayer::StartProcess(double seekSeconds,const std::shared_ptr<ReaderState>& state) {
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 1024 * 1024)) return false;
    if(!SetHandleInformation(readPipe,HANDLE_FLAG_INHERIT,0)){CloseHandle(readPipe);CloseHandle(writePipe);return false;}
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nul == INVALID_HANDLE_VALUE) nul = nullptr;

    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul; si.hStdOutput = writePipe; si.hStdError = nul;
    std::wostringstream args;
    args << L"-hide_banner -loglevel error -nostdin ";
    if (seekSeconds > 0.0) args << L"-ss " << std::fixed << std::setprecision(6) << seekSeconds << L" ";
    args << L"-i " << Q(m_path)
         << L" -map 0:a:0? -vn -sn -dn -ac 2 -ar 48000 -c:a pcm_s16le -f s16le pipe:1";
    std::wstring cmd = Q(m_ffmpeg) + L" " + args.str();
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end()); mutableCmd.push_back(L'\0');
    HANDLE job=CreateJobObjectW(nullptr,nullptr);if(job){JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){CloseHandle(job);job=nullptr;}}
    PROCESS_INFORMATION pi{};
    BOOL ok = job&&CreateProcessW(m_ffmpeg.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW|CREATE_SUSPENDED,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe); if (nul) CloseHandle(nul);
    if (!ok) { CloseHandle(readPipe);if(job)CloseHandle(job); LOG("Audio: CreateProcess(ffmpeg) failed winerr=" << GetLastError()); return false; }
    if(!AssignProcessToJobObject(job,pi.hProcess)){if(!TerminateProcess(pi.hProcess,1))LOG("Audio: failed to terminate unassigned child winerr="<<GetLastError());const DWORD waited=WaitForSingleObject(pi.hProcess,500);if(waited!=WAIT_OBJECT_0)LOG("Audio: unassigned child did not exit within bound result="<<waited);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(readPipe);CloseHandle(job);return false;}
    if(ResumeThread(pi.hThread)==DWORD(-1)){LOG("Audio: ResumeThread failed winerr="<<GetLastError());if(!TerminateJobObject(job,1))LOG("Audio: failed to terminate suspended job winerr="<<GetLastError());WaitForSingleObject(pi.hProcess,500);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(readPipe);CloseHandle(job);return false;}
    CloseHandle(pi.hThread);state->process=pi.hProcess;state->stdoutPipe=readPipe;state->job=job;
    LOG("Audio: FFmpeg PCM/WaveOut path started at " << seekSeconds << " s.");
    return true;
}

void AudioPlayer::StopProcess(const std::shared_ptr<ReaderState>& state) {
    // Closing a kill-on-close job is the fallback when the explicit job
    // termination API fails. The process handle remains owned until the reader
    // has exited, so no handle used by ThreadMain is closed concurrently.
    if(state->job){
        BOOL terminated=FALSE;
#ifdef AUDIO_PLAYER_TESTING
        if(!m_failTerminateJob)
#endif
            terminated=TerminateJobObject(state->job,0);
        if(!terminated)LOG("Audio: job termination fallback engaged winerr="<<GetLastError());
        if(!CloseHandle(state->job))LOG("Audio: CloseHandle(job) failed winerr="<<GetLastError());
        state->job=nullptr;
    }
    if(state->process){
        DWORD waitResult=WAIT_TIMEOUT;
#ifdef AUDIO_PLAYER_TESTING
        if(!m_failInitialProcessWait)
#endif
            waitResult=WaitForSingleObject(state->process,200);
        if(waitResult!=WAIT_OBJECT_0){
            if(waitResult==WAIT_FAILED)LOG("Audio: process wait failed winerr="<<GetLastError());
            DWORD code=STILL_ACTIVE;
            BOOL queried=FALSE;
#ifdef AUDIO_PLAYER_TESTING
            if(!m_failGetExitCodeProcess)
#endif
                queried=GetExitCodeProcess(state->process,&code);
            if(!queried)LOG("Audio: GetExitCodeProcess failed winerr="<<GetLastError());
            if(!queried||code==STILL_ACTIVE){
                if(!TerminateProcess(state->process,1))LOG("Audio: owned-process fallback termination failed winerr="<<GetLastError());
            }
            DWORD finalWait=WAIT_TIMEOUT;
#ifdef AUDIO_PLAYER_TESTING
            if(!m_failFinalProcessWait)
#endif
                finalWait=WaitForSingleObject(state->process,200);
            if(finalWait!=WAIT_OBJECT_0)LOG("Audio: owned child did not exit within final bound result="<<finalWait);
        }
    }
}

void AudioPlayer::ReaderThread(std::shared_ptr<ReaderState> state) noexcept
{
    try { ThreadMain(state); }
    catch (...) { LOG("Audio: reader thread stopped after an unexpected exception."); }
    if(state->completed&&!SetEvent(state->completed))LOG("Audio: SetEvent(reader completion) failed winerr="<<GetLastError());
}

void AudioPlayer::ThreadMain(const std::shared_ptr<ReaderState>& state) {
    constexpr size_t BufferCount = 8;
    constexpr size_t BytesPerBuffer = 16384; // ~85 ms stereo/48k/16-bit
    struct Slot { std::vector<char> bytes; WAVEHDR hdr{}; bool prepared=false; };
    Slot slots[BufferCount];
    for (auto& s : slots) { s.bytes.resize(BytesPerBuffer); s.hdr.lpData = s.bytes.data(); s.hdr.dwBufferLength = 0; }
    size_t index = 0;

    while (!state->stop) {
        Slot& s = slots[index];
        if (s.prepared) {
            while (!state->stop && !(s.hdr.dwFlags & WHDR_DONE)) Sleep(2);
            if (state->stop) break;
            { std::lock_guard<std::mutex> lock(state->waveMutex);const MMRESULT unprepared=waveOutUnprepareHeader(state->waveOut,&s.hdr,sizeof(s.hdr));if(unprepared!=MMSYSERR_NOERROR)LOG("Audio: unprepare failed result="<<unprepared); }
            s.prepared = false; s.hdr = {}; s.hdr.lpData = s.bytes.data();
        }

        size_t total = 0;
        while (!state->stop && total < BytesPerBuffer) {
            DWORD available=0;
            if(!PeekNamedPipe(state->stdoutPipe,nullptr,0,nullptr,&available,nullptr)){
                const DWORD error=GetLastError();if(error!=ERROR_BROKEN_PIPE)LOG("Audio: PeekNamedPipe failed winerr="<<error);break;
            }
            if(available==0){
                if(state->process&&WaitForSingleObject(state->process,0)==WAIT_OBJECT_0)break;
                Sleep(2);continue;
            }
            const DWORD want=static_cast<DWORD>(std::min<size_t>(BytesPerBuffer-total,available));DWORD got=0;
            if(!ReadFile(state->stdoutPipe,s.bytes.data()+total,want,&got,nullptr)){const DWORD error=GetLastError();if(error!=ERROR_OPERATION_ABORTED&&error!=ERROR_BROKEN_PIPE)LOG("Audio: ReadFile failed winerr="<<error);break;}
            if(got==0)break;
            total += got;
        }
        if (state->stop || total == 0) break;
        state->hasAudioData = true;
        if(state->disableWaveOut)continue;
        s.hdr.dwBufferLength = DWORD(total);
        {
            std::lock_guard<std::mutex> lock(state->waveMutex);
            // Re-check under the same lock used by Stop() before submitting audio.
            // Once Stop() has set m_stop and reset WaveOut, no late buffer can be queued.
            if (state->stop) break;
            if (waveOutPrepareHeader(state->waveOut, &s.hdr, sizeof(s.hdr)) != MMSYSERR_NOERROR) break;
            s.prepared = true;
            if (waveOutWrite(state->waveOut, &s.hdr, sizeof(s.hdr)) != MMSYSERR_NOERROR) break;
            if(!state->paused)++state->submittedBuffers;
        }
        index = (index + 1) % BufferCount;
    }

    // On natural EOF, drain the queued WaveOut buffers instead of calling waveOutReset().
    // Resetting here would snap TIME_SAMPLES back to zero and make the audio-master clock
    // jump backwards during the last video frames.  Stop()/Seek() already perform an
    // explicit reset, so only the cancellation path should discard queued audio.
    if (!state->stop && state->waveOut) {
        for (auto& s : slots) {
            if (!s.prepared) continue;
            while (!state->stop && !(s.hdr.dwFlags & WHDR_DONE)) Sleep(2);
        }
    }
    for (auto& s : slots) {
        if (s.prepared) {
            { std::lock_guard<std::mutex> lock(state->waveMutex);const MMRESULT unprepared=waveOutUnprepareHeader(state->waveOut,&s.hdr,sizeof(s.hdr));if(unprepared!=MMSYSERR_NOERROR)LOG("Audio: final unprepare failed result="<<unprepared); }
            s.prepared = false;
        }
    }
}

double AudioPlayer::PositionSeconds() const {
    const auto state=m_reader;
    if (!state || !state->waveOut || !state->hasAudioData.load()) return -1.0;
    MMTIME mt{}; mt.wType = TIME_SAMPLES;
    { std::lock_guard<std::mutex> lock(state->waveMutex);
      if (waveOutGetPosition(state->waveOut, &mt, sizeof(mt)) != MMSYSERR_NOERROR || mt.wType != TIME_SAMPLES)
          return -1.0; }
    return m_seekBaseSec + double(mt.u.sample) / 48000.0;
}

bool AudioPlayer::Active() const {const auto state=m_reader;return state&&state->waveOut;}
bool AudioPlayer::HasAudioData() const {const auto state=m_reader;return state&&state->hasAudioData.load();}
bool AudioPlayer::Paused() const {const auto state=m_reader;return state&&state->paused.load();}

void AudioPlayer::Pause(bool paused) {
    const auto state=m_reader;if(!state)return;
    state->paused = paused;
    if (!state->waveOut) return;
    std::lock_guard<std::mutex> lock(state->waveMutex);
    const MMRESULT result=paused?waveOutPause(state->waveOut):waveOutRestart(state->waveOut);
    if(result!=MMSYSERR_NOERROR)LOG("Audio: pause/restart failed result="<<result);
}

void AudioPlayer::SetVolume(float volume01) {
    m_volume = std::clamp(volume01, 0.0f, 1.0f);
    const auto state=m_reader;if(!state||!state->waveOut)return;
    DWORD v = DWORD(m_volume * 65535.0f + 0.5f);
    std::lock_guard<std::mutex> lock(state->waveMutex);
    if(waveOutSetVolume(state->waveOut,MAKELONG(v,v))!=MMSYSERR_NOERROR)LOG("Audio: set volume failed.");
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
    if(!state){if(m_thread.joinable())m_thread.detach();return;}
    state->stop = true;
    state->hasAudioData = false;
    state->paused = false;

    // Release queued WaveOut buffers, then stop the owned writer/process tree.
    // ReaderState keeps every handle and mutex alive if a failed wait forces a
    // detach; the availability-driven reader then retires and closes them.
    if (state->waveOut) { std::lock_guard<std::mutex> lock(state->waveMutex);const MMRESULT reset=waveOutReset(state->waveOut);if(reset!=MMSYSERR_NOERROR)LOG("Audio: waveOutReset failed result="<<reset); }
    StopProcess(state);
    if (m_thread.joinable()) {
        HANDLE readerThread=reinterpret_cast<HANDLE>(m_thread.native_handle());
        if(!CancelSynchronousIo(readerThread)){const DWORD error=GetLastError();if(error!=ERROR_NOT_FOUND)LOG("Audio: CancelSynchronousIo failed winerr="<<error);}
        DWORD readerWait=WAIT_TIMEOUT;
#ifdef AUDIO_PLAYER_TESTING
        if(!m_failInitialReaderWait)
#endif
            readerWait=WaitForSingleObject(state->completed,200);
        if(readerWait!=WAIT_OBJECT_0){
            LOG("Audio: reader required final bounded cancellation result="<<readerWait);StopProcess(state);
            if(!CancelSynchronousIo(readerThread)){const DWORD error=GetLastError();if(error!=ERROR_NOT_FOUND)LOG("Audio: final CancelSynchronousIo failed winerr="<<error);}
            readerWait=WAIT_TIMEOUT;
#ifdef AUDIO_PLAYER_TESTING
            if(!m_failFinalReaderWait)
#endif
                readerWait=WaitForSingleObject(state->completed,200);
        }
        if(readerWait==WAIT_OBJECT_0)m_thread.join();
        else{LOG("Audio: retiring reader state after bounded wait failure.");m_thread.detach();}
    }
    m_reader.reset();
}
