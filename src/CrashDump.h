#pragma once

#include <windows.h>
#include <dbghelp.h>

#include <filesystem>
#include <string>

#include "Log.h"

// An unhandled access violation produced no artifact at all: no dump, and -
// before the log learned to append - not even the log lines leading up to it,
// because relaunching to collect them truncated the file.
//
// This writes a minidump beside the log, which is the one place the user has
// already been told to look, and logs where it went so the line survives even
// if the dump itself cannot be written.
namespace crash_dump {

inline std::filesystem::path PathFor(const std::filesystem::path& logPath, const SYSTEMTIME& now)
{
    wchar_t stamp[64]{};
    swprintf_s(stamp, L"-crash-%04u%02u%02u-%02u%02u%02u-%lu.dmp",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
               static_cast<unsigned long>(GetCurrentProcessId()));
    std::filesystem::path base = logPath.empty() ? std::filesystem::path(L"DLSSVideoPlayer") : logPath;
    base.replace_extension();
    base += stamp;
    return base;
}

namespace detail {

inline LONG WINAPI Handler(EXCEPTION_POINTERS* exception)
{
    // Everything here runs in a process that is already broken, so it stays on
    // the stack, takes no lock this thread might already hold, and treats every
    // call as allowed to fail.
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const std::filesystem::path dump = PathFor(Log::Path(), now);

    const HANDLE file = CreateFileW(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    bool written = false;
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
        CloseHandle(file);
        if (!written) DeleteFileW(dump.c_str());
    }

    const DWORD code = exception && exception->ExceptionRecord
                           ? exception->ExceptionRecord->ExceptionCode : 0u;
    const void* at = exception && exception->ExceptionRecord
                         ? exception->ExceptionRecord->ExceptionAddress : nullptr;
    if (written)
        LOG("Unhandled exception 0x" << std::hex << code << std::dec << " at " << at
            << "; minidump written to " << dump.string());
    else
        LOG("Unhandled exception 0x" << std::hex << code << std::dec << " at " << at
            << "; a minidump could not be written (winerr=" << GetLastError() << ").");

    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace detail

// Call once, as early as possible. Both the player and the helper install it:
// a helper that dies leaves the parent reporting only an exit code.
inline void Install()
{
    SetUnhandledExceptionFilter(&detail::Handler);
}

} // namespace crash_dump
