#pragma once

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "DroppedFilesPolicy.h"

// The next or previous file in the playing video's folder (P2.52), as mpv's
// autocreate-playlist and MPC-HC's PgDn and PgUp do it: the folder's media
// files in the order Explorer lists them by name, the one playing among them,
// and one step either way. Media is what the open dialog lists
// (dropped_files::kMediaPatterns), so a subtitle, a thumbnail or a text file
// beside the video is passed over. There is no wrap: the last file's next is
// nothing, and the player says it is the last.
namespace folder_playlist {

namespace detail {

inline bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

} // namespace detail

// Explorer's name order (StrCmpLogicalW's idea of it): runs of digits compare
// as numbers, so "Episode 2" comes before "Episode 10", and everything else
// compares case-insensitively. Negative, zero or positive, as wcscmp.
inline int CompareNatural(std::wstring_view a, std::wstring_view b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (detail::IsDigit(a[i]) && detail::IsDigit(b[j])) {
            // Leading zeros carry no value; the longer run of what is left is
            // the larger number, and equal lengths compare digit by digit.
            size_t endA = i, endB = j;
            while (endA < a.size() && detail::IsDigit(a[endA])) ++endA;
            while (endB < b.size() && detail::IsDigit(b[endB])) ++endB;
            size_t startA = i, startB = j;
            while (startA + 1 < endA && a[startA] == L'0') ++startA;
            while (startB + 1 < endB && b[startB] == L'0') ++startB;
            const size_t lengthA = endA - startA, lengthB = endB - startB;
            if (lengthA != lengthB) return lengthA < lengthB ? -1 : 1;
            for (size_t k = 0; k < lengthA; ++k)
                if (a[startA + k] != b[startB + k]) return a[startA + k] < b[startB + k] ? -1 : 1;
            // "007" after "7", so equal numbers still have one order.
            if (endA - i != endB - j) return endA - i < endB - j ? -1 : 1;
            i = endA;
            j = endB;
            continue;
        }
        const wchar_t x = static_cast<wchar_t>(std::towlower(a[i])), y = static_cast<wchar_t>(std::towlower(b[j]));
        if (x != y) return x < y ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    // Names that differ only in case still have one order.
    return a.compare(b) < 0 ? -1 : (a == b ? 0 : 1);
}

inline bool SameName(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
        return std::towlower(x) == std::towlower(y);
    });
}

// The media files of a folder in play order: the names `patterns` admits, and
// `current` whatever its extension, since it is already playing.
inline std::vector<std::wstring> Ordered(std::wstring_view current, std::span<const std::wstring> names,
                                         std::wstring_view patterns)
{
    std::vector<std::wstring> media;
    for (const std::wstring& name : names)
        if (dropped_files::MatchesPatterns(name, patterns) || SameName(name, current)) media.push_back(name);
    std::sort(media.begin(), media.end(),
              [](const std::wstring& a, const std::wstring& b) { return CompareNatural(a, b) < 0; });
    return media;
}

// The file one step from `current` (+1 next, -1 previous) among `names`, the
// file names in its folder; nothing at either end. A `current` that is no
// longer in the folder still has neighbours: the names either side of where
// it would sort.
inline std::optional<std::wstring> Neighbour(std::wstring_view current, std::span<const std::wstring> names,
                                             std::wstring_view patterns, int direction)
{
    if (direction == 0 || current.empty()) return std::nullopt;
    const std::vector<std::wstring> media = Ordered(current, names, patterns);
    const auto here = std::find_if(media.begin(), media.end(), [&](const std::wstring& name) { return SameName(name, current); });
    if (here != media.end()) {
        const ptrdiff_t at = std::distance(media.begin(), here) + (direction > 0 ? 1 : -1);
        if (at < 0 || at >= ptrdiff_t(media.size())) return std::nullopt;
        return media[size_t(at)];
    }
    if (direction > 0) {
        for (const std::wstring& name : media)
            if (CompareNatural(name, current) > 0) return name;
        return std::nullopt;
    }
    for (auto it = media.rbegin(); it != media.rend(); ++it)
        if (CompareNatural(*it, current) < 0) return *it;
    return std::nullopt;
}

} // namespace folder_playlist
