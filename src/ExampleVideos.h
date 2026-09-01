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
    ExampleVideo{ExampleVideoCategory::Games, L"GTA VI Trailer 2", L"Rockstar Games", L"https://www.youtube.com/watch?v=VQRLujxTm3c"},
    ExampleVideo{ExampleVideoCategory::Games, L"Resident Evil Requiem - Launch Trailer", L"Resident Evil", L"https://www.youtube.com/watch?v=9lrThxCoznw"},
    ExampleVideo{ExampleVideoCategory::Games, L"Battlefield 6 Season 3 Official Gameplay Trailer", L"Battlefield", L"https://www.youtube.com/watch?v=XCMr55EjFew"},
    ExampleVideo{ExampleVideoCategory::Anime, L"2026 Summer Anime Season Trailer", L"Crunchyroll", L"https://www.youtube.com/watch?v=DWM2IfkzLHo"},
    ExampleVideo{ExampleVideoCategory::Anime, L"Spring 2026 Season Official Trailer", L"Crunchyroll", L"https://www.youtube.com/watch?v=7Wc6ugY3meg"},
    ExampleVideo{ExampleVideoCategory::Anime, L"My Hero Academia FINAL SEASON \"More\" Official Trailer", L"Crunchyroll", L"https://www.youtube.com/watch?v=pxbEWUjh6E4"},
};
