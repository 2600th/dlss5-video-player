#pragma once

#include <array>
#include <string_view>

enum class ExampleVideoCategory {
    Games,
    Anime,
};

struct ExampleVideo {
    ExampleVideoCategory category;
    std::wstring_view title;
    std::wstring_view channel;
    std::wstring_view url;
};

inline constexpr std::array kExampleVideos{
    ExampleVideo{ExampleVideoCategory::Games, L"Grand Theft Auto V: Official Gameplay Video", L"Rockstar Games", L"https://www.youtube.com/watch?v=N-xHcvug3WI"},
    ExampleVideo{ExampleVideoCategory::Games, L"Resident Evil Requiem - Launch Trailer", L"Resident Evil", L"https://www.youtube.com/watch?v=9lrThxCoznw"},
    ExampleVideo{ExampleVideoCategory::Games, L"Battlefield 6 Official Multiplayer Gameplay Trailer", L"Battlefield", L"https://www.youtube.com/watch?v=wFGEMfyAQtI"},
    ExampleVideo{ExampleVideoCategory::Anime, L"The Apothecary Diaries | OFFICIAL TRAILER", L"Crunchyroll", L"https://www.youtube.com/watch?v=XYNGkSvFT8c"},
    ExampleVideo{ExampleVideoCategory::Anime, L"Tomb Raider King | Official Trailer", L"Crunchyroll", L"https://www.youtube.com/watch?v=vwQY5heVraU"},
    ExampleVideo{ExampleVideoCategory::Anime, L"My Hero Academia FINAL SEASON \"More\" Official Trailer", L"Crunchyroll", L"https://www.youtube.com/watch?v=pxbEWUjh6E4"},
};
