#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "AtomicFile.h"
#include "NeuralCache.h"
#include "RuntimePolicy.h"

// The community dlssg_for_sm86 add-on (github.com/sdli1995/dlssg_for_sm86),
// which runs DLSS Frame Generation on RTX 20 and 30 GPUs, and what the player
// says when the runtime refuses Frame Generation (issue #14).
//
// The add-on is a proxy DLL - version.dll, winmm.dll, dbghelp.dll or
// dinput8.dll beside the executable - that hooks the load of nvngx_dlssg.dll
// once it is in the process. It relies on the executable's own import of that
// name being found beside it. The player's load-time imports resolve from
// System32 only (/DEPENDENTLOADFLAG:0x800, CMakeLists.txt), so the System32
// VERSION.dll is in by the time NGX loads nvngx_dlssg.dll, whose own
// VERSION.dll import then binds to it, and the proxy is never loaded.
//
// It is loaded here by full path instead, which Windows admits as a second
// module of the same name. Only when the add-on's own ini sits beside it: a
// lone version.dll is not the add-on, and nothing beside the player loads
// silently - the start screen names what was loaded. Never dxgi.dll, which the
// player refuses beside itself, or d3d12.dll, which it renders through; the
// add-on ships those two only as alternatives.
namespace framegen_addon {

inline constexpr std::wstring_view kMarkerFile = L"dlssg_sm86.ini";
// The add-on's own order: the first one present is the one that loads.
inline constexpr std::array<std::wstring_view, 4> kProxyNames{
    L"version.dll", L"winmm.dll", L"dbghelp.dll", L"dinput8.dll"};

// `isFileBeside(name)` answers whether a regular file of that name is beside
// the executable. Empty when the add-on is not installed there.
template <class IsFileBeside>
std::wstring ProxyToLoad(IsFileBeside&& isFileBeside)
{
    if (!isFileBeside(kMarkerFile)) return {};
    for (const std::wstring_view name : kProxyNames)
        if (isFileBeside(name)) return std::wstring(name);
    return {};
}

struct LoadResult {
    std::wstring proxy;      // the proxy's file name; empty when none was found
    bool loaded{};
    DWORD error{};           // GetLastError when the load failed
    HMODULE module{};        // held for the life of the process
    // The proxy's SHA-256 (lowercase hex; empty when it could not be read), and
    // whether it is the build kPinnedFiles names for that file name. Reported,
    // never enforced: a user may install another release of the add-on by hand,
    // and the log is where a mismatch is worth seeing.
    std::string sha256;
    bool pinned{};
};

// Defined after kPinnedFiles, below.
inline bool PinnedBuild(std::wstring_view saveAs, std::string_view sha256);

inline bool RegularFile(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

// Loads the add-on's proxy from `directory` when it is installed there.
// Called once, early in wWinMain, before anything creates an NGX feature, so
// the add-on's hooks are in place when nvngx_dlssg.dll is first loaded - the
// point a game's import would have loaded it at.
inline LoadResult LoadBeside(const std::filesystem::path& directory)
{
    LoadResult result{};
    result.proxy = ProxyToLoad([&](std::wstring_view name) { return RegularFile(directory / name); });
    if (result.proxy.empty()) return result;
    // The proxy's own dependencies - the real DLL it forwards to included -
    // come from its folder and System32, never the current directory.
    const std::filesystem::path path = directory / result.proxy;
    result.module = LoadLibraryExW(path.c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    result.loaded = result.module != nullptr;
    if (!result.loaded) result.error = GetLastError();
    // After the load: a mapped image cannot be rewritten in place, so on success
    // this is the file the process is running.
    result.sha256 = Sha256File(path).value_or(std::string());
    result.pinned = !result.sha256.empty() && PinnedBuild(result.proxy, result.sha256);
    return result;
}

// What follows "This GPU and driver admit no generated frames." The runtime's
// refusal on an RTX 20 or 30 is NVIDIA shipping Frame Generation for RTX 40
// and 50 only, which no driver update changes; with the add-on loaded, the
// add-on's own log is where the answer is.
enum class RefusalAdvice { UpdateDriver, NeedsRtx40, AddonRefused };

inline RefusalAdvice AdviceFor(GpuGeneration generation, bool addonLoaded)
{
    if (addonLoaded) return RefusalAdvice::AddonRefused;
    if (generation == GpuGeneration::Rtx20Turing || generation == GpuGeneration::Rtx30Ampere)
        return RefusalAdvice::NeedsRtx40;
    return RefusalAdvice::UpdateDriver;
}

inline const wchar_t* AdviceKey(RefusalAdvice advice)
{
    switch (advice) {
    case RefusalAdvice::NeedsRtx40: return L"framegen.gpu_next_step";
    case RefusalAdvice::AddonRefused: return L"framegen.addon_next_step";
    case RefusalAdvice::UpdateDriver: break;
    }
    return L"framegen.driver_next_step";
}

// The status line's form, in place of "refused by the driver".
inline const wchar_t* AdviceShortKey(RefusalAdvice advice)
{
    switch (advice) {
    case RefusalAdvice::NeedsRtx40: return L"framegen.refusal.gpu.short";
    case RefusalAdvice::AddonRefused: return L"framegen.refusal.addon.short";
    case RefusalAdvice::UpdateDriver: break;
    }
    return L"framegen.refusal.runtime.short";
}

// ---- Getting it --------------------------------------------------------------
//
// The add-on is not bundled. Its binary embeds NVIDIA's runtime with kernels
// recompiled outside any NVIDIA licence and its source is unpublished, so the
// package must not carry it. On an RTX 20/30 whose runtime refusal the user
// sees, the player offers to fetch it instead: from its author's repository,
// at one pinned commit, each file held to its SHA-256 before anything is
// written. What is offered is exactly what was reviewed.
struct PinnedFile {
    std::wstring_view name;     // the path in the author's repository
    std::string_view sha256;    // lowercase hex
    uint64_t bytes{};
    std::wstring_view saveAs;   // beside the player
};

inline constexpr std::wstring_view kSourceHost = L"raw.githubusercontent.com";
inline constexpr std::wstring_view kSourceRepository = L"sdli1995/dlssg_for_sm86";
// 0.3.5, 2026-09-19.
inline constexpr std::wstring_view kSourceCommit = L"9621db573e07ed54f50c15bbb585ed9a7bdfac28";
inline constexpr std::wstring_view kSourcePage = L"https://github.com/sdli1995/dlssg_for_sm86";
// The ini last: it is the loader's marker, so a run cut short leaves a proxy
// nothing loads rather than a half-installed add-on that does.
inline constexpr std::array<PinnedFile, 3> kPinnedFiles{{
    {L"version.dll", "c3934a09399f022504227c72df0bf8c0de55f9a08880dddde898c5262cefa838", 30021920, L"version.dll"},
    {L"THIRD_PARTY_NOTICES.txt", "ac3b44ab30a4235edd18feca1ab4f802d57c8d3d0ee4878dc77b81a6b127155f", 3349,
     L"dlssg_sm86-THIRD_PARTY_NOTICES.txt"},
    {L"dlssg_sm86.ini", "2616857ee29ec61e33c8b52e1b50f4c93cb5339adbb13b73f0ae71a722427a43", 3548, L"dlssg_sm86.ini"},
}};

inline bool PinnedBuild(std::wstring_view saveAs, std::string_view sha256)
{
    return std::ranges::any_of(kPinnedFiles, [&](const PinnedFile& file) {
        return file.saveAs == saveAs && file.sha256 == sha256;
    });
}

inline uint64_t PinnedBytes()
{
    uint64_t total = 0;
    for (const PinnedFile& file : kPinnedFiles) total += file.bytes;
    return total;
}

inline std::wstring SourcePath(const PinnedFile& file)
{
    return L"/" + std::wstring(kSourceRepository) + L"/" + std::wstring(kSourceCommit) + L"/" + std::wstring(file.name);
}

// Offered where the runtime refuses because of the GPU and nothing would
// change that: an RTX 20 or 30 without the add-on loaded.
inline bool OfferDownload(GpuGeneration generation, bool addonLoaded)
{
    return !addonLoaded && AdviceFor(generation, false) == RefusalAdvice::NeedsRtx40;
}

enum class InstallStep { None, Download, Verify, Conflict, Write };

struct InstallResult {
    InstallStep failed{InstallStep::None};
    std::wstring file;      // the file the step failed on
    DWORD error{};          // Write only
};

inline std::optional<std::string> ReadWholeFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// Whether a file already at `file`'s destination is something other than the
// pinned build - another mod's version.dll - which the install must not replace.
inline bool ForeignFileAt(const std::filesystem::path& directory, const PinnedFile& file)
{
    const std::filesystem::path destination = directory / file.saveAs;
    if (!RegularFile(destination)) return false;
    const auto existing = ReadWholeFile(destination);
    return !existing || Sha256Bytes(*existing).value_or(std::string()) != file.sha256;
}

// `fetch(file)` returns the file's bytes, or nothing when it could not be
// fetched. Every file is fetched and verified before any is written, and an
// existing file of the same name that is not the pinned one - another mod's
// version.dll - stops the install with nothing changed. That check is made
// again for each file just before it is written: the download takes seconds,
// and a foreign file that appeared meanwhile must not be overwritten either.
// A stop there can leave the files before it written, but only as their pinned
// builds, and the ini - the loader's marker - is the last of them.
template <class Fetch>
InstallResult Install(const std::filesystem::path& directory, Fetch&& fetch,
                      std::span<const PinnedFile> files = kPinnedFiles)
{
    std::vector<std::string> bodies;
    bodies.reserve(files.size());
    for (const PinnedFile& file : files) {
        if (ForeignFileAt(directory, file)) return {InstallStep::Conflict, std::wstring(file.saveAs)};
        std::optional<std::string> body = fetch(file);
        if (!body) return {InstallStep::Download, std::wstring(file.name)};
        if (body->size() != file.bytes || Sha256Bytes(*body).value_or(std::string()) != file.sha256)
            return {InstallStep::Verify, std::wstring(file.name)};
        bodies.push_back(std::move(*body));
    }
    for (size_t index = 0; index < files.size(); ++index) {
        if (ForeignFileAt(directory, files[index]))
            return {InstallStep::Conflict, std::wstring(files[index].saveAs)};
        const atomic_file::Outcome written = atomic_file::Replace(directory / files[index].saveAs, bodies[index]);
        if (!written) return {InstallStep::Write, std::wstring(files[index].saveAs), written.error};
    }
    return {};
}

} // namespace framegen_addon
