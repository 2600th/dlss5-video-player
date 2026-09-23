#pragma once
#include "PlatformPaths.h"

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <mutex>
#include <string>
#include <sstream>
#include <iomanip>
#include <filesystem>

class Log {
public:
    static Log& Get() { static Log l; return l; }

    void Write(const std::string& s) {
        // Formatted before the lock: the render thread and the decode, audio and
        // encoder threads all log, and the lock only has to cover the file.
        SYSTEMTIME st{}; GetLocalTime(&st);
        std::ostringstream formatted;
        formatted << '[' << std::setfill('0') << std::setw(2) << st.wHour << ':'
                  << std::setw(2) << st.wMinute << ':' << std::setw(2) << st.wSecond
                  << '.' << std::setw(3) << st.wMilliseconds << "] " << s << "\n";
        const std::string line = WithCrLf(formatted.str());
        // OutputDebugStringA takes the system-wide DBWinMutex, so with no
        // debugger attached every log line serialized this process against
        // every other one on the machine - and in a degraded session the
        // renderer logs its reset reason per frame.
        if (m_debugger) OutputDebugStringA(line.c_str());
        std::lock_guard<std::mutex> lock(m_mutex);
        // One unbuffered WriteFile per line and no flush. The line is in the
        // kernel's cache when this returns, which is what the old per-line
        // flush bought: it survives this process crashing or hanging. A flush
        // on a timer was the alternative, and it would lose exactly the lines
        // written just before a crash - the crash handler's own among them.
        Append(line);
        m_written += line.size();
        if (m_written >= kMaxBytes) Roll();
    }

    // Where the log actually ended up, so a diagnostics action can point at it
    // and a crash dump can be written beside it.
    static const std::filesystem::path& Path() { return Get().m_path; }

private:
    // A session is appended, so the file has to be bounded. Rolling keeps one
    // previous file: a crash report needs the run that crashed, and often the
    // one before it.
    static constexpr std::uintmax_t kMaxBytes = 8u * 1024u * 1024u;

    static std::filesystem::path ModuleDirectory() {
        return platform_paths::ModuleDirectory().value_or(std::filesystem::path{});
    }

    // %LOCALAPPDATA%\DLSSVideoPlayer, the same fallback the neural cache uses
    // when the install directory cannot be written. Without it, an install
    // under Program Files kept the cache working and lost the log silently -
    // which is exactly the configuration TROUBLESHOOTING.md warns about, and
    // the one whose users most need a log to send.
    static std::filesystem::path FallbackDirectory() {
        PWSTR raw = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) || !raw) {
            if (raw) CoTaskMemFree(raw);
            return {};
        }
        std::filesystem::path directory(raw);
        CoTaskMemFree(raw);
        directory /= L"DLSSVideoPlayer";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return error ? std::filesystem::path{} : directory;
    }

    // The helper ships into neural-runtime/ beside the player, so it writes its
    // own file rather than truncating the player's.
    static std::wstring FileName() {
        const auto path = platform_paths::ModulePath();
        if (!path) return L"DLSSVideoPlayer.log";
        const std::wstring stem = path->stem().wstring();
        return stem.empty() ? std::wstring(L"DLSSVideoPlayer.log") : stem + L".log";
    }

    // FILE_APPEND_DATA without FILE_WRITE_DATA: every write lands at the end
    // of the file, so there is no file pointer to keep. The sharing matches what
    // std::ofstream allowed, so a reader can still open the log while it grows.
    static HANDLE OpenForAppend(const std::filesystem::path& path, DWORD disposition) {
        return CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    // The file has always had CRLF line ends - std::ofstream in text mode wrote
    // them - and a raw handle has no text mode, so the conversion is done here.
    static std::string WithCrLf(const std::string& text) {
        std::string converted;
        converted.reserve(text.size() + 8);
        for (const char character : text) {
            if (character == '\n') converted += '\r';
            converted += character;
        }
        return converted;
    }

    void Append(const std::string& text) {
        if (m_file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(m_file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }

    void Open() {
        const std::wstring name = FileName();
        // Appended, never truncated. Opening with ios::trunc destroyed the
        // evidence of a crash the moment the user relaunched to collect it -
        // and the bug report template asks for this exact file.
        for (const auto& directory : {ModuleDirectory(), FallbackDirectory()}) {
            if (directory.empty()) continue;
            m_path = directory / name;
            m_file = OpenForAppend(m_path, OPEN_ALWAYS);
            if (m_file != INVALID_HANDLE_VALUE) return;
        }
        m_path = name;
        m_file = OpenForAppend(m_path, OPEN_ALWAYS);
    }

    void Roll() {
        if (m_path.empty()) { m_written = 0; return; }
        if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
        std::error_code error;
        std::filesystem::path previous = m_path;
        previous += L".1";
        std::filesystem::remove(previous, error);
        std::filesystem::rename(m_path, previous, error);
        m_written = 0;
        m_file = OpenForAppend(m_path, CREATE_ALWAYS);
    }

    Log() {
        Open();
        std::error_code error;
        const auto existing = std::filesystem::file_size(m_path, error);
        m_written = error ? 0u : existing;
        SYSTEMTIME st{}; GetLocalTime(&st);
        std::ostringstream banner;
        banner << "\n===== session started " << std::setfill('0')
               << st.wYear << '-' << std::setw(2) << st.wMonth << '-' << std::setw(2) << st.wDay
               << ' ' << std::setw(2) << st.wHour << ':' << std::setw(2) << st.wMinute
               << ':' << std::setw(2) << st.wSecond << " pid=" << GetCurrentProcessId()
               << " =====\n";
        const std::string opening = WithCrLf(banner.str());
        Append(opening);
        m_written += opening.size();
    }

    // Never runs in practice - the instance is a function-local static - but a
    // handle is closed where it was opened.
    ~Log() {
        if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
    }
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    // Sampled once: attaching a debugger mid-session is not worth a syscall
    // on every line.
    const bool m_debugger = IsDebuggerPresent() != FALSE;
    std::filesystem::path m_path;
    HANDLE m_file = INVALID_HANDLE_VALUE;
    std::uintmax_t m_written = 0;
    std::mutex m_mutex;
};

#define LOG(x) do { std::ostringstream _oss; _oss << x; Log::Get().Write(_oss.str()); } while(0)
