#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>

// Replacing a file so that a reader - or the next launch after a power cut -
// sees the old contents or the new ones and never part of either: write a
// temporary beside it, flush that to the device, and rename it over the
// destination write-through.
//
// Three writers carried their own copy (the stored preflight verdict, the
// recent-videos list, ReShade.ini), and copies had already lost the flush
// once: a rename is only as durable as the bytes it publishes, so an
// unflushed copy could leave a published file empty after a crash. One
// implementation keeps every step in every writer.
namespace atomic_file {

enum class Step { None, CreateTemporary, Write, Replace };

struct Outcome {
    Step failed = Step::None;
    DWORD error = ERROR_SUCCESS;
    explicit operator bool() const { return failed == Step::None; }
};

// Beside `destination`, unique to this process and call. The tick count keeps
// a temporary a dead process left under a reused pid from colliding with it.
inline std::filesystem::path TemporaryBeside(const std::filesystem::path& destination)
{
    static std::atomic<unsigned long> sequence{};
    std::filesystem::path temporary = destination;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1));
    return temporary;
}

// Writes `bytes` to `temporary`, flushes it, and renames it over
// `destination`. `creation` is CREATE_NEW for a name the caller made up - a
// file already there is someone else's and is left alone - or CREATE_ALWAYS
// for one the caller already reserved (GetTempFileNameW creates its file).
// On any failure the temporary is removed once it is known to be ours; the
// destination is never touched.
inline Outcome WriteAndReplace(const std::filesystem::path& temporary, DWORD creation,
                               const std::filesystem::path& destination, std::string_view bytes)
{
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, creation,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (creation != CREATE_NEW) DeleteFileW(temporary.c_str());
        return {Step::CreateTemporary, error};
    }
    bool written = true;
    for (size_t offset = 0; written && offset < bytes.size();) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, MAXDWORD));
        DWORD wrote = 0;
        written = WriteFile(file, bytes.data() + offset, chunk, &wrote, nullptr) && wrote == chunk;
        offset += wrote;
    }
    const bool flushed = written && FlushFileBuffers(file);
    const DWORD writeError = flushed ? ERROR_SUCCESS : GetLastError();
    const bool closed = CloseHandle(file) != FALSE;
    if (!flushed || !closed) {
        const DWORD error = flushed ? GetLastError() : writeError;
        DeleteFileW(temporary.c_str());
        return {Step::Write, error};
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileW(temporary.c_str());
        return {Step::Replace, error};
    }
    return {};
}

inline Outcome Replace(const std::filesystem::path& destination, std::string_view bytes)
{
    return WriteAndReplace(TemporaryBeside(destination), CREATE_NEW, destination, bytes);
}

} // namespace atomic_file
