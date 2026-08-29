#include "AudioPlayer.h"
#include "Log.h"
#include <filesystem>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <iterator>

namespace fs = std::filesystem;
static std::wstring Q(const std::wstring& s) { return L"\"" + s + L"\""; }

AudioPlayer::~AudioPlayer() { Stop(); }

std::wstring AudioPlayer::FindFFmpeg() {
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

bool AudioPlayer::Start(const std::wstring& videoPath, double seekSeconds) {
    Stop();
    m_seekBaseSec = std::max(0.0, seekSeconds);
    m_hasAudioData = false;
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
    if (waveOutOpen(&m_waveOut, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        m_waveOut = nullptr;
        LOG("Audio: waveOutOpen failed.");
        return false;
    }
    SetVolume(m_volume);
    if (!StartProcess(seekSeconds)) { waveOutClose(m_waveOut); m_waveOut = nullptr; return false; }
    m_stop = false;
    m_thread = std::thread(&AudioPlayer::ThreadMain, this);
    return true;
}

bool AudioPlayer::StartProcess(double seekSeconds) {
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 1024 * 1024)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
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
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(m_ffmpeg.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe); if (nul) CloseHandle(nul);
    if (!ok) { CloseHandle(readPipe); LOG("Audio: CreateProcess(ffmpeg) failed winerr=" << GetLastError()); return false; }
    CloseHandle(pi.hThread); m_process = pi.hProcess; m_stdout = readPipe;
    LOG("Audio: FFmpeg PCM/WaveOut path started at " << seekSeconds << " s.");
    return true;
}

void AudioPlayer::StopProcess() {
    // IMPORTANT: do not close m_stdout here. ThreadMain may be blocked in a
    // synchronous ReadFile on that handle. Terminating FFmpeg closes the pipe's
    // write end and wakes ReadFile naturally; the read handle is closed only
    // after the worker thread has joined in Stop(). This removes a use-after-
    // close race that was particularly easy to trigger while seeking.
    if (m_process) {
        DWORD code = 0;
        if (GetExitCodeProcess(m_process, &code) && code == STILL_ACTIVE) {
            TerminateProcess(m_process, 0);
            WaitForSingleObject(m_process, 1000);
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
            { std::lock_guard<std::mutex> lock(m_waveMutex); waveOutUnprepareHeader(m_waveOut, &s.hdr, sizeof(s.hdr)); }
            s.prepared = false; s.hdr = {}; s.hdr.lpData = s.bytes.data();
        }

        size_t total = 0;
        while (!m_stop && total < BytesPerBuffer) {
            DWORD got = 0;
            if (!ReadFile(m_stdout, s.bytes.data() + total, DWORD(BytesPerBuffer - total), &got, nullptr) || got == 0) break;
            total += got;
        }
        if (m_stop || total == 0) break;
        m_hasAudioData = true;
        s.hdr.dwBufferLength = DWORD(total);
        {
            std::lock_guard<std::mutex> lock(m_waveMutex);
            // Re-check under the same lock used by Stop() before submitting audio.
            // Once Stop() has set m_stop and reset WaveOut, no late buffer can be queued.
            if (m_stop) break;
            if (waveOutPrepareHeader(m_waveOut, &s.hdr, sizeof(s.hdr)) != MMSYSERR_NOERROR) break;
            s.prepared = true;
            if (waveOutWrite(m_waveOut, &s.hdr, sizeof(s.hdr)) != MMSYSERR_NOERROR) break;
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
            while (!(s.hdr.dwFlags & WHDR_DONE)) Sleep(2);
        }
    }
    for (auto& s : slots) {
        if (s.prepared) {
            { std::lock_guard<std::mutex> lock(m_waveMutex); waveOutUnprepareHeader(m_waveOut, &s.hdr, sizeof(s.hdr)); }
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
    if (paused) waveOutPause(m_waveOut); else waveOutRestart(m_waveOut);
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
    bool ok = Start(path, std::max(0.0, seconds));
    if (ok && wasPaused) Pause(true);
    return ok;
}

void AudioPlayer::Stop() {
    m_stop = true;
    m_hasAudioData = false;
    m_paused = false;

    // First release queued WaveOut buffers, then stop FFmpeg so a worker blocked
    // in ReadFile sees EOF. Keep BOTH m_stdout and m_waveOut valid until the
    // worker exits: it still has to unprepare any WAVEHDRs it owns.
    if (m_waveOut) { std::lock_guard<std::mutex> lock(m_waveMutex); waveOutReset(m_waveOut); }
    StopProcess();
    if (m_thread.joinable()) m_thread.join();

    if (m_stdout) { CloseHandle(m_stdout); m_stdout = nullptr; }
    if (m_process) { CloseHandle(m_process); m_process = nullptr; }
    if (m_waveOut) { waveOutClose(m_waveOut); m_waveOut = nullptr; }
    m_stop = false;
}
