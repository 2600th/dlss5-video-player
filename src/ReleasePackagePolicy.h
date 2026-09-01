#pragma once

#include <algorithm>
#include <array>
#include <cwctype>
#include <string>
#include <string_view>

namespace release_package_policy {

inline std::wstring NormalizePath(std::wstring_view path)
{
    std::wstring normalized(path);
    std::replace(normalized.begin(), normalized.end(), L'\\', L'/');
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return normalized;
}

inline bool IsSafeRelativePath(std::wstring_view normalized)
{
    if (normalized.empty() || normalized.front() == L'/' || normalized.find(L':') != std::wstring::npos ||
        normalized.find(L"../") != std::wstring::npos || normalized.find(L"//") != std::wstring::npos) {
        return false;
    }
    return true;
}

inline bool IsAllowedPath(std::wstring_view path)
{
    const std::wstring normalized = NormalizePath(path);
    if (!IsSafeRelativePath(normalized)) return false;

    static constexpr std::array<std::wstring_view, 38> allowed = {
        L"dlssvideoplayer.exe", L"ffmpeg.exe", L"ffprobe.exe", L"yt-dlp.exe", L"deno.exe",
        L"dxgi.dll", L"reshade.ini", L"reshadepreset.ini", L"renodx-dlss5.addon64",
        L"nvngx_dlss.dll", L"nvngx_dlssnr.dll", L"sl.common.dll", L"sl.dlss.dll",
        L"sl.dlss_g.dll", L"sl.dlss_nr.dll", L"sl.interposer.dll", L"sl.nis.dll",
        L"sl.pcl.dll", L"sl.reflex.dll", L"readme.md", L"license", L"security.md",
        L"contributing.md", L"changelog.md", L"third_party.md",
        L"docs/architecture.md", L"docs/building.md", L"docs/dlss5_setup.md",
        L"docs/troubleshooting.md", L"experimental_runtime_notice.txt", L"package_manifest.txt",
        L"third_party_licenses/yt-dlp-2026.08.19.txt", L"third_party_licenses/deno-2.9.5.txt",
        L"third_party_licenses/ffmpeg.txt", L"third_party_licenses/experimental-runtime.txt",
        L"third_party_licenses/dlss5-feeder-mit.txt", L"third_party_licenses/tabler-mit.txt",
        L"docs/related_projects.md"
    };
    return std::find(allowed.begin(), allowed.end(), normalized) != allowed.end();
}

inline bool IsAllowedPublicPath(std::wstring_view path)
{
    const std::wstring normalized = NormalizePath(path);
    if (!IsSafeRelativePath(normalized)) return false;

    static constexpr std::array<std::wstring_view, 17> allowed = {
        L"dlssvideoplayer.exe", L"nvngx_dlss.dll", L"readme.md", L"license",
        L"security.md", L"contributing.md", L"changelog.md", L"third_party.md",
        L"public_release_notice.txt", L"package_manifest.txt",
        L"third_party_licenses/nvidia-dlss-sdk.txt",
        L"third_party_licenses/tabler-mit.txt", L"docs/architecture.md",
        L"docs/building.md", L"docs/dlss5_setup.md", L"docs/related_projects.md",
        L"docs/troubleshooting.md"
    };
    return std::find(allowed.begin(), allowed.end(), normalized) != allowed.end();
}

} // namespace release_package_policy
