#include "TestSupport.h"

#include "RuntimePolicy.h"
#include "ReShadeConfig.h"
#include "Localization.h"
#include "AppMenu.h"
#include "UiLayout.h"
#include "UiResources.h"
#include "RuntimeLifetime.h"
#include "YouTubeResolver.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

struct YouTubeResolverTestAccess {
    static std::unique_ptr<YouTubeResolver> Create(
        const std::filesystem::path& helperDirectory,
        std::chrono::milliseconds deadline = std::chrono::milliseconds{500})
    {
        YouTubeResolver::Settings settings;
        settings.helperDirectory = helperDirectory;
        settings.deadline = deadline;
        settings.pollInterval = std::chrono::milliseconds{10};
        settings.shutdownWait = std::chrono::milliseconds{500};
        return std::unique_ptr<YouTubeResolver>(new YouTubeResolver(std::move(settings)));
    }
};

namespace {

constexpr std::string_view kNeuralAddon = "DLSS 5 Neural Rendering@renodx-dlss5.addon64";
constexpr std::string_view kNeuralAddonName = "DLSS 5 Neural Rendering";
constexpr std::string_view kNeuralAddonFilename = "renodx-dlss5.addon64";

void runtime_shutdown_releases_player_before_media_foundation_and_com_test()
{
    std::vector<int> observed;
    struct OwnedPlayer {
        std::vector<int>& order;
        ~OwnedPlayer() { order.push_back(1); }
    };

    const int result = RunPlayerRuntime(
        [&] {
            OwnedPlayer player{observed};
            return 27;
        },
        [&] { observed.push_back(2); },
        [&] { observed.push_back(3); });

    CHECK_EQ(27, result);
    CHECK_EQ(size_t{3}, observed.size());
    if (observed.size() == 3) {
        CHECK_EQ(1, observed[0]);
        CHECK_EQ(2, observed[1]);
        CHECK_EQ(3, observed[2]);
    }
}

void runtime_shutdown_rethrows_only_after_single_ordered_cleanup_test()
{
    std::vector<int> observed;
    int mediaFoundationShutdowns = 0;
    int comUninitializations = 0;
    bool caughtExpectedException = false;
    struct ExpectedFailure {};
    struct OwnedPlayer {
        std::vector<int>& order;
        ~OwnedPlayer() { order.push_back(1); }
    };

    try {
        RunPlayerRuntime(
            [&]() -> int {
                OwnedPlayer player{observed};
                throw ExpectedFailure{};
            },
            [&] {
                ++mediaFoundationShutdowns;
                observed.push_back(2);
            },
            [&] {
                ++comUninitializations;
                observed.push_back(3);
            });
    } catch (const ExpectedFailure&) {
        caughtExpectedException = true;
    }

    CHECK(caughtExpectedException);
    CHECK_EQ(1, mediaFoundationShutdowns);
    CHECK_EQ(1, comUninitializations);
    CHECK_EQ(size_t{3}, observed.size());
    if (observed.size() == 3) {
        CHECK_EQ(1, observed[0]);
        CHECK_EQ(2, observed[1]);
        CHECK_EQ(3, observed[2]);
    }
}

void write_binary_file(const std::filesystem::path& path, std::string_view content)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    CHECK(output.is_open());
    if (!output.is_open()) return;
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    CHECK(output.good());
    output.flush();
    CHECK(output.good());
    output.close();
    CHECK(!output.fail());
}

std::string read_binary_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    CHECK(input.is_open());
    if (!input.is_open()) return {};
    input.seekg(0, std::ios::end);
    CHECK(input.good());
    const std::streamoff size = input.tellg();
    CHECK(size >= 0);
    if (size < 0) return {};
    input.seekg(0, std::ios::beg);
    CHECK(input.good());
    std::string content(static_cast<size_t>(size), '\0');
    if (!content.empty()) input.read(content.data(), static_cast<std::streamsize>(content.size()));
    CHECK(input.good() || input.eof());
    CHECK_EQ(static_cast<std::streamsize>(content.size()), input.gcount());
    input.close();
    CHECK(!input.fail());
    return content;
}

void remove_file_if_present(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK(!error);
    CHECK(!std::filesystem::exists(path, error));
    CHECK(!error);
}

struct MenuEntry {
    std::wstring text;
    UINT command;
    UINT state;
};

void collect_menu_entries(HMENU menu, std::vector<MenuEntry>& entries)
{
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_STRING | MIIM_ID | MIIM_SUBMENU | MIIM_STATE;
        GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &item);

        std::wstring text(item.cch + 1, L'\0');
        item.dwTypeData = text.data();
        item.cch = static_cast<UINT>(text.size());
        GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &item);
        text.resize(item.cch);
        entries.push_back({std::move(text), item.wID, item.fState});
        if (item.hSubMenu) collect_menu_entries(item.hSubMenu, entries);
    }
}

bool has_menu_entry(const std::vector<MenuEntry>& entries, std::wstring_view text, UINT command)
{
    for (const auto& entry : entries) {
        if (entry.text == text && entry.command == command) return true;
    }
    return false;
}

bool has_menu_text(const std::vector<MenuEntry>& entries, std::wstring_view text)
{
    for (const auto& entry : entries) {
        if (entry.text == text) return true;
    }
    return false;
}

HMENU find_top_level_submenu(HMENU menu, std::wstring_view text)
{
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_STRING | MIIM_SUBMENU;
        GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &item);
        std::wstring label(item.cch + 1, L'\0');
        item.dwTypeData = label.data();
        item.cch = static_cast<UINT>(label.size());
        GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &item);
        label.resize(item.cch);
        if (label == text) return item.hSubMenu;
    }
    return nullptr;
}

std::vector<ToolbarAction> toolbar_actions(const std::vector<ToolbarItem>& items)
{
    std::vector<ToolbarAction> actions;
    actions.reserve(items.size());
    for (const auto& item : items) actions.push_back(item.action);
    return actions;
}

const ToolbarItem* find_toolbar_item(const std::vector<ToolbarItem>& items, ToolbarAction action)
{
    for (const auto& item : items) {
        if (item.action == action) return &item;
    }
    return nullptr;
}

void check_toolbar_items_do_not_overlap(const std::vector<ToolbarItem>& items)
{
    for (size_t left = 0; left < items.size(); ++left) {
        for (size_t right = left + 1; right < items.size(); ++right) {
            const RECT& a = items[left].bounds;
            const RECT& b = items[right].bounds;
            CHECK(a.right <= b.left || b.right <= a.left ||
                  a.bottom <= b.top || b.bottom <= a.top);
        }
    }
}

void toolbar_layout_selects_stable_action_sets_for_width_modes_test()
{
    const std::vector<ToolbarAction> requiredNarrow{
        ToolbarAction::Open,
        ToolbarAction::PlayPause,
        ToolbarAction::Mute,
        ToolbarAction::ToggleDlss,
        ToolbarAction::Fullscreen,
    };
    const std::vector<ToolbarAction> allActions{
        ToolbarAction::Open,
        ToolbarAction::Back10,
        ToolbarAction::PlayPause,
        ToolbarAction::Stop,
        ToolbarAction::Forward10,
        ToolbarAction::Mute,
        ToolbarAction::ToggleDlss,
        ToolbarAction::Aspect,
        ToolbarAction::Adjustments,
        ToolbarAction::DebugView,
        ToolbarAction::Fullscreen,
    };

    const auto narrow = LayoutToolbar(320, 180, 96);
    CHECK_EQ(requiredNarrow, toolbar_actions(narrow));
    for (const auto& item : narrow) CHECK(item.compact);

    const auto normal = LayoutToolbar(640, 180, 96);
    CHECK_EQ(allActions, toolbar_actions(normal));
    for (const auto& item : normal) CHECK(item.compact);

    const auto wide = LayoutToolbar(1000, 180, 96);
    CHECK_EQ(allActions, toolbar_actions(wide));
    for (const auto& item : wide) CHECK(!item.compact);
}

void toolbar_layout_preserves_group_separation_test()
{
    const auto items = LayoutToolbar(1000, 180, 96);
    const ToolbarItem* back = find_toolbar_item(items, ToolbarAction::Back10);
    const ToolbarItem* play = find_toolbar_item(items, ToolbarAction::PlayPause);
    const ToolbarItem* mute = find_toolbar_item(items, ToolbarAction::Mute);
    const ToolbarItem* dlss = find_toolbar_item(items, ToolbarAction::ToggleDlss);
    const ToolbarItem* adjustments = find_toolbar_item(items, ToolbarAction::Adjustments);
    const ToolbarItem* debug = find_toolbar_item(items, ToolbarAction::DebugView);
    const ToolbarItem* fullscreen = find_toolbar_item(items, ToolbarAction::Fullscreen);
    CHECK(back && play && mute && dlss && adjustments && debug && fullscreen);
    if (!(back && play && mute && dlss && adjustments && debug && fullscreen)) return;

    CHECK_EQ(4L, play->bounds.left - back->bounds.right);
    CHECK_EQ(12L, dlss->bounds.left - mute->bounds.right);
    CHECK_EQ(12L, debug->bounds.left - adjustments->bounds.right);
    CHECK_EQ(4L, fullscreen->bounds.left - debug->bounds.right);
}

void toolbar_layout_scales_hit_height_and_avoids_overlap_test()
{
    const auto narrow = LayoutToolbar(320, 180, 96);
    const auto normal = LayoutToolbar(640, 180, 96);
    const auto wide = LayoutToolbar(1000, 180, 96);
    const auto scaled = LayoutToolbar(960, 300, 144);

    for (const auto* items : {&narrow, &normal, &wide}) {
        CHECK(!items->empty());
        for (const auto& item : *items) {
            CHECK(item.bounds.bottom - item.bounds.top >= 36);
        }
        check_toolbar_items_do_not_overlap(*items);
    }
    CHECK(!scaled.empty());
    for (const auto& item : scaled) {
        CHECK(item.bounds.bottom - item.bounds.top >= 54);
    }
    check_toolbar_items_do_not_overlap(scaled);

    CHECK_EQ(16L, wide.front().bounds.left);
    CHECK(wide.back().bounds.right <= 1000 - 16);
    CHECK_EQ(24L, scaled.front().bounds.left);
    CHECK(scaled.back().bounds.right <= 960 - 24);
}

void toolbar_hit_testing_is_half_open_and_boundary_stable_test()
{
    const auto items = LayoutToolbar(640, 180, 96);
    CHECK(!items.empty());
    for (const auto& item : items) {
        const LONG middleX = item.bounds.left + (item.bounds.right - item.bounds.left) / 2;
        const LONG middleY = item.bounds.top + (item.bounds.bottom - item.bounds.top) / 2;
        CHECK_EQ(item.action, HitTestToolbar(items, POINT{item.bounds.left, middleY}));
        CHECK_EQ(item.action, HitTestToolbar(items, POINT{item.bounds.right - 1, middleY}));
        CHECK_EQ(ToolbarAction::None, HitTestToolbar(items, POINT{item.bounds.right, middleY}));
        CHECK_EQ(item.action, HitTestToolbar(items, POINT{middleX, item.bounds.bottom - 1}));
        CHECK_EQ(ToolbarAction::None, HitTestToolbar(items, POINT{middleX, item.bounds.bottom}));
    }
    CHECK_EQ(ToolbarAction::None, HitTestToolbar(items, POINT{-1, -1}));
}

void minimum_toolbar_client_width_owns_required_target_floor_across_dpi_test()
{
    struct Case {
        UINT dpi;
        int expectedClientWidth;
        int expectedTargetSize;
        int expectedGutter;
    };
    constexpr Case cases[]{
        {0, 244, 36, 16},
        {96, 244, 36, 16},
        {120, 305, 45, 20},
        {144, 366, 54, 24},
        {192, 488, 72, 32},
    };
    const std::vector<ToolbarAction> requiredNarrow{
        ToolbarAction::Open,
        ToolbarAction::PlayPause,
        ToolbarAction::Mute,
        ToolbarAction::ToggleDlss,
        ToolbarAction::Fullscreen,
    };

    for (const auto& test : cases) {
        CHECK_EQ(test.expectedClientWidth, MinimumToolbarClientWidth(test.dpi));
        const auto items = LayoutToolbar(test.expectedClientWidth, test.expectedTargetSize * 4, test.dpi);
        CHECK_EQ(requiredNarrow, toolbar_actions(items));
        CHECK_EQ(static_cast<size_t>(5), items.size());
        for (const auto& item : items) {
            CHECK(item.compact);
            CHECK(item.bounds.right - item.bounds.left >= test.expectedTargetSize);
            CHECK(item.bounds.bottom - item.bounds.top >= test.expectedTargetSize);
        }
        check_toolbar_items_do_not_overlap(items);
        if (!items.empty()) {
            CHECK_EQ(static_cast<LONG>(test.expectedGutter), items.front().bounds.left);
            CHECK_EQ(static_cast<LONG>(test.expectedClientWidth - test.expectedGutter),
                     items.back().bounds.right);
        }
    }
}

bool rectangles_intersect(const RECT& left, const RECT& right)
{
    return left.left < right.right && left.right > right.left &&
           left.top < right.bottom && left.bottom > right.top;
}

void volume_slider_never_intersects_compact_or_threshold_toolbar_test()
{
    for (const int width : {320, 640, 922}) {
        const auto items = LayoutToolbar(width, 180, 96);
        CHECK(!LayoutVolumeSlider(width, 180, 96, items).has_value());
    }

    const auto thresholdItems = LayoutToolbar(923, 180, 96);
    const auto slider = LayoutVolumeSlider(923, 180, 96, thresholdItems);
    CHECK(slider.has_value());
    if (!slider) return;
    for (const auto& item : thresholdItems) {
        CHECK(!rectangles_intersect(*slider, item.bounds));
    }
    CHECK_EQ(738L, slider->left);
    CHECK_EQ(828L, slider->right);
}

void toolbar_focus_order_includes_idle_open_and_skips_disabled_actions_test()
{
    const std::array idleItems{ToolbarItem{ToolbarAction::Open, RECT{0, 0, 190, 42}, false}};
    const ToolbarAvailability idle{};
    CHECK(IsToolbarActionEnabled(ToolbarAction::Open, idle));
    CHECK_EQ(ToolbarAction::Open,
             NextFocusableToolbarAction(idleItems, ToolbarAction::None, false, idle));
    CHECK_EQ(ToolbarAction::Open,
             NextFocusableToolbarAction(idleItems, ToolbarAction::None, true, idle));
    CHECK_EQ(ToolbarAction::Open,
             NextFocusableToolbarAction(idleItems, ToolbarAction::Open, true, idle));

    const auto loadedItems = LayoutToolbar(640, 180, 96);
    const ToolbarAvailability withoutRenderer{true, false, false};
    CHECK(!IsToolbarActionEnabled(ToolbarAction::ToggleDlss, withoutRenderer));
    CHECK(!IsToolbarActionEnabled(ToolbarAction::Adjustments, withoutRenderer));
    CHECK(!IsToolbarActionEnabled(ToolbarAction::DebugView, withoutRenderer));
    const ToolbarAvailability seeking{true, true, true};
    CHECK(!IsToolbarActionEnabled(ToolbarAction::Aspect, seeking));
    CHECK(IsToolbarActionEnabled(ToolbarAction::Adjustments, seeking));
    CHECK(IsToolbarActionEnabled(ToolbarAction::DebugView, seeking));
    CHECK_EQ(ToolbarAction::Adjustments,
             NextFocusableToolbarAction(loadedItems, ToolbarAction::Mute, false,
                                        seeking));
}

void open_action_content_keeps_idle_and_toolbar_copy_distinct_test()
{
    Localizer localizer;
    const std::wstring idle = localizer.Get(OpenActionLabelKey(true).data());
    const std::wstring toolbar = localizer.Get(OpenActionLabelKey(false).data());

    CHECK_EQ(std::wstring(L"Open file"), idle);
    CHECK_EQ(std::wstring(L"Open"), toolbar);
    CHECK(idle != toolbar);
    CHECK(idle.find(L"Abrir") == std::wstring::npos);
    CHECK(toolbar.find(L"Abrir") == std::wstring::npos);
}

void focused_toolbar_action_reconciles_layout_and_availability_changes_test()
{
    const ToolbarAvailability loaded{true, false, true, false};
    const auto wide = LayoutToolbar(1200, 180, 96);
    const auto narrow = LayoutToolbar(320, 180, 96);
    const auto contains = [](std::span<const ToolbarItem> items, ToolbarAction action) {
        return std::any_of(items.begin(), items.end(),
                           [action](const ToolbarItem& item) { return item.action == action; });
    };

    CHECK(contains(wide, ToolbarAction::DebugView));
    CHECK(!contains(narrow, ToolbarAction::DebugView));
    CHECK(contains(wide, ToolbarAction::ToggleDlss));
    CHECK(contains(wide, ToolbarAction::PlayPause));

    CHECK_EQ(ToolbarAction::Open,
             ReconcileFocusedToolbarAction(narrow, ToolbarAction::DebugView, loaded));

    ToolbarAvailability withoutRenderer = loaded;
    withoutRenderer.rendererReady = false;
    CHECK_EQ(ToolbarAction::Open,
             ReconcileFocusedToolbarAction(wide, ToolbarAction::ToggleDlss,
                                            withoutRenderer));

    CHECK_EQ(ToolbarAction::PlayPause,
             ReconcileFocusedToolbarAction(wide, ToolbarAction::PlayPause, loaded));
}

void idle_surface_exposes_file_and_disabled_youtube_without_focusing_it_test()
{
    const IdleSurfaceLayout wide = LayoutIdleSurface(900, 520, 96);
    CHECK(!wide.stacked);
    CHECK_EQ(ToolbarAction::Open, wide.actions[0].action);
    CHECK_EQ(ToolbarAction::OpenYouTube, wide.actions[1].action);
    CHECK(wide.actions[0].bounds.right <= wide.actions[1].bounds.left);

    const IdleSurfaceLayout small = LayoutIdleSurface(320, 360, 96);
    CHECK(small.stacked);
    for (const auto& action : small.actions) {
        CHECK(action.bounds.left >= 0);
        CHECK(action.bounds.top >= 0);
        CHECK(action.bounds.right <= 320);
        CHECK(action.bounds.bottom <= 360);
        CHECK(action.bounds.right > action.bounds.left);
        CHECK(action.bounds.bottom > action.bounds.top);
    }
    CHECK(small.actions[0].bounds.bottom <= small.actions[1].bounds.top);
    CHECK(small.youtubeReason.bottom <= 360);

    struct HeightCase { UINT dpi; int clientHeight; int hitHeight; };
    constexpr HeightCase heights[]{
        {96, 150, 36},
        {120, 188, 45},
        {144, 225, 54},
        {192, 300, 72},
    };
    for (const auto& test : heights) {
        CHECK_EQ(test.clientHeight, MinimumIdleClientHeight(test.dpi));
        const IdleSurfaceLayout shortLayout = LayoutIdleSurface(
            MinimumToolbarClientWidth(test.dpi), test.clientHeight, test.dpi);
        CHECK(shortLayout.stacked);
        CHECK_EQ(shortLayout.subtitle.top, shortLayout.subtitle.bottom);
        for (const auto& action : shortLayout.actions) {
            CHECK(action.bounds.top >= 0);
            CHECK(action.bounds.bottom <= test.clientHeight);
            CHECK(action.bounds.bottom - action.bounds.top >= test.hitHeight);
        }
        CHECK(shortLayout.youtubeReason.top >= shortLayout.actions[1].bounds.bottom);
        CHECK(shortLayout.youtubeReason.bottom >= shortLayout.youtubeReason.top);
        CHECK(shortLayout.youtubeReason.bottom <= test.clientHeight);
    }

    const bool youtubeAvailable = YouTubePlaybackAvailable();
    CHECK(!youtubeAvailable);
    const ToolbarAvailability intermediate{false, false, false, youtubeAvailable};
    CHECK(IsToolbarActionEnabled(ToolbarAction::Open, intermediate));
    CHECK(!IsToolbarActionEnabled(ToolbarAction::OpenYouTube, intermediate));
    CHECK_EQ(ToolbarAction::Open,
             NextFocusableToolbarAction(small.actions, ToolbarAction::None, false,
                                        intermediate));
    CHECK_EQ(ToolbarAction::Open,
             NextFocusableToolbarAction(small.actions, ToolbarAction::Open, false,
                                        intermediate));

    ToolbarAvailability later = intermediate;
    later.youtubeAvailable = true;
    CHECK_EQ(ToolbarAction::OpenYouTube,
             NextFocusableToolbarAction(small.actions, ToolbarAction::Open, false, later));
}

void dpi_change_suggested_rect_respects_new_monitor_minimum_track_size_test()
{
    const RECT tooSmall{120, 80, 520, 300};
    const RECT clamped = ClampWindowRectToMinimumTrackSize(tooSmall, POINT{451, 361});
    CHECK_EQ(120L, clamped.left);
    CHECK_EQ(80L, clamped.top);
    CHECK_EQ(571L, clamped.right);
    CHECK_EQ(441L, clamped.bottom);

    const RECT alreadyLarge{120, 80, 700, 600};
    const RECT unchanged = ClampWindowRectToMinimumTrackSize(alreadyLarge, POINT{451, 361});
    CHECK_EQ(alreadyLarge.left, unchanged.left);
    CHECK_EQ(alreadyLarge.top, unchanged.top);
    CHECK_EQ(alreadyLarge.right, unchanged.right);
    CHECK_EQ(alreadyLarge.bottom, unchanged.bottom);
}

void player_status_formats_exact_runtime_and_playback_states_test()
{
    PlayerStatusSnapshot status{};
    CHECK(BuildPlayerStatusText(status).empty());

    status.activity = PlayerStatusActivity::ResolvingYouTube;
    CHECK_EQ(std::wstring(L"Resolving YouTube\u2026"), BuildPlayerStatusText(status));

    status.activity = PlayerStatusActivity::None;
    status.mediaLoaded = true;
    status.runtimeConfiguration = PlayerRuntimeConfiguration::NeuralAddonExperimental;
    status.dlssState = PlayerDlssState::Active;
    status.sourceWidth = 1920;
    status.sourceHeight = 1080;
    status.inputWidth = 1280;
    status.inputHeight = 720;
    status.outputWidth = 3840;
    status.outputHeight = 2160;
    status.quality = L"Quality";
    status.renderedFps = 58.4;
    status.sourceFps = 59.94;
    status.droppedFrames = 3;
    CHECK_EQ(std::wstring(L"Neural addon enabled (experimental) \u00b7 DLSS SR active \u00b7 Source 1920\u00d71080 \u00b7 Input 1280\u00d7720 \u00b7 Output 3840\u00d72160 \u00b7 Quality \u00b7 FPS 58 rendered / 60 source \u00b7 Dropped 3"),
             BuildPlayerStatusText(status));

    status.runtimeConfiguration = PlayerRuntimeConfiguration::DlssSrSafeMode;
    status.dlssState = PlayerDlssState::Active;
    CHECK(BuildPlayerStatusText(status).starts_with(L"DLSS SR safe mode \u00b7 DLSS SR active \u00b7"));
    status.dlssState = PlayerDlssState::ScalerFallback;
    CHECK(BuildPlayerStatusText(status).starts_with(L"DLSS SR safe mode \u00b7 Scaler fallback \u00b7"));

    const PlayerRuntimeStatus neuralActive = ResolvePlayerRuntimeStatus(false, true, true, true);
    CHECK_EQ(PlayerRuntimeConfiguration::NeuralAddonExperimental, neuralActive.configuration);
    CHECK_EQ(PlayerDlssState::Active, neuralActive.dlssState);
    const PlayerRuntimeStatus neuralFallback = ResolvePlayerRuntimeStatus(false, true, false, true);
    CHECK_EQ(PlayerRuntimeConfiguration::NeuralAddonExperimental, neuralFallback.configuration);
    CHECK_EQ(PlayerDlssState::ScalerFallback, neuralFallback.dlssState);
    const PlayerRuntimeStatus safeActive = ResolvePlayerRuntimeStatus(true, false, true, true);
    CHECK_EQ(PlayerRuntimeConfiguration::DlssSrSafeMode, safeActive.configuration);
    CHECK_EQ(PlayerDlssState::Active, safeActive.dlssState);
    const PlayerRuntimeStatus safeFallback = ResolvePlayerRuntimeStatus(true, false, true, false);
    CHECK_EQ(PlayerRuntimeConfiguration::DlssSrSafeMode, safeFallback.configuration);
    CHECK_EQ(PlayerDlssState::ScalerFallback, safeFallback.dlssState);
}

void long_media_title_is_bounded_with_a_real_ellipsis_test()
{
    const std::wstring longTitle(240, L'X');
    const std::wstring title = BuildPlayerWindowTitle(L"DLSS Video Player", longTitle, 64);
    CHECK_EQ(static_cast<size_t>(64), title.size());
    CHECK(title.starts_with(L"DLSS Video Player \u2014 "));
    CHECK_EQ(L'\u2026', title.back());
    CHECK_EQ(std::wstring(L"DLSS Video Player"),
             BuildPlayerWindowTitle(L"DLSS Video Player", L"", 64));
}

void recovery_copy_and_rehook_confirmation_are_actionable_test()
{
    Localizer localizer;
    const std::wstring decode = localizer.Get(L"error.decode");
    const std::wstring renderer = localizer.Get(L"error.renderer");
    const std::wstring rehook = localizer.Get(L"rehook.confirm");
    const std::wstring compactYoutubeReason = localizer.Get(L"idle.youtube_unavailable_compact");
    CHECK(decode.find(L"bundled FFmpeg") != std::wstring::npos);
    CHECK(decode.find(L"try again") != std::wstring::npos);
    CHECK(renderer.find(L"NVIDIA driver") != std::wstring::npos);
    CHECK(renderer.find(L"safe mode") != std::wstring::npos);
    CHECK(rehook.find(L"reset playback") != std::wstring::npos);
    CHECK(rehook.find(L"hang") != std::wstring::npos);
    CHECK(rehook.find(L"experimental neural add-on") != std::wstring::npos);
    CHECK_EQ(std::wstring(L"YouTube unavailable in this build."), compactYoutubeReason);
    int recreateRequests = 0;
    const auto request = [&] { ++recreateRequests; };
    CHECK(!ExecuteGuardedRehook(IDNO, request));
    CHECK(!ExecuteGuardedRehook(IDCANCEL, request));
    CHECK_EQ(0, recreateRequests);
    CHECK(ExecuteGuardedRehook(IDYES, request));
    CHECK_EQ(1, recreateRequests);
    CHECK(app_menu::RoutesToRehook(app_menu::PlayerCommandRoute::KeyDown, VK_F6));
    CHECK(app_menu::RoutesToRehook(app_menu::PlayerCommandRoute::NativeMenu, app_menu::IDM_REHOOK));
    CHECK(!app_menu::RoutesToRehook(app_menu::PlayerCommandRoute::KeyDown, 'R'));
    CHECK(!app_menu::RoutesToRehook(app_menu::PlayerCommandRoute::NativeMenu, app_menu::IDM_OPEN));
}

void unchanged_hover_action_has_no_dirty_rectangles_test()
{
    const std::array items{
        ToolbarItem{ToolbarAction::Open, RECT{10, 20, 80, 56}, false},
        ToolbarItem{ToolbarAction::PlayPause, RECT{84, 20, 146, 56}, false},
    };

    CHECK(HoverDirtyRectangles(items, ToolbarAction::Open, ToolbarAction::Open).empty());
    CHECK(HoverDirtyRectangles(items, ToolbarAction::None, ToolbarAction::None).empty());
}

void changed_hover_action_dirties_only_present_old_and_new_actions_test()
{
    const RECT openBounds{10, 20, 80, 56};
    const RECT playBounds{84, 20, 146, 56};
    const std::array items{
        ToolbarItem{ToolbarAction::Open, openBounds, false},
        ToolbarItem{ToolbarAction::PlayPause, playBounds, false},
    };

    const auto changed = HoverDirtyRectangles(items, ToolbarAction::Open,
                                               ToolbarAction::PlayPause);
    CHECK_EQ(2u, changed.size());
    if (changed.size() == 2) {
        CHECK_EQ(openBounds.left, changed[0].left);
        CHECK_EQ(openBounds.top, changed[0].top);
        CHECK_EQ(openBounds.right, changed[0].right);
        CHECK_EQ(openBounds.bottom, changed[0].bottom);
        CHECK_EQ(playBounds.left, changed[1].left);
        CHECK_EQ(playBounds.top, changed[1].top);
        CHECK_EQ(playBounds.right, changed[1].right);
        CHECK_EQ(playBounds.bottom, changed[1].bottom);
    }

    const auto entered = HoverDirtyRectangles(items, ToolbarAction::None,
                                               ToolbarAction::PlayPause);
    CHECK_EQ(1u, entered.size());
    if (entered.size() == 1) {
        CHECK_EQ(playBounds.left, entered[0].left);
        CHECK_EQ(playBounds.right, entered[0].right);
    }

    const auto left = HoverDirtyRectangles(items, ToolbarAction::Open,
                                            ToolbarAction::None);
    CHECK_EQ(1u, left.size());
    if (left.size() == 1) {
        CHECK_EQ(openBounds.left, left[0].left);
        CHECK_EQ(openBounds.right, left[0].right);
    }

    const auto absent = HoverDirtyRectangles(items, ToolbarAction::Stop,
                                              ToolbarAction::Forward10);
    CHECK(absent.empty());
}

void hover_resolution_tracks_layout_action_changes_and_disappearance_test()
{
    const POINT point{35, 35};
    const ToolbarAvailability available{true, false, true};
    const std::array oldLayout{
        ToolbarItem{ToolbarAction::Open, RECT{10, 20, 80, 56}, false},
    };
    const std::array changedLayout{
        ToolbarItem{ToolbarAction::PlayPause, RECT{10, 20, 80, 56}, false},
    };
    const std::array<ToolbarItem, 0> disappearedLayout{};

    CHECK_EQ(ToolbarAction::Open,
             ResolveToolbarHover(oldLayout, point, available));
    CHECK_EQ(ToolbarAction::PlayPause,
             ResolveToolbarHover(changedLayout, point, available));
    CHECK_EQ(ToolbarAction::None,
             ResolveToolbarHover(disappearedLayout, point, available));

    const ToolbarAvailability disabledTransport{true, true, true};
    CHECK_EQ(ToolbarAction::None,
             ResolveToolbarHover(changedLayout, point, disabledTransport));
}

void current_cursor_hover_clears_when_cursor_query_is_unavailable_test()
{
    const std::array layout{
        ToolbarItem{ToolbarAction::Open, RECT{10, 20, 80, 56}, false},
    };
    const ToolbarAvailability available{true, false, true};
    CHECK_EQ(ToolbarAction::Open,
             ResolveToolbarHoverForCursor(layout, POINT{35, 35}, available));
    CHECK_EQ(ToolbarAction::None,
             ResolveToolbarHoverForCursor(layout, std::nullopt, available));
}

void paint_buffer_layout_uses_only_the_clipped_nonzero_paint_rectangle_test()
{
    const RECT client{0, 0, 3840, 2160};
    const auto partial = LayoutPaintBuffer(client, RECT{3011, 1990, 3039, 2018});
    CHECK(partial.has_value());
    if (partial) {
        CHECK_EQ(3011L, partial->paintBounds.left);
        CHECK_EQ(1990L, partial->paintBounds.top);
        CHECK_EQ(3039L, partial->paintBounds.right);
        CHECK_EQ(2018L, partial->paintBounds.bottom);
        CHECK_EQ(28, partial->width);
        CHECK_EQ(28, partial->height);
        CHECK_EQ(-3011L, partial->viewportOrigin.x);
        CHECK_EQ(-1990L, partial->viewportOrigin.y);
    }

    const auto clipped = LayoutPaintBuffer(client, RECT{-20, 2140, 50, 2200});
    CHECK(clipped.has_value());
    if (clipped) {
        CHECK_EQ(0L, clipped->paintBounds.left);
        CHECK_EQ(2140L, clipped->paintBounds.top);
        CHECK_EQ(50L, clipped->paintBounds.right);
        CHECK_EQ(2160L, clipped->paintBounds.bottom);
        CHECK_EQ(50, clipped->width);
        CHECK_EQ(20, clipped->height);
        CHECK_EQ(0L, clipped->viewportOrigin.x);
        CHECK_EQ(-2140L, clipped->viewportOrigin.y);
    }

    CHECK(!LayoutPaintBuffer(client, RECT{4000, 2300, 4010, 2310}).has_value());
    CHECK(!LayoutPaintBuffer(client, RECT{120, 120, 120, 160}).has_value());
}

void tabler_glyph_mapping_uses_the_pinned_css_codepoints_test()
{
    CHECK_EQ(L'\xfaf7', GlyphForIcon(UiIcon::Open));
    CHECK_EQ(L'\xfaba', GlyphForIcon(UiIcon::Rewind));
    CHECK_EQ(L'\xed46', GlyphForIcon(UiIcon::Play));
    CHECK_EQ(L'\xed45', GlyphForIcon(UiIcon::Pause));
    CHECK_EQ(L'\xed4a', GlyphForIcon(UiIcon::Stop));
    CHECK_EQ(L'\xfac2', GlyphForIcon(UiIcon::FastForward));
    CHECK_EQ(L'\xeb51', GlyphForIcon(UiIcon::Volume));
    CHECK_EQ(L'\xf1c3', GlyphForIcon(UiIcon::VolumeOff));
    CHECK_EQ(L'\xf6d7', GlyphForIcon(UiIcon::Sparkles));
    CHECK_EQ(L'\xea85', GlyphForIcon(UiIcon::Crop));
    CHECK_EQ(L'\xea03', GlyphForIcon(UiIcon::Adjustments));
    CHECK_EQ(L'\xea48', GlyphForIcon(UiIcon::Debug));
    CHECK_EQ(L'\xeaea', GlyphForIcon(UiIcon::Maximize));
    CHECK_EQ(L'\xec90', GlyphForIcon(UiIcon::YouTube));
    CHECK_EQ(L'\xea06', GlyphForIcon(UiIcon::Warning));
}

void native_button_palette_has_distinct_interaction_states_test()
{
    const ButtonVisual defaultVisual = ResolveButtonVisual({});
    CHECK_EQ(RGB(47, 49, 53), defaultVisual.fill);
    CHECK_EQ(RGB(240, 240, 242), defaultVisual.text);

    ButtonState state{};
    state.hover = true;
    CHECK_EQ(RGB(62, 65, 70), ResolveButtonVisual(state).fill);
    state.pressed = true;
    CHECK_EQ(RGB(27, 28, 31), ResolveButtonVisual(state).fill);
    state.pressed = false;
    state.active = true;
    const ButtonVisual active = ResolveButtonVisual(state);
    CHECK_EQ(RGB(55, 139, 226), active.fill);
    CHECK(active.text != RGB(240, 240, 242));
    state.hover = true;
    CHECK_EQ(active.fill, ResolveButtonVisual(state).fill);
    state.pressed = true;
    CHECK_EQ(RGB(27, 28, 31), ResolveButtonVisual(state).fill);
    state.focus = true;
    state.enabled = false;
    const ButtonVisual disabled = ResolveButtonVisual(state);
    CHECK_EQ(RGB(27, 28, 31), disabled.fill);
    CHECK_EQ(RGB(160, 164, 172), disabled.text);
    CHECK(disabled.drawFocus == state.focus);
}

double linear_color_channel(BYTE value)
{
    const double channel = static_cast<double>(value) / 255.0;
    return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

double contrast_ratio(COLORREF foreground, COLORREF background)
{
    const auto luminance = [](COLORREF color) {
        return 0.2126 * linear_color_channel(GetRValue(color)) +
               0.7152 * linear_color_channel(GetGValue(color)) +
               0.0722 * linear_color_channel(GetBValue(color));
    };
    const double foregroundLuminance = luminance(foreground);
    const double backgroundLuminance = luminance(background);
    return (std::max(foregroundLuminance, backgroundLuminance) + 0.05) /
           (std::min(foregroundLuminance, backgroundLuminance) + 0.05);
}

void active_button_small_text_meets_wcag_contrast_test()
{
    ButtonState state{};
    state.active = true;
    const ButtonVisual active = ResolveButtonVisual(state);
    CHECK(contrast_ratio(active.text, active.fill) >= 4.5);
}

void failed_icon_font_uses_label_only_presentation_test()
{
    UiResources resources;
    CHECK(!resources.Load(nullptr));
    CHECK(resources.CreateIconFont(96) == nullptr);
    CHECK(ResolveButtonPresentation(false) == ButtonPresentation::LabelOnly);
    CHECK(ResolveButtonPresentation(true) == ButtonPresentation::IconAndLabel);
}

void debug_view_popup_contains_all_existing_views_and_selection_test()
{
    const HMENU menu = app_menu::CreateDebugViewMenu(app_menu::IDM_VIEW_DEPTH);
    CHECK(menu != nullptr);
    CHECK_EQ(5, menu ? GetMenuItemCount(menu) : 0);
    std::vector<MenuEntry> entries;
    if (menu) collect_menu_entries(menu, entries);
    CHECK(has_menu_entry(entries, L"Final output\t1", app_menu::IDM_VIEW_FINAL));
    CHECK(has_menu_entry(entries, L"DLSS input\t2", app_menu::IDM_VIEW_INPUT));
    CHECK(has_menu_entry(entries, L"Motion vectors\t3", app_menu::IDM_VIEW_MV));
    CHECK(has_menu_entry(entries, L"Depth\t4", app_menu::IDM_VIEW_DEPTH));
    CHECK(has_menu_entry(entries, L"Bias mask\t5", app_menu::IDM_VIEW_MASK));
    for (const auto& entry : entries) {
        if (entry.command == app_menu::IDM_VIEW_DEPTH) CHECK((entry.state & MFS_CHECKED) != 0);
        else CHECK((entry.state & MFS_CHECKED) == 0);
    }
    if (menu) DestroyMenu(menu);
}

void player_menu_is_english_only_and_retains_advanced_commands_test()
{
    Localizer localizer;
    const HMENU menu = app_menu::CreateMenuBar(localizer, YouTubePlaybackAvailable());
    CHECK(menu != nullptr);

    std::vector<MenuEntry> entries;
    if (menu) collect_menu_entries(menu, entries);
    CHECK(!has_menu_text(entries, L"Language"));
    CHECK(has_menu_text(entries, L"Advanced"));
    CHECK(has_menu_entry(entries, L"Open file\tCtrl+O", app_menu::IDM_OPEN));
    CHECK(has_menu_entry(entries, L"Open YouTube URL\u2026\tCtrl+L", app_menu::IDM_OPEN_YOUTUBE));
    CHECK(has_menu_entry(entries, L"Stop\tS", app_menu::IDM_STOP));
    CHECK(has_menu_entry(entries, L"Original aspect ratio (Fit)\tA", app_menu::IDM_ASPECT_FIT));
    CHECK(has_menu_entry(entries, L"Restart in DLSS SR safe mode", app_menu::IDM_ADVANCED_SAFE_MODE));
    CHECK(has_menu_entry(entries, L"Recreate NGX / re-hook DLSS 5\tF6", app_menu::IDM_REHOOK));
    HMENU advanced = find_top_level_submenu(menu, L"Advanced");
    CHECK(advanced != nullptr);
    if (advanced) {
        std::vector<MenuEntry> advancedEntries;
        collect_menu_entries(advanced, advancedEntries);
        CHECK(has_menu_entry(advancedEntries, L"Recreate NGX / re-hook DLSS 5\tF6", app_menu::IDM_REHOOK));
    }
    for (const auto& entry : entries) {
        if (entry.command == app_menu::IDM_OPEN_YOUTUBE) {
            CHECK((entry.state & (MFS_DISABLED | MFS_GRAYED)) != 0);
        }
    }
    for (const auto& entry : entries) {
        CHECK(entry.command < 500 || entry.command >= 600);
    }

    if (menu) DestroyMenu(menu);
}

void youtube_availability_drives_real_menu_and_idle_action_consistently_test()
{
    Localizer localizer;
    for (const bool available : {false, true}) {
        const HMENU menu = app_menu::CreateMenuBar(localizer, available);
        CHECK(menu != nullptr);
        std::vector<MenuEntry> entries;
        if (menu) collect_menu_entries(menu, entries);
        bool found = false;
        for (const auto& entry : entries) {
            if (entry.command != app_menu::IDM_OPEN_YOUTUBE) continue;
            found = true;
            const bool disabled = (entry.state & (MFS_DISABLED | MFS_GRAYED)) != 0;
            CHECK_EQ(!available, disabled);
        }
        CHECK(found);
        ToolbarAvailability state{};
        state.youtubeAvailable = available;
        CHECK_EQ(available, IsToolbarActionEnabled(ToolbarAction::OpenYouTube, state));
        if (menu) DestroyMenu(menu);
    }
}

std::filesystem::path executable_directory()
{
    wchar_t executablePath[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
    return std::filesystem::path(executablePath, executablePath + length).parent_path();
}

struct RestoredFile {
    explicit RestoredFile(std::filesystem::path file)
        : path(std::move(file))
    {
        std::error_code error;
        existed = std::filesystem::exists(path, error);
        CHECK(!error);
        if (existed) original = read_binary_file(path);
    }

    ~RestoredFile()
    {
        Restore();
    }

    void Restore()
    {
        if (restored) return;
        std::error_code error;
        if (existed) {
            write_binary_file(path, original);
            CHECK_EQ(original, read_binary_file(path));
        } else {
            std::filesystem::remove(path, error);
            CHECK(!error);
            CHECK(!std::filesystem::exists(path, error));
            CHECK(!error);
        }
        restored = true;
    }

    std::filesystem::path path;
    bool existed = false;
    std::string original;
    bool restored = false;
};

void legacy_language_configuration_is_ignored_and_english_lookup_remains_builtin_test()
{
    const std::filesystem::path runtimeDirectory = executable_directory();
    const std::filesystem::path configuration = runtimeDirectory / "DLSSVideoPlayer.ini";
    const std::filesystem::path languageDirectory = runtimeDirectory / "languages";
    const std::filesystem::path portuguesePack = languageDirectory / "pt-BR.lang";
    std::error_code error;
    const bool languageDirectoryExisted = std::filesystem::exists(languageDirectory, error);
    CHECK(!error);
    CHECK(!languageDirectoryExisted);
    std::filesystem::create_directories(languageDirectory, error);
    CHECK(!error);
    RestoredFile restoreConfiguration(configuration);
    RestoredFile restorePortuguesePack(portuguesePack);
    write_binary_file(configuration, "[General]\r\nLanguage=pt-BR\r\n");
    write_binary_file(portuguesePack, "app.title=Leitor em Portugues\r\nmenu.file=Arquivo\r\n");

    Localizer localizer;
    localizer.Initialize();

    CHECK_EQ(std::wstring(L"DLSS Video Player"), localizer.Get(L"app.title"));
    CHECK_EQ(std::wstring(L"File"), localizer.Get(L"menu.file"));

    restorePortuguesePack.Restore();
    restoreConfiguration.Restore();
    if (!languageDirectoryExisted) {
        std::filesystem::remove(languageDirectory, error);
        CHECK(!error);
        CHECK(!std::filesystem::exists(languageDirectory, error));
        CHECK(!error);
    }
}

void harness_sanity_test()
{
    CHECK(true);
    CHECK_EQ(2 + 2, 4);
}

void gpu_classification_table_test()
{
    struct Case {
        uint32_t vendor_id;
        std::wstring_view description;
        GpuGeneration expected;
    };

    constexpr Case cases[] = {
        {0x10DE, L"NVIDIA GeForce RTX 4090", GpuGeneration::Rtx40Ada},
        {0x10DE, L"NVIDIA GeForce RTX 4090 Laptop GPU", GpuGeneration::Rtx40Ada},
        {0x10DE, L"nViDiA gEfOrCe rTx 5090", GpuGeneration::Rtx50Blackwell},
        {0x10DE, L"NVIDIA GeForce RTX 3090", GpuGeneration::OtherNvidia},
        {0x1002, L"AMD Radeon RX 7900 XTX", GpuGeneration::Unsupported},
        {0x8086, L"Intel(R) Arc(TM) A770 Graphics", GpuGeneration::Unsupported},
        {0x10DE, L"", GpuGeneration::OtherNvidia},
        {0, L"GeForce RTX 5090", GpuGeneration::Unsupported},
        {0, L"", GpuGeneration::Unsupported},
    };

    for (const auto& test : cases) {
        CHECK_EQ(test.expected, ClassifyGpu(test.vendor_id, test.description));
    }
}

void neural_addon_policy_test()
{
    CHECK(NeuralAddonDesired(GpuGeneration::Rtx40Ada, false));
    CHECK(NeuralAddonDesired(GpuGeneration::Rtx50Blackwell, false));
    CHECK(!NeuralAddonDesired(GpuGeneration::Rtx40Ada, true));
    CHECK(!NeuralAddonDesired(GpuGeneration::Rtx50Blackwell, true));
    CHECK(!NeuralAddonDesired(GpuGeneration::OtherNvidia, false));
    CHECK(!NeuralAddonDesired(GpuGeneration::Unsupported, false));
}

void bootstrap_action_matrix_test()
{
    struct Case {
        bool desired_enabled;
        bool config_enabled;
        bool already_restarted;
        bool update_succeeded;
        BootstrapAction expected;
    };

    constexpr Case cases[] = {
        {true, true, false, true, BootstrapAction::Continue},
        {false, false, false, true, BootstrapAction::Continue},
        {true, true, true, true, BootstrapAction::Continue},
        {false, false, true, true, BootstrapAction::Continue},
        {true, false, false, true, BootstrapAction::Relaunch},
        {false, true, false, true, BootstrapAction::Relaunch},
        {true, false, true, true, BootstrapAction::Fail},
        {false, true, true, true, BootstrapAction::Fail},
        {true, true, false, false, BootstrapAction::Fail},
        {false, true, false, false, BootstrapAction::Fail},
        {true, false, true, false, BootstrapAction::Fail},
    };

    for (const auto& test : cases) {
        CHECK_EQ(test.expected, DecideBootstrap(
            test.desired_enabled,
            test.config_enabled,
            test.already_restarted,
            test.update_succeeded));
    }
}

void windows_command_line_quoting_round_trip_test()
{
    constexpr std::wstring_view executable = L"C:\\Program Files\\DLSS Player\\DLSSVideoPlayer.exe";
    const std::vector<std::wstring> arguments = {
        L"movie.mp4",
        L"C:\\Videos\\clip with spaces.mp4",
        L"",
        L"--future-option=\"quoted value\"",
        L"C:\\trailing slash\\",
        L"plain\\slashes",
        L"embedded\"quote",
    };
    constexpr std::wstring_view expected =
        L"\"C:\\Program Files\\DLSS Player\\DLSSVideoPlayer.exe\" "
        L"\"movie.mp4\" "
        L"\"C:\\Videos\\clip with spaces.mp4\" "
        L"\"\" "
        L"\"--future-option=\\\"quoted value\\\"\" "
        L"\"C:\\trailing slash\\\\\" "
        L"\"plain\\slashes\" "
        L"\"embedded\\\"quote\"";

    const std::wstring commandLine = BuildWindowsCommandLine(executable, arguments);
    CHECK_EQ(std::wstring(expected), commandLine);

    int parsedCount = 0;
    LPWSTR* parsed = CommandLineToArgvW(commandLine.c_str(), &parsedCount);
    CHECK(parsed != nullptr);
    if (parsed) {
        CHECK_EQ(static_cast<int>(arguments.size() + 1), parsedCount);
        if (parsedCount == static_cast<int>(arguments.size() + 1)) {
            CHECK_EQ(std::wstring(executable), std::wstring(parsed[0]));
            for (size_t index = 0; index < arguments.size(); ++index) {
                CHECK_EQ(arguments[index], std::wstring(parsed[index + 1]));
            }
        }
        LocalFree(parsed);
    }
}

void runtime_argument_parsing_preserves_user_arguments_and_strips_markers_test()
{
    const wchar_t* argv[] = {
        L"C:\\Program Files\\DLSS Player\\DLSSVideoPlayer.exe",
        L"--future-flag",
        L"",
        L"C:\\Videos\\clip with spaces.mp4",
        L"quoted\"value",
        L"--safe-mode",
        L"--addon-bootstrap-restarted",
        L"--safe-mode",
        L"--addon-bootstrap-restarted",
    };
    const std::vector<std::wstring> expected = {
        L"--future-flag",
        L"",
        L"C:\\Videos\\clip with spaces.mp4",
        L"quoted\"value",
        L"--safe-mode",
    };

    const RuntimeArguments parsed = ParseRuntimeArguments(static_cast<int>(std::size(argv)), argv);
    CHECK(parsed.ok);
    CHECK(parsed.safeMode);
    CHECK(parsed.addonBootstrapRestarted);
    CHECK(parsed.error.empty());
    CHECK_EQ(expected.size(), parsed.userArguments.size());
    if (parsed.userArguments.size() == expected.size()) {
        for (size_t index = 0; index < expected.size(); ++index) {
            CHECK_EQ(expected[index], parsed.userArguments[index]);
        }
    }

    const RuntimeArguments failed = ParseRuntimeArguments(0, nullptr);
    CHECK(!failed.ok);
    CHECK(!failed.safeMode);
    CHECK(!failed.addonBootstrapRestarted);
    CHECK(failed.userArguments.empty());
    CHECK(!failed.error.empty());
}

void observed_config_bootstrap_decision_test()
{
    CHECK_EQ(BootstrapAction::Continue,
        DecideBootstrapFromObservedUpdate(true, true, true, false, true));
    CHECK_EQ(BootstrapAction::Continue,
        DecideBootstrapFromObservedUpdate(false, false, false, true, true));
    CHECK_EQ(BootstrapAction::Relaunch,
        DecideBootstrapFromObservedUpdate(true, false, true, false, true));
    CHECK_EQ(BootstrapAction::Fail,
        DecideBootstrapFromObservedUpdate(true, false, true, true, true));
    CHECK_EQ(BootstrapAction::Fail,
        DecideBootstrapFromObservedUpdate(true, false, false, false, true));
    CHECK_EQ(BootstrapAction::Fail,
        DecideBootstrapFromObservedUpdate(false, true, false, false, false));
}

void restart_argument_lifecycle_and_create_process_command_line_test()
{
    const std::vector<std::wstring> contaminated = {
        L"--future-flag",
        L"",
        L"--addon-bootstrap-restarted",
        L"C:\\Videos\\clip with spaces.mp4",
        L"--safe-mode",
        L"--safe-mode",
        L"--addon-bootstrap-restarted",
    };

    const std::vector<std::wstring> bootstrap = BuildBootstrapRelaunchArguments(contaminated);
    const std::vector<std::wstring> expectedBootstrap = {
        L"--future-flag",
        L"",
        L"C:\\Videos\\clip with spaces.mp4",
        L"--safe-mode",
        L"--addon-bootstrap-restarted",
    };
    CHECK_EQ(expectedBootstrap.size(), bootstrap.size());
    if (bootstrap.size() == expectedBootstrap.size()) {
        for (size_t index = 0; index < bootstrap.size(); ++index) {
            CHECK_EQ(expectedBootstrap[index], bootstrap[index]);
        }
    }

    const std::vector<std::wstring> safeMode = BuildSafeModeRestartArguments(contaminated);
    const std::vector<std::wstring> expectedSafeMode = {
        L"--future-flag",
        L"",
        L"C:\\Videos\\clip with spaces.mp4",
        L"--safe-mode",
    };
    CHECK_EQ(expectedSafeMode.size(), safeMode.size());
    if (safeMode.size() == expectedSafeMode.size()) {
        for (size_t index = 0; index < safeMode.size(); ++index) {
            CHECK_EQ(expectedSafeMode[index], safeMode[index]);
        }
    }

    constexpr std::wstring_view executable = L"C:\\Program Files\\DLSS Player\\DLSSVideoPlayer.exe";
    constexpr std::wstring_view expectedCommandLine =
        L"\"C:\\Program Files\\DLSS Player\\DLSSVideoPlayer.exe\" "
        L"\"--future-flag\" \"\" "
        L"\"C:\\Videos\\clip with spaces.mp4\" "
        L"\"--safe-mode\" \"--addon-bootstrap-restarted\"";
    CHECK_EQ(std::wstring(expectedCommandLine), BuildWindowsCommandLine(executable, bootstrap));
}

void advanced_safe_mode_normal_invocation_adds_safe_mode_test()
{
    const std::vector<std::wstring> normalArguments = {
        L"--future-flag",
        L"",
        L"C:\\Videos\\clip with spaces.mp4",
    };
    const std::vector<std::wstring> expected = {
        L"--future-flag",
        L"",
        L"C:\\Videos\\clip with spaces.mp4",
        L"--safe-mode",
    };
    int launchCalls = 0;
    std::vector<std::wstring> launchedArguments;

    const SafeModeRestartOutcome outcome = ExecuteAdvancedSafeModeRestart(
        true,
        normalArguments,
        [&](const std::vector<std::wstring>& arguments) {
            ++launchCalls;
            launchedArguments = arguments;
            return true;
        });

    CHECK_EQ(SafeModeRestartOutcome::CloseCurrent, outcome);
    CHECK_EQ(1, launchCalls);
    CHECK_EQ(expected.size(), launchedArguments.size());
    if (launchedArguments.size() == expected.size()) {
        for (size_t index = 0; index < expected.size(); ++index) {
            CHECK_EQ(expected[index], launchedArguments[index]);
        }
    }
}

void advanced_safe_mode_cancel_keeps_current_open_without_launch_test()
{
    int launchCalls = 0;
    const SafeModeRestartOutcome outcome = ExecuteAdvancedSafeModeRestart(
        false,
        {L"--future-flag"},
        [&](const std::vector<std::wstring>&) {
            ++launchCalls;
            return true;
        });

    CHECK_EQ(SafeModeRestartOutcome::Cancelled, outcome);
    CHECK_EQ(0, launchCalls);
}

void advanced_safe_mode_launch_failure_keeps_current_open_test()
{
    int launchCalls = 0;
    const SafeModeRestartOutcome outcome = ExecuteAdvancedSafeModeRestart(
        true,
        {L"--future-flag"},
        [&](const std::vector<std::wstring>& arguments) {
            ++launchCalls;
            CHECK_EQ(2u, arguments.size());
            if (arguments.size() == 2) {
                CHECK_EQ(std::wstring(L"--future-flag"), arguments[0]);
                CHECK_EQ(std::wstring(L"--safe-mode"), arguments[1]);
            }
            return false;
        });

    CHECK_EQ(SafeModeRestartOutcome::LaunchFailed, outcome);
    CHECK_EQ(1, launchCalls);
}

void advanced_safe_mode_launch_success_closes_with_sanitized_arguments_test()
{
    const std::vector<std::wstring> contaminated = {
        L"--addon-bootstrap-restarted",
        L"--safe-mode",
        L"--future-flag",
        L"--safe-mode",
    };
    const std::vector<std::wstring> expected = {
        L"--safe-mode",
        L"--future-flag",
    };
    int launchCalls = 0;
    std::vector<std::wstring> launchedArguments;

    const SafeModeRestartOutcome outcome = ExecuteAdvancedSafeModeRestart(
        true,
        contaminated,
        [&](const std::vector<std::wstring>& arguments) {
            ++launchCalls;
            launchedArguments = arguments;
            return true;
        });

    CHECK_EQ(SafeModeRestartOutcome::CloseCurrent, outcome);
    CHECK_EQ(1, launchCalls);
    CHECK_EQ(expected.size(), launchedArguments.size());
    if (launchedArguments.size() == expected.size()) {
        for (size_t index = 0; index < expected.size(); ++index) {
            CHECK_EQ(expected[index], launchedArguments[index]);
        }
    }
}

void disabled_addons_creates_missing_addon_section_test()
{
    constexpr std::string_view input =
        "[GENERAL]\r\n"
        "PresetPath=C:\\Games\\Player\r\n";
    constexpr std::string_view expected =
        "[GENERAL]\r\n"
        "PresetPath=C:\\Games\\Player\r\n"
        "[ADDON]\r\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\r\n";

    CHECK_EQ(std::string(expected), UpdateDisabledAddonsIni(input, kNeuralAddon, true));
}

void disabled_addons_updates_empty_and_populated_lists_test()
{
    constexpr std::string_view emptyInput =
        "[ADDON]\n"
        "DisabledAddons=\n"
        "[INPUT]\n"
        "KeyMenu=36\n";
    constexpr std::string_view emptyExpected =
        "[ADDON]\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n"
        "[INPUT]\n"
        "KeyMenu=36\n";
    constexpr std::string_view populatedInput =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,third-party.addon64\n";
    constexpr std::string_view populatedExpected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,third-party.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";

    CHECK_EQ(std::string(emptyExpected), UpdateDisabledAddonsIni(emptyInput, kNeuralAddon, true));
    CHECK_EQ(std::string(populatedExpected), UpdateDisabledAddonsIni(populatedInput, kNeuralAddon, true));
}

void disabled_addons_preserves_mixed_line_endings_and_unrelated_sections_test()
{
    constexpr std::string_view input =
        "[GENERAL]\r\n"
        "NoReloadOnInit=1\n"
        "[ADDON]\r"
        "DisabledAddons=legacy.addon64\r"
        "[OVERLAY]\n"
        "TutorialProgress=3\r\n";
    constexpr std::string_view expected =
        "[GENERAL]\r\n"
        "NoReloadOnInit=1\n"
        "[ADDON]\r"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64\r"
        "[OVERLAY]\n"
        "TutorialProgress=3\r\n";

    CHECK_EQ(std::string(expected), UpdateDisabledAddonsIni(input, kNeuralAddon, true));
}

void disabled_addons_removes_only_exact_target_entries_test()
{
    constexpr std::string_view input =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64,renodx-dlss5.addon64.bak,DLSS 5 Neural Rendering@renodx-dlss5.addon64,other.addon64\n";
    constexpr std::string_view expected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,renodx-dlss5.addon64.bak,other.addon64\n";

    CHECK_EQ(std::string(expected), UpdateDisabledAddonsIni(input, kNeuralAddon, false));
}

void disabled_addons_collapses_only_exact_target_duplicates_test()
{
    constexpr std::string_view input =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64,renodx-dlss5.addon64.bak\n";
    constexpr std::string_view expected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64,renodx-dlss5.addon64.bak\n";

    CHECK_EQ(std::string(expected), UpdateDisabledAddonsIni(input, kNeuralAddon, true));
}

void disabled_addons_matches_trimmed_tokens_without_changing_retained_whitespace_test()
{
    constexpr std::string_view input =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64, DLSS 5 Neural Rendering@renodx-dlss5.addon64,  other.addon64\n";
    constexpr std::string_view enabledExpected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,  other.addon64\n";

    CHECK_EQ(std::string(enabledExpected), UpdateDisabledAddonsIni(input, kNeuralAddon, false));
    CHECK_EQ(std::string(input), UpdateDisabledAddonsIni(input, kNeuralAddon, true));
    CHECK_EQ(std::string(input), UpdateDisabledAddonsIni(
        UpdateDisabledAddonsIni(input, kNeuralAddon, true), kNeuralAddon, true));
}

void reshade_68_disabled_addon_token_conformance_test()
{
    const std::string canonical = "[ADDON]\nDisabledAddons=" + std::string(kNeuralAddon) + "\n";
    const std::string registeredName = "[ADDON]\nDisabledAddons=" + std::string(kNeuralAddonName) + "\n";
    const std::string atFilename = "[ADDON]\nDisabledAddons=@" + std::string(kNeuralAddonFilename) + "\n";
    const std::string legacyBareFilename = "[ADDON]\nDisabledAddons=" + std::string(kNeuralAddonFilename) + "\n";
    const std::string wrongCaseCanonical =
        "[ADDON]\nDisabledAddons=dlss 5 neural rendering@RENODX-DLSS5.ADDON64\n";

    const ConfigUpdate canonicalState = EvaluateNeuralAddonConfigUpdate(canonical, canonical, false, false);
    CHECK(canonicalState.ok);
    CHECK(!canonicalState.addonEnabled);
    const ConfigUpdate nameState = EvaluateNeuralAddonConfigUpdate(registeredName, registeredName, false, false);
    CHECK(nameState.ok);
    CHECK(!nameState.addonEnabled);
    const ConfigUpdate filenameState = EvaluateNeuralAddonConfigUpdate(atFilename, atFilename, false, false);
    CHECK(filenameState.ok);
    CHECK(!filenameState.addonEnabled);
    const ConfigUpdate legacyState = EvaluateNeuralAddonConfigUpdate(
        legacyBareFilename, legacyBareFilename, false, true);
    CHECK(legacyState.ok);
    CHECK(legacyState.addonEnabled);
    const ConfigUpdate wrongCaseState = EvaluateNeuralAddonConfigUpdate(
        wrongCaseCanonical, wrongCaseCanonical, false, true);
    CHECK(wrongCaseState.ok);
    CHECK(wrongCaseState.addonEnabled);
}

void reshade_68_aliases_migrate_to_one_canonical_token_test()
{
    constexpr std::string_view input =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering,@renodx-dlss5.addon64,renodx-dlss5.addon64,dlss 5 neural rendering@RENODX-DLSS5.ADDON64,other.addon64\n";
    constexpr std::string_view disabledExpected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64,other.addon64\n";
    constexpr std::string_view enabledExpected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,other.addon64\n";

    const std::string disabled = UpdateDisabledAddonsIni(input, kNeuralAddon, true);
    CHECK_EQ(std::string(disabledExpected), disabled);
    CHECK_EQ(disabled, UpdateDisabledAddonsIni(disabled, kNeuralAddon, true));
    CHECK_EQ(std::string(enabledExpected), UpdateDisabledAddonsIni(input, kNeuralAddon, false));
}

void reshade_68_section_and_key_lookup_are_case_sensitive_test()
{
    constexpr std::string_view wrongCaseSection =
        "[addon]\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";
    constexpr std::string_view wrongCaseSectionExpected =
        "[addon]\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n"
        "[ADDON]\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";
    constexpr std::string_view wrongCaseKey =
        "[ADDON]\r\n"
        "disabledaddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\r\n"
        "KeyOverlay=36\r\n";
    constexpr std::string_view wrongCaseKeyExpected =
        "[ADDON]\r\n"
        "disabledaddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\r\n"
        "KeyOverlay=36\r\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\r\n";

    CHECK_EQ(std::string(wrongCaseSectionExpected),
        UpdateDisabledAddonsIni(wrongCaseSection, kNeuralAddon, true));
    CHECK_EQ(std::string(wrongCaseKeyExpected),
        UpdateDisabledAddonsIni(wrongCaseKey, kNeuralAddon, true));
}

void reshade_68_utf8_bom_is_ignored_for_lookup_and_preserved_test()
{
    const std::string input =
        std::string("\xEF\xBB\xBF") + "[ADDON]\r\nDisabledAddons=" + std::string(kNeuralAddon) + "\r\n";
    const std::string enabledExpected =
        std::string("\xEF\xBB\xBF") + "[ADDON]\r\nDisabledAddons=\r\n";

    CHECK_EQ(input, UpdateDisabledAddonsIni(input, kNeuralAddon, true));
    CHECK_EQ(enabledExpected, UpdateDisabledAddonsIni(input, kNeuralAddon, false));
    const ConfigUpdate state = EvaluateNeuralAddonConfigUpdate(input, input, false, false);
    CHECK(state.ok);
    CHECK(!state.addonEnabled);
}

void disabled_addons_insertion_uses_target_section_line_ending_test()
{
    constexpr std::string_view input =
        "[GENERAL]\r\n"
        "PresetPath=.\\ReShadePreset.ini\r\n"
        "[ADDON]\n"
        "KeyOverlay=36\n"
        "[INPUT]\r\n"
        "KeyMenu=36\r\n";
    constexpr std::string_view expected =
        "[GENERAL]\r\n"
        "PresetPath=.\\ReShadePreset.ini\r\n"
        "[ADDON]\n"
        "KeyOverlay=36\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n"
        "[INPUT]\r\n"
        "KeyMenu=36\r\n";

    CHECK_EQ(std::string(expected), UpdateDisabledAddonsIni(input, kNeuralAddon, true));
}

void configure_neural_addon_is_idempotent_test()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "PolicyTests-ReShade.ini";
    remove_file_if_present(path);
    constexpr std::string_view input =
        "[GENERAL]\n"
        "PresetPath=C:\\Games\\Player\n"
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";
    constexpr std::string_view expected =
        "[GENERAL]\n"
        "PresetPath=C:\\Games\\Player\n"
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64\n";
    write_binary_file(path, input);

    const ConfigUpdate first = ConfigureNeuralAddon(path, true);
    CHECK(first.ok);
    CHECK(first.changed);
    CHECK(first.addonEnabled);
    CHECK(first.error.empty());
    CHECK_EQ(std::string(expected), read_binary_file(path));

    const std::string afterFirst = read_binary_file(path);
    const ConfigUpdate second = ConfigureNeuralAddon(path, true);
    CHECK(second.ok);
    CHECK(!second.changed);
    CHECK(second.previousAddonEnabled);
    CHECK(second.addonEnabled);
    CHECK(second.error.empty());
    CHECK_EQ(afterFirst, read_binary_file(path));

    remove_file_if_present(path);
}

void configure_neural_addon_reports_semantic_state_across_text_canonicalization_test()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "PolicyTests-ReShade-semantic.ini";
    remove_file_if_present(path);

    constexpr std::string_view missingAddonInput =
        "[GENERAL]\n"
        "PresetPath=.\\ReShadePreset.ini\n";
    write_binary_file(path, missingAddonInput);
    const ConfigUpdate missingAddon = ConfigureNeuralAddon(path, true);
    CHECK(missingAddon.ok);
    CHECK(missingAddon.changed);
    CHECK(missingAddon.previousAddonEnabled);
    CHECK(missingAddon.addonEnabled);

    constexpr std::string_view duplicateTargetInput =
        "[ADDON]\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64,legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";
    write_binary_file(path, duplicateTargetInput);
    const ConfigUpdate duplicateTarget = ConfigureNeuralAddon(path, false);
    CHECK(duplicateTarget.ok);
    CHECK(duplicateTarget.changed);
    CHECK(!duplicateTarget.previousAddonEnabled);
    CHECK(!duplicateTarget.addonEnabled);

    remove_file_if_present(path);
}

void configure_neural_addon_safe_then_normal_observes_reshade_state_test()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "PolicyTests-ReShade-safe-normal.ini";
    remove_file_if_present(path);
    constexpr std::string_view legacyInput =
        "[ADDON]\r\n"
        "DisabledAddons=legacy.addon64,renodx-dlss5.addon64\r\n";
    constexpr std::string_view safeExpected =
        "[ADDON]\r\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64\r\n";
    constexpr std::string_view normalExpected =
        "[ADDON]\r\n"
        "DisabledAddons=legacy.addon64\r\n";
    write_binary_file(path, legacyInput);

    const ConfigUpdate safe = ConfigureNeuralAddon(path, false);
    CHECK(safe.ok);
    CHECK(safe.changed);
    CHECK(safe.previousAddonEnabled);
    CHECK(!safe.addonEnabled);
    CHECK_EQ(std::string(safeExpected), read_binary_file(path));

    const ConfigUpdate safeAgain = ConfigureNeuralAddon(path, false);
    CHECK(safeAgain.ok);
    CHECK(!safeAgain.changed);
    CHECK(!safeAgain.previousAddonEnabled);
    CHECK(!safeAgain.addonEnabled);
    CHECK_EQ(std::string(safeExpected), read_binary_file(path));

    const ConfigUpdate normal = ConfigureNeuralAddon(path, true);
    CHECK(normal.ok);
    CHECK(normal.changed);
    CHECK(!normal.previousAddonEnabled);
    CHECK(normal.addonEnabled);
    CHECK_EQ(std::string(normalExpected), read_binary_file(path));

    const ConfigUpdate normalAgain = ConfigureNeuralAddon(path, true);
    CHECK(normalAgain.ok);
    CHECK(!normalAgain.changed);
    CHECK(normalAgain.previousAddonEnabled);
    CHECK(normalAgain.addonEnabled);
    CHECK_EQ(std::string(normalExpected), read_binary_file(path));

    remove_file_if_present(path);
}

void evaluated_config_update_observes_actual_final_bytes_test()
{
    constexpr std::string_view enabledIni =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64\n";
    constexpr std::string_view disabledIni =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";

    const ConfigUpdate mismatch = EvaluateNeuralAddonConfigUpdate(enabledIni, disabledIni, true, true);
    CHECK(!mismatch.ok);
    CHECK(mismatch.changed);
    CHECK(mismatch.previousAddonEnabled);
    CHECK(!mismatch.addonEnabled);
    CHECK(!mismatch.error.empty());

    const ConfigUpdate observed = EvaluateNeuralAddonConfigUpdate(disabledIni, enabledIni, true, true);
    CHECK(observed.ok);
    CHECK(observed.changed);
    CHECK(!observed.previousAddonEnabled);
    CHECK(observed.addonEnabled);
    CHECK(observed.error.empty());
}

void configure_neural_addon_fails_closed_for_malformed_ini_test()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "PolicyTests-ReShade-malformed.ini";
    remove_file_if_present(path);
    constexpr char nulContent[] = "[ADDON]\nDisabledAddons=legacy.addon64\0tail";
    const std::string nulInput{nulContent, sizeof(nulContent) - 1};
    write_binary_file(path, nulInput);

    const ConfigUpdate nulResult = ConfigureNeuralAddon(path, false);
    CHECK(!nulResult.ok);
    CHECK(!nulResult.changed);
    CHECK(!nulResult.addonEnabled);
    CHECK(!nulResult.error.empty());
    CHECK_EQ(nulInput, read_binary_file(path));

    constexpr std::string_view duplicateInput =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";
    write_binary_file(path, duplicateInput);
    const ConfigUpdate duplicateResult = ConfigureNeuralAddon(path, true);
    CHECK(!duplicateResult.ok);
    CHECK(!duplicateResult.changed);
    CHECK(!duplicateResult.addonEnabled);
    CHECK(!duplicateResult.error.empty());
    CHECK_EQ(std::string(duplicateInput), read_binary_file(path));

    constexpr std::string_view crossSectionDuplicateInput =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64\n"
        "[ADDON]\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";
    write_binary_file(path, crossSectionDuplicateInput);
    const ConfigUpdate crossSectionResult = ConfigureNeuralAddon(path, true);
    CHECK(!crossSectionResult.ok);
    CHECK(!crossSectionResult.changed);
    CHECK(!crossSectionResult.addonEnabled);
    CHECK(!crossSectionResult.error.empty());
    CHECK_EQ(std::string(crossSectionDuplicateInput), read_binary_file(path));

    remove_file_if_present(path);
}

void configure_neural_addon_rejects_non_regular_path_before_replacement_test()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "PolicyTests-ReShade-directory";
    std::error_code removeError;
    std::filesystem::remove_all(path, removeError);
    CHECK(!removeError);
    std::filesystem::create_directory(path, removeError);
    CHECK(!removeError);
    CHECK(std::filesystem::is_directory(path, removeError));
    CHECK(!removeError);

    const ConfigUpdate result = ConfigureNeuralAddon(path, true);
    CHECK(!result.ok);
    CHECK(!result.changed);
    CHECK(!result.addonEnabled);
    CHECK(!result.error.empty());
    CHECK(std::filesystem::is_directory(path));

    std::filesystem::remove_all(path, removeError);
    CHECK(!removeError);
    CHECK(!std::filesystem::exists(path, removeError));
    CHECK(!removeError);
}

void youtube_url_validation_accepts_only_supported_video_routes_test()
{
    const std::array accepted{
        L"https://youtube.com/watch?v=dQw4w9WgXcQ",
        L"https://www.youtube.com/watch?v=dQw4w9WgXcQ",
        L"https://m.youtube.com/watch?v=dQw4w9WgXcQ",
        L"https://music.youtube.com/watch?v=dQw4w9WgXcQ",
        L"https://youtu.be/dQw4w9WgXcQ",
        L"https://youtu.be/dQw4w9WgXcQ?t=12",
        L"https://youtube.com/shorts/dQw4w9WgXcQ",
        L"https://youtube.com/watch?list=PL123&v=dQw4w9WgXcQ&index=2",
        L"https://youtube.com:443/watch?v=dQw4w9WgXcQ",
        L"https://youtube.com:8443/watch?v=dQw4w9WgXcQ",
    };

    for (const std::wstring_view value : accepted) {
        CHECK(IsSupportedYouTubeUrl(value));
    }
}

void youtube_url_validation_rejects_unsafe_or_unselected_inputs_test()
{
    std::vector<std::wstring> rejected{
        L"http://youtube.com/watch?v=dQw4w9WgXcQ",
        L"https://user@youtube.com/watch?v=dQw4w9WgXcQ",
        L"https://user:secret@youtube.com/watch?v=dQw4w9WgXcQ",
        L"https://youtube.com.evil.example/watch?v=dQw4w9WgXcQ",
        L"https://notyoutube.com/watch?v=dQw4w9WgXcQ",
        L"https://example.com/watch?v=dQw4w9WgXcQ",
        L"file:///C:/video.mp4",
        L"C:\\video.mp4",
        L"\\\\server\\share\\video.mp4",
        L"https://youtube.com/watch?v=bad\nvalue",
        L"https://youtube.com/watch?v=\"secret\"",
        L"https://youtube.com/watch?v='secret'",
        L"",
        L"   ",
        L" https://youtube.com/watch?v=dQw4w9WgXcQ",
        L"https://youtube.com/watch?v=dQw4w9WgXcQ ",
        L"https://youtube.com/playlist?list=PL123",
        L"https://youtube.com/watch?list=PL123",
        L"https://youtube.com/watch?v=",
        L"https://youtube.com/shorts/",
        L"https://youtu.be/",
        L"https://youtube.com/embed/dQw4w9WgXcQ",
        L"https://youtube.com./watch?v=dQw4w9WgXcQ",
        L"https://youtube.com.evil.example./watch?v=dQw4w9WgXcQ",
        L"https://y\u043eutube.com/watch?v=dQw4w9WgXcQ",
        L"https://youtube.com\u3002evil.example/watch?v=dQw4w9WgXcQ",
        L"https://xn--youtube-9d0b.com/watch?v=dQw4w9WgXcQ",
    };
    rejected.push_back(L"https://youtube.com/watch?v=" + std::wstring(2049, L'a'));

    std::wstring embeddedNul = L"https://youtube.com/watch?v=abc";
    embeddedNul.push_back(L'\0');
    embeddedNul.append(L"def");
    rejected.push_back(std::move(embeddedNul));

    for (const std::wstring& value : rejected) {
        CHECK(!IsSupportedYouTubeUrl(value));
    }
}

void youtube_watch_query_requires_one_unambiguous_lowercase_v_field_test()
{
    const std::array rejected{
        L"https://youtube.com/watch?v=abc&v=abc",
        L"https://youtube.com/watch?v=abc&v=def",
        L"https://youtube.com/watch?v=&v=abc",
        L"https://youtube.com/watch?v=abc&v=",
        L"https://youtube.com/watch?V=abc",
        L"https://youtube.com/watch?V=abc&v=def",
        L"https://youtube.com/watch?%76=abc",
        L"https://youtube.com/watch?%56=abc&v=def",
        L"https://youtube.com/watch?v%3Dabc",
        L"https://youtube.com/watch?v=abc%26v%3Ddef",
        L"https://youtube.com/watch?v=abc%26list%3DPL123",
    };

    for (const std::wstring_view value : rejected) {
        CHECK(!IsSupportedYouTubeUrl(value));
    }
}

void youtube_url_validation_enforces_exact_2048_character_boundary_test()
{
    constexpr std::wstring_view prefix = L"https://youtu.be/";
    const std::wstring accepted = std::wstring(prefix) +
                                  std::wstring(2048 - prefix.size(), L'a');
    const std::wstring rejected = accepted + L'a';

    CHECK_EQ(size_t{2048}, accepted.size());
    CHECK(IsSupportedYouTubeUrl(accepted));
    CHECK_EQ(size_t{2049}, rejected.size());
    CHECK(!IsSupportedYouTubeUrl(rejected));
}

void resolver_output_accepts_one_https_googlevideo_url_and_trims_crlf_test()
{
    struct Case {
        std::string_view output;
        std::wstring_view expected;
    };
    const std::array accepted{
        Case{"https://googlevideo.com/videoplayback?id=plain",
             L"https://googlevideo.com/videoplayback?id=plain"},
        Case{"https://rr1---sn-a5mekn6r.googlevideo.com/videoplayback?id=abc",
             L"https://rr1---sn-a5mekn6r.googlevideo.com/videoplayback?id=abc"},
        Case{"https://rr1---sn-a5mekn6r.googlevideo.com/videoplayback?id=abc\n",
             L"https://rr1---sn-a5mekn6r.googlevideo.com/videoplayback?id=abc"},
        Case{"https://rr1---sn-a5mekn6r.googlevideo.com/videoplayback?id=abc\r\n",
             L"https://rr1---sn-a5mekn6r.googlevideo.com/videoplayback?id=abc"},
    };

    for (const Case& test : accepted) {
        const ResolveResult result = ParseResolverOutput(test.output, 0);
        CHECK(result.ok);
        CHECK_EQ(ResolveError::None, result.error);
        CHECK_EQ(std::wstring(test.expected), result.mediaUrl);
        CHECK(result.detail.empty());
        CHECK(result.mediaUrl.find(L'\r') == std::wstring::npos);
        CHECK(result.mediaUrl.find(L'\n') == std::wstring::npos);
    }
}

void resolver_output_rejects_empty_multiple_oversize_or_untrusted_urls_test()
{
    const std::vector<std::string> rejected{
        "",
        "\r\n",
        "https://a.googlevideo.com/one\nhttps://b.googlevideo.com/two",
        "http://a.googlevideo.com/videoplayback?id=abc",
        "https://googlevideo.com.evil.example/videoplayback?id=abc",
        "https://evilgooglevideo.com/videoplayback?id=abc",
        "https://example.com/videoplayback?id=abc",
        "https://user@googlevideo.com/videoplayback?id=abc",
        "not-a-url",
        "a.googlevideo.com/videoplayback?id=abc",
        "https: //a.googlevideo.com/videoplayback?id=abc",
        "\nhttps://a.googlevideo.com/videoplayback?id=abc",
        "https://a.googlevideo.com/videoplayback?id=abc\n\n",
        "https://a.googlevideo.com/videoplayback?id=abc\r\n\r\n",
        "https://a.googlevideo.com/videoplayback?id=abc\r",
        std::string{"https://a.googlevideo.com/\xc3\x28", 28},
        std::string("https://a.googlevideo.com/") + std::string(16 * 1024, 'a'),
    };

    for (const std::string& output : rejected) {
        const ResolveResult result = ParseResolverOutput(output, 0);
        CHECK(!result.ok);
        CHECK_EQ(ResolveError::InvalidOutput, result.error);
        CHECK(result.mediaUrl.empty());
        CHECK(result.detail.size() <= 4096);
        CHECK(result.detail.find(L"http") == std::wstring::npos);
        CHECK(result.detail.find(L"googlevideo") == std::wstring::npos);
        CHECK(result.detail.find(L"not-a-url") == std::wstring::npos);
    }
}

void resolver_output_enforces_raw_16k_and_single_trailing_line_ending_test()
{
    constexpr std::string_view prefix = "https://a.googlevideo.com/";
    const std::string exactRaw = std::string(prefix) +
                                 std::string(16 * 1024 - prefix.size(), 'a');
    const std::string overRaw = exactRaw + 'a';
    const std::string exactWithCrLf = std::string(prefix) +
        std::string(16 * 1024 - prefix.size() - 2, 'a') + "\r\n";
    const std::string overWithCrLf = std::string(prefix) +
        std::string(16 * 1024 - prefix.size() - 1, 'a') + "\r\n";

    CHECK_EQ(size_t{16 * 1024}, exactRaw.size());
    CHECK(ParseResolverOutput(exactRaw, 0).ok);
    CHECK_EQ(size_t{16 * 1024 + 1}, overRaw.size());
    CHECK_EQ(ResolveError::InvalidOutput, ParseResolverOutput(overRaw, 0).error);
    CHECK_EQ(size_t{16 * 1024}, exactWithCrLf.size());
    CHECK(ParseResolverOutput(exactWithCrLf, 0).ok);
    CHECK_EQ(size_t{16 * 1024 + 1}, overWithCrLf.size());
    CHECK_EQ(ResolveError::InvalidOutput, ParseResolverOutput(overWithCrLf, 0).error);
}

void resolver_nonzero_exit_returns_fixed_generic_non_url_detail_test()
{
    constexpr std::wstring_view expectedDetail =
        L"Could not extract a playable YouTube stream.";
    const std::vector<std::string> diagnostics{
        "ERROR: video unavailable\r\n",
        "ERROR: rejected https://example.com/watch?v=secret-token\r\n",
        "ERROR: rejected https: //example.com/watch?v=split-secret\r\n",
        "Bearer abc123",
        "Authorization=Basic-secret password=hunter2 token=abc secret=qwerty cookie=session",
        "(https://example.com/watch?v=adjacent-secret); [Bearer abc123]",
        std::string(8 * 1024, 'x'),
    };

    for (const std::string& diagnostic : diagnostics) {
        const ResolveResult result = ParseResolverOutput(diagnostic, 7);
        CHECK(!result.ok);
        CHECK_EQ(ResolveError::ExtractionFailed, result.error);
        CHECK(result.mediaUrl.empty());
        CHECK_EQ(std::wstring(expectedDetail), result.detail);
        CHECK(result.detail.size() <= 4096);
    }
}

void youtube_resolver_windows_argument_quoting_covers_empty_spaces_quotes_and_slashes_test()
{
    struct Case {
        std::wstring_view input;
        std::wstring_view expected;
    };
    const std::array cases{
        Case{L"", L"\"\""},
        Case{L"plain", L"plain"},
        Case{L"two words", L"\"two words\""},
        Case{L"a\\\\\"b", L"\"a\\\\\\\\\\\"b\""},
        Case{L"ends with slash \\", L"\"ends with slash \\\\\""},
        Case{L"https://youtube.com/watch?v=abc&list=PL123", L"https://youtube.com/watch?v=abc&list=PL123"},
    };

    for (const Case& test : cases) {
        CHECK_EQ(std::wstring(test.expected), QuoteWindowsArgument(test.input));
    }
}

void youtube_resolver_argument_vector_is_exact_and_ordered_test()
{
    const std::filesystem::path helperDirectory = LR"(C:\Program Files\DLSS Player)";
    constexpr std::wstring_view url =
        L"https://youtube.com/watch?v=abc_DEF-123&list=PL123";
    const std::vector<std::wstring> expected{
        L"--no-config",
        L"--no-playlist",
        L"--no-warnings",
        L"--js-runtimes",
        LR"(deno:C:\Program Files\DLSS Player\deno.exe)",
        L"-f",
        L"b[ext=mp4]/b",
        L"--get-url",
        std::wstring(url),
    };

    CHECK_EQ(expected, BuildYouTubeResolverArguments(helperDirectory, url));
}

std::filesystem::path current_test_executable()
{
    std::wstring value(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    CHECK(length > 0);
    CHECK(length < value.size());
    value.resize(length);
    return std::filesystem::path(std::move(value));
}

struct ResolverFixture {
    std::filesystem::path directory;

    explicit ResolverFixture(bool validHelper = true)
    {
        const auto unique = std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(GetTickCount64());
        directory = std::filesystem::temp_directory_path() /
                    (L"PolicyTests-YouTubeResolver-" + unique);
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        CHECK(!error);
        if (validHelper) {
            CHECK(CopyFileW(current_test_executable().c_str(),
                            (directory / L"yt-dlp.exe").c_str(), FALSE) != FALSE);
        } else {
            write_binary_file(directory / L"yt-dlp.exe", "not a Windows executable");
        }
        write_binary_file(directory / L"deno.exe", "test-only placeholder");
    }

    ~ResolverFixture()
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        CHECK(!error);
    }
};

bool wait_for_file(const std::filesystem::path& path, std::chrono::milliseconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    std::error_code error;
    do {
        if (std::filesystem::exists(path, error)) return !error;
        if (error) return false;
        Sleep(10);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void youtube_resolver_success_uses_beside_app_helpers_and_exact_child_arguments_test()
{
    ResolverFixture fixture;
    auto resolver = YouTubeResolverTestAccess::Create(fixture.directory);
    const ResolveResult result = resolver->Resolve(
        L"https://youtube.com/watch?v=success&list=PL123", {});

    CHECK(result.ok);
    CHECK_EQ(ResolveError::None, result.error);
    CHECK_EQ(std::wstring(L"https://r1.googlevideo.com/videoplayback?id=success"),
             result.mediaUrl);
    CHECK(result.detail.empty());
}

void youtube_resolver_reports_missing_and_unstartable_helpers_without_sensitive_data_test()
{
    const std::filesystem::path missingDirectory =
        std::filesystem::temp_directory_path() / L"PolicyTests-resolver-missing";
    std::error_code error;
    std::filesystem::remove_all(missingDirectory, error);
    auto missingResolver = YouTubeResolverTestAccess::Create(missingDirectory);
    const ResolveResult missing = missingResolver->Resolve(
        L"https://youtube.com/watch?v=secret_missing", {});
    CHECK(!missing.ok);
    CHECK_EQ(ResolveError::HelperMissing, missing.error);
    CHECK(missing.mediaUrl.empty());
    CHECK(missing.detail.find(L"secret_missing") == std::wstring::npos);

    ResolverFixture corruptFixture(false);
    auto corruptResolver = YouTubeResolverTestAccess::Create(corruptFixture.directory);
    const ResolveResult corrupt = corruptResolver->Resolve(
        L"https://youtube.com/watch?v=secret_start", {});
    CHECK(!corrupt.ok);
    CHECK_EQ(ResolveError::StartFailed, corrupt.error);
    CHECK(corrupt.mediaUrl.empty());
    CHECK(corrupt.detail.find(L"secret_start") == std::wstring::npos);
}

void youtube_resolver_maps_nonzero_exit_and_output_overflow_precisely_test()
{
    ResolverFixture fixture;
    auto resolver = YouTubeResolverTestAccess::Create(fixture.directory);

    const ResolveResult nonzero = resolver->Resolve(
        L"https://youtu.be/nonzero", {});
    CHECK(!nonzero.ok);
    CHECK_EQ(ResolveError::ExtractionFailed, nonzero.error);
    CHECK(nonzero.mediaUrl.empty());

    const ResolveResult exactCaptureLimit = resolver->Resolve(
        L"https://youtu.be/cap64", {});
    CHECK(!exactCaptureLimit.ok);
    CHECK_EQ(ResolveError::InvalidOutput, exactCaptureLimit.error);

    const ResolveResult overflow = resolver->Resolve(
        L"https://youtu.be/cap64plus", {});
    CHECK(!overflow.ok);
    CHECK_EQ(ResolveError::OutputTooLarge, overflow.error);
    CHECK(overflow.mediaUrl.empty());
    CHECK(overflow.detail.find(L"https") == std::wstring::npos);
}

void youtube_resolver_honors_stop_token_and_explicit_cancel_with_bounded_wait_test()
{
    ResolverFixture fixture;
    auto resolver = YouTubeResolverTestAccess::Create(
        fixture.directory, std::chrono::seconds{5});

    ResolveResult stopped;
    const auto stopStarted = std::chrono::steady_clock::now();
    std::jthread stopWorker([&](std::stop_token token) {
        stopped = resolver->Resolve(L"https://youtu.be/hang", token);
    });
    Sleep(75);
    stopWorker.request_stop();
    stopWorker.join();
    const auto stopElapsed = std::chrono::steady_clock::now() - stopStarted;
    CHECK_EQ(ResolveError::Cancelled, stopped.error);
    CHECK(stopElapsed < std::chrono::seconds{2});

    ResolveResult cancelled;
    const auto cancelStarted = std::chrono::steady_clock::now();
    std::thread cancelWorker([&] {
        cancelled = resolver->Resolve(L"https://youtu.be/hang", {});
    });
    Sleep(75);
    resolver->Cancel();
    cancelWorker.join();
    const auto cancelElapsed = std::chrono::steady_clock::now() - cancelStarted;
    CHECK_EQ(ResolveError::Cancelled, cancelled.error);
    CHECK(cancelElapsed < std::chrono::seconds{2});

    resolver->Cancel();
    const ResolveResult afterCancel = resolver->Resolve(
        L"https://youtu.be/success", {});
    CHECK(afterCancel.ok);
}

void youtube_resolver_times_out_and_kills_its_descendant_job_tree_test()
{
    ResolverFixture fixture;
    const std::wstring suffix = std::to_wstring(GetCurrentProcessId());
    const std::filesystem::path marker = std::filesystem::temp_directory_path() /
        (L"PolicyTests-resolver-descendant-" + suffix + L".pid");
    remove_file_if_present(marker);

    auto resolver = YouTubeResolverTestAccess::Create(
        fixture.directory, std::chrono::milliseconds{350});
    const auto started = std::chrono::steady_clock::now();
    const ResolveResult result = resolver->Resolve(
        L"https://youtu.be/descendant_" + suffix, {});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(!result.ok);
    CHECK_EQ(ResolveError::TimedOut, result.error);
    CHECK(elapsed < std::chrono::seconds{2});
    const bool markerExists = wait_for_file(marker, std::chrono::milliseconds{250});
    CHECK(markerExists);
    if (!markerExists) return;

    const std::string pidText = read_binary_file(marker);
    CHECK(!pidText.empty());
    if (pidText.empty()) return;
    const DWORD descendantPid = static_cast<DWORD>(std::stoul(pidText));
    HANDLE descendant = OpenProcess(SYNCHRONIZE, FALSE, descendantPid);
    if (descendant) {
        CHECK_EQ(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(descendant, 1000));
        CloseHandle(descendant);
    }
    remove_file_if_present(marker);
}

void youtube_resolver_repeated_runs_leave_process_handle_count_stable_test()
{
    ResolverFixture fixture;
    auto resolver = YouTubeResolverTestAccess::Create(fixture.directory);
    DWORD before = 0;
    DWORD after = 0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE);
    for (int run = 0; run < 20; ++run) {
        const ResolveResult result = resolver->Resolve(
            L"https://youtu.be/success", {});
        CHECK(result.ok);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE);
    CHECK(after <= before + 2);
}

int run_fake_resolver_child(int argc, wchar_t* argv[])
{
    if (argc == 3 && std::wstring_view(argv[1]) == L"--resolver-descendant") {
        write_binary_file(argv[2], std::to_string(GetCurrentProcessId()));
        Sleep(INFINITE);
        return 0;
    }
    if (argc != 10 || std::wstring_view(argv[1]) != L"--no-config" ||
        std::wstring_view(argv[2]) != L"--no-playlist" ||
        std::wstring_view(argv[3]) != L"--no-warnings" ||
        std::wstring_view(argv[4]) != L"--js-runtimes" ||
        !std::wstring_view(argv[5]).starts_with(L"deno:") ||
        std::wstring_view(argv[6]) != L"-f" ||
        std::wstring_view(argv[7]) != L"b[ext=mp4]/b" ||
        std::wstring_view(argv[8]) != L"--get-url") {
        return 91;
    }
    const std::filesystem::path expectedDeno =
        current_test_executable().parent_path() / L"deno.exe";
    if (std::wstring_view(argv[5]).substr(5) != expectedDeno.wstring()) return 92;

    const std::wstring_view url = argv[9];
    if (url.find(L"success") != std::wstring_view::npos) {
        std::cout << "https://r1.googlevideo.com/videoplayback?id=success\n" << std::flush;
        return 0;
    }
    if (url.find(L"nonzero") != std::wstring_view::npos) return 7;
    if (url.find(L"cap64plus") != std::wstring_view::npos) {
        std::cout << std::string(64 * 1024 + 1, 'x') << std::flush;
        Sleep(INFINITE);
        return 0;
    }
    if (url.find(L"cap64") != std::wstring_view::npos) {
        std::cout << std::string(64 * 1024, 'x') << std::flush;
        return 0;
    }
    const size_t descendant = url.find(L"descendant_");
    if (descendant != std::wstring_view::npos) {
        const std::wstring suffix(url.substr(descendant + 11));
        const std::filesystem::path marker = std::filesystem::temp_directory_path() /
            (L"PolicyTests-resolver-descendant-" + suffix + L".pid");
        const std::filesystem::path executable = current_test_executable();
        std::wstring command = QuoteWindowsArgument(executable.wstring()) +
            L" --resolver-descendant " + QuoteWindowsArgument(marker.wstring());
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(),
                            &startup, &process)) {
            return 93;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        Sleep(INFINITE);
        return 0;
    }
    Sleep(INFINITE);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    if (argc > 1) return run_fake_resolver_child(argc, argv);
    harness_sanity_test();
    runtime_shutdown_releases_player_before_media_foundation_and_com_test();
    runtime_shutdown_rethrows_only_after_single_ordered_cleanup_test();
    toolbar_layout_selects_stable_action_sets_for_width_modes_test();
    toolbar_layout_preserves_group_separation_test();
    toolbar_layout_scales_hit_height_and_avoids_overlap_test();
    toolbar_hit_testing_is_half_open_and_boundary_stable_test();
    minimum_toolbar_client_width_owns_required_target_floor_across_dpi_test();
    volume_slider_never_intersects_compact_or_threshold_toolbar_test();
    toolbar_focus_order_includes_idle_open_and_skips_disabled_actions_test();
    open_action_content_keeps_idle_and_toolbar_copy_distinct_test();
    focused_toolbar_action_reconciles_layout_and_availability_changes_test();
    idle_surface_exposes_file_and_disabled_youtube_without_focusing_it_test();
    dpi_change_suggested_rect_respects_new_monitor_minimum_track_size_test();
    player_status_formats_exact_runtime_and_playback_states_test();
    long_media_title_is_bounded_with_a_real_ellipsis_test();
    recovery_copy_and_rehook_confirmation_are_actionable_test();
    unchanged_hover_action_has_no_dirty_rectangles_test();
    changed_hover_action_dirties_only_present_old_and_new_actions_test();
    hover_resolution_tracks_layout_action_changes_and_disappearance_test();
    current_cursor_hover_clears_when_cursor_query_is_unavailable_test();
    paint_buffer_layout_uses_only_the_clipped_nonzero_paint_rectangle_test();
    tabler_glyph_mapping_uses_the_pinned_css_codepoints_test();
    native_button_palette_has_distinct_interaction_states_test();
    active_button_small_text_meets_wcag_contrast_test();
    failed_icon_font_uses_label_only_presentation_test();
    debug_view_popup_contains_all_existing_views_and_selection_test();
    player_menu_is_english_only_and_retains_advanced_commands_test();
    youtube_availability_drives_real_menu_and_idle_action_consistently_test();
    legacy_language_configuration_is_ignored_and_english_lookup_remains_builtin_test();
    gpu_classification_table_test();
    neural_addon_policy_test();
    bootstrap_action_matrix_test();
    windows_command_line_quoting_round_trip_test();
    runtime_argument_parsing_preserves_user_arguments_and_strips_markers_test();
    observed_config_bootstrap_decision_test();
    restart_argument_lifecycle_and_create_process_command_line_test();
    advanced_safe_mode_normal_invocation_adds_safe_mode_test();
    advanced_safe_mode_cancel_keeps_current_open_without_launch_test();
    advanced_safe_mode_launch_failure_keeps_current_open_test();
    advanced_safe_mode_launch_success_closes_with_sanitized_arguments_test();
    disabled_addons_creates_missing_addon_section_test();
    disabled_addons_updates_empty_and_populated_lists_test();
    disabled_addons_preserves_mixed_line_endings_and_unrelated_sections_test();
    disabled_addons_removes_only_exact_target_entries_test();
    disabled_addons_collapses_only_exact_target_duplicates_test();
    disabled_addons_matches_trimmed_tokens_without_changing_retained_whitespace_test();
    reshade_68_disabled_addon_token_conformance_test();
    reshade_68_aliases_migrate_to_one_canonical_token_test();
    reshade_68_section_and_key_lookup_are_case_sensitive_test();
    reshade_68_utf8_bom_is_ignored_for_lookup_and_preserved_test();
    disabled_addons_insertion_uses_target_section_line_ending_test();
    configure_neural_addon_is_idempotent_test();
    configure_neural_addon_reports_semantic_state_across_text_canonicalization_test();
    configure_neural_addon_safe_then_normal_observes_reshade_state_test();
    evaluated_config_update_observes_actual_final_bytes_test();
    configure_neural_addon_fails_closed_for_malformed_ini_test();
    configure_neural_addon_rejects_non_regular_path_before_replacement_test();
    youtube_url_validation_accepts_only_supported_video_routes_test();
    youtube_url_validation_rejects_unsafe_or_unselected_inputs_test();
    youtube_watch_query_requires_one_unambiguous_lowercase_v_field_test();
    youtube_url_validation_enforces_exact_2048_character_boundary_test();
    resolver_output_accepts_one_https_googlevideo_url_and_trims_crlf_test();
    resolver_output_rejects_empty_multiple_oversize_or_untrusted_urls_test();
    resolver_output_enforces_raw_16k_and_single_trailing_line_ending_test();
    resolver_nonzero_exit_returns_fixed_generic_non_url_detail_test();
    youtube_resolver_windows_argument_quoting_covers_empty_spaces_quotes_and_slashes_test();
    youtube_resolver_argument_vector_is_exact_and_ordered_test();
    youtube_resolver_success_uses_beside_app_helpers_and_exact_child_arguments_test();
    youtube_resolver_reports_missing_and_unstartable_helpers_without_sensitive_data_test();
    youtube_resolver_maps_nonzero_exit_and_output_overflow_precisely_test();
    youtube_resolver_honors_stop_token_and_explicit_cancel_with_bounded_wait_test();
    youtube_resolver_times_out_and_kills_its_descendant_job_tree_test();
    youtube_resolver_repeated_runs_leave_process_handle_count_stable_test();

    if (test_support::failure_count != 0) {
        std::cerr << test_support::failure_count << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "PolicyTests: all assertions passed\n";
    return EXIT_SUCCESS;
}
