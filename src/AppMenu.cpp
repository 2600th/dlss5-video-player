#include "AppMenu.h"

#include "Localization.h"

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
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), play = CreatePopupMenu(), video = CreatePopupMenu(), dlss = CreatePopupMenu(), quality = CreatePopupMenu(), advanced = CreatePopupMenu();
    const auto add = [&](HMENU menu, UINT command, const wchar_t* key) {
        const std::wstring text = localizer.Get(key);
        AppendMenuW(menu, MF_STRING, command, text.c_str());
    };
    add(file, IDM_OPEN, L"menu.open");
    const std::wstring youtubeName = localizer.Get(L"menu.open_youtube");
    AppendMenuW(file, MF_STRING | (youtubeAvailable ? 0 : MF_GRAYED), IDM_OPEN_YOUTUBE, youtubeName.c_str());
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr); add(file, IDM_EXIT, L"menu.exit");
    add(play, IDM_PLAY, L"menu.playpause"); add(play, IDM_STOP, L"menu.stop"); add(play, IDM_BACK10, L"menu.back10"); add(play, IDM_FWD10, L"menu.forward10"); add(play, IDM_MUTE, L"menu.mute");
    add(video, IDM_ASPECT_FIT, L"menu.aspectfit"); add(video, IDM_ASPECT_FILL, L"menu.aspectfill"); add(video, IDM_VIDEO_ADJUSTMENTS, L"menu.adjustments"); AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
    add(video, IDM_VIEW_FINAL, L"menu.final"); add(video, IDM_VIEW_INPUT, L"menu.input"); add(video, IDM_VIEW_MV, L"menu.mv"); add(video, IDM_VIEW_DEPTH, L"menu.depth"); add(video, IDM_VIEW_MASK, L"menu.mask"); AppendMenuW(video, MF_SEPARATOR, 0, nullptr); add(video, IDM_FULLSCREEN, L"menu.fullscreen");
    add(quality, IDM_QUALITY_AUTO, L"menu.quality_auto"); AppendMenuW(quality, MF_STRING, IDM_QUALITY_QUALITY, L"Quality"); AppendMenuW(quality, MF_STRING, IDM_QUALITY_BALANCED, L"Balanced"); AppendMenuW(quality, MF_STRING, IDM_QUALITY_PERFORMANCE, L"Performance"); AppendMenuW(quality, MF_STRING, IDM_QUALITY_ULTRAPERF, L"Ultra Performance"); AppendMenuW(quality, MF_STRING, IDM_QUALITY_DLAA, L"DLAA");
    add(dlss, IDM_DLSS, L"menu.dlss_toggle"); add(dlss, IDM_DEPTH_MODE, L"menu.depthmode"); const std::wstring qualityName = localizer.Get(L"menu.quality"); AppendMenuW(dlss, MF_POPUP, reinterpret_cast<UINT_PTR>(quality), qualityName.c_str());
    add(advanced, IDM_ADVANCED_SAFE_MODE, L"menu.safe_mode"); AppendMenuW(advanced, MF_SEPARATOR, 0, nullptr); add(advanced, IDM_REHOOK, L"menu.rehook");
    const std::wstring fileName = localizer.Get(L"menu.file"), playName = localizer.Get(L"menu.playback"), videoName = localizer.Get(L"menu.video"), dlssName = localizer.Get(L"menu.dlss"), advancedName = localizer.Get(L"menu.advanced");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), fileName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(play), playName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(video), videoName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(dlss), dlssName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(advanced), advancedName.c_str());
    return bar;
}

bool RoutesToRehook(PlayerCommandRoute route, UINT value)
{
    return route == PlayerCommandRoute::KeyDown ? value == VK_F6 : value == IDM_REHOOK;
}

} // namespace app_menu
