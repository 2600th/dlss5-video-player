#pragma once
#include <windows.h>
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
    }
private:
    static std::filesystem::path LogPath() {
        std::wstring path(32768,L'\0');
        const DWORD length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
        if(!length||length>=path.size())return L"DLSSVideoPlayer.log";
        path.resize(length);
        return std::filesystem::path(path).parent_path()/L"DLSSVideoPlayer.log";
    }
    Log() : m_file(LogPath(), std::ios::out | std::ios::trunc) {}
    std::ofstream m_file;
    std::mutex m_mutex;
};

#define LOG(x) do { std::ostringstream _oss; _oss << x; Log::Get().Write(_oss.str()); } while(0)
