#include "AppMenu.h"

#include "Localization.h"
#include "YouTubeResolver.h"

namespace {

HMENU find_menu_containing_command(HMENU menu, UINT command)
{
    if (!menu) return nullptr;
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &item)) continue;
        if (item.wID == command) return menu;
        if (HMENU nested = find_menu_containing_command(item.hSubMenu, command)) {
            return nested;
        }
    }
    return nullptr;
}

} // namespace

namespace app_menu {

HMENU CreateDebugViewMenu(UINT selectedCommand)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return nullptr;
    AppendMenuW(menu, MF_STRING, IDM_VIEW_FINAL, L"Final output\t1");
    AppendMenuW(menu, MF_STRING, IDM_VIEW_INPUT, L"DLSS input\t2");
    AppendMenuW(menu, MF_STRING, IDM_VIEW_MV, L"Motion vectors\t3");
    AppendMenuW(menu, MF_STRING, IDM_VIEW_DEPTH, L"Depth\t4");
    AppendMenuW(menu, MF_STRING, IDM_VIEW_MASK, L"Bias mask\t5");
    if (selectedCommand < IDM_VIEW_FINAL || selectedCommand > IDM_VIEW_MASK) {
        selectedCommand = IDM_VIEW_FINAL;
    }
    CheckMenuRadioItem(menu, IDM_VIEW_FINAL, IDM_VIEW_MASK, selectedCommand, MF_BYCOMMAND);
    return menu;
}

HMENU CreateMenuBar(const Localizer& localizer, bool youtubeAvailable)
{
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), examples = CreatePopupMenu(), games = CreatePopupMenu(), anime = CreatePopupMenu(), play = CreatePopupMenu(), video = CreatePopupMenu(), youtubeQuality = CreatePopupMenu(), dlss = CreatePopupMenu(), advanced = CreatePopupMenu();
    const auto add = [&](HMENU menu, UINT command, const wchar_t* key) {
        const std::wstring text = localizer.Get(key);
        AppendMenuW(menu, MF_STRING, command, text.c_str());
    };
    add(file, IDM_OPEN, L"menu.open");
    const std::wstring youtubeName = localizer.Get(L"menu.open_youtube");
    AppendMenuW(file, MF_STRING | (youtubeAvailable ? 0 : MF_GRAYED), IDM_OPEN_YOUTUBE, youtubeName.c_str());
    for (size_t index = 0; index < kExampleVideos.size(); ++index) {
        const ExampleVideo& example = kExampleVideos[index];
        HMENU category = example.category == ExampleVideoCategory::Games ? games : anime;
        AppendMenuW(category, MF_STRING | (youtubeAvailable ? 0 : MF_GRAYED),
                    IDM_EXAMPLE_VIDEO_FIRST + static_cast<UINT>(index), example.title.data());
    }
    const std::wstring gamesName = localizer.Get(L"menu.examples_games");
    const std::wstring animeName = localizer.Get(L"menu.examples_anime");
    const std::wstring examplesName = localizer.Get(L"menu.examples");
    AppendMenuW(examples, MF_POPUP, reinterpret_cast<UINT_PTR>(games), gamesName.c_str());
    AppendMenuW(examples, MF_POPUP, reinterpret_cast<UINT_PTR>(anime), animeName.c_str());
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(examples), examplesName.c_str());
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr); add(file, IDM_EXIT, L"menu.exit");
    add(play, IDM_PLAY, L"menu.playpause"); add(play, IDM_STOP, L"menu.stop"); add(play, IDM_BACK10, L"menu.back10"); add(play, IDM_FWD10, L"menu.forward10"); add(play, IDM_MUTE, L"menu.mute");
    add(youtubeQuality, IDM_YOUTUBE_QUALITY_AUTO, L"menu.youtube_quality_auto"); add(youtubeQuality, IDM_YOUTUBE_QUALITY_2160, L"menu.youtube_quality_2160"); add(youtubeQuality, IDM_YOUTUBE_QUALITY_1440, L"menu.youtube_quality_1440"); add(youtubeQuality, IDM_YOUTUBE_QUALITY_1080, L"menu.youtube_quality_1080"); CheckMenuRadioItem(youtubeQuality, IDM_YOUTUBE_QUALITY_AUTO, IDM_YOUTUBE_QUALITY_1080, IDM_YOUTUBE_QUALITY_AUTO, MF_BYCOMMAND);
    const std::wstring youtubeQualityName = localizer.Get(L"menu.youtube_quality"); AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(youtubeQuality), youtubeQualityName.c_str()); AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
    add(video, IDM_ASPECT_FIT, L"menu.aspectfit"); add(video, IDM_ASPECT_FILL, L"menu.aspectfill"); add(video, IDM_VIDEO_ADJUSTMENTS, L"menu.adjustments"); AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
    add(video, IDM_VIEW_FINAL, L"menu.final"); add(video, IDM_VIEW_INPUT, L"menu.input"); add(video, IDM_VIEW_MV, L"menu.mv"); add(video, IDM_VIEW_DEPTH, L"menu.depth"); add(video, IDM_VIEW_MASK, L"menu.mask"); AppendMenuW(video, MF_SEPARATOR, 0, nullptr); add(video, IDM_FULLSCREEN, L"menu.fullscreen");
    add(dlss, IDM_NEURAL_RENDERING, L"menu.neural_rendering");
    add(dlss, IDM_DLSS_UPSCALING, L"menu.dlss_upscaling");
    HMENU upscaleOutput=CreatePopupMenu();
    AppendMenuW(upscaleOutput,MF_STRING|MF_CHECKED,IDM_UPSCALE_1440,L"1440p (default)");
    AppendMenuW(upscaleOutput,MF_STRING,IDM_UPSCALE_2160,L"2160p (4K)");
    AppendMenuW(dlss,MF_POPUP,reinterpret_cast<UINT_PTR>(upscaleOutput),L"Upscaling output");
    add(dlss, IDM_FRAME_GENERATION, L"menu.frame_generation_unavailable");
    add(advanced, IDM_CLEAR_NEURAL_CACHE, L"menu.clear_neural_cache");
    AppendMenuW(advanced, MF_SEPARATOR, 0, nullptr);
    add(advanced, IDM_DEPTH_MODE, L"menu.depthmode");
    add(advanced, IDM_ADVANCED_SAFE_MODE, L"menu.safe_mode"); AppendMenuW(advanced, MF_SEPARATOR, 0, nullptr); add(advanced, IDM_REHOOK, L"menu.rehook");
    const std::wstring fileName = localizer.Get(L"menu.file"), playName = localizer.Get(L"menu.playback"), videoName = localizer.Get(L"menu.video"), dlssName = localizer.Get(L"menu.dlss"), advancedName = localizer.Get(L"menu.advanced");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), fileName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(play), playName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(video), videoName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(dlss), dlssName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(advanced), advancedName.c_str());
    UpdateFeatureAvailability(bar, true, false, false, false, false, false, false);
    return bar;
}

bool RoutesToRehook(PlayerCommandRoute route, UINT value)
{
    return route == PlayerCommandRoute::KeyDown ? value == VK_F6 : value == IDM_REHOOK;
}

bool RoutesToOpenYouTube(PlayerCommandRoute route, UINT value, bool controlDown)
{
    if (route == PlayerCommandRoute::NativeMenu) return value == IDM_OPEN_YOUTUBE;
    return controlDown && value == 'L';
}

const ExampleVideo* ExampleVideoForCommand(UINT command)
{
    if (command < IDM_EXAMPLE_VIDEO_FIRST) return nullptr;
    const size_t index = static_cast<size_t>(command - IDM_EXAMPLE_VIDEO_FIRST);
    if (index >= kExampleVideos.size()) return nullptr;
    return &kExampleVideos[index];
}

bool UpdateSourceActionAvailability(HMENU menuBar, bool openEnabled,
                                    bool youtubeEnabled)
{
    if (!menuBar) return false;
    HMENU fileMenu = GetSubMenu(menuBar, 0);
    if (!fileMenu) return false;
    const UINT openState = EnableMenuItem(
        fileMenu, IDM_OPEN,
        MF_BYCOMMAND | (openEnabled ? MF_ENABLED : MF_GRAYED));
    const UINT youtubeState = EnableMenuItem(
        fileMenu, IDM_OPEN_YOUTUBE,
        MF_BYCOMMAND | (youtubeEnabled ? MF_ENABLED : MF_GRAYED));
    bool examplesUpdated = true;
    for (size_t index = 0; index < kExampleVideos.size(); ++index) {
        const UINT state = EnableMenuItem(
            fileMenu, IDM_EXAMPLE_VIDEO_FIRST + static_cast<UINT>(index),
            MF_BYCOMMAND | (youtubeEnabled ? MF_ENABLED : MF_GRAYED));
        examplesUpdated = examplesUpdated && state != static_cast<UINT>(-1);
    }
    return openState != static_cast<UINT>(-1) &&
           youtubeState != static_cast<UINT>(-1) && examplesUpdated;
}

std::optional<YouTubeSourceQuality> YouTubeQualityForCommand(UINT command)
{
    switch (command) {
    case IDM_YOUTUBE_QUALITY_AUTO: return YouTubeSourceQuality::Auto;
    case IDM_YOUTUBE_QUALITY_2160: return YouTubeSourceQuality::P2160;
    case IDM_YOUTUBE_QUALITY_1440: return YouTubeSourceQuality::P1440;
    case IDM_YOUTUBE_QUALITY_1080: return YouTubeSourceQuality::P1080;
    default: return std::nullopt;
    }
}

UINT CommandForYouTubeQuality(YouTubeSourceQuality quality)
{
    switch (quality) {
    case YouTubeSourceQuality::Auto: return IDM_YOUTUBE_QUALITY_AUTO;
    case YouTubeSourceQuality::P2160: return IDM_YOUTUBE_QUALITY_2160;
    case YouTubeSourceQuality::P1440: return IDM_YOUTUBE_QUALITY_1440;
    case YouTubeSourceQuality::P1080: return IDM_YOUTUBE_QUALITY_1080;
    }
    return IDM_YOUTUBE_QUALITY_AUTO;
}

bool UpdateYouTubeQualitySelection(HMENU menuBar, YouTubeSourceQuality quality)
{
    HMENU qualityMenu = find_menu_containing_command(menuBar, IDM_YOUTUBE_QUALITY_AUTO);
    if (!qualityMenu) return false;
    return CheckMenuRadioItem(qualityMenu, IDM_YOUTUBE_QUALITY_AUTO,
                              IDM_YOUTUBE_QUALITY_1080,
                              CommandForYouTubeQuality(quality),
                              MF_BYCOMMAND) != FALSE;
}

bool UpdateFeatureAvailability(HMENU menuBar, bool neuralRequested,
                               bool neuralAvailable, bool neuralActive,
                               bool upscalingAvailable, bool upscalingActive,
                               bool frameGenerationAvailable, bool frameGenerationActive)
{
    const auto update = [&](UINT command, bool available, bool checked) {
        const HMENU menu = find_menu_containing_command(menuBar, command);
        if (!menu) return false;
        const UINT enabled = EnableMenuItem(menu, command,
            MF_BYCOMMAND | (available ? MF_ENABLED : MF_GRAYED));
        const DWORD marked = CheckMenuItem(menu, command,
            MF_BYCOMMAND | ((available ? checked : command == IDM_NEURAL_RENDERING && neuralRequested)
                ? MF_CHECKED : MF_UNCHECKED));
        return enabled != static_cast<UINT>(-1) && marked != static_cast<DWORD>(-1);
    };
    return update(IDM_NEURAL_RENDERING, neuralAvailable, neuralActive) &&
           update(IDM_DLSS_UPSCALING, upscalingAvailable, upscalingActive) &&
           update(IDM_FRAME_GENERATION, frameGenerationAvailable, frameGenerationActive);
}

} // namespace app_menu
