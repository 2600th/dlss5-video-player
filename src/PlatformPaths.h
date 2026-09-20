#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

// One checked answer to "where is this executable", for the fourteen call
// sites that used to ask it three incompatible ways.
//
// Two of those were wrong. DLSSBackend.cpp and DLSSGBackend.cpp were
// byte-identical:
//
//     wchar_t exePath[MAX_PATH]{};
//     GetModuleFileNameW(nullptr, exePath, MAX_PATH);   // no return check
//     auto logDir = std::filesystem::path(exePath).parent_path() / L"ngx_logs";
//
// On a path longer than 260 characters GetModuleFileNameW fills the buffer,
// returns exactly the size it was given and sets ERROR_INSUFFICIENT_BUFFER -
// and on Windows before 10 does not null-terminate at all. Both sites then
// took parent_path() of a truncated string and created ngx_logs somewhere
// else entirely, silently.
//
// A return equal to the buffer size therefore means truncation and never a
// complete path. That distinction is the whole point of this file.
namespace platform_paths {

// Windows caps a path at 32767 characters including the terminator, so this
// is the size at which growing stops being useful.
inline constexpr uint32_t kMaxPathCharacters = 32768;
// Big enough for essentially every real install, so the common case is one
// call; the loop below covers the rest rather than assuming.
inline constexpr uint32_t kInitialPathCharacters = 512;

// `query(buffer, size)` has GetModuleFileNameW's contract: 0 on failure, the
// character count on success, and `size` when the path did not fit. Injected
// so the growth loop is testable without a 300-character install directory.
template <class Query>
std::optional<std::filesystem::path> ModulePathWith(Query&& query)
{
    std::wstring buffer;
    for (uint32_t size = kInitialPathCharacters; size <= kMaxPathCharacters; size *= 2) {
        buffer.assign(size, L'\0');
        const uint32_t length = query(buffer.data(), size);
        if (length == 0) return std::nullopt;   // a real failure; a bigger buffer will not help
        if (length >= size) continue;           // truncated: ask again with room
        buffer.resize(length);
        return std::filesystem::path(std::move(buffer));
    }
    return std::nullopt;
}

template <class Query>
std::optional<std::filesystem::path> ModuleDirectoryWith(Query&& query)
{
    const auto path = ModulePathWith(std::forward<Query>(query));
    if (!path) return std::nullopt;
    return path->parent_path();
}

inline uint32_t QueryCurrentModulePath(wchar_t* buffer, uint32_t size)
{
    return GetModuleFileNameW(nullptr, buffer, size);
}

// The running executable's full path, and the directory holding it. Both
// return nullopt rather than a truncated or empty path, so a caller has to
// decide what to do when the answer is unavailable instead of silently
// operating on the wrong directory.
inline std::optional<std::filesystem::path> ModulePath()
{
    return ModulePathWith(&QueryCurrentModulePath);
}

inline std::optional<std::filesystem::path> ModuleDirectory()
{
    return ModuleDirectoryWith(&QueryCurrentModulePath);
}

} // namespace platform_paths
