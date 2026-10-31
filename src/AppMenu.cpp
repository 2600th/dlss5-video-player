#include "AppMenu.h"
#include "NeuralPresets.h"
#include "UpscalingPolicy.h"

#include <algorithm>
#include <iterator>

#include "KeyBindings.h"
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
    // The same names and keys as View > Debug view: one name per view.
    const Localizer english;
    const key_bindings::Table& keys = key_bindings::Defaults();
    for (const auto& [command, key] : {std::pair{IDM_VIEW_FINAL, L"menu.final"}, std::pair{IDM_VIEW_INPUT, L"menu.input"},
                                       std::pair{IDM_VIEW_MV, L"menu.mv"}, std::pair{IDM_VIEW_DEPTH, L"menu.depth"}}) {
        const std::wstring text = english.Get(key) + L"\t" + keys.KeysFor(command);
        AppendMenuW(menu, MF_STRING, command, text.c_str());
    }
    if (selectedCommand < IDM_VIEW_FINAL || selectedCommand > IDM_VIEW_DEPTH) {
        selectedCommand = IDM_VIEW_FINAL;
    }
    CheckMenuRadioItem(menu, IDM_VIEW_FINAL, IDM_VIEW_DEPTH, selectedCommand, MF_BYCOMMAND);
    return menu;
}

HMENU CreateToolbarMenu(ToolbarMenu which, HMENU menuBar, const Localizer& localizer, const key_bindings::Table& keys)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return nullptr;
    const auto heading = [&](const wchar_t* key) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, localizer.Get(key).c_str());
    };
    const auto mirror = [&](UINT command, const wchar_t* fallbackKey) {
        const HMENU source = find_menu_containing_command(menuBar, command);
        wchar_t text[256]{};
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_STATE | MIIM_FTYPE | MIIM_STRING;
        item.dwTypeData = text;
        item.cch = static_cast<UINT>(std::size(text));
        if (source && GetMenuItemInfoW(source, command, FALSE, &item)) {
            MENUITEMINFOW copy{sizeof(copy)};
            copy.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_STRING;
            copy.wID = command;
            copy.fType = item.fType & MFT_RADIOCHECK;
            copy.fState = item.fState & (MFS_CHECKED | MFS_DISABLED);
            copy.dwTypeData = text;
            InsertMenuItemW(menu, static_cast<UINT>(GetMenuItemCount(menu)), TRUE, &copy);
            return;
        }
        std::wstring label = fallbackKey ? localizer.Get(fallbackKey) : std::wstring{};
        if (const std::wstring bound = keys.KeysFor(command); !bound.empty()) label += L"\t" + bound;
        AppendMenuW(menu, MF_STRING, command, label.c_str());
    };
    switch (which) {
    case ToolbarMenu::Neural:
        heading(L"menu.neural_presets");
        for (UINT index = 0; index < neural_presets::kPresetCount; ++index) mirror(IDM_NEURAL_PRESET_FIRST + index, nullptr);
        mirror(IDM_NEURAL_PRESET_CUSTOM, L"menu.neural_preset_custom");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        mirror(IDM_NEURAL_SETTINGS, L"menu.neural_settings");
        mirror(IDM_RERENDER_HERE, L"menu.rerender_here");
        mirror(IDM_PREVIEW_FRAME, L"menu.preview_frame");
        break;
    case ToolbarMenu::Upscaler:
        mirror(IDM_RTX_VSR_UPSCALING, L"menu.rtx_vsr_upscaling");
        mirror(IDM_DLSS_UPSCALING, L"menu.dlss_upscaling");
        mirror(IDM_UPSCALER_NONE, L"menu.upscaler_none");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        heading(L"menu.compare_vsr_quality");
        for (UINT index = 0; index < IDM_COMPARE_VSR_QUALITY_COUNT; ++index) mirror(IDM_COMPARE_VSR_QUALITY_FIRST + index, nullptr);
        break;
    }
    return menu;
}

HMENU CreateMenuBar(const Localizer& localizer, bool youtubeAvailable)
{
    return CreateMenuBar(localizer, youtubeAvailable, key_bindings::Defaults());
}

// File · Playback · View · Compare · Enhance · Tools · Help: the UI proposal's
// §3.1 (2026-10-04), which every row below follows. The menus are named for
// jobs, not technologies: "Enhance" was "DLSS", and RTX VSR is not DLSS; the
// DLSS names stay on the items. Nothing was removed - every command of the
// six-menu bar is a row here, most of them where a viewer would look first.
//
// Microsoft's menu guidance is still the measure: separators between logical
// groups, no more than six of them, two to seven items per group. NN/g's is
// the reason it matters - "groups of unrelated options reduce clarity,
// decrease findability, hinder spatial memorability, and increase cognitive
// load". The one group past seven is Compare's mode radios, which are one
// radio set in the compare bar's own order.
//
// No label carries its key. ApplyKeyBindings writes the key column from the
// key table (KeyBindings.h), so a key rebound in input.conf is the key the
// menu prints and the shortcut sheet lists.
HMENU CreateMenuBar(const Localizer& localizer, bool youtubeAvailable, const key_bindings::Table& keys)
{
    HMENU bar = CreateMenu();
    const auto add = [&](HMENU menu, UINT command, const wchar_t* key, UINT flags = 0) {
        const std::wstring text = localizer.Get(key);
        AppendMenuW(menu, MF_STRING | flags, command, text.c_str());
    };
    const auto popup = [&](HMENU menu, HMENU child, const wchar_t* key) {
        const std::wstring text = localizer.Get(key);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(child), text.c_str());
    };
    const auto separator = [](HMENU menu) { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };

    // --- File: what comes in, what goes out. -------------------------------
    HMENU file = CreatePopupMenu();
    add(file, IDM_OPEN, L"menu.open");
    add(file, IDM_OPEN_YOUTUBE, L"menu.open_youtube", youtubeAvailable ? 0 : MF_GRAYED);
    // The resolution a YouTube source is fetched at is a property of opening it,
    // so it sits under Open YouTube URL rather than among the view settings.
    HMENU youtubeQuality = CreatePopupMenu();
    add(youtubeQuality, IDM_YOUTUBE_QUALITY_AUTO, L"menu.youtube_quality_auto");
    add(youtubeQuality, IDM_YOUTUBE_QUALITY_2160, L"menu.youtube_quality_2160");
    add(youtubeQuality, IDM_YOUTUBE_QUALITY_1440, L"menu.youtube_quality_1440");
    add(youtubeQuality, IDM_YOUTUBE_QUALITY_1080, L"menu.youtube_quality_1080");
    CheckMenuRadioItem(youtubeQuality, IDM_YOUTUBE_QUALITY_AUTO, IDM_YOUTUBE_QUALITY_1080, IDM_YOUTUBE_QUALITY_AUTO, MF_BYCOMMAND);
    popup(file, youtubeQuality, L"menu.youtube_quality");
    HMENU examples = CreatePopupMenu();
    for (size_t index = 0; index < kExampleVideos.size(); ++index) {
        const ExampleVideo& example = kExampleVideos[index];
        AppendMenuW(examples, MF_STRING | (youtubeAvailable ? 0 : MF_GRAYED),
                    IDM_EXAMPLE_VIDEO_FIRST + static_cast<UINT>(index), example.title.data());
    }
    popup(file, examples, L"menu.game_trailers");
    HMENU recent = CreatePopupMenu();
    add(recent, IDM_RECENT_VIDEO_FIRST, L"menu.recent_none", MF_GRAYED);
    popup(file, recent, L"menu.recent");
    separator(file);
    // Everything that writes a file. Export leads: it answers "give me a file of
    // what I am watching". It becomes Cancel export while one runs.
    add(file, IDM_EXPORT_STAGES, L"menu.export_stages");
    add(file, IDM_EXPORT_CACHED_VIDEO, L"menu.export_cached", MF_GRAYED);
    add(file, IDM_SAVE_COMPARISON_IMAGE, L"menu.save_comparison", MF_GRAYED);
    // Grayed on creation: main.cpp enables it once an export or a frame
    // generation has written a file, which is otherwise unreachable once its
    // confirmation closes.
    add(file, IDM_SHOW_FRAMEGEN_OUTPUT, L"menu.show_framegen_output", MF_GRAYED);
    separator(file);
    add(file, IDM_EXIT, L"menu.exit");

    // --- Playback: transport, then sound and words, then the range. ---------
    HMENU play = CreatePopupMenu();
    add(play, IDM_PLAY, L"menu.playpause"); add(play, IDM_STOP, L"menu.stop");
    add(play, IDM_BACK10, L"menu.back10"); add(play, IDM_FWD10, L"menu.forward10");
    // The '.' key had no row and was found only on the shortcut sheet.
    add(play, IDM_STEP_FRAME, L"menu.step_frame");
    add(play, IDM_GOTO_TIMECODE, L"menu.goto_timecode");
    separator(play);
    add(play, IDM_MUTE, L"menu.mute");
    // Which track, then how it leaves the machine. UpdateAudioTracks rebuilds
    // only the rows above the separator, so the passthrough check survives
    // every media load.
    HMENU audioTracks = CreatePopupMenu();
    add(audioTracks, IDM_AUDIO_TRACK_FIRST, L"menu.audio_none", MF_GRAYED);
    separator(audioTracks);
    add(audioTracks, IDM_AUDIO_PASSTHROUGH, L"menu.audio_passthrough");
    popup(play, audioTracks, L"menu.audio");
    // The choices first (UpdateSubtitles fills them), then loading a file, then
    // the timing. Greyed until something is loaded; UpdateSubtitles enables them.
    HMENU subtitles = CreatePopupMenu();
    add(subtitles, IDM_SUBTITLE_OFF, L"menu.subtitles_off", MF_GRAYED);
    separator(subtitles);
    add(subtitles, IDM_SUBTITLE_NEXT, L"menu.subtitles_next", MF_GRAYED);
    add(subtitles, IDM_SUBTITLE_LOAD, L"menu.subtitles_load", MF_GRAYED);
    separator(subtitles);
    add(subtitles, IDM_SUBTITLE_EARLIER, L"menu.subtitles_earlier", MF_GRAYED);
    add(subtitles, IDM_SUBTITLE_LATER, L"menu.subtitles_later", MF_GRAYED);
    add(subtitles, IDM_SUBTITLE_DELAY_RESET, L"menu.subtitles_delay_reset", MF_GRAYED);
    CheckMenuRadioItem(subtitles, 0, 0, 0, MF_BYPOSITION);
    popup(play, subtitles, L"menu.subtitles");
    separator(play);
    add(play, IDM_MARK_IN, L"menu.mark_in"); add(play, IDM_MARK_OUT, L"menu.mark_out");
    add(play, IDM_CLEAR_MARKS, L"menu.clear_marks");

    // --- View: how the picture sits on this screen. -------------------------
    HMENU view = CreatePopupMenu();
    add(view, IDM_ASPECT_FIT, L"menu.aspectfit"); add(view, IDM_ASPECT_FILL, L"menu.aspectfill");
    add(view, IDM_ASPECT_ONE_TO_ONE, L"menu.aspect_pixels"); add(view, IDM_FULLSCREEN, L"menu.fullscreen");
    separator(view);
    // The Picture panel on its viewing zone: instant, this screen only.
    add(view, IDM_VIDEO_ADJUSTMENTS, L"menu.adjustments");
    // A developer's view of what the model is given, one submenu rather than four
    // top-level rows.
    HMENU debugView = CreatePopupMenu();
    add(debugView, IDM_VIEW_FINAL, L"menu.final"); add(debugView, IDM_VIEW_INPUT, L"menu.input");
    add(debugView, IDM_VIEW_MV, L"menu.mv"); add(debugView, IDM_VIEW_DEPTH, L"menu.depth");
    popup(view, debugView, L"menu.debug_view");

    // --- Compare: promoted from Video > Compare. ----------------------------
    // The modes, in the compare bar's order: one radio group from
    // IDM_COMPARE_NEURAL to kLastComparisonModeCommand, by position.
    HMENU compare = CreatePopupMenu();
    add(compare, IDM_COMPARE_NEURAL, L"menu.compare_neural"); add(compare, IDM_COMPARE_ORIGINAL, L"menu.compare_original");
    add(compare, IDM_COMPARE_VSR, L"menu.compare_vsr"); add(compare, IDM_COMPARE_SPLIT, L"menu.compare_split");
    add(compare, IDM_COMPARE_WIPE, L"menu.compare_wipe"); add(compare, IDM_COMPARE_DIFFERENCE, L"menu.compare_difference");
    add(compare, IDM_COMPARE_SIDE_BY_SIDE, L"menu.compare_side_by_side"); add(compare, IDM_COMPARE_QUAD, L"menu.compare_quad");
    separator(compare);
    add(compare, IDM_COMPARE_NEXT_MODE, L"menu.compare_next_mode"); add(compare, IDM_COMPARE_PREVIOUS_MODE, L"menu.compare_previous_mode");
    add(compare, IDM_COMPARE_BLEND_LESS, L"menu.compare_blend_less"); add(compare, IDM_COMPARE_BLEND_MORE, L"menu.compare_blend_more");
    add(compare, IDM_COMPARE_SWAP, L"menu.compare_swap");
    separator(compare);
    add(compare, IDM_COMPARE_ZOOM, L"menu.compare_zoom"); add(compare, IDM_COMPARE_ZOOM_OUT, L"menu.compare_zoom_out");
    add(compare, IDM_COMPARE_ZOOM_FIT, L"menu.compare_zoom_fit"); add(compare, IDM_COMPARE_LOUPE, L"menu.compare_loupe");
    separator(compare);
    // What the other modes compare against, then one submenu per view that has
    // settings of its own. M3's loop, speed and flicker-map rows go in this
    // group, one submenu per feature, as the compare bar's contextual part does.
    add(compare, IDM_COMPARE_AGAINST_VSR, L"menu.compare_against_vsr");
    HMENU difference = CreatePopupMenu();
    add(difference, IDM_COMPARE_DIFFERENCE_LESS, L"menu.compare_difference_less");
    add(difference, IDM_COMPARE_DIFFERENCE_MORE, L"menu.compare_difference_more");
    add(difference, IDM_COMPARE_DIFFERENCE_LUMA, L"menu.compare_difference_luma");
    popup(compare, difference, L"menu.compare_difference_view");
    HMENU secondMix = CreatePopupMenu();
    for (UINT index = 0; index < IDM_COMPARE_SECOND_MIX_COUNT; ++index) {
        const std::wstring key = L"menu.compare_second_mix_" + std::to_wstring(index);
        add(secondMix, IDM_COMPARE_SECOND_MIX_FIRST + index, key.c_str());
    }
    popup(compare, secondMix, L"menu.compare_second_mix");
    // The spatial mask on the Mix: load, how soft its edge is, which way round, gone.
    HMENU mask = CreatePopupMenu();
    add(mask, IDM_COMPARE_MASK_LOAD, L"menu.compare_mask_load");
    HMENU feather = CreatePopupMenu();
    for (UINT index = 0; index < IDM_COMPARE_MASK_FEATHER_COUNT; ++index) {
        const std::wstring key = L"menu.compare_mask_feather_" + std::to_wstring(index);
        add(feather, IDM_COMPARE_MASK_FEATHER_FIRST + index, key.c_str());
    }
    popup(mask, feather, L"menu.compare_mask_feather");
    add(mask, IDM_COMPARE_MASK_INVERT, L"menu.compare_mask_invert"); add(mask, IDM_COMPARE_MASK_CLEAR, L"menu.compare_mask_clear");
    popup(compare, mask, L"menu.compare_mask");
    add(compare, IDM_COMPARE_HDR_AT_SDR, L"menu.compare_hdr_at_sdr");

    // --- Enhance: was "DLSS". One feature per block. ------------------------
    HMENU enhance = CreatePopupMenu();
    // DLSS 5: the toggle, the panel, the look, and the render's own pace.
    add(enhance, IDM_NEURAL_RENDERING, L"menu.neural_rendering");
    add(enhance, IDM_NEURAL_SETTINGS, L"menu.neural_settings");
    // Every preset is one neural evaluation at the same resolution, so this is a
    // choice of look, not a speed trade, and the menu says so where the choice is
    // made. Disabled, because it is a statement and not a choice. The numbers and
    // how to reproduce them are in NeuralPresets.h.
    HMENU presets = CreatePopupMenu();
    add(presets, 0, L"menu.neural_presets_measured", MF_GRAYED);
    separator(presets);
    for (size_t index = 0; index < neural_presets::kPresetCount; ++index) {
        const std::wstring label(neural_presets::kPresets[index].label.begin(),
                                 neural_presets::kPresets[index].label.end());
        AppendMenuW(presets, MF_STRING, IDM_NEURAL_PRESET_FIRST + index, label.c_str());
    }
    // Ends the radio block and is never selectable: it reports that a control
    // has been moved off every preset.
    separator(presets);
    add(presets, IDM_NEURAL_PRESET_CUSTOM, L"menu.neural_preset_custom", MF_GRAYED);
    CheckMenuRadioItem(presets, IDM_NEURAL_PRESET_FIRST, IDM_NEURAL_PRESET_CUSTOM,
                       IDM_NEURAL_PRESET_FIRST + UINT(neural_presets::kDefaultPresetIndex), MF_BYCOMMAND);
    popup(enhance, presets, L"menu.neural_presets");
    // The one neural choice that trades picture for speed, so every rung carries
    // the rate it measured and the default is the top rung. UpscalingPolicy.h.
    HMENU processingScale = CreatePopupMenu();
    add(processingScale, 0, L"menu.processing_scale_measured", MF_GRAYED);
    separator(processingScale);
    add(processingScale, IDM_PROCESSING_SCALE_FIRST, L"menu.processing_scale_100");
    add(processingScale, IDM_PROCESSING_SCALE_FIRST + 1, L"menu.processing_scale_75");
    add(processingScale, IDM_PROCESSING_SCALE_FIRST + 2, L"menu.processing_scale_50");
    CheckMenuRadioItem(processingScale, IDM_PROCESSING_SCALE_FIRST, IDM_PROCESSING_SCALE_LAST,
                       CommandForProcessingScale(kDefaultProcessingScale), MF_BYCOMMAND);
    popup(enhance, processingScale, L"menu.processing_scale");
    add(enhance, IDM_PAUSE_NEURAL_RENDER, L"menu.pause_neural_render");
    separator(enhance);
    // Look before you commit, then commit.
    add(enhance, IDM_PREVIEW_FRAME, L"menu.preview_frame");
    add(enhance, IDM_PREVIEW_CLIP, L"menu.preview_clip");
    add(enhance, IDM_RERENDER_HERE, L"menu.rerender_here");
    add(enhance, IDM_RENDER_RANGE, L"menu.render_range");
    add(enhance, IDM_RENDER_WHOLE, L"menu.render_whole");
    separator(enhance);
    // The playback upscaler: three radios by position where two checkboxes were,
    // because there were only ever three states - RTX VSR, DLSS Super
    // Resolution (RTX VSR stands aside while it runs), or the window's own
    // scaling - and "both off" was not a state a viewer could pick.
    add(enhance, IDM_RTX_VSR_UPSCALING, L"menu.rtx_vsr_upscaling");
    add(enhance, IDM_DLSS_UPSCALING, L"menu.dlss_upscaling");
    add(enhance, IDM_UPSCALER_NONE, L"menu.upscaler_none");
    // RTX VSR's ladder drives playback and export as well as the comparison view.
    HMENU vsrQuality = CreatePopupMenu();
    for (UINT index = 0; index < IDM_COMPARE_VSR_QUALITY_COUNT; ++index) {
        const std::wstring key = L"menu.compare_vsr_quality_" + std::to_wstring(index);
        add(vsrQuality, IDM_COMPARE_VSR_QUALITY_FIRST + index, key.c_str());
    }
    popup(enhance, vsrQuality, L"menu.compare_vsr_quality");
    HMENU upscaleOutput = CreatePopupMenu();
    add(upscaleOutput, IDM_UPSCALE_AUTO, L"menu.upscale_auto");
    add(upscaleOutput, IDM_UPSCALE_1080, L"menu.upscale_1080");
    add(upscaleOutput, IDM_UPSCALE_1440, L"menu.upscale_1440");
    add(upscaleOutput, IDM_UPSCALE_2160, L"menu.upscale_2160");
    CheckMenuRadioItem(upscaleOutput, IDM_UPSCALE_AUTO, IDM_UPSCALE_2160, IDM_UPSCALE_AUTO, MF_BYCOMMAND);
    popup(enhance, upscaleOutput, L"menu.upscale_output");
    // Whether Super Resolution accumulates over frames or upscales each one on its
    // own, with what each measured. UpscalingPolicy.h, docs/measurements/sr-history-20260924.
    HMENU upscaleHistory = CreatePopupMenu();
    add(upscaleHistory, 0, L"menu.upscale_history_measured", MF_GRAYED);
    separator(upscaleHistory);
    add(upscaleHistory, IDM_UPSCALE_HISTORY_TEMPORAL, L"menu.upscale_history_temporal");
    add(upscaleHistory, IDM_UPSCALE_HISTORY_PER_FRAME, L"menu.upscale_history_per_frame");
    CheckMenuRadioItem(upscaleHistory, IDM_UPSCALE_HISTORY_TEMPORAL, IDM_UPSCALE_HISTORY_PER_FRAME,
                       IDM_UPSCALE_HISTORY_TEMPORAL, MF_BYCOMMAND);
    popup(enhance, upscaleHistory, L"menu.upscale_history");
    separator(enhance);
    // Frame generation: the verb, which becomes Cancel frame generation while a
    // conversion runs, and the rate it targets. The multiple is a preference, not
    // an automatic maximum: the default doubles the rate and the rest is asked for.
    add(enhance, IDM_FRAME_GENERATION, L"menu.frame_generation");
    HMENU framegenMultiple = CreatePopupMenu();
    add(framegenMultiple, IDM_FRAMEGEN_2X, L"menu.framegen_2x");
    add(framegenMultiple, IDM_FRAMEGEN_3X, L"menu.framegen_3x");
    add(framegenMultiple, IDM_FRAMEGEN_4X, L"menu.framegen_4x");
    add(framegenMultiple, IDM_FRAMEGEN_5X, L"menu.framegen_5x");
    add(framegenMultiple, IDM_FRAMEGEN_MAX, L"menu.framegen_max");
    CheckMenuRadioItem(framegenMultiple, IDM_FRAMEGEN_2X, IDM_FRAMEGEN_MAX, IDM_FRAMEGEN_2X, MF_BYCOMMAND);
    separator(framegenMultiple);
    // Constraints on the multiple rather than choices of one.
    add(framegenMultiple, IDM_FRAMEGEN_EVEN_ONLY, L"menu.framegen_even_only");
    add(framegenMultiple, IDM_FRAMEGEN_HOLD_DUPLICATES, L"menu.framegen_hold_duplicates");
    popup(enhance, framegenMultiple, L"menu.framegen_multiple");

    // --- Tools: was "Advanced". What the render leaves behind. ------------
    HMENU tools = CreatePopupMenu();
    add(tools, IDM_ENCODER_SETTINGS, L"menu.encoder_settings");
    add(tools, IDM_CLEAR_NEURAL_CACHE, L"menu.clear_neural_cache");
    separator(tools);
    add(tools, IDM_RENDER_REPORT, L"menu.render_report");
    add(tools, IDM_OPEN_RENDER_RECEIPT, L"menu.open_receipt");
    separator(tools);
    add(tools, IDM_ADVANCED_SAFE_MODE, L"menu.safe_mode");
    add(tools, IDM_REHOOK, L"menu.rehook");

    // --- Help: the keys, the guide, the log, and what this build is. -------
    HMENU help = CreatePopupMenu();
    add(help, IDM_KEYBOARD_SHORTCUTS, L"menu.keyboard_shortcuts");
    add(help, IDM_HELP_KEY_BINDINGS, L"menu.key_bindings");
    add(help, IDM_HELP_USER_GUIDE, L"menu.user_guide");
    add(help, IDM_HELP_LOG_FOLDER, L"menu.log_folder");
    separator(help);
    add(help, IDM_CHECK_FOR_UPDATES, L"menu.check_updates");
    add(help, IDM_HELP_ABOUT, L"menu.about");

    popup(bar, file, L"menu.file"); popup(bar, play, L"menu.playback"); popup(bar, view, L"menu.view");
    popup(bar, compare, L"menu.compare"); popup(bar, enhance, L"menu.enhance"); popup(bar, tools, L"menu.tools");
    popup(bar, help, L"menu.help");
    ApplyKeyBindings(bar, keys, localizer);
    UpdateUpscalerMenu(bar, IDM_RTX_VSR_UPSCALING, true, false);
    UpdateFeatureAvailability(bar, true, false, false, false, false, false, false);
    UpdateRenderActionAvailability(bar, false, false, false, false, false);
    UpdateComparisonMenu(bar, false, false, IDM_COMPARE_NEURAL, false);
    return bar;
}

namespace {

// The keys a row prints: its command's own, and, for Fit, the keys of the
// command that has no row of its own and has always been printed there (A,
// which flips Fit and Fill).
std::wstring RowKeys(const key_bindings::Table& keys, UINT command)
{
    std::wstring text = keys.KeysFor(command);
    if (command == IDM_ASPECT_FIT) {
        const std::wstring toggle = keys.KeysFor(IDM_ASPECT_TOGGLE);
        if (!toggle.empty()) text = text.empty() ? toggle : toggle + L" / " + text;
    }
    return text;
}

void ApplyKeyBindingsTo(HMENU menu, const key_bindings::Table& keys, const Localizer& localizer)
{
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        wchar_t text[256]{};
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_FTYPE | MIIM_SUBMENU | MIIM_STRING | MIIM_ID;
        item.dwTypeData = text;
        item.cch = static_cast<UINT>(std::size(text));
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &item) || (item.fType & MFT_SEPARATOR)) continue;
        if (item.hSubMenu) { ApplyKeyBindingsTo(item.hSubMenu, keys, localizer); continue; }
        // A row with no command name keeps its text: the measured rungs use the
        // key column for what each one measured.
        if (key_bindings::CommandName(item.wID).empty()) continue;
        std::wstring label(text);
        label = label.substr(0, label.find(L'\t'));
        std::wstring column = RowKeys(keys, item.wID);
        // The two overlay hotkeys are registered with Windows, not read from the
        // key table, and are printed after the row's own keys.
        const wchar_t* overlay = item.wID == IDM_PLAY ? L"menu.playpause_overlay"
                               : item.wID == IDM_VIDEO_ADJUSTMENTS ? L"menu.adjustments_overlay" : nullptr;
        if (overlay) column += (column.empty() ? L"" : L"   ") + localizer.Get(overlay);
        if (!column.empty()) label += L"\t" + column;
        MENUITEMINFOW update{sizeof(update)};
        update.fMask = MIIM_STRING;
        update.dwTypeData = label.data();
        SetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &update);
    }
}

} // namespace

void ApplyKeyBindings(HMENU menuBar, const key_bindings::Table& keys, const Localizer& localizer)
{
    if (menuBar) ApplyKeyBindingsTo(menuBar, keys, localizer);
}

bool UpdateUpscalerMenu(HMENU menuBar, UINT chosen, bool vsrAvailable, bool superResolutionAvailable)
{
    const HMENU menu = find_menu_containing_command(menuBar, IDM_RTX_VSR_UPSCALING);
    if (!menu) return false;
    bool ok = EnableMenuItem(menu, IDM_RTX_VSR_UPSCALING, MF_BYCOMMAND | (vsrAvailable ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1);
    ok = EnableMenuItem(menu, IDM_DLSS_UPSCALING, MF_BYCOMMAND | (superResolutionAvailable ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1) && ok;
    if (chosen != IDM_DLSS_UPSCALING && chosen != IDM_UPSCALER_NONE) chosen = IDM_RTX_VSR_UPSCALING;
    return CheckRadioCommand(menu, IDM_RTX_VSR_UPSCALING, IDM_UPSCALER_NONE, chosen) && ok;
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

void UpdateRecentVideos(HMENU menuBar, std::span<const std::wstring> titles, bool enabled, const std::wstring& noneLabel)
{
    HMENU recent=find_menu_containing_command(menuBar,IDM_RECENT_VIDEO_FIRST);
    if(!recent)return;
    while(GetMenuItemCount(recent)>0)DeleteMenu(recent,0,MF_BYPOSITION);
    if(titles.empty())AppendMenuW(recent,MF_STRING|MF_GRAYED,IDM_RECENT_VIDEO_FIRST,noneLabel.c_str());
    for(size_t index=0;index<titles.size()&&index<5;++index){
        std::wstring label=std::to_wstring(index+1)+L". ";
        for(wchar_t c:titles[index].substr(0,120)){
            if(c==L'&')label+=L'&';
            label+=(c<L' '?L' ':c);
        }
        AppendMenuW(recent,MF_STRING|(enabled?MF_ENABLED:MF_GRAYED),IDM_RECENT_VIDEO_FIRST+static_cast<UINT>(index),label.c_str());
    }
}

void UpdateAudioTracks(HMENU menuBar, std::span<const std::wstring> labels, int selected, const std::wstring& noneLabel)
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
        InsertMenuW(tracks,0,MF_BYPOSITION|MF_STRING|MF_GRAYED,IDM_AUDIO_TRACK_FIRST,noneLabel.c_str());
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
    return route == PlayerCommandRoute::KeyDown ? CommandForPlayerKey(value, false, false) == IDM_REHOOK : value == IDM_REHOOK;
}

bool RoutesToOpenYouTube(PlayerCommandRoute route, UINT value, bool controlDown)
{
    if (route == PlayerCommandRoute::NativeMenu) return value == IDM_OPEN_YOUTUBE;
    return CommandForPlayerKey(value, controlDown, false) == IDM_OPEN_YOUTUBE;
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
                               bool upscalingAvailable, bool /*upscalingActive*/,
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
           // Super Resolution is one of the upscaler radios (UpdateUpscalerMenu),
           // which carry its mark; a checkbox mark on a radio row would be a second.
           EnableMenuItem(menuBar, IDM_DLSS_UPSCALING, MF_BYCOMMAND | (upscalingAvailable ? MF_ENABLED : MF_GRAYED)) !=
               static_cast<UINT>(-1) &&
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
    // A label without a tab keeps the row's key column: the keys are the key
    // table's (ApplyKeyBindings), and Export's Ctrl+S is still Ctrl+S while the
    // row reads Cancel export.
    std::wstring labelled = text;
    if (text.find(L'\t') == std::wstring::npos) {
        wchar_t current[256]{};
        if (GetMenuStringW(menu, command, current, static_cast<int>(std::size(current)), MF_BYCOMMAND) > 0)
            if (const wchar_t* tab = wcschr(current, L'\t')) labelled += tab;
    }
    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_STRING;
    info.dwTypeData = labelled.data();
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
                          UINT selectedMode, bool zoomed, bool swapped, bool loupe, bool differenceLuma,
                          bool vsrAvailable, bool againstVsr)
{
    const HMENU menu = find_menu_containing_command(menuBar, IDM_COMPARE_NEURAL);
    if (!menu) return false;
    constexpr UINT modes[] = {IDM_COMPARE_NEURAL, IDM_COMPARE_ORIGINAL, IDM_COMPARE_VSR, IDM_COMPARE_SPLIT, IDM_COMPARE_WIPE,
                              IDM_COMPARE_DIFFERENCE, IDM_COMPARE_SIDE_BY_SIDE, IDM_COMPARE_QUAD};
    if (std::find(std::begin(modes), std::end(modes), selectedMode) == std::end(modes)) selectedMode = IDM_COMPARE_NEURAL;
    if (selectedMode == IDM_COMPARE_VSR && !vsrAvailable) selectedMode = IDM_COMPARE_NEURAL;
    bool ok = true;
    for (const UINT command : modes) {
        const bool available = modesAvailable && (command != IDM_COMPARE_VSR || vsrAvailable);
        ok = EnableMenuItem(menu, command, MF_BYCOMMAND | (available ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1) && ok;
    }
    ok = EnableMenuItem(menu, IDM_COMPARE_AGAINST_VSR, MF_BYCOMMAND | (modesAvailable && vsrAvailable ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1) && ok;
    ok = CheckMenuItem(menu, IDM_COMPARE_AGAINST_VSR, MF_BYCOMMAND | (vsrAvailable && againstVsr ? MF_CHECKED : MF_UNCHECKED)) != static_cast<DWORD>(-1) && ok;
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

bool UpdateVsrQualityMenu(HMENU menuBar, bool available, UINT index)
{
    const UINT last = IDM_COMPARE_VSR_QUALITY_FIRST + IDM_COMPARE_VSR_QUALITY_COUNT - 1;
    bool ok = true;
    for (UINT command = IDM_COMPARE_VSR_QUALITY_FIRST; command <= last; ++command)
        ok = EnableMenuItem(menuBar, command, MF_BYCOMMAND | (available ? MF_ENABLED : MF_GRAYED)) != static_cast<UINT>(-1) && ok;
    return CheckRadioCommand(menuBar, IDM_COMPARE_VSR_QUALITY_FIRST, last,
                             IDM_COMPARE_VSR_QUALITY_FIRST + std::min(index, IDM_COMPARE_VSR_QUALITY_COUNT - 1)) && ok;
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
        // The processing-scale and upscaling-history rungs use the accelerator column
        // for what each one measured ("14.8 fps", "63-94"), not for a key, and the
        // sheet listed them as keys.
        if (item.wID >= IDM_PROCESSING_SCALE_FIRST && item.wID <= IDM_PROCESSING_SCALE_LAST) continue;
        if (item.wID == IDM_UPSCALE_HISTORY_TEMPORAL || item.wID == IDM_UPSCALE_HISTORY_PER_FRAME) continue;
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
    return key_bindings::Defaults().LookupKey(key, controlDown, shiftDown);
}

} // namespace app_menu
