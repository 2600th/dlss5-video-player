#pragma once

#include <array>
#include <string_view>

struct ExampleVideo {
    std::wstring_view title;
    std::wstring_view channel;
    std::wstring_view url;
};

inline constexpr std::array kExampleVideos{
    // Editorial showcase, checked 2026-09-02; see docs/EXAMPLE_VIDEOS.md.
    ExampleVideo{L"GTA VI - Trailer 2", L"Rockstar Games", L"https://www.youtube.com/watch?v=VQRLujxTm3c"},
    ExampleVideo{L"Marvel's Wolverine - Extended Gameplay", L"Marvel Entertainment", L"https://www.youtube.com/watch?v=_U56cQFx_Vw"},
    ExampleVideo{L"Gears of War: E-Day - Gameplay Reveal", L"XBOX", L"https://www.youtube.com/watch?v=dFk4bL3a8I8"},
    ExampleVideo{L"Fable - Showcase 2026", L"XBOX", L"https://www.youtube.com/watch?v=3iW1i78zFvk"},
    ExampleVideo{L"The Witcher 4 - Cinematic Reveal", L"The Witcher", L"https://www.youtube.com/watch?v=54dabgZJ5YA"},
};
