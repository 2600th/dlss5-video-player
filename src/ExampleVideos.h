#pragma once

#include <array>
#include <string_view>

struct ExampleVideo {
    std::wstring_view title;
    std::wstring_view channel;
    std::wstring_view url;
};

inline constexpr std::array kExampleVideos{
    // Official in-engine trailers under three minutes, chosen for close-up
    // faces, skin, hair and lighting - where DLSS 5 shows most - with little
    // on-screen text. Re-checked 2026-09-24 with the bundled yt-dlp and the
    // player's own flags: every one is age_limit 0 and public, so an anonymous
    // session gets the whole ladder to 2160p rather than one 640x360 format.
    // The start screen shows the first few, so the strongest go first, and a
    // tile's title line holds about 22 characters, so a title is the game's
    // name; GTA VI's keeps "Trailer 2", the name it is known by.
    // Evidence and the rejected candidates are in docs/EXAMPLE_VIDEOS.md.
    ExampleVideo{L"GTA VI - Trailer 2", L"Rockstar Games", L"https://www.youtube.com/watch?v=VQRLujxTm3c"},
    ExampleVideo{L"Resident Evil Requiem", L"Resident Evil", L"https://www.youtube.com/watch?v=0wFNN1f6hF8"},
    ExampleVideo{L"007 First Light", L"PlayStation", L"https://www.youtube.com/watch?v=trvIyyFt_MM"},
    ExampleVideo{L"Tomb Raider: Legacy of Atlantis", L"PlayStation", L"https://www.youtube.com/watch?v=1lJBNZ1Dk6k"},
    ExampleVideo{L"Assassin's Creed Shadows", L"Assassin's Creed", L"https://www.youtube.com/watch?v=SHaN4MIGqpo"},
    ExampleVideo{L"Mafia: The Old Country", L"Mafia Game", L"https://www.youtube.com/watch?v=EAEYZDgHNv8"},
    ExampleVideo{L"Kingdom Come: Deliverance II", L"Warhorse Studios", L"https://www.youtube.com/watch?v=vPS-pgg3scE"},
};
