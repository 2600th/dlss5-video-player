#pragma once

// What a test process shares with every other test process on the machine:
// the temp directory and the children it starts. Kept out of TestSupport.h,
// which deliberately includes nothing of windows.h.

#include <windows.h>

#include "KillOnCloseJob.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace test_support {

// A cache fixture nests "<fixture>\cache\staging\render-<64-char key>-<pid>-
// <nonce>\neural-settings.ini" under the temp directory, about 190 characters
// before the temp path itself. The test executables are not long-path aware,
// so a long %TMP% (a sandbox's per-session scratch folder was 130) pushed the
// staging path past MAX_PATH and every cache case failed with error=3. Past
// this length the fixtures move to a short root on the same drive instead.
constexpr size_t kLongestFixtureTempPath = 64;

inline std::filesystem::path FixtureTempRoot()
{
    std::error_code error;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(error);
    if (!error && temp.native().size() <= kLongestFixtureTempPath) return temp;
    const std::filesystem::path shortRoot =
        (error ? std::filesystem::path(L"C:\\") : temp.root_path()) / L"t";
    std::filesystem::create_directories(shortRoot, error);
    return shortRoot;
}

// Conservative, like the cache's own owner check: a process this one may not
// open counts as alive. Only a pid the kernel does not know, or one whose
// process has exited, is gone.
inline bool ProcessAlive(DWORD pid)
{
    if (pid == GetCurrentProcessId()) return true;
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

// Whether a "<prefix><pid>-<anything>" fixture another test process left
// behind may be deleted. Its owner may be a concurrent run of the same suite
// - another worktree, another CI shard - that is still using it: deleting
// every sibling but this process's own broke such runs at random. So only a
// dead owner's directory goes, or one older than any run could be, which is
// also what frees a directory whose pid was reused by an unrelated process.
constexpr std::chrono::hours kAbandonedFixtureAge{24};

inline bool FixtureAbandoned(const std::filesystem::path& directory, std::wstring_view prefix)
{
    const std::wstring name = directory.filename().wstring();
    if (!name.starts_with(prefix)) return false;
    uint64_t pid = 0;
    size_t digits = 0;
    for (size_t index = prefix.size(); index < name.size() && name[index] != L'-'; ++index, ++digits) {
        const wchar_t digit = name[index];
        if (digit < L'0' || digit > L'9' || digits == 10) return false;
        pid = pid * 10 + static_cast<uint64_t>(digit - L'0');
    }
    if (digits == 0 || pid > MAXDWORD || pid == GetCurrentProcessId()) return false;
    if (!ProcessAlive(static_cast<DWORD>(pid))) return true;
    std::error_code error;
    const auto written = std::filesystem::last_write_time(directory, error);
    return !error && std::filesystem::file_time_type::clock::now() - written > kAbandonedFixtureAge;
}

// Every process a test starts, and everything those start, dies with the test
// process. A crashed NeuralPrerenderTests left its suspended --suspended-child
// copies running; one held the test executable open and the next build's link
// failed with LNK1104. Putting this process in a kill-on-close job makes each
// child a member at creation (Windows nests the product's own per-child jobs
// under it), and the kernel closes the only handle when this process exits,
// however it exits. Call it from the test's main before it starts any child.
inline void ContainChildProcesses()
{
    static const bool contained = [] {
        const HANDLE job = CreateKillOnCloseJob();
        if (!job || !AssignProcessToJobObject(job, GetCurrentProcess())) {
            std::cerr << "warning: children of this test are not contained, error "
                      << GetLastError() << '\n';
            if (job) CloseHandle(job);
            return false;
        }
        return true; // The handle stays open until the process ends; that is the point.
    }();
    (void)contained;
}

} // namespace test_support
