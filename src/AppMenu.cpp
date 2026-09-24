#include "AppMenu.h"
#include "NeuralPresets.h"
#include "UpscalingPolicy.h"

#include <algorithm>
#include <iterator>

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
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), examples = CreatePopupMenu(), recent = CreatePopupMenu(), play = CreatePopupMenu(), video = CreatePopupMenu(), youtubeQuality = CreatePopupMenu(), compare = CreatePopupMenu(), dlss = CreatePopupMenu(), convert = CreatePopupMenu(), presets = CreatePopupMenu(), advanced = CreatePopupMenu();
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
    // A picture of what is on screen: the one thing File writes rather than opens, kept
    // in the file group because a group of one breaks the menu's own rule (only Exit
    // may stand alone).
    AppendMenuW(file, MF_STRING | MF_GRAYED, IDM_SAVE_COMPARISON_IMAGE, localizer.Get(L"menu.save_comparison").c_str());
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr); add(file, IDM_EXIT, L"menu.exit");
    // Transport, and the audio track is part of it: which track is playing is a
    // property of playback, and alone behind its own separator it was a group of
    // one - below the two-to-seven Microsoft's guidance asks for, and a rule this
    // menu broke twice.
    add(play, IDM_PLAY, L"menu.playpause"); add(play, IDM_STOP, L"menu.stop");
    add(play, IDM_BACK10, L"menu.back10"); add(play, IDM_FWD10, L"menu.forward10");
    add(play, IDM_MUTE, L"menu.mute");
    // Which track, then how it leaves the machine. UpdateAudioTracks rebuilds
    // only the rows above the separator, so the passthrough check survives
    // every media load.
    HMENU audioTracks = CreatePopupMenu();
    AppendMenuW(audioTracks, MF_STRING | MF_GRAYED, IDM_AUDIO_TRACK_FIRST, L"No audio tracks");
    AppendMenuW(audioTracks, MF_SEPARATOR, 0, nullptr);
    add(audioTracks, IDM_AUDIO_PASSTHROUGH, L"menu.audio_passthrough");
    AppendMenuW(play, MF_POPUP, reinterpret_cast<UINT_PTR>(audioTracks),
                localizer.Get(L"menu.audio").c_str());
    // Subtitles beside Audio, for the same reason: which ones are up
    // is a property of playback. The choices first (UpdateSubtitles fills them),
    // then loading a file, then the timing.
    // Greyed until something is loaded; UpdateSubtitles enables them.
    HMENU subtitles = CreatePopupMenu();
    const auto addGrayed = [&](UINT command, const wchar_t* key) {
        AppendMenuW(subtitles, MF_STRING | MF_GRAYED, command, localizer.Get(key).c_str());
    };
    addGrayed(IDM_SUBTITLE_OFF, L"menu.subtitles_off");
    AppendMenuW(subtitles, MF_SEPARATOR, 0, nullptr);
    addGrayed(IDM_SUBTITLE_NEXT, L"menu.subtitles_next");
    addGrayed(IDM_SUBTITLE_LOAD, L"menu.subtitles_load");
    AppendMenuW(subtitles, MF_SEPARATOR, 0, nullptr);
    addGrayed(IDM_SUBTITLE_EARLIER, L"menu.subtitles_earlier");
    addGrayed(IDM_SUBTITLE_LATER, L"menu.subtitles_later");
    addGrayed(IDM_SUBTITLE_DELAY_RESET, L"menu.subtitles_delay_reset");
    CheckMenuRadioItem(subtitles, 0, 0, 0, MF_BYPOSITION);
    AppendMenuW(play, MF_POPUP, reinterpret_cast<UINT_PTR>(subtitles),
                localizer.Get(L"menu.subtitles").c_str());
    AppendMenuW(play, MF_SEPARATOR, 0, nullptr);
    // The range tools. IDM_PAUSE_NEURAL_RENDER used to sit alone below these:
    // it is a control over the neural RENDER, not over playback, and it now
    // lives in the DLSS menu beside the rest of neural rendering.
    add(play, IDM_MARK_IN, L"menu.mark_in"); add(play, IDM_MARK_OUT, L"menu.mark_out");
    add(play, IDM_CLEAR_MARKS, L"menu.clear_marks"); add(play, IDM_GOTO_TIMECODE, L"menu.goto_timecode");
    add(youtubeQuality, IDM_YOUTUBE_QUALITY_AUTO, L"menu.youtube_quality_auto"); add(youtubeQuality, IDM_YOUTUBE_QUALITY_2160, L"menu.youtube_quality_2160"); add(youtubeQuality, IDM_YOUTUBE_QUALITY_1440, L"menu.youtube_quality_1440"); add(youtubeQuality, IDM_YOUTUBE_QUALITY_1080, L"menu.youtube_quality_1080"); CheckMenuRadioItem(youtubeQuality, IDM_YOUTUBE_QUALITY_AUTO, IDM_YOUTUBE_QUALITY_1080, IDM_YOUTUBE_QUALITY_AUTO, MF_BYCOMMAND);
    // How the picture sits in the window. Fullscreen belongs here rather than
    // alone at the bottom: it is the third answer to the same question the two
    // aspect commands answer.
    add(video, IDM_ASPECT_FIT, L"menu.aspectfit"); add(video, IDM_ASPECT_FILL, L"menu.aspectfill"); add(video, IDM_ASPECT_ONE_TO_ONE, L"menu.aspect_pixels");
    add(video, IDM_FULLSCREEN, L"menu.fullscreen");
    AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
    // What the picture is made of: the resolution it was fetched at, and the
    // colour controls applied to it.
    const std::wstring youtubeQualityName = localizer.Get(L"menu.youtube_quality");
    AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(youtubeQuality), youtubeQualityName.c_str());
    add(video, IDM_VIDEO_ADJUSTMENTS, L"menu.adjustments");
    AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
    // The modes, in the compare bar's order, then the Mix and the view. One radio
    // group from IDM_COMPARE_NEURAL to kLastComparisonModeCommand, by position.
    add(compare, IDM_COMPARE_NEURAL, L"menu.compare_neural"); add(compare, IDM_COMPARE_ORIGINAL, L"menu.compare_original"); add(compare, IDM_COMPARE_SPLIT, L"menu.compare_split"); add(compare, IDM_COMPARE_WIPE, L"menu.compare_wipe"); add(compare, IDM_COMPARE_DIFFERENCE, L"menu.compare_difference"); add(compare, IDM_COMPARE_SIDE_BY_SIDE, L"menu.compare_side_by_side"); add(compare, IDM_COMPARE_QUAD, L"menu.compare_quad"); AppendMenuW(compare, MF_SEPARATOR, 0, nullptr);
    // C and Shift+C walk the modes; listed so the keys reach the shortcut sheet.
    add(compare, IDM_COMPARE_NEXT_MODE, L"menu.compare_next_mode"); add(compare, IDM_COMPARE_PREVIOUS_MODE, L"menu.compare_previous_mode");
    add(compare, IDM_COMPARE_BLEND_LESS, L"menu.compare_blend_less"); add(compare, IDM_COMPARE_BLEND_MORE, L"menu.compare_blend_more"); add(compare, IDM_COMPARE_SWAP, L"menu.compare_swap");
    HMENU secondMix = CreatePopupMenu();
    for (UINT index = 0; index < IDM_COMPARE_SECOND_MIX_COUNT; ++index) {
        const std::wstring key = L"menu.compare_second_mix_" + std::to_wstring(index);
        add(secondMix, IDM_COMPARE_SECOND_MIX_FIRST + index, key.c_str());
    }
    AppendMenuW(compare, MF_POPUP, reinterpret_cast<UINT_PTR>(secondMix), localizer.Get(L"menu.compare_second_mix").c_str());
    AppendMenuW(compare, MF_SEPARATOR, 0, nullptr);
    // The spatial mask on the Mix: load, how soft its edge is, which way round, gone.
    add(compare, IDM_COMPARE_MASK_LOAD, L"menu.compare_mask_load");
    HMENU feather = CreatePopupMenu();
    for (UINT index = 0; index < IDM_COMPARE_MASK_FEATHER_COUNT; ++index) {
        const std::wstring key = L"menu.compare_mask_feather_" + std::to_wstring(index);
        add(feather, IDM_COMPARE_MASK_FEATHER_FIRST + index, key.c_str());
    }
    AppendMenuW(compare, MF_POPUP, reinterpret_cast<UINT_PTR>(feather), localizer.Get(L"menu.compare_mask_feather").c_str());
    add(compare, IDM_COMPARE_MASK_INVERT, L"menu.compare_mask_invert"); add(compare, IDM_COMPARE_MASK_CLEAR, L"menu.compare_mask_clear"); AppendMenuW(compare, MF_SEPARATOR, 0, nullptr);
    // How the Difference view is drawn.
    add(compare, IDM_COMPARE_DIFFERENCE_LESS, L"menu.compare_difference_less"); add(compare, IDM_COMPARE_DIFFERENCE_MORE, L"menu.compare_difference_more"); add(compare, IDM_COMPARE_DIFFERENCE_LUMA, L"menu.compare_difference_luma"); AppendMenuW(compare, MF_SEPARATOR, 0, nullptr);
    add(compare, IDM_COMPARE_ZOOM, L"menu.compare_zoom"); add(compare, IDM_COMPARE_ZOOM_OUT, L"menu.compare_zoom_out"); add(compare, IDM_COMPARE_ZOOM_FIT, L"menu.compare_zoom_fit"); add(compare, IDM_COMPARE_LOUPE, L"menu.compare_loupe");
    AppendMenuW(compare, MF_SEPARATOR, 0, nullptr); add(compare, IDM_COMPARE_HDR_AT_SDR, L"menu.compare_hdr_at_sdr");
    // Which image is on screen: the comparison modes and the four debug views
    // are the same question asked two ways, so they are one group.
    const std::wstring compareName = localizer.Get(L"menu.compare");
    AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(compare), compareName.c_str());
    add(video, IDM_VIEW_FINAL, L"menu.final"); add(video, IDM_VIEW_INPUT, L"menu.input");
    add(video, IDM_VIEW_MV, L"menu.mv"); add(video, IDM_VIEW_DEPTH, L"menu.depth");
    // The DLSS menu, grouped one feature per block.
    //
    // Microsoft's own menu guidance is the measure here: separators between
    // logical groups, no more than six of them, two to seven items per group.
    // NN/g's is the reason it matters - "groups of unrelated options reduce
    // clarity, decrease findability, hinder spatial memorability, and increase
    // cognitive load" - and that is exactly what this menu used to be. Neural
    // Rendering was the first row and its presets and settings were rows ten
    // and eleven, with all of upscaling and all of frame generation in between,
    // so the one feature a viewer touches most was split across the full height
    // of the menu by two features they may never touch.
    //
    // Now: neural, then upscaling, then frame generation, then the two preview
    // commands, then everything that writes a file. Each block is one feature
    // and holds its own toggle, its own submenu and its own settings, so a
    // viewer who wants one thing reads one block and stops.

    // --- Neural rendering: the toggle, its looks, its controls. ------------
    add(dlss, IDM_NEURAL_RENDERING, L"menu.neural_rendering");
    // Pausing the background render is a neural command, so it belongs to the
    // neural block. In Playback it was a group of one under a separator, and it
    // asked the viewer to look for a render control in the transport menu.
    add(dlss, IDM_PAUSE_NEURAL_RENDER, L"menu.pause_neural_render");
    // Presets before the controls they set: six sliders with measured tooltips
    // are the right surface for an expert and the wrong first contact. Every
    // preset is one neural evaluation at the same resolution, so this is a
    // choice of look, not a speed trade - that one lives in Encoder settings.
    // The measured cost, stated where the choice is made. The project's rule
    // is that a quality ladder prints what each rung costs; here the measured
    // answer is that they all cost the same, and that is precisely the answer
    // worth printing - without it "Strong" reads as the expensive one and
    // nobody picks it. Disabled, because it is a statement and not a choice.
    // The numbers and how to reproduce them are in NeuralPresets.h.
    AppendMenuW(presets, MF_STRING | MF_GRAYED, 0,
                L"All four render in the same time (6.3 s measured) - they change the look");
    AppendMenuW(presets, MF_SEPARATOR, 0, nullptr);
    for (size_t index = 0; index < neural_presets::kPresetCount; ++index) {
        const std::wstring label(neural_presets::kPresets[index].label.begin(),
                                 neural_presets::kPresets[index].label.end());
        AppendMenuW(presets, MF_STRING, IDM_NEURAL_PRESET_FIRST + index, label.c_str());
    }
    // Ends the radio block and is never selectable: it reports that a control
    // has been moved off every preset, which is the expected path once someone
    // starts from one and edits it.
    AppendMenuW(presets, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(presets, MF_STRING | MF_GRAYED, IDM_NEURAL_PRESET_CUSTOM,
                localizer.Get(L"menu.neural_preset_custom").c_str());
    CheckMenuRadioItem(presets, IDM_NEURAL_PRESET_FIRST, IDM_NEURAL_PRESET_CUSTOM,
                       IDM_NEURAL_PRESET_FIRST + UINT(neural_presets::kDefaultPresetIndex),
                       MF_BYCOMMAND);
    const std::wstring presetsName = localizer.Get(L"menu.neural_presets");
    AppendMenuW(dlss, MF_POPUP, reinterpret_cast<UINT_PTR>(presets), presetsName.c_str());
    add(dlss, IDM_NEURAL_SETTINGS, L"menu.neural_settings");
    // The resolution the model runs at. The one neural choice that trades
    // picture for speed, so it follows the project's ladder rule to the letter:
    // the default is the top rung and says so, and every rung carries the rate
    // it measured, so the trade is read before the render rather than after.
    // Numbers and method in UpscalingPolicy.h.
    HMENU processingScale = CreatePopupMenu();
    AppendMenuW(processingScale, MF_STRING | MF_GRAYED, 0,
                localizer.Get(L"menu.processing_scale_measured").c_str());
    AppendMenuW(processingScale, MF_SEPARATOR, 0, nullptr);
    add(processingScale, IDM_PROCESSING_SCALE_FIRST, L"menu.processing_scale_100");
    add(processingScale, IDM_PROCESSING_SCALE_FIRST + 1, L"menu.processing_scale_75");
    add(processingScale, IDM_PROCESSING_SCALE_FIRST + 2, L"menu.processing_scale_50");
    CheckMenuRadioItem(processingScale, IDM_PROCESSING_SCALE_FIRST, IDM_PROCESSING_SCALE_LAST,
                       CommandForProcessingScale(kDefaultProcessingScale), MF_BYCOMMAND);
    AppendMenuW(dlss, MF_POPUP, reinterpret_cast<UINT_PTR>(processingScale),
                localizer.Get(L"menu.processing_scale").c_str());
    AppendMenuW(dlss, MF_SEPARATOR, 0, nullptr);

    // --- DLSS Super Resolution: the toggle and the size it targets. --------
    add(dlss, IDM_DLSS_UPSCALING, L"menu.dlss_upscaling");
    HMENU upscaleOutput=CreatePopupMenu();
    add(upscaleOutput, IDM_UPSCALE_AUTO, L"menu.upscale_auto");
    add(upscaleOutput, IDM_UPSCALE_1080, L"menu.upscale_1080");
    add(upscaleOutput, IDM_UPSCALE_1440, L"menu.upscale_1440");
    add(upscaleOutput, IDM_UPSCALE_2160, L"menu.upscale_2160");
    CheckMenuRadioItem(upscaleOutput,IDM_UPSCALE_AUTO,IDM_UPSCALE_2160,IDM_UPSCALE_AUTO,MF_BYCOMMAND);
    AppendMenuW(dlss,MF_POPUP,reinterpret_cast<UINT_PTR>(upscaleOutput),localizer.Get(L"menu.upscale_output").c_str());
    // Whether Super Resolution accumulates over frames or upscales each one on its
    // own. Neither beats a plain scaler on decoded video, and which of the two does
    // better depends on the clip, so the choice carries what it measured in the
    // same greyed-statement form the processing scale uses. Numbers and method in
    // UpscalingPolicy.h and docs/measurements/sr-history-20260924.
    HMENU upscaleHistory=CreatePopupMenu();
    AppendMenuW(upscaleHistory, MF_STRING | MF_GRAYED, 0,
                localizer.Get(L"menu.upscale_history_measured").c_str());
    AppendMenuW(upscaleHistory, MF_SEPARATOR, 0, nullptr);
    add(upscaleHistory, IDM_UPSCALE_HISTORY_TEMPORAL, L"menu.upscale_history_temporal");
    add(upscaleHistory, IDM_UPSCALE_HISTORY_PER_FRAME, L"menu.upscale_history_per_frame");
    CheckMenuRadioItem(upscaleHistory,IDM_UPSCALE_HISTORY_TEMPORAL,IDM_UPSCALE_HISTORY_PER_FRAME,
                       IDM_UPSCALE_HISTORY_TEMPORAL,MF_BYCOMMAND);
    AppendMenuW(dlss,MF_POPUP,reinterpret_cast<UINT_PTR>(upscaleHistory),localizer.Get(L"menu.upscale_history").c_str());
    AppendMenuW(dlss, MF_SEPARATOR, 0, nullptr);

    // --- Frame generation: the verb and the rate it targets. ---------------
    //
    // IDM_FRAME_GENERATION's label swaps to "Cancel frame generation" while a
    // conversion runs, which is what the toolbar pill has always done. A
    // separate permanently-greyed cancel row was two rows of dead menu and one
    // more place for the two surfaces to disagree.
    add(dlss, IDM_FRAME_GENERATION, L"menu.frame_generation");
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
    AppendMenuW(framegenMultiple, MF_SEPARATOR, 0, nullptr);
    // A constraint on the multiple, not a choice of one: with it on, nothing is
    // generated unless the generated rate divides the display's refresh.
    add(framegenMultiple, IDM_FRAMEGEN_EVEN_ONLY, L"menu.framegen_even_only");
    // Also a constraint rather than a multiple: which source pairs are generated
    // between at all. For animation drawn on twos and threes.
    add(framegenMultiple, IDM_FRAMEGEN_HOLD_DUPLICATES, L"menu.framegen_hold_duplicates");
    AppendMenuW(dlss,MF_POPUP,reinterpret_cast<UINT_PTR>(framegenMultiple),
                localizer.Get(L"menu.framegen_multiple").c_str());
    AppendMenuW(dlss, MF_SEPARATOR, 0, nullptr);

    // --- Look before you commit. ------------------------------------------
    add(dlss, IDM_PREVIEW_FRAME, L"menu.preview_frame");
    add(dlss, IDM_PREVIEW_CLIP, L"menu.preview_clip");
    AppendMenuW(dlss, MF_SEPARATOR, 0, nullptr);

    // --- Everything that writes a file, plus how it is encoded. ------------
    //
    // The combined export leads because it is the one that answers "give me a
    // file with the stages I picked"; the two cache conversions below it are
    // about what PLAYBACK will use, which is a different question wearing
    // similar words.
    add(convert, IDM_EXPORT_STAGES, L"menu.export_stages");
    AppendMenuW(convert, MF_SEPARATOR, 0, nullptr);
    // Conversion writes a neural video to disk with the settings in the neural
    // settings dialog; it is deliberately separate from watching with the
    // rendering turned on.
    add(convert, IDM_RENDER_RANGE, L"menu.render_range");
    add(convert, IDM_RENDER_WHOLE, L"menu.render_whole");
    AppendMenuW(convert, MF_STRING | MF_GRAYED, IDM_EXPORT_CACHED_VIDEO, localizer.Get(L"menu.export_cached").c_str());
    AppendMenuW(convert, MF_SEPARATOR, 0, nullptr);
    // Grayed on creation: main.cpp enables it once a converted file exists.
    // Without it the output is unreachable after the confirmation dialog
    // closes - the export item above it is gated on a neural cache path a
    // frame-generation output never has.
    AppendMenuW(convert, MF_STRING | MF_GRAYED, IDM_SHOW_FRAMEGEN_OUTPUT,
                localizer.Get(L"menu.show_framegen_output").c_str());
    const std::wstring convertName = localizer.Get(L"menu.convert");
    AppendMenuW(dlss, MF_POPUP, reinterpret_cast<UINT_PTR>(convert), convertName.c_str());
    add(dlss, IDM_ENCODER_SETTINGS, L"menu.encoder_settings");
    // What this render left behind, and what it reported.
    add(advanced, IDM_CLEAR_NEURAL_CACHE, L"menu.clear_neural_cache");
    add(advanced, IDM_OPEN_RENDER_RECEIPT, L"menu.open_receipt");
    add(advanced, IDM_RENDER_REPORT, L"menu.render_report");
    AppendMenuW(advanced, MF_SEPARATOR, 0, nullptr);
    // Getting the runtime and the app back into a known state. Three separators
    // for five items made four groups, three of them a single row each.
    add(advanced, IDM_ADVANCED_SAFE_MODE, L"menu.safe_mode");
    add(advanced, IDM_REHOOK, L"menu.rehook");
    add(advanced, IDM_CHECK_FOR_UPDATES, L"menu.check_updates");
    const std::wstring fileName = localizer.Get(L"menu.file"), playName = localizer.Get(L"menu.playback"), videoName = localizer.Get(L"menu.video"), dlssName = localizer.Get(L"menu.dlss"), advancedName = localizer.Get(L"menu.advanced");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), fileName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(play), playName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(video), videoName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(dlss), dlssName.c_str()); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(advanced), advancedName.c_str());
    // Where a viewer looks for "what are the keys". About 30 shortcuts over
    // five menus were findable only by opening each menu in turn.
    HMENU help = CreatePopupMenu();
    add(help, IDM_KEYBOARD_SHORTCUTS, L"menu.keyboard_shortcuts");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), localizer.Get(L"menu.help").c_str());
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

void UpdateAudioTracks(HMENU menuBar, std::span<const std::wstring> labels, int selected)
{
    HMENU tracks=find_menu_containing_command(menuBar,IDM_AUDIO_TRACK_FIRST);
    if(!tracks)return;
    // The track rows are everything above the first separator; what is below
    // it is not a track and keeps its state.
    while(GetMenuItemCount(tracks)>0){
        MENUITEMINFOW item{};item.cbSize=sizeof(item);item.fMask=MIIM_FTYPE;
        if(!GetMenuItemInfoW(tracks,0,TRUE,&item)||(item.fType&MFT_SEPARATOR))break;
        DeleteMenu(tracks,0,MF_BYPOSITION);
    }
    // A source with one track has nothing to choose between, and the player
    // does not build a list for it, so the placeholder covers both that and
    // nothing being loaded.
    if(labels.empty()){
        InsertMenuW(tracks,0,MF_BYPOSITION|MF_STRING|MF_GRAYED,IDM_AUDIO_TRACK_FIRST,L"No audio tracks");
        return;
    }
    const size_t shown=std::min<size_t>(labels.size(),IDM_AUDIO_TRACK_COUNT);
    for(size_t index=0;index<shown;++index){
        // A title is free text from whoever made the file, so an ampersand in
        // it is a character rather than an accelerator, and a control
        // character is not allowed to break the item.
        std::wstring label;
        for(wchar_t character:labels[index].substr(0,160)){
            if(character==L'&')label+=L'&';
            label+=(character<L' '?L' ':character);
        }
        InsertMenuW(tracks,static_cast<UINT>(index),MF_BYPOSITION|MF_STRING,
                    IDM_AUDIO_TRACK_FIRST+static_cast<UINT>(index),label.c_str());
    }
    const UINT chosen=IDM_AUDIO_TRACK_FIRST+
        static_cast<UINT>(selected>=0&&size_t(selected)<shown?size_t(selected):0);
    CheckMenuRadioItem(tracks,IDM_AUDIO_TRACK_FIRST,
                       IDM_AUDIO_TRACK_FIRST+static_cast<UINT>(shown)-1,chosen,MF_BYCOMMAND);
}

void UpdateSubtitles(HMENU menuBar, const std::wstring& offLabel, std::span<const std::wstring> trackLabels,
                     const std::wstring& fileLabel, UINT chosen, bool loaded)
{
    HMENU menu=find_menu_containing_command(menuBar,IDM_SUBTITLE_OFF);
    if(!menu)return;
    // The group is everything above the first separator.
    while(GetMenuItemCount(menu)>0){
        const UINT id=GetMenuItemID(menu,0);
        if(id!=IDM_SUBTITLE_OFF&&id!=IDM_SUBTITLE_FILE&&
           (id<IDM_SUBTITLE_TRACK_FIRST||id>=IDM_SUBTITLE_TRACK_FIRST+IDM_SUBTITLE_TRACK_COUNT))break;
        DeleteMenu(menu,0,MF_BYPOSITION);
    }
    const auto escaped=[](std::wstring_view text){
        // Free text from whoever made the file: an ampersand is a character,
        // and a control character may not break the row.
        std::wstring label;
        for(wchar_t character:text.substr(0,160)){
            if(character==L'&')label+=L'&';
            label+=(character<L' '?L' ':character);
        }
        return label;
    };
    const UINT enabled=loaded?MF_ENABLED:MF_GRAYED;
    UINT position=0;
    InsertMenuW(menu,position++,MF_BYPOSITION|MF_STRING|enabled,IDM_SUBTITLE_OFF,offLabel.c_str());
    const size_t shown=std::min<size_t>(trackLabels.size(),IDM_SUBTITLE_TRACK_COUNT);
    for(size_t index=0;index<shown;++index)
        InsertMenuW(menu,position++,MF_BYPOSITION|MF_STRING|enabled,IDM_SUBTITLE_TRACK_FIRST+static_cast<UINT>(index),
                    escaped(trackLabels[index]).c_str());
    if(!fileLabel.empty())
        InsertMenuW(menu,position++,MF_BYPOSITION|MF_STRING|enabled,IDM_SUBTITLE_FILE,escaped(fileLabel).c_str());
    const UINT last=!fileLabel.empty()?IDM_SUBTITLE_FILE:
                    shown?IDM_SUBTITLE_TRACK_FIRST+static_cast<UINT>(shown)-1:IDM_SUBTITLE_OFF;
    CheckRadioCommand(menuBar,IDM_SUBTITLE_OFF,last,chosen);
    for(const UINT command:{IDM_SUBTITLE_NEXT,IDM_SUBTITLE_LOAD,IDM_SUBTITLE_EARLIER,IDM_SUBTITLE_LATER,IDM_SUBTITLE_DELAY_RESET})
        EnableMenuItem(menu,command,MF_BYCOMMAND|enabled);
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

std::optional<uint32_t> ProcessingScaleForCommand(UINT command)
{
    if (command < IDM_PROCESSING_SCALE_FIRST || command > IDM_PROCESSING_SCALE_LAST) return std::nullopt;
    return kProcessingScaleRungs[command - IDM_PROCESSING_SCALE_FIRST];
}

UINT CommandForProcessingScale(uint32_t percent)
{
    for (UINT index = 0; index < UINT(std::size(kProcessingScaleRungs)); ++index)
        if (kProcessingScaleRungs[index] == percent) return IDM_PROCESSING_SCALE_FIRST + index;
    return IDM_PROCESSING_SCALE_FIRST;
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
                               bool frameGenerationAvailable, bool frameGenerationRunning)
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
    // minutes-long conversion. It reports that a conversion is running by
    // becoming the cancel command, the way the toolbar pill does; a separate
    // permanently-greyed cancel row was a dead row and a second place for the
    // two surfaces to disagree. The two real toggles keep their checkmarks.
    const HMENU frameGeneration = find_menu_containing_command(menuBar, IDM_FRAME_GENERATION);
    return update(IDM_NEURAL_RENDERING, neuralAvailable, neuralActive) &&
           update(IDM_DLSS_UPSCALING, upscalingAvailable, upscalingActive) &&
           frameGeneration &&
           EnableMenuItem(frameGeneration, IDM_FRAME_GENERATION,
               MF_BYCOMMAND | ((frameGenerationAvailable || frameGenerationRunning) ? MF_ENABLED : MF_GRAYED)) !=
               static_cast<UINT>(-1);
}

HMENU FindMenuContainingCommand(HMENU menuBar, UINT command)
{
    return find_menu_containing_command(menuBar, command);
}

// Replaces one command's text in place, keeping its id, position and state.
// ModifyMenuW with MF_BYCOMMAND resets the item's type flags, so the text is
// set through MENUITEMINFO instead - a menu item that silently lost MF_GRAYED
// when it was relabelled would be worse than the dead row this replaced.
bool SetMenuCommandText(HMENU menu, UINT command, const std::wstring& text)
{
    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_STRING;
    info.dwTypeData = const_cast<wchar_t*>(text.c_str());
    return SetMenuItemInfoW(menu, command, FALSE, &info) != FALSE;
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
    // The report is read out of the receipt, so it is available exactly when it is.
    ok = enable(IDM_RENDER_REPORT, receiptAvailable) && ok;
    return enable(IDM_OPEN_RENDER_RECEIPT, receiptAvailable) && ok;
}

bool UpdateComparisonMenu(HMENU menuBar, bool modesAvailable, bool zoomAvailable,
                          UINT selectedMode, bool zoomed, bool swapped, bool loupe, bool differenceLuma)
{
    const HMENU menu = find_menu_containing_command(menuBar, IDM_COMPARE_NEURAL);
    if (!menu) return false;
    constexpr UINT modes[] = {IDM_COMPARE_NEURAL, IDM_COMPARE_ORIGINAL, IDM_COMPARE_SPLIT, IDM_COMPARE_WIPE, IDM_COMPARE_DIFFERENCE,
                              IDM_COMPARE_SIDE_BY_SIDE, IDM_COMPARE_QUAD};
    if (std::find(std::begin(modes), std::end(modes), selectedMode) == std::end(modes)) selectedMode = IDM_COMPARE_NEURAL;
    bool ok = true;
    for (const UINT command : modes)
        ok = EnableMenuItem(menu, command, MF_BYCOMMAND | (modesAvailable ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1) && ok;
    for (const UINT command : {IDM_COMPARE_NEXT_MODE, IDM_COMPARE_PREVIOUS_MODE, IDM_COMPARE_BLEND_LESS, IDM_COMPARE_BLEND_MORE, IDM_COMPARE_SWAP,
                               IDM_COMPARE_DIFFERENCE_LESS, IDM_COMPARE_DIFFERENCE_MORE, IDM_COMPARE_DIFFERENCE_LUMA})
        ok = EnableMenuItem(menu, command, MF_BYCOMMAND | (modesAvailable ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1) && ok;
    ok = CheckMenuItem(menu, IDM_COMPARE_SWAP, MF_BYCOMMAND | (swapped ? MF_CHECKED : MF_UNCHECKED)) != static_cast<DWORD>(-1) && ok;
    ok = CheckMenuItem(menu, IDM_COMPARE_DIFFERENCE_LUMA, MF_BYCOMMAND | (differenceLuma ? MF_CHECKED : MF_UNCHECKED)) != static_cast<DWORD>(-1) && ok;
    ok = CheckRadioCommand(menu, IDM_COMPARE_NEURAL, kLastComparisonModeCommand, selectedMode) && ok;
    ok = EnableMenuItem(menu, IDM_COMPARE_ZOOM, MF_BYCOMMAND | (zoomAvailable ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1) && ok;
    for (const UINT command : {IDM_COMPARE_ZOOM_OUT, IDM_COMPARE_ZOOM_FIT})
        ok = EnableMenuItem(menu, command, MF_BYCOMMAND | (zoomAvailable && zoomed ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1) && ok;
    ok = EnableMenuItem(menu, IDM_COMPARE_LOUPE, MF_BYCOMMAND | (modesAvailable ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1) && ok;
    return CheckMenuItem(menu, IDM_COMPARE_LOUPE, MF_BYCOMMAND | (loupe ? MF_CHECKED : MF_UNCHECKED)) != static_cast<DWORD>(-1) && ok;
}

bool UpdateMaskMenu(HMENU menuBar, bool loadAvailable, bool maskLoaded, bool inverted, UINT featherIndex)
{
    const HMENU menu = find_menu_containing_command(menuBar, IDM_COMPARE_MASK_LOAD);
    if (!menu) return false;
    bool ok = EnableMenuItem(menu, IDM_COMPARE_MASK_LOAD, MF_BYCOMMAND | (loadAvailable ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1);
    for (const UINT command : {IDM_COMPARE_MASK_INVERT, IDM_COMPARE_MASK_CLEAR})
        ok = EnableMenuItem(menu, command, MF_BYCOMMAND | (maskLoaded ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1) && ok;
    ok = CheckMenuItem(menu, IDM_COMPARE_MASK_INVERT, MF_BYCOMMAND | (maskLoaded && inverted ? MF_CHECKED : MF_UNCHECKED)) != static_cast<DWORD>(-1) && ok;
    const UINT last = IDM_COMPARE_MASK_FEATHER_FIRST + IDM_COMPARE_MASK_FEATHER_COUNT - 1;
    for (UINT command = IDM_COMPARE_MASK_FEATHER_FIRST; command <= last; ++command)
        ok = EnableMenuItem(menuBar, command, MF_BYCOMMAND | (maskLoaded ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1) && ok;
    return CheckRadioCommand(menuBar, IDM_COMPARE_MASK_FEATHER_FIRST, last,
                             IDM_COMPARE_MASK_FEATHER_FIRST + std::min(featherIndex, IDM_COMPARE_MASK_FEATHER_COUNT - 1)) && ok;
}

bool UpdateSecondMixMenu(HMENU menuBar, bool available, UINT index)
{
    const UINT last = IDM_COMPARE_SECOND_MIX_FIRST + IDM_COMPARE_SECOND_MIX_COUNT - 1;
    bool ok = true;
    for (UINT command = IDM_COMPARE_SECOND_MIX_FIRST; command <= last; ++command)
        ok = EnableMenuItem(menuBar, command, MF_BYCOMMAND | (available ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1) && ok;
    return CheckRadioCommand(menuBar, IDM_COMPARE_SECOND_MIX_FIRST, last,
                             IDM_COMPARE_SECOND_MIX_FIRST + std::min(index, IDM_COMPARE_SECOND_MIX_COUNT - 1)) && ok;
}

bool CheckRadioCommand(HMENU menuBar, UINT first, UINT last, UINT chosen)
{
    const HMENU menu = find_menu_containing_command(menuBar, first);
    if (!menu) return false;
    int firstIndex = -1, lastIndex = -1, chosenIndex = -1;
    for (int index = 0; index < GetMenuItemCount(menu); ++index) {
        const UINT id = GetMenuItemID(menu, index);
        if (id == first) firstIndex = index;
        if (id == last) lastIndex = index;
        if (id == chosen) chosenIndex = index;
    }
    if (firstIndex < 0 || lastIndex < firstIndex) return false;
    // An unknown choice checks the group's first item, as the callers always did.
    if (chosenIndex < firstIndex || chosenIndex > lastIndex) chosenIndex = firstIndex;
    return CheckMenuRadioItem(menu, UINT(firstIndex), UINT(lastIndex), UINT(chosenIndex), MF_BYPOSITION) != FALSE;
}

namespace {

// A menu item's text without its mnemonic ampersands ("Convert && export" is
// "Convert & export"), and an accelerator with its runs of spaces closed up.
std::wstring PlainMenuText(std::wstring_view text)
{
    std::wstring plain;
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'&') {
            if (index + 1 < text.size() && text[index + 1] == L'&') { plain.push_back(L'&'); ++index; }
            continue;
        }
        plain.push_back(text[index]);
    }
    return plain;
}

std::wstring CollapsedSpaces(std::wstring_view text)
{
    std::wstring collapsed;
    for (const wchar_t character : text) {
        if (character == L' ' && (collapsed.empty() || collapsed.back() == L' ')) continue;
        collapsed.push_back(character);
    }
    while (!collapsed.empty() && collapsed.back() == L' ') collapsed.pop_back();
    return collapsed;
}

void CollectMenuShortcuts(HMENU menu, const std::wstring& group, std::vector<ShortcutRow>& rows)
{
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        wchar_t text[256]{};
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_FTYPE | MIIM_SUBMENU | MIIM_STRING | MIIM_ID;
        item.dwTypeData = text;
        item.cch = static_cast<UINT>(std::size(text));
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &item)) continue;
        if (item.fType & MFT_SEPARATOR) continue;
        if (item.hSubMenu) { CollectMenuShortcuts(item.hSubMenu, group, rows); continue; }
        // The processing-scale rungs use the accelerator column for what each one
        // measured ("14.8 fps"), not for a key, and the sheet listed them as keys.
        if (item.wID >= IDM_PROCESSING_SCALE_FIRST && item.wID <= IDM_PROCESSING_SCALE_LAST) continue;
        const std::wstring_view label(text);
        const size_t tab = label.find(L'\t');
        if (tab == std::wstring_view::npos) continue;
        rows.push_back(ShortcutRow{group, PlainMenuText(label.substr(0, tab)),
                                   CollapsedSpaces(label.substr(tab + 1))});
    }
}

// The keys no menu command names. Every one is handled in main.cpp's
// WM_KEYDOWN (or the wheel, or a registered hotkey) and nowhere else, which
// is exactly why they were the ones nobody found.
struct KeyboardOnlyShortcut {
    const wchar_t* labelKey;
    const wchar_t* keys;
};
constexpr KeyboardOnlyShortcut kKeyboardOnlyShortcuts[] = {
    {L"shortcuts.focus", L"Tab / Shift+Tab"},
    {L"shortcuts.activate", L"Enter"},
    {L"shortcuts.step", L"."},
    {L"shortcuts.escape", L"Esc"},
    {L"shortcuts.reveal", L"F10 / Alt"},
    {L"shortcuts.volume", L"Mouse wheel"},
    {L"shortcuts.overlay", L"Ctrl+Alt+Left / Right / M / D"},
    {L"shortcuts.media_key", L"Media Play/Pause"},
};

} // namespace

std::vector<ShortcutRow> CollectShortcuts(HMENU menuBar, const Localizer& localizer)
{
    std::vector<ShortcutRow> rows;
    const int count = menuBar ? GetMenuItemCount(menuBar) : 0;
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menuBar, static_cast<UINT>(index), TRUE, &item) || !item.hSubMenu) continue;
        wchar_t name[128]{};
        GetMenuStringW(menuBar, static_cast<UINT>(index), name, static_cast<int>(std::size(name)), MF_BYPOSITION);
        CollectMenuShortcuts(item.hSubMenu, PlainMenuText(name), rows);
    }
    const std::wstring keyboard = localizer.Get(L"shortcuts.group.keyboard");
    for (const auto& shortcut : kKeyboardOnlyShortcuts)
        rows.push_back(ShortcutRow{keyboard, localizer.Get(shortcut.labelKey), shortcut.keys});
    return rows;
}

std::optional<UINT> CommandForPlayerKey(UINT key, bool controlDown, bool shiftDown)
{
    if (controlDown) {
        // Ctrl+Shift+S beside Ctrl+S: both write a file of what is loaded.
        if (shiftDown) return key == 'S' ? std::optional<UINT>(IDM_SAVE_COMPARISON_IMAGE) : std::nullopt;
        switch (key) {
        case 'G': return IDM_GOTO_TIMECODE;
        case 'R': return IDM_RENDER_RANGE;
        case 'N': return IDM_NEURAL_SETTINGS;
        // Ctrl+S, not Ctrl+E: Ctrl+E has opened Image adjustments since long
        // before this command existed, and the menu label claimed it anyway.
        // Bare S stops playback and is matched later, without the modifier.
        case 'S': return IDM_EXPORT_STAGES;
        default: return std::nullopt;
        }
    }
    switch (key) {
    case 'I': return shiftDown ? IDM_CLEAR_MARKS : IDM_MARK_IN;
    case 'O': return shiftDown ? IDM_CLEAR_MARKS : IDM_MARK_OUT;
    case 'F': return shiftDown ? IDM_PREVIEW_CLIP : IDM_PREVIEW_FRAME;
    case 'Z': return shiftDown ? IDM_COMPARE_ZOOM_OUT : IDM_COMPARE_ZOOM;
    case 'L': return shiftDown ? std::nullopt : std::optional<UINT>(IDM_COMPARE_LOUPE);
    case 'X': return shiftDown ? std::nullopt : std::optional<UINT>(IDM_COMPARE_SWAP);
    case 'C': return shiftDown ? IDM_COMPARE_PREVIOUS_MODE : IDM_COMPARE_NEXT_MODE;
    // V steps through the subtitles as it does in VLC and mpv; H and J sit
    // side by side for earlier and later, since G belongs to Ctrl+G's go-to.
    case 'V': return shiftDown ? std::nullopt : std::optional<UINT>(IDM_SUBTITLE_NEXT);
    case 'H': return shiftDown ? std::nullopt : std::optional<UINT>(IDM_SUBTITLE_EARLIER);
    case 'J': return shiftDown ? std::nullopt : std::optional<UINT>(IDM_SUBTITLE_LATER);
    case VK_OEM_4: return shiftDown ? IDM_COMPARE_DIFFERENCE_LESS : IDM_COMPARE_BLEND_LESS;
    case VK_OEM_6: return shiftDown ? IDM_COMPARE_DIFFERENCE_MORE : IDM_COMPARE_BLEND_MORE;
    default: return std::nullopt;
    }
}

} // namespace app_menu
