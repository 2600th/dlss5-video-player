#pragma once
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
        std::lock_guard<std::mutex> lock(m_mutex);
        SYSTEMTIME st{}; GetLocalTime(&st);
        std::ostringstream line;
        line << '[' << std::setfill('0') << std::setw(2) << st.wHour << ':'
             << std::setw(2) << st.wMinute << ':' << std::setw(2) << st.wSecond
             << '.' << std::setw(3) << st.wMilliseconds << "] " << s << "\n";
        OutputDebugStringA(line.str().c_str());
        m_file << line.str();
        m_file.flush();
        m_written += line.str().size();
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
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return {};
        path.resize(length);
        return std::filesystem::path(path).parent_path();
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
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return L"DLSSVideoPlayer.log";
        path.resize(length);
        std::wstring stem = std::filesystem::path(path).stem().wstring();
        return stem.empty() ? std::wstring(L"DLSSVideoPlayer.log") : stem + L".log";
    }

    void Open() {
        const std::wstring name = FileName();
        // Appended, never truncated. Opening with ios::trunc destroyed the
        // evidence of a crash the moment the user relaunched to collect it -
        // and the bug report template asks for this exact file.
        for (const auto& directory : {ModuleDirectory(), FallbackDirectory()}) {
            if (directory.empty()) continue;
            m_path = directory / name;
            m_file.open(m_path, std::ios::out | std::ios::app);
            if (m_file.is_open()) return;
            m_file.clear();
        }
        m_path = name;
        m_file.open(m_path, std::ios::out | std::ios::app);
        if (!m_file.is_open()) m_file.clear();
    }

    void Roll() {
        if (m_path.empty()) { m_written = 0; return; }
        m_file.close();
        std::error_code error;
        std::filesystem::path previous = m_path;
        previous += L".1";
        std::filesystem::remove(previous, error);
        std::filesystem::rename(m_path, previous, error);
        m_written = 0;
        m_file.open(m_path, std::ios::out | std::ios::trunc);
        if (!m_file.is_open()) m_file.clear();
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
        m_file << banner.str();
        m_file.flush();
        m_written += banner.str().size();
    }

    std::filesystem::path m_path;
    std::ofstream m_file;
    std::uintmax_t m_written = 0;
    std::mutex m_mutex;
};

#define LOG(x) do { std::ostringstream _oss; _oss << x; Log::Get().Write(_oss.str()); } while(0)
