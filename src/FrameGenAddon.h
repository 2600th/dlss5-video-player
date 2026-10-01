#pragma once

#include <windows.h>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include "RuntimePolicy.h"

// The community dlssg_for_sm86 add-on (github.com/sdli1995/dlssg_for_sm86),
// which runs DLSS Frame Generation on RTX 20 and 30 GPUs, and what the player
// says when the runtime refuses Frame Generation (issue #14).
//
// The add-on is a proxy DLL - version.dll, winmm.dll, dbghelp.dll or
// dinput8.dll beside the executable - that hooks the load of nvngx_dlssg.dll
// once it is in the process. It relies on the executable's own import of that
// name being found beside it. Since 0.27.0 the player's load-time imports
// resolve from System32 only (/DEPENDENTLOADFLAG:0x800, CMakeLists.txt), so
// the System32 VERSION.dll is in by the time NGX loads nvngx_dlssg.dll, whose
// own VERSION.dll import then binds to it, and the proxy is never loaded.
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
};

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

} // namespace framegen_addon
