#pragma once

#include <windows.h>
#include <dbghelp.h>

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <string>

#include "Log.h"

// The handler below calls MiniDumpWriteDump, so whoever includes this links
// what it needs rather than depending on the target's library list.
#pragma comment(lib, "dbghelp.lib")

// An unhandled access violation produced no artifact at all: no dump, and -
// before the log learned to append - not even the log lines leading up to it,
// because relaunching to collect them truncated the file.
//
// This writes a minidump beside the log, which is the one place the user has
// already been told to look, and logs where it went so the line survives even
// if the dump itself cannot be written.
namespace crash_dump {

namespace detail {

// Twice MAX_PATH: the log sits beside the executable or under LOCALAPPDATA,
// and a longer path only costs the dump, never the handler. Kept small because
// the handler also runs for a stack overflow, on what is left of the stack.
inline constexpr size_t kPathCapacity = 520;

// Filled once by Prepare, at install time, and only read by the handler. The
// handler used to build the dump path with std::filesystem and write its line
// through LOG: both allocate, and LOG takes the log's mutex, so a fault inside
// Log::Write - holding that mutex - deadlocked the handler, and a helper that
// hung there kept the GPU until the parent's 120 s watchdog killed it.
inline wchar_t g_dumpBase[kPathCapacity]{};   // log path without its extension
inline wchar_t g_logPath[kPathCapacity]{};
inline bool g_prepared{};

inline bool CopyWide(wchar_t (&out)[kPathCapacity], const std::wstring& text)
{
    if (text.size() >= kPathCapacity) return false;
    wmemcpy(out, text.data(), text.size());
    out[text.size()] = L'\0';
    return true;
}

inline void Prepare(const std::filesystem::path& logPath)
{
    std::filesystem::path base = logPath.empty() ? std::filesystem::path(L"DLSSVideoPlayer") : logPath;
    base.replace_extension();
    g_prepared = CopyWide(g_dumpBase, base.wstring());
    if (!CopyWide(g_logPath, logPath.wstring())) g_logPath[0] = L'\0';
}

// Bounded appenders over a caller's stack buffer: no allocation, no CRT
// formatting, and a full buffer truncates rather than overruns.
template <class Char>
struct Text {
    Char* data;
    size_t capacity;   // including the terminator
    size_t length{};

    void Put(Char c)
    {
        if (length + 1 < capacity) data[length++] = c;
        data[length] = Char{};
    }
    void Put(const Char* text)
    {
        while (*text) Put(*text++);
    }
    void Decimal(uint64_t value, int width = 1)
    {
        Char digits[20];
        int count = 0;
        do {
            digits[count++] = static_cast<Char>('0' + value % 10);
            value /= 10;
        } while (value && count < 20);
        for (int pad = count; pad < width; ++pad) Put(static_cast<Char>('0'));
        while (count) Put(digits[--count]);
    }
    void Hex(uint64_t value)
    {
        Char digits[16];
        int count = 0;
        do {
            digits[count++] = static_cast<Char>("0123456789abcdef"[value & 0xF]);
            value >>= 4;
        } while (value && count < 16);
        while (count) Put(digits[--count]);
    }
};

// "<base>-crash-YYYYMMDD-HHMMSS-<pid>.dmp", the name PathFor produced before.
inline bool ComposeDumpPath(wchar_t* out, size_t capacity, const SYSTEMTIME& now, DWORD processId)
{
    if (!g_prepared || !capacity) return false;
    Text<wchar_t> path{out, capacity};
    path.Put(g_dumpBase);
    path.Put(L"-crash-");
    path.Decimal(now.wYear, 4);
    path.Decimal(now.wMonth, 2);
    path.Decimal(now.wDay, 2);
    path.Put(L'-');
    path.Decimal(now.wHour, 2);
    path.Decimal(now.wMinute, 2);
    path.Decimal(now.wSecond, 2);
    path.Put(L'-');
    path.Decimal(processId);
    path.Put(L".dmp");
    // Truncated means some other file's name; better no dump than that one.
    return path.length + 1 < capacity;
}

// Starts a line in Log's own format: "[HH:MM:SS.mmm] ".
inline void StampLine(Text<char>& line, const SYSTEMTIME& now)
{
    line.Put('[');
    line.Decimal(now.wHour, 2);
    line.Put(':');
    line.Decimal(now.wMinute, 2);
    line.Put(':');
    line.Decimal(now.wSecond, 2);
    line.Put('.');
    line.Decimal(now.wMilliseconds, 3);
    line.Put("] ");
}

// Appends one finished line straight to the log file, bypassing the Log object
// and its mutex. Log flushes after every line and opens the file shared, so
// the line lands after whatever was last logged.
inline void AppendToLog(const Text<char>& line)
{
    if (!g_logPath[0]) return;
    const HANDLE file = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line.data, static_cast<DWORD>(line.length), &written, nullptr);
    CloseHandle(file);
}

inline LONG WINAPI Handler(EXCEPTION_POINTERS* exception)
{
    // Everything here runs in a process that is already broken, so it stays on
    // the stack, takes no lock this program owns, and treats every call as
    // allowed to fail. MiniDumpWriteDump itself is the one thing left that
    // allocates, and there is no dump without it.
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t dump[kPathCapacity + 64];
    const bool named = ComposeDumpPath(dump, sizeof(dump) / sizeof(dump[0]), now, GetCurrentProcessId());

    bool written = false;
    DWORD error = ERROR_BAD_PATHNAME;
    const HANDLE file = named ? CreateFileW(dump, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                            FILE_ATTRIBUTE_NORMAL, nullptr)
                              : INVALID_HANDLE_VALUE;
    if (named && file == INVALID_HANDLE_VALUE) error = GetLastError();
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION information{};
        information.ThreadId = GetCurrentThreadId();
        information.ExceptionPointers = exception;
        information.ClientPointers = FALSE;
        // WithIndirectlyReferencedMemory costs a few MB and is what makes the
        // frame buffers and the failing pointer readable in a debugger; without
        // it a dump of this player shows the stack and almost nothing else.
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                     MiniDumpWithDataSegs |
                                                     MiniDumpWithThreadInfo |
                                                     MiniDumpWithUnloadedModules);
        written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                    exception ? &information : nullptr, nullptr, nullptr) != FALSE;
        if (!written) error = GetLastError();
        CloseHandle(file);
        if (!written) DeleteFileW(dump);
    }

    const DWORD code = exception && exception->ExceptionRecord
                           ? exception->ExceptionRecord->ExceptionCode : 0u;
    const void* at = exception && exception->ExceptionRecord
                         ? exception->ExceptionRecord->ExceptionAddress : nullptr;
    char buffer[kPathCapacity * 3 + 128];
    Text<char> line{buffer, sizeof(buffer)};
    StampLine(line, now);
    line.Put("Unhandled exception 0x");
    line.Hex(code);
    line.Put(" at 0x");
    line.Hex(reinterpret_cast<uintptr_t>(at));
    if (written) {
        line.Put("; minidump written to ");
        // Converted in place into the line's remaining space.
        const int room = static_cast<int>(line.capacity - line.length - 2);
        const int bytes = room > 0 ? WideCharToMultiByte(CP_UTF8, 0, dump, -1, buffer + line.length, room,
                                                         nullptr, nullptr) : 0;
        if (bytes > 1) line.length += static_cast<size_t>(bytes - 1);
        else line.Put("(unprintable path)");
    } else {
        line.Put("; a minidump could not be written (winerr=");
        line.Decimal(error);
        line.Put(").");
    }
    line.Put('\n');
    AppendToLog(line);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace detail

// Call once, as early as possible. Both the player and the helper install it:
// a helper that dies leaves the parent reporting only an exit code. Resolving
// the log's path here also opens the log, so the handler never has to.
inline void Install()
{
    detail::Prepare(Log::Path());
    SetUnhandledExceptionFilter(&detail::Handler);
}

} // namespace crash_dump
