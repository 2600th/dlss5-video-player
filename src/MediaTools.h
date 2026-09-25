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

// Whether a PATH entry names one directory whatever the current directory and
// drive are: `X:\...` or a UNC `\\server\...` path. `.`, `bin`, the
// drive-relative `C:bin` and the root-relative `\bin` all resolve against the
// current directory or drive - wherever the video was opened from - so a tool
// found through them is no better than one planted beside the video.
inline bool IsFullyQualifiedDirectory(std::wstring_view entry)
{
    const auto separator = [](wchar_t c) { return c == L'\\' || c == L'/'; };
    const auto letter = [](wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); };
    if (entry.size() < 3) return false;
    if (letter(entry[0]) && entry[1] == L':' && separator(entry[2])) return true;
    return separator(entry[0]) && separator(entry[1]) && !separator(entry[2]);
}

// The first `name` in a fully qualified directory of `pathVariable`, a PATH
// value, for which `isFile` holds. SearchPathW with no path looks in the
// current directory before PATH, and SetSearchPathMode does not stop that, so
// PATH is walked here and nothing else is ever asked about. Entries split on
// semicolons outside double quotes and lose their quotes, as the command
// processor reads them; a quote cannot occur in a Windows path, so dropping
// one never turns a real directory into a different one.
template <class IsFile>
std::filesystem::path FindOnPathIn(std::wstring_view pathVariable, std::wstring_view name, IsFile&& isFile)
{
    std::wstring entry;
    bool quoted = false;
    for (size_t i = 0; i <= pathVariable.size(); ++i) {
        const bool end = i == pathVariable.size();
        const wchar_t c = end ? L';' : pathVariable[i];
        if (c == L'"') {
            quoted = !quoted;
            continue;
        }
        if (c != L';' || (quoted && !end)) {
            entry.push_back(c);
            continue;
        }
        if (IsFullyQualifiedDirectory(entry)) {
            std::filesystem::path candidate = std::filesystem::path(entry) / name;
            if (isFile(candidate)) return candidate;
        }
        entry.clear();
        quoted = false;
    }
    return {};
}

// This process's PATH, empty when it is unset. It can change between the size
// query and the read, so a read that no longer fits is asked again rather than
// trusted.
inline std::wstring PathVariable()
{
    std::wstring value;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD needed = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (needed == 0) return {};
        value.assign(needed, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"PATH", value.data(), needed);
        if (length < needed) {
            value.resize(length);
            return value;
        }
    }
    return {};
}

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
    return FindOnPathIn(PathVariable(), name, isFile);
}

inline std::filesystem::path FindTool(const std::filesystem::path& explicitDirectory, std::wstring_view name,
                                      Fallback fallback)
{
    return FindToolIn(explicitDirectory,
                      platform_paths::ModuleDirectory().value_or(std::filesystem::path{}), name, fallback);
}

} // namespace media_tools
