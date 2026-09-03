#pragma once

#include <array>
#include <string_view>

struct ExampleVideo {
    std::wstring_view title;
    std::wstring_view channel;
    std::wstring_view url;
};

inline constexpr std::array kExampleVideos{
    // Human-focused trailers under three minutes, checked 2026-09-03; see docs/EXAMPLE_VIDEOS.md.
    ExampleVideo{L"Hellblade II - Launch Trailer", L"XBOX", L"https://www.youtube.com/watch?v=PRbOmIcVXak"},
    ExampleVideo{L"The Last of Us Part II Remastered - PC Launch Trailer", L"PlayStation", L"https://www.youtube.com/watch?v=Tg1oRHd5zlw"},
    ExampleVideo{L"Mafia: The Old Country - Family Takes Sacrifice", L"Mafia Game", L"https://www.youtube.com/watch?v=EAEYZDgHNv8"},
    ExampleVideo{L"Cyberpunk 2077: Phantom Liberty - Launch Trailer", L"Cyberpunk 2077", L"https://www.youtube.com/watch?v=kfX9n_G0N2Y"},
    ExampleVideo{L"GTA VI - Trailer 2", L"Rockstar Games", L"https://www.youtube.com/watch?v=VQRLujxTm3c"},
    ExampleVideo{L"Death Stranding 2 - Accolades Trailer", L"PlayStation", L"https://www.youtube.com/watch?v=od0ULrKzylQ"},
};
