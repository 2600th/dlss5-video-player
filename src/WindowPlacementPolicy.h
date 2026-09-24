#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <span>
#include <string>

// Where the player's window was when it closed, and whether it can go back
// there. A saved placement wins over the first-run size (InitialWindowPolicy),
// but only onto a screen that still exists: a rectangle left on a monitor that
// has since been unplugged, or a resolution that shrank, would open the player
// where nobody can see or grab it. Such a placement is dropped and the first-run
// rule places the window instead.
namespace window_placement {

struct Saved {
    RECT normal{};      // the restored (not maximised) frame, in screen pixels
    bool maximized{};
};

// "left,top,right,bottom,maximized" - one ini value, parsed strictly.
inline std::wstring Format(const Saved& saved)
{
    wchar_t text[96]{};
    swprintf_s(text, L"%ld,%ld,%ld,%ld,%d", saved.normal.left, saved.normal.top, saved.normal.right,
               saved.normal.bottom, saved.maximized ? 1 : 0);
    return text;
}

inline std::optional<Saved> Parse(const std::wstring& text)
{
    long values[5]{};
    wchar_t tail = 0;
    if (swscanf_s(text.c_str(), L"%ld,%ld,%ld,%ld,%ld%lc", &values[0], &values[1], &values[2], &values[3],
                  &values[4], &tail, 1) != 5)
        return std::nullopt;
    if (values[4] != 0 && values[4] != 1) return std::nullopt;
    Saved saved{RECT{values[0], values[1], values[2], values[3]}, values[4] == 1};
    if (saved.normal.right <= saved.normal.left || saved.normal.bottom <= saved.normal.top) return std::nullopt;
    return saved;
}

inline long long Area(const RECT& r)
{
    return r.right > r.left && r.bottom > r.top ? static_cast<long long>(r.right - r.left) * (r.bottom - r.top) : 0;
}

// Usable when one work area holds at least half of the frame and its caption:
// the top 32 px, across at least 120 px, so the window can be grabbed and
// moved. The frame must also be at least the player's minimum size.
inline bool Usable(const Saved& saved, std::span<const RECT> workAreas, SIZE minimum)
{
    const RECT& r = saved.normal;
    if (r.right - r.left < minimum.cx || r.bottom - r.top < minimum.cy) return false;
    const RECT caption{r.left, r.top, r.right, r.top + 32};
    for (const RECT& work : workAreas) {
        RECT overlap{};
        const bool body = IntersectRect(&overlap, &r, &work) && Area(overlap) * 2 >= Area(r);
        RECT grab{};
        const bool title = IntersectRect(&grab, &caption, &work) && grab.right - grab.left >= 120;
        if (body && title) return true;
    }
    return false;
}

} // namespace window_placement
