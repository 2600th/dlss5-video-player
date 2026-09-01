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
    m_hasAudioData = false;
    m_submittedBuffers = 0;
    m_paused = state == AudioStartState::Paused;
    m_path = videoPath;
    m_ffmpeg = FindFFmpeg();
    if (m_ffmpeg.empty()) { LOG("Audio: ffmpeg.exe not found."); return false; }

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 48000;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    if(!m_disableWaveOut){
        if (waveOutOpen(&m_waveOut, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            m_waveOut = nullptr;LOG("Audio: waveOutOpen failed.");return false;
        }
        SetVolume(m_volume);
        if(m_paused&&waveOutPause(m_waveOut)!=MMSYSERR_NOERROR){waveOutClose(m_waveOut);m_waveOut=nullptr;LOG("Audio: initial pause failed.");return false;}
    }
    if (!StartProcess(seekSeconds)) { if(m_waveOut)waveOutClose(m_waveOut);m_waveOut = nullptr; return false; }
    m_stop = false;
    try{m_thread = std::thread(&AudioPlayer::ThreadMain, this);}catch(...){StopProcess();if(m_stdout){CloseHandle(m_stdout);m_stdout=nullptr;}if(m_process){CloseHandle(m_process);m_process=nullptr;}if(m_job){CloseHandle(m_job);m_job=nullptr;}if(m_waveOut){waveOutClose(m_waveOut);m_waveOut=nullptr;}throw;}
    return true;
}

bool AudioPlayer::StartProcess(double seekSeconds) {
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
    CloseHandle(pi.hThread); m_process = pi.hProcess; m_stdout = readPipe;m_job=job;
    LOG("Audio: FFmpeg PCM/WaveOut path started at " << seekSeconds << " s.");
    return true;
}

void AudioPlayer::StopProcess() {
    // Closing a kill-on-close job is the fallback when the explicit job
    // termination API fails. The process handle remains owned until the reader
    // has exited, so no handle used by ThreadMain is closed concurrently.
    if(m_job){
        BOOL terminated=FALSE;
#ifdef AUDIO_PLAYER_TESTING
        if(!m_failTerminateJob)
#endif
            terminated=TerminateJobObject(m_job,0);
        if(!terminated)LOG("Audio: job termination fallback engaged winerr="<<GetLastError());
        if(!CloseHandle(m_job))LOG("Audio: CloseHandle(job) failed winerr="<<GetLastError());
        m_job=nullptr;
    }
    if(m_process){
        DWORD waitResult=WAIT_TIMEOUT;
#ifdef AUDIO_PLAYER_TESTING
        if(!m_forceProcessWaitTimeout)
#endif
            waitResult=WaitForSingleObject(m_process,500);
        if(waitResult!=WAIT_OBJECT_0){
            if(waitResult==WAIT_FAILED)LOG("Audio: process wait failed winerr="<<GetLastError());
            DWORD code=STILL_ACTIVE;
            const BOOL queried=GetExitCodeProcess(m_process,&code);
            if(!queried)LOG("Audio: GetExitCodeProcess failed winerr="<<GetLastError());
            if(!queried||code==STILL_ACTIVE){
                if(!TerminateProcess(m_process,1))LOG("Audio: owned-process fallback termination failed winerr="<<GetLastError());
            }
            const DWORD finalWait=WaitForSingleObject(m_process,500);
            if(finalWait!=WAIT_OBJECT_0)LOG("Audio: owned child did not exit within final bound result="<<finalWait);
        }
    }
}

void AudioPlayer::ThreadMain() {
    constexpr size_t BufferCount = 8;
    constexpr size_t BytesPerBuffer = 16384; // ~85 ms stereo/48k/16-bit
    struct Slot { std::vector<char> bytes; WAVEHDR hdr{}; bool prepared=false; };
    Slot slots[BufferCount];
    for (auto& s : slots) { s.bytes.resize(BytesPerBuffer); s.hdr.lpData = s.bytes.data(); s.hdr.dwBufferLength = 0; }
    size_t index = 0;

    while (!m_stop) {
        Slot& s = slots[index];
        if (s.prepared) {
            while (!m_stop && !(s.hdr.dwFlags & WHDR_DONE)) Sleep(2);
            if (m_stop) break;
            { std::lock_guard<std::mutex> lock(m_waveMutex);const MMRESULT unprepared=waveOutUnprepareHeader(m_waveOut,&s.hdr,sizeof(s.hdr));if(unprepared!=MMSYSERR_NOERROR)LOG("Audio: unprepare failed result="<<unprepared); }
            s.prepared = false; s.hdr = {}; s.hdr.lpData = s.bytes.data();
        }

        size_t total = 0;
        while (!m_stop && total < BytesPerBuffer) {
            DWORD available=0;
            if(!PeekNamedPipe(m_stdout,nullptr,0,nullptr,&available,nullptr)){
                const DWORD error=GetLastError();if(error!=ERROR_BROKEN_PIPE)LOG("Audio: PeekNamedPipe failed winerr="<<error);break;
            }
            if(available==0){
                if(m_process&&WaitForSingleObject(m_process,0)==WAIT_OBJECT_0)break;
                Sleep(2);continue;
            }
            const DWORD want=static_cast<DWORD>(std::min<size_t>(BytesPerBuffer-total,available));DWORD got=0;
            if(!ReadFile(m_stdout,s.bytes.data()+total,want,&got,nullptr)){const DWORD error=GetLastError();if(error!=ERROR_OPERATION_ABORTED&&error!=ERROR_BROKEN_PIPE)LOG("Audio: ReadFile failed winerr="<<error);break;}
            if(got==0)break;
            total += got;
        }
        if (m_stop || total == 0) break;
        m_hasAudioData = true;
        if(m_disableWaveOut)continue;
        s.hdr.dwBufferLength = DWORD(total);
        {
            std::lock_guard<std::mutex> lock(m_waveMutex);
            // Re-check under the same lock used by Stop() before submitting audio.
            // Once Stop() has set m_stop and reset WaveOut, no late buffer can be queued.
            if (m_stop) break;
            if (waveOutPrepareHeader(m_waveOut, &s.hdr, sizeof(s.hdr)) != MMSYSERR_NOERROR) break;
            s.prepared = true;
            if (waveOutWrite(m_waveOut, &s.hdr, sizeof(s.hdr)) != MMSYSERR_NOERROR) break;
            if(!m_paused)++m_submittedBuffers;
        }
        index = (index + 1) % BufferCount;
    }

    // On natural EOF, drain the queued WaveOut buffers instead of calling waveOutReset().
    // Resetting here would snap TIME_SAMPLES back to zero and make the audio-master clock
    // jump backwards during the last video frames.  Stop()/Seek() already perform an
    // explicit reset, so only the cancellation path should discard queued audio.
    if (!m_stop && m_waveOut) {
        for (auto& s : slots) {
            if (!s.prepared) continue;
            while (!m_stop && !(s.hdr.dwFlags & WHDR_DONE)) Sleep(2);
        }
    }
    for (auto& s : slots) {
        if (s.prepared) {
            { std::lock_guard<std::mutex> lock(m_waveMutex);const MMRESULT unprepared=waveOutUnprepareHeader(m_waveOut,&s.hdr,sizeof(s.hdr));if(unprepared!=MMSYSERR_NOERROR)LOG("Audio: final unprepare failed result="<<unprepared); }
            s.prepared = false;
        }
    }
}

double AudioPlayer::PositionSeconds() const {
    if (!m_waveOut || !m_hasAudioData.load()) return -1.0;
    MMTIME mt{}; mt.wType = TIME_SAMPLES;
    { std::lock_guard<std::mutex> lock(m_waveMutex);
      if (waveOutGetPosition(m_waveOut, &mt, sizeof(mt)) != MMSYSERR_NOERROR || mt.wType != TIME_SAMPLES)
          return -1.0; }
    return m_seekBaseSec + double(mt.u.sample) / 48000.0;
}

void AudioPlayer::Pause(bool paused) {
    m_paused = paused;
    if (!m_waveOut) return;
    std::lock_guard<std::mutex> lock(m_waveMutex);
    const MMRESULT result=paused?waveOutPause(m_waveOut):waveOutRestart(m_waveOut);
    if(result!=MMSYSERR_NOERROR)LOG("Audio: pause/restart failed result="<<result);
}

void AudioPlayer::SetVolume(float volume01) {
    m_volume = std::clamp(volume01, 0.0f, 1.0f);
    if (!m_waveOut) return;
    DWORD v = DWORD(m_volume * 65535.0f + 0.5f);
    std::lock_guard<std::mutex> lock(m_waveMutex);
    waveOutSetVolume(m_waveOut, MAKELONG(v, v));
}

bool AudioPlayer::Seek(double seconds) {
    if (m_path.empty()) return false;
    const bool wasPaused = m_paused.load();
    const float vol = m_volume;
    std::wstring path = m_path;
    Stop();
    m_volume = vol;
    return Start(path,std::max(0.0,seconds),wasPaused?AudioStartState::Paused:AudioStartState::Playing);
}

void AudioPlayer::Stop() {
    m_stop = true;
    m_hasAudioData = false;
    m_paused = false;

    // First release queued WaveOut buffers, then stop FFmpeg so a worker blocked
    // in ReadFile sees EOF. Keep BOTH m_stdout and m_waveOut valid until the
    // worker exits: it still has to unprepare any WAVEHDRs it owns.
    if (m_waveOut) { std::lock_guard<std::mutex> lock(m_waveMutex);const MMRESULT reset=waveOutReset(m_waveOut);if(reset!=MMSYSERR_NOERROR)LOG("Audio: waveOutReset failed result="<<reset); }
    StopProcess();
    if (m_thread.joinable()) {
        HANDLE readerThread=reinterpret_cast<HANDLE>(m_thread.native_handle());
        if(!CancelSynchronousIo(readerThread)){const DWORD error=GetLastError();if(error!=ERROR_NOT_FOUND)LOG("Audio: CancelSynchronousIo failed winerr="<<error);}
        DWORD readerWait=WaitForSingleObject(readerThread,500);
        if(readerWait!=WAIT_OBJECT_0){
            LOG("Audio: reader required final bounded cancellation result="<<readerWait);StopProcess();
            if(!CancelSynchronousIo(readerThread)){const DWORD error=GetLastError();if(error!=ERROR_NOT_FOUND)LOG("Audio: final CancelSynchronousIo failed winerr="<<error);}
            readerWait=WaitForSingleObject(readerThread,500);
        }
        if(readerWait!=WAIT_OBJECT_0){LOG("Audio: reader violated bounded-exit invariant; terminating to avoid an unbounded join.");std::terminate();}
        m_thread.join();
    }

    if (m_stdout) { if(!CloseHandle(m_stdout))LOG("Audio: CloseHandle(stdout) failed winerr="<<GetLastError());m_stdout = nullptr; }
    if (m_process) { if(!CloseHandle(m_process))LOG("Audio: CloseHandle(process) failed winerr="<<GetLastError());m_process = nullptr; }
    if (m_job) { if(!CloseHandle(m_job))LOG("Audio: CloseHandle(job) failed winerr="<<GetLastError());m_job = nullptr; }
    if (m_waveOut) {const MMRESULT closed=waveOutClose(m_waveOut);if(closed!=MMSYSERR_NOERROR)LOG("Audio: waveOutClose failed result="<<closed);m_waveOut = nullptr; }
    m_stop = false;
}
