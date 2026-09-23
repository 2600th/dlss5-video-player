#pragma once

#include <cstddef>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Which of the files dropped on the window gets opened. The player plays one
// file at a time, and a drop of several used to open the first entry Explorer
// listed - a folder, a subtitle, a thumbnail - and discard the rest without a
// word. Now the first entry the open dialog would have listed as a supported
// file wins, the first file of any kind is the fallback (the dialog's default
// filter is "everything FFmpeg can open", so an unlisted extension is not a
// refusal), and the caller is told how many it did not open so it can say so.
namespace dropped_files {

// Whether `path` ends in one of the extensions in `patterns`, the open
// dialog's own "*.mp4;*.mkv;..." list, compared case-insensitively.
inline bool MatchesPatterns(std::wstring_view path, std::wstring_view patterns)
{
    const size_t dot = path.find_last_of(L'.');
    const size_t separator = path.find_last_of(L"\\/");
    if (dot == std::wstring_view::npos || (separator != std::wstring_view::npos && dot < separator))
        return false;
    const std::wstring_view extension = path.substr(dot);
    const auto same = [](std::wstring_view a, std::wstring_view b) {
        if (a.size() != b.size()) return false;
        for (size_t index = 0; index < a.size(); ++index)
            if (std::towlower(a[index]) != std::towlower(b[index])) return false;
        return true;
    };
    while (!patterns.empty()) {
        const size_t end = patterns.find(L';');
        std::wstring_view pattern = patterns.substr(0, end);
        if (pattern.starts_with(L'*')) pattern.remove_prefix(1);
        if (!pattern.empty() && same(pattern, extension)) return true;
        if (end == std::wstring_view::npos) break;
        patterns.remove_prefix(end + 1);
    }
    return false;
}

struct Choice {
    // Index into the dropped list, empty when nothing in it can be opened.
    std::optional<size_t> open;
    // Entries dropped that will not be opened.
    size_t ignored{};
};

// `isDirectory` answers for one entry; a folder is never opened.
template <typename IsDirectory>
Choice Choose(const std::vector<std::wstring>& paths, std::wstring_view patterns, IsDirectory isDirectory)
{
    Choice choice{};
    std::optional<size_t> firstFile;
    for (size_t index = 0; index < paths.size(); ++index) {
        if (paths[index].empty() || isDirectory(paths[index])) continue;
        if (!firstFile) firstFile = index;
        if (MatchesPatterns(paths[index], patterns)) { choice.open = index; break; }
    }
    if (!choice.open) choice.open = firstFile;
    choice.ignored = paths.size() - (choice.open ? 1u : 0u);
    return choice;
}

} // namespace dropped_files
