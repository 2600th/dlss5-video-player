#pragma once

#include "PlatformPaths.h"

#include <windows.h>

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

// Where ffmpeg.exe and ffprobe.exe are taken from. Three lookups had drifted:
// the video decoder guarded the neural-runtime package and searched four
// places, MediaPipeline guarded it and searched one, and the audio player had
// no guard and skipped <dir>\ffmpeg, so on a layout with the tools only there
// the audio of a video could come from whatever ffmpeg PATH offered while its
// frames came from the packaged one.
namespace media_tools {

// Whether PATH is searched after every packaged location missed. Playback
// keeps it (a development tree without staged tools still plays, and the
// real-media tests rely on it); the export and render pipeline never does, so
// what it writes into the cache or a file is always the packaged build.
enum class Fallback { None, SearchPath };

// `explicitDirectory`, when set, is the only directory looked in. Otherwise
// `moduleDirectory` and the development layouts around it are. Either way a
// directory named neural-runtime is the contained helper package, which
// carries no tools of its own: only the copy beside its parent - the player's
// - is used, never one from PATH.
inline std::filesystem::path FindToolIn(const std::filesystem::path& explicitDirectory,
                                        const std::filesystem::path& moduleDirectory,
                                        std::wstring_view name, Fallback fallback)
{
    namespace fs = std::filesystem;
    const auto isFile = [](const fs::path& path) {
        std::error_code error;
        return fs::is_regular_file(path, error) && !error;
    };
    const fs::path& base = explicitDirectory.empty() ? moduleDirectory : explicitDirectory;
    if (!base.empty() && base.filename() == L"neural-runtime") {
        const fs::path shared = base.parent_path() / name;
        return isFile(shared) ? shared : fs::path{};
    }
    if (!explicitDirectory.empty()) {
        const fs::path candidate = explicitDirectory / name;
        return isFile(candidate) ? candidate : fs::path{};
    }
    if (!base.empty()) {
        for (const fs::path& candidate : {base / name, base / L"ffmpeg" / name, base / L"ffmpeg" / L"bin" / name,
                                          base.parent_path() / L"ffmpeg" / L"bin" / name})
            if (isFile(candidate)) return candidate;
    }
    if (fallback == Fallback::None) return {};
    const std::wstring file(name);
    std::wstring found(platform_paths::kMaxPathCharacters, L'\0');
    const DWORD length = SearchPathW(nullptr, file.c_str(), nullptr, static_cast<DWORD>(found.size()),
                                     found.data(), nullptr);
    if (!length || length >= found.size()) return {};
    found.resize(length);
    return fs::path(found);
}

inline std::filesystem::path FindTool(const std::filesystem::path& explicitDirectory, std::wstring_view name,
                                      Fallback fallback)
{
    return FindToolIn(explicitDirectory,
                      platform_paths::ModuleDirectory().value_or(std::filesystem::path{}), name, fallback);
}

} // namespace media_tools
