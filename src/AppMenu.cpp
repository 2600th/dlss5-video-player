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
    if (selectedCommand < IDM_VIEW_FINAL || selectedCommand > IDM_VIEW_DEPTH) {
        selectedCommand = IDM_VIEW_FINAL;
    }
    CheckMenuRadioItem(menu, IDM_VIEW_FINAL, IDM_VIEW_DEPTH, selectedCommand, MF_BYCOMMAND);
    return menu;
}

HMENU CreateMenuBar(const Localizer& localizer, bool youtubeAvailable)
{
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), examples = CreatePopupMenu(), recent = CreatePopupMenu(), play = CreatePopupMenu(), video = CreatePopupMenu(), youtubeQuality = CreatePopupMenu(), compare = CreatePopupMenu(), dlss = CreatePopupMenu(), convert = CreatePopupMenu(), advanced = CreatePopupMenu();
    const auto add = [&](HMENU menu, UINT command, const wchar_t* key) {
        const std::wstring text = localizer.Get(key);
        AppendMenuW(menu, MF_STRING, command, text.c_str());
    };
    add(file, IDM_OPEN, L"menu.open");
    const std::wstring youtubeName = localizer.Get(L"menu.open_youtube");
    AppendMenuW(file, MF_STRING | (youtubeAvailable ? 0 : MF_GRAYED), IDM_OPEN_YOUTUBE, youtubeName.c_str());
    for (size_t index = 0; index < kExampleVideos.size(); ++index) {
        const ExampleVideo& example = kExampleVideos[index];
        AppendMenuW(examples, MF_STRING | (youtubeAvailable ? 0 : MF_GRAYED),
                    IDM_EXAMPLE_VIDEO_FIRST + static_cast<UINT>(index), example.title.data());
    }
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(examples), L"Game trailers");
    AppendMenuW(recent, MF_STRING | MF_GRAYED, IDM_RECENT_VIDEO_FIRST, L"No recent videos");
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(recent), L"Recent videos");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr); add(file, IDM_EXIT, L"menu.exit");
    add(play, IDM_PLAY, L"menu.playpause"); add(play, IDM_STOP, L"menu.stop"); add(play, IDM_BACK10, L"menu.back10"); add(play, IDM_FWD10, L"menu.forward10"); add(play, IDM_MUTE, L"menu.mute"); AppendMenuW(play, MF_SEPARATOR, 0, nullptr);
    add(play, IDM_MARK_IN, L"menu.mark_in"); add(play, IDM_MARK_OUT, L"menu.mark_out"); add(play, IDM_CLEAR_MARKS, L"menu.clear_marks"); add(play, IDM_GOTO_TIMECODE, L"menu.goto_timecode"); AppendMenuW(play, MF_SEPARATOR, 0, nullptr); add(play, IDM_PAUSE_NEURAL_RENDER, L"menu.pause_neural_render");
    add(youtubeQuality, IDM_YOUTUBE_QUALITY_AUTO, L"menu.youtube_quality_auto"); add(youtubeQuality, IDM_YOUTUBE_QUALITY_2160, L"menu.youtube_quality_2160"); add(youtubeQuality, IDM_YOUTUBE_QUALITY_1440, L"menu.youtube_quality_1440"); add(youtubeQuality, IDM_YOUTUBE_QUALITY_1080, L"menu.youtube_quality_1080"); CheckMenuRadioItem(youtubeQuality, IDM_YOUTUBE_QUALITY_AUTO, IDM_YOUTUBE_QUALITY_1080, IDM_YOUTUBE_QUALITY_AUTO, MF_BYCOMMAND);
    const std::wstring youtubeQualityName = localizer.Get(L"menu.youtube_quality"); AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(youtubeQuality), youtubeQualityName.c_str()); AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
    add(video, IDM_ASPECT_FIT, L"menu.aspectfit"); add(video, IDM_ASPECT_FILL, L"menu.aspectfill"); add(video, IDM_VIDEO_ADJUSTMENTS, L"menu.adjustments");
    add(compare, IDM_COMPARE_NEURAL, L"menu.compare_neural"); add(compare, IDM_COMPARE_BLEND, L"menu.compare_blend"); add(compare, IDM_COMPARE_SPLIT, L"menu.compare_split"); add(compare, IDM_COMPARE_WIPE, L"menu.compare_wipe"); AppendMenuW(compare, MF_SEPARATOR, 0, nullptr);
    add(compare, IDM_COMPARE_BLEND_LESS, L"menu.compare_blend_less"); add(compare, IDM_COMPARE_BLEND_MORE, L"menu.compare_blend_more"); AppendMenuW(compare, MF_SEPARATOR, 0, nullptr); add(compare, IDM_COMPARE_ZOOM, L"menu.compare_zoom");
    const std::wstring compareName = localizer.Get(L"menu.compare"); AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(compare), compareName.c_str()); AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
    add(video, IDM_VIEW_FINAL, L"menu.final"); add(video, IDM_VIEW_INPUT, L"menu.input"); add(video, IDM_VIEW_MV, L"menu.mv"); add(video, IDM_VIEW_DEPTH, L"menu.depth"); AppendMenuW(video, MF_SEPARATOR, 0, nullptr); add(video, IDM_FULLSCREEN, L"menu.fullscreen");
    add(dlss, IDM_NEURAL_RENDERING, L"menu.neural_rendering");
    add(dlss, IDM_DLSS_UPSCALING, L"menu.dlss_upscaling");
    HMENU upscaleOutput=CreatePopupMenu();
    add(upscaleOutput, IDM_UPSCALE_AUTO, L"menu.upscale_auto");
    add(upscaleOutput, IDM_UPSCALE_1080, L"menu.upscale_1080");
    add(upscaleOutput, IDM_UPSCALE_1440, L"menu.upscale_1440");
    add(upscaleOutput, IDM_UPSCALE_2160, L"menu.upscale_2160");
    CheckMenuRadioItem(upscaleOutput,IDM_UPSCALE_AUTO,IDM_UPSCALE_2160,IDM_UPSCALE_AUTO,MF_BYCOMMAND);
    AppendMenuW(dlss,MF_POPUP,reinterpret_cast<UINT_PTR>(upscaleOutput),localizer.Get(L"menu.upscale_output").c_str());
    add(dlss, IDM_FRAME_GENERATION, L"menu.frame_generation");
    add(dlss, IDM_CANCEL_FRAME_GENERATION, L"menu.cancel_frame_generation");
    // The multiple is a preference, not an automatic maximum: a conversion is
    // minutes of GPU work and a large file, so the default doubles the rate and
    // anything beyond that is asked for. "As many as the display allows" is the
    // old behaviour, kept for whoever wants it.
    HMENU framegenMultiple=CreatePopupMenu();
    add(framegenMultiple, IDM_FRAMEGEN_2X, L"menu.framegen_2x");
    add(framegenMultiple, IDM_FRAMEGEN_3X, L"menu.framegen_3x");
    add(framegenMultiple, IDM_FRAMEGEN_4X, L"menu.framegen_4x");
    add(framegenMultiple, IDM_FRAMEGEN_5X, L"menu.framegen_5x");
    add(framegenMultiple, IDM_FRAMEGEN_MAX, L"menu.framegen_max");
    CheckMenuRadioItem(framegenMultiple,IDM_FRAMEGEN_2X,IDM_FRAMEGEN_MAX,IDM_FRAMEGEN_2X,MF_BYCOMMAND);
    AppendMenuW(dlss,MF_POPUP,reinterpret_cast<UINT_PTR>(framegenMultiple),
                localizer.Get(L"menu.framegen_multiple").c_str());
    // Grayed on creation like IDM_CANCEL_EXPORT below: main.cpp enables it once
    // a converted file exists. Without it the output is unreachable after the
    // confirmation dialog closes - the export item beside it is gated on a
    // neural cache path a frame-generation output never has.
    AppendMenuW(dlss, MF_STRING | MF_GRAYED, IDM_SHOW_FRAMEGEN_OUTPUT,
                localizer.Get(L"menu.show_framegen_output").c_str());
    AppendMenuW(dlss, MF_SEPARATOR, 0, nullptr);
    add(dlss, IDM_PREVIEW_FRAME, L"menu.preview_frame"); add(dlss, IDM_PREVIEW_CLIP, L"menu.preview_clip"); AppendMenuW(dlss, MF_SEPARATOR, 0, nullptr);
    // Conversion writes a neural video to disk with the settings in the neural
    // settings dialog; it is deliberately separate from watching with the
    // rendering turned on.
    add(convert, IDM_RENDER_RANGE, L"menu.render_range"); add(convert, IDM_RENDER_WHOLE, L"menu.render_whole"); AppendMenuW(convert, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(convert, MF_STRING | MF_GRAYED, IDM_EXPORT_CACHED_VIDEO, localizer.Get(L"menu.export_cached").c_str());
    AppendMenuW(convert, MF_STRING | MF_GRAYED, IDM_CANCEL_EXPORT, localizer.Get(L"menu.cancel_export").c_str());
    const std::wstring convertName = localizer.Get(L"menu.convert");
    AppendMenuW(dlss, MF_POPUP, reinterpret_cast<UINT_PTR>(convert), convertName.c_str()); AppendMenuW(dlss, MF_SEPARATOR, 0, nullptr);
    add(dlss, IDM_NEURAL_SETTINGS, L"menu.neural_settings");
    add(dlss, IDM_ENCODER_SETTINGS, L"menu.encoder_settings");
    add(advanced, IDM_CLEAR_NEURAL_CACHE, L"menu.clear_neural_cache");
    add(advanced, IDM_OPEN_RENDER_RECEIPT, L"menu.open_receipt");
    AppendMenuW(advanced, MF_SEPARATOR, 0, nullptr);
    add(advanced, IDM_ADVANCED_SAFE_MODE, L"menu.safe_mode"); AppendMenuW(advanced, MF_SEPARATOR, 0, nullptr); add(advanced, IDM_REHOOK, L"menu.rehook");
    AppendMenuW(advanced, MF_SEPARATOR, 0, nullptr); add(advanced, IDM_CHECK_FOR_UPDATES, L"menu.check_updates");
    const std::wstring fileName = localizer.Get(L"menu.file"), playName = localizer.Get(L"menu.playback"), videoName = localizer.Get(L"menu.video"), dlssName = localizer.Get(L"menu.dlss"), advancedName = localizer.Get(L"menu.advanced");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), fileName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(play), playName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(video), videoName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(dlss), dlssName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(advanced), advancedName.c_str());
    UpdateFeatureAvailability(bar, true, false, false, false, false, false, false);
    UpdateRenderActionAvailability(bar, false, false, false, false, false);
    UpdateComparisonMenu(bar, false, false, IDM_COMPARE_NEURAL, false);
    return bar;
}

bool SetUpdateBadge(HMENU menuBar, std::wstring_view label)
{
    if (!menuBar) return false;
    int existing = -1;
    const int count = GetMenuItemCount(menuBar);
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_ID;
        if (GetMenuItemInfoW(menuBar, static_cast<UINT>(index), TRUE, &item) && item.wID == IDM_UPDATE_AVAILABLE) {
            existing = index;
            break;
        }
    }
    if (label.empty()) {
        return existing < 0 || DeleteMenu(menuBar, static_cast<UINT>(existing), MF_BYPOSITION) != FALSE;
    }
    // A bare '&' would silently become a mnemonic underline in a version label.
    std::wstring text;
    for (const wchar_t character : label.substr(0, 64)) {
        if (character == L'&') text.push_back(L'&');
        text.push_back(character < L' ' ? L' ' : character);
    }
    const UINT flags = MF_STRING | MF_RIGHTJUSTIFY;
    if (existing >= 0)
        return ModifyMenuW(menuBar, static_cast<UINT>(existing), flags | MF_BYPOSITION, IDM_UPDATE_AVAILABLE,
                           text.c_str()) != FALSE;
    return AppendMenuW(menuBar, flags, IDM_UPDATE_AVAILABLE, text.c_str()) != FALSE;
}

void UpdateRecentVideos(HMENU menuBar, std::span<const std::wstring> titles, bool enabled)
{
    HMENU recent=find_menu_containing_command(menuBar,IDM_RECENT_VIDEO_FIRST);
    if(!recent)return;
    while(GetMenuItemCount(recent)>0)DeleteMenu(recent,0,MF_BYPOSITION);
    if(titles.empty())AppendMenuW(recent,MF_STRING|MF_GRAYED,IDM_RECENT_VIDEO_FIRST,L"No recent videos");
    for(size_t index=0;index<titles.size()&&index<5;++index){
        std::wstring label=std::to_wstring(index+1)+L". ";
        for(wchar_t c:titles[index].substr(0,120)){
            if(c==L'&')label+=L'&';
            label+=(c<L' '?L' ':c);
        }
        AppendMenuW(recent,MF_STRING|(enabled?MF_ENABLED:MF_GRAYED),IDM_RECENT_VIDEO_FIRST+static_cast<UINT>(index),label.c_str());
    }
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
                               bool frameGenerationAvailable, bool /*frameGenerationActive*/)
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
    // Frame generation gets its enable state and no checkmark: a check states a
    // persistent mode, and this item is a one-shot action that starts a
    // minutes-long conversion - the cancel item beside it is what reports that
    // a conversion is running. The two real toggles keep their checkmarks.
    const HMENU frameGeneration = find_menu_containing_command(menuBar, IDM_FRAME_GENERATION);
    return update(IDM_NEURAL_RENDERING, neuralAvailable, neuralActive) &&
           update(IDM_DLSS_UPSCALING, upscalingAvailable, upscalingActive) &&
           frameGeneration &&
           EnableMenuItem(frameGeneration, IDM_FRAME_GENERATION,
               MF_BYCOMMAND | (frameGenerationAvailable ? MF_ENABLED : MF_GRAYED)) !=
               static_cast<UINT>(-1);
}

bool UpdateRenderActionAvailability(HMENU menuBar, bool markersAvailable, bool rangeRenderAvailable,
                                    bool jobActive, bool jobPaused, bool receiptAvailable)
{
    const auto enable = [&](UINT command, bool available) {
        const HMENU menu = find_menu_containing_command(menuBar, command);
        return menu && EnableMenuItem(menu, command, MF_BYCOMMAND | (available ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1);
    };
    bool ok = true;
    for (const UINT command : {IDM_MARK_IN, IDM_MARK_OUT, IDM_CLEAR_MARKS, IDM_GOTO_TIMECODE}) ok = enable(command, markersAvailable) && ok;
    for (const UINT command : {IDM_PREVIEW_FRAME, IDM_PREVIEW_CLIP, IDM_RENDER_RANGE, IDM_RENDER_WHOLE}) ok = enable(command, rangeRenderAvailable) && ok;
    ok = enable(IDM_PAUSE_NEURAL_RENDER, jobActive) && ok;
    const HMENU pauseMenu = find_menu_containing_command(menuBar, IDM_PAUSE_NEURAL_RENDER);
    ok = pauseMenu && CheckMenuItem(pauseMenu, IDM_PAUSE_NEURAL_RENDER, MF_BYCOMMAND | (jobActive && jobPaused ? MF_CHECKED : MF_UNCHECKED)) != static_cast<DWORD>(-1) && ok;
    return enable(IDM_OPEN_RENDER_RECEIPT, receiptAvailable) && ok;
}

bool UpdateComparisonMenu(HMENU menuBar, bool modesAvailable, bool zoomAvailable,
                          UINT selectedMode, bool zoomed)
{
    const HMENU menu = find_menu_containing_command(menuBar, IDM_COMPARE_NEURAL);
    if (!menu) return false;
    if (selectedMode < IDM_COMPARE_NEURAL || selectedMode > IDM_COMPARE_WIPE) selectedMode = IDM_COMPARE_NEURAL;
    bool ok = true;
    for (const UINT command : {IDM_COMPARE_NEURAL, IDM_COMPARE_BLEND, IDM_COMPARE_SPLIT, IDM_COMPARE_WIPE, IDM_COMPARE_BLEND_LESS, IDM_COMPARE_BLEND_MORE})
        ok = EnableMenuItem(menu, command, MF_BYCOMMAND | (modesAvailable ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1) && ok;
    ok = CheckMenuRadioItem(menu, IDM_COMPARE_NEURAL, IDM_COMPARE_WIPE, selectedMode, MF_BYCOMMAND) && ok;
    ok = EnableMenuItem(menu, IDM_COMPARE_ZOOM, MF_BYCOMMAND | (zoomAvailable ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1) && ok;
    return CheckMenuItem(menu, IDM_COMPARE_ZOOM, MF_BYCOMMAND | (zoomed ? MF_CHECKED : MF_UNCHECKED)) != static_cast<DWORD>(-1) && ok;
}

std::optional<UINT> CommandForPlayerKey(UINT key, bool controlDown, bool shiftDown)
{
    if (controlDown) {
        if (shiftDown) return std::nullopt;
        switch (key) {
        case 'G': return IDM_GOTO_TIMECODE;
        case 'R': return IDM_RENDER_RANGE;
        case 'N': return IDM_NEURAL_SETTINGS;
        default: return std::nullopt;
        }
    }
    switch (key) {
    case 'I': return shiftDown ? IDM_CLEAR_MARKS : IDM_MARK_IN;
    case 'O': return shiftDown ? IDM_CLEAR_MARKS : IDM_MARK_OUT;
    case 'F': return shiftDown ? IDM_PREVIEW_CLIP : IDM_PREVIEW_FRAME;
    case 'Z': return shiftDown ? std::nullopt : std::optional<UINT>(IDM_COMPARE_ZOOM);
    case VK_OEM_4: return shiftDown ? std::nullopt : std::optional<UINT>(IDM_COMPARE_BLEND_LESS);
    case VK_OEM_6: return shiftDown ? std::nullopt : std::optional<UINT>(IDM_COMPARE_BLEND_MORE);
    default: return std::nullopt;
    }
}

} // namespace app_menu
