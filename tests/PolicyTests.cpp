#include "TestSupport.h"

#include "RuntimePolicy.h"
#include "ReShadeConfig.h"
#include "Localization.h"
#include "AppMenu.h"
#include "UiLayout.h"
#include "UiResources.h"
#include "RuntimeLifetime.h"
#include "YouTubeResolver.h"
#include "ExampleVideos.h"
#include "CompletionRegistry.h"
#include "VideoDecoder.h"
#include "AudioPlayer.h"
#include "NetworkMediaTransaction.h"
#include "D3D12FenceWait.h"
#include "NgxSession.h"
#include "D3D12Renderer.h"
#include "ReleasePackagePolicy.h"
#include "PlaybackTiming.h"
#ifdef small
#undef small
#endif

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

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
        std::chrono::milliseconds deadline = std::chrono::milliseconds{500},
        YouTubeResolver::FailureStage failureStage = YouTubeResolver::FailureStage::None)
    {
        YouTubeResolver::Settings settings;
        settings.helperDirectory = helperDirectory;
        settings.deadline = deadline;
        settings.pollInterval = std::chrono::milliseconds{10};
        settings.shutdownWait = std::chrono::milliseconds{500};
        settings.failureStage = failureStage;
        return std::unique_ptr<YouTubeResolver>(new YouTubeResolver(std::move(settings)));
    }
};

struct VideoDecoderTestAccess {
    static std::unique_ptr<VideoDecoder> Create(
        const std::filesystem::path& helperDirectory,
        std::chrono::milliseconds probeTimeout = std::chrono::milliseconds{250},
        std::chrono::milliseconds stallTimeout = std::chrono::milliseconds{120},
        VideoDecoder::FailureStage failureStage = VideoDecoder::FailureStage::None)
    {
        VideoDecoder::Settings settings;
        settings.helperDirectory=helperDirectory.wstring();settings.probeTimeout=probeTimeout;settings.stallTimeout=stallTimeout;settings.failureStage=failureStage;
        return std::unique_ptr<VideoDecoder>(new VideoDecoder(std::move(settings)));
    }
};

struct AudioPlayerTestAccess {
    static std::unique_ptr<AudioPlayer> Create(
        const std::filesystem::path& helperDirectory,
        bool failTerminateJob = false,
        bool failInitialProcessWait = false,
        bool failGetExitCodeProcess = false,
        bool failFinalProcessWait = false,
        bool failInitialReaderWait = false,
        bool failFinalReaderWait = false)
    {
        AudioPlayer::Settings settings;
        settings.helperDirectory=helperDirectory.wstring();settings.disableWaveOut=true;
        settings.failTerminateJob=failTerminateJob;
        settings.failInitialProcessWait=failInitialProcessWait;
        settings.failGetExitCodeProcess=failGetExitCodeProcess;
        settings.failFinalProcessWait=failFinalProcessWait;
        settings.failInitialReaderWait=failInitialReaderWait;
        settings.failFinalReaderWait=failFinalReaderWait;
        return std::unique_ptr<AudioPlayer>(new AudioPlayer(std::move(settings)));
    }
    static double SeekBase(const AudioPlayer& player){return player.m_seekBaseSec;}
    static uint64_t SubmittedBuffers(const AudioPlayer& player){return player.m_reader?player.m_reader->submittedBuffers.load():0;}
};

struct RendererOwnedSentinel final : D3D12RendererTestOwnedResource {
    explicit RendererOwnedSentinel(std::shared_ptr<int> destroyed):destroyed(std::move(destroyed)){}
    ~RendererOwnedSentinel() override {++*destroyed;}
    std::shared_ptr<int> destroyed;
};

struct D3D12RendererTestAccess {
    static void ConfigureWait(D3D12Renderer& renderer,
                              d3d12_renderer_detail::FenceWaitResult result,
                              int& waits)
    {
        renderer.m_testWaitGPU=[&waits,result]{++waits;return result;};
    }
    static void OwnSentinel(D3D12Renderer& renderer,
                            std::unique_ptr<D3D12RendererTestOwnedResource> sentinel)
    {
        renderer.m_testOwnedResource=std::move(sentinel);
    }
    static bool WaitForContinuedUse(D3D12Renderer& renderer)
    {
        return renderer.WaitGPUForContinuedUse();
    }
    static void ConfigureFrameSignal(D3D12Renderer& renderer,HRESULT signalResult,
                                     HRESULT deviceRemovedReason,int& signalCalls,
                                     int& reasonChecks)
    {
        renderer.m_testFrameSignal=[&signalCalls,signalResult](uint64_t){
            ++signalCalls;return signalResult;
        };
        renderer.m_testDeviceRemovedReason=[&reasonChecks,deviceRemovedReason]{
            ++reasonChecks;return deviceRemovedReason;
        };
    }
    static void SetFrameTracking(D3D12Renderer& renderer,uint32_t frameSlot,
                                 uint64_t fenceValue,uint32_t trackedSlot,
                                 uint64_t trackedFence)
    {
        renderer.m_frameSlot=frameSlot;renderer.m_fenceValue=fenceValue;
        renderer.m_frameFence[trackedSlot]=trackedFence;
    }
    static bool SignalFrameSlot(D3D12Renderer& renderer,uint32_t slot)
    {
        return renderer.SignalFrameSlot(slot);
    }
    static uint32_t FrameSlot(const D3D12Renderer& renderer){return renderer.m_frameSlot;}
    static uint64_t FenceValue(const D3D12Renderer& renderer){return renderer.m_fenceValue;}
    static uint64_t FrameFence(const D3D12Renderer& renderer,uint32_t slot){return renderer.m_frameFence[slot];}
    static bool GPUUnusable(const D3D12Renderer& renderer){return renderer.m_gpuUnusable;}
    static d3d12_renderer_detail::FenceWaitResult LastFenceResult(const D3D12Renderer& renderer){return renderer.m_lastFenceWaitResult;}
    static void ConfigureCacheCapture(D3D12Renderer& renderer,uint32_t width,uint32_t height,
                                      bool neuralUsed,
                                      std::function<bool(std::vector<uint8_t>&)> capture)
    {
        renderer.m_outputW=width;renderer.m_outputH=height;
        renderer.m_lastDLSSUsed=neuralUsed;renderer.m_testCacheCapture=std::move(capture);
    }
    static bool CaptureEvaluatedFrame(D3D12Renderer& renderer,CapturedVideoFrame& frame)
    {
        return renderer.CaptureEvaluatedFrame(frame);
    }
};

namespace {

constexpr std::string_view kNeuralAddon = "DLSS 5 Neural Rendering@renodx-dlss5.addon64";
constexpr std::string_view kNeuralAddonName = "DLSS 5 Neural Rendering";
constexpr std::string_view kNeuralAddonFilename = "renodx-dlss5.addon64";
bool resolverSymlinkCoverageExercised = false;

void release_package_filename_policy_is_allowlisted_and_fail_closed_test()
{
    using release_package_policy::IsAllowedPath;

    const std::array<std::wstring_view, 19> allowed = {
        L"DLSSVideoPlayer.exe", L"ffmpeg.exe", L"ffprobe.exe", L"yt-dlp.exe",
        L"deno.exe", L"nvngx_dlss.dll", L"nvngx_dlssnr.dll", L"dxgi.dll",
        L"sl.common.dll", L"ReShade.ini", L"ReShadePreset.ini",
        L"docs/DLSS5_SETUP.md", L"THIRD_PARTY_LICENSES/yt-dlp-2026.08.19.txt",
        L"PACKAGE_MANIFEST.txt", L"SECURITY.md", L"CONTRIBUTING.md",
        L"CHANGELOG.md", L"docs/RELATED_PROJECTS.md",
        L"THIRD_PARTY_LICENSES/dlss5-feeder-MIT.txt"
    };
    for (const auto path : allowed) CHECK(IsAllowedPath(path));

    const std::array<std::wstring_view, 15> forbidden = {
        L"pt-BR.lang", L"languages/pt-BR.lang", L"downloads/video.mp4",
        L"ReShade.log", L"DLSSVideoPlayer.log", L"DLSSVideoPlayer.ini",
        L"developer-settings.ini", L"test.mp4", L"sample.mkv",
        L"DLSSVideoPlayer.pdb", L"thing.obj", L"source.zip", L"source.7z",
        L"nvngx_dlssnr.rollback.dll", L"unexpected-helper.exe"
    };
    for (const auto path : forbidden) CHECK(!IsAllowedPath(path));
}

void public_release_package_policy_excludes_private_and_optional_binaries_test()
{
    using release_package_policy::IsAllowedPublicPath;

    const std::array<std::wstring_view, 17> allowed = {
        L"DLSSVideoPlayer.exe", L"nvngx_dlss.dll", L"README.md", L"LICENSE",
        L"SECURITY.md", L"CONTRIBUTING.md", L"CHANGELOG.md", L"THIRD_PARTY.md",
        L"PUBLIC_RELEASE_NOTICE.txt", L"PACKAGE_MANIFEST.txt",
        L"THIRD_PARTY_LICENSES/NVIDIA-DLSS-SDK.txt",
        L"THIRD_PARTY_LICENSES/tabler-MIT.txt", L"docs/ARCHITECTURE.md",
        L"docs/BUILDING.md", L"docs/DLSS5_SETUP.md", L"docs/RELATED_PROJECTS.md",
        L"docs/TROUBLESHOOTING.md"
    };
    for (const auto path : allowed) CHECK(IsAllowedPublicPath(path));

    const std::array<std::wstring_view, 20> forbidden = {
        L"nvngx_dlssnr.dll", L"renodx-dlss5.addon64", L"dxgi.dll",
        L"ReShade.ini", L"ReShadePreset.ini", L"sl.common.dll", L"sl.dlss.dll",
        L"sl.dlss_g.dll", L"sl.dlss_nr.dll", L"sl.interposer.dll", L"sl.nis.dll",
        L"sl.pcl.dll", L"sl.reflex.dll", L"ffmpeg.exe", L"ffprobe.exe",
        L"yt-dlp.exe", L"deno.exe", L"DLSSVideoPlayer.ini",
        L"DLSSVideoPlayer.log", L"unexpected.dll"
    };
    for (const auto path : forbidden) CHECK(!IsAllowedPublicPath(path));
}

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
    CHECK(youtubeAvailable);
    const ToolbarAvailability intermediate{false, false, false, false};
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

void playback_timeline_follows_the_presented_frame_test()
{
    CHECK_EQ(12.0, playback_timing::TimelinePosition(
        false, 0.0, false, 0.0, 12.0, 12.08));
    CHECK_EQ(12.0, playback_timing::PausePosition(12.0, 12.08));
    CHECK_EQ(18.0, playback_timing::TimelinePosition(
        true, 18.0, false, 0.0, 12.0, 12.08));
    CHECK_EQ(24.0, playback_timing::TimelinePosition(
        false, 0.0, true, 24.0, 12.0, 12.08));
}

void playback_lateness_is_bounded_to_one_and_a_half_frames_test()
{
    CHECK(std::abs(playback_timing::LateFrameThreshold(1.0 / 60.0) - 0.025) < 1e-9);
    CHECK(std::abs(playback_timing::LateFrameThreshold(1.0 / 30.0) - 0.050) < 1e-9);
    CHECK(std::abs(playback_timing::LateFrameThreshold(1.0 / 24.0) - 0.0625) < 1e-9);
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

void button_content_layout_preserves_required_insets_and_icon_gap_at_every_dpi_test()
{
    for(const UINT dpi:{96u,120u,144u,192u}){
        const int width=MulDiv(180,int(dpi),96),height=MulDiv(40,int(dpi),96);
        const SIZE icon{MulDiv(17,int(dpi),96),MulDiv(17,int(dpi),96)};
        const SIZE text{MulDiv(64,int(dpi),96),MulDiv(15,int(dpi),96)};
        const RECT outer{0,0,width,height};
        const auto layout=LayoutButtonContent(outer,icon,text,false,dpi);
        CHECK(outer.bottom-outer.top>=MulDiv(36,int(dpi),96));
        CHECK(layout.icon.left-outer.left>=MulDiv(kButtonHorizontalInsetDip,int(dpi),96));
        CHECK(outer.right-layout.text.right>=MulDiv(kButtonHorizontalInsetDip,int(dpi),96));
        CHECK(layout.icon.top-outer.top>=MulDiv(kButtonVerticalInsetDip,int(dpi),96));
        CHECK(outer.bottom-layout.icon.bottom>=MulDiv(kButtonVerticalInsetDip,int(dpi),96));
        CHECK(layout.text.top-outer.top>=MulDiv(kButtonVerticalInsetDip,int(dpi),96));
        CHECK(outer.bottom-layout.text.bottom>=MulDiv(kButtonVerticalInsetDip,int(dpi),96));
        CHECK(layout.text.left-layout.icon.right>=MulDiv(kButtonIconLabelGapDip,int(dpi),96));
    }
}

void button_content_layout_centers_combined_icon_and_label_without_outline_contact_test()
{
    for(const UINT dpi:{96u,120u,144u,192u}){
        const RECT outer{0,0,MulDiv(220,int(dpi),96),MulDiv(44,int(dpi),96)};
        const auto layout=LayoutButtonContent(
            outer,SIZE{MulDiv(18,int(dpi),96),MulDiv(18,int(dpi),96)},
            SIZE{MulDiv(72,int(dpi),96),MulDiv(16,int(dpi),96)},false,dpi);
        const int leftSpace=layout.content.left-outer.left;
        const int rightSpace=outer.right-layout.content.right;
        CHECK(std::abs(leftSpace-rightSpace)<=1);
        CHECK(leftSpace>=MulDiv(kButtonHorizontalInsetDip,int(dpi),96));
        CHECK(layout.icon.right<=layout.text.left);
    }
}

void prerender_surface_layout_keeps_progress_cancel_and_text_inside_client_bounds_test()
{
    const auto inside=[](const RECT& inner,const RECT& outer){
        return inner.left>=outer.left&&inner.top>=outer.top&&inner.right<=outer.right&&
               inner.bottom<=outer.bottom&&inner.right>=inner.left&&inner.bottom>=inner.top;
    };
    for(const UINT dpi:{96u,120u,144u,192u}){
        const int width=MulDiv(640,int(dpi),96),height=MulDiv(420,int(dpi),96);
        const RECT client{0,0,width,height};const auto layout=LayoutPreRenderSurface(width,height,dpi);
        for(const RECT rect:{layout.title,layout.phase,layout.resolution,layout.frameCount,
                             layout.elapsedEta,layout.size,layout.progressTrack,
                             layout.progressFill,layout.cancelButton})CHECK(inside(rect,client));
        CHECK(layout.cancelButton.right-layout.cancelButton.left<=MulDiv(120,int(dpi),96));
        CHECK(layout.cancelButton.bottom-layout.cancelButton.top<=MulDiv(40,int(dpi),96));
        CHECK(layout.progressFill.left==layout.progressTrack.left);
    }
}

void advanced_menu_contains_clear_neural_cache_and_no_removed_quality_commands_test()
{
    Localizer localizer;const HMENU menu=app_menu::CreateMenuBar(localizer,true);CHECK(menu!=nullptr);
    std::vector<MenuEntry> entries;if(menu)collect_menu_entries(menu,entries);
    CHECK(has_menu_entry(entries,L"Clear Neural Cache",app_menu::IDM_CLEAR_NEURAL_CACHE));
    CHECK(!has_menu_text(entries,L"720p"));CHECK(!has_menu_text(entries,L"480p"));
    CHECK_EQ(std::wstring(L"Acquiring"),localizer.Get(L"neural.phase.acquiring"));
    CHECK_EQ(std::wstring(L"Neural rendered"),localizer.Get(L"neural.view.rendered"));
    if(menu)DestroyMenu(menu);
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
            CHECK((entry.state & (MFS_DISABLED | MFS_GRAYED)) == 0);
        }
    }
    for (const auto& entry : entries) {
        CHECK(entry.command < 500 || entry.command >= 600);
    }

    if (menu) DestroyMenu(menu);
}

void youtube_source_quality_menu_is_distinct_radio_group_and_updates_test()
{
    Localizer localizer;
    const HMENU menu = app_menu::CreateMenuBar(localizer, true);
    CHECK(menu != nullptr);
    HMENU video = find_top_level_submenu(menu, L"Video");
    HMENU quality = find_top_level_submenu(video, L"YouTube source quality");
    CHECK(video != nullptr);
    CHECK(quality != nullptr);

    const std::array expected{
        std::pair{L"Auto (best available)", app_menu::IDM_YOUTUBE_QUALITY_AUTO},
        std::pair{L"2160p", app_menu::IDM_YOUTUBE_QUALITY_2160},
        std::pair{L"1440p", app_menu::IDM_YOUTUBE_QUALITY_1440},
        std::pair{L"1080p", app_menu::IDM_YOUTUBE_QUALITY_1080},
    };
    std::vector<MenuEntry> entries;
    if (quality) collect_menu_entries(quality, entries);
    CHECK_EQ(expected.size(), entries.size());
    for (const auto& [label, command] : expected) {
        CHECK(has_menu_entry(entries, label, command));
        const auto selected = app_menu::YouTubeQualityForCommand(command);
        CHECK(selected.has_value());
        if (selected) CHECK_EQ(command, app_menu::CommandForYouTubeQuality(*selected));
    }
    for (const auto& entry : entries) {
        CHECK_EQ(entry.command == app_menu::IDM_YOUTUBE_QUALITY_AUTO,
                  (entry.state & MFS_CHECKED) != 0);
    }

    CHECK(app_menu::UpdateYouTubeQualitySelection(menu, YouTubeSourceQuality::P1080));
    entries.clear();
    if (quality) collect_menu_entries(quality, entries);
    for (const auto& entry : entries) {
        CHECK_EQ(entry.command == app_menu::IDM_YOUTUBE_QUALITY_1080,
                 (entry.state & MFS_CHECKED) != 0);
    }
    CHECK(!app_menu::YouTubeQualityForCommand(app_menu::IDM_QUALITY_AUTO).has_value());
    CHECK(!app_menu::YouTubeQualityForCommand(414).has_value());
    CHECK(!app_menu::YouTubeQualityForCommand(415).has_value());
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

void youtube_resolution_generation_accepts_only_the_current_completion_test()
{
    YouTubeResolutionLifecycle lifecycle;
    const uint64_t first = lifecycle.Begin();
    CHECK(lifecycle.IsResolving());
    const uint64_t second = lifecycle.Begin();
    CHECK(second > first);
    CHECK(!lifecycle.Complete(first));
    CHECK(lifecycle.IsResolving());
    CHECK(lifecycle.Complete(second));
    CHECK(!lifecycle.IsResolving());
    CHECK(!lifecycle.Complete(second));

    const uint64_t cancelled = lifecycle.Begin();
    lifecycle.Invalidate();
    CHECK(!lifecycle.IsResolving());
    CHECK(!lifecycle.Complete(cancelled));
}

void youtube_resolution_disables_only_conflicting_source_actions_test()
{
    ToolbarAvailability state{};
    state.mediaLoaded = true;
    state.rendererReady = true;
    state.youtubeAvailable = true;
    CHECK(IsToolbarActionEnabled(ToolbarAction::Open, state));
    CHECK(IsToolbarActionEnabled(ToolbarAction::OpenYouTube, state));
    CHECK(IsToolbarActionEnabled(ToolbarAction::PlayPause, state));

    state.resolvingYouTube = true;
    CHECK(!IsToolbarActionEnabled(ToolbarAction::Open, state));
    CHECK(!IsToolbarActionEnabled(ToolbarAction::OpenYouTube, state));
    CHECK(IsToolbarActionEnabled(ToolbarAction::PlayPause, state));
}

void youtube_resolution_error_mapping_is_actionable_and_distinct_test()
{
    CHECK_EQ(std::wstring_view(L"youtube.error.invalid"),
             YouTubeResolveErrorMessageKey(ResolveError::InvalidUrl));
    CHECK_EQ(std::wstring_view(L"youtube.error.helper_missing"),
             YouTubeResolveErrorMessageKey(ResolveError::HelperMissing));
    CHECK_EQ(std::wstring_view(L"youtube.error.start_failed"),
             YouTubeResolveErrorMessageKey(ResolveError::StartFailed));
    CHECK_EQ(std::wstring_view(L"youtube.error.timeout"),
             YouTubeResolveErrorMessageKey(ResolveError::TimedOut));
    CHECK_EQ(std::wstring_view(L"youtube.error.cancelled"),
             YouTubeResolveErrorMessageKey(ResolveError::Cancelled));
    CHECK_EQ(std::wstring_view(L"youtube.error.extraction"),
             YouTubeResolveErrorMessageKey(ResolveError::ExtractionFailed));
    CHECK_EQ(std::wstring_view(L"youtube.error.extraction"),
             YouTubeResolveErrorMessageKey(ResolveError::OutputTooLarge));
    CHECK_EQ(std::wstring_view(L"youtube.error.extraction"),
             YouTubeResolveErrorMessageKey(ResolveError::InvalidOutput));
    Localizer localizer;
    CHECK_EQ(std::wstring(L"The YouTube stream did not become ready within 20 seconds. Check your connection and try again."),localizer.Get(L"youtube.error.media_timeout"));
    CHECK_EQ(std::wstring(L"The YouTube stream stopped delivering video for 15 seconds. Check your connection and try again."),localizer.Get(L"youtube.error.media_stalled"));
}

void youtube_source_forces_ffmpeg_and_never_allows_media_foundation_fallback_test()
{
    CHECK_EQ(DecoderOpenPolicy::FfmpegThenMediaFoundation,
             DecoderPolicyForSource(MediaSourceKind::LocalFile));
    CHECK_EQ(DecoderOpenPolicy::FfmpegOnly,
             DecoderPolicyForSource(MediaSourceKind::YouTube));
}

void youtube_resolution_cancellation_runs_stop_cancel_join_in_order_test()
{
    std::vector<int> order;
    ExecuteYouTubeCancellationSequence(
        [&] { order.push_back(1); },
        [&] { order.push_back(2); },
        [&] { order.push_back(3); });
    CHECK_EQ(std::vector<int>({1, 2, 3}), order);
}

void youtube_display_and_log_labels_never_expose_direct_urls_test()
{
    const std::wstring direct =
        L"https://r1---sn.example.googlevideo.com/videoplayback?expire=1&token=secret";
    CHECK_EQ(std::wstring(L"YouTube video"),
             DisplayTitleForSource(MediaSourceKind::YouTube, L""));
    CHECK_EQ(std::wstring(L"Official game trailer"),
             DisplayTitleForSource(MediaSourceKind::YouTube, L"Official game trailer"));
    CHECK_EQ(std::string_view("YouTube stream"),
             SafeSourceLogLabel(MediaSourceKind::YouTube));
    const std::wstring title = DisplayTitleForSource(MediaSourceKind::YouTube, direct);
    CHECK(title.find(L"https://") == std::wstring::npos);
    CHECK(title.find(L"secret") == std::wstring::npos);
}

void youtube_real_menu_and_ctrl_l_route_share_the_enabled_action_test()
{
    Localizer localizer;
    const HMENU menu = app_menu::CreateMenuBar(localizer, true);
    CHECK(menu != nullptr);
    std::vector<MenuEntry> entries;
    if (menu) collect_menu_entries(menu, entries);
    bool enabled = false;
    for (const auto& entry : entries) {
        if (entry.command != app_menu::IDM_OPEN_YOUTUBE) continue;
        enabled = (entry.state & (MFS_DISABLED | MFS_GRAYED)) == 0;
    }
    CHECK(enabled);
    CHECK(app_menu::RoutesToOpenYouTube(app_menu::PlayerCommandRoute::NativeMenu,
                                        app_menu::IDM_OPEN_YOUTUBE, false));
    CHECK(app_menu::RoutesToOpenYouTube(app_menu::PlayerCommandRoute::KeyDown,
                                        'L', true));
    CHECK(!app_menu::RoutesToOpenYouTube(app_menu::PlayerCommandRoute::KeyDown,
                                         'L', false));
    CHECK(!app_menu::RoutesToOpenYouTube(app_menu::PlayerCommandRoute::KeyDown,
                                         'O', true));
    CHECK(app_menu::UpdateSourceActionAvailability(menu, false, false));
    entries.clear();
    collect_menu_entries(menu, entries);
    for (const auto& entry : entries) {
        if (entry.command == app_menu::IDM_OPEN ||
            entry.command == app_menu::IDM_OPEN_YOUTUBE) {
            CHECK((entry.state & (MFS_DISABLED | MFS_GRAYED)) != 0);
        }
    }
    CHECK(app_menu::UpdateSourceActionAvailability(menu, true, true));
    entries.clear();
    collect_menu_entries(menu, entries);
    for (const auto& entry : entries) {
        if (entry.command == app_menu::IDM_OPEN ||
            entry.command == app_menu::IDM_OPEN_YOUTUBE) {
            CHECK((entry.state & (MFS_DISABLED | MFS_GRAYED)) == 0);
        }
    }
    if (menu) DestroyMenu(menu);
}

void fixed_youtube_examples_are_complete_safe_and_menu_routable_test()
{
    CHECK_EQ(size_t{6}, kExampleVideos.size());

    size_t games = 0;
    size_t anime = 0;
    std::vector<std::wstring_view> urls;
    for (const ExampleVideo& example : kExampleVideos) {
        CHECK(!example.title.empty());
        CHECK(!example.channel.empty());
        CHECK(example.url.starts_with(L"https://www.youtube.com/watch?v="));
        CHECK(IsSupportedYouTubeUrl(example.url));
        CHECK(std::find(urls.begin(), urls.end(), example.url) == urls.end());
        urls.push_back(example.url);
        if (example.category == ExampleVideoCategory::Games) ++games;
        if (example.category == ExampleVideoCategory::Anime) ++anime;
    }
    CHECK_EQ(size_t{3}, games);
    CHECK_EQ(size_t{3}, anime);

    Localizer localizer;
    const HMENU menu = app_menu::CreateMenuBar(localizer, true);
    CHECK(menu != nullptr);
    HMENU file = find_top_level_submenu(menu, L"File");
    HMENU examples = find_top_level_submenu(file, L"Examples");
    HMENU gamesMenu = find_top_level_submenu(examples, L"Games");
    HMENU animeMenu = find_top_level_submenu(examples, L"Anime");
    CHECK(file != nullptr);
    CHECK(examples != nullptr);
    CHECK(gamesMenu != nullptr);
    CHECK(animeMenu != nullptr);
    std::vector<MenuEntry> entries;
    if (menu) collect_menu_entries(menu, entries);
    for (size_t index = 0; index < kExampleVideos.size(); ++index) {
        const UINT command = app_menu::IDM_EXAMPLE_VIDEO_FIRST + static_cast<UINT>(index);
        CHECK(app_menu::ExampleVideoForCommand(command) == &kExampleVideos[index]);
        CHECK(has_menu_entry(entries, kExampleVideos[index].title,
                             command));
    }
    std::vector<MenuEntry> gameEntries;
    std::vector<MenuEntry> animeEntries;
    if (gamesMenu) collect_menu_entries(gamesMenu, gameEntries);
    if (animeMenu) collect_menu_entries(animeMenu, animeEntries);
    CHECK_EQ(size_t{3}, gameEntries.size());
    CHECK_EQ(size_t{3}, animeEntries.size());
    CHECK(app_menu::ExampleVideoForCommand(app_menu::IDM_OPEN_YOUTUBE) == nullptr);
    if (menu) DestroyMenu(menu);
}

struct RegistryCompletion {
    uint64_t generation{};
    int value{};
};

void youtube_completion_registry_is_scalar_once_only_and_spoof_safe_test()
{
    CompletionRegistry<RegistryCompletion> registry;
    const uint64_t first = registry.Register(std::make_unique<RegistryCompletion>(RegistryCompletion{7, 41}));
    const uint64_t second = registry.Register(std::make_unique<RegistryCompletion>(RegistryCompletion{8, 42}));
    CHECK(first != 0);
    CHECK(second > first);
    CHECK(!registry.Take(first + second + 1000));
    auto owned = registry.Take(first);
    CHECK(owned != nullptr);
    if (owned) {
        CHECK_EQ(uint64_t{7}, owned->generation);
        CHECK_EQ(41, owned->value);
    }
    CHECK(!registry.Take(first));
    CHECK_EQ(size_t{1}, registry.Size());
    registry.Clear();
    CHECK_EQ(size_t{0}, registry.Size());
    CHECK(!registry.Take(second));
}

void youtube_completion_registry_post_failure_and_concurrency_are_owned_test()
{
    CompletionRegistry<RegistryCompletion> registry;
    const uint64_t failed = registry.RegisterAndPost(
        std::make_unique<RegistryCompletion>(RegistryCompletion{1, 9}),
        [](uint64_t) { return false; });
    CHECK_EQ(uint64_t{0}, failed);
    CHECK_EQ(size_t{0}, registry.Size());

    constexpr int count = 200;
    std::atomic<int> taken{0};
    std::vector<uint64_t> tokens;
    tokens.reserve(count);
    std::mutex tokensMutex;
    std::jthread producer([&] {
        for (int index = 0; index < count; ++index) {
            const uint64_t token = registry.Register(
                std::make_unique<RegistryCompletion>(RegistryCompletion{2, index}));
            std::scoped_lock lock(tokensMutex);
            tokens.push_back(token);
        }
    });
    producer.join();
    std::jthread firstTaker([&] {
        for (const uint64_t token : tokens) if (registry.Take(token)) ++taken;
    });
    std::jthread secondTaker([&] {
        for (const uint64_t token : tokens) if (registry.Take(token)) ++taken;
    });
    firstTaker.join();
    secondTaker.join();
    CHECK_EQ(count, taken.load());
    CHECK_EQ(size_t{0}, registry.Size());

    const uint64_t stale = registry.Register(
        std::make_unique<RegistryCompletion>(RegistryCompletion{3, 17}));
    registry.Clear(); // models destroy/new-source invalidation after worker join.
    CHECK(!registry.Take(stale));

    std::atomic<bool> registering{true};
    std::jthread concurrentProducer([&]{for(int index=0;index<500;++index)registry.Register(std::make_unique<RegistryCompletion>(RegistryCompletion{4,index}));registering=false;});
    std::jthread concurrentClearer([&]{while(registering.load()){registry.Clear();Sleep(0);}registry.Clear();});
    std::jthread concurrentTaker([&]{uint64_t token=1;while(registering.load()){registry.Take(token++);Sleep(0);}});
    concurrentProducer.join();concurrentClearer.join();concurrentTaker.join();registry.Clear();CHECK_EQ(size_t{0},registry.Size());
}

NetworkRenderConfiguration render_configuration(int quality,uint32_t decodeWidth=1280,uint32_t decodeHeight=720,uint32_t inputWidth=1280,uint32_t inputHeight=720)
{
    NetworkRenderConfiguration config{};config.sourceWidth=1920;config.sourceHeight=1080;config.decodeWidth=decodeWidth;config.decodeHeight=decodeHeight;config.inputWidth=inputWidth;config.inputHeight=inputHeight;config.outputWidth=1920;config.outputHeight=1080;config.guideWidth=320;config.guideHeight=180;config.quality=quality;return config;
}

void youtube_renderer_transaction_validates_every_open_seek_and_quality_candidate_geometry_test()
{
    const auto active=render_configuration(1);
    CHECK(NetworkPreparedGeometryIsValid(active,1280,720,
          static_cast<size_t>(1280)*720*4));
    auto geometry=active;geometry.decodeWidth=960;geometry.inputWidth=960;
    CHECK(NetworkPreparedGeometryIsValid(geometry,960,720,
          static_cast<size_t>(960)*720*4));
    // MaxPerf, Balanced, MaxQuality, UltraPerformance, and DLAA are every
    // explicit quality route exposed by the native menu.
    uint32_t decodeWidth=832;
    for(const int quality:{0,1,2,3,5}){
        const auto prepared=render_configuration(quality,decodeWidth,468,decodeWidth,468);
        CHECK(prepared.decodeWidth==decodeWidth&&prepared.decodeHeight==468);
        CHECK(prepared.inputWidth==decodeWidth&&prepared.inputHeight==468);
        CHECK(prepared.outputWidth==1920&&prepared.outputHeight==1080);
        CHECK(NetworkPreparedGeometryIsValid(prepared,decodeWidth,468,
              static_cast<size_t>(decodeWidth)*468*4));
        CHECK(!NetworkPreparedGeometryIsValid(prepared,decodeWidth+2,468,
               static_cast<size_t>(decodeWidth)*468*4));
        CHECK(!NetworkPreparedGeometryIsValid(prepared,decodeWidth,468,
               static_cast<size_t>(decodeWidth)*468*4-1));
        decodeWidth+=64;
    }
}

void youtube_renderer_transaction_validates_before_atomic_handoff_and_rolls_back_test()
{
    struct Candidate{int id;};
    std::vector<int> order;int activeRenderer=10,activeMedia=20,activeAudio=30;
    const bool failed=ExecuteNetworkCandidateTransaction<Candidate>(
        [&]{order.push_back(1);return std::make_unique<Candidate>(Candidate{11});},
        [&](Candidate&){order.push_back(2);return false;},
        [&](std::unique_ptr<Candidate>){order.push_back(3);activeRenderer=11;activeMedia=21;activeAudio=31;});
    CHECK(!failed);CHECK_EQ(std::vector<int>({1,2}),order);CHECK_EQ(10,activeRenderer);CHECK_EQ(20,activeMedia);CHECK_EQ(30,activeAudio);
    order.clear();
    const bool committed=ExecuteNetworkCandidateTransaction<Candidate>(
        [&]{order.push_back(1);return std::make_unique<Candidate>(Candidate{12});},
        [&](Candidate&){order.push_back(2);return true;},
        [&](std::unique_ptr<Candidate> candidate){order.push_back(3);activeAudio=0;order.push_back(4);activeMedia=22;activeRenderer=candidate->id;order.push_back(5);activeAudio=32;});
    CHECK(committed);CHECK_EQ(std::vector<int>({1,2,3,4,5}),order);CHECK_EQ(12,activeRenderer);CHECK_EQ(22,activeMedia);CHECK_EQ(32,activeAudio);
}

void youtube_candidate_seek_render_failure_preserves_all_active_state_before_commit_test()
{
    struct PlaybackState {
        int decoder{};
        int audio{};
        int renderer{};
        int renderWindow{};
        int quality{};
        bool qualityExplicit{};
        bool playing{};
        double position{};
        int64_t lastRenderedTimestamp{};
        int guideHistory{};
        int dlssHistory{};
        bool operator==(const PlaybackState&) const = default;
    };
    struct Candidate {
        bool renderFirst{};
        int decoder{};
        int audio{};
        int renderer{};
        int renderWindow{};
        int quality{};
    };

    PlaybackState active{10,20,30,40,2,true,true,17.5,175000000,51,61};
    const PlaybackState before=active;
    std::vector<int> order;
    bool commitCalled=false;
    const bool renderFirst=false;
    const bool rejected=ExecuteNetworkCandidateTransaction<Candidate>(
        [&]{order.push_back(1);return std::make_unique<Candidate>(
            Candidate{renderFirst,11,21,31,41,3});},
        [&](Candidate& candidate){order.push_back(2);return candidate.renderFirst;},
        [&](std::unique_ptr<Candidate> candidate){
            order.push_back(3);commitCalled=true;
            active={candidate->decoder,candidate->audio,candidate->renderer,
                    candidate->renderWindow,candidate->quality,false,false,
                    42.0,420000000,0,0};
        });
    CHECK(!rejected);
    CHECK_EQ(std::vector<int>({1,2}),order);
    CHECK(!commitCalled);
    CHECK_EQ(before,active);

    order.clear();
    int oldStopCount=0,oldRendererDestroyCount=0;
    const bool committed=ExecuteNetworkCandidateTransaction<Candidate>(
        [&]{order.push_back(1);return std::make_unique<Candidate>(
            Candidate{true,12,22,32,42,4});},
        [&](Candidate& candidate){order.push_back(2);return candidate.renderFirst;},
        [&](std::unique_ptr<Candidate> candidate){
            CommitPreparedAudioHandoff(
                [&]{order.push_back(3);active={candidate->decoder,candidate->audio,
                    candidate->renderer,candidate->renderWindow,candidate->quality,
                    false,false,24.0,240000000,0,0};},
                [&]{order.push_back(4);return true;},
                [&]{order.push_back(5);++oldStopCount;++oldRendererDestroyCount;},
                [&]{order.push_back(6);active.playing=true;});
        });
    CHECK(committed);
    CHECK_EQ(std::vector<int>({1,2,4,3,5,6}),order);
    CHECK_EQ(1,oldStopCount);
    CHECK_EQ(1,oldRendererDestroyCount);
    CHECK_EQ(12,active.decoder);CHECK_EQ(22,active.audio);
    CHECK_EQ(32,active.renderer);CHECK_EQ(42,active.renderWindow);
    CHECK_EQ(4,active.quality);CHECK(!active.qualityExplicit);CHECK(active.playing);
    CHECK_EQ(24.0,active.position);CHECK_EQ(int64_t{240000000},active.lastRenderedTimestamp);
    CHECK_EQ(0,active.guideHistory);CHECK_EQ(0,active.dlssHistory);
}

void youtube_network_read_decisions_are_identical_and_once_only_at_both_positions_test()
{
    for(const NetworkReadPosition position:{NetworkReadPosition::BeforeRender,NetworkReadPosition::AfterRender}){
        NetworkReadState state;
        auto wait=state.Resolve(VideoReadResult::NotReady,position);CHECK_EQ(NetworkReadAction::Wait,wait.action);CHECK(!wait.notify);
        auto ready=state.Resolve(VideoReadResult::FrameReady,position);CHECK_EQ(NetworkReadAction::UseFrame,ready.action);CHECK(!ready.notify);
        auto stalled=state.Resolve(VideoReadResult::Stalled,position);CHECK_EQ(NetworkReadAction::StopError,stalled.action);CHECK(stalled.notify);CHECK_EQ(std::wstring_view(L"youtube.error.media_stalled"),stalled.messageKey);
        auto repeated=state.Resolve(VideoReadResult::Stalled,position);CHECK_EQ(NetworkReadAction::StopError,repeated.action);CHECK(!repeated.notify);
        state.Reset();auto error=state.Resolve(VideoReadResult::Error,position);CHECK_EQ(NetworkReadAction::StopError,error.action);CHECK(error.notify);CHECK_EQ(std::wstring_view(L"youtube.error.ffmpeg"),error.messageKey);
        state.Reset();auto ended=state.Resolve(VideoReadResult::EndOfStream,position);CHECK_EQ(NetworkReadAction::StopClean,ended.action);CHECK(!ended.notify);
        state.Reset();auto cancelled=state.Resolve(VideoReadResult::Cancelled,position);CHECK_EQ(NetworkReadAction::StopCancelled,cancelled.action);CHECK(!cancelled.notify);
    }
}

void youtube_async_transaction_coalesces_and_discards_stale_work_before_handoff_test()
{
    struct Prepared {
        uint64_t generation{};
        NetworkRenderConfiguration configuration;
        bool preparationOk{true};
        int decoder{};
        int audio{};
    };

    YouTubeResolutionLifecycle lifecycle;
    CompletionRegistry<Prepared> registry;
    const auto activeConfiguration=render_configuration(2);
    int activeDecoder=10,activeRenderer=20,activeAudio=30;

    const uint64_t firstGeneration=lifecycle.Begin();
    const uint64_t firstToken=registry.Register(std::make_unique<Prepared>(Prepared{
        firstGeneration,activeConfiguration,true,11,31}));
    const uint64_t secondGeneration=lifecycle.Begin();
    const uint64_t secondToken=registry.Register(std::make_unique<Prepared>(Prepared{
        secondGeneration,activeConfiguration,true,12,32}));
    const uint64_t thirdGeneration=lifecycle.Begin();
    const auto qualityConfiguration=render_configuration(3,960,540,960,540);
    const uint64_t thirdToken=registry.Register(std::make_unique<Prepared>(Prepared{
        thirdGeneration,qualityConfiguration,false,13,33}));

    auto first=registry.Take(firstToken);CHECK(first!=nullptr);CHECK(!lifecycle.Complete(first->generation));
    auto second=registry.Take(secondToken);CHECK(second!=nullptr);CHECK(!lifecycle.Complete(second->generation));
    auto third=registry.Take(thirdToken);CHECK(third!=nullptr);CHECK(lifecycle.Complete(third->generation));
    if(!third->preparationOk){
        CHECK_EQ(10,activeDecoder);CHECK_EQ(20,activeRenderer);CHECK_EQ(30,activeAudio);
    }

    const uint64_t fourthGeneration=lifecycle.Begin();
    const uint64_t fourthToken=registry.Register(std::make_unique<Prepared>(Prepared{
        fourthGeneration,qualityConfiguration,true,14,34}));
    auto fourth=registry.Take(fourthToken);CHECK(fourth!=nullptr);CHECK(lifecycle.Complete(fourth->generation));
    std::vector<int> handoff;int retiringAudio=activeAudio;
    const bool committed=ExecuteNetworkCandidateTransaction<int>(
        [&]{return std::make_unique<int>(24);},
        [&](int& renderer){return renderer==24&&NetworkPreparedGeometryIsValid(
            fourth->configuration,960,540,static_cast<size_t>(960)*540*4);},
        [&](std::unique_ptr<int> renderer){
            CommitPreparedAudioHandoff(
                [&]{handoff.push_back(1);activeDecoder=fourth->decoder;activeRenderer=*renderer;activeAudio=fourth->audio;},
                [&]{handoff.push_back(2);return true;},
                [&]{handoff.push_back(3);retiringAudio=0;},
                [&]{handoff.push_back(4);});
        });
    CHECK(committed);CHECK_EQ(std::vector<int>({2,1,3,4}),handoff);
    CHECK_EQ(0,retiringAudio);
    CHECK_EQ(14,activeDecoder);CHECK_EQ(24,activeRenderer);CHECK_EQ(34,activeAudio);
    CHECK(!registry.Take(fourthToken));

    const uint64_t cancelledGeneration=lifecycle.Begin();
    const uint64_t cancelledToken=registry.Register(std::make_unique<Prepared>(Prepared{
        cancelledGeneration,activeConfiguration,true,15,35}));
    lifecycle.Invalidate();registry.Clear();
    CHECK(!registry.Take(cancelledToken));CHECK(!lifecycle.Complete(cancelledGeneration));
}

void youtube_stale_and_cancelled_prepared_seek_ownership_is_destroyed_once_test()
{
    struct Prepared {
        Prepared(uint64_t generation,int* destroyed)
            : generation(generation),destroyed(destroyed) {}
        ~Prepared(){if(destroyed)++*destroyed;}
        uint64_t generation{};
        int* destroyed{};
    };

    YouTubeResolutionLifecycle lifecycle;
    CompletionRegistry<Prepared> registry;
    int destroyed=0;
    const uint64_t staleGeneration=lifecycle.Begin();
    const uint64_t staleToken=registry.Register(
        std::make_unique<Prepared>(staleGeneration,&destroyed));
    const uint64_t currentGeneration=lifecycle.Begin();
    const uint64_t currentToken=registry.Register(
        std::make_unique<Prepared>(currentGeneration,&destroyed));

    auto stale=registry.Take(staleToken);
    CHECK(stale!=nullptr);
    CHECK(!lifecycle.Complete(stale->generation));
    stale.reset();
    CHECK_EQ(1,destroyed);

    auto current=registry.Take(currentToken);
    CHECK(current!=nullptr);
    CHECK(lifecycle.Complete(current->generation));
    current.reset();
    CHECK_EQ(2,destroyed);

    const uint64_t cancelledGeneration=lifecycle.Begin();
    const uint64_t cancelledToken=registry.Register(
        std::make_unique<Prepared>(cancelledGeneration,&destroyed));
    lifecycle.Invalidate();
    registry.Clear();
    CHECK_EQ(3,destroyed);
    CHECK(!registry.Take(cancelledToken));
    CHECK(!lifecycle.Complete(cancelledGeneration));
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

void gpu_teardown_fence_signal_failure_stops_before_event_registration_test()
{
    int completionQueries=0,eventRegistrations=0,waits=0;
    const auto result=d3d12_renderer_detail::WaitForGPUFenceTeardown(
        uint64_t{41},
        [&](uint64_t value){CHECK_EQ(uint64_t{41},value);return E_FAIL;},
        [&]{++completionQueries;return uint64_t{0};},
        [&](uint64_t){++eventRegistrations;return S_OK;},
        [&](DWORD){++waits;return DWORD{WAIT_OBJECT_0};});

    CHECK_EQ(d3d12_renderer_detail::FenceWaitResult::SignalFailed,result);
    CHECK_EQ(0,completionQueries);CHECK_EQ(0,eventRegistrations);CHECK_EQ(0,waits);
}

void gpu_teardown_fence_signal_failure_maps_to_device_removed_when_device_reason_failed_test()
{
    int reasonChecks=0,eventRegistrations=0,waits=0;
    const auto result=d3d12_renderer_detail::WaitForGPUFenceTeardown(
        uint64_t{411},
        [](uint64_t){return E_FAIL;},
        []{return uint64_t{0};},
        [&](uint64_t){++eventRegistrations;return S_OK;},
        [&](DWORD){++waits;return DWORD{WAIT_OBJECT_0};},
        [&]{++reasonChecks;return DXGI_ERROR_DEVICE_REMOVED;});

    CHECK_EQ(d3d12_renderer_detail::FenceWaitResult::DeviceRemoved,result);
    CHECK_EQ(1,reasonChecks);CHECK_EQ(0,eventRegistrations);CHECK_EQ(0,waits);
}

void gpu_teardown_fence_event_registration_failure_stops_before_wait_test()
{
    int eventRegistrations=0,waits=0;
    const auto result=d3d12_renderer_detail::WaitForGPUFenceTeardown(
        uint64_t{42},
        [](uint64_t){return S_OK;},
        []{return uint64_t{0};},
        [&](uint64_t value){CHECK_EQ(uint64_t{42},value);++eventRegistrations;return E_FAIL;},
        [&](DWORD){++waits;return DWORD{WAIT_OBJECT_0};});

    CHECK_EQ(d3d12_renderer_detail::FenceWaitResult::EventRegistrationFailed,result);
    CHECK_EQ(1,eventRegistrations);CHECK_EQ(0,waits);
}

void gpu_teardown_fence_wait_failure_is_bounded_and_reported_test()
{
    DWORD observedTimeout=INFINITE;
    const auto result=d3d12_renderer_detail::WaitForGPUFenceTeardown(
        uint64_t{43},
        [](uint64_t){return S_OK;},
        []{return uint64_t{0};},
        [](uint64_t){return S_OK;},
        [&](DWORD timeout){observedTimeout=timeout;return DWORD{WAIT_FAILED};});

    CHECK_EQ(d3d12_renderer_detail::FenceWaitResult::WaitFailed,result);
    CHECK(observedTimeout!=INFINITE);CHECK(observedTimeout<=DWORD{2000});
}

void gpu_teardown_fence_timeout_is_bounded_and_reported_test()
{
    DWORD observedTimeout=INFINITE;
    const auto result=d3d12_renderer_detail::WaitForGPUFenceTeardown(
        uint64_t{44},
        [](uint64_t){return S_OK;},
        []{return uint64_t{0};},
        [](uint64_t){return S_OK;},
        [&](DWORD timeout){observedTimeout=timeout;return DWORD{WAIT_TIMEOUT};});

    CHECK_EQ(d3d12_renderer_detail::FenceWaitResult::TimedOut,result);
    CHECK(observedTimeout!=INFINITE);CHECK(observedTimeout<=DWORD{2000});
}

void gpu_teardown_fence_ignores_old_event_wake_until_new_target_completes_test()
{
    int completionQueries=0,waits=0;
    const auto result=d3d12_renderer_detail::WaitForGPUFenceTeardown(
        uint64_t{45},
        [](uint64_t){return S_OK;},
        [&]{++completionQueries;return completionQueries>=3?uint64_t{45}:uint64_t{12};},
        [](uint64_t){return S_OK;},
        [&](DWORD){++waits;return DWORD{WAIT_OBJECT_0};});

    CHECK_EQ(d3d12_renderer_detail::FenceWaitResult::Completed,result);
    CHECK_EQ(3,completionQueries);
    CHECK_EQ(2,waits);
}

void gpu_teardown_fence_consecutive_timeout_does_not_let_old_registration_complete_new_target_test()
{
    uint64_t completed=12;
    std::vector<uint64_t> registrations;
    const auto first=d3d12_renderer_detail::WaitForGPUFenceTeardown(
        uint64_t{45},
        [](uint64_t){return S_OK;},
        [&]{return completed;},
        [&](uint64_t value){registrations.push_back(value);return S_OK;},
        [](DWORD){return DWORD{WAIT_TIMEOUT};});
    CHECK_EQ(d3d12_renderer_detail::FenceWaitResult::TimedOut,first);

    int secondWaits=0;
    const auto second=d3d12_renderer_detail::WaitForGPUFenceTeardown(
        uint64_t{46},
        [](uint64_t){return S_OK;},
        [&]{return completed;},
        [&](uint64_t value){registrations.push_back(value);return S_OK;},
        [&](DWORD){
            completed=++secondWaits==1?uint64_t{45}:uint64_t{46};
            return DWORD{WAIT_OBJECT_0};
        });

    CHECK_EQ(d3d12_renderer_detail::FenceWaitResult::Completed,second);
    CHECK_EQ(std::vector<uint64_t>({45,46}),registrations);
    CHECK_EQ(2,secondWaits);
}

void gpu_teardown_fence_device_removed_sentinel_is_not_completion_test()
{
    int completionQueries=0;
    int eventRegistrations=0,waits=0;
    const auto result=d3d12_renderer_detail::WaitForGPUFenceTeardown(
        uint64_t{46},
        [](uint64_t){return S_OK;},
        [&]{return ++completionQueries==1?uint64_t{0}:UINT64_MAX;},
        [&](uint64_t){++eventRegistrations;return S_OK;},
        [&](DWORD){++waits;return DWORD{WAIT_OBJECT_0};});

    CHECK_EQ(d3d12_renderer_detail::FenceWaitResult::DeviceRemoved,result);
    CHECK_EQ(1,eventRegistrations);
    CHECK_EQ(1,waits);
}

void gpu_teardown_fence_stale_wakes_share_one_absolute_timeout_budget_test()
{
    std::vector<DWORD> timeouts;
    int waits=0;
    const auto result=d3d12_renderer_detail::WaitForGPUFenceTeardown(
        uint64_t{47},
        [](uint64_t){return S_OK;},
        []{return uint64_t{8};},
        [](uint64_t){return S_OK;},
        [&](DWORD timeout){
            timeouts.push_back(timeout);
            if(++waits==1){Sleep(25);return DWORD{WAIT_OBJECT_0};}
            return DWORD{WAIT_TIMEOUT};
        });

    CHECK_EQ(d3d12_renderer_detail::FenceWaitResult::TimedOut,result);
    CHECK_EQ(size_t{2},timeouts.size());
    if(timeouts.size()==2){CHECK(timeouts[0]<=DWORD{2000});CHECK(timeouts[1]<timeouts[0]);}
}

void renderer_non_teardown_wait_failure_is_propagated_test()
{
    int waits=0;
    auto renderer=MakeD3D12Renderer();
    D3D12RendererTestAccess::ConfigureWait(
        *renderer,d3d12_renderer_detail::FenceWaitResult::TimedOut,waits);

    CHECK(!D3D12RendererTestAccess::WaitForContinuedUse(*renderer));
    CHECK_EQ(1,waits);
    renderer.reset();
    CHECK_EQ(1,waits);
}

void renderer_safe_owner_releases_owned_resources_only_after_completed_or_removed_drain_test()
{
    for(const auto result:{d3d12_renderer_detail::FenceWaitResult::Completed,
                           d3d12_renderer_detail::FenceWaitResult::DeviceRemoved}){
        int waits=0;auto destroyed=std::make_shared<int>(0);
        auto renderer=MakeD3D12Renderer();
        D3D12RendererTestAccess::ConfigureWait(*renderer,result,waits);
        D3D12RendererTestAccess::OwnSentinel(
            *renderer,std::make_unique<RendererOwnedSentinel>(destroyed));

        renderer.reset();

        CHECK_EQ(1,waits);
        CHECK_EQ(1,*destroyed);
    }
}

void renderer_safe_owner_retains_resources_after_live_device_drain_failure_test()
{
    for(const auto result:{d3d12_renderer_detail::FenceWaitResult::SignalFailed,
                           d3d12_renderer_detail::FenceWaitResult::EventRegistrationFailed,
                           d3d12_renderer_detail::FenceWaitResult::WaitFailed,
                           d3d12_renderer_detail::FenceWaitResult::TimedOut}){
        int waits=0;auto destroyed=std::make_shared<int>(0);
        auto renderer=MakeD3D12Renderer();
        D3D12RendererTestAccess::ConfigureWait(*renderer,result,waits);
        D3D12RendererTestAccess::OwnSentinel(
            *renderer,std::make_unique<RendererOwnedSentinel>(destroyed));

        renderer.reset();

        CHECK_EQ(1,waits);
        CHECK_EQ(0,*destroyed);
    }
}

void renderer_frame_signal_failure_is_cached_without_advancing_tracking_test()
{
    int signalCalls=0,reasonChecks=0;
    auto destroyed=std::make_shared<int>(0);
    auto renderer=MakeD3D12Renderer();
    D3D12RendererTestAccess::ConfigureFrameSignal(
        *renderer,E_FAIL,S_OK,signalCalls,reasonChecks);
    D3D12RendererTestAccess::SetFrameTracking(*renderer,1,9,1,4);
    D3D12RendererTestAccess::OwnSentinel(
        *renderer,std::make_unique<RendererOwnedSentinel>(destroyed));

    CHECK(!D3D12RendererTestAccess::SignalFrameSlot(*renderer,1));
    CHECK_EQ(1,signalCalls);CHECK_EQ(1,reasonChecks);
    CHECK_EQ(uint32_t{1},D3D12RendererTestAccess::FrameSlot(*renderer));
    CHECK_EQ(uint64_t{9},D3D12RendererTestAccess::FenceValue(*renderer));
    CHECK_EQ(uint64_t{4},D3D12RendererTestAccess::FrameFence(*renderer,1));
    CHECK(D3D12RendererTestAccess::GPUUnusable(*renderer));
    CHECK_EQ(d3d12_renderer_detail::FenceWaitResult::SignalFailed,
             D3D12RendererTestAccess::LastFenceResult(*renderer));
    CHECK(!D3D12RendererTestAccess::SignalFrameSlot(*renderer,1));
    CHECK_EQ(1,signalCalls);

    renderer.reset();
    CHECK_EQ(0,*destroyed);
}

void renderer_frame_signal_device_removal_is_cached_and_safe_owner_releases_test()
{
    int signalCalls=0,reasonChecks=0;
    auto destroyed=std::make_shared<int>(0);
    auto renderer=MakeD3D12Renderer();
    D3D12RendererTestAccess::ConfigureFrameSignal(
        *renderer,E_FAIL,DXGI_ERROR_DEVICE_REMOVED,signalCalls,reasonChecks);
    D3D12RendererTestAccess::SetFrameTracking(*renderer,2,12,2,8);
    D3D12RendererTestAccess::OwnSentinel(
        *renderer,std::make_unique<RendererOwnedSentinel>(destroyed));

    CHECK(!D3D12RendererTestAccess::SignalFrameSlot(*renderer,2));
    CHECK_EQ(1,signalCalls);CHECK_EQ(1,reasonChecks);
    CHECK_EQ(uint32_t{2},D3D12RendererTestAccess::FrameSlot(*renderer));
    CHECK_EQ(uint64_t{12},D3D12RendererTestAccess::FenceValue(*renderer));
    CHECK_EQ(uint64_t{8},D3D12RendererTestAccess::FrameFence(*renderer,2));
    CHECK_EQ(d3d12_renderer_detail::FenceWaitResult::DeviceRemoved,
             D3D12RendererTestAccess::LastFenceResult(*renderer));

    renderer.reset();
    CHECK_EQ(1,*destroyed);
}

void renderer_frame_signal_success_advances_tracking_once_test()
{
    int signalCalls=0,reasonChecks=0;
    auto renderer=MakeD3D12Renderer();
    D3D12RendererTestAccess::ConfigureFrameSignal(
        *renderer,S_OK,S_OK,signalCalls,reasonChecks);
    D3D12RendererTestAccess::SetFrameTracking(*renderer,0,20,0,14);

    CHECK(D3D12RendererTestAccess::SignalFrameSlot(*renderer,0));
    CHECK_EQ(1,signalCalls);CHECK_EQ(0,reasonChecks);
    CHECK_EQ(uint32_t{1},D3D12RendererTestAccess::FrameSlot(*renderer));
    CHECK_EQ(uint64_t{21},D3D12RendererTestAccess::FenceValue(*renderer));
    CHECK_EQ(uint64_t{21},D3D12RendererTestAccess::FrameFence(*renderer,0));
    CHECK(!D3D12RendererTestAccess::GPUUnusable(*renderer));
}

void renderer_cache_capture_requires_a_successful_neural_evaluation_test()
{
    auto renderer=MakeD3D12Renderer();int captures=0;
    D3D12RendererTestAccess::ConfigureCacheCapture(
        *renderer,2,2,false,[&](std::vector<uint8_t>&){++captures;return true;});
    CapturedVideoFrame frame;frame.bgra.assign(7,0x55);frame.width=9;frame.height=9;
    CHECK(!D3D12RendererTestAccess::CaptureEvaluatedFrame(*renderer,frame));
    CHECK_EQ(0,captures);CHECK(frame.bgra.empty());CHECK_EQ(uint32_t{0},frame.width);
    CHECK_EQ(uint32_t{0},frame.height);
}

void renderer_cache_capture_returns_exact_tight_bgra_geometry_test()
{
    auto renderer=MakeD3D12Renderer();
    D3D12RendererTestAccess::ConfigureCacheCapture(
        *renderer,2,2,true,[](std::vector<uint8_t>& bytes){
            bytes={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};return true;
        });
    CapturedVideoFrame frame;
    CHECK(D3D12RendererTestAccess::CaptureEvaluatedFrame(*renderer,frame));
    CHECK_EQ(uint32_t{2},frame.width);CHECK_EQ(uint32_t{2},frame.height);
    CHECK_EQ(size_t{16},frame.bgra.size());CHECK_EQ(uint8_t{15},frame.bgra.back());

    D3D12RendererTestAccess::ConfigureCacheCapture(
        *renderer,2,2,true,[](std::vector<uint8_t>& bytes){bytes.assign(17,0);return true;});
    CHECK(!D3D12RendererTestAccess::CaptureEvaluatedFrame(*renderer,frame));
    CHECK(frame.bgra.empty());CHECK_EQ(uint32_t{0},frame.width);CHECK_EQ(uint32_t{0},frame.height);
}

void renderer_cache_capture_wait_failure_never_exposes_partial_bytes_test()
{
    auto renderer=MakeD3D12Renderer();
    D3D12RendererTestAccess::ConfigureCacheCapture(
        *renderer,2,2,true,[](std::vector<uint8_t>& bytes){bytes.assign(8,0x44);return false;});
    CapturedVideoFrame frame;frame.bgra.assign(16,0x22);frame.width=2;frame.height=2;
    CHECK(!D3D12RendererTestAccess::CaptureEvaluatedFrame(*renderer,frame));
    CHECK(frame.bgra.empty());CHECK_EQ(uint32_t{0},frame.width);CHECK_EQ(uint32_t{0},frame.height);
}

void renderer_cache_capture_does_not_apply_playback_color_adjustments_test()
{
    auto renderer=MakeD3D12Renderer();
    D3D12Renderer::ColorSettings adjusted{};adjusted.brightness=2.0f;adjusted.contrast=3.0f;
    adjusted.saturation=0.0f;adjusted.gamma=0.25f;adjusted.temperature=1.0f;adjusted.tint=-1.0f;
    renderer->SetColorSettings(adjusted);
    const std::vector<uint8_t> neuralBytes{10,20,30,255};
    D3D12RendererTestAccess::ConfigureCacheCapture(
        *renderer,1,1,true,[&](std::vector<uint8_t>& bytes){bytes=neuralBytes;return true;});
    CapturedVideoFrame frame;
    CHECK(D3D12RendererTestAccess::CaptureEvaluatedFrame(*renderer,frame));
    CHECK_EQ(neuralBytes,frame.bgra);
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

void neural_prerender_defaults_prefer_1080p_and_preserve_explicit_output_test()
{
    const auto experimental = ResolveNeuralRenderDefaults(true, false, 3840, 2160);
    CHECK_EQ(uint32_t{1920}, experimental.width);
    CHECK_EQ(uint32_t{1080}, experimental.height);

    const auto explicitOutput = ResolveNeuralRenderDefaults(true, true, 2560, 1440);
    CHECK_EQ(uint32_t{2560}, explicitOutput.width);
    CHECK_EQ(uint32_t{1440}, explicitOutput.height);

    const auto nativeOnly = ResolveNeuralRenderDefaults(false, false, 3840, 2160);
    CHECK_EQ(uint32_t{3840}, nativeOnly.width);
    CHECK_EQ(uint32_t{2160}, nativeOnly.height);
}

void neural_runtime_layout_is_absent_complete_or_fail_closed_test()
{
    CHECK_EQ(NeuralRuntimeLayout::Absent,
             ClassifyNeuralRuntimeLayout(false, false, false, false));
    CHECK_EQ(NeuralRuntimeLayout::Complete,
             ClassifyNeuralRuntimeLayout(true, true, true, true));

    for (unsigned presentMask = 1; presentMask < 15; ++presentMask) {
        CHECK_EQ(NeuralRuntimeLayout::Incomplete,
                 ClassifyNeuralRuntimeLayout(
                     (presentMask & 1U) != 0,
                     (presentMask & 2U) != 0,
                     (presentMask & 4U) != 0,
                     (presentMask & 8U) != 0));
    }
}

void default_neural_carrier_uses_native_resolution_dlaa_test()
{
    CHECK_EQ(NVSDK_NGX_PerfQuality_Value_DLAA,
             DefaultNeuralCarrierQuality());
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

void neural_addon_runtime_settings_enable_neural_and_disable_upscaling_test()
{
    constexpr std::string_view input =
        "[ADDON]\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64,legacy.addon64\n"
        "[RenoDX.DLSS5]\n"
        "EnableHooks=0\n"
        "NeuralUplift=0\n"
        "NREnableUpscaling=1\n"
        "NRIntensity=1.25\n";
    constexpr std::string_view expected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64\n"
        "[RenoDX.DLSS5]\n"
        "EnableHooks=2\n"
        "NeuralUplift=1\n"
        "NREnableUpscaling=0\n"
        "NRIntensity=1.25\n";

    const std::string updated = UpdateNeuralAddonIni(input, true);
    CHECK_EQ(std::string(expected), updated);
    CHECK_EQ(updated, UpdateNeuralAddonIni(updated, true));
}

void neural_addon_runtime_settings_are_created_without_enabling_upscaling_test()
{
    constexpr std::string_view input =
        "[GENERAL]\r\n"
        "PresetPath=.\\ReShadePreset.ini\r\n";
    constexpr std::string_view expected =
        "[GENERAL]\r\n"
        "PresetPath=.\\ReShadePreset.ini\r\n"
        "[ADDON]\r\n"
        "DisabledAddons=\r\n"
        "[RenoDX.DLSS5]\r\n"
        "EnableHooks=2\r\n"
        "NeuralUplift=1\r\n"
        "NREnableUpscaling=0\r\n";

    CHECK_EQ(std::string(expected), UpdateNeuralAddonIni(input, true));
}

void neural_addon_runtime_settings_fail_closed_on_duplicate_managed_keys_test()
{
    constexpr std::string_view input =
        "[ADDON]\n"
        "DisabledAddons=\n"
        "[RenoDX.DLSS5]\n"
        "NeuralUplift=1\n"
        "NeuralUplift=0\n";
    bool rejected = false;
    try {
        (void)UpdateNeuralAddonIni(input, true);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

void reshade_trailing_section_text_uses_reshade_section_boundaries_test()
{
    constexpr std::string_view input =
        "[ADDON] ; ReShade accepts trailing text\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n"
        "[RenoDX.DLSS5] ; existing settings\n"
        "NRIntensity=1.25\n"
        "[OTHER] ; this must end the RenoDX section\n"
        "Foo=1\n";
    constexpr std::string_view expected =
        "[ADDON] ; ReShade accepts trailing text\n"
        "DisabledAddons=\n"
        "[RenoDX.DLSS5] ; existing settings\n"
        "NRIntensity=1.25\n"
        "EnableHooks=2\n"
        "NeuralUplift=1\n"
        "NREnableUpscaling=0\n"
        "[OTHER] ; this must end the RenoDX section\n"
        "Foo=1\n";

    CHECK_EQ(std::string(expected), UpdateNeuralAddonIni(input, true));
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
        "DisabledAddons=legacy.addon64\n"
        "[RenoDX.DLSS5]\n"
        "EnableHooks=2\n"
        "NeuralUplift=1\n"
        "NREnableUpscaling=0\n";
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
        "DisabledAddons=legacy.addon64\r\n"
        "[RenoDX.DLSS5]\r\n"
        "EnableHooks=2\r\n"
        "NeuralUplift=1\r\n"
        "NREnableUpscaling=0\r\n";
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
        CHECK_EQ(std::wstring(test.expected), result.audioUrl);
        CHECK(result.detail.empty());
        CHECK(result.mediaUrl.find(L'\r') == std::wstring::npos);
        CHECK(result.mediaUrl.find(L'\n') == std::wstring::npos);
    }
}

void resolver_output_accepts_separate_https_video_and_audio_urls_test()
{
    const ResolveResult result = ParseResolverOutput(
        "https://v1.googlevideo.com/videoplayback?id=video\r\n"
        "https://a1.googlevideo.com/videoplayback?id=audio\r\n", 0);
    CHECK(result.ok);
    CHECK_EQ(std::wstring(L"https://v1.googlevideo.com/videoplayback?id=video"),
             result.mediaUrl);
    CHECK_EQ(std::wstring(L"https://a1.googlevideo.com/videoplayback?id=audio"),
             result.audioUrl);
}

void resolver_output_rejects_empty_multiple_oversize_or_untrusted_urls_test()
{
    const std::vector<std::string> rejected{
        "",
        "\r\n",
        "https://a.googlevideo.com/one\nhttps://b.googlevideo.com/two\nhttps://c.googlevideo.com/three",
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
        CHECK(result.audioUrl.empty());
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
        L"--no-cache-dir",
        L"--no-plugin-dirs",
        L"--no-playlist",
        L"--no-warnings",
        L"--js-runtimes",
        LR"(deno:C:\Program Files\DLSS Player\deno.exe)",
        L"-f",
        L"bv[height=1080][ext=mp4]+ba[ext=m4a]/bv[height=1080]+ba/b[height=1080]",
        L"--get-url",
        std::wstring(url),
    };

    CHECK_EQ(expected, BuildYouTubeResolverArguments(
                           helperDirectory, url, YouTubeSourceQuality::P1080));
}

void youtube_source_quality_selectors_prefer_exact_requested_heights_and_auto_fallback_test()
{
    const std::array cases{
        std::pair{YouTubeSourceQuality::Auto,
                  std::wstring_view(L"bv[height=1080][ext=mp4]+ba[ext=m4a]/bv[height=1080]+ba/b[height=1080]/bv[height<=2160][ext=mp4]+ba[ext=m4a]/bv[height<=2160]+ba/b[height<=2160]")},
        std::pair{YouTubeSourceQuality::P2160,
                  std::wstring_view(L"bv[height=2160][ext=mp4]+ba[ext=m4a]/bv[height=2160]+ba/b[height=2160]")},
        std::pair{YouTubeSourceQuality::P1440,
                  std::wstring_view(L"bv[height=1440][ext=mp4]+ba[ext=m4a]/bv[height=1440]+ba/b[height=1440]")},
        std::pair{YouTubeSourceQuality::P1080,
                  std::wstring_view(L"bv[height=1080][ext=mp4]+ba[ext=m4a]/bv[height=1080]+ba/b[height=1080]")},
    };
    for (const auto& [quality, expected] : cases) {
        CHECK_EQ(expected, YouTubeFormatSelector(quality));
    }
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

std::wstring read_environment_variable(std::wstring_view name)
{
    const DWORD needed = GetEnvironmentVariableW(name.data(), nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name.data(), value.data(), needed);
    CHECK(written < needed);
    if (written >= needed) return {};
    value.resize(written);
    return value;
}

struct ScopedEnvironmentVariable {
    explicit ScopedEnvironmentVariable(std::wstring_view name, std::wstring_view value)
        : name(name), previous(read_environment_variable(name))
    {
        hadPrevious = !previous.empty();
        CHECK(SetEnvironmentVariableW(this->name.c_str(), std::wstring(value).c_str()) != FALSE);
    }

    ~ScopedEnvironmentVariable()
    {
        CHECK(SetEnvironmentVariableW(name.c_str(), hadPrevious ? previous.c_str() : nullptr) != FALSE);
    }

    std::wstring name;
    std::wstring previous;
    bool hadPrevious{};
};

size_t count_named_processes(std::wstring_view executableName);
bool wait_for_named_process_count(std::wstring_view executableName,size_t expected,std::chrono::milliseconds timeout);

struct MediaFixture {
    std::filesystem::path directory;
    MediaFixture()
    {
        directory=std::filesystem::temp_directory_path()/(L"PolicyTests-NetworkMedia-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        std::error_code error;std::filesystem::create_directories(directory,error);CHECK(!error);
        CHECK(CopyFileW(current_test_executable().c_str(),(directory/L"ffprobe.exe").c_str(),FALSE)!=FALSE);
        CHECK(CopyFileW(current_test_executable().c_str(),(directory/L"ffmpeg.exe").c_str(),FALSE)!=FALSE);
    }
    ~MediaFixture(){std::error_code error;std::filesystem::remove_all(directory,error);CHECK(!error);}
};

void youtube_decoder_probe_and_frame_reads_are_bounded_nonblocking_test()
{
    MediaFixture fixture;
    {
        auto decoder=VideoDecoderTestAccess::Create(fixture.directory);
        const auto started=std::chrono::steady_clock::now();
        CHECK(decoder->Open(L"https://media.invalid/trickle",MediaSourceKind::YouTube));
        CHECK(std::chrono::steady_clock::now()-started<std::chrono::seconds{1});
        VideoFrame frame;VideoReadResult result=VideoReadResult::NotReady;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{1};
        while(result==VideoReadResult::NotReady&&std::chrono::steady_clock::now()<deadline){
            const auto callStarted=std::chrono::steady_clock::now();result=decoder->ReadNextAvailable(frame);
            CHECK(std::chrono::steady_clock::now()-callStarted<std::chrono::milliseconds{40});Sleep(5);
        }
        CHECK_EQ(VideoReadResult::FrameReady,result);CHECK_EQ(size_t{16},frame.bgra.size());
        const auto closeStarted=std::chrono::steady_clock::now();decoder->Close();CHECK(std::chrono::steady_clock::now()-closeStarted<std::chrono::seconds{1});
    }
    {
        auto decoder=VideoDecoderTestAccess::Create(fixture.directory,std::chrono::milliseconds{120},std::chrono::milliseconds{80});
        const auto started=std::chrono::steady_clock::now();CHECK(!decoder->Open(L"https://media.invalid/holdprobe",MediaSourceKind::YouTube));
        CHECK(std::chrono::steady_clock::now()-started<std::chrono::seconds{1});
    }
}

void youtube_decoder_partial_stall_cancel_and_exit_leave_no_children_test()
{
    MediaFixture fixture;const size_t beforeFfmpeg=count_named_processes(L"ffmpeg.exe");const size_t beforeProbe=count_named_processes(L"ffprobe.exe");
    DWORD beforeHandles=0,afterHandles=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&beforeHandles)!=FALSE);
    for(int cycle=0;cycle<4;++cycle){
        auto decoder=VideoDecoderTestAccess::Create(fixture.directory,std::chrono::milliseconds{250},std::chrono::milliseconds{75});
        CHECK(decoder->Open(L"https://media.invalid/stallmid",MediaSourceKind::YouTube));VideoFrame frame;VideoReadResult result=VideoReadResult::NotReady;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{1};
        while(result==VideoReadResult::NotReady&&std::chrono::steady_clock::now()<deadline){const auto call=std::chrono::steady_clock::now();result=decoder->ReadNextAvailable(frame);CHECK(std::chrono::steady_clock::now()-call<std::chrono::milliseconds{40});Sleep(5);}
        CHECK_EQ(VideoReadResult::Stalled,result);decoder->Close();
    }
    {
        auto decoder=VideoDecoderTestAccess::Create(fixture.directory);CHECK(decoder->Open(L"https://media.invalid/hold",MediaSourceKind::YouTube));
        const auto closeStarted=std::chrono::steady_clock::now();decoder->Close();CHECK(std::chrono::steady_clock::now()-closeStarted<std::chrono::seconds{1});
    }
    {
        auto decoder=VideoDecoderTestAccess::Create(fixture.directory);CHECK(decoder->Open(L"https://media.invalid/exit",MediaSourceKind::YouTube));VideoFrame frame;
        VideoReadResult result=VideoReadResult::NotReady;for(int i=0;i<50&&result==VideoReadResult::NotReady;++i){result=decoder->ReadNextAvailable(frame);Sleep(5);}CHECK(result==VideoReadResult::EndOfStream||result==VideoReadResult::Error);
    }
    Sleep(50);CHECK(wait_for_named_process_count(L"ffmpeg.exe",beforeFfmpeg,std::chrono::milliseconds{500}));CHECK(wait_for_named_process_count(L"ffprobe.exe",beforeProbe,std::chrono::milliseconds{500}));
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&afterHandles)!=FALSE);CHECK(afterHandles<=beforeHandles+2);
}

void youtube_decoder_discards_only_expected_trailing_partial_frame_test()
{
    MediaFixture fixture;
    auto decoder=VideoDecoderTestAccess::Create(fixture.directory);
    CHECK(decoder->Open(L"https://media.invalid/partialend",MediaSourceKind::YouTube));
    VideoFrame frame;VideoReadResult result=VideoReadResult::NotReady;size_t frames=0;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{1};
    while(std::chrono::steady_clock::now()<deadline){
        result=decoder->ReadNextAvailable(frame);
        if(result==VideoReadResult::FrameReady){++frames;result=VideoReadResult::NotReady;}
        else if(result!=VideoReadResult::NotReady)break;
        Sleep(5);
    }
    CHECK_EQ(size_t{2},frames);
    CHECK_EQ(VideoReadResult::EndOfStream,result);
}

void youtube_decoder_background_seek_trickles_and_cancels_boundedly_test()
{
    MediaFixture fixture;
    {
        auto decoder=VideoDecoderTestAccess::Create(fixture.directory);CHECK(decoder->Open(L"https://media.invalid/trickle",MediaSourceKind::YouTube));CHECK(decoder->SeekSeconds(12.0));
        VideoFrame frame;VideoReadResult result=VideoReadResult::NotReady;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{1};
        while(result==VideoReadResult::NotReady&&std::chrono::steady_clock::now()<deadline){const auto call=std::chrono::steady_clock::now();result=decoder->ReadNextAvailable(frame);CHECK(std::chrono::steady_clock::now()-call<std::chrono::milliseconds{40});Sleep(5);}
        CHECK_EQ(VideoReadResult::FrameReady,result);CHECK(frame.timestamp100ns>=120000000);
    }
    {
        auto decoder=VideoDecoderTestAccess::Create(fixture.directory,std::chrono::milliseconds{250},std::chrono::seconds{5});CHECK(decoder->Open(L"https://media.invalid/hold",MediaSourceKind::YouTube));CHECK(decoder->SeekSeconds(9.0));
        const auto started=std::chrono::steady_clock::now();std::jthread worker([&](std::stop_token stop){VideoFrame frame;while(decoder->ReadNextAvailable(frame,stop)==VideoReadResult::NotReady)Sleep(5);});Sleep(30);worker.request_stop();worker.join();decoder->Close();CHECK(std::chrono::steady_clock::now()-started<std::chrono::seconds{1});
    }
}

void video_decoder_hardware_failure_falls_back_to_software_test()
{
    MediaFixture fixture;
    const auto marker=fixture.directory/L"acceleration-order.txt";
    ScopedEnvironmentVariable markerVariable(L"DLSS_VIDEO_TEST_ACCEL_MARKER",marker.wstring());
    auto decoder=VideoDecoderTestAccess::Create(fixture.directory);
    CHECK(decoder->Open(L"https://media.invalid/hardwarefallback",MediaSourceKind::YouTube));
    VideoFrame frame;VideoReadResult result=VideoReadResult::NotReady;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{1};
    while(result==VideoReadResult::NotReady&&std::chrono::steady_clock::now()<deadline){result=decoder->ReadNextAvailable(frame);Sleep(5);}
    CHECK_EQ(VideoReadResult::FrameReady,result);CHECK_EQ(size_t{16},frame.bgra.size());
    CHECK_EQ(std::string("cuda\nd3d11va\nsoftware\n"),read_binary_file(marker));
}

void video_decoder_background_queue_is_bounded_to_four_frames_test()
{
    MediaFixture fixture;
    const auto marker=fixture.directory/L"large-frame-progress.bin";
    ScopedEnvironmentVariable markerVariable(L"DLSS_VIDEO_TEST_FRAME_MARKER",marker.wstring());
    auto decoder=VideoDecoderTestAccess::Create(fixture.directory);
    CHECK(decoder->Open(L"https://media.invalid/largeburst",MediaSourceKind::YouTube));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds{750};
    uintmax_t produced=0;
    while(std::chrono::steady_clock::now()<deadline){
        std::error_code error;produced=std::filesystem::file_size(marker,error);if(!error&&produced>=4)break;Sleep(10);
    }
    Sleep(75);
    std::error_code error;produced=std::filesystem::file_size(marker,error);if(error)produced=0;
    CHECK(produced>=4);CHECK(produced<=6);
}

void video_decoder_resume_failures_are_bounded_and_leak_free_for_local_and_network_startup_test()
{
    struct Case {
        VideoDecoder::FailureStage stage;
        MediaSourceKind sourceKind;
        std::wstring_view path;
    };
    const std::array cases{
        Case{VideoDecoder::FailureStage::ProbeResume,MediaSourceKind::LocalFile,
             LR"(C:\missing\local-probe-resume.mp4)"},
        Case{VideoDecoder::FailureStage::ProbeResume,MediaSourceKind::YouTube,
             L"https://media.invalid/network-probe-resume"},
        Case{VideoDecoder::FailureStage::DecodeResume,MediaSourceKind::LocalFile,
             LR"(C:\missing\local-decode-resume.mp4)"},
        Case{VideoDecoder::FailureStage::DecodeResume,MediaSourceKind::YouTube,
             L"https://media.invalid/network-decode-resume"},
    };
    MediaFixture fixture;
    const size_t beforeProbe=count_named_processes(L"ffprobe.exe");
    const size_t beforeFfmpeg=count_named_processes(L"ffmpeg.exe");
    // The first failed local open initializes process-wide Media Foundation
    // state before its fallback rejects the missing file. Warm that one-time
    // state before measuring per-attempt handle ownership.
    {
        auto warmup=VideoDecoderTestAccess::Create(
            fixture.directory,std::chrono::milliseconds{250},
            std::chrono::milliseconds{120},VideoDecoder::FailureStage::ProbeResume);
        const auto started=std::chrono::steady_clock::now();
        CHECK(!warmup->Open(LR"(C:\missing\resume-warmup.mp4)",MediaSourceKind::LocalFile));
        CHECK(std::chrono::steady_clock::now()-started<std::chrono::seconds{1});
    }
    CHECK(wait_for_named_process_count(L"ffprobe.exe",beforeProbe,
                                       std::chrono::milliseconds{500}));
    CHECK(wait_for_named_process_count(L"ffmpeg.exe",beforeFfmpeg,
                                       std::chrono::milliseconds{500}));
    DWORD beforeHandles=0,afterHandles=0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&beforeHandles)!=FALSE);

    for(int repetition=0;repetition<4;++repetition){
        for(const Case& test:cases){
            auto decoder=VideoDecoderTestAccess::Create(
                fixture.directory,std::chrono::milliseconds{250},
                std::chrono::milliseconds{120},test.stage);
            const auto started=std::chrono::steady_clock::now();
            CHECK(!decoder->Open(std::wstring(test.path),test.sourceKind));
            CHECK(std::chrono::steady_clock::now()-started<std::chrono::seconds{1});
            decoder.reset();
            CHECK(wait_for_named_process_count(L"ffprobe.exe",beforeProbe,
                                               std::chrono::milliseconds{500}));
            CHECK(wait_for_named_process_count(L"ffmpeg.exe",beforeFfmpeg,
                                               std::chrono::milliseconds{500}));
        }
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&afterHandles)!=FALSE);
    CHECK(afterHandles<=beforeHandles+2);
}

void youtube_audio_held_pipe_stop_destroy_and_failure_fallback_are_bounded_test()
{
    MediaFixture fixture;const size_t beforeProcesses=count_named_processes(L"ffmpeg.exe");DWORD beforeHandles=0,afterHandles=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&beforeHandles)!=FALSE);
    for(int cycle=0;cycle<8;++cycle){
        auto audio=AudioPlayerTestAccess::Create(fixture.directory,cycle%2==0,cycle%3==0);
        CHECK(audio->Start(L"https://media.invalid/audiohold",7.5,AudioStartState::Paused));CHECK(audio->Paused());CHECK_EQ(7.5,AudioPlayerTestAccess::SeekBase(*audio));CHECK_EQ(uint64_t{0},AudioPlayerTestAccess::SubmittedBuffers(*audio));
        const auto started=std::chrono::steady_clock::now();if(cycle%2==0)audio->Stop();else audio.reset();CHECK(std::chrono::steady_clock::now()-started<std::chrono::seconds{1});
        CHECK(wait_for_named_process_count(L"ffmpeg.exe",beforeProcesses,std::chrono::milliseconds{500}));
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&afterHandles)!=FALSE);CHECK(afterHandles<=beforeHandles+2);
}

void youtube_audio_failed_waits_and_query_retire_reader_without_termination_or_leaks_test()
{
    MediaFixture fixture;
    const size_t beforeProcesses=count_named_processes(L"ffmpeg.exe");
    DWORD beforeHandles=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&beforeHandles)!=FALSE);
    for(int cycle=0;cycle<12;++cycle){
        auto audio=AudioPlayerTestAccess::Create(fixture.directory,true,true,true,true,true,true);
        CHECK(audio->Start(L"https://media.invalid/audiohold",3.0,AudioStartState::Paused));
        const auto started=std::chrono::steady_clock::now();
        if(cycle%3==0){
            CHECK(audio->Start(L"https://media.invalid/audiohold",4.0,AudioStartState::Paused));
            audio->Stop();
        }else if(cycle%3==1){audio->Stop();}else{audio.reset();}
        CHECK(std::chrono::steady_clock::now()-started<std::chrono::seconds{1});
        CHECK(wait_for_named_process_count(L"ffmpeg.exe",beforeProcesses,std::chrono::milliseconds{500}));
    }
    const auto handlesDeadline=std::chrono::steady_clock::now()+std::chrono::seconds{1};
    DWORD afterHandles=0;
    do{
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&afterHandles)!=FALSE);
        if(afterHandles<=beforeHandles+2)break;
        Sleep(5);
    }while(std::chrono::steady_clock::now()<handlesDeadline);
    CHECK(afterHandles<=beforeHandles+2);
}

void youtube_prepared_audio_starts_silent_and_handoff_has_no_overlap_test()
{
    MediaFixture fixture;auto prepared=AudioPlayerTestAccess::Create(fixture.directory);
    CHECK(prepared->Start(L"https://media.invalid/audiotrickle",12.25,AudioStartState::Paused));CHECK(prepared->Paused());CHECK_EQ(12.25,AudioPlayerTestAccess::SeekBase(*prepared));Sleep(60);CHECK_EQ(uint64_t{0},AudioPlayerTestAccess::SubmittedBuffers(*prepared));
    bool oldAudible=true,newAudible=false;std::vector<int> order;
    CommitPreparedAudioHandoff(
        [&]{CHECK(oldAudible);CHECK(!newAudible);CHECK(prepared->Paused());order.push_back(1);},
        [&]{CHECK(oldAudible);CHECK(!newAudible);CHECK(prepared->Paused());order.push_back(2);return true;},
        [&]{CHECK(oldAudible);CHECK(!newAudible);CHECK(prepared->Paused());oldAudible=false;order.push_back(3);},
        [&]{CHECK(!oldAudible);prepared->Pause(false);newAudible=true;order.push_back(4);});
    CHECK_EQ(std::vector<int>({2,1,3,4}),order);CHECK(!prepared->Paused());
    prepared->Stop();

    auto cancelled=AudioPlayerTestAccess::Create(fixture.directory);CHECK(cancelled->Start(L"https://media.invalid/audiohold",4.0,AudioStartState::Paused));CHECK_EQ(uint64_t{0},AudioPlayerTestAccess::SubmittedBuffers(*cancelled));cancelled.reset();
}

void youtube_prepared_handoff_shows_candidate_and_retires_every_old_owner_before_activation_test()
{
    enum Event {
        ShowCandidate=1,
        InstallCandidate,
        RetireOldAudio,
        RetireOldDecoder,
        RetireOldRenderer,
        RetireOldWindow,
        EstablishClocks,
        ActivatePreparedAudio,
    };
    std::vector<int> order;
    std::array<int,4> retireCounts{};
    const std::array<int,4> expectedRetireCounts{1,1,1,1};
    bool candidateInstalled=false,candidateVisible=false,preparedAudioPaused=true;

    CommitPreparedAudioHandoff(
        [&]{
            CHECK(preparedAudioPaused);
            CHECK(candidateVisible);
            candidateInstalled=true;
            order.push_back(InstallCandidate);
        },
        [&]{
            CHECK(!candidateInstalled);
            CHECK(preparedAudioPaused);
            candidateVisible=true;
            order.push_back(ShowCandidate);
            return true;
        },
        [&]{
            CHECK(candidateVisible);
            CHECK(preparedAudioPaused);
            ++retireCounts[0];order.push_back(RetireOldAudio);
            ++retireCounts[1];order.push_back(RetireOldDecoder);
            ++retireCounts[2];order.push_back(RetireOldRenderer);
            ++retireCounts[3];order.push_back(RetireOldWindow);
        },
        [&]{
            CHECK(candidateVisible);
            CHECK_EQ(expectedRetireCounts,retireCounts);
            order.push_back(EstablishClocks);
            preparedAudioPaused=false;
            order.push_back(ActivatePreparedAudio);
        });

    CHECK_EQ(std::vector<int>({ShowCandidate,InstallCandidate,RetireOldAudio,
              RetireOldDecoder,RetireOldRenderer,RetireOldWindow,
              EstablishClocks,ActivatePreparedAudio}),order);
    CHECK(!preparedAudioPaused);
    CHECK_EQ(expectedRetireCounts,retireCounts);
}

void youtube_prepared_handoff_sizes_and_shows_real_candidate_before_owned_retirement_test()
{
    enum Event {Visible=1,Install,RetireAudio,RetireDecoder,RetireRenderer,RetireWindow,Activate};
    struct OwnedSentinel {
        std::vector<int>* order{};
        int event{};
        int* destroyCount{};
        HWND* candidateWindow{};
        int* hiddenAtDestruction{};
        ~OwnedSentinel(){
            if(!destroyCount)return;
            if(!candidateWindow||!IsWindowVisible(*candidateWindow))++*hiddenAtDestruction;
            ++*destroyCount;
            order->push_back(event);
        }
    };

    HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"handoff-host",
        WS_POPUP|WS_VISIBLE,-32000,-32000,640,360,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    HWND viewport=CreateWindowExW(0,L"STATIC",L"handoff-viewport",
        WS_CHILD|WS_VISIBLE,0,0,640,360,host,nullptr,GetModuleHandleW(nullptr),nullptr);
    HWND activeWindow=CreateWindowExW(0,L"STATIC",L"old-render-window",
        WS_CHILD|WS_VISIBLE,0,0,640,360,viewport,nullptr,GetModuleHandleW(nullptr),nullptr);
    HWND preparedWindow=CreateWindowExW(0,L"STATIC",L"prepared-render-window",
        WS_CHILD,0,0,100,100,viewport,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(host!=nullptr);CHECK(viewport!=nullptr);CHECK(activeWindow!=nullptr);CHECK(preparedWindow!=nullptr);
    CHECK(!IsWindowVisible(preparedWindow));

    std::vector<int> order;
    std::array<int,3> destroyCounts{};
    const std::array<int,3> expectedDestroyCounts{1,1,1};
    int hiddenAtDestruction=0;
    auto owner=[&](int index,int event){
        auto value=std::make_unique<OwnedSentinel>();
        value->order=&order;value->event=event;
        value->destroyCount=&destroyCounts[static_cast<size_t>(index)];
        value->candidateWindow=&activeWindow;value->hiddenAtDestruction=&hiddenAtDestruction;
        return value;
    };
    auto preparedOwner=[] {return std::make_unique<OwnedSentinel>();};
    auto activeAudio=owner(0,RetireAudio),activeDecoder=owner(1,RetireDecoder),activeRenderer=owner(2,RetireRenderer);
    auto preparedAudio=preparedOwner(),preparedDecoder=preparedOwner(),preparedRenderer=preparedOwner();
    std::unique_ptr<OwnedSentinel> retiringAudio,retiringDecoder,retiringRenderer;
    HWND retiringWindow=nullptr;

    CommitPreparedAudioHandoff(
        [&]{
            retiringAudio=std::move(activeAudio);activeAudio=std::move(preparedAudio);
            retiringDecoder=std::move(activeDecoder);activeDecoder=std::move(preparedDecoder);
            retiringRenderer=std::move(activeRenderer);activeRenderer=std::move(preparedRenderer);
            retiringWindow=activeWindow;activeWindow=preparedWindow;preparedWindow=nullptr;
            order.push_back(Install);
        },
        [&]{
            const bool shown=ShowPreparedRenderWindow(viewport,preparedWindow);
            CHECK(shown);
            RECT candidateBounds{};CHECK(GetWindowRect(preparedWindow,&candidateBounds)!=FALSE);
            CHECK_EQ(LONG{640},candidateBounds.right-candidateBounds.left);
            CHECK_EQ(LONG{360},candidateBounds.bottom-candidateBounds.top);
            CHECK(IsWindowVisible(preparedWindow));
            CHECK_EQ(preparedWindow,GetWindow(viewport,GW_CHILD));
            order.push_back(Visible);
            return shown;
        },
        [&]{
            retiringAudio.reset();retiringDecoder.reset();retiringRenderer.reset();
            CHECK(DestroyWindow(retiringWindow)!=FALSE);retiringWindow=nullptr;order.push_back(RetireWindow);
        },
        [&]{
            CHECK_EQ(expectedDestroyCounts,destroyCounts);
            CHECK_EQ(0,hiddenAtDestruction);
            CHECK(IsWindowVisible(activeWindow));
            order.push_back(Activate);
        });

    const std::vector<int> expected{Visible,Install,RetireAudio,RetireDecoder,
                                    RetireRenderer,RetireWindow,Activate};
    CHECK_EQ(expected,order);
    if(host)DestroyWindow(host);
}

void youtube_prepared_window_api_failures_are_reported_before_commit_test()
{
    const HWND fakeViewport=reinterpret_cast<HWND>(uintptr_t{1});
    const HWND fakeWindow=reinterpret_cast<HWND>(uintptr_t{2});
    int getClientRectCalls=0,setWindowPosCalls=0;
    CHECK(!ShowPreparedRenderWindowWithOperations(
        fakeViewport,fakeWindow,
        [](HWND){return TRUE;},
        [&](HWND,RECT*){++getClientRectCalls;return FALSE;},
        [&](HWND,HWND,int,int,int,int,UINT){++setWindowPosCalls;return TRUE;},
        [](HWND){return TRUE;}));
    CHECK_EQ(1,getClientRectCalls);CHECK_EQ(0,setWindowPosCalls);

    getClientRectCalls=0;setWindowPosCalls=0;
    CHECK(!ShowPreparedRenderWindowWithOperations(
        fakeViewport,fakeWindow,
        [](HWND){return TRUE;},
        [&](HWND,RECT* bounds){++getClientRectCalls;*bounds=RECT{0,0,640,360};return TRUE;},
        [&](HWND,HWND,int,int,int,int,UINT){++setWindowPosCalls;return FALSE;},
        [](HWND){return TRUE;}));
    CHECK_EQ(1,getClientRectCalls);CHECK_EQ(1,setWindowPosCalls);

    CHECK(!ShowPreparedRenderWindowWithOperations(
        fakeViewport,fakeWindow,
        [](HWND){return TRUE;},
        [](HWND,RECT* bounds){*bounds=RECT{0,0,640,360};return TRUE;},
        [](HWND,HWND,int,int,int,int,UINT){return TRUE;},
        [](HWND){return FALSE;}));
}

void youtube_destroyed_window_and_visibility_failure_leave_active_state_unchanged_test()
{
    HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"visibility-host",
        WS_POPUP|WS_VISIBLE,-32000,-32000,640,360,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    HWND viewport=CreateWindowExW(0,L"STATIC",L"visibility-viewport",
        WS_CHILD|WS_VISIBLE,0,0,640,360,host,nullptr,GetModuleHandleW(nullptr),nullptr);
    HWND destroyed=CreateWindowExW(0,L"STATIC",L"destroyed-candidate",
        WS_CHILD,0,0,100,100,viewport,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(host!=nullptr);CHECK(viewport!=nullptr);CHECK(destroyed!=nullptr);
    CHECK(DestroyWindow(destroyed)!=FALSE);
    CHECK(!ShowPreparedRenderWindow(viewport,destroyed));
    CHECK(!ShowPreparedRenderWindow(nullptr,destroyed));

    struct Candidate{
        explicit Candidate(bool& destroyed):destroyed(destroyed),renderer(MakeD3D12Renderer()){}
        ~Candidate(){destroyed=true;}
        bool& destroyed;
        D3D12RendererOwner renderer;
    };
    struct Active{int decoder{10};int audio{20};int renderer{30};bool playing{true};bool operator==(const Active&)const=default;};
    Active active;const Active before=active;bool candidateDestroyed=false,preparedAudioPaused=true;
    int candidateWaits=0;auto candidateResourcesDestroyed=std::make_shared<int>(0);
    std::vector<int> order;
    const bool committed=ExecuteNetworkCandidateTransaction<Candidate>(
        [&]{
            auto candidate=std::make_unique<Candidate>(candidateDestroyed);
            D3D12RendererTestAccess::ConfigureWait(
                *candidate->renderer,d3d12_renderer_detail::FenceWaitResult::TimedOut,candidateWaits);
            D3D12RendererTestAccess::OwnSentinel(
                *candidate->renderer,std::make_unique<RendererOwnedSentinel>(candidateResourcesDestroyed));
            return candidate;
        },
        [](Candidate&){return true;},
        [&](std::unique_ptr<Candidate>){
            return CommitPreparedAudioHandoff(
                [&]{order.push_back(2);active={11,21,31,false};},
                [&]{order.push_back(1);return ShowPreparedRenderWindow(viewport,destroyed);},
                [&]{order.push_back(3);},
                [&]{order.push_back(4);preparedAudioPaused=false;});
        });
    CHECK(!committed);CHECK_EQ(std::vector<int>({1}),order);
    CHECK_EQ(before,active);CHECK(preparedAudioPaused);CHECK(candidateDestroyed);
    CHECK_EQ(1,candidateWaits);CHECK_EQ(0,*candidateResourcesDestroyed);
    if(host)DestroyWindow(host);
}

void youtube_candidate_render_failure_releases_window_handle_and_prepared_processes_test()
{
    struct ActivePlayback {
        int decoder{10};
        int audio{20};
        int renderer{30};
        int quality{2};
        bool playing{true};
        int64_t history{170000000};
        bool operator==(const ActivePlayback&) const=default;
    };
    struct Candidate {
        Candidate(HWND window,HANDLE handle,bool renderFirst,bool* destroyed)
            : window(window),handle(handle),renderFirst(renderFirst),destroyed(destroyed) {}
        Candidate(const Candidate&)=delete;
        Candidate& operator=(const Candidate&)=delete;
        HWND window{};
        HANDLE handle{};
        bool renderFirst{};
        bool* destroyed{};
        ~Candidate(){
            if(handle)CloseHandle(handle);
            if(window)DestroyWindow(window);
            if(destroyed)*destroyed=true;
        }
    };

    MediaFixture fixture;
    const size_t beforeProcesses=count_named_processes(L"ffmpeg.exe");
    DWORD beforeHandles=0,afterHandles=0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&beforeHandles)!=FALSE);
    auto preparedDecoder=VideoDecoderTestAccess::Create(fixture.directory);
    auto preparedAudio=AudioPlayerTestAccess::Create(fixture.directory);
    CHECK(preparedDecoder->Open(L"https://media.invalid/hold",MediaSourceKind::YouTube));
    CHECK(preparedAudio->Start(L"https://media.invalid/audiohold",9.0,AudioStartState::Paused));
    CHECK(wait_for_named_process_count(L"ffmpeg.exe",beforeProcesses+2,
                                       std::chrono::milliseconds{500}));

    ActivePlayback active;
    const ActivePlayback before=active;
    HWND candidateWindow=nullptr;
    bool candidateDestroyed=false;
    bool commitCalled=false;
    const bool committed=ExecuteNetworkCandidateTransaction<Candidate>(
        [&]{
            candidateWindow=CreateWindowExW(0,L"STATIC",L"candidate",0,0,0,1,1,
                                             HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
            HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            CHECK(candidateWindow!=nullptr);CHECK(event!=nullptr);
            return std::make_unique<Candidate>(candidateWindow,event,false,&candidateDestroyed);
        },
        [&](Candidate& candidate){return candidate.renderFirst;},
        [&](std::unique_ptr<Candidate>){
            commitCalled=true;active={11,21,31,3,false,90000000};
            preparedAudio.reset();preparedDecoder.reset();
        });
    CHECK(!committed);
    CHECK(!commitCalled);
    CHECK_EQ(before,active);
    CHECK(candidateDestroyed);
    CHECK(candidateWindow!=nullptr);
    CHECK(!IsWindow(candidateWindow));

    preparedAudio.reset();
    preparedDecoder.reset();
    CHECK(wait_for_named_process_count(L"ffmpeg.exe",beforeProcesses,
                                       std::chrono::milliseconds{500}));
    const auto handlesDeadline=std::chrono::steady_clock::now()+std::chrono::seconds{1};
    do{
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&afterHandles)!=FALSE);
        if(afterHandles<=beforeHandles+2)break;
        Sleep(5);
    }while(std::chrono::steady_clock::now()<handlesDeadline);
    CHECK(afterHandles<=beforeHandles+2);
}

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
    CHECK_EQ(std::wstring(L"https://r1.googlevideo.com/videoplayback?id=success-audio"),
             result.audioUrl);
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

int run_fake_media_child(int argc,wchar_t* argv[])
{
    const std::wstring name=current_test_executable().filename().wstring();std::wstring all;
    for(int index=1;index<argc;++index){all+=L" ";all+=argv[index];}
    if(_wcsicmp(name.c_str(),L"ffprobe.exe")==0){
        if(all.find(L"holdprobe")!=std::wstring::npos){Sleep(INFINITE);return 0;}
        if(all.find(L"largeburst")!=std::wstring::npos){std::cout<<"width=1024\nheight=1024\ndisplay_aspect_ratio=1:1\nsample_aspect_ratio=1:1\navg_frame_rate=30/1\nr_frame_rate=30/1\nduration=30\n"<<std::flush;return 0;}
        if(all.find(L"partialend")!=std::wstring::npos){std::cout<<"width=2\nheight=2\ndisplay_aspect_ratio=1:1\nsample_aspect_ratio=1:1\navg_frame_rate=30/1\nr_frame_rate=30/1\nduration=0.067\n"<<std::flush;return 0;}
        std::cout<<"width=2\nheight=2\ndisplay_aspect_ratio=1:1\nsample_aspect_ratio=1:1\navg_frame_rate=30/1\nr_frame_rate=30/1\nduration=30\n"<<std::flush;return 0;
    }
    if(_wcsicmp(name.c_str(),L"ffmpeg.exe")!=0)return 94;
    if(all.find(L"hardwarefallback")!=std::wstring::npos){
        const std::wstring marker=read_environment_variable(L"DLSS_VIDEO_TEST_ACCEL_MARKER");
        const bool cuda=all.find(L"-hwaccel cuda")!=std::wstring::npos;
        const bool d3d11=all.find(L"-hwaccel d3d11va")!=std::wstring::npos;
        if(!marker.empty()){std::ofstream out(marker,std::ios::binary|std::ios::app);out<<(cuda?"cuda\n":d3d11?"d3d11va\n":"software\n");}
        if(cuda||d3d11)return 7;
        std::cout.write("1234567890abcdef",16);std::cout.flush();return 0;
    }
    if(all.find(L"largeburst")!=std::wstring::npos){
        const std::wstring marker=read_environment_variable(L"DLSS_VIDEO_TEST_FRAME_MARKER");
        const std::vector<char> frame(4u*1024u*1024u,'x');
        for(int index=0;index<20;++index){
            std::cout.write(frame.data(),static_cast<std::streamsize>(frame.size()));std::cout.flush();
            if(!marker.empty()){std::ofstream out(marker,std::ios::binary|std::ios::app);out.put('x');}
        }
        Sleep(INFINITE);return 0;
    }
    if(all.find(L"exit")!=std::wstring::npos)return 7;
    if(all.find(L"partialend")!=std::wstring::npos){std::cout.write("1234567890abcdef1234567890abcdef12345678",40);std::cout.flush();return 0;}
    if(all.find(L"stallmid")!=std::wstring::npos){std::cout.write("1234",4);std::cout.flush();Sleep(INFINITE);return 0;}
    if(all.find(L"trickle")!=std::wstring::npos){std::cout.write("12345678",8);std::cout.flush();Sleep(35);std::cout.write("abcdefgh",8);std::cout.flush();return 0;}
    Sleep(INFINITE);return 0;
}

int run_fake_resolver_child(int argc, wchar_t* argv[])
{
    if (argc == 3 && std::wstring_view(argv[1]) == L"--resolver-descendant") {
        write_binary_file(argv[2], std::to_string(GetCurrentProcessId()));
        Sleep(INFINITE);
        return 0;
    }
    const std::wstring_view requestedUrl = argc > 1 ? std::wstring_view(argv[argc - 1]) : L"";
    const bool noCacheDir = argc > 2 && std::wstring_view(argv[2]) == L"--no-cache-dir";
    const bool noPluginDirs = argc > 3 && std::wstring_view(argv[3]) == L"--no-plugin-dirs";
    if (requestedUrl.find(L"ytcacheaudit") != std::wstring_view::npos && !noCacheDir) {
        const std::wstring xdgCache = read_environment_variable(L"XDG_CACHE_HOME");
        if (!xdgCache.empty()) {
            write_binary_file(std::filesystem::path(xdgCache) / L"yt-dlp-default.marker",
                              "default-cache-write");
        }
    }
    if (requestedUrl.find(L"pluginaudit") != std::wstring_view::npos && !noPluginDirs) {
        const std::wstring xdgConfig = read_environment_variable(L"XDG_CONFIG_HOME");
        if (!xdgConfig.empty()) {
            const std::filesystem::path pluginDirectory =
                std::filesystem::path(xdgConfig) / L"yt-dlp" / L"plugins";
            const std::filesystem::path pluginSource = pluginDirectory / L"exec-on-import.plugin";
            std::error_code pluginError;
            if (std::filesystem::is_regular_file(pluginSource, pluginError) && !pluginError) {
                write_binary_file(pluginDirectory / L"default-plugin-executed.marker",
                                  "default-plugin-executed");
            }
        }
    }
    if (argc != 12 || std::wstring_view(argv[1]) != L"--no-config" ||
        std::wstring_view(argv[2]) != L"--no-cache-dir" ||
        std::wstring_view(argv[3]) != L"--no-plugin-dirs" ||
        std::wstring_view(argv[4]) != L"--no-playlist" ||
        std::wstring_view(argv[5]) != L"--no-warnings" ||
        std::wstring_view(argv[6]) != L"--js-runtimes" ||
        !std::wstring_view(argv[7]).starts_with(L"deno:") ||
        std::wstring_view(argv[8]) != L"-f" ||
        std::wstring_view(argv[9]) != YouTubeFormatSelector(YouTubeSourceQuality::Auto) ||
        std::wstring_view(argv[10]) != L"--get-url") {
        return 91;
    }
    const std::filesystem::path expectedDeno =
        current_test_executable().parent_path() / L"deno.exe";
    std::error_code equivalentError;
    if (!std::filesystem::equivalent(
            std::filesystem::path(std::wstring(std::wstring_view(argv[7]).substr(5))),
            expectedDeno, equivalentError) || equivalentError) {
        return 92;
    }

    const std::wstring_view url = argv[11];
    if (url.find(L"envcapture") != std::wstring_view::npos) {
        const std::filesystem::path expectedCache =
            current_test_executable().parent_path() / L"youtube-helper-cache";
        const std::wstring received = read_environment_variable(L"DENO_DIR");
        std::error_code cacheEquivalentError;
        if (received.empty() ||
            !std::filesystem::equivalent(std::filesystem::path(received), expectedCache,
                                         cacheEquivalentError) || cacheEquivalentError) {
            return 95;
        }
        write_binary_file(expectedCache / L"resolver-envcapture.marker", "package-local");
        std::cout << "https://r1.googlevideo.com/videoplayback?id=envcapture\n" << std::flush;
        return 0;
    }
    const size_t symlinkAttack = url.find(L"symlinkattack_");
    if (symlinkAttack != std::wstring_view::npos) {
        const std::wstring suffix(url.substr(symlinkAttack + 14));
        const std::filesystem::path marker = std::filesystem::temp_directory_path() /
            (L"PolicyTests-resolver-symlink-executed-" + suffix + L".marker");
        write_binary_file(marker, "executed");
        std::cout << "https://r1.googlevideo.com/videoplayback?id=symlink\n" << std::flush;
        return 0;
    }
    if (url.find(L"success") != std::wstring_view::npos) {
        std::cout << "https://r1.googlevideo.com/videoplayback?id=success\n"
                     "https://r1.googlevideo.com/videoplayback?id=success-audio\n" << std::flush;
        return 0;
    }
    if (url.find(L"uismoke") != std::wstring_view::npos) {
        write_binary_file(current_test_executable().parent_path()/L"ui-helper-launch.marker","launched");
        std::cout << "https://r1.googlevideo.com/videoplayback?id=uismoke\n" << std::flush;
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

size_t count_named_processes(std::wstring_view executableName)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    CHECK(snapshot != INVALID_HANDLE_VALUE);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    size_t count = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (CompareStringOrdinal(entry.szExeFile, -1, executableName.data(),
                                     static_cast<int>(executableName.size()), TRUE) == CSTR_EQUAL) {
                ++count;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CHECK(CloseHandle(snapshot) != FALSE);
    return count;
}

bool wait_for_named_process_count(std::wstring_view executableName, size_t expected,
                                  std::chrono::milliseconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    do {
        if (count_named_processes(executableName) == expected) return true;
        Sleep(10);
    } while (std::chrono::steady_clock::now() < deadline);
    return count_named_processes(executableName) == expected;
}

void youtube_resolver_rejects_symlink_and_nonregular_helpers_before_execution_test()
{
    ResolverFixture fixture;
    const std::filesystem::path outsideDirectory = fixture.directory.parent_path() /
        (L"PolicyTests-YouTubeResolver-outside-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path outsideHelper = outsideDirectory / L"outside-helper.exe";
    const std::filesystem::path executionMarker = std::filesystem::temp_directory_path() /
        (L"PolicyTests-resolver-symlink-executed-" +
         std::to_wstring(GetCurrentProcessId()) + L".marker");
    std::error_code error;
    std::filesystem::remove_all(outsideDirectory, error);
    error.clear();
    std::filesystem::create_directories(outsideDirectory, error);
    CHECK(!error);
    CHECK(CopyFileW(current_test_executable().c_str(), outsideHelper.c_str(), FALSE) != FALSE);
    remove_file_if_present(executionMarker);

    std::filesystem::remove(fixture.directory / L"yt-dlp.exe", error);
    CHECK(!error);
    const DWORD symlinkFlags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    const bool symlinkCreated = CreateSymbolicLinkW(
        (fixture.directory / L"yt-dlp.exe").c_str(), outsideHelper.c_str(),
        symlinkFlags) != FALSE;
    if (symlinkCreated) {
        resolverSymlinkCoverageExercised = true;
        auto resolver = YouTubeResolverTestAccess::Create(fixture.directory);
        const ResolveResult result = resolver->Resolve(
            L"https://youtu.be/symlinkattack_" + std::to_wstring(GetCurrentProcessId()), {});
        CHECK(!result.ok);
        CHECK_EQ(ResolveError::HelperMissing, result.error);
        CHECK(result.detail.find(L"symlinkattack") == std::wstring::npos);
        CHECK(!std::filesystem::exists(executionMarker));
    } else {
        const DWORD errorCode = GetLastError();
        CHECK(errorCode == ERROR_PRIVILEGE_NOT_HELD || errorCode == ERROR_INVALID_PARAMETER ||
              errorCode == ERROR_NOT_SUPPORTED);
    }

    std::filesystem::remove(fixture.directory / L"yt-dlp.exe", error);
    error.clear();
    std::filesystem::create_directory(fixture.directory / L"yt-dlp.exe", error);
    CHECK(!error);
    auto directoryResolver = YouTubeResolverTestAccess::Create(fixture.directory);
    CHECK_EQ(ResolveError::HelperMissing,
             directoryResolver->Resolve(L"https://youtu.be/success", {}).error);

    std::filesystem::remove_all(fixture.directory / L"yt-dlp.exe", error);
    CHECK(!error);
    CHECK(CopyFileW(current_test_executable().c_str(),
                    (fixture.directory / L"yt-dlp.exe").c_str(), FALSE) != FALSE);
    std::filesystem::remove(fixture.directory / L"deno.exe", error);
    CHECK(!error);
    error.clear();
    std::filesystem::create_directory(fixture.directory / L"deno.exe", error);
    CHECK(!error);
    auto denoDirectoryResolver = YouTubeResolverTestAccess::Create(fixture.directory);
    CHECK_EQ(ResolveError::HelperMissing,
             denoDirectoryResolver->Resolve(L"https://youtu.be/success", {}).error);

    std::filesystem::remove_all(fixture.directory / L"deno.exe", error);
    CHECK(!error);
    write_binary_file(fixture.directory / L"deno.exe", "test-only placeholder");
    const std::filesystem::path cacheLink = fixture.directory / L"youtube-helper-cache";
    const bool cacheLinkCreated = CreateSymbolicLinkW(
        cacheLink.c_str(), outsideDirectory.c_str(),
        SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
    if (cacheLinkCreated) {
        auto cacheLinkResolver = YouTubeResolverTestAccess::Create(fixture.directory);
        CHECK_EQ(ResolveError::HelperMissing,
                 cacheLinkResolver->Resolve(L"https://youtu.be/success", {}).error);
        std::filesystem::remove(cacheLink, error);
        CHECK(!error);
    } else {
        const DWORD errorCode = GetLastError();
        CHECK(errorCode == ERROR_PRIVILEGE_NOT_HELD || errorCode == ERROR_INVALID_PARAMETER ||
              errorCode == ERROR_NOT_SUPPORTED);
    }

    std::filesystem::remove_all(outsideDirectory, error);
    CHECK(!error);
    remove_file_if_present(executionMarker);
}

void youtube_resolver_holds_verified_helpers_against_replacement_until_completion_test()
{
    ResolverFixture fixture;
    const size_t beforeProcesses = count_named_processes(L"yt-dlp.exe");
    auto resolver = YouTubeResolverTestAccess::Create(
        fixture.directory, std::chrono::seconds{5});
    ResolveResult result;
    std::thread worker([&] {
        result = resolver->Resolve(L"https://youtu.be/hang", {});
    });
    CHECK(wait_for_named_process_count(L"yt-dlp.exe", beforeProcesses + 1,
                                       std::chrono::milliseconds{500}));

    const std::filesystem::path replacement = fixture.directory / L"replacement-deno.exe";
    write_binary_file(replacement, "replacement");
    SetLastError(ERROR_SUCCESS);
    const BOOL replaced = MoveFileExW(replacement.c_str(),
                                      (fixture.directory / L"deno.exe").c_str(),
                                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    CHECK(replaced == FALSE);
    const DWORD replacementError = GetLastError();
    CHECK(replacementError == ERROR_SHARING_VIOLATION ||
          replacementError == ERROR_ACCESS_DENIED);

    resolver->Cancel();
    worker.join();
    CHECK_EQ(ResolveError::Cancelled, result.error);
    remove_file_if_present(replacement);
}

void youtube_resolver_forces_package_local_deno_cache_over_parent_override_test()
{
    ResolverFixture fixture;
    const std::filesystem::path callerCache = fixture.directory.parent_path() /
        (L"PolicyTests-caller-deno-cache-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(callerCache, error);
    CHECK(!error);
    const std::filesystem::path callerXdgCache = fixture.directory.parent_path() /
        (L"PolicyTests-caller-xdg-cache-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(callerXdgCache, error);
    CHECK(!error);
    const ScopedEnvironmentVariable inheritedOverride(L"DENO_DIR", callerCache.wstring());
    const ScopedEnvironmentVariable inheritedXdgCache(L"XDG_CACHE_HOME", callerXdgCache.wstring());

    const std::filesystem::path packageCache = fixture.directory / L"youtube-helper-cache";
    const std::filesystem::path packageMarker = packageCache / L"resolver-envcapture.marker";
    const std::filesystem::path callerMarker = callerCache / L"resolver-envcapture.marker";
    const std::filesystem::path callerXdgMarker = callerXdgCache / L"yt-dlp-default.marker";
    const size_t beforeProcesses = count_named_processes(L"yt-dlp.exe");
    auto resolver = YouTubeResolverTestAccess::Create(fixture.directory);
    const ResolveResult result = resolver->Resolve(L"https://youtu.be/envcapture-ytcacheaudit", {});

    CHECK(result.ok);
    CHECK(std::filesystem::is_directory(packageCache, error));
    CHECK(!error);
    CHECK_EQ(std::string("package-local"), read_binary_file(packageMarker));
    CHECK(!std::filesystem::exists(callerMarker, error));
    CHECK(!error);
    CHECK(!std::filesystem::exists(callerXdgMarker, error));
    CHECK(!error);
    CHECK(wait_for_named_process_count(L"yt-dlp.exe", beforeProcesses,
                                       std::chrono::milliseconds{500}));

    std::filesystem::remove_all(callerCache, error);
    CHECK(!error);
    std::filesystem::remove_all(callerXdgCache, error);
    CHECK(!error);
}

void youtube_resolver_disables_default_plugin_execution_from_inherited_config_test()
{
    ResolverFixture fixture;
    const std::filesystem::path callerConfig = fixture.directory.parent_path() /
        (L"PolicyTests-caller-xdg-config-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    const std::filesystem::path pluginDirectory = callerConfig / L"yt-dlp" / L"plugins";
    const std::filesystem::path pluginSource = pluginDirectory / L"exec-on-import.plugin";
    const std::filesystem::path executionMarker =
        pluginDirectory / L"default-plugin-executed.marker";
    std::error_code error;
    std::filesystem::create_directories(pluginDirectory, error);
    CHECK(!error);
    write_binary_file(pluginSource, "executable-default-plugin");
    const ScopedEnvironmentVariable inheritedXdgConfig(L"XDG_CONFIG_HOME",
                                                        callerConfig.wstring());
    const size_t beforeProcesses = count_named_processes(L"yt-dlp.exe");
    auto resolver = YouTubeResolverTestAccess::Create(fixture.directory);

    const ResolveResult result = resolver->Resolve(L"https://youtu.be/success-pluginaudit", {});

    CHECK(result.ok);
    CHECK(std::filesystem::is_regular_file(pluginSource, error));
    CHECK(!error);
    CHECK(!std::filesystem::exists(executionMarker, error));
    CHECK(!error);
    CHECK(wait_for_named_process_count(L"yt-dlp.exe", beforeProcesses,
                                       std::chrono::milliseconds{500}));
    std::filesystem::remove_all(callerConfig, error);
    CHECK(!error);
}

void youtube_resolver_serializes_queued_resolve_and_cancel_does_not_poison_reuse_test()
{
    ResolverFixture fixture;
    auto resolver = YouTubeResolverTestAccess::Create(
        fixture.directory, std::chrono::seconds{5});
    ResolveResult active;
    ResolveResult queued;
    std::atomic<bool> queuedFinished{false};
    std::thread activeThread([&] {
        active = resolver->Resolve(L"https://youtu.be/hang", {});
    });
    Sleep(75);
    std::thread queuedThread([&] {
        queued = resolver->Resolve(L"https://youtu.be/success", {});
        queuedFinished = true;
    });
    Sleep(75);
    CHECK(!queuedFinished.load());
    resolver->Cancel();
    activeThread.join();
    queuedThread.join();

    CHECK_EQ(ResolveError::Cancelled, active.error);
    CHECK(queued.ok);
    CHECK(queuedFinished.load());
    CHECK(resolver->Resolve(L"https://youtu.be/success", {}).ok);
}

void youtube_resolver_queued_stop_token_cancels_before_launch_test()
{
    ResolverFixture fixture;
    auto resolver = YouTubeResolverTestAccess::Create(
        fixture.directory, std::chrono::seconds{5});
    const size_t beforeProcesses = count_named_processes(L"yt-dlp.exe");
    ResolveResult active;
    ResolveResult queued;
    std::thread activeThread([&] {
        active = resolver->Resolve(L"https://youtu.be/hang", {});
    });
    Sleep(75);
    std::stop_source queuedStop;
    std::thread queuedThread([&] {
        queued = resolver->Resolve(L"https://youtu.be/success", queuedStop.get_token());
    });
    Sleep(75);
    queuedStop.request_stop();
    resolver->Cancel();
    activeThread.join();
    queuedThread.join();
    CHECK_EQ(ResolveError::Cancelled, active.error);
    CHECK_EQ(ResolveError::Cancelled, queued.error);
    Sleep(50);
    CHECK(wait_for_named_process_count(L"yt-dlp.exe", beforeProcesses,
                                       std::chrono::milliseconds{500}));
}

void youtube_resolver_injected_startup_and_drain_failures_cleanup_boundedly_test()
{
    ResolverFixture fixture;
    struct Case {
        YouTubeResolver::FailureStage stage;
        ResolveError expected;
    };
    const std::array cases{
        Case{YouTubeResolver::FailureStage::PipeHandlesOwned, ResolveError::StartFailed},
        Case{YouTubeResolver::FailureStage::JobAssignment, ResolveError::StartFailed},
        Case{YouTubeResolver::FailureStage::Resume, ResolveError::StartFailed},
        Case{YouTubeResolver::FailureStage::PipeRead, ResolveError::ExtractionFailed},
    };

    for (const Case& test : cases) {
        DWORD beforeHandles = 0;
        DWORD afterHandles = 0;
        CHECK(GetProcessHandleCount(GetCurrentProcess(), &beforeHandles) != FALSE);
        const size_t beforeProcesses = count_named_processes(L"yt-dlp.exe");
        auto resolver = YouTubeResolverTestAccess::Create(
            fixture.directory, std::chrono::seconds{5}, test.stage);
        const auto started = std::chrono::steady_clock::now();
        const ResolveResult result = resolver->Resolve(L"https://youtu.be/hang", {});
        const auto elapsed = std::chrono::steady_clock::now() - started;
        CHECK_EQ(test.expected, result.error);
        CHECK(result.detail.find(L"hang") == std::wstring::npos);
        CHECK(result.detail.find(L"https") == std::wstring::npos);
        CHECK(elapsed < std::chrono::seconds{2});
        resolver.reset();
        Sleep(25);
        CHECK(GetProcessHandleCount(GetCurrentProcess(), &afterHandles) != FALSE);
        CHECK(afterHandles <= beforeHandles + 2);
        CHECK(wait_for_named_process_count(L"yt-dlp.exe", beforeProcesses,
                                           std::chrono::milliseconds{500}));
    }
}

void youtube_resolver_repeated_owned_pipe_failures_cannot_hide_two_handle_leaks_test()
{
    ResolverFixture fixture;
    constexpr int repetitions = 16;
    DWORD beforeHandles = 0;
    DWORD afterHandles = 0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &beforeHandles) != FALSE);
    const size_t beforeProcesses = count_named_processes(L"yt-dlp.exe");

    for (int repetition = 0; repetition < repetitions; ++repetition) {
        auto resolver = YouTubeResolverTestAccess::Create(
            fixture.directory, std::chrono::seconds{5},
            YouTubeResolver::FailureStage::PipeHandlesOwned);
        const auto started = std::chrono::steady_clock::now();
        const ResolveResult result = resolver->Resolve(L"https://youtu.be/hang", {});
        const auto elapsed = std::chrono::steady_clock::now() - started;
        CHECK_EQ(ResolveError::StartFailed, result.error);
        CHECK(elapsed < std::chrono::seconds{2});
        resolver.reset();
        CHECK(wait_for_named_process_count(L"yt-dlp.exe", beforeProcesses,
                                           std::chrono::milliseconds{500}));

        size_t entries = 0;
        std::error_code error;
        for (std::filesystem::directory_iterator item(fixture.directory, error), end;
             !error && item != end; item.increment(error)) {
            ++entries;
        }
        CHECK(!error);
        CHECK_EQ(size_t{3}, entries);
    }

    CHECK(GetProcessHandleCount(GetCurrentProcess(), &afterHandles) != FALSE);
    CHECK(afterHandles <= beforeHandles + 2);
}

void youtube_resolver_repeated_timeout_cancel_overflow_cycles_are_leak_free_test()
{
    ResolverFixture fixture;
    const size_t beforeProcesses = count_named_processes(L"yt-dlp.exe");

    DWORD beforeTimeoutHandles = 0;
    DWORD afterTimeoutHandles = 0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &beforeTimeoutHandles) != FALSE);
    for (int cycle = 0; cycle < 4; ++cycle) {
        auto timeoutResolver = YouTubeResolverTestAccess::Create(
            fixture.directory, std::chrono::milliseconds{80});
        CHECK_EQ(ResolveError::TimedOut,
                 timeoutResolver->Resolve(L"https://youtu.be/hang", {}).error);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &afterTimeoutHandles) != FALSE);
    CHECK(afterTimeoutHandles <= beforeTimeoutHandles + 2);
    CHECK(wait_for_named_process_count(L"yt-dlp.exe", beforeProcesses,
                                       std::chrono::milliseconds{500}));

    DWORD beforeCancelHandles = 0;
    DWORD afterCancelHandles = 0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &beforeCancelHandles) != FALSE);
    for (int cycle = 0; cycle < 4; ++cycle) {
        auto cancelResolver = YouTubeResolverTestAccess::Create(
            fixture.directory, std::chrono::seconds{5});
        ResolveResult cancelled;
        std::thread worker([&] {
            cancelled = cancelResolver->Resolve(L"https://youtu.be/hang", {});
        });
        Sleep(30);
        cancelResolver->Cancel();
        worker.join();
        CHECK_EQ(ResolveError::Cancelled, cancelled.error);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &afterCancelHandles) != FALSE);
    CHECK(afterCancelHandles <= beforeCancelHandles + 2);
    CHECK(wait_for_named_process_count(L"yt-dlp.exe", beforeProcesses,
                                       std::chrono::milliseconds{500}));

    DWORD beforeOverflowHandles = 0;
    DWORD afterOverflowHandles = 0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &beforeOverflowHandles) != FALSE);
    for (int cycle = 0; cycle < 4; ++cycle) {
        auto overflowResolver = YouTubeResolverTestAccess::Create(fixture.directory);
        CHECK_EQ(ResolveError::OutputTooLarge,
                 overflowResolver->Resolve(L"https://youtu.be/cap64plus", {}).error);
    }
    Sleep(50);
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &afterOverflowHandles) != FALSE);
    CHECK(afterOverflowHandles <= beforeOverflowHandles + 2);
    CHECK(wait_for_named_process_count(L"yt-dlp.exe", beforeProcesses,
                                       std::chrono::milliseconds{500}));
}

void ngx_same_device_overlapping_sessions_initialize_and_shutdown_once_test()
{
    ngx_session_detail::Registry sessions;
    int device = 0;
    int initCalls = 0;
    int shutdownCalls = 0;

    CHECK(sessions.Acquire(&device, [&] { ++initCalls; return true; }));
    CHECK(sessions.Acquire(&device, [&] { ++initCalls; return true; }));
    CHECK_EQ(1, initCalls);
    CHECK_EQ(size_t{2}, sessions.LeaseCount(&device));

    sessions.Release(&device, [&] { ++shutdownCalls; });
    CHECK_EQ(0, shutdownCalls);
    CHECK_EQ(size_t{1}, sessions.LeaseCount(&device));
    CHECK(sessions.Acquire(&device, [&] { ++initCalls; return true; }));
    CHECK_EQ(1, initCalls);
    sessions.Release(&device, [&] { ++shutdownCalls; });
    CHECK_EQ(0, shutdownCalls);
    sessions.Release(&device, [&] { ++shutdownCalls; });
    CHECK_EQ(1, shutdownCalls);
    CHECK_EQ(size_t{0}, sessions.LeaseCount(&device));
}

void ngx_failed_initialization_never_acquires_a_session_test()
{
    ngx_session_detail::Registry sessions;
    int device = 0;
    int shutdownCalls = 0;

    CHECK(!sessions.Acquire(&device, [] { return false; }));
    CHECK_EQ(size_t{0}, sessions.LeaseCount(&device));
    sessions.Release(&device, [&] { ++shutdownCalls; });
    CHECK_EQ(0, shutdownCalls);
}

void ngx_failed_candidate_setup_releases_only_its_overlapping_lease_test()
{
    ngx_session_detail::Registry sessions;
    int device = 0;
    int initCalls = 0;
    int shutdownCalls = 0;

    CHECK(sessions.Acquire(&device, [&] { ++initCalls; return true; }));
    CHECK(sessions.Acquire(&device, [&] { ++initCalls; return true; }));
    sessions.Release(&device, [&] { ++shutdownCalls; });

    CHECK_EQ(1, initCalls);
    CHECK_EQ(0, shutdownCalls);
    CHECK_EQ(size_t{1}, sessions.LeaseCount(&device));
    sessions.Release(&device, [&] { ++shutdownCalls; });
    CHECK_EQ(1, shutdownCalls);
}

void ngx_distinct_devices_own_independent_sessions_test()
{
    ngx_session_detail::Registry sessions;
    int firstDevice = 0;
    int secondDevice = 0;
    int initCalls = 0;
    int shutdownCalls = 0;

    CHECK(sessions.Acquire(&firstDevice, [&] { ++initCalls; return true; }));
    CHECK(sessions.Acquire(&secondDevice, [&] { ++initCalls; return true; }));
    CHECK_EQ(2, initCalls);
    sessions.Release(&firstDevice, [&] { ++shutdownCalls; });
    CHECK_EQ(1, shutdownCalls);
    CHECK_EQ(size_t{1}, sessions.LeaseCount(&secondDevice));
    sessions.Release(&secondDevice, [&] { ++shutdownCalls; });
    CHECK_EQ(2, shutdownCalls);
}

void ngx_create_failure_is_not_retried_until_explicit_reset_test()
{
    ngx_session_detail::FeatureCreateGate gate;
    CHECK(gate.ShouldAttempt());
    gate.RecordFailure();
    CHECK(!gate.ShouldAttempt());
    CHECK(!gate.ShouldAttempt());
    gate.Reset();
    CHECK(gate.ShouldAttempt());
}

void ngx_renderer_frame_state_prioritizes_explicit_rehook_after_create_failure_test()
{
    ngx_session_detail::FeatureCreateGate gate;
    bool delayedRecreateDone = false;
    bool recreateRequested = false;
    int createAttempts = 0;
    const auto ensureFeature = [&] {
        if (!gate.ShouldAttempt()) return false;
        ++createAttempts;
        gate.RecordFailure();
        return false;
    };
    const auto recreateFeature = [&] {
        gate.Reset();
        ++createAttempts;
        gate.RecordFailure();
        return false;
    };

    const auto firstFailure = ngx_session_detail::PrepareFeatureForFrame(
        true, false, 2, delayedRecreateDone, recreateRequested,
        ensureFeature, recreateFeature);
    CHECK(firstFailure.selected);
    CHECK_EQ(1, createAttempts);

    const auto automaticNextFrame = ngx_session_detail::PrepareFeatureForFrame(
        true, false, 3, delayedRecreateDone, recreateRequested,
        ensureFeature, recreateFeature);
    CHECK(automaticNextFrame.selected);
    CHECK_EQ(1, createAttempts);

    recreateRequested = true;
    const auto explicitRehook = ngx_session_detail::PrepareFeatureForFrame(
        true, false, 4, delayedRecreateDone, recreateRequested,
        ensureFeature, recreateFeature);
    CHECK(explicitRehook.selected);
    CHECK(!recreateRequested);
    CHECK_EQ(2, createAttempts);

    ngx_session_detail::PrepareFeatureForFrame(
        true, false, 5, delayedRecreateDone, recreateRequested,
        ensureFeature, recreateFeature);
    CHECK_EQ(2, createAttempts);
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    const std::wstring executableName=current_test_executable().filename().wstring();
    if(_wcsicmp(executableName.c_str(),L"ffprobe.exe")==0||_wcsicmp(executableName.c_str(),L"ffmpeg.exe")==0)return run_fake_media_child(argc,argv);
    if (argc > 1) return run_fake_resolver_child(argc, argv);
    harness_sanity_test();
    runtime_shutdown_releases_player_before_media_foundation_and_com_test();
    release_package_filename_policy_is_allowlisted_and_fail_closed_test();
    public_release_package_policy_excludes_private_and_optional_binaries_test();
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
    playback_timeline_follows_the_presented_frame_test();
    playback_lateness_is_bounded_to_one_and_a_half_frames_test();
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
    button_content_layout_preserves_required_insets_and_icon_gap_at_every_dpi_test();
    button_content_layout_centers_combined_icon_and_label_without_outline_contact_test();
    prerender_surface_layout_keeps_progress_cancel_and_text_inside_client_bounds_test();
    advanced_menu_contains_clear_neural_cache_and_no_removed_quality_commands_test();
    debug_view_popup_contains_all_existing_views_and_selection_test();
    player_menu_is_english_only_and_retains_advanced_commands_test();
    youtube_source_quality_menu_is_distinct_radio_group_and_updates_test();
    youtube_availability_drives_real_menu_and_idle_action_consistently_test();
    youtube_resolution_generation_accepts_only_the_current_completion_test();
    youtube_resolution_disables_only_conflicting_source_actions_test();
    youtube_resolution_error_mapping_is_actionable_and_distinct_test();
    youtube_source_forces_ffmpeg_and_never_allows_media_foundation_fallback_test();
    youtube_resolution_cancellation_runs_stop_cancel_join_in_order_test();
    youtube_display_and_log_labels_never_expose_direct_urls_test();
    youtube_real_menu_and_ctrl_l_route_share_the_enabled_action_test();
    fixed_youtube_examples_are_complete_safe_and_menu_routable_test();
    youtube_completion_registry_is_scalar_once_only_and_spoof_safe_test();
    youtube_completion_registry_post_failure_and_concurrency_are_owned_test();
    youtube_renderer_transaction_validates_every_open_seek_and_quality_candidate_geometry_test();
    youtube_renderer_transaction_validates_before_atomic_handoff_and_rolls_back_test();
    youtube_candidate_seek_render_failure_preserves_all_active_state_before_commit_test();
    youtube_network_read_decisions_are_identical_and_once_only_at_both_positions_test();
    youtube_async_transaction_coalesces_and_discards_stale_work_before_handoff_test();
    youtube_stale_and_cancelled_prepared_seek_ownership_is_destroyed_once_test();
    youtube_decoder_probe_and_frame_reads_are_bounded_nonblocking_test();
    youtube_decoder_partial_stall_cancel_and_exit_leave_no_children_test();
    youtube_decoder_discards_only_expected_trailing_partial_frame_test();
    youtube_decoder_background_seek_trickles_and_cancels_boundedly_test();
    video_decoder_hardware_failure_falls_back_to_software_test();
    video_decoder_background_queue_is_bounded_to_four_frames_test();
    video_decoder_resume_failures_are_bounded_and_leak_free_for_local_and_network_startup_test();
    youtube_audio_held_pipe_stop_destroy_and_failure_fallback_are_bounded_test();
    youtube_audio_failed_waits_and_query_retire_reader_without_termination_or_leaks_test();
    youtube_prepared_audio_starts_silent_and_handoff_has_no_overlap_test();
    youtube_prepared_handoff_shows_candidate_and_retires_every_old_owner_before_activation_test();
    youtube_prepared_handoff_sizes_and_shows_real_candidate_before_owned_retirement_test();
    youtube_prepared_window_api_failures_are_reported_before_commit_test();
    youtube_destroyed_window_and_visibility_failure_leave_active_state_unchanged_test();
    youtube_candidate_render_failure_releases_window_handle_and_prepared_processes_test();
    legacy_language_configuration_is_ignored_and_english_lookup_remains_builtin_test();
    gpu_teardown_fence_signal_failure_stops_before_event_registration_test();
    gpu_teardown_fence_signal_failure_maps_to_device_removed_when_device_reason_failed_test();
    gpu_teardown_fence_event_registration_failure_stops_before_wait_test();
    gpu_teardown_fence_wait_failure_is_bounded_and_reported_test();
    gpu_teardown_fence_timeout_is_bounded_and_reported_test();
    gpu_teardown_fence_ignores_old_event_wake_until_new_target_completes_test();
    gpu_teardown_fence_consecutive_timeout_does_not_let_old_registration_complete_new_target_test();
    gpu_teardown_fence_device_removed_sentinel_is_not_completion_test();
    gpu_teardown_fence_stale_wakes_share_one_absolute_timeout_budget_test();
    renderer_non_teardown_wait_failure_is_propagated_test();
    renderer_safe_owner_releases_owned_resources_only_after_completed_or_removed_drain_test();
    renderer_safe_owner_retains_resources_after_live_device_drain_failure_test();
    renderer_frame_signal_failure_is_cached_without_advancing_tracking_test();
    renderer_frame_signal_device_removal_is_cached_and_safe_owner_releases_test();
    renderer_frame_signal_success_advances_tracking_once_test();
    renderer_cache_capture_requires_a_successful_neural_evaluation_test();
    renderer_cache_capture_returns_exact_tight_bgra_geometry_test();
    renderer_cache_capture_wait_failure_never_exposes_partial_bytes_test();
    renderer_cache_capture_does_not_apply_playback_color_adjustments_test();
    gpu_classification_table_test();
    neural_addon_policy_test();
    neural_prerender_defaults_prefer_1080p_and_preserve_explicit_output_test();
    neural_runtime_layout_is_absent_complete_or_fail_closed_test();
    default_neural_carrier_uses_native_resolution_dlaa_test();
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
    neural_addon_runtime_settings_enable_neural_and_disable_upscaling_test();
    neural_addon_runtime_settings_are_created_without_enabling_upscaling_test();
    neural_addon_runtime_settings_fail_closed_on_duplicate_managed_keys_test();
    reshade_trailing_section_text_uses_reshade_section_boundaries_test();
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
    resolver_output_accepts_separate_https_video_and_audio_urls_test();
    resolver_output_rejects_empty_multiple_oversize_or_untrusted_urls_test();
    resolver_output_enforces_raw_16k_and_single_trailing_line_ending_test();
    resolver_nonzero_exit_returns_fixed_generic_non_url_detail_test();
    youtube_resolver_windows_argument_quoting_covers_empty_spaces_quotes_and_slashes_test();
    youtube_resolver_argument_vector_is_exact_and_ordered_test();
    youtube_source_quality_selectors_prefer_exact_requested_heights_and_auto_fallback_test();
    youtube_resolver_success_uses_beside_app_helpers_and_exact_child_arguments_test();
    youtube_resolver_reports_missing_and_unstartable_helpers_without_sensitive_data_test();
    youtube_resolver_maps_nonzero_exit_and_output_overflow_precisely_test();
    youtube_resolver_honors_stop_token_and_explicit_cancel_with_bounded_wait_test();
    youtube_resolver_times_out_and_kills_its_descendant_job_tree_test();
    youtube_resolver_repeated_runs_leave_process_handle_count_stable_test();
    youtube_resolver_rejects_symlink_and_nonregular_helpers_before_execution_test();
    youtube_resolver_holds_verified_helpers_against_replacement_until_completion_test();
    youtube_resolver_forces_package_local_deno_cache_over_parent_override_test();
    youtube_resolver_disables_default_plugin_execution_from_inherited_config_test();
    youtube_resolver_serializes_queued_resolve_and_cancel_does_not_poison_reuse_test();
    youtube_resolver_queued_stop_token_cancels_before_launch_test();
    youtube_resolver_injected_startup_and_drain_failures_cleanup_boundedly_test();
    youtube_resolver_repeated_owned_pipe_failures_cannot_hide_two_handle_leaks_test();
    youtube_resolver_repeated_timeout_cancel_overflow_cycles_are_leak_free_test();
    ngx_same_device_overlapping_sessions_initialize_and_shutdown_once_test();
    ngx_failed_initialization_never_acquires_a_session_test();
    ngx_failed_candidate_setup_releases_only_its_overlapping_lease_test();
    ngx_distinct_devices_own_independent_sessions_test();
    ngx_create_failure_is_not_retried_until_explicit_reset_test();
    ngx_renderer_frame_state_prioritizes_explicit_rehook_after_create_failure_test();

    if (test_support::failure_count != 0) {
        std::cerr << test_support::failure_count << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "Resolver symlink coverage: "
              << (resolverSymlinkCoverageExercised ? "exercised" : "unavailable") << '\n';
    std::cout << "PolicyTests: all assertions passed\n";
    return EXIT_SUCCESS;
}
