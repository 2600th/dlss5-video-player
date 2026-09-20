#include "CacheEvictionPolicy.h"
#include "PlatformPaths.h"
#include "RendererRecoveryPolicy.h"
#include "TestSupport.h"

#include "RuntimePolicy.h"
#include "GpuPreference.h"
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
#include "PlaybackTiming.h"
#include "LiveSessionPolicy.h"
#include "FrameRatePolicy.h"
#include "PlaybackCadence.h"
#include "NeuralCoverage.h"
#include "SynchronizedPlayback.h"
#include "HardErrorSuppression.h"
#include "DeferredCapture.h"
#include "AudioClockPolicy.h"
#include "CachedRenderVerdict.h"
#include "Nv12Convert.h"
#include "NeuralPresets.h"
#ifdef small
#undef small
#endif

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winioctl.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <stop_token>
#include <sstream>
#include <string>
#include <future>
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
    // The defaults are for cases that need a probe and a first frame to simply
    // arrive: the fake helpers are this binary re-entering itself, which a cold
    // CI runner spawns in well over 250 ms, and that is how
    // youtube_decoder_discards_only_expected_trailing_partial_frame_test failed
    // once on GitHub while passing everywhere else. Every case that measures a
    // timeout passes its own.
    static std::unique_ptr<VideoDecoder> Create(
        const std::filesystem::path& helperDirectory,
        std::chrono::milliseconds probeTimeout = std::chrono::seconds{5},
        std::chrono::milliseconds stallTimeout = std::chrono::seconds{2},
        VideoDecoder::FailureStage failureStage = VideoDecoder::FailureStage::None,
        bool resetAcceleration = true)
    {
        // Dead hardware paths are remembered per memo, so a test decoder gets a
        // fresh one and never touches the process-wide memory. Passing false is
        // how a test that is checking exactly that memory keeps the memo the
        // previous Create handed out.
        static std::shared_ptr<AccelerationMemo> memo;
        if(resetAcceleration||!memo)memo=MakeAccelerationMemo();
        VideoDecoder::Settings settings;
        settings.helperDirectory=helperDirectory.wstring();settings.probeTimeout=probeTimeout;settings.stallTimeout=stallTimeout;settings.faults.resume=failureStage;
        settings.accelerationMemo=memo;
        return std::make_unique<VideoDecoder>(std::move(settings));
    }

    // main.cpp's two Swap callers hand a stream to the decoder that then answers
    // ColorDescription(), PixelLayout() and Media() for it, so every one of
    // those has to follow the swap. One side is opened as a known sibling (memo
    // key handed in, no probe: nothing declared, Bgra), the other sequentially
    // on the fixture's declared BT.709 4x2 stream (probed, NV12); afterwards
    // each must describe the other's stream, in both directions.
    static void CheckSwapCarriesSource(const std::filesystem::path& helperDirectory)
    {
        auto known=Create(helperDirectory);
        VideoDecoder::KnownMedia media{};
        media.width=4;media.height=2;media.fps=30.0;media.durationSec=30.0;media.hardwareProfile="hevc/yuv420p";
        CHECK(known->OpenKnown(L"nv12geom",media,MediaSourceKind::LocalFile));
        CHECK(known->PixelLayout()==VideoPixelLayout::Bgra);
        CHECK(known->ColorDescription().matrix==ColorMatrix::Unspecified);
        auto probed=Create(helperDirectory);
        CHECK(probed->OpenSequential(L"nv12geom",MediaSourceKind::LocalFile));
        CHECK(probed->PixelLayout()==VideoPixelLayout::Nv12);
        CHECK(probed->ColorDescription().matrix==ColorMatrix::Bt709);
        known->Swap(*probed);
        CHECK(known->PixelLayout()==VideoPixelLayout::Nv12);
        CHECK(known->ColorDescription().matrix==ColorMatrix::Bt709);
        CHECK(known->ColorDescription().range==ColorRange::Limited);
        CHECK(known->Media().hardwareProfile.empty());
        CHECK(probed->PixelLayout()==VideoPixelLayout::Bgra);
        CHECK(probed->ColorDescription().matrix==ColorMatrix::Unspecified);
        CHECK(probed->ColorDescription().range==ColorRange::Unspecified);
        CHECK_EQ(std::string("hevc/yuv420p"),probed->Media().hardwareProfile);
        CHECK_EQ(uint32_t{4},probed->Width());
        CHECK_EQ(uint32_t{2},known->Height());
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
        settings.helperDirectory=helperDirectory.wstring();settings.faults.disableAudioDevice=true;
        settings.faults.failTerminateJob=failTerminateJob;
        settings.faults.failInitialProcessWait=failInitialProcessWait;
        settings.faults.failGetExitCodeProcess=failGetExitCodeProcess;
        settings.faults.failFinalProcessWait=failFinalProcessWait;
        settings.faults.failInitialReaderWait=failInitialReaderWait;
        settings.faults.failFinalReaderWait=failFinalReaderWait;
        return std::make_unique<AudioPlayer>(std::move(settings));
    }
    static double SeekBase(const AudioPlayer& player){return player.SeekBaseSeconds();}
    static uint64_t SubmittedBuffers(const AudioPlayer& player){return player.SubmittedBuffers();}
};

struct RendererOwnedSentinel final : D3D12RendererTestOwnedResource {
    explicit RendererOwnedSentinel(std::shared_ptr<int> destroyed):destroyed(std::move(destroyed)){}
    ~RendererOwnedSentinel() override {++*destroyed;}
    std::shared_ptr<int> destroyed;
};

struct D3D12RendererTestAccess {
    // The hooks object exists only once a test installs something.
    static D3D12RendererTestHooks& Hooks(D3D12Renderer& renderer)
    {
        if(!renderer.m_testHooks)
            renderer.m_testHooks=std::make_unique<D3D12RendererTestHooks>();
        return *renderer.m_testHooks;
    }

    // The source conversion's program, compiled the way the renderer compiles it
    // but without a device, so every arm is checkable on any machine.
    static bool CompileSourceNv12(SourceNv12Conversion conversion,
                                  Microsoft::WRL::ComPtr<ID3DBlob>& blob)
    {
        return D3D12Renderer::CompileSourceNv12(conversion,blob);
    }

    static void ConfigureWait(D3D12Renderer& renderer,
                              d3d12_renderer_detail::FenceWaitResult result,
                              int& waits)
    {
        Hooks(renderer).waitGPU=[&waits,result]{++waits;return result;};
    }
    static void OwnSentinel(D3D12Renderer& renderer,
                            std::unique_ptr<D3D12RendererTestOwnedResource> sentinel)
    {
        Hooks(renderer).ownedResource=std::move(sentinel);
    }
    static bool WaitForContinuedUse(D3D12Renderer& renderer)
    {
        return renderer.WaitGPUForContinuedUse();
    }
    static void ConfigureFrameSignal(D3D12Renderer& renderer,HRESULT signalResult,
                                     HRESULT deviceRemovedReason,int& signalCalls,
                                     int& reasonChecks)
    {
        Hooks(renderer).frameSignal=[&signalCalls,signalResult](uint64_t){
            ++signalCalls;return signalResult;
        };
        Hooks(renderer).deviceRemovedReason=[&reasonChecks,deviceRemovedReason]{
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
        renderer.m_lastDLSSUsed=neuralUsed;Hooks(renderer).cacheCapture=std::move(capture);
    }
    static bool CaptureEvaluatedFrame(D3D12Renderer& renderer,CapturedVideoFrame& frame)
    {
        return renderer.CaptureEvaluatedFrame(frame);
    }
    // The deleter ends the process on the second renderer it has to retain;
    // a suite that retains one per case starts each such case from zero.
    static void ResetRetainedRenderers(){D3D12Renderer::s_retainedRenderers.store(0);}
    static void OnExitProcess(D3D12Renderer& renderer,std::function<void()> exit)
    {
        Hooks(renderer).exitProcess=std::move(exit);
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
    const std::vector<ToolbarAction> allActions{
        ToolbarAction::Open,
        ToolbarAction::Back10,
        ToolbarAction::PlayPause,
        ToolbarAction::Stop,
        ToolbarAction::Forward10,
        ToolbarAction::Mute,
        ToolbarAction::ToggleNeuralRendering,
        ToolbarAction::ToggleUpscaling,
        ToolbarAction::ToggleFrameGeneration,
        ToolbarAction::Aspect,
        ToolbarAction::Adjustments,
        ToolbarAction::DebugView,
        ToolbarAction::Fullscreen,
    };

    // The required set is whatever the layout keeps at its own minimum width,
    // measured here rather than restated: a copy of the list only reported
    // that somebody had edited the toolbar, which is how this test failed when
    // the frame-generation pill stopped being required.
    const auto normal = LayoutToolbar(MinimumToolbarClientWidth(96), 180, 96);
    const std::vector<ToolbarAction> required = toolbar_actions(normal);
    CHECK(!required.empty());
    for (const auto& item : normal) CHECK(!item.compact);

    // Below that width nothing required drops off the bar - the pills only go
    // compact - so every action reachable at 320 px is reachable at any width.
    const auto narrow = LayoutToolbar(320, 180, 96);
    CHECK_EQ(required, toolbar_actions(narrow));
    for (const auto& item : narrow) CHECK(item.compact);

    const auto wide = LayoutToolbar(1600, 180, 96);
    CHECK_EQ(allActions, toolbar_actions(wide));
    for (const auto& item : wide) CHECK(!item.compact);

    // Whatever the required set holds, it keeps the wide bar's order, so a
    // pill never jumps across its neighbours when the window is resized. And
    // opening a file, starting playback and leaving fullscreen have to stay
    // reachable at the narrowest window the player allows.
    const auto is_ordered_subset = [](const std::vector<ToolbarAction>& subset,
                                      const std::vector<ToolbarAction>& full) {
        auto cursor = full.begin();
        for (const ToolbarAction action : subset) {
            cursor = std::find(cursor, full.end(), action);
            if (cursor == full.end()) return false;
            ++cursor;
        }
        return true;
    };
    CHECK(is_ordered_subset(required, allActions));
    for (const ToolbarAction action : {ToolbarAction::Open, ToolbarAction::PlayPause,
                                       ToolbarAction::Fullscreen}) {
        CHECK(std::find(required.begin(), required.end(), action) != required.end());
    }
}

void toolbar_layout_preserves_group_separation_test()
{
    const auto items = LayoutToolbar(1600, 180, 96);
    const ToolbarItem* back = find_toolbar_item(items, ToolbarAction::Back10);
    const ToolbarItem* play = find_toolbar_item(items, ToolbarAction::PlayPause);
    const ToolbarItem* mute = find_toolbar_item(items, ToolbarAction::Mute);
    const ToolbarItem* neural = find_toolbar_item(items, ToolbarAction::ToggleNeuralRendering);
    const ToolbarItem* upscaling = find_toolbar_item(items, ToolbarAction::ToggleUpscaling);
    const ToolbarItem* frameGeneration = find_toolbar_item(items, ToolbarAction::ToggleFrameGeneration);
    const ToolbarItem* adjustments = find_toolbar_item(items, ToolbarAction::Adjustments);
    const ToolbarItem* debug = find_toolbar_item(items, ToolbarAction::DebugView);
    const ToolbarItem* fullscreen = find_toolbar_item(items, ToolbarAction::Fullscreen);
    CHECK(back && play && mute && neural && upscaling && frameGeneration && adjustments && debug && fullscreen);
    if (!(back && play && mute && neural && upscaling && frameGeneration && adjustments && debug && fullscreen)) return;

    CHECK_EQ(4L, play->bounds.left - back->bounds.right);
    CHECK_EQ(12L, neural->bounds.left - mute->bounds.right);
    CHECK_EQ(4L, upscaling->bounds.left - neural->bounds.right);
    CHECK_EQ(4L, frameGeneration->bounds.left - upscaling->bounds.right);
    CHECK_EQ(12L, debug->bounds.left - adjustments->bounds.right);
    CHECK_EQ(4L, fullscreen->bounds.left - debug->bounds.right);
}

void toolbar_layout_scales_hit_height_and_avoids_overlap_test()
{
    const auto narrow = LayoutToolbar(320, 180, 96);
    const auto normal = LayoutToolbar(900, 180, 96);
    const auto wide = LayoutToolbar(1600, 180, 96);
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
    CHECK(wide.back().bounds.right <= 1600 - 16);
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
    // Measured from the layout at 96 dpi instead of listed: the set's
    // membership is the toolbar's business, and what this test owns is that
    // the floor produces the same set, uncompacted, at every dpi.
    const std::vector<ToolbarAction> requiredNarrow =
        toolbar_actions(LayoutToolbar(MinimumToolbarClientWidth(96), 400, 96));
    CHECK(!requiredNarrow.empty());

    for (const UINT dpi : {0u, 96u, 120u, 144u, 192u}) {
        const UINT effectiveDpi = dpi == 0 ? 96 : dpi;
        const int clientWidth = MinimumToolbarClientWidth(dpi);
        const int clientHeight = MulDiv(400, static_cast<int>(effectiveDpi), 96);
        const auto items = LayoutToolbar(clientWidth, clientHeight, dpi);
        CHECK_EQ(requiredNarrow, toolbar_actions(items));
        // The floor is exact: one pixel less and the same pills have to shrink
        // to their compact form, which is what the minimum track size buys.
        const auto belowFloor = LayoutToolbar(clientWidth - 1, clientHeight, dpi);
        CHECK_EQ(requiredNarrow, toolbar_actions(belowFloor));
        for (const auto& item : belowFloor) CHECK(item.compact);
        for (const auto& item : items) {
            CHECK(!item.compact);
            CHECK(item.bounds.bottom - item.bounds.top >= MulDiv(36, static_cast<int>(effectiveDpi), 96));
        }
        check_toolbar_items_do_not_overlap(items);
        if (!items.empty()) {
            CHECK_EQ(static_cast<LONG>(MulDiv(kToolbarOuterGutterDip, static_cast<int>(effectiveDpi), 96)), items.front().bounds.left);
            CHECK_EQ(static_cast<LONG>(clientWidth - MulDiv(kToolbarOuterGutterDip, static_cast<int>(effectiveDpi), 96)), items.back().bounds.right);
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
    for (const int width : {320}) {
        const auto items = LayoutToolbar(width, 180, 96);
        CHECK(!LayoutVolumeSlider(width, 180, 96, items).has_value());
    }

    const int thresholdWidth = MinimumToolbarClientWidth(96) + 185;
    const auto thresholdItems = LayoutToolbar(thresholdWidth, 180, 96);
    const auto slider = LayoutVolumeSlider(thresholdWidth, 180, 96, thresholdItems);
    CHECK(slider.has_value());
    if (!slider) return;
    for (const auto& item : thresholdItems) {
        CHECK(!rectangles_intersect(*slider, item.bounds));
    }
    CHECK_EQ(static_cast<LONG>(thresholdWidth - 185), slider->left);
    CHECK_EQ(static_cast<LONG>(thresholdWidth - 95), slider->right);
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

    const auto loadedItems = LayoutToolbar(1600, 180, 96);
    const ToolbarAvailability withoutRenderer{true, false, false};
    CHECK(!IsToolbarActionEnabled(ToolbarAction::ToggleNeuralRendering, withoutRenderer));
    CHECK(!IsToolbarActionEnabled(ToolbarAction::ToggleUpscaling, withoutRenderer));
    // Frame generation is not on the renderer's gate here: the assertion that
    // used to sit at this line read as "no renderer, so no conversion" while
    // passing only because availability was false. Its real rule is below.
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

// The frame-generation pill answers to exactly two facts, and a seek is not
// one of them: a conversion reads the whole file, so it can be started while
// the playhead is moving. Restating the seek gate here is what greyed the pill
// out mid-seek while the identically-named menu item stayed live.
void frame_generation_pill_follows_media_and_availability_only_test()
{
    ToolbarAvailability ready{};
    ready.mediaLoaded = true;
    ready.frameGenerationAvailable = true;
    CHECK(IsToolbarActionEnabled(ToolbarAction::ToggleFrameGeneration, ready));

    ToolbarAvailability withoutMedia = ready;
    withoutMedia.mediaLoaded = false;
    CHECK(!IsToolbarActionEnabled(ToolbarAction::ToggleFrameGeneration, withoutMedia));

    ToolbarAvailability withoutAvailability = ready;
    withoutAvailability.frameGenerationAvailable = false;
    CHECK(!IsToolbarActionEnabled(ToolbarAction::ToggleFrameGeneration, withoutAvailability));

    ToolbarAvailability seekingReady = ready;
    seekingReady.seeking = true;
    CHECK(IsToolbarActionEnabled(ToolbarAction::ToggleFrameGeneration, seekingReady));
    // The pills beside it do stop during a seek, so the difference is the
    // point and not an accident of this availability value.
    CHECK(!IsToolbarActionEnabled(ToolbarAction::ToggleUpscaling, seekingReady));

    // The pill reads "Frame Generation · Cancel" while a conversion runs, and a
    // control that says Cancel has to be clickable. Availability is false then
    // by construction - nothing can be started - so gating the pill on it alone
    // made the label a lie: the click never reached CancelFrameGeneration.
    ToolbarAvailability converting{};
    converting.mediaLoaded = true;
    converting.frameGenerationAvailable = false;
    converting.frameGenerationConverting = true;
    CHECK(IsToolbarActionEnabled(ToolbarAction::ToggleFrameGeneration, converting));
    // Still nothing without media, whatever a stale conversion flag says.
    ToolbarAvailability convertingWithoutMedia = converting;
    convertingWithoutMedia.mediaLoaded = false;
    CHECK(!IsToolbarActionEnabled(ToolbarAction::ToggleFrameGeneration, convertingWithoutMedia));
    // And the flag is frame generation's alone: it must not revive a neighbour.
    CHECK(!IsToolbarActionEnabled(ToolbarAction::ToggleUpscaling, converting));
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
    const auto wide = LayoutToolbar(1600, 180, 96);
    const auto narrow = LayoutToolbar(320, 180, 96);
    const auto contains = [](std::span<const ToolbarItem> items, ToolbarAction action) {
        return std::any_of(items.begin(), items.end(),
                           [action](const ToolbarItem& item) { return item.action == action; });
    };

    CHECK(contains(wide, ToolbarAction::DebugView));
    CHECK(!contains(narrow, ToolbarAction::DebugView));
    CHECK(contains(wide, ToolbarAction::ToggleNeuralRendering));
    CHECK(contains(wide, ToolbarAction::ToggleUpscaling));
    CHECK(contains(wide, ToolbarAction::ToggleFrameGeneration));
    CHECK(contains(wide, ToolbarAction::PlayPause));

    CHECK_EQ(ToolbarAction::Open,
             ReconcileFocusedToolbarAction(narrow, ToolbarAction::DebugView, loaded));

    ToolbarAvailability withoutRenderer = loaded;
    withoutRenderer.rendererReady = false;
    CHECK_EQ(ToolbarAction::Open,
             ReconcileFocusedToolbarAction(wide, ToolbarAction::ToggleNeuralRendering,
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
        CHECK(!shortLayout.stacked);
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
    // Stand-ins for whatever the two producers say: the bar does not compose
    // this copy, it carries it, so the test states that and not the wording of
    // the day. Both fields are always assigned by their producers, and the old
    // "unavailable" fallbacks for an empty field are gone with them.
    status.upscalingStatus = L"<upscaling segment>";
    status.frameGenerationStatus = L"<frame generation segment>";
    // The components a viewer reads off the bar: which runtime is active, both
    // feature segments verbatim, the source geometry, the output geometry when
    // the pipeline changed it, the quality name, both frame rates rounded to
    // whole frames, and a dropped count only when frames were dropped. Their
    // separator and order are the bar's own business.
    //
    // The DLSS input geometry is deliberately NOT here: it only ever repeated
    // the source or the output, and this line is drawn into one ellipsised row
    // where a repeated fact costs a fact that is not.
    const auto shows = [](const std::wstring& text, std::wstring_view part) {
        return text.find(part) != std::wstring::npos;
    };
    const std::wstring neural = BuildPlayerStatusText(status);
    CHECK(shows(neural, L"Neural addon enabled"));
    CHECK(shows(neural, status.upscalingStatus));
    CHECK(shows(neural, status.frameGenerationStatus));
    CHECK(shows(neural, L"1920\u00d71080"));
    CHECK(shows(neural, L"3840\u00d72160"));
    CHECK(shows(neural, L"Quality"));
    // The format itself, not the digits in isolation: "60" alone is satisfied by
    // the 2160 in a geometry, and "fps" by any segment carrying a rate.
    CHECK(shows(neural, L"58 / 60 fps"));
    CHECK(shows(neural, L"Dropped 3"));
    // Rounded, never the raw double: a status line that reads 58.4 implies a
    // precision the sampled rate does not have.
    CHECK(!shows(neural, L"58.4"));
    CHECK(!shows(neural, L"59.94"));

    // A source the pipeline did not resize prints ONE geometry, not the same
    // numbers two or three times.
    PlayerStatusSnapshot unresized = status;
    unresized.outputWidth = unresized.sourceWidth;
    unresized.outputHeight = unresized.sourceHeight;
    unresized.droppedFrames = 0;
    const std::wstring passthrough = BuildPlayerStatusText(unresized);
    const auto occurrences = [](const std::wstring& text, std::wstring_view part) {
        size_t count = 0;
        for (size_t at = text.find(part); at != std::wstring::npos; at = text.find(part, at + 1)) ++count;
        return count;
    };
    CHECK_EQ(size_t(1), occurrences(passthrough, L"1920\u00d71080"));
    // And a clean run says nothing about dropped frames at all.
    CHECK(!shows(passthrough, L"Dropped"));

    // Changing one segment changes only that segment.
    status.upscalingStatus = L"DLSS Upscaling on \u00b7 3840\u00d72160 (auto)";
    const std::wstring upscaling = BuildPlayerStatusText(status);
    CHECK(shows(upscaling, status.upscalingStatus));
    CHECK(shows(upscaling, status.frameGenerationStatus));
    CHECK(!shows(upscaling, L"<upscaling segment>"));

    status.runtimeConfiguration = PlayerRuntimeConfiguration::DlssSrSafeMode;
    status.dlssState = PlayerDlssState::Active;
    CHECK(BuildPlayerStatusText(status).starts_with(L"DLSS SR safe mode"));
    CHECK(!shows(BuildPlayerStatusText(status), L"Neural addon"));
    status.dlssState = PlayerDlssState::ScalerFallback;
    CHECK(BuildPlayerStatusText(status).starts_with(L"DLSS SR safe mode"));

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
    // A drag previews, a pending seek shows its target, and otherwise the
    // timeline and a pause both report the frame that was last presented.
    CHECK_EQ(12.0, playback_timing::TimelinePosition(false, 0.0, false, 0.0, 12.0));
    CHECK_EQ(12.0, playback_timing::PausePosition(12.0));
    CHECK_EQ(18.0, playback_timing::TimelinePosition(true, 18.0, false, 0.0, 12.0));
    CHECK_EQ(24.0, playback_timing::TimelinePosition(false, 0.0, true, 24.0, 12.0));
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
    if(menu)DestroyMenu(menu);
}

void feature_menu_uses_distinct_controls_and_honest_availability_test()
{
    Localizer localizer;
    const HMENU menu = app_menu::CreateMenuBar(localizer, true);
    CHECK(menu != nullptr);
    std::vector<MenuEntry> entries;
    if (menu) collect_menu_entries(menu, entries);
    CHECK(has_menu_entry(entries, L"Neural Rendering\tD", app_menu::IDM_NEURAL_RENDERING));
    CHECK(has_menu_entry(entries, L"DLSS Upscaling",
                         app_menu::IDM_DLSS_UPSCALING));
    // Frame generation is a command now, not a permanently disabled label, and
    // the cancel item beside it is what makes a minutes-long conversion
    // stoppable. The old entry read "Unavailable in this build", which was
    // honest while no backend existed and would now be a lie.
    CHECK(has_menu_entry(entries, L"Generate frames (higher frame rate)...",
                         app_menu::IDM_FRAME_GENERATION));
    CHECK(has_menu_entry(entries, L"Cancel frame generation",
                         app_menu::IDM_CANCEL_FRAME_GENERATION));
    CHECK(!has_menu_text(entries, L"Enable DLSS\tD"));
    CHECK(!has_menu_text(entries, L"Frame Generation\tUnavailable in this build"));

    // The two real toggles report a mode, so they carry a checkmark. Frame
    // generation is an action that starts a one-shot conversion: a check on it
    // would state a mode the player never has, so it gets an enable state and
    // nothing else, whatever its arguments say.
    CHECK(app_menu::UpdateFeatureAvailability(menu, true, true, true,
                                              false, false, false, false));
    const UINT neural = GetMenuState(menu, app_menu::IDM_NEURAL_RENDERING, MF_BYCOMMAND);
    const UINT upscaling = GetMenuState(menu, app_menu::IDM_DLSS_UPSCALING, MF_BYCOMMAND);
    const UINT frameGeneration = GetMenuState(menu, app_menu::IDM_FRAME_GENERATION, MF_BYCOMMAND);
    CHECK((neural & (MF_DISABLED | MF_GRAYED)) == 0);
    CHECK((neural & MF_CHECKED) != 0);
    CHECK((upscaling & (MF_DISABLED | MF_GRAYED)) != 0);
    CHECK((frameGeneration & (MF_DISABLED | MF_GRAYED)) != 0);
    CHECK((frameGeneration & MF_CHECKED) == 0);
    // And it enables when the player says a conversion is possible, which the
    // permanently-false argument could never show - still without a check,
    // with both toggles on and checked beside it.
    CHECK(app_menu::UpdateFeatureAvailability(menu, true, true, true,
                                              true, true, true, true));
    const UINT enabledFrameGeneration =
        GetMenuState(menu, app_menu::IDM_FRAME_GENERATION, MF_BYCOMMAND);
    CHECK((enabledFrameGeneration & (MF_DISABLED | MF_GRAYED)) == 0);
    CHECK((enabledFrameGeneration & MF_CHECKED) == 0);
    CHECK((GetMenuState(menu, app_menu::IDM_NEURAL_RENDERING, MF_BYCOMMAND) & MF_CHECKED) != 0);
    CHECK((GetMenuState(menu, app_menu::IDM_DLSS_UPSCALING, MF_BYCOMMAND) & MF_CHECKED) != 0);
    // The converted file is unreachable until one exists: the item that opens
    // it is created greyed and only main.cpp's own state can enable it, so no
    // availability argument here may light it up.
    const UINT showOutput = GetMenuState(menu, app_menu::IDM_SHOW_FRAMEGEN_OUTPUT, MF_BYCOMMAND);
    CHECK(showOutput != static_cast<UINT>(-1));
    CHECK((showOutput & (MF_DISABLED | MF_GRAYED)) != 0);
    if (menu) DestroyMenu(menu);
}

void debug_view_popup_contains_all_existing_views_and_selection_test()
{
    const HMENU menu = app_menu::CreateDebugViewMenu(app_menu::IDM_VIEW_DEPTH);
    CHECK(menu != nullptr);
    CHECK_EQ(4, menu ? GetMenuItemCount(menu) : 0);
    std::vector<MenuEntry> entries;
    if (menu) collect_menu_entries(menu, entries);
    CHECK(has_menu_entry(entries, L"Final output\t1", app_menu::IDM_VIEW_FINAL));
    CHECK(has_menu_entry(entries, L"DLSS input\t2", app_menu::IDM_VIEW_INPUT));
    CHECK(has_menu_entry(entries, L"Motion vectors\t3", app_menu::IDM_VIEW_MV));
    CHECK(has_menu_entry(entries, L"Depth\t4", app_menu::IDM_VIEW_DEPTH));
    for (const auto& entry : entries) {
        if (entry.command == app_menu::IDM_VIEW_DEPTH) CHECK((entry.state & MFS_CHECKED) != 0);
        else CHECK((entry.state & MFS_CHECKED) == 0);
    }
    if (menu) DestroyMenu(menu);
}

void range_preview_and_comparison_menus_route_keys_and_gate_availability_test()
{
    Localizer localizer;
    const HMENU menu = app_menu::CreateMenuBar(localizer, true);
    CHECK(menu != nullptr);
    std::vector<MenuEntry> entries;
    if (menu) collect_menu_entries(menu, entries);
    CHECK(has_menu_entry(entries, L"Mark In\tI", app_menu::IDM_MARK_IN));
    CHECK(has_menu_entry(entries, L"Mark Out\tO", app_menu::IDM_MARK_OUT));
    CHECK(has_menu_entry(entries, L"Clear Marks\tShift+I / Shift+O", app_menu::IDM_CLEAR_MARKS));
    CHECK(has_menu_entry(entries, L"Go to timecode...\tCtrl+G", app_menu::IDM_GOTO_TIMECODE));
    CHECK(has_menu_entry(entries, L"Pause neural render\tSpace", app_menu::IDM_PAUSE_NEURAL_RENDER));
    CHECK(has_menu_entry(entries, L"Preview this frame (neural)\tF", app_menu::IDM_PREVIEW_FRAME));
    CHECK(has_menu_entry(entries, L"Preview 4 s clip (neural)\tShift+F", app_menu::IDM_PREVIEW_CLIP));
    // Conversion writes a file; it lives in its own submenu beside saving.
    CHECK(has_menu_entry(entries, L"Convert marked clip to neural video\tCtrl+R", app_menu::IDM_RENDER_RANGE));
    CHECK(has_menu_entry(entries, L"Convert whole video to neural video", app_menu::IDM_RENDER_WHOLE));
    CHECK(has_menu_entry(entries, L"Save converted video...", app_menu::IDM_EXPORT_CACHED_VIDEO));
    CHECK(has_menu_entry(entries, L"Cancel saving", app_menu::IDM_CANCEL_EXPORT));
    CHECK(has_menu_entry(entries, L"Neural settings...\tCtrl+N", app_menu::IDM_NEURAL_SETTINGS));
    CHECK(has_menu_entry(entries, L"Open render receipt", app_menu::IDM_OPEN_RENDER_RECEIPT));
    CHECK(has_menu_entry(entries, L"Zoom 2x\tZ", app_menu::IDM_COMPARE_ZOOM));
    // Depth is a persisted guide switch in the neural settings dialog now.
    CHECK(!has_menu_text(entries, L"Estimated / flat depth proxy\tG"));
    HMENU video = find_top_level_submenu(menu, L"Video");
    HMENU compare = find_top_level_submenu(video, L"Compare");
    CHECK(compare != nullptr);
    std::vector<MenuEntry> compareEntries;
    if (compare) collect_menu_entries(compare, compareEntries);
    CHECK(has_menu_entry(compareEntries, L"Neural", app_menu::IDM_COMPARE_NEURAL));
    CHECK(has_menu_entry(compareEntries, L"Blend", app_menu::IDM_COMPARE_BLEND));
    CHECK(has_menu_entry(compareEntries, L"Split", app_menu::IDM_COMPARE_SPLIT));
    CHECK(has_menu_entry(compareEntries, L"Wipe", app_menu::IDM_COMPARE_WIPE));
    CHECK(has_menu_entry(compareEntries, L"Blend less\t[", app_menu::IDM_COMPARE_BLEND_LESS));
    CHECK(has_menu_entry(compareEntries, L"Blend more\t]", app_menu::IDM_COMPARE_BLEND_MORE));

    // A fresh bar has nothing loaded: every range, render and compare item is grayed.
    const auto grayed = [&](UINT command) {
        return (GetMenuState(menu, command, MF_BYCOMMAND) & (MF_DISABLED | MF_GRAYED)) != 0;
    };
    const auto checked = [&](UINT command) {
        return (GetMenuState(menu, command, MF_BYCOMMAND) & MF_CHECKED) != 0;
    };
    for (const UINT command : {app_menu::IDM_MARK_IN, app_menu::IDM_GOTO_TIMECODE, app_menu::IDM_PREVIEW_FRAME,
                               app_menu::IDM_RENDER_WHOLE, app_menu::IDM_PAUSE_NEURAL_RENDER, app_menu::IDM_OPEN_RENDER_RECEIPT,
                               app_menu::IDM_COMPARE_BLEND, app_menu::IDM_COMPARE_ZOOM})
        CHECK(grayed(command));
    CHECK(checked(app_menu::IDM_COMPARE_NEURAL));

    CHECK(app_menu::UpdateRenderActionAvailability(menu, true, false, true, true, true));
    CHECK(!grayed(app_menu::IDM_MARK_OUT));
    CHECK(grayed(app_menu::IDM_PREVIEW_CLIP));
    CHECK(!grayed(app_menu::IDM_PAUSE_NEURAL_RENDER));
    CHECK(checked(app_menu::IDM_PAUSE_NEURAL_RENDER));
    CHECK(!grayed(app_menu::IDM_OPEN_RENDER_RECEIPT));
    CHECK(app_menu::UpdateRenderActionAvailability(menu, true, true, false, true, false));
    CHECK(!grayed(app_menu::IDM_RENDER_RANGE));
    CHECK(grayed(app_menu::IDM_PAUSE_NEURAL_RENDER));
    CHECK(!checked(app_menu::IDM_PAUSE_NEURAL_RENDER)); // No job: a stale pause flag never shows.

    CHECK(app_menu::UpdateComparisonMenu(menu, true, true, app_menu::IDM_COMPARE_SPLIT, true));
    CHECK(!grayed(app_menu::IDM_COMPARE_WIPE));
    CHECK(checked(app_menu::IDM_COMPARE_SPLIT));
    CHECK(!checked(app_menu::IDM_COMPARE_NEURAL));
    CHECK(checked(app_menu::IDM_COMPARE_ZOOM));
    CHECK(app_menu::UpdateComparisonMenu(menu, false, true, 999u, false));
    CHECK(grayed(app_menu::IDM_COMPARE_SPLIT));
    CHECK(!grayed(app_menu::IDM_COMPARE_ZOOM)); // Zoom is view-independent.
    CHECK(checked(app_menu::IDM_COMPARE_NEURAL)); // Unknown selection falls back to Neural.
    CHECK(!checked(app_menu::IDM_COMPARE_ZOOM));

    using app_menu::CommandForPlayerKey;
    CHECK(CommandForPlayerKey('I', false, false) == app_menu::IDM_MARK_IN);
    CHECK(CommandForPlayerKey('O', false, false) == app_menu::IDM_MARK_OUT);
    CHECK(CommandForPlayerKey('I', false, true) == app_menu::IDM_CLEAR_MARKS);
    CHECK(CommandForPlayerKey('O', false, true) == app_menu::IDM_CLEAR_MARKS);
    CHECK(CommandForPlayerKey('G', true, false) == app_menu::IDM_GOTO_TIMECODE);
    CHECK(CommandForPlayerKey('F', false, false) == app_menu::IDM_PREVIEW_FRAME);
    CHECK(CommandForPlayerKey('F', false, true) == app_menu::IDM_PREVIEW_CLIP);
    CHECK(CommandForPlayerKey('R', true, false) == app_menu::IDM_RENDER_RANGE);
    CHECK(CommandForPlayerKey('N', true, false) == app_menu::IDM_NEURAL_SETTINGS);
    CHECK(CommandForPlayerKey('Z', false, false) == app_menu::IDM_COMPARE_ZOOM);
    CHECK(CommandForPlayerKey(VK_OEM_4, false, false) == app_menu::IDM_COMPARE_BLEND_LESS);
    CHECK(CommandForPlayerKey(VK_OEM_6, false, false) == app_menu::IDM_COMPARE_BLEND_MORE);
    // Existing single-letter and Ctrl accelerators keep their owners.
    for (const UINT key : {UINT('D'), UINT('S'), UINT('A'), UINT('M'), UINT('G'), UINT('R'), UINT('N'), UINT(VK_SPACE), UINT(VK_F6)})
        CHECK(!CommandForPlayerKey(key, false, false).has_value());
    for (const UINT key : {'O', 'E', 'L', 'I', 'F', 'Z'})
        CHECK(!CommandForPlayerKey(key, true, false).has_value());
    CHECK(!CommandForPlayerKey('G', true, true).has_value());
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

    // The labels come from the localizer on purpose: this test defends the
    // entry-to-command mapping and the radio group, not the wording, which
    // changed when Auto stopped meaning "1080p preferred".
    const std::array expected{
        std::pair{localizer.Get(L"menu.youtube_quality_auto"), app_menu::IDM_YOUTUBE_QUALITY_AUTO},
        std::pair{localizer.Get(L"menu.youtube_quality_2160"), app_menu::IDM_YOUTUBE_QUALITY_2160},
        std::pair{localizer.Get(L"menu.youtube_quality_1440"), app_menu::IDM_YOUTUBE_QUALITY_1440},
        std::pair{localizer.Get(L"menu.youtube_quality_1080"), app_menu::IDM_YOUTUBE_QUALITY_1080},
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
    CHECK(!app_menu::YouTubeQualityForCommand(330u /* removed legacy quality command */).has_value());
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
    // Every key the mapping and the media path can hand the UI resolves to
    // its own sentence: a missing entry would show the raw key, and two
    // failures sharing a sentence would be indistinguishable to the user.
    Localizer localizer;
    std::vector<std::wstring> messages;
    for (const wchar_t* key : {L"youtube.error.invalid", L"youtube.error.helper_missing",
                               L"youtube.error.start_failed", L"youtube.error.timeout",
                               L"youtube.error.cancelled", L"youtube.error.extraction",
                               L"youtube.error.ffmpeg", L"youtube.error.media_timeout",
                               L"youtube.error.media_stalled"}) {
        const std::wstring message = localizer.Get(key);
        CHECK(message != key);
        CHECK(std::find(messages.begin(), messages.end(), message) == messages.end());
        messages.push_back(message);
    }
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

    std::vector<std::wstring_view> urls;
    for (const ExampleVideo& example : kExampleVideos) {
        CHECK(!example.title.empty());
        CHECK(!example.channel.empty());
        CHECK(example.url.starts_with(L"https://www.youtube.com/watch?v="));
        CHECK(IsSupportedYouTubeUrl(example.url));
        CHECK(std::find(urls.begin(), urls.end(), example.url) == urls.end());
        urls.push_back(example.url);
    }

    Localizer localizer;
    const HMENU menu = app_menu::CreateMenuBar(localizer, true);
    CHECK(menu != nullptr);
    HMENU file = find_top_level_submenu(menu, L"File");
    HMENU examples = find_top_level_submenu(file, L"Game trailers");
    HMENU animeMenu = find_top_level_submenu(examples, L"Anime");
    CHECK(file != nullptr);
    CHECK(examples != nullptr);
    CHECK(animeMenu == nullptr);
    std::vector<MenuEntry> entries;
    if (menu) collect_menu_entries(menu, entries);
    for (size_t index = 0; index < kExampleVideos.size(); ++index) {
        const UINT command = app_menu::IDM_EXAMPLE_VIDEO_FIRST + static_cast<UINT>(index);
        CHECK(app_menu::ExampleVideoForCommand(command) == &kExampleVideos[index]);
        CHECK(has_menu_entry(entries, kExampleVideos[index].title,
                             command));
    }
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

// The harness is the only thing standing between one bad case and the rest of
// the suite, so it gets tested like anything else. CHECK(true) said nothing.
//
// These three probes are run through the real runner by the case below. They
// are deliberately not in kCases: they fail on purpose.
int g_harness_statements_after_require = 0;

void harness_probe_require_stops_the_case()
{
    // Read through the counter so the condition is not a constant the
    // compiler folds - /W4 reports C4127 for a literally false REQUIRE.
    REQUIRE(g_harness_statements_after_require < 0);
    ++g_harness_statements_after_require;  // must never run
}

void harness_probe_access_violation()
{
    volatile int* nowhere = nullptr;
    *nowhere = 1;
}

void harness_probe_passes()
{
    CHECK(true);
}

// Three properties, all of which the suite lacked: a hard failure stops its
// own case rather than the run; the case after a crash still executes; and
// each failure is attributed to the case that produced it by name.
void harness_isolates_a_failing_case_from_the_ones_after_it_test()
{
    static constexpr test_support::TestCase probes[] = {
        TEST_CASE(harness_probe_require_stops_the_case),
        TEST_CASE(harness_probe_access_violation),
        TEST_CASE(harness_probe_passes),
    };

    g_harness_statements_after_require = 0;
    const int failuresBefore = test_support::failure_count;
    // The probes report through the same stream every other case does, so it
    // is borrowed for the duration rather than letting two expected failures
    // print as if the suite were broken.
    std::ostringstream captured;
    std::streambuf* const previous = std::cerr.rdbuf(captured.rdbuf());
    const test_support::RunSummary summary =
        test_support::run_cases(probes, std::size(probes), {});
    std::cerr.rdbuf(previous);
    test_support::failure_count = failuresBefore;

    CHECK_EQ(size_t{3}, summary.ran);
    CHECK_EQ(size_t{2}, summary.failed);
    // A failed REQUIRE abandons its case at the point of failure. A failed
    // CHECK would have carried on to the increment.
    CHECK_EQ(0, g_harness_statements_after_require);

    const std::string report = captured.str();
    CHECK(report.find("harness_probe_require_stops_the_case") != std::string::npos);
    CHECK(report.find("harness_probe_access_violation") != std::string::npos);
    // The access violation is reported as one, not as a silent abort.
    CHECK(report.find("c0000005") != std::string::npos);
    // The case after the crash ran and did not fail.
    CHECK(report.find("harness_probe_passes") == std::string::npos);
}

// There was no eviction at all. RemoveSource and RemoveRender existed with no
// production caller; the only reclamation was Clear(), which is all or
// nothing. Meanwhile the key deliberately retires entries wholesale -
// applicationVersion, driverVersion, modelStoreDigest, runtimeDigest and the
// manifest schema are all key terms - so an NVIDIA driver update changes every
// key at once. A user with 40 GB of renders takes the update, all 40 GB
// becomes unreachable, everything re-renders, and the cache grows to 80 GB.
// The only remedy offered destroys the new renders too.
//
// Two rules, in order. An entry whose manifest can no longer be reused is
// dead whatever the disk looks like. Everything else is only evicted when the
// disk is actually under pressure, because re-rendering a film costs minutes
// to hours and free space costs nothing until it runs out.
namespace {
cache_eviction::Entry EvictionEntry(std::string key, uintmax_t bytes, int64_t lastUsed,
                                    bool reusable = true, bool active = false)
{
    return cache_eviction::Entry{std::move(key), bytes, lastUsed, reusable, active};
}
} // namespace

void eviction_removes_entries_that_can_never_match_a_key_again_test()
{
    const cache_eviction::Entry entries[] = {
        EvictionEntry("live", 10, 500),
        EvictionEntry("retired", 25, 900, /*reusable=*/false),
        EvictionEntry("also-live", 10, 100),
    };
    // Acres of free space: nothing is under pressure, and the dead entry still
    // goes.
    const auto plan = cache_eviction::PlanEviction(entries, /*freeBytes=*/1'000'000,
                                                   /*freeFloorBytes=*/1000);
    CHECK_EQ(size_t{1}, plan.evict.size());
    if (plan.evict.size() == 1) CHECK_EQ(std::string("retired"), plan.evict.front());
    CHECK_EQ(uintmax_t{25}, plan.freedBytes);
}

void eviction_keeps_everything_reusable_while_the_disk_has_room_test()
{
    const cache_eviction::Entry entries[] = {
        EvictionEntry("old", 100, 1),
        EvictionEntry("older", 100, 0),
    };
    const auto plan = cache_eviction::PlanEviction(entries, 1'000'000, 1000);
    CHECK(plan.evict.empty());
    CHECK_EQ(uintmax_t{0}, plan.freedBytes);
}

void eviction_frees_the_least_recently_used_until_the_floor_is_met_test()
{
    const cache_eviction::Entry entries[] = {
        EvictionEntry("newest", 100, 300),
        EvictionEntry("oldest", 100, 100),
        EvictionEntry("middle", 100, 200),
    };
    // 250 free, floor 400: 150 short, so the two oldest go and the newest stays.
    const auto plan = cache_eviction::PlanEviction(entries, 250, 400);
    CHECK_EQ(size_t{2}, plan.evict.size());
    if (plan.evict.size() == 2) {
        CHECK_EQ(std::string("oldest"), plan.evict[0]);
        CHECK_EQ(std::string("middle"), plan.evict[1]);
    }
    CHECK_EQ(uintmax_t{200}, plan.freedBytes);
    // It stops as soon as the floor is met rather than emptying the cache.
    CHECK(plan.freedBytes + 250 >= 400);
}

// A render in progress is reading and writing its own entry. Removing it under
// the job is worse than running out of disk.
void eviction_never_touches_an_active_entry_test()
{
    const cache_eviction::Entry entries[] = {
        EvictionEntry("rendering-now", 100, 0, /*reusable=*/true, /*active=*/true),
        EvictionEntry("dead-but-open", 100, 0, /*reusable=*/false, /*active=*/true),
        EvictionEntry("free-to-go", 100, 50),
    };
    const auto plan = cache_eviction::PlanEviction(entries, 0, 1'000'000);
    CHECK_EQ(size_t{1}, plan.evict.size());
    if (plan.evict.size() == 1) CHECK_EQ(std::string("free-to-go"), plan.evict.front());
}

// The floor cannot always be met. It must free what it can and say so rather
// than emptying the cache in a loop that can never succeed.
void eviction_frees_what_it_can_when_the_floor_is_unreachable_test()
{
    const cache_eviction::Entry entries[] = {
        EvictionEntry("a", 10, 1),
        EvictionEntry("b", 10, 2, /*reusable=*/true, /*active=*/true),
    };
    const auto plan = cache_eviction::PlanEviction(entries, 0, 1'000'000);
    CHECK_EQ(size_t{1}, plan.evict.size());
    CHECK_EQ(uintmax_t{10}, plan.freedBytes);
    CHECK(!plan.floorMet);
}

// A floor of zero disables pressure eviction, leaving only the dead entries.
// That is the switch for anyone who would rather manage the cache by hand.
void a_zero_floor_evicts_only_the_dead_test()
{
    const cache_eviction::Entry entries[] = {
        EvictionEntry("dead", 10, 1, /*reusable=*/false),
        EvictionEntry("alive", 10, 2),
    };
    const auto plan = cache_eviction::PlanEviction(entries, 0, 0);
    CHECK_EQ(size_t{1}, plan.evict.size());
    if (plan.evict.size() == 1) CHECK_EQ(std::string("dead"), plan.evict.front());
}

// DLSSBackend.cpp and DLSSGBackend.cpp were byte-identical and both wrong:
//
//     wchar_t exePath[MAX_PATH]{};
//     GetModuleFileNameW(nullptr, exePath, MAX_PATH);   // no return check
//
// On a path longer than 260 characters GetModuleFileNameW fills the buffer,
// returns exactly the size it was given, sets ERROR_INSUFFICIENT_BUFFER, and
// - before Windows 10 - does not even null-terminate. Both sites then took
// parent_path() of a truncated string and created ngx_logs somewhere else
// entirely. Fourteen sites called this function across three incompatible
// buffer strategies.
//
// The query is injected so the growth loop is testable without a 300-character
// install directory.
void module_path_grows_past_max_path_test()
{
    // Longer than kInitialPathCharacters, so the loop has to grow at least
    // once; 400 would have fit the first buffer and proved nothing.
    const std::wstring actual(700, L'x');
    int calls = 0;
    const auto queried = platform_paths::ModulePathWith(
        [&](wchar_t* buffer, uint32_t size) -> uint32_t {
            ++calls;
            if (actual.size() >= size) {          // truncated, exactly as Win32 reports it
                std::copy_n(actual.begin(), size, buffer);
                return size;
            }
            std::copy(actual.begin(), actual.end(), buffer);
            buffer[actual.size()] = L'\0';
            return static_cast<uint32_t>(actual.size());
        });

    CHECK(queried.has_value());
    if (queried) CHECK_EQ(actual, queried->wstring());
    // It grew rather than giving up on the first short answer.
    CHECK(calls > 1);
}

// The whole bug: a return equal to the buffer size means truncation, never a
// complete path. Accepting it is what put ngx_logs in the wrong directory.
void module_path_never_accepts_a_filled_buffer_test()
{
    const auto queried = platform_paths::ModulePathWith(
        [](wchar_t* buffer, uint32_t size) -> uint32_t {
            std::fill_n(buffer, size, L'y');   // always exactly full: never enough room
            return size;
        });
    CHECK(!queried.has_value());
}

void module_path_reports_a_failed_query_test()
{
    int calls = 0;
    const auto queried = platform_paths::ModulePathWith(
        [&](wchar_t*, uint32_t) -> uint32_t { ++calls; return 0; });
    CHECK(!queried.has_value());
    // A hard failure is not retried at a larger size.
    CHECK_EQ(1, calls);
}

void module_directory_is_the_parent_of_the_module_test()
{
    const auto directory = platform_paths::ModuleDirectoryWith(
        [](wchar_t* buffer, uint32_t size) -> uint32_t {
            const std::wstring path = LR"(C:\Program Files\Player\DLSSVideoPlayer.exe)";
            if (path.size() >= size) return size;
            std::copy(path.begin(), path.end(), buffer);
            buffer[path.size()] = L'\0';
            return static_cast<uint32_t>(path.size());
        });
    CHECK(directory.has_value());
    if (directory) CHECK_EQ(std::wstring(LR"(C:\Program Files\Player)"), directory->wstring());
}

// The real module, which every production caller uses. It has to answer on
// this machine or the fourteen call sites have no fallback.
void module_directory_answers_for_this_process_test()
{
    const auto directory = platform_paths::ModuleDirectory();
    CHECK(directory.has_value());
    if (directory) CHECK(std::filesystem::exists(*directory));
}

// RecoverUnusableRenderer rebuilt into the SAME HWND. DXGI allows one
// flip-model swapchain per window, and the renderer being retired may still
// own one: D3D12Renderer's deleter deliberately retains a renderer whose
// bounded GPU drain did not complete, because deleting it would free
// resources its command lists are still reading. So the rebuild met
// DXGI_ERROR_INVALID_CALL, the media unloaded, and the user was told the GPU
// was lost - on the one path that exists to survive exactly that.
//
// Every other renderer-swap path in the player (EnableUpscaling,
// CreateRendererCandidate) already creates a fresh child window. Only
// recovery reused, and it is the path no one exercises by hand.
//
// The window work is injected so the ordering is testable without a device,
// a display or a message loop - the same shape as the fence-teardown cases
// above.
void renderer_recovery_rebuilds_into_a_fresh_window_test()
{
    HWND retiring = reinterpret_cast<HWND>(0x1001);
    HWND initializedWith = nullptr;
    int created = 0;

    const auto result = renderer_recovery::Rebuild(
        retiring,
        [&] { ++created; return reinterpret_cast<HWND>(0x2002); },
        [](HWND) {},
        [] { return true; },
        [&](HWND window) { initializedWith = window; return true; });

    CHECK_EQ(1, created);
    CHECK(result.outcome == renderer_recovery::Outcome::Rebuilt);
    // The whole bug in one assertion.
    CHECK(initializedWith != retiring);
    CHECK_EQ(reinterpret_cast<HWND>(0x2002), initializedWith);
    CHECK_EQ(reinterpret_cast<HWND>(0x2002), result.window);
}

// A renderer that could not be drained is kept alive on purpose, and its
// swapchain with it. Destroying the window underneath it would leave that
// swapchain pointing at a dead HWND.
void renderer_recovery_keeps_a_retained_renderers_window_test()
{
    HWND retiring = reinterpret_cast<HWND>(0x1001);
    int destroyed = 0;

    const auto result = renderer_recovery::Rebuild(
        retiring,
        [] { return reinterpret_cast<HWND>(0x2002); },
        [&](HWND) { ++destroyed; },
        [] { return false; },   // retained: the drain did not complete
        [](HWND) { return true; });

    CHECK_EQ(0, destroyed);
    CHECK(!result.oldWindowDestroyed);
    // The rebuild still happens; it just happens somewhere else.
    CHECK(result.outcome == renderer_recovery::Outcome::Rebuilt);
    CHECK_EQ(reinterpret_cast<HWND>(0x2002), result.window);
}

void renderer_recovery_destroys_the_old_window_once_its_renderer_is_gone_test()
{
    HWND retiring = reinterpret_cast<HWND>(0x1001);
    std::vector<HWND> destroyed;

    const auto result = renderer_recovery::Rebuild(
        retiring,
        [] { return reinterpret_cast<HWND>(0x2002); },
        [&](HWND window) { destroyed.push_back(window); },
        [] { return true; },
        [](HWND) { return true; });

    CHECK_EQ(size_t{1}, destroyed.size());
    if (!destroyed.empty()) CHECK_EQ(retiring, destroyed.front());
    CHECK(result.oldWindowDestroyed);
}

// No window means no rebuild: initializing against the old one is the bug.
void renderer_recovery_without_a_window_does_not_initialize_test()
{
    HWND retiring = reinterpret_cast<HWND>(0x1001);
    int initialized = 0, destroyed = 0, released = 0;

    const auto result = renderer_recovery::Rebuild(
        retiring,
        [] { return HWND{nullptr}; },
        [&](HWND) { ++destroyed; },
        [&] { ++released; return true; },
        [&](HWND) { ++initialized; return true; });

    CHECK(result.outcome == renderer_recovery::Outcome::WindowCreationFailed);
    CHECK_EQ(0, initialized);
    CHECK_EQ(0, destroyed);
    // Nothing is retired either: the caller still has a usable old window to
    // report the failure through.
    CHECK_EQ(0, released);
    CHECK_EQ(retiring, result.window);
}

// A rebuild that fails on the device still reports which window it owns, so
// the caller tears down the right one.
void renderer_recovery_reports_the_new_window_after_a_failed_initialize_test()
{
    const auto result = renderer_recovery::Rebuild(
        reinterpret_cast<HWND>(0x1001),
        [] { return reinterpret_cast<HWND>(0x2002); },
        [](HWND) {},
        [] { return true; },
        [](HWND) { return false; });

    CHECK(result.outcome == renderer_recovery::Outcome::RendererInitFailed);
    CHECK_EQ(reinterpret_cast<HWND>(0x2002), result.window);
    CHECK(result.oldWindowDestroyed);
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

// Frame waits during rendering carry their own budget: long enough for the
// slowest supported GPU to finish three pipelined NR frames, still finite, and
// distinct from the short teardown budget. Device loss is not subject to it.
void gpu_render_fence_wait_uses_the_render_budget_and_reports_device_loss_first_test()
{
    DWORD observedTimeout=INFINITE;
    const auto timedOut=d3d12_renderer_detail::WaitForGPUFenceCompletion(
        uint64_t{48},GetTickCount64(),d3d12_renderer_detail::RenderFenceWaitMilliseconds,
        []{return uint64_t{0};},
        [](uint64_t){return S_OK;},
        [&](DWORD timeout){observedTimeout=timeout;return DWORD{WAIT_TIMEOUT};});
    CHECK_EQ(d3d12_renderer_detail::FenceWaitResult::TimedOut,timedOut);
    CHECK(observedTimeout>d3d12_renderer_detail::TeardownFenceWaitMilliseconds);
    CHECK(observedTimeout<=d3d12_renderer_detail::RenderFenceWaitMilliseconds);

    int waits=0;
    const auto removed=d3d12_renderer_detail::WaitForGPUFenceDrain(
        uint64_t{49},d3d12_renderer_detail::RenderFenceWaitMilliseconds,
        [](uint64_t){return S_OK;},
        [&]{return waits?UINT64_MAX:uint64_t{0};},
        [](uint64_t){return S_OK;},
        [&](DWORD){++waits;return DWORD{WAIT_OBJECT_0};});
    CHECK_EQ(d3d12_renderer_detail::FenceWaitResult::DeviceRemoved,removed);
    CHECK_EQ(1,waits);
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
    D3D12RendererTestAccess::ResetRetainedRenderers();
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
        D3D12RendererTestAccess::ResetRetainedRenderers();
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

// A drain that fails leaves resources the GPU may still be touching, so the
// renderer is leaked rather than freed - once. A second such renderer in one
// process is a loop that would leak the GPU dry, and the deleter ends the
// process instead; under test the exit is a hook, and the renderer is still
// kept rather than freed.
void renderer_second_retained_renderer_ends_the_process_test()
{
    D3D12RendererTestAccess::ResetRetainedRenderers();
    int exits=0;
    for(int retained=1;retained<=2;++retained){
        int waits=0;auto destroyed=std::make_shared<int>(0);
        auto renderer=MakeD3D12Renderer();
        D3D12RendererTestAccess::ConfigureWait(
            *renderer,d3d12_renderer_detail::FenceWaitResult::TimedOut,waits);
        D3D12RendererTestAccess::OnExitProcess(*renderer,[&]{++exits;});
        D3D12RendererTestAccess::OwnSentinel(
            *renderer,std::make_unique<RendererOwnedSentinel>(destroyed));

        renderer.reset();

        CHECK_EQ(1,waits);
        CHECK_EQ(0,*destroyed);
        CHECK_EQ(retained-1,exits);
    }
}

void renderer_frame_signal_failure_is_cached_without_advancing_tracking_test()
{
    D3D12RendererTestAccess::ResetRetainedRenderers();
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

// The Signal's own HRESULT can say the device is gone before
// GetDeviceRemovedReason does: a loss code latches DeviceRemoved, which the
// safe owner may then release, instead of SignalFailed, which it must retain.
void renderer_frame_signal_device_loss_code_latches_device_removed_test()
{
    using d3d12_renderer_detail::FenceWaitResult;
    static_assert(d3d12_renderer_detail::IsDeviceLossCode(DXGI_ERROR_DEVICE_REMOVED));
    static_assert(d3d12_renderer_detail::IsDeviceLossCode(DXGI_ERROR_DEVICE_RESET));
    static_assert(d3d12_renderer_detail::IsDeviceLossCode(DXGI_ERROR_DEVICE_HUNG));
    static_assert(!d3d12_renderer_detail::IsDeviceLossCode(E_FAIL));
    static_assert(!d3d12_renderer_detail::IsDeviceLossCode(S_OK));
    CHECK_EQ(FenceWaitResult::SignalFailed,
             d3d12_renderer_detail::ClassifyDeviceCallFailure(E_FAIL,FenceWaitResult::SignalFailed,[]{return S_OK;}));
    CHECK_EQ(FenceWaitResult::DeviceRemoved,
             d3d12_renderer_detail::ClassifyDeviceCallFailure(E_FAIL,FenceWaitResult::SignalFailed,[]{return DXGI_ERROR_DEVICE_REMOVED;}));
    CHECK_EQ(FenceWaitResult::DeviceRemoved,
             d3d12_renderer_detail::ClassifyDeviceCallFailure(DXGI_ERROR_DEVICE_RESET,FenceWaitResult::WaitFailed,[]{return S_OK;}));

    int signalCalls=0,reasonChecks=0;
    auto destroyed=std::make_shared<int>(0);
    auto renderer=MakeD3D12Renderer();
    D3D12RendererTestAccess::ConfigureFrameSignal(
        *renderer,DXGI_ERROR_DEVICE_HUNG,S_OK,signalCalls,reasonChecks);
    D3D12RendererTestAccess::SetFrameTracking(*renderer,2,12,2,8);
    D3D12RendererTestAccess::OwnSentinel(
        *renderer,std::make_unique<RendererOwnedSentinel>(destroyed));

    CHECK(!D3D12RendererTestAccess::SignalFrameSlot(*renderer,2));
    CHECK_EQ(1,signalCalls);CHECK_EQ(1,reasonChecks);
    CHECK(D3D12RendererTestAccess::GPUUnusable(*renderer));
    CHECK_EQ(FenceWaitResult::DeviceRemoved,D3D12RendererTestAccess::LastFenceResult(*renderer));

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
    CapturedVideoFrame frame;frame.pixels.assign(7,0x55);frame.width=9;frame.height=9;
    CHECK(!D3D12RendererTestAccess::CaptureEvaluatedFrame(*renderer,frame));
    CHECK_EQ(0,captures);CHECK(frame.pixels.empty());CHECK_EQ(uint32_t{0},frame.width);
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
    CHECK_EQ(size_t{16},frame.pixels.size());CHECK_EQ(uint8_t{15},frame.pixels.back());

    D3D12RendererTestAccess::ConfigureCacheCapture(
        *renderer,2,2,true,[](std::vector<uint8_t>& bytes){bytes.assign(17,0);return true;});
    CHECK(!D3D12RendererTestAccess::CaptureEvaluatedFrame(*renderer,frame));
    CHECK(frame.pixels.empty());CHECK_EQ(uint32_t{0},frame.width);CHECK_EQ(uint32_t{0},frame.height);
}

void renderer_cache_capture_wait_failure_never_exposes_partial_bytes_test()
{
    auto renderer=MakeD3D12Renderer();
    D3D12RendererTestAccess::ConfigureCacheCapture(
        *renderer,2,2,true,[](std::vector<uint8_t>& bytes){bytes.assign(8,0x44);return false;});
    CapturedVideoFrame frame;frame.pixels.assign(16,0x22);frame.width=2;frame.height=2;
    CHECK(!D3D12RendererTestAccess::CaptureEvaluatedFrame(*renderer,frame));
    CHECK(frame.pixels.empty());CHECK_EQ(uint32_t{0},frame.width);CHECK_EQ(uint32_t{0},frame.height);
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
    CHECK_EQ(neuralBytes,frame.pixels);
}

void gpu_classification_table_test()
{
    struct Case {
        uint32_t vendor_id;
        std::wstring_view description;
        GpuGeneration expected;
    };

    constexpr Case cases[] = {
        {0x10DE, L"NVIDIA GeForce RTX 2080 Ti", GpuGeneration::Rtx20Turing},
        {0x10DE, L"NVIDIA GeForce RTX 3060 Laptop GPU", GpuGeneration::Rtx30Ampere},
        {0x10DE, L"NVIDIA GeForce RTX 4090", GpuGeneration::Rtx40Ada},
        {0x10DE, L"NVIDIA GeForce RTX 4080 SUPER", GpuGeneration::Rtx40Ada},
        {0x10DE, L"NVIDIA GeForce RTX 4090 Laptop GPU", GpuGeneration::Rtx40Ada},
        {0x10DE, L"nViDiA gEfOrCe rTx 5090", GpuGeneration::Rtx50Blackwell},
        {0x10DE, L"NVIDIA RTX A4000", GpuGeneration::OtherRtx},
        {0x10DE, L"NVIDIA RTX 6000 Ada Generation", GpuGeneration::OtherRtx},
        {0x10DE, L"NVIDIA GeForce GTX 1660 SUPER", GpuGeneration::OtherNvidia},
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

// The renderer compares the adapter its device was created on against the one
// DetectHighPerformanceGpu classified, and it has to do that by LUID: a
// description is a model name, so two identical cards collide, and a hybrid
// laptop is exactly the machine where the comparison has to hold. Zero is
// DXGI's "no adapter", which is why it can never agree with anything - two
// missing adapters are not the same adapter.
void adapter_luid_identity_compares_parts_not_model_names_test()
{
    constexpr uint64_t part = PackAdapterLuid(0, uint32_t{0x0000C0DE});
    constexpr uint64_t twin = PackAdapterLuid(0, uint32_t{0x0000C0DF});
    CHECK_EQ(AdapterMatch::Same, CompareAdapterLuids(part, part));
    CHECK_EQ(AdapterMatch::Different, CompareAdapterLuids(part, twin));
    CHECK_EQ(AdapterMatch::Unknown, CompareAdapterLuids(part, 0));
    CHECK_EQ(AdapterMatch::Unknown, CompareAdapterLuids(0, part));
    CHECK_EQ(AdapterMatch::Unknown, CompareAdapterLuids(0, 0));

    // HighPart is signed and DXGI does issue negative ones; sign-extending it
    // would fold every such adapter onto the same packed value and make two
    // different parts compare Same.
    CHECK_EQ(uint64_t{0xFFFFFFFF0000C0DE}, PackAdapterLuid(-1, uint32_t{0x0000C0DE}));
    CHECK_EQ(AdapterMatch::Different,
             CompareAdapterLuids(PackAdapterLuid(-1, 1), PackAdapterLuid(-2, 1)));
    // The two halves must not be interchangeable either.
    CHECK_EQ(AdapterMatch::Different,
             CompareAdapterLuids(PackAdapterLuid(7, 0), PackAdapterLuid(0, 7)));
}

// Whatever adapter this machine has, the LUID the policy reports must name
// the adapter the rest of its answer describes - otherwise the renderer's
// comparison would be against a part nobody classified. Checked by finding
// that adapter again through DXGI, which needs no NVIDIA hardware.
void detected_high_performance_gpu_carries_the_luid_of_the_adapter_it_describes_test()
{
    const DetectedGpu detected = DetectHighPerformanceGpu();
    if (detected.adapterLuid == 0) {
        // No hardware adapter was detected; then nothing else may claim one.
        CHECK(detected.description.empty());
        CHECK_EQ(uint32_t{0}, detected.vendorId);
        CHECK_EQ(GpuGeneration::Unsupported, detected.generation);
        return;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    if (!factory) return;

    unsigned matches = 0;
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) continue;
        if (CompareAdapterLuids(detected.adapterLuid,
                                PackAdapterLuid(description.AdapterLuid.HighPart,
                                                description.AdapterLuid.LowPart)) !=
            AdapterMatch::Same) {
            continue;
        }
        ++matches;
        CHECK_EQ(detected.description, std::wstring(description.Description));
        CHECK_EQ(detected.vendorId, description.VendorId);
        CHECK_EQ(detected.deviceId, description.DeviceId);
        CHECK_EQ(detected.dedicatedVideoMemoryBytes,
                 static_cast<uint64_t>(description.DedicatedVideoMemory));
        CHECK_EQ(detected.generation, ClassifyGpu(description.VendorId, description.Description));
    }
    // A LUID is unique per physical adapter, so the detected one resolves to
    // exactly one entry of the same enumeration.
    CHECK_EQ(unsigned{1}, matches);
}

void nvidia_driver_version_is_read_out_of_the_dxgi_quad_test()
{
    struct Case {
        std::wstring_view dxgi;
        uint32_t major;
        uint32_t minor;
    };
    // The three machines this project has actually run on.
    constexpr Case accepted[] = {
        {L"32.0.15.6614", 566, 14},  // RTX 3060 Laptop, feature 18 refused
        {L"32.0.16.1047", 610, 47},  // RTX 4080 SUPER
        {L"32.0.16.1664", 616, 64},  // RTX 5090
    };
    for (const auto& test : accepted) {
        const auto parsed = ParseNvidiaDriverVersion(test.dxgi);
        CHECK(parsed.has_value());
        if (!parsed) continue;
        CHECK_EQ(test.major, parsed->major);
        CHECK_EQ(test.minor, parsed->minor);
        CHECK_EQ(std::to_wstring(test.major) + L"." + std::to_wstring(test.minor),
                 FormatNvidiaDriverVersion(*parsed));
    }
    // A single-digit minor keeps the two-digit form NVIDIA publishes.
    CHECK_EQ(std::wstring(L"580.05"), FormatNvidiaDriverVersion(NvidiaDriverVersion{580, 5}));

    constexpr std::wstring_view rejected[] = {
        L"", L"32.0.16", L"32.0.16.abcd", L"32.0.16.99999", L"32.0.16.1664.2", L"32.0..1664",
        L"32.0.16.", L"32.0.16.16 64", L"0.0.0.0",
    };
    for (const std::wstring_view text : rejected) {
        CHECK(!ParseNvidiaDriverVersion(text).has_value());
    }
}

void neural_driver_floor_separates_the_failing_machine_from_the_working_ones_test()
{
    CHECK_EQ(NeuralDriverSupport::BelowFloor, ClassifyNeuralDriver(L"32.0.15.6614"));
    CHECK_EQ(NeuralDriverSupport::Supported, ClassifyNeuralDriver(L"32.0.16.1047"));
    CHECK_EQ(NeuralDriverSupport::Supported, ClassifyNeuralDriver(L"32.0.16.1664"));
    CHECK_EQ(NeuralDriverSupport::Unknown, ClassifyNeuralDriver(L"not-a-driver"));
    CHECK_EQ(NeuralDriverSupport::Unknown, ClassifyNeuralDriver(L""));

    // The boundary is inclusive: 610.47 is the lowest driver known to work.
    CHECK_EQ(NeuralDriverSupport::BelowFloor, ClassifyNeuralDriver(L"32.0.16.1046"));
    constexpr NvidiaDriverVersion justBelowFloor{610, 46};
    CHECK(justBelowFloor < kNeuralDriverFloor);
    CHECK(!(kNeuralDriverFloor < kNeuralDriverFloor));
    CHECK(kNeuralDriverFloor < kNeuralDriverRecommended);
    CHECK_EQ(std::wstring(L"610.47"), FormatNvidiaDriverVersion(kNeuralDriverFloor));
    CHECK_EQ(std::wstring(L"616.64"), FormatNvidiaDriverVersion(kNeuralDriverRecommended));
}

void neural_addon_policy_test()
{
    for (const GpuGeneration rtx : {GpuGeneration::Rtx20Turing, GpuGeneration::Rtx30Ampere,
                                    GpuGeneration::Rtx40Ada, GpuGeneration::Rtx50Blackwell,
                                    GpuGeneration::OtherRtx}) {
        CHECK(NeuralAddonDesired(rtx, false));
        CHECK(!NeuralAddonDesired(rtx, true));
    }
    CHECK(!NeuralAddonDesired(GpuGeneration::OtherNvidia, false));
    CHECK(!NeuralAddonDesired(GpuGeneration::Unsupported, false));
    CHECK_EQ(std::string_view("rtx20"), GpuGenerationPathName(GpuGeneration::Rtx20Turing));
    CHECK_EQ(std::string_view("rtx30"), GpuGenerationPathName(GpuGeneration::Rtx30Ampere));
    CHECK_EQ(std::string_view("rtx40"), GpuGenerationPathName(GpuGeneration::Rtx40Ada));
    CHECK_EQ(std::string_view("rtx50"), GpuGenerationPathName(GpuGeneration::Rtx50Blackwell));
    CHECK_EQ(std::string_view("rtx"), GpuGenerationPathName(GpuGeneration::OtherRtx));
    CHECK_EQ(std::string_view("unsupported"), GpuGenerationPathName(GpuGeneration::OtherNvidia));
}

// H2's regression gate, written as the machines this project has actually
// seen: each adapter string paired with the driver DXGI reported on it. The
// generation only picks cache identity and the pace prior, so the #9 Ampere
// laptop is wanted exactly as much as the 5090 and the driver floor is the
// axis that refuses it. The GTX row is the other half of the same rule - the
// newest driver in the table grants nothing to a part with no tensor cores.
void neural_addon_is_gated_by_the_driver_floor_not_by_the_generation_test()
{
    struct Machine {
        uint32_t vendorId;
        std::wstring_view description;
        std::wstring_view driver;
        GpuGeneration generation;
        bool addonDesired;
        NeuralDriverSupport driverSupport;
    };

    constexpr Machine machines[] = {
        // Issue #9, in the shape Windows reports a mobile part.
        {0x10DE, L"NVIDIA GeForce RTX 3070 Ti Laptop GPU", L"32.0.15.6614",
         GpuGeneration::Rtx30Ampere, true, NeuralDriverSupport::BelowFloor},
        // The oldest supported generation on the newest driver: age refuses nothing.
        {0x10DE, L"NVIDIA GeForce RTX 2080 Ti", L"32.0.16.1664",
         GpuGeneration::Rtx20Turing, true, NeuralDriverSupport::Supported},
        {0x10DE, L"NVIDIA GeForce RTX 4080 SUPER", L"32.0.16.1047",
         GpuGeneration::Rtx40Ada, true, NeuralDriverSupport::Supported},
        {0x10DE, L"NVIDIA RTX PRO 6000 Blackwell", L"32.0.16.1664",
         GpuGeneration::OtherRtx, true, NeuralDriverSupport::Supported},
        {0x10DE, L"NVIDIA GeForce GTX 1660 SUPER", L"32.0.16.1664",
         GpuGeneration::OtherNvidia, false, NeuralDriverSupport::Supported},
        {0x8086, L"Intel(R) UHD Graphics 770", L"",
         GpuGeneration::Unsupported, false, NeuralDriverSupport::Unknown},
    };

    for (const Machine& machine : machines) {
        const GpuGeneration generation = ClassifyGpu(machine.vendorId, machine.description);
        CHECK_EQ(machine.generation, generation);
        CHECK_EQ(machine.addonDesired, NeuralAddonDesired(generation, false));
        CHECK_EQ(machine.driverSupport, ClassifyNeuralDriver(machine.driver));
    }
}

// A zero prior is a missing measurement, not a verdict that the card cannot
// render. Downstream the two would be indistinguishable if the forecast ever
// spoke from a guess: a shrunk start cushion would be a measurement the user
// never made, and a refusal would turn "nobody has timed this generation" into
// "unsupported". So the untimed generations must keep the full cushion while
// the timed ones move it.
void render_pace_prior_zero_means_unmeasured_not_unsupported_test()
{
    // The numbers themselves move as the field matrix fills in; that these
    // two generations have one at all, and the others do not, is the contract.
    for (const GpuGeneration timed : {GpuGeneration::Rtx40Ada, GpuGeneration::Rtx50Blackwell})
        CHECK(RenderPacePrior(timed) > 0.0);
    for (const GpuGeneration untimed : {GpuGeneration::Rtx20Turing, GpuGeneration::Rtx30Ampere,
                                        GpuGeneration::OtherRtx, GpuGeneration::OtherNvidia,
                                        GpuGeneration::Unsupported})
        CHECK_EQ(0.0, RenderPacePrior(untimed));

    // Same clip, same session, same empty profile: only the prior differs.
    for (const GpuGeneration untimed : {GpuGeneration::Rtx20Turing, GpuGeneration::Rtx30Ampere,
                                        GpuGeneration::OtherRtx}) {
        CHECK(NeuralAddonDesired(untimed, false));
        const auto forecast =
            playback_timing::ForecastLiveRender(1920, 1080, 30.0, {}, RenderPacePrior(untimed));
        CHECK(!forecast.measured);
        CHECK(forecast.keepsUp);
        CHECK_EQ(0.0, forecast.realtimeRatio);
        CHECK_EQ(live_session::kStartLead, live_session::StartLead(forecast.realtimeRatio));
    }
    for (const GpuGeneration timed : {GpuGeneration::Rtx40Ada, GpuGeneration::Rtx50Blackwell}) {
        const auto forecast =
            playback_timing::ForecastLiveRender(1920, 1080, 30.0, {}, RenderPacePrior(timed));
        CHECK(forecast.measured);
        CHECK(forecast.realtimeRatio > 1.5);
        CHECK(live_session::StartLead(forecast.realtimeRatio) < live_session::kStartLead);
    }
}

// A render slower than real time DRAINS the buffer while playback runs, so no
// fixed cushion prevents a rebuffer - it only sets how often one happens. The
// case is a frame-generated source: 2560x1440 at 119.88 fps measured 0.814x
// real time on an RTX 5090, where the two-second resume bought about eleven
// seconds of playback before the next stall. That is the "plays a few seconds
// then pauses" report.
void live_session_sizes_the_cushion_from_the_render_drain_test()
{
    using namespace live_session;
    // At or above real time the buffer refills faster than playback drains it,
    // so the cushion only ever shrinks - the behaviour these arms always had.
    CHECK_EQ(kStartLead, StartLead(0.0));
    CHECK_EQ(kStartLead, StartLead(1.0));
    CHECK_EQ(kResumeLead, ResumeLead(1.0));
    CHECK_EQ(kResumeLead, ResumeLead(2.0));
    CHECK(StartLead(1.6) < kStartLead);
    CHECK(StartLead(3.5) < StartLead(1.6));

    // Below it, both cushions grow, and the resume one is the one that matters:
    // the first attach happens once, a rebuffer repeats.
    const double measured = 0.814;
    const double lead = SustainedLead(measured);
    CHECK(lead > kStartLead);
    CHECK_EQ(lead, StartLead(measured));
    CHECK_EQ(lead, ResumeLead(measured));
    // It buys a sustained run rather than a round number: the drain is
    // (1 - ratio) per second, so this is what covers kSustainSeconds of play.
    CHECK(std::abs(lead - (1.0 - measured) * kSustainSeconds) < 0.01);
    // And the fill that buys it stays inside the wait budget.
    CHECK(lead / measured <= kMaxRefillWaitSeconds + 0.01);

    // A render at half speed cannot have both, so the wait budget wins and the
    // run is shorter rather than the panel being up for a minute.
    const double halfSpeed = SustainedLead(0.5);
    CHECK(std::abs(halfSpeed - kMaxRefillWaitSeconds * 0.5) < 0.01);
    CHECK(halfSpeed / 0.5 <= kMaxRefillWaitSeconds + 0.01);

    // Barely below real time needs almost nothing, and never less than the
    // cushion a card that keeps up exactly gets.
    CHECK_EQ(kStartLead, SustainedLead(0.99));
    CHECK(SustainedLead(0.2) <= kMaxLead);
    // Monotone: the slower the render, the bigger the cushion, up to the cap.
    CHECK(SustainedLead(0.9) <= SustainedLead(0.8));
    CHECK(SustainedLead(0.8) <= SustainedLead(0.7));

    // The decisions that read them move with it.
    SessionView view{};
    view.headSec = 6.0;
    view.attached = false;
    CHECK(ShouldAttach(view, StartLead(2.0)));    // fast GPU: 6 s is plenty
    CHECK(!ShouldAttach(view, StartLead(0.814))); // slow: 6 s is not the cushion
    view.attached = true;
    CHECK(ShouldResume(view, ResumeLead(2.0)));
    CHECK(!ShouldResume(view, ResumeLead(0.814)));
    // A finished job never grows again, so it resumes on whatever it has.
    view.finished = true;
    CHECK(ShouldResume(view, ResumeLead(0.814)));
}

// Ada's prior is one scalar that has to serve every geometry, and the driven
// player sessions of docs/VERIFICATION-matrix.md measured what it is actually
// standing in for on an RTX 4080 SUPER (driver 610.47, eight 1080p sessions and
// two at 2160p, idle GPU, segment-arrival method):
//   1920x1080  11.25-11.57 ms/frame  0.897-0.923x the reference model
//   3840x2160  38.69-41.20 ms/frame  1.377-1.467x
// So the one scalar is bracketed by measurement, and 1.22 sits inside the
// bracket. That is the whole reason it was not replaced by the 1080p figure:
// the two decisions below are the ones the user sees, and each end of the
// bracket gets one of them wrong.
//
// Lowering the prior towards the measured 1080p scale makes the forecast
// promise 4K30 - it would predict 25.6 ms against a measured 38.7-41.2 - and a
// session started on that promise renders at 25 fps against a 30 fps playhead.
// Raising it to the measured 4K scale makes the forecast refuse 1080p60, which
// this machine runs at 1.45x realtime. This test pins both, so a future
// "update the prior to the measurement" cannot quietly flip either.
void ada_render_pace_prior_forecasts_both_ends_of_the_measured_bracket_test()
{
    const double ada = RenderPacePrior(GpuGeneration::Rtx40Ada);

    // 4K30 must ask before it starts: the measured cost does not keep up.
    const auto uhd30 = playback_timing::ForecastLiveRender(3840, 2160, 30.0, {}, ada);
    CHECK(uhd30.measured);
    CHECK(!uhd30.keepsUp);
    // 1080p60 must not ask: the measured cost keeps up with room to spare.
    const auto fhd60 = playback_timing::ForecastLiveRender(1920, 1080, 60.0, {}, ada);
    CHECK(fhd60.measured);
    CHECK(fhd60.keepsUp);

    // The same two verdicts from the measurements themselves, so the prior is
    // checked against the machine rather than against another constant. The
    // slowest 1080p and fastest 4K samples are used: they are the samples that
    // come closest to overturning each verdict.
    playback_timing::RenderPaceProfile measured1080p;
    measured1080p.Record({1920, 1080, 11.5734});
    CHECK(playback_timing::ForecastLiveRender(1920, 1080, 60.0, measured1080p, ada).keepsUp);
    playback_timing::RenderPaceProfile measured2160p;
    measured2160p.Record({3840, 2160, 38.6853});
    CHECK(!playback_timing::ForecastLiveRender(3840, 2160, 30.0, measured2160p, ada).keepsUp);

    // One measured geometry must not be extrapolated optimistically to
    // another: a profile holding only the 1080p sample still has to predict a
    // 4K frame at no less than the 1080p cost per pixel, which is what kept a
    // one-sample profile from starting a 4K session it could not follow.
    const double uhdFrom1080p = playback_timing::PredictRenderMs(measured1080p, 3840, 2160, ada);
    CHECK(uhdFrom1080p > 4.0 * 11.5734 * 0.99);
    CHECK(!playback_timing::ForecastLiveRender(3840, 2160, 30.0, measured1080p, ada).keepsUp);
}

// The panel is half of every answer here, which is what separates this policy
// from "double anything under 45 fps". Each row below is a case where a
// source-only rule and this one disagree, or where a plausible refactor would
// start generating frames that land worse on the display than the source's own
// rate does - or would refuse frames that land better, which is the mistake
// this suite was written around and now records.
//
// Every row is planned against frame_rate_policy::kPhaseVerifiedMultiFrameCount
// rather than a literal cap. That constant is a measurement of where the
// generated frames actually land - it has already moved once, from one
// generated frame to five, when the phase probe was changed from a flat white
// box (no interior detail to localise, so the generated frame measures as a
// blend near the midpoint) to a textured patch - so the suite has to follow it
// instead of restating today's value. Where a case is about the RUNTIME
// bounding the plan, DLSSG.MultiFrameCountMax is passed explicitly, because
// there the argument is the point.
void frame_generation_plan_follows_the_panel_not_just_the_source_test()
{
    using namespace frame_rate_policy;
    const auto plan = [](double fps, double refresh,
                         uint32_t max = kPhaseVerifiedMultiFrameCount,
                         bool evenOnly = false) {
        return PlanFrameGeneration(SourceCadence{fps, false, true}, refresh, max, evenOnly);
    };

    // 24 fps film on a 120 Hz panel: 5x lands exactly on the refresh, every
    // frame is scanned out once and the 3:2 pulldown disappears. This is the
    // largest win the policy has and the reason the ceiling matters: while the
    // phase-verified count was one generated frame, this case planned nothing
    // at all and the viewer kept the pulldown. 6x, which the runtime would also
    // admit, overshoots the panel at 144 and is correctly passed over - the cap
    // is not the target, the refresh is.
    const auto film120 = plan(24.0, 120.0);
    CHECK(film120.Generates());
    CHECK_EQ(5u, film120.multiplier);
    CHECK_EQ(4u, film120.generatedPerSource);
    CHECK_EQ(120.0, film120.targetFps);
    CHECK_EQ(1u, film120.presentsPerFrame);
    CHECK(film120.cadence.even);

    // The same source on a 60 Hz panel DOUBLES, where it used to be refused
    // for the claim that generating there "would trade one uneven cadence for
    // another". These two spreads are the measurement that refutes it: 24 fps
    // is held 2 or 3 scan-outs (33/50 ms, 3:2 pulldown) and 48 fps is held 1 or
    // 2 (17/33 ms), so the unevenness is one refresh period in BOTH cases -
    // that is all it can ever be - while the step between presented frames
    // halves. Refusing cost the viewer that halving and bought nothing.
    const auto film60 = plan(24.0, 60.0);
    CHECK(film60.Generates());
    CHECK_EQ(2u, film60.multiplier);
    CHECK_EQ(48.0, film60.targetFps);
    CHECK(!film60.cadence.even);
    CHECK_EQ(film60.sourceCadence.spreadMs, film60.cadence.spreadMs);
    CHECK(film60.cadence.stepMs < film60.sourceCadence.stepMs * 0.51);
    CHECK_EQ(2u, film60.sourceCadence.shortHold);
    CHECK_EQ(3u, film60.sourceCadence.longHold);
    CHECK_EQ(1u, film60.cadence.shortHold);
    CHECK_EQ(2u, film60.cadence.longHold);

    // The even-cadence-only setting restores the old behaviour for exactly
    // that case, and names itself: it is the one refusal here a user can lift.
    CHECK(!plan(24.0, 60.0, kPhaseVerifiedMultiFrameCount, true).Generates());
    CHECK(plan(24.0, 60.0, kPhaseVerifiedMultiFrameCount, true).refusal ==
          FrameGenerationRefusal::EvenCadenceRequired);
    // And it cannot change a case that already lands evenly: 30 -> 60 on 60 Hz
    // is one scan-out per frame with the setting on or off.
    CHECK_EQ(2u, plan(30.0, 60.0, kPhaseVerifiedMultiFrameCount, true).multiplier);

    // 30 fps on 120 Hz: 4x reaches the refresh exactly. 5x and 6x overshoot.
    const auto thirty120 = plan(30.0, 120.0);
    CHECK_EQ(4u, thirty120.multiplier);
    CHECK_EQ(3u, thirty120.generatedPerSource);
    CHECK_EQ(120.0, thirty120.targetFps);
    CHECK_EQ(1u, thirty120.presentsPerFrame);
    CHECK(thirty120.sourceCadence.even);
    CHECK(thirty120.cadence.even);

    // The other side of the admission rule, and the one a "more frames is
    // always better" refactor breaks: this source ALREADY lands evenly - 4
    // scan-outs per frame, zero spread - so a multiple that does not divide the
    // refresh must lose even though it is the higher rate. A runtime admitting
    // two generated frames could reach 90 fps here (8/17 ms holds, a fresh
    // unevenness where there was none); it takes 60 instead.
    const auto thirty120Capped = plan(30.0, 120.0, 2);
    CHECK_EQ(2u, thirty120Capped.multiplier);
    CHECK_EQ(60.0, thirty120Capped.targetFps);
    CHECK(thirty120Capped.cadence.even);
    CHECK_EQ(0.0, thirty120Capped.cadence.spreadMs);

    // 30 -> 60 on a 60 Hz panel: the doubling case, one scan-out per frame.
    const auto thirty60 = plan(30.0, 60.0);
    CHECK_EQ(2u, thirty60.multiplier);
    CHECK_EQ(1u, thirty60.generatedPerSource);
    CHECK_EQ(60.0, thirty60.targetFps);
    CHECK_EQ(1u, thirty60.presentsPerFrame);

    // 60 -> 120 on a 120 Hz panel is the same shape one octave up.
    const auto sixty120 = plan(60.0, 120.0);
    CHECK_EQ(2u, sixty120.multiplier);
    CHECK_EQ(120.0, sixty120.targetFps);
    CHECK_EQ(1u, sixty120.presentsPerFrame);

    // 60 on a 60 Hz panel names the reason a viewer can act on (get a faster
    // panel) rather than a cadence argument, which would be true and useless.
    CHECK(!plan(60.0, 60.0).Generates());
    CHECK(plan(60.0, 60.0).refusal == FrameGenerationRefusal::SourceMeetsRefresh);

    // The NTSC family against a refresh Windows reports as a whole number:
    // 24000/1001 x 5 is 119.88 against a mode called 120, which is inside
    // kRateTolerance (0.005, and 120/119.88 is 1.001 of a scan-out) and must
    // not be read as uneven. The plan carries the source's own rate times the
    // multiplier, not a rounded 120 - the conversion is driven at this rate and
    // a rounded one would drift against the audio.
    const double ntsc = 24000.0 / 1001.0;
    const auto ntsc120 = plan(ntsc, 120.0);
    CHECK_EQ(5u, ntsc120.multiplier);
    CHECK_EQ(ntsc * 5.0, ntsc120.targetFps);
    CHECK(ntsc120.targetFps < 120.0);
    CHECK(120.0 - ntsc120.targetFps < 120.0 * kRateTolerance);
    CHECK_EQ(1u, ntsc120.presentsPerFrame);
    CHECK(ntsc120.cadence.even);

    // A runtime that admits fewer frames than the phase measurement verified
    // bounds the plan: the player takes min(runtime max, phase-verified max).
    // A machine reporting one generated frame loses the 24 -> 120 case, and
    // this is the trade the policy still refuses - the source lands evenly at 5
    // scan-outs and 2x = 48 does not divide 120, so generating would introduce
    // an unevenness the viewer does not have. It says which one it is.
    CHECK(!plan(24.0, 120.0, 1).Generates());
    CHECK(plan(24.0, 120.0, 1).refusal == FrameGenerationRefusal::SourceCadenceEven);
    // The doubling case survives that same runtime, because it only ever needed
    // one generated frame.
    CHECK_EQ(2u, plan(30.0, 60.0, 1).multiplier);
    // And a runtime that admits nothing is a refusal with a reason, not a
    // silent 1x that looks like a policy decision.
    CHECK(plan(30.0, 60.0, 0).refusal == FrameGenerationRefusal::RuntimeRefused);

    // PAL on 60 Hz: no multiple of 25 divides 60, and it doubles anyway. 25 fps
    // is held 2 or 3 scan-outs, 50 fps is held 1 or 2, and the step goes from
    // 40 ms to 20 ms for the same one-period spread.
    const auto pal60 = plan(25.0, 60.0);
    CHECK_EQ(2u, pal60.multiplier);
    CHECK_EQ(50.0, pal60.targetFps);
    CHECK(!pal60.cadence.even);
    CHECK_EQ(pal60.sourceCadence.spreadMs, pal60.cadence.spreadMs);

    // 144 Hz divides neither 60 nor 30, which is not a reason to leave the
    // source where it is: 60 -> 120 and 30 -> 120 land in the same grid 60 and
    // 30 already land in on this panel, two and four times as often.
    const auto sixty144 = plan(60.0, 144.0);
    CHECK_EQ(2u, sixty144.multiplier);
    CHECK_EQ(120.0, sixty144.targetFps);
    CHECK(!sixty144.cadence.even);
    CHECK_EQ(sixty144.sourceCadence.spreadMs, sixty144.cadence.spreadMs);
    CHECK_EQ(4u, plan(30.0, 144.0).multiplier);
    CHECK_EQ(120.0, plan(30.0, 144.0).targetFps);

    // A panel that cannot even double the source says so. 40 fps needs 80 and
    // this panel scans out 60, so no multiple exists at all - a different fact
    // from "no multiple divides the refresh", which is what it used to report.
    CHECK(!plan(40.0, 60.0).Generates());
    CHECK(plan(40.0, 60.0).refusal == FrameGenerationRefusal::RefreshBelowDouble);

    // A panel well above the source: the target does not have to reach the
    // refresh. Whatever multiple the search settles on for 30 fps on 240 Hz,
    // targetFps is the source rate times that multiple and each generated frame
    // occupies a whole number of scan-outs, because the source does too.
    const auto thirty240 = plan(30.0, 240.0);
    CHECK(thirty240.Generates());
    CHECK_EQ(30.0 * double(thirty240.multiplier), thirty240.targetFps);
    CHECK_EQ(240.0, thirty240.targetFps * double(thirty240.presentsPerFrame));
    CHECK(thirty240.cadence.even);

    // Sources with nothing to interpolate, each naming itself and generating
    // nothing: the refusal is the answer the status line shows.
    const auto still = PlanFrameGeneration(SourceCadence{0.0, true, true}, 60.0,
                                           kPhaseVerifiedMultiFrameCount);
    CHECK(still.refusal == FrameGenerationRefusal::StillImage);
    CHECK(!still.Generates());
    CHECK_EQ(0u, still.generatedPerSource);
    const auto unknownRate = PlanFrameGeneration(SourceCadence{0.0, false, true}, 60.0,
                                                 kPhaseVerifiedMultiFrameCount);
    CHECK(unknownRate.refusal == FrameGenerationRefusal::UnknownSourceRate);
    CHECK(!unknownRate.Generates());
    CHECK_EQ(0u, unknownRate.generatedPerSource);
    const auto variable = PlanFrameGeneration(SourceCadence{30.0, false, false}, 60.0,
                                              kPhaseVerifiedMultiFrameCount);
    CHECK(variable.refusal == FrameGenerationRefusal::VariableFrameRate);
    CHECK(!variable.Generates());
    CHECK_EQ(0u, variable.generatedPerSource);
    const auto unknownRefresh = plan(30.0, 0.0);
    CHECK(unknownRefresh.refusal == FrameGenerationRefusal::UnknownRefresh);
    CHECK(!unknownRefresh.Generates());
    CHECK_EQ(0u, unknownRefresh.generatedPerSource);

    CHECK(FrameGenerationRefusalName(FrameGenerationRefusal::RefreshBelowDouble) ==
          "refresh-below-double");
    CHECK(FrameGenerationRefusalName(FrameGenerationRefusal::SourceCadenceEven) ==
          "source-cadence-even");
    CHECK(FrameGenerationRefusalName(FrameGenerationRefusal::EvenCadenceRequired) ==
          "even-cadence-required");
}

// The two terms the policy trades, measured on their own. The equality below is
// the whole reason 24 fps doubles on a 60 Hz panel: a presentation grid is
// uneven by one refresh period or not at all, because a frame can only be held
// for floor or ceil of refresh/rate scan-outs. A higher rate therefore cannot
// be "more uneven" than a lower one on the same display, only finer-stepped.
void display_cadence_spread_is_one_refresh_period_or_nothing_test()
{
    using namespace frame_rate_policy;
    const auto film = CadenceOf(24.0, 60.0);
    CHECK(!film.even);
    CHECK_EQ(2u, film.shortHold);
    CHECK_EQ(3u, film.longHold);
    CHECK(std::abs(film.spreadMs - 1000.0 / 60.0) < 1e-9);
    CHECK(std::abs(film.stepMs - 1000.0 / 24.0) < 1e-9);

    const auto doubled = CadenceOf(48.0, 60.0);
    CHECK(!doubled.even);
    CHECK_EQ(1u, doubled.shortHold);
    CHECK_EQ(2u, doubled.longHold);
    CHECK_EQ(film.spreadMs, doubled.spreadMs);
    CHECK(doubled.stepMs < film.stepMs);

    // Even is even at any hold length, and carries no spread at all.
    const auto even = CadenceOf(30.0, 120.0);
    CHECK(even.even);
    CHECK_EQ(4u, even.shortHold);
    CHECK_EQ(4u, even.longHold);
    CHECK_EQ(0.0, even.spreadMs);

    // NTSC against a mode Windows calls 120: inside kRateTolerance, so this is
    // one scan-out per frame rather than a 1.001-wide unevenness.
    const auto ntsc = CadenceOf(24000.0 / 1001.0 * 5.0, 120.0);
    CHECK(ntsc.even);
    CHECK_EQ(1u, ntsc.shortHold);
    CHECK_EQ(0.0, ntsc.spreadMs);

    // Nothing to measure is not an even cadence.
    CHECK(!CadenceOf(0.0, 120.0).even);
    CHECK(!CadenceOf(24.0, 0.0).even);
}

// The display-side answer. Every refusal above is about a grid the player does
// not own - but the monitor usually offers another one, and switching to it is
// the only move that removes an unevenness instead of reducing it.
void refresh_switch_offer_names_the_mode_that_removes_the_pulldown_test()
{
    using namespace frame_rate_policy;
    const SourceCadence film{24.0, false, true};
    const double modes[] = {60.0, 120.0, 144.0};

    // On 60 Hz the film doubles to 48 and keeps its pulldown; the same
    // monitor's 120 Hz mode takes it to 120 exactly at 5x. 144 Hz is in the
    // list and loses: it only reaches 72 evenly.
    const auto from60 = BetterRefreshForSource(film, modes, 60.0, kPhaseVerifiedMultiFrameCount);
    CHECK(from60.Offered());
    CHECK_EQ(120.0, from60.refreshHz);
    CHECK_EQ(5u, from60.multiplier);
    CHECK_EQ(120.0, from60.targetFps);

    // Already on the best mode: nothing to offer, and the player must not nag.
    CHECK(!BetterRefreshForSource(film, modes, 120.0, kPhaseVerifiedMultiFrameCount).Offered());

    // The offer survives the even-cadence-only setting, because switching the
    // display is precisely how that setting gets what it asks for.
    CHECK(BetterRefreshForSource(film, modes, 60.0, kPhaseVerifiedMultiFrameCount, true).Offered());

    // The other case a mode change fixes: a source that meets its panel
    // generates nothing at all on 60 Hz, and doubles on 120.
    const SourceCadence sixty{60.0, false, true};
    const auto faster = BetterRefreshForSource(sixty, modes, 60.0, kPhaseVerifiedMultiFrameCount);
    CHECK(faster.Offered());
    CHECK_EQ(120.0, faster.refreshHz);
    CHECK_EQ(2u, faster.multiplier);
    CHECK_EQ(120.0, faster.targetFps);

    // A tie in presented rate goes to the lower refresh: 30 fps reaches 120
    // evenly on a 120 Hz mode and on a 240 Hz one, and 120 asks less of the
    // panel for the same result.
    const SourceCadence thirty{30.0, false, true};
    const double highModes[] = {60.0, 120.0, 240.0};
    const auto tie = BetterRefreshForSource(thirty, highModes, 60.0, kPhaseVerifiedMultiFrameCount);
    CHECK_EQ(120.0, tie.refreshHz);
    CHECK_EQ(120.0, tie.targetFps);

    // No alternative mode is not an offer, and neither is a list holding only
    // the mode the display is already in.
    CHECK(!BetterRefreshForSource(film, {}, 60.0, kPhaseVerifiedMultiFrameCount).Offered());
    const double only60[] = {60.0};
    CHECK(!BetterRefreshForSource(film, only60, 60.0, kPhaseVerifiedMultiFrameCount).Offered());

    // A runtime that admits nothing has no offer to make either: the mode
    // change would not buy a conversion.
    CHECK(!BetterRefreshForSource(film, modes, 60.0, 0).Offered());
}

// The ceiling above is a measurement, and measurements move: it was one
// generated frame while the phase probe was a featureless box and is five now
// that a textured probe localised the generated frames inside the source
// interval. So one case pins the RELATIONSHIP instead of the number. A source
// running at refresh/m is exactly the case m-times generation exists for - it
// lands on the refresh with one scan-out per frame - so every multiplier the
// constant admits has to be reachable, and nothing beyond it may be.
void frame_generation_reaches_every_multiplier_the_verified_ceiling_admits_test()
{
    using namespace frame_rate_policy;
    // 120 Hz is used because it is a real mode that every multiplier from 2
    // upwards divides into a plausible source rate (60, 40, 30, 24, 20).
    constexpr double kRefresh = 120.0;
    for (uint32_t multiplier = 2; multiplier <= kPhaseVerifiedMultiFrameCount + 1; ++multiplier) {
        const double fps = kRefresh / double(multiplier);
        const auto planned = PlanFrameGeneration(SourceCadence{fps, false, true}, kRefresh,
                                                 kPhaseVerifiedMultiFrameCount);
        CHECK(planned.refusal == FrameGenerationRefusal::None);
        CHECK_EQ(multiplier, planned.multiplier);
        CHECK_EQ(multiplier - 1u, planned.generatedPerSource);
        CHECK_EQ(1u, planned.presentsPerFrame);
        CHECK(std::abs(planned.targetFps - kRefresh) <= kRefresh * kRateTolerance);
    }

    // And the ceiling is a ceiling. The source that would need one multiple
    // more than the constant admits must not get it: either the search finds a
    // smaller admissible multiple that still divides the refresh, or it
    // refuses, but the multiplier never exceeds the verified count plus the
    // source frame itself, and the target never overshoots the panel.
    const double beyond = kRefresh / double(kPhaseVerifiedMultiFrameCount + 2);
    const auto bounded = PlanFrameGeneration(SourceCadence{beyond, false, true}, kRefresh,
                                             kPhaseVerifiedMultiFrameCount);
    CHECK(bounded.multiplier <= kPhaseVerifiedMultiFrameCount + 1);
    CHECK(bounded.targetFps <= kRefresh * (1.0 + kRateTolerance));
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

void neural_playback_lifecycle_accepts_its_generation_and_reaches_ready_test()
{
    NeuralPlaybackLifecycle lifecycle;const uint64_t generation=lifecycle.Begin();
    CHECK(lifecycle.Accept(generation));CHECK(lifecycle.Transition(NeuralPlaybackState::Ready));
}

void neural_playback_lifecycle_runs_render_validate_then_ready_test()
{
    NeuralPlaybackLifecycle lifecycle;lifecycle.Begin();
    CHECK(lifecycle.Transition(NeuralPlaybackState::Rendering));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Validating));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Ready));
}

void neural_completion_publishes_only_after_probe_and_manifest_validation_test()
{
    CHECK(CanPublishNeuralCompletion(true,true,true));
    CHECK(!CanPublishNeuralCompletion(false,true,true));
    CHECK(!CanPublishNeuralCompletion(true,false,true));
    CHECK(!CanPublishNeuralCompletion(true,true,false));
}

void neural_publish_tolerance_admits_one_muxer_rounding_per_joined_segment_test()
{
    // The refused session: 2607 frames of 30 fps video published as 44
    // separately muxed segments and then joined, so the joined file may sit
    // one 1 ms Matroska rounding per part away from the rendered duration.
    const int64_t rounding=10000;
    const int64_t expected=869000000;
    const int64_t drift=44*rounding;
    const int64_t joined=JoinedMediaDurationTolerance100ns(30.0,44);
    const int64_t single=JoinedMediaDurationTolerance100ns(30.0,1);
    const bool durationsMatch=NeuralPublishDurationsMatch(expected+drift,expected,expected,joined);
    CHECK(durationsMatch);
    CHECK(!NeuralPublishDurationsMatch(expected+drift,expected,expected,single));
    CHECK_EQ(joined-single,43*rounding);
    // Drift beyond the roundings and the one allowed frame is still a defect,
    // on whichever side it shows up.
    CHECK(!NeuralPublishDurationsMatch(expected+joined+1,expected,expected,joined));
    CHECK(!NeuralPublishDurationsMatch(expected,expected+joined+1,expected,joined));
    // A lost frame is never publishable, however well the durations agree.
    const uint64_t probeFrames=2606,resultFrames=2607;
    CHECK(!CanPublishNeuralCompletion(true,durationsMatch&&probeFrames==resultFrames,true));
    // An unreadable frame rate must not collapse the tolerance to nothing.
    CHECK(JoinedMediaDurationTolerance100ns(0.0,44)>=drift);
    CHECK(JoinedMediaDurationTolerance100ns(std::numeric_limits<double>::infinity(),0)>=rounding);
}

void render_range_residual_below_one_frame_is_coverage_not_work_test()
{
    // The re-toggled session: a head accumulated from integer per-frame
    // segment ends against a range end taken from the probed source duration.
    const int64_t head=947333000;
    const int64_t frame=333334;
    CHECK(RenderRangeIsCovered(head,head,30.0));
    CHECK(RenderRangeIsCovered(head,head+1,30.0));
    CHECK(RenderRangeIsCovered(head,head+frame-1,30.0));
    CHECK(!RenderRangeIsCovered(head,head+frame,30.0));
    CHECK(!RenderRangeIsCovered(head,head+30*frame,30.0));
    // Without a frame rate a residual of unknown length stays work.
    CHECK(!RenderRangeIsCovered(head,head+1,0.0));
    CHECK(RenderRangeIsCovered(head+1,head,0.0));
}

// 100ns source timestamps: the units the segment index, the renderer and the
// playback clock all speak. One second, and one frame of the 30 fps clip the
// backward-seek bug was reported against.
constexpr int64_t kSecond100ns = 10000000;
constexpr int64_t kFrame100ns = 333333;

// CoverageSpan has no equality operator - nothing in the player compares two
// spans - so the tests name the endpoints they mean.
void check_span(const CoverageSpan& span, int64_t start100ns, int64_t end100ns)
{
    CHECK_EQ(start100ns, span.start100ns);
    CHECK_EQ(end100ns, span.end100ns);
}

void coverage_merge_sorts_drops_degenerate_and_joins_touching_spans_test()
{
    // A run publishes out of order relative to earlier runs, retries overlap
    // what they redo, and a cancelled job can leave an empty span behind.
    const std::vector<CoverageSpan> merged = MergeSpans({
        {40 * kSecond100ns, 50 * kSecond100ns},
        {10 * kSecond100ns, 20 * kSecond100ns},
        {20 * kSecond100ns, 30 * kSecond100ns},
        {5 * kSecond100ns, 5 * kSecond100ns},
        {9 * kSecond100ns, 3 * kSecond100ns},
        {25 * kSecond100ns, 35 * kSecond100ns},
    });
    CHECK_EQ(size_t{2}, merged.size());
    if (merged.size() == 2) {
        check_span(merged[0], 10 * kSecond100ns, 35 * kSecond100ns);
        check_span(merged[1], 40 * kSecond100ns, 50 * kSecond100ns);
    }

    // Two runs that meet exactly are one rendered region: the first job's last
    // frame and the second job's first are consecutive frames of the video, so
    // a render boundary must never be reported as a hole.
    const std::vector<CoverageSpan> touching =
        MergeSpans({{0, 10 * kSecond100ns}, {10 * kSecond100ns, 20 * kSecond100ns}});
    CHECK_EQ(size_t{1}, touching.size());
    if (touching.size() == 1) check_span(touching[0], 0, 20 * kSecond100ns);

    // One tick short of touching is a real gap and must survive as two spans,
    // otherwise an unrendered frame would be claimed as playable.
    const std::vector<CoverageSpan> gapped =
        MergeSpans({{0, 10 * kSecond100ns}, {10 * kSecond100ns + 1, 20 * kSecond100ns}});
    CHECK_EQ(size_t{2}, gapped.size());
    if (gapped.size() == 2) {
        check_span(gapped[0], 0, 10 * kSecond100ns);
        check_span(gapped[1], 10 * kSecond100ns + 1, 20 * kSecond100ns);
    }
}

void uncovered_spans_of_nothing_is_everything_and_of_everything_is_nothing_test()
{
    const CoverageSpan range{0, 104 * kSecond100ns};

    const std::vector<CoverageSpan> fresh = UncoveredSpans({}, range, kFrame100ns);
    CHECK_EQ(size_t{1}, fresh.size());
    if (fresh.size() == 1) check_span(fresh[0], range.start100ns, range.end100ns);

    CHECK(UncoveredSpans({{0, 104 * kSecond100ns}}, range, kFrame100ns).empty());

    // Two runs that meet leave nothing to render between them.
    CHECK(UncoveredSpans({{0, 52 * kSecond100ns}, {52 * kSecond100ns, 104 * kSecond100ns}}, range,
                         kFrame100ns)
              .empty());
}

void uncovered_spans_find_the_hole_between_regions_and_the_lead_in_before_the_first_test()
{
    // The reported session: the user let it render from 20s, seeked back, and
    // the opening twenty seconds were never rendered at all. Both the lead-in
    // and the tail are work, and the lead-in is the one that used to be lost.
    const CoverageSpan range{0, 104 * kSecond100ns};
    const std::vector<CoverageSpan> holes =
        UncoveredSpans({{20 * kSecond100ns, 60 * kSecond100ns}}, range, kFrame100ns);
    CHECK_EQ(size_t{2}, holes.size());
    if (holes.size() == 2) {
        check_span(holes[0], 0, 20 * kSecond100ns);
        check_span(holes[1], 60 * kSecond100ns, 104 * kSecond100ns);
    }

    // The mirror image: the opening and the tail rendered, the middle not.
    const std::vector<CoverageSpan> between = UncoveredSpans(
        {{60 * kSecond100ns, 104 * kSecond100ns}, {0, 20 * kSecond100ns}}, range, kFrame100ns);
    CHECK_EQ(size_t{1}, between.size());
    if (between.size() == 1) check_span(between[0], 20 * kSecond100ns, 60 * kSecond100ns);
}

void uncovered_spans_clip_coverage_to_the_range_and_ignore_coverage_outside_it_test()
{
    // A range narrower than the coverage: a trimmed export, or a session
    // measuring what is left of the clip from a point the user seeked to.
    const CoverageSpan range{30 * kSecond100ns, 90 * kSecond100ns};

    const std::vector<CoverageSpan> leading =
        UncoveredSpans({{10 * kSecond100ns, 40 * kSecond100ns}}, range, kFrame100ns);
    CHECK_EQ(size_t{1}, leading.size());
    if (leading.size() == 1) check_span(leading[0], 40 * kSecond100ns, 90 * kSecond100ns);

    const std::vector<CoverageSpan> trailing =
        UncoveredSpans({{80 * kSecond100ns, 120 * kSecond100ns}}, range, kFrame100ns);
    CHECK_EQ(size_t{1}, trailing.size());
    if (trailing.size() == 1) check_span(trailing[0], 30 * kSecond100ns, 80 * kSecond100ns);

    const std::vector<CoverageSpan> both = UncoveredSpans(
        {{10 * kSecond100ns, 40 * kSecond100ns}, {80 * kSecond100ns, 120 * kSecond100ns}}, range,
        kFrame100ns);
    CHECK_EQ(size_t{1}, both.size());
    if (both.size() == 1) check_span(both[0], 40 * kSecond100ns, 80 * kSecond100ns);

    // Coverage that never meets the range buys nothing inside it.
    const std::vector<CoverageSpan> outside = UncoveredSpans(
        {{0, 5 * kSecond100ns}, {100 * kSecond100ns, 110 * kSecond100ns}}, range, kFrame100ns);
    CHECK_EQ(size_t{1}, outside.size());
    if (outside.size() == 1) check_span(outside[0], 30 * kSecond100ns, 90 * kSecond100ns);
}

void uncovered_spans_drop_sub_frame_holes_but_keep_a_hole_one_frame_wide_test()
{
    // Integer per-frame segment ends against a fractional frame rate leave a
    // few ticks of residual that no job can render: asking for less than a
    // frame earns a range refusal from the worker. A hole exactly one frame
    // wide is a frame the user would watch unrendered, so it is work.
    const CoverageSpan range{0, 10 * kSecond100ns};
    const std::vector<CoverageSpan> covered{
        {0, 3 * kSecond100ns},
        {3 * kSecond100ns + kFrame100ns - 1, 6 * kSecond100ns},
        {6 * kSecond100ns + kFrame100ns, 10 * kSecond100ns},
    };

    const std::vector<CoverageSpan> holes = UncoveredSpans(covered, range, kFrame100ns);
    CHECK_EQ(size_t{1}, holes.size());
    if (holes.size() == 1)
        check_span(holes[0], 6 * kSecond100ns, 6 * kSecond100ns + kFrame100ns);

    // The sliver is only dropped by the width filter, not by the arithmetic.
    CHECK_EQ(size_t{2}, UncoveredSpans(covered, range, 0).size());

    // The same residual at the end of the range, which is where the re-toggled
    // session found it.
    CHECK(UncoveredSpans({{0, 10 * kSecond100ns - (kFrame100ns - 1)}}, range, kFrame100ns).empty());
    const std::vector<CoverageSpan> tail =
        UncoveredSpans({{0, 10 * kSecond100ns - kFrame100ns}}, range, kFrame100ns);
    CHECK_EQ(size_t{1}, tail.size());
    if (tail.size() == 1)
        check_span(tail[0], 10 * kSecond100ns - kFrame100ns, 10 * kSecond100ns);
}

void next_render_target_clips_the_hole_under_the_playhead_to_the_playhead_test()
{
    // The user is waiting on this frame. Rendering the seconds they already
    // passed first would make them wait for all of it before anything appears.
    const std::vector<CoverageSpan> holes{{0, 20 * kSecond100ns},
                                          {60 * kSecond100ns, 104 * kSecond100ns}};

    const std::optional<CoverageSpan> inside = NextRenderTarget(holes, 8 * kSecond100ns);
    CHECK(inside.has_value());
    if (inside) check_span(*inside, 8 * kSecond100ns, 20 * kSecond100ns);

    // At the first tick of a hole the whole hole is the target.
    const std::optional<CoverageSpan> atStart = NextRenderTarget(holes, 60 * kSecond100ns);
    CHECK(atStart.has_value());
    if (atStart) check_span(*atStart, 60 * kSecond100ns, 104 * kSecond100ns);

    // A hole is half-open: its end timestamp is rendered, so the playhead
    // sitting exactly there is not inside it and the target is the next hole.
    const std::optional<CoverageSpan> atEnd = NextRenderTarget(holes, 20 * kSecond100ns);
    CHECK(atEnd.has_value());
    if (atEnd) check_span(*atEnd, 60 * kSecond100ns, 104 * kSecond100ns);
}

void next_render_target_prefers_the_nearest_hole_ahead_then_the_earliest_behind_test()
{
    const std::vector<CoverageSpan> holes{{0, 20 * kSecond100ns},
                                          {40 * kSecond100ns, 50 * kSecond100ns},
                                          {80 * kSecond100ns, 104 * kSecond100ns}};

    // Playhead on rendered video: playback is about to arrive at the nearest
    // hole ahead, so that one is rendered before either of the others.
    const std::optional<CoverageSpan> ahead = NextRenderTarget(holes, 30 * kSecond100ns);
    CHECK(ahead.has_value());
    if (ahead) check_span(*ahead, 40 * kSecond100ns, 50 * kSecond100ns);

    const std::optional<CoverageSpan> next = NextRenderTarget(holes, 60 * kSecond100ns);
    CHECK(next.has_value());
    if (next) check_span(*next, 80 * kSecond100ns, 104 * kSecond100ns);

    // Nothing left ahead: the earliest hole behind is taken, not the latest.
    // This is how a session started mid-video eventually renders its opening.
    const std::optional<CoverageSpan> behind = NextRenderTarget(holes, 104 * kSecond100ns);
    CHECK(behind.has_value());
    if (behind) check_span(*behind, 0, 20 * kSecond100ns);

    const std::optional<CoverageSpan> onlyBehind =
        NextRenderTarget({{0, 20 * kSecond100ns}, {40 * kSecond100ns, 50 * kSecond100ns}},
                         55 * kSecond100ns);
    CHECK(onlyBehind.has_value());
    if (onlyBehind) check_span(*onlyBehind, 0, 20 * kSecond100ns);
}

void next_render_target_without_a_hole_has_nothing_to_render_test()
{
    CHECK(!NextRenderTarget({}, 12 * kSecond100ns).has_value());

    // Degenerate holes are not work either: a job asked to render an empty
    // range is refused, and a session that kept asking would never idle.
    CHECK(!NextRenderTarget({{5 * kSecond100ns, 5 * kSecond100ns},
                             {9 * kSecond100ns, 3 * kSecond100ns}},
                            12 * kSecond100ns)
               .has_value());
}

void span_containing_returns_the_playable_region_not_a_later_disjoint_one_test()
{
    // Two rendered regions with a hole between them, the first one built from
    // two runs that met exactly.
    const std::vector<CoverageSpan> covered{{20 * kSecond100ns, 40 * kSecond100ns},
                                            {40 * kSecond100ns, 60 * kSecond100ns},
                                            {80 * kSecond100ns, 104 * kSecond100ns}};

    const std::optional<CoverageSpan> playing = SpanContaining(covered, 30 * kSecond100ns);
    CHECK(playing.has_value());
    if (playing) check_span(*playing, 20 * kSecond100ns, 60 * kSecond100ns);

    const std::optional<CoverageSpan> later = SpanContaining(covered, 90 * kSecond100ns);
    CHECK(later.has_value());
    if (later) check_span(*later, 80 * kSecond100ns, 104 * kSecond100ns);

    // Inside the hole there is no playable buffer. The later region is not
    // lead: playback cannot reach it without crossing unrendered video, and a
    // session measuring its buffer against the newest rendered timestamp would
    // attach with nothing to show.
    CHECK(!SpanContaining(covered, 70 * kSecond100ns).has_value());
    // Before the first region, and at the exclusive end of a region.
    CHECK(!SpanContaining(covered, 10 * kSecond100ns).has_value());
    CHECK(!SpanContaining(covered, 60 * kSecond100ns).has_value());
    CHECK(!SpanContaining(covered, 104 * kSecond100ns).has_value());
    // The first and last rendered ticks are inside.
    CHECK(SpanContaining(covered, 20 * kSecond100ns).has_value());
    CHECK(SpanContaining(covered, 104 * kSecond100ns - 1).has_value());
}

void covered_duration_counts_only_rendered_video_inside_the_range_test()
{
    // The render pace is measured against this, so counting a hole as work
    // done would report a session as faster than real time when it is not.
    const CoverageSpan range{0, 104 * kSecond100ns};
    const std::vector<CoverageSpan> covered{{20 * kSecond100ns, 60 * kSecond100ns},
                                            {30 * kSecond100ns, 50 * kSecond100ns},
                                            {80 * kSecond100ns, 104 * kSecond100ns},
                                            {110 * kSecond100ns, 120 * kSecond100ns}};
    CHECK_EQ(64 * kSecond100ns, CoveredDuration100ns(covered, range));
    CHECK(std::abs(CoveredFraction(covered, range) - 64.0 / 104.0) < 1e-12);

    // Coverage wider than the range contributes only the overlap, and a range
    // entirely inside one rendered region is finished.
    CHECK_EQ(10 * kSecond100ns, CoveredDuration100ns({{0, 100 * kSecond100ns}},
                                                     {10 * kSecond100ns, 20 * kSecond100ns}));
    CHECK_EQ(1.0, CoveredFraction({{0, 100 * kSecond100ns}},
                                  {10 * kSecond100ns, 20 * kSecond100ns}));
    // Two runs that meet cover the range exactly once, not twice.
    CHECK_EQ(1.0, CoveredFraction({{0, 52 * kSecond100ns}, {52 * kSecond100ns, 104 * kSecond100ns}},
                                  range));

    // Nothing rendered, and a range with nothing in it.
    CHECK_EQ(int64_t{0}, CoveredDuration100ns({}, range));
    CHECK_EQ(0.0, CoveredFraction({}, range));
    CHECK_EQ(int64_t{0}, CoveredDuration100ns(covered, {50 * kSecond100ns, 50 * kSecond100ns}));
    CHECK_EQ(0.0, CoveredFraction(covered, {50 * kSecond100ns, 50 * kSecond100ns}));
    CHECK_EQ(0.0, CoveredFraction(covered, {60 * kSecond100ns, 40 * kSecond100ns}));
}

void neural_cancel_and_failure_offer_original_only_without_partial_cache_test()
{
    NeuralPlaybackLifecycle lifecycle;const uint64_t generation=lifecycle.Begin();
    CHECK(lifecycle.Transition(NeuralPlaybackState::Rendering));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Cancelling));
    CHECK(lifecycle.Transition(NeuralPlaybackState::OriginalOnly));
    lifecycle.Invalidate();CHECK(!lifecycle.Accept(generation));
    lifecycle.Begin();CHECK(lifecycle.Transition(NeuralPlaybackState::Failed));
    CHECK(lifecycle.Transition(NeuralPlaybackState::OriginalOnly));
}

void neural_pause_suspends_rendering_and_resumes_without_advancing_test()
{
    NeuralPlaybackLifecycle lifecycle;lifecycle.Begin();
    CHECK(!lifecycle.Transition(NeuralPlaybackState::Paused));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Rendering));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Paused));
    CHECK(!lifecycle.Transition(NeuralPlaybackState::Validating));
    CHECK(!lifecycle.Transition(NeuralPlaybackState::Ready));
    CHECK_EQ(NeuralPlaybackState::Paused,lifecycle.state);
    CHECK(lifecycle.Transition(NeuralPlaybackState::Rendering));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Paused));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Cancelling));
    CHECK(lifecycle.Transition(NeuralPlaybackState::OriginalOnly));
}

void neural_recovery_resolves_to_rendering_failed_or_retry_exhausted_test()
{
    NeuralPlaybackLifecycle lifecycle;lifecycle.Begin();
    CHECK(lifecycle.Transition(NeuralPlaybackState::Recovering));
    CHECK(!lifecycle.Transition(NeuralPlaybackState::Ready));
    CHECK(!lifecycle.Transition(NeuralPlaybackState::Paused));
    CHECK(!lifecycle.Transition(NeuralPlaybackState::Validating));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Rendering));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Recovering));
    CHECK(lifecycle.Transition(NeuralPlaybackState::RetryExhausted));
    CHECK(!lifecycle.Transition(NeuralPlaybackState::Rendering));
    CHECK(!lifecycle.Transition(NeuralPlaybackState::Recovering));
    CHECK(lifecycle.Transition(NeuralPlaybackState::OriginalOnly));
    lifecycle.Begin();
    CHECK(lifecycle.Transition(NeuralPlaybackState::Rendering));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Recovering));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Failed));
    lifecycle.Begin();
    CHECK(lifecycle.Transition(NeuralPlaybackState::Rendering));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Recovering));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Cancelling));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Idle));
    CHECK(!lifecycle.Transition(NeuralPlaybackState::RetryExhausted));
    CHECK(!lifecycle.Transition(NeuralPlaybackState::Recovering));
}

void neural_failure_kind_selects_the_lifecycle_state_test()
{
    CHECK_EQ(NeuralPlaybackState::RetryExhausted,StateForFailure(NeuralRenderFailure::RetryExhausted));
    CHECK_EQ(NeuralPlaybackState::OriginalOnly,StateForFailure(NeuralRenderFailure::Cancelled));
    CHECK_EQ(NeuralPlaybackState::Failed,StateForFailure(NeuralRenderFailure::DeviceRemoved));
    CHECK_EQ(NeuralPlaybackState::Failed,StateForFailure(NeuralRenderFailure::Preflight));
    CHECK_EQ(NeuralPlaybackState::Failed,StateForFailure(NeuralRenderFailure::WorkerCrashed));
    NeuralPlaybackLifecycle lifecycle;lifecycle.Begin();
    CHECK(lifecycle.Transition(NeuralPlaybackState::Rendering));
    CHECK(lifecycle.Transition(NeuralPlaybackState::Recovering));
    CHECK(lifecycle.Transition(StateForFailure(NeuralRenderFailure::RetryExhausted)));
}

void neural_progress_phase_drives_the_lifecycle_through_pause_and_recovery_test()
{
    // Phases without a state of their own never move the job.
    CHECK_EQ(NeuralPlaybackState::Acquiring,StateForProgressPhase(NeuralRenderPhase::CheckingCache,NeuralPlaybackState::Acquiring));
    CHECK_EQ(NeuralPlaybackState::Validating,StateForProgressPhase(NeuralRenderPhase::Ready,NeuralPlaybackState::Validating));
    CHECK_EQ(NeuralPlaybackState::Acquiring,StateForProgressPhase(NeuralRenderPhase::Preflight,NeuralPlaybackState::Acquiring));
    CHECK_EQ(NeuralPlaybackState::Validating,StateForProgressPhase(NeuralRenderPhase::Validating,NeuralPlaybackState::Rendering));
    // A worker walking through preflight, decode, pause, resume, a frame retry and
    // encode lands in exactly the states the UI presents.
    NeuralPlaybackLifecycle lifecycle;lifecycle.Begin();
    const auto advance=[&](NeuralRenderPhase phase){return lifecycle.Transition(StateForProgressPhase(phase,lifecycle.state));};
    CHECK(!advance(NeuralRenderPhase::Preflight));
    CHECK_EQ(NeuralPlaybackState::Acquiring,lifecycle.state);
    CHECK(advance(NeuralRenderPhase::Decoding));
    CHECK_EQ(NeuralPlaybackState::Rendering,lifecycle.state);
    CHECK(advance(NeuralRenderPhase::Paused));
    CHECK_EQ(NeuralPlaybackState::Paused,lifecycle.state);
    CHECK(advance(NeuralRenderPhase::NeuralRendering));
    CHECK_EQ(NeuralPlaybackState::Rendering,lifecycle.state);
    CHECK(advance(NeuralRenderPhase::Recovering));
    CHECK_EQ(NeuralPlaybackState::Recovering,lifecycle.state);
    // A relaunched helper re-acquires while the job is still recovering.
    CHECK(!advance(NeuralRenderPhase::Acquiring));
    CHECK_EQ(NeuralPlaybackState::Recovering,lifecycle.state);
    CHECK(advance(NeuralRenderPhase::Encoding));
    CHECK_EQ(NeuralPlaybackState::Rendering,lifecycle.state);
    CHECK(advance(NeuralRenderPhase::Validating));
    CHECK(!advance(NeuralRenderPhase::Ready));
    CHECK_EQ(NeuralPlaybackState::Validating,lifecycle.state);
    CHECK(lifecycle.Transition(NeuralPlaybackState::Ready));
}

void dlss_toggle_in_cached_playback_changes_comparison_view_not_renderer_feature_test()
{
    CHECK_EQ(ComparisonView::Neural,ToggleComparisonView(ComparisonView::Original));
    CHECK_EQ(ComparisonView::Original,ToggleComparisonView(ComparisonView::Neural));
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
    CHECK(failed.userArguments.empty());
    CHECK(!failed.error.empty());
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
        L"\"--safe-mode\"";
    CHECK_EQ(std::wstring(expectedCommandLine), BuildWindowsCommandLine(executable, safeMode));
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

    std::wstring embeddedNul = L"https://youtube.com/watch?v=dQw4w9WgXcQ";
    embeddedNul.push_back(L'\0');
    embeddedNul.append(L"dQw4w9WgXcQ");
    rejected.push_back(std::move(embeddedNul));

    for (const std::wstring& value : rejected) {
        CHECK(!IsSupportedYouTubeUrl(value));
    }
}

void youtube_watch_query_requires_one_unambiguous_lowercase_v_field_test()
{
    // Every id below is a valid eleven-character one, so only the field rules
    // can be what rejects these.
    const std::array rejected{
        L"https://youtube.com/watch?v=dQw4w9WgXcQ&v=dQw4w9WgXcQ",
        L"https://youtube.com/watch?v=dQw4w9WgXcQ&v=dQw4w9WgXcR",
        L"https://youtube.com/watch?v=&v=dQw4w9WgXcQ",
        L"https://youtube.com/watch?v=dQw4w9WgXcQ&v=",
        L"https://youtube.com/watch?V=dQw4w9WgXcQ",
        L"https://youtube.com/watch?V=dQw4w9WgXcQ&v=dQw4w9WgXcR",
        L"https://youtube.com/watch?%76=dQw4w9WgXcQ",
        L"https://youtube.com/watch?%56=dQw4w9WgXcQ&v=dQw4w9WgXcR",
        L"https://youtube.com/watch?v%3DdQw4w9WgXcQ",
        L"https://youtube.com/watch?v=dQw4w9WgXcQ%26v%3DdQw4w9WgXcR",
        L"https://youtube.com/watch?v=dQw4w9WgXcQ%26list%3DPL123",
    };

    for (const std::wstring_view value : rejected) {
        CHECK(!IsSupportedYouTubeUrl(value));
    }
}

void youtube_video_id_must_be_exactly_eleven_characters_test()
{
    // The alphabet was always enforced; the length is part of the format too,
    // and a recent-history entry that is not eleven characters is not an id.
    for (const auto* route : {L"https://youtube.com/watch?v=", L"https://youtu.be/", L"https://youtube.com/shorts/"}) {
        CHECK(IsSupportedYouTubeUrl(std::wstring(route) + L"dQw4w9WgXcQ"));
        CHECK(IsSupportedYouTubeUrl(std::wstring(route) + L"-_A1b2C3d4E"));
        CHECK(!IsSupportedYouTubeUrl(std::wstring(route) + L"dQw4w9WgXc"));
        CHECK(!IsSupportedYouTubeUrl(std::wstring(route) + L"dQw4w9WgXcQQ"));
        CHECK(!IsSupportedYouTubeUrl(std::wstring(route) + std::wstring(2000, L'a')));
        CHECK(CanonicalYouTubeVideoId(std::wstring(route) + L"dQw4w9WgXc").empty());
        CHECK_EQ(std::string("dQw4w9WgXcQ"), CanonicalYouTubeVideoId(std::wstring(route) + L"dQw4w9WgXcQ"));
    }
}

void youtube_url_validation_enforces_exact_2048_character_boundary_test()
{
    // Padding goes into a playlist field the watch route ignores, so the id
    // itself stays eleven characters and only the total length is at issue.
    constexpr std::wstring_view prefix = L"https://youtube.com/watch?v=dQw4w9WgXcQ&list=";
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
        CHECK_EQ(0.0, result.durationSeconds);
        CHECK(result.detail.empty());
        CHECK(result.mediaUrl.find(L'\r') == std::wstring::npos);
        CHECK(result.mediaUrl.find(L'\n') == std::wstring::npos);
    }
}

void resolver_output_accepts_separate_https_video_and_audio_urls_test()
{
    const ResolveResult result = ParseResolverOutput(
        "https://v1.googlevideo.com/videoplayback?id=video&itag=137&expire=999\r\n"
        "https://a1.googlevideo.com/videoplayback?id=audio&itag=140&expire=999\r\n", 0);
    CHECK(result.ok);
    CHECK_EQ(std::wstring(L"https://v1.googlevideo.com/videoplayback?id=video&itag=137&expire=999"),
             result.mediaUrl);
    CHECK_EQ(std::wstring(L"https://a1.googlevideo.com/videoplayback?id=audio&itag=140&expire=999"),
             result.audioUrl);
    CHECK_EQ(std::string("video-itag=137|audio-itag=140"),
             StableYouTubeStreamIdentity(result.mediaUrl,result.audioUrl));
    CHECK_EQ(StableYouTubeStreamIdentity(result.mediaUrl,result.audioUrl),
             StableYouTubeStreamIdentity(
                 L"https://v2.googlevideo.com/videoplayback?expire=123&itag=137",
                 L"https://a2.googlevideo.com/videoplayback?expire=456&itag=140"));
    CHECK(StableYouTubeStreamIdentity(
        L"https://v1.googlevideo.com/videoplayback?id=video",
        L"https://a1.googlevideo.com/videoplayback?id=audio").empty());
    constexpr auto audio = L"https://a.googlevideo.com/videoplayback?itag=140";
    for(const auto* hls : {
        L"https://manifest.googlevideo.com/api/manifest/hls_playlist/expire/123/itag/270/signature/one/file/index.m3u8",
        L"https://manifest.googlevideo.com/api/manifest/hls_playlist/expire/456/itag/270/signature/two/file/index.m3u8"}) {
        CHECK_EQ(std::string("video-itag=270|audio-itag=140"),StableYouTubeStreamIdentity(hls,audio));
    }
    for(const auto* invalid : {
        L"https://manifest.googlevideo.com/api/manifest/hls_playlist/itag/not-a-number/file/index.m3u8",
        L"https://manifest.googlevideo.com/api/manifest/hls_playlist/itag/270/itag/271/file/index.m3u8",
        L"https://manifest.googlevideo.com/api/manifest/hls_playlist/itag//file/index.m3u8",
        L"https://manifest.googlevideo.com/api/manifest/hls_playlist/itag/123456789/file/index.m3u8",
        L"https://manifest.googlevideo.com/unrelated/itag/270/file/index.m3u8",
        L"https://googlevideo.com.example.invalid/api/manifest/hls_playlist/itag/270/file/index.m3u8"}) {
        CHECK(StableYouTubeStreamIdentity(invalid,audio).empty());
    }
    CHECK_EQ(std::string("9lrThxCoznw"),
             CanonicalYouTubeVideoId(L"https://www.youtube.com/watch?v=9lrThxCoznw"));
    CHECK_EQ(std::string("9lrThxCoznw"),
             CanonicalYouTubeVideoId(L"https://youtu.be/9lrThxCoznw"));
}

void resolver_output_accepts_authoritative_duration_before_stream_urls_test()
{
    const std::array accepted{
        std::pair{"duration=167;live_status=not_live\nhttps://v.googlevideo.com/video\n", 167.0},
        std::pair{"duration=167.25;live_status=not_live\r\nhttps://v.googlevideo.com/video\r\nhttps://a.googlevideo.com/audio\r\n", 167.25},
        std::pair{"duration=2592000;live_status=was_live\nhttps://v.googlevideo.com/video", 2592000.0},
    };
    for (const auto& [output, expectedDuration] : accepted) {
        const auto result = ParseResolverOutput(output, 0);
        CHECK(result.ok);
        CHECK_EQ(ResolveError::None, result.error);
        CHECK_EQ(std::wstring(L"https://v.googlevideo.com/video"), result.mediaUrl);
        CHECK_EQ(expectedDuration, result.durationSeconds);
    }
}

void resolver_output_rejects_invalid_duration_live_status_and_metadata_framing_test()
{
    const std::array rejectedMetadata{
        "duration=;live_status=not_live", "duration=NA;live_status=not_live",
        "duration=0;live_status=not_live", "duration=-1;live_status=not_live",
        "duration=nan;live_status=not_live", "duration=inf;live_status=not_live",
        "duration=1e309;live_status=not_live", "duration=2592001;live_status=not_live",
        "duration=167seconds;live_status=not_live", "duration=167 ;live_status=not_live",
        "duration= 167;live_status=not_live", "duration=+167;live_status=not_live",
        "duration=167;live_status=is_live", "duration=167;live_status=is_upcoming",
        "duration=167;live_status=post_live", "duration=167;live_status=NA",
        "duration=167;live_status=not_live;extra=1", "duration=167",
        "duration=167;live_status=not_live\nduration=167;live_status=not_live",
        "duration=167;live_status=not_live\n", "duration=167;live_status=not_live\r\r",
    };
    for (const auto metadata : rejectedMetadata) {
        const auto result = ParseResolverOutput(
            std::string(metadata) + "\nhttps://v.googlevideo.com/video\n", 0);
        CHECK(!result.ok);
        CHECK_EQ(ResolveError::InvalidOutput, result.error);
        CHECK(result.mediaUrl.empty());
        CHECK(result.audioUrl.empty());
        CHECK_EQ(0.0, result.durationSeconds);
        CHECK(result.detail.find(L"http") == std::wstring::npos);
        CHECK(result.detail.find(L"live_status") == std::wstring::npos);
    }
    CHECK(!ParseResolverOutput("duration=167;live_status=not_live\n", 0).ok);
    CHECK(!ParseResolverOutput(
        "duration=167;live_status=not_live\nhttps://untrusted.example/video\n", 0).ok);
    CHECK(!ParseResolverOutput(
        "https://v.googlevideo.com/video\nduration=167;live_status=not_live\n", 0).ok);
    CHECK(!ParseResolverOutput(
        "duration=167;live_status=not_live\nhttps://v.googlevideo.com/video\n\n", 0).ok);
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
        L"bv*[height=1080]+ba/b[height=1080]",
        L"--format-sort-force",
        L"-S",
        L"height,vbr,abr",
        L"--get-url",
        L"--print",
        L"duration=%(duration)s;live_status=%(live_status)s;video_available_at=%(requested_formats.0.available_at,available_at|0)s;audio_available_at=%(requested_formats.1.available_at|0)s;selected_height=%(height)s;video_kbps=%(requested_formats.0.vbr,vbr,tbr|0)s;age_limit=%(age_limit)s",
        std::wstring(url),
    };

    CHECK_EQ(expected, BuildYouTubeResolverArguments(
                           helperDirectory, url, YouTubeSourceQuality::P1080));
}

void youtube_source_quality_selectors_pin_exact_rungs_and_cap_auto_at_1440_test()
{
    const std::array cases{
        // Auto climbs to the tallest rung at or below 1440p and only falls through
        // to 2160p when a video publishes nothing shorter.
        std::pair{YouTubeSourceQuality::Auto,
                  std::wstring_view(L"bv*[height<=1440]+ba/b[height<=1440]/bv*[height<=2160]+ba/b[height<=2160]")},
        std::pair{YouTubeSourceQuality::P2160,
                  std::wstring_view(L"bv*[height=2160]+ba/b[height=2160]")},
        std::pair{YouTubeSourceQuality::P1440,
                  std::wstring_view(L"bv*[height=1440]+ba/b[height=1440]")},
        std::pair{YouTubeSourceQuality::P1080,
                  std::wstring_view(L"bv*[height=1080]+ba/b[height=1080]")},
    };
    for (const auto& [quality, expected] : cases) {
        CHECK_EQ(expected, YouTubeFormatSelector(quality));
    }
}

void resolver_metadata_reports_selected_height_video_bitrate_and_age_limit_test()
{
    const ResolveResult restricted = ParseResolverOutput(
        "duration=167;live_status=not_live;video_available_at=0;audio_available_at=0"
        ";selected_height=1080;video_kbps=3898.893;age_limit=18"
        "\nhttps://v.googlevideo.com/video\nhttps://a.googlevideo.com/audio\n", 0);
    CHECK(restricted.ok);
    CHECK_EQ(167.0, restricted.durationSeconds);
    CHECK_EQ(1080, restricted.selectedHeight);
    CHECK_EQ(3898.893, restricted.videoKbps);
    CHECK_EQ(18, restricted.ageLimit);

    // yt-dlp prints NA for any field it could not fill, and these three only
    // describe what was picked. The stream URLs are what playback needs, so an
    // unusable metric reads as zero instead of failing a usable resolve.
    for (const auto* metrics : {";selected_height=NA;video_kbps=NA;age_limit=NA",
                                ";selected_height=;video_kbps=;age_limit=",
                                ";selected_height=-1;video_kbps=-1;age_limit=-1"}) {
        const ResolveResult result = ParseResolverOutput(
            std::string("duration=167;live_status=not_live") + metrics +
            "\nhttps://v.googlevideo.com/video\n", 0);
        CHECK(result.ok);
        CHECK_EQ(167.0, result.durationSeconds);
        CHECK_EQ(0, result.selectedHeight);
        CHECK_EQ(0.0, result.videoKbps);
        CHECK_EQ(0, result.ageLimit);
    }

    const ResolveResult withoutMetrics = ParseResolverOutput(
        "duration=167;live_status=not_live;video_available_at=1788425760;audio_available_at=0"
        "\nhttps://v.googlevideo.com/video\n", 0);
    CHECK(withoutMetrics.ok);
    CHECK_EQ(int64_t{1788425760}, withoutMetrics.availableAtUnixSeconds);
    CHECK_EQ(0, withoutMetrics.selectedHeight);
    CHECK_EQ(0.0, withoutMetrics.videoKbps);
    CHECK_EQ(0, withoutMetrics.ageLimit);
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

// A child that has just been terminated keeps its image file mapped for a short
// while after the process count drops, so deleting the directory it was copied
// into fails with a sharing violation for a few milliseconds. Retry briefly
// instead of failing the test that already proved what it set out to prove.
void remove_fixture_directory(const std::filesystem::path& directory)
{
    std::error_code error;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    do {
        error.clear();
        std::filesystem::remove_all(directory, error);
        if (!error) return;
        Sleep(10);
    } while (std::chrono::steady_clock::now() < deadline);
    CHECK(!error);
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
        remove_fixture_directory(directory);
    }
};

void youtube_bitrate_selection_uses_real_helper_without_network_test()
{
    const auto helpers = current_test_executable().parent_path();
    const auto executable = helpers / L"yt-dlp.exe";
    if (!std::filesystem::is_regular_file(executable)) {
        std::cout << "SKIP: bitrate selection requires the staged yt-dlp helper.\n";
        return;
    }
    ResolverFixture fixture;
    const auto metadata = fixture.directory / L"formats.info.json";
    const auto selected = fixture.directory / L"selected.txt";
    const std::string audio = R"({"format_id":"140","url":"https://example.invalid/aac","ext":"m4a","vcodec":"none","acodec":"mp4a.40.2","abr":128},
        {"format_id":"251","url":"https://example.invalid/opus","ext":"webm","vcodec":"none","acodec":"opus","abr":160})";
    const std::string p1080 = R"({"format_id":"av1","url":"https://example.invalid/av1","ext":"mp4","height":1080,"width":1920,"fps":60,"vcodec":"av01","acodec":"none","vbr":1555,"quality":10,"preference":10},
        {"format_id":"avc","url":"https://example.invalid/avc","ext":"mp4","height":1080,"width":1920,"fps":30,"vcodec":"avc1","acodec":"none","vbr":4000},
        {"format_id":"vp9","url":"https://example.invalid/vp9","ext":"webm","height":1080,"width":1920,"fps":30,"vcodec":"vp9","acodec":"none","vbr":6000},
        {"format_id":"unknown","url":"https://example.invalid/unknown","ext":"mp4","height":1080,"width":1920,"fps":60,"vcodec":"av01","acodec":"none","quality":99,"preference":99})";
    const std::string others = R"({"format_id":"720","url":"https://example.invalid/720","ext":"mp4","height":720,"width":1280,"vcodec":"avc1","acodec":"none","vbr":20000},
        {"format_id":"1440av1","url":"https://example.invalid/1440av1","ext":"mp4","height":1440,"width":2560,"vcodec":"av01","acodec":"none","vbr":5000},
        {"format_id":"1440avc","url":"https://example.invalid/1440avc","ext":"mp4","height":1440,"width":2560,"vcodec":"avc1","acodec":"none","vbr":8000},
        {"format_id":"2160av1","url":"https://example.invalid/2160av1","ext":"mp4","height":2160,"width":3840,"vcodec":"av01","acodec":"none","vbr":7000},
        {"format_id":"2160vp9","url":"https://example.invalid/2160vp9","ext":"webm","height":2160,"width":3840,"vcodec":"vp9","acodec":"none","vbr":12000})";
    const std::string ultraOnly = R"({"format_id":"2160av1","url":"https://example.invalid/2160av1","ext":"mp4","height":2160,"width":3840,"vcodec":"av01","acodec":"none","vbr":7000},
        {"format_id":"2160vp9","url":"https://example.invalid/2160vp9","ext":"webm","height":2160,"width":3840,"vcodec":"vp9","acodec":"none","vbr":12000})";
    auto checkSelection = [&](YouTubeSourceQuality quality, const std::string& video,
                              std::string_view expected) {
        write_binary_file(metadata, "{\"id\":\"fixture\",\"title\":\"Offline formats\",\"duration\":10,\"extractor\":\"youtube\",\"formats\":[" + audio + "," + video + "]}");
        std::filesystem::remove(selected);
        auto args = BuildYouTubeResolverArguments(helpers, L"https://youtu.be/fixture", quality);
        // Keep production selection arguments; substitute an offline catalog for
        // the network URL and print only the selected format IDs.
        args.erase(std::find(args.begin(), args.end(), L"--get-url"), args.end());
        args.insert(args.end(), {L"--simulate", L"--no-check-formats", L"--quiet",
            L"--load-info-json", metadata.wstring(), L"--print-to-file",
            L"%(format_id)s", selected.wstring()});
        std::wstring command = QuoteWindowsArgument(executable.wstring());
        for (const auto& arg : args) command += L" " + QuoteWindowsArgument(arg);
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        const bool started = CreateProcessW(executable.c_str(), command.data(), nullptr,
            nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
        CHECK(started);
        if (!started) return;
        CloseHandle(process.hThread);
        const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
        CHECK_EQ(DWORD{WAIT_OBJECT_0}, wait);
        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 2000);
        }
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess);
        if (expected.empty()) { CHECK(code != 0); return; }
        CHECK_EQ(DWORD{0}, code);
        std::ifstream input(selected, std::ios::binary);
        std::string actual;
        std::getline(input, actual);
        if (!actual.empty() && actual.back() == '\r') actual.pop_back();
        if (actual != expected) std::cerr << "Selected " << actual << "; expected " << expected << '\n';
        CHECK_EQ(expected, actual);
    };
    checkSelection(YouTubeSourceQuality::P1080, p1080 + "," + others, "vp9+251");
    checkSelection(YouTubeSourceQuality::P1440, p1080 + "," + others, "1440avc+251");
    checkSelection(YouTubeSourceQuality::P2160, p1080 + "," + others, "2160vp9+251");
    // Auto stops at the 1440p rung even when 2160p is published, and takes the
    // fattest stream inside that rung rather than the first codec listed.
    checkSelection(YouTubeSourceQuality::Auto, p1080 + "," + others, "1440avc+251");
    checkSelection(YouTubeSourceQuality::Auto, others, "1440avc+251");
    // Nothing at or below 1440p: the second rung keeps 4K-only videos playable.
    checkSelection(YouTubeSourceQuality::Auto, ultraOnly, "2160vp9+251");
    checkSelection(YouTubeSourceQuality::P1080, others, "");
    checkSelection(YouTubeSourceQuality::P1080, p1080 + R"(,
        {"format_id":"combined","url":"https://example.invalid/combined","ext":"mp4","height":1080,"width":1920,"vcodec":"avc1","acodec":"mp4a.40.2","vbr":7000,"abr":128})", "combined");
}

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
    ~MediaFixture(){remove_fixture_directory(directory);}
};

void video_decoder_prefers_video_duration_tag_over_longer_container_test()
{
    MediaFixture fixture;
    const std::array cases{
        std::pair{L"durationtag_valid", 1.0},
        std::pair{L"durationtag_fractional", 3723.25},
        std::pair{L"durationtag_missing", 3.0},
        std::pair{L"durationtag_badminute", 3.0},
        std::pair{L"durationtag_badsecond", 3.0},
        std::pair{L"durationtag_negative", 3.0},
        std::pair{L"durationtag_nonfinite", 3.0},
        std::pair{L"durationtag_trailing", 3.0},
        std::pair{L"durationtag_overflow", 3.0},
        std::pair{L"durationtag_zero", 3.0},
        std::pair{L"durationtag_mp4", 1.25},
    };
    for (const auto& [scenario, expectedDuration] : cases) {
        auto decoder = VideoDecoderTestAccess::Create(fixture.directory);
        CHECK(decoder->OpenSequential(std::wstring(L"https://media.invalid/") + scenario,
                                      MediaSourceKind::YouTube));
        CHECK_EQ(expectedDuration, decoder->DurationSeconds());
        decoder->Close();
    }
}

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
        while(result==VideoReadResult::NotReady&&std::chrono::steady_clock::now()<deadline){result=decoder->ReadNextAvailable(frame);Sleep(5);}
        CHECK_EQ(VideoReadResult::FrameReady,result);CHECK_EQ(size_t{16},frame.bgra.size());
        const auto closeStarted=std::chrono::steady_clock::now();decoder->Close();CHECK(std::chrono::steady_clock::now()-closeStarted<std::chrono::seconds{1});
    }
    {
        // Non-blocking means a read never waits for the child. This child writes
        // a quarter of a frame and then nothing, and the stall timeout is set far
        // beyond the bound: a read that waited for the frame or for the stall
        // would take those 5 s, while a poll answers NotReady at once. The bound
        // is a full second because a loaded runner can hold a thread off the CPU
        // for tens of milliseconds, which the 40 ms this used to allow did not
        // survive - but never for a second.
        auto decoder=VideoDecoderTestAccess::Create(fixture.directory,std::chrono::milliseconds{250},std::chrono::seconds{5});
        CHECK(decoder->Open(L"https://media.invalid/stallmid",MediaSourceKind::YouTube));
        VideoFrame frame;
        const auto callStarted=std::chrono::steady_clock::now();
        const VideoReadResult result=decoder->ReadNextAvailable(frame);
        CHECK(std::chrono::steady_clock::now()-callStarted<std::chrono::seconds{1});
        CHECK_EQ(VideoReadResult::NotReady,result);
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
        while(result==VideoReadResult::NotReady&&std::chrono::steady_clock::now()<deadline){result=decoder->ReadNextAvailable(frame);Sleep(5);}
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
        while(result==VideoReadResult::NotReady&&std::chrono::steady_clock::now()<deadline){result=decoder->ReadNextAvailable(frame);Sleep(5);}
        CHECK_EQ(VideoReadResult::FrameReady,result);CHECK(frame.timestamp100ns>=120000000);
    }
    {
        auto decoder=VideoDecoderTestAccess::Create(fixture.directory,std::chrono::milliseconds{250},std::chrono::seconds{5});CHECK(decoder->Open(L"https://media.invalid/hold",MediaSourceKind::YouTube));CHECK(decoder->SeekSeconds(9.0));
        const auto started=std::chrono::steady_clock::now();std::jthread worker([&](std::stop_token stop){VideoFrame frame;while(decoder->ReadNextAvailable(frame,stop)==VideoReadResult::NotReady)Sleep(5);});Sleep(30);worker.request_stop();worker.join();decoder->Close();CHECK(std::chrono::steady_clock::now()-started<std::chrono::seconds{1});
    }
}

// Regression: StopFrameQueue used to reset m_frameTerminal back to NotReady after
// joining the queue thread, and never notified. The queue thread exits on its own stop
// token without publishing a terminal state, so a reader parked in ReadNextBlocking
// waited forever on a predicate that could not become true again. ReadNext runs on the
// UI message pump for local files, so that wedged the whole window.
void video_decoder_close_releases_a_blocked_blocking_read_test()
{
    MediaFixture fixture;
    auto decoder=VideoDecoderTestAccess::Create(fixture.directory,std::chrono::milliseconds{250},std::chrono::seconds{5});
    // "/hold" never emits a frame, so the reader is guaranteed to park on the queue.
    CHECK(decoder->Open(L"https://media.invalid/hold",MediaSourceKind::YouTube));
    VideoFrame frameStorage;
    VideoReadResult result=VideoReadResult::NotReady;
    // A default-constructed stop token has no stop state, which is exactly how the UI
    // path calls in: only the decoder shutdown can release this reader.
    std::jthread reader([&]{result=decoder->ReadNextBlocking(frameStorage);});
    Sleep(50);
    const auto closeStarted=std::chrono::steady_clock::now();
    decoder->Close();
    reader.join();
    CHECK(std::chrono::steady_clock::now()-closeStarted<std::chrono::seconds{1});
    CHECK_EQ(VideoReadResult::Cancelled,result);
}

void video_decoder_hardware_failure_falls_back_to_software_test()
{
    MediaFixture fixture;
    const auto marker=fixture.directory/L"acceleration-order.txt";
    ScopedEnvironmentVariable markerVariable(L"DLSS_VIDEO_TEST_ACCEL_MARKER",marker.wstring());
    auto decoder=VideoDecoderTestAccess::Create(fixture.directory,std::chrono::seconds{2},std::chrono::seconds{2});
    CHECK(decoder->Open(L"https://media.invalid/hardwarefallbackdelayedexit",MediaSourceKind::YouTube));
    VideoFrame frame;VideoReadResult result=VideoReadResult::NotReady;
    const auto started=std::chrono::steady_clock::now();
    const auto deadline=started+std::chrono::seconds{5};
    while(result==VideoReadResult::NotReady&&std::chrono::steady_clock::now()<deadline){result=decoder->ReadNextAvailable(frame);Sleep(5);}
    const std::string accelerationOrder=read_binary_file(marker);
    if(result!=VideoReadResult::FrameReady){
        const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
        std::cerr<<"hardware fallback diagnostic: result="<<static_cast<int>(result)
                 <<" elapsed_ms="<<elapsed<<" marker="<<accelerationOrder<<'\n';
    }
    CHECK_EQ(VideoReadResult::FrameReady,result);CHECK_EQ(size_t{16},frame.bgra.size());
    CHECK_EQ(std::string("cuda\nd3d11va\nsoftware\n"),accelerationOrder);
}

// A failed hardware path stays failed: the next decoder in the same process
// launches software directly, which is what keeps a seek to one process start.
void video_decoder_remembers_dead_hardware_paths_test()
{
    MediaFixture fixture;
    {
        const auto first=fixture.directory/L"first-order.txt";
        ScopedEnvironmentVariable markerVariable(L"DLSS_VIDEO_TEST_ACCEL_MARKER",first.wstring());
        auto decoder=VideoDecoderTestAccess::Create(fixture.directory,std::chrono::seconds{2},std::chrono::seconds{2});
        CHECK(decoder->Open(L"https://media.invalid/hardwarefallbackdelayedexit",MediaSourceKind::YouTube));
        VideoFrame frame;VideoReadResult result=VideoReadResult::NotReady;
        for(const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{5};
            result==VideoReadResult::NotReady&&std::chrono::steady_clock::now()<deadline;)
            {result=decoder->ReadNextAvailable(frame);Sleep(5);}
        CHECK_EQ(VideoReadResult::FrameReady,result);
        CHECK_EQ(std::string("cuda\nd3d11va\nsoftware\n"),read_binary_file(first));
    }
    // Same process, new decoder, no reset: the two dead paths are skipped.
    const auto second=fixture.directory/L"second-order.txt";
    ScopedEnvironmentVariable markerVariable(L"DLSS_VIDEO_TEST_ACCEL_MARKER",second.wstring());
    auto decoder=VideoDecoderTestAccess::Create(fixture.directory,std::chrono::seconds{2},std::chrono::seconds{2},
                                                VideoDecoder::FailureStage::None,false);
    CHECK(decoder->Open(L"https://media.invalid/hardwarefallbackdelayedexit",MediaSourceKind::YouTube));
    VideoFrame frame;VideoReadResult result=VideoReadResult::NotReady;
    for(const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{5};
        result==VideoReadResult::NotReady&&std::chrono::steady_clock::now()<deadline;)
        {result=decoder->ReadNextAvailable(frame);Sleep(5);}
    CHECK_EQ(VideoReadResult::FrameReady,result);
    CHECK_EQ(std::string("software\n"),read_binary_file(second));
}

void video_decoder_drains_complete_raw_frame_buffered_after_child_exit_test()
{
    MediaFixture fixture;
    const auto marker=fixture.directory/L"sequential-acceleration.txt";
    ScopedEnvironmentVariable markerVariable(L"DLSS_VIDEO_TEST_ACCEL_MARKER",marker.wstring());
    auto decoder=VideoDecoderTestAccess::Create(fixture.directory);
    CHECK(decoder->OpenSequential(L"drainexit",MediaSourceKind::LocalFile));
    VideoFrame frame;VideoReadResult result=VideoReadResult::NotReady;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{2};
    while(result==VideoReadResult::NotReady&&std::chrono::steady_clock::now()<deadline){
        result=decoder->ReadNextAvailable(frame);Sleep(1);
    }
    CHECK_EQ(VideoReadResult::FrameReady,result);
    // 1920x1080 is even, so OpenSequential picks NV12 (w*h*3/2 bytes, not w*h*4):
    // 11.1 MB BGRA -> 4.2 MB NV12 per 2578x1080 frame is the same saving at this size.
    CHECK(decoder->PixelLayout()==VideoPixelLayout::Nv12);
    CHECK(frame.layout==VideoPixelLayout::Nv12);
    CHECK_EQ(size_t{1920u*1080u*3u/2u},frame.bgra.size());
    // OpenSequential now requests CUDA decode (NVDEC is a separate engine from
    // NVENC/D3D12 and the GPU is idle during export), so the fake ffmpeg child
    // launches with -hwaccel cuda.
    CHECK_EQ(std::string("cuda\n"),read_binary_file(marker));
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
    // Four queued frames plus the two the stdout pipe is sized to hold, so the
    // child can decode one frame while the reader copies the previous one out.
    CHECK(produced>=4);CHECK(produced<=7);
}

// The fake child stamps every frame with its absolute source index, so a seek
// that keeps the running child can be held to the exact frame a restarting seek
// hands out - the property the whole optimisation rests on.
uint32_t stamped_frame_index(const VideoFrame& frame)
{
    CHECK_EQ(size_t{16},frame.bgra.size());
    uint32_t index=0;
    for(int byte=0;byte<4;++byte)index|=static_cast<uint32_t>(frame.bgra[static_cast<size_t>(byte)])<<(8*byte);
    return index;
}

VideoFrame read_one_frame(VideoDecoder& decoder)
{
    VideoFrame frame;VideoReadResult result=VideoReadResult::NotReady;
    for(const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{5};
        std::chrono::steady_clock::now()<deadline;){
        result=decoder.ReadNextAvailable(frame);
        if(result!=VideoReadResult::NotReady)break;
        Sleep(1);
    }
    CHECK_EQ(VideoReadResult::FrameReady,result);
    return frame;
}

// Regression guard for the LocalFile queue thread's blocking ReadFile: once the fake
// child parks in Sleep(INFINITE) with the pipe empty, the queue thread has nothing left
// to peek-sleep-poll for and sits inside the kernel read instead. StopFrameQueue has to
// unpark it with CancelSynchronousIo (rather than wait on a child that never exits), so
// Close must still return promptly.
void video_decoder_close_returns_promptly_when_local_queue_thread_is_blocked_on_pipe_read_test()
{
    MediaFixture fixture;
    auto decoder=VideoDecoderTestAccess::Create(fixture.directory);
    CHECK(decoder->OpenSequential(L"largeburst",MediaSourceKind::LocalFile));
    // 1024x1024 is even, so this OpenSequential open picks NV12: the fake child
    // writes exactly 20 frames of 1024x1024 NV12 (w*h*3/2 bytes) and then parks.
    // Every one of them has to be consumed here: with any still unread the queue
    // thread would be waiting for queue space, not inside the kernel read this
    // test is about.
    CHECK(decoder->PixelLayout()==VideoPixelLayout::Nv12);
    for(int index=0;index<20;++index)CHECK_EQ(size_t{1024u*1024u*3u/2u},read_one_frame(*decoder).bgra.size());
    // Give the queue thread a chance to settle into the blocking read for data that
    // will never arrive.
    Sleep(75);
    const auto closeStarted=std::chrono::steady_clock::now();
    decoder->Close();
    const double elapsedMs=
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-closeStarted).count();
    CHECK(elapsedMs<2000.0);
}

// NV12 needs even plane dimensions (the UV plane is half-resolution in both
// axes), so OpenSequential's NV12 preference is conditional on the probed
// geometry: even geometry gets NV12, odd geometry falls back to BGRA exactly
// like normal playback.
void video_decoder_open_sequential_selects_nv12_for_even_geometry_test()
{
    MediaFixture fixture;
    auto decoder=VideoDecoderTestAccess::Create(fixture.directory);
    CHECK(decoder->OpenSequential(L"nv12geom",MediaSourceKind::LocalFile));
    CHECK(decoder->PixelLayout()==VideoPixelLayout::Nv12);
    const VideoFrame frame=read_one_frame(*decoder);
    CHECK(frame.layout==VideoPixelLayout::Nv12);
    CHECK_EQ(FrameBytes(VideoPixelLayout::Nv12,4,2),frame.bgra.size());
}

// A caller can opt out of OpenSequential's NV12 preference (preferNv12=false) to
// keep BGRA even for geometry that would otherwise qualify for NV12 - the neural
// render uses this when the GPU is the scarce resource and ffmpeg's CPU-side
// conversion is cheaper to spend than a GPU cycle.
void video_decoder_open_sequential_can_keep_bgra_for_even_geometry_test()
{
    MediaFixture fixture;
    auto decoder=VideoDecoderTestAccess::Create(fixture.directory);
    CHECK(decoder->OpenSequential(L"nv12geom",MediaSourceKind::LocalFile,{},/*preferNv12=*/false));
    CHECK(decoder->PixelLayout()==VideoPixelLayout::Bgra);
    const VideoFrame frame=read_one_frame(*decoder);
    CHECK(frame.layout==VideoPixelLayout::Bgra);
    CHECK_EQ(FrameBytes(VideoPixelLayout::Bgra,4,2),frame.bgra.size());
}

void video_decoder_open_sequential_stays_bgra_for_odd_geometry_test()
{
    MediaFixture fixture;
    auto decoder=VideoDecoderTestAccess::Create(fixture.directory);
    CHECK(decoder->OpenSequential(L"oddgeom",MediaSourceKind::LocalFile));
    CHECK(decoder->PixelLayout()==VideoPixelLayout::Bgra);
    const VideoFrame frame=read_one_frame(*decoder);
    CHECK(frame.layout==VideoPixelLayout::Bgra);
    CHECK_EQ(FrameBytes(VideoPixelLayout::Bgra,3,3),frame.bgra.size());
}

// The GPU source conversion is a matrix plus a range mapping. A source that
// declares neither, or declares a matrix the conversion has no coefficients for,
// cannot be converted correctly - so it is never handed over as NV12, however
// loudly the caller asked for it. Undeclared is not BT.709.
void video_decoder_open_sequential_refuses_nv12_for_an_unconvertible_source_test()
{
    MediaFixture fixture;
    {
        // Declares nothing at all - the common case for a clip nobody tagged.
        auto decoder=VideoDecoderTestAccess::Create(fixture.directory);
        CHECK(decoder->OpenSequential(L"colortag_none",MediaSourceKind::LocalFile));
        CHECK(decoder->PixelLayout()==VideoPixelLayout::Bgra);
        CHECK(decoder->ColorDescription().matrix==ColorMatrix::Unspecified);
        CHECK(decoder->ColorDescription().range==ColorRange::Unspecified);
        // The refusal is not a failure: the frames still arrive, converted by
        // ffmpeg on the CPU, at BGRA's four bytes per pixel.
        const VideoFrame frame=read_one_frame(*decoder);
        CHECK(frame.layout==VideoPixelLayout::Bgra);
        CHECK_EQ(FrameBytes(VideoPixelLayout::Bgra,4,2),frame.bgra.size());
    }
    {
        // Declares everything, and declares BT.2020 constant luminance-free plus a
        // PQ transfer. Declared-but-unhandled must refuse exactly like undeclared,
        // and must not be rounded to the nearest matrix the shader does have.
        auto decoder=VideoDecoderTestAccess::Create(fixture.directory);
        CHECK(decoder->OpenSequential(L"colortag_bt2020",MediaSourceKind::LocalFile));
        CHECK(decoder->PixelLayout()==VideoPixelLayout::Bgra);
        CHECK(decoder->ColorDescription().matrix==ColorMatrix::Other);
        CHECK(decoder->ColorDescription().range==ColorRange::Limited);
        CHECK(decoder->ColorDescription().transfer==ColorTransfer::Other);
        CHECK(read_one_frame(*decoder).layout==VideoPixelLayout::Bgra);
    }
}

// A source that does declare a description the conversion implements keeps the
// NV12 path, and the description travels far enough out of the decoder for the
// renderer to specialise the shader from it rather than assume BT.709.
void video_decoder_open_sequential_keeps_nv12_for_a_declared_source_test()
{
    MediaFixture fixture;
    {
        auto decoder=VideoDecoderTestAccess::Create(fixture.directory);
        CHECK(decoder->OpenSequential(L"nv12geom",MediaSourceKind::LocalFile));
        CHECK(decoder->PixelLayout()==VideoPixelLayout::Nv12);
        const SourceColorDescription color=decoder->ColorDescription();
        CHECK(color.matrix==ColorMatrix::Bt709);
        CHECK(color.range==ColorRange::Limited);
        CHECK(color.primaries==ColorPrimaries::Bt709);
        CHECK(color.transfer==ColorTransfer::Bt709);
        CHECK(SourceNv12ConversionFor(color)==SourceNv12Conversion::Bt709Limited);
    }
    {
        // BT.601 full range, and the stub prints the colour entries before the
        // geometry ones, so this also covers the description surviving ffprobe's
        // own ordering.
        auto decoder=VideoDecoderTestAccess::Create(fixture.directory);
        CHECK(decoder->OpenSequential(L"colortag_bt601full",MediaSourceKind::LocalFile));
        CHECK(decoder->PixelLayout()==VideoPixelLayout::Nv12);
        const SourceColorDescription color=decoder->ColorDescription();
        CHECK(color.matrix==ColorMatrix::Bt601);
        CHECK(color.range==ColorRange::Full);
        CHECK(SourceNv12ConversionFor(color)==SourceNv12Conversion::Bt601Full);
        const VideoFrame frame=read_one_frame(*decoder);
        CHECK(frame.layout==VideoPixelLayout::Nv12);
        CHECK_EQ(FrameBytes(VideoPixelLayout::Nv12,4,2),frame.bgra.size());
    }
}

void video_decoder_swap_carries_probe_derived_state_test()
{
    MediaFixture fixture;
    VideoDecoderTestAccess::CheckSwapCarriesSource(fixture.directory);
}

// The seven numbers the NV12 source pass is compiled with, per conversion. The
// BT.709 limited-range row is frozen: it is the program every cached render on
// disk was made with, and because the constants reach the shader as preprocessor
// tokens, these exact strings are what make that arm's bytecode byte-identical to
// the day it shipped. Changing one of them silently re-colours every cache hit.
void source_nv12_conversion_constants_are_the_shipped_coefficients_test()
{
    using d3d12_renderer_detail::SourceNv12ConstantsFor;
    const auto* limited709=SourceNv12ConstantsFor(SourceNv12Conversion::Bt709Limited);
    CHECK(limited709!=nullptr);
    if(limited709){
        CHECK_EQ(std::string("16.0"),std::string(limited709->lumaOffset));
        CHECK_EQ(std::string("219.0"),std::string(limited709->lumaScale));
        CHECK_EQ(std::string("224.0"),std::string(limited709->chromaScale));
        CHECK_EQ(std::string("1.5748"),std::string(limited709->redV));
        CHECK_EQ(std::string("0.187324"),std::string(limited709->greenU));
        CHECK_EQ(std::string("0.468124"),std::string(limited709->greenV));
        CHECK_EQ(std::string("1.8556"),std::string(limited709->blueU));
    }
    // BT.601 selects BT.601's coefficients, not 709's, and keeps the studio-swing
    // range mapping it shares with 709 limited.
    const auto* limited601=SourceNv12ConstantsFor(SourceNv12Conversion::Bt601Limited);
    CHECK(limited601!=nullptr);
    if(limited601){
        CHECK_EQ(std::string("1.402"),std::string(limited601->redV));
        CHECK_EQ(std::string("0.344136"),std::string(limited601->greenU));
        CHECK_EQ(std::string("0.714136"),std::string(limited601->greenV));
        CHECK_EQ(std::string("1.772"),std::string(limited601->blueU));
        CHECK_EQ(std::string("16.0"),std::string(limited601->lumaOffset));
        CHECK_EQ(std::string("219.0"),std::string(limited601->lumaScale));
        CHECK_EQ(std::string("224.0"),std::string(limited601->chromaScale));
    }
    // Full range keeps its matrix and drops the studio-swing mapping: no 16 offset
    // and the whole byte in both planes.
    const auto* full601=SourceNv12ConstantsFor(SourceNv12Conversion::Bt601Full);
    CHECK(full601!=nullptr);
    if(full601){
        CHECK_EQ(std::string("0.0"),std::string(full601->lumaOffset));
        CHECK_EQ(std::string("255.0"),std::string(full601->lumaScale));
        CHECK_EQ(std::string("255.0"),std::string(full601->chromaScale));
        CHECK_EQ(std::string("1.402"),std::string(full601->redV));
    }
    // No nearest variant: a description the pass cannot honour yields no constants,
    // which is what leaves the renderer nothing to compile.
    CHECK(SourceNv12ConstantsFor(SourceNv12Conversion::Unsupported)==nullptr);
}

// A per-arm label is not evidence that the arm took effect. Each conversion is
// compiled here the way the renderer compiles it - same text, same flags, no
// device - and the four programs must come out pairwise different, which is only
// true if the constants actually reached the compiler.
void source_nv12_conversion_compiles_a_distinct_program_per_arm_test()
{
    const std::array conversions{SourceNv12Conversion::Bt709Limited,SourceNv12Conversion::Bt709Full,
                                 SourceNv12Conversion::Bt601Limited,SourceNv12Conversion::Bt601Full};
    std::vector<std::string> programs;
    for(const SourceNv12Conversion conversion:conversions){
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        CHECK(D3D12RendererTestAccess::CompileSourceNv12(conversion,blob));
        if(!blob)continue;
        programs.emplace_back(static_cast<const char*>(blob->GetBufferPointer()),blob->GetBufferSize());
    }
    CHECK_EQ(conversions.size(),programs.size());
    for(size_t a=0;a+1<programs.size();++a)
        for(size_t b=a+1;b<programs.size();++b)
            CHECK(programs[a]!=programs[b]);
    // An unsupported conversion has no program at all, so nothing can bind one.
    Microsoft::WRL::ComPtr<ID3DBlob> refused;
    CHECK(!D3D12RendererTestAccess::CompileSourceNv12(SourceNv12Conversion::Unsupported,refused));
    CHECK(refused==nullptr);
}

void video_decoder_forward_seek_reuses_child_and_delivers_the_same_frame_as_a_restart_test()
{
    MediaFixture fixture;
    const auto expectedTimestamp=[](int64_t frameIndex){
        return static_cast<int64_t>((static_cast<double>(frameIndex)/30.0)*10000000.0);
    };
    // A short forward hop: the child stays, and the frames before the target
    // are dropped without the caller ever seeing them.
    auto reused=VideoDecoderTestAccess::Create(fixture.directory);
    CHECK(reused->Open(L"seekreuse",MediaSourceKind::LocalFile));
    CHECK_EQ(uint32_t{0},stamped_frame_index(read_one_frame(*reused)));
    CHECK(reused->SeekSeconds(0.1));
    CHECK(reused->LastSeekTiming().reusedChild);
    const VideoFrame afterReuse=read_one_frame(*reused);

    // The same target reached by rewinding, which can only be served by a
    // restarted child.
    auto restarted=VideoDecoderTestAccess::Create(fixture.directory);
    CHECK(restarted->Open(L"seekreuse",MediaSourceKind::LocalFile));
    CHECK(restarted->SeekSeconds(2.0));
    CHECK(restarted->SeekSeconds(0.1));
    CHECK(!restarted->LastSeekTiming().reusedChild);
    const VideoFrame afterRestart=read_one_frame(*restarted);

    CHECK_EQ(uint32_t{3},stamped_frame_index(afterRestart));
    CHECK(afterReuse.bgra==afterRestart.bgra);
    CHECK_EQ(afterRestart.timestamp100ns,afterReuse.timestamp100ns);
    CHECK_EQ(expectedTimestamp(3),afterReuse.timestamp100ns);
    CHECK_EQ(afterRestart.frameNumber,afterReuse.frameNumber);
    CHECK_EQ(uint64_t{3},afterReuse.frameNumber);
    CHECK_EQ(afterRestart.discontinuity,afterReuse.discontinuity);

    // Frame stepping is the smallest forward seek there is: every step keeps the
    // child and walks exactly one frame.
    for(int64_t step=4;step<9;++step){
        CHECK(reused->SeekSeconds(static_cast<double>(step)/30.0));
        CHECK(reused->LastSeekTiming().reusedChild);
        const VideoFrame stepped=read_one_frame(*reused);
        CHECK_EQ(static_cast<uint32_t>(step),stamped_frame_index(stepped));
        CHECK_EQ(expectedTimestamp(step),stepped.timestamp100ns);
        CHECK_EQ(static_cast<uint64_t>(step),stepped.frameNumber);
    }

    // Rewinding cannot be served by a running child.
    CHECK(reused->SeekSeconds(0.1));
    CHECK(!reused->LastSeekTiming().reusedChild);
    CHECK_EQ(uint32_t{3},stamped_frame_index(read_one_frame(*reused)));

    // A hop far enough that decoding to it costs more than a fresh child does.
    CHECK(reused->SeekSeconds(20.0));
    CHECK(!reused->LastSeekTiming().reusedChild);
    const VideoFrame afterLongSeek=read_one_frame(*reused);
    CHECK_EQ(uint32_t{600},stamped_frame_index(afterLongSeek));
    CHECK_EQ(expectedTimestamp(600),afterLongSeek.timestamp100ns);
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
    D3D12RendererTestAccess::ResetRetainedRenderers();
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
        L"https://youtube.com/watch?v=dQw4w9WgXcQ&list=PL123&success", {});

    CHECK(result.ok);
    CHECK_EQ(ResolveError::None, result.error);
    CHECK_EQ(std::wstring(L"https://r1.googlevideo.com/videoplayback?id=success"),
             result.mediaUrl);
    CHECK_EQ(std::wstring(L"https://r1.googlevideo.com/videoplayback?id=success-audio"),
             result.audioUrl);
    CHECK_EQ(167.0, result.durationSeconds);
    CHECK(result.detail.empty());
}

void youtube_resolver_waits_until_both_selected_streams_are_available_test()
{
    ResolverFixture fixture;
    auto resolver = YouTubeResolverTestAccess::Create(fixture.directory, std::chrono::seconds{5});
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto ready = now + 2;
    const auto url = L"https://youtu.be/dQw4w9WgXcQ?availability_" + std::to_wstring(now + 1) +
        L"_" + std::to_wstring(ready);
    const auto result = resolver->Resolve(url, {});
    CHECK(result.ok);
    CHECK(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() >= ready);
}

void youtube_resolver_availability_wait_is_cancellable_and_deadline_bounded_test()
{
    ResolverFixture fixture;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto url = L"https://youtu.be/dQw4w9WgXcQ?availability_" + std::to_wstring(now + 10) + L"_0";
    auto bounded = YouTubeResolverTestAccess::Create(fixture.directory, std::chrono::milliseconds{150});
    CHECK_EQ(ResolveError::TimedOut, bounded->Resolve(url, {}).error);
    const auto marker = fixture.directory / L"availability-ready.marker";
    std::filesystem::remove(marker);
    for (const bool explicitCancel : {false, true}) {
        auto resolver = YouTubeResolverTestAccess::Create(fixture.directory, std::chrono::seconds{5});
        const auto beforeHelpers = count_named_processes(L"yt-dlp.exe");
        ResolveResult result;
        std::jthread worker([&](std::stop_token stop) { result = resolver->Resolve(url, stop); });
        CHECK(wait_for_file(marker, std::chrono::seconds{2}));
        // The extractor has exited: cancellation must still interrupt the
        // resolver's availability wait, independently of its child-process loop.
        const auto childDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (count_named_processes(L"yt-dlp.exe") != beforeHelpers &&
               std::chrono::steady_clock::now() < childDeadline) Sleep(10);
        CHECK_EQ(beforeHelpers, count_named_processes(L"yt-dlp.exe"));
        if (explicitCancel) resolver->Cancel(); else worker.request_stop();
        worker.join();
        CHECK_EQ(ResolveError::Cancelled, result.error);
        CHECK(result.mediaUrl.empty());
        CHECK(result.audioUrl.empty());
        std::filesystem::remove(marker);
    }
}

void youtube_resolver_waits_for_fractional_stream_availability_test()
{
    ResolverFixture fixture;
    auto resolver = YouTubeResolverTestAccess::Create(fixture.directory, std::chrono::seconds{5});
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto ready = now + 1;
    const auto result = resolver->Resolve(
        L"https://youtu.be/dQw4w9WgXcQ?availability_0_" + std::to_wstring(ready) + L"_fraction", {});
    CHECK(result.ok);
    CHECK(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()
          >= static_cast<double>(ready) + 0.5);
}

void resolver_output_validates_stream_availability_metadata_test()
{
    for (const auto suffix : {";video_available_at=1788425759;audio_available_at=1788425760",
                               ";video_available_at=1788425759.5;audio_available_at=1788425760.25",
                               ";video_available_at=0;audio_available_at=0"}) {
        CHECK(ParseResolverOutput(std::string("duration=167;live_status=not_live") + suffix +
            "\nhttps://v.googlevideo.com/video\n", 0).ok);
    }
    for (const auto suffix : {";video_available_at=-1;audio_available_at=0",
                               ";video_available_at=nan;audio_available_at=0",
                               ";video_available_at=inf;audio_available_at=0",
                               ";video_available_at=253402300800;audio_available_at=0",
                               ";video_available_at=18446744073709551616;audio_available_at=0",
                               ";video_available_at=1;audio_available_at=",
                               ";video_available_at=1;audio_available_at=0;extra=1",
                               ";video_available_at=1", ";audio_available_at=1"}) {
        CHECK_EQ(ResolveError::InvalidOutput, ParseResolverOutput(
            std::string("duration=167;live_status=not_live") + suffix +
            "\nhttps://v.googlevideo.com/video\n", 0).error);
    }
    CHECK_EQ(int64_t{1788425761}, ParseResolverOutput(
        "duration=167;live_status=not_live;video_available_at=1788425759.5;audio_available_at=1788425760.25"
        "\nhttps://v.googlevideo.com/video\n", 0).availableAtUnixSeconds);
}

void youtube_resolver_requires_duration_metadata_before_acquisition_test()
{
    ResolverFixture fixture;
    auto resolver = YouTubeResolverTestAccess::Create(fixture.directory);
    for (const auto url : {L"https://youtu.be/dQw4w9WgXcQ?missingduration",
                           L"https://youtu.be/dQw4w9WgXcQ?unknownduration",
                           L"https://youtu.be/dQw4w9WgXcQ?liveduration"}) {
        const auto result = resolver->Resolve(url, {});
        CHECK(!result.ok);
        CHECK_EQ(ResolveError::InvalidOutput, result.error);
        CHECK(result.mediaUrl.empty());
        CHECK(result.audioUrl.empty());
        CHECK_EQ(0.0, result.durationSeconds);
        CHECK(result.detail.find(L"http") == std::wstring::npos);
    }
}

void youtube_resolver_reports_missing_and_unstartable_helpers_without_sensitive_data_test()
{
    const std::filesystem::path missingDirectory =
        std::filesystem::temp_directory_path() / L"PolicyTests-resolver-missing";
    std::error_code error;
    std::filesystem::remove_all(missingDirectory, error);
    auto missingResolver = YouTubeResolverTestAccess::Create(missingDirectory);
    const ResolveResult missing = missingResolver->Resolve(
        L"https://youtube.com/watch?v=dQw4w9WgXcQ&secret_missing", {});
    CHECK(!missing.ok);
    CHECK_EQ(ResolveError::HelperMissing, missing.error);
    CHECK(missing.mediaUrl.empty());
    CHECK(missing.detail.find(L"secret_missing") == std::wstring::npos);

    ResolverFixture corruptFixture(false);
    auto corruptResolver = YouTubeResolverTestAccess::Create(corruptFixture.directory);
    const ResolveResult corrupt = corruptResolver->Resolve(
        L"https://youtube.com/watch?v=dQw4w9WgXcQ&secret_start", {});
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
        L"https://youtu.be/dQw4w9WgXcQ?nonzero", {});
    CHECK(!nonzero.ok);
    CHECK_EQ(ResolveError::ExtractionFailed, nonzero.error);
    CHECK(nonzero.mediaUrl.empty());

    const ResolveResult exactCaptureLimit = resolver->Resolve(
        L"https://youtu.be/dQw4w9WgXcQ?cap64", {});
    CHECK(!exactCaptureLimit.ok);
    CHECK_EQ(ResolveError::InvalidOutput, exactCaptureLimit.error);

    const ResolveResult overflow = resolver->Resolve(
        L"https://youtu.be/dQw4w9WgXcQ?cap64plus", {});
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
        stopped = resolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?hang", token);
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
        cancelled = resolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?hang", {});
    });
    Sleep(75);
    resolver->Cancel();
    cancelWorker.join();
    const auto cancelElapsed = std::chrono::steady_clock::now() - cancelStarted;
    CHECK_EQ(ResolveError::Cancelled, cancelled.error);
    CHECK(cancelElapsed < std::chrono::seconds{2});

    resolver->Cancel();
    const ResolveResult afterCancel = resolver->Resolve(
        L"https://youtu.be/dQw4w9WgXcQ?success", {});
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
        L"https://youtu.be/dQw4w9WgXcQ?descendant_" + suffix, {});
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
            L"https://youtu.be/dQw4w9WgXcQ?success", {});
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
        if(all.find(L"durationtag_")!=std::wstring::npos){
            std::cout << "width=2\nheight=2\navg_frame_rate=30/1\nr_frame_rate=30/1\n";
            std::cout << (all.find(L"durationtag_mp4")!=std::wstring::npos ? "duration=1.25\n" : "duration=N/A\n");
            if(all.find(L"stream_tags=DURATION")!=std::wstring::npos){
                std::string_view tag = "N/A";
                if(all.find(L"durationtag_valid")!=std::wstring::npos)tag="00:00:01.000000000";
                else if(all.find(L"durationtag_fractional")!=std::wstring::npos)tag="01:02:03.250000000";
                else if(all.find(L"durationtag_badminute")!=std::wstring::npos)tag="00:60:01.0";
                else if(all.find(L"durationtag_badsecond")!=std::wstring::npos)tag="00:00:60.0";
                else if(all.find(L"durationtag_negative")!=std::wstring::npos)tag="00:00:-1.0";
                else if(all.find(L"durationtag_nonfinite")!=std::wstring::npos)tag="00:00:nan";
                else if(all.find(L"durationtag_trailing")!=std::wstring::npos)tag="00:00:01.0junk";
                else if(all.find(L"durationtag_overflow")!=std::wstring::npos)tag="999999999999999999999999999999:00:01.0";
                else if(all.find(L"durationtag_zero")!=std::wstring::npos)tag="00:00:00.000000000";
                std::cout << "TAG:DURATION=" << tag << '\n';
            }
            std::cout << "duration=3.000\n" << std::flush;
            return 0;
        }
        if(all.find(L"holdprobe")!=std::wstring::npos){Sleep(INFINITE);return 0;}
        // ffprobe prints a colour entry only when -show_entries asked for it, and this
        // stub does the same: a scenario below whose colour decision comes out right is
        // therefore proof that the decoder requested the four entries in the SAME
        // invocation that produced the geometry, which is the one ffprobe child an open
        // is allowed to spend.
        const bool colorAsked=all.find(L"color_space")!=std::wstring::npos&&
                              all.find(L"color_range")!=std::wstring::npos&&
                              all.find(L"color_primaries")!=std::wstring::npos&&
                              all.find(L"color_transfer")!=std::wstring::npos;
        const auto color=[&](const char* space,const char* range,const char* primaries,
                             const char* transfer){
            if(!colorAsked)return std::string{};
            return std::string("color_space=")+space+"\ncolor_range="+range+
                   "\ncolor_primaries="+primaries+"\ncolor_transfer="+transfer+"\n";
        };
        const auto geometry=[](unsigned w,unsigned h,const char* dar,const char* duration="30"){
            return "width="+std::to_string(w)+"\nheight="+std::to_string(h)+
                   "\ndisplay_aspect_ratio="+dar+
                   "\nsample_aspect_ratio=1:1\navg_frame_rate=30/1\nr_frame_rate=30/1\nduration="+
                   duration+"\n";
        };
        // largeburst and drainexit are sequential opens whose subject is the frame
        // queue, so they declare BT.709 limited range: the frames they are about have
        // to be the NV12 ones, and only a declared description gets those.
        if(all.find(L"largeburst")!=std::wstring::npos){
            std::cout<<geometry(1024,1024,"1:1")<<color("bt709","tv","bt709","bt709")<<std::flush;return 0;
        }
        if(all.find(L"drainexit")!=std::wstring::npos){
            std::cout<<geometry(1920,1080,"16:9","0.034")<<color("bt709","tv","bt709","bt709")<<std::flush;return 0;
        }
        if(all.find(L"partialend")!=std::wstring::npos){
            std::cout<<geometry(2,2,"1:1","0.067")<<std::flush;return 0;
        }
        // Even geometry AND a declared colour description the GPU conversion implements:
        // OpenSequential can pick NV12 here.
        if(all.find(L"nv12geom")!=std::wstring::npos){
            std::cout<<geometry(4,2,"2:1")<<color("bt709","tv","bt709","bt709")<<std::flush;return 0;
        }
        // Even geometry, nothing declared. NV12 would hand the shader a frame whose
        // matrix and range it would have to guess, so OpenSequential must stay BGRA.
        if(all.find(L"colortag_none")!=std::wstring::npos){
            std::cout<<geometry(4,2,"2:1")<<color("unknown","unknown","unknown","unknown")<<std::flush;return 0;
        }
        // Even geometry, BT.601 full range - a description the conversion does have
        // coefficients for, so NV12 is allowed and the shader is specialised for it.
        // Printed colour-first, because ffprobe emits entries in its own order and the
        // decoder's parse composes the description only once all four are in hand.
        if(all.find(L"colortag_bt601full")!=std::wstring::npos){
            std::cout<<color("bt470bg","pc","bt470bg","smpte170m")<<geometry(4,2,"2:1")<<std::flush;return 0;
        }
        // Even geometry, fully declared, and declared as something the conversion has no
        // coefficients for. Declared-but-unhandled refuses exactly like undeclared.
        if(all.find(L"colortag_bt2020")!=std::wstring::npos){
            std::cout<<color("bt2020nc","tv","bt2020","smpte2084")<<geometry(4,2,"2:1")<<std::flush;return 0;
        }
        // Odd geometry: NV12's half-resolution UV plane needs even dimensions, so
        // OpenSequential must stay BGRA here even though it prefers NV12 - and the
        // colour description is the one the conversion likes best, so only the geometry
        // can be what refused it.
        if(all.find(L"oddgeom")!=std::wstring::npos){
            std::cout<<geometry(3,3,"1:1")<<color("bt709","tv","bt709","bt709")<<std::flush;return 0;
        }
        std::cout<<"width=2\nheight=2\ndisplay_aspect_ratio=1:1\nsample_aspect_ratio=1:1\navg_frame_rate=30/1\nr_frame_rate=30/1\nduration=30\n"<<std::flush;return 0;
    }
    if(_wcsicmp(name.c_str(),L"ffmpeg.exe")!=0)return 94;
    // Mirrors FrameBytes(layout,w,h): OpenSequential's NV12 request shows up here as
    // `-pix_fmt nv12` on the command line, and the raw frame this fake child writes
    // has to match it exactly or the decoder's frameBytes-sized reads never complete.
    const bool nv12Requested=all.find(L"-pix_fmt nv12")!=std::wstring::npos;
    const auto rawFrameBytes=[nv12Requested](size_t w,size_t h){
        return nv12Requested?w*h*3u/2u:w*h*4u;
    };
    if(all.find(L"nv12geom")!=std::wstring::npos){
        const std::vector<char> frame(rawFrameBytes(4,2),'n');
        std::cout.write(frame.data(),static_cast<std::streamsize>(frame.size()));std::cout.flush();return 0;
    }
    // Every colortag_ scenario shares nv12geom's 4x2 geometry; only the declared
    // colour differs, so the layout each one ends up with is attributable to that
    // alone.
    if(all.find(L"colortag_")!=std::wstring::npos){
        const std::vector<char> frame(rawFrameBytes(4,2),'c');
        std::cout.write(frame.data(),static_cast<std::streamsize>(frame.size()));std::cout.flush();return 0;
    }
    if(all.find(L"oddgeom")!=std::wstring::npos){
        const std::vector<char> frame(rawFrameBytes(3,3),'o');
        std::cout.write(frame.data(),static_cast<std::streamsize>(frame.size()));std::cout.flush();return 0;
    }
    if(all.find(L"hardwarefallback")!=std::wstring::npos){
        const std::wstring marker=read_environment_variable(L"DLSS_VIDEO_TEST_ACCEL_MARKER");
        const bool cuda=all.find(L"-hwaccel cuda")!=std::wstring::npos;
        const bool d3d11=all.find(L"-hwaccel d3d11va")!=std::wstring::npos;
        const bool delayedExit=all.find(L"hardwarefallbackdelayedexit")!=std::wstring::npos;
        if(!marker.empty()){std::ofstream out(marker,std::ios::binary|std::ios::app);out<<(cuda?"cuda\n":d3d11?"d3d11va\n":"software\n");}
        if(cuda||d3d11){if(delayedExit){CloseHandle(GetStdHandle(STD_OUTPUT_HANDLE));Sleep(75);}return 7;}
        std::cout.write("1234567890abcdef",16);std::cout.flush();return 0;
    }
    if(all.find(L"seekreuse")!=std::wstring::npos){
        // 2x2 BGRA at 30fps, every frame stamped with its absolute source index
        // so a caller can tell exactly which frame it was handed. Writes go
        // through the handle: the CRT's text mode would rewrite 0x0A bytes.
        double seekSeconds=0.0;
        const size_t at=all.find(L"-ss ");
        if(at!=std::wstring::npos)try{seekSeconds=std::stod(all.substr(at+4));}catch(...){}
        uint32_t index=static_cast<uint32_t>(std::llround(seekSeconds*30.0));
        const HANDLE out=GetStdHandle(STD_OUTPUT_HANDLE);
        for(int emitted=0;emitted<3000;++emitted,++index){
            unsigned char frame[16];
            for(int byte=0;byte<4;++byte)frame[byte]=static_cast<unsigned char>((index>>(8*byte))&0xFFu);
            for(int byte=4;byte<16;++byte)frame[byte]=static_cast<unsigned char>(index&0xFFu);
            DWORD written=0;
            if(!WriteFile(out,frame,sizeof(frame),&written,nullptr)||written!=sizeof(frame))return 0;
        }
        Sleep(INFINITE);return 0;
    }
    if(all.find(L"largeburst")!=std::wstring::npos){
        const std::wstring marker=read_environment_variable(L"DLSS_VIDEO_TEST_FRAME_MARKER");
        const std::vector<char> frame(rawFrameBytes(1024,1024),'x');
        for(int index=0;index<20;++index){
            std::cout.write(frame.data(),static_cast<std::streamsize>(frame.size()));std::cout.flush();
            if(!marker.empty()){std::ofstream out(marker,std::ios::binary|std::ios::app);out.put('x');}
        }
        Sleep(INFINITE);return 0;
    }
    if(all.find(L"drainexit")!=std::wstring::npos){
        const std::wstring marker=read_environment_variable(L"DLSS_VIDEO_TEST_ACCEL_MARKER");
        if(!marker.empty()){
            const bool cuda=all.find(L"-hwaccel cuda")!=std::wstring::npos;
            const bool d3d11=all.find(L"-hwaccel d3d11va")!=std::wstring::npos;
            std::ofstream out(marker,std::ios::binary|std::ios::app);
            out<<(cuda?"cuda\n":d3d11?"d3d11va\n":"software\n");
        }
        const std::vector<char> frame(rawFrameBytes(1920,1080),'z');
        std::cout.write(frame.data(),static_cast<std::streamsize>(frame.size()));
        std::cout.flush();return 0;
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
    if (argc != 17 || std::wstring_view(argv[1]) != L"--no-config" ||
        std::wstring_view(argv[2]) != L"--no-cache-dir" ||
        std::wstring_view(argv[3]) != L"--no-plugin-dirs" ||
        std::wstring_view(argv[4]) != L"--no-playlist" ||
        std::wstring_view(argv[5]) != L"--no-warnings" ||
        std::wstring_view(argv[6]) != L"--js-runtimes" ||
        !std::wstring_view(argv[7]).starts_with(L"deno:") ||
        std::wstring_view(argv[8]) != L"-f" ||
        std::wstring_view(argv[9]) != YouTubeFormatSelector(YouTubeSourceQuality::Auto) ||
        std::wstring_view(argv[10]) != L"--format-sort-force" ||
        std::wstring_view(argv[11]) != L"-S" ||
        std::wstring_view(argv[12]) != L"height,vbr,abr" ||
        std::wstring_view(argv[13]) != L"--get-url" ||
        std::wstring_view(argv[14]) != L"--print" ||
        std::wstring_view(argv[15]) != L"duration=%(duration)s;live_status=%(live_status)s;video_available_at=%(requested_formats.0.available_at,available_at|0)s;audio_available_at=%(requested_formats.1.available_at|0)s;selected_height=%(height)s;video_kbps=%(requested_formats.0.vbr,vbr,tbr|0)s;age_limit=%(age_limit)s") {
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

    const std::wstring_view url = argv[16];
    if (url.find(L"availability_") != std::wstring_view::npos) {
        const auto times = url.substr(url.find(L"availability_") + 13);
        const auto separator = times.find(L'_');
        const std::wstring video(times.substr(0, separator));
        std::wstring audio(times.substr(separator + 1));
        if (audio.ends_with(L"_fraction")) audio.replace(audio.size() - 9, 9, L".5");
        std::cout << "duration=167;live_status=not_live;video_available_at=";
        for (const wchar_t character : video) std::cout.put(static_cast<char>(character));
        std::cout << ";audio_available_at=";
        for (const wchar_t character : audio) std::cout.put(static_cast<char>(character));
        std::cout << "\nhttps://r1.googlevideo.com/videoplayback?id=availability\n" << std::flush;
        write_binary_file(current_test_executable().parent_path() / L"availability-ready.marker", "ready");
        return 0;
    }
    if (url.find(L"missingduration") != std::wstring_view::npos) {
        std::cout << "https://r1.googlevideo.com/videoplayback?id=duration-missing\n" << std::flush;
        return 0;
    }
    if (url.find(L"unknownduration") != std::wstring_view::npos) {
        std::cout << "duration=NA;live_status=not_live\n"
                     "https://r1.googlevideo.com/videoplayback?id=duration-unknown\n" << std::flush;
        return 0;
    }
    if (url.find(L"liveduration") != std::wstring_view::npos) {
        std::cout << "duration=167;live_status=is_live\n"
                     "https://r1.googlevideo.com/videoplayback?id=duration-live\n" << std::flush;
        return 0;
    }
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
        std::cout << "duration=167;live_status=not_live\n"
                     "https://r1.googlevideo.com/videoplayback?id=envcapture\n" << std::flush;
        return 0;
    }
    if (url.find(L"success") != std::wstring_view::npos) {
        std::cout << "duration=167;live_status=not_live\n"
                     "https://r1.googlevideo.com/videoplayback?id=success\n"
                     "https://r1.googlevideo.com/videoplayback?id=success-audio\n" << std::flush;
        return 0;
    }
    if (url.find(L"uismoke") != std::wstring_view::npos) {
        write_binary_file(current_test_executable().parent_path()/L"ui-helper-launch.marker","launched");
        std::cout << "duration=167;live_status=not_live\n"
                     "https://r1.googlevideo.com/videoplayback?id=uismoke\n" << std::flush;
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

// Only this process's own children count. Counting every ffmpeg.exe on the machine
// made the leak checks depend on what the rest of the suite was doing: the real
// ffmpeg that CachedExportTests and NeuralPrerenderTests run appears and exits
// under a different test binary, moving the baseline mid-test and failing a
// decoder that leaked nothing. Every helper these fixtures spawn is a direct
// child, so the parent id is the exact scope the checks always meant.
size_t count_named_processes(std::wstring_view executableName)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    CHECK(snapshot != INVALID_HANDLE_VALUE);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    const DWORD self = GetCurrentProcessId();
    PROCESSENTRY32W entry{sizeof(entry)};
    size_t count = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID != self) continue;
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

// The two reparse points the refusal is tested with need no privilege, unlike
// the symbolic links this used to attempt: NTFS lets any writer stamp a file
// with a non-Microsoft reparse tag, and a directory junction is the reparse
// point Windows hands out without SeCreateSymbolicLinkPrivilege. The resolver
// reads only the reparse attribute, never the tag, so either stands in for a
// symbolic link exactly - and unlike one, they exist on every runner.
bool set_reparse_point(HANDLE handle, const void* data, size_t bytes)
{
    DWORD returned = 0;
    return DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, const_cast<void*>(data),
                           static_cast<DWORD>(bytes), nullptr, 0, &returned, nullptr) != FALSE;
}

bool create_reparse_file(const std::filesystem::path& path)
{
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    REPARSE_GUID_DATA_BUFFER data{};
    data.ReparseTag = 0x42;
    data.ReparseGuid = GUID{0x8f2c5a1e, 0x4d3b, 0x4c7a, {0x9e, 0x11, 0x20, 0x26, 0x09, 0x16, 0x00, 0x01}};
    const bool set = set_reparse_point(handle, &data, REPARSE_GUID_DATA_BUFFER_HEADER_SIZE);
    CloseHandle(handle);
    return set;
}

bool create_junction(const std::filesystem::path& link, const std::filesystem::path& target)
{
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    // ntifs.h's REPARSE_DATA_BUFFER for IO_REPARSE_TAG_MOUNT_POINT; the SDK
    // does not declare it. The path buffer holds the NT-form substitute name,
    // its terminator, an empty print name and its terminator.
    struct MountPoint {
        ULONG tag;
        USHORT dataLength, reserved, substituteOffset, substituteLength, printOffset, printLength;
        wchar_t path[1];
    };
    const std::wstring substitute = L"\\??\\" + target.wstring();
    const size_t substituteBytes = substitute.size() * sizeof(wchar_t);
    std::vector<char> buffer(offsetof(MountPoint, path) + substituteBytes + 2 * sizeof(wchar_t));
    auto* data = reinterpret_cast<MountPoint*>(buffer.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength = static_cast<USHORT>(buffer.size() - offsetof(MountPoint, substituteOffset));
    data->substituteLength = static_cast<USHORT>(substituteBytes);
    data->printOffset = static_cast<USHORT>(substituteBytes + sizeof(wchar_t));
    memcpy(data->path, substitute.data(), substituteBytes);
    const bool set = set_reparse_point(handle, buffer.data(), buffer.size());
    CloseHandle(handle);
    return set;
}

void youtube_resolver_rejects_reparse_points_and_nonregular_helpers_before_execution_test()
{
    ResolverFixture fixture;
    const std::filesystem::path outsideDirectory = fixture.directory.parent_path() /
        (L"PolicyTests-YouTubeResolver-outside-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(outsideDirectory, error);
    error.clear();
    std::filesystem::create_directories(outsideDirectory, error);
    CHECK(!error);

    // A helper that is a reparse point is refused before anything is spawned:
    // HelperMissing is the verification's own answer, StartFailed would mean
    // the launch was attempted.
    std::filesystem::remove(fixture.directory / L"yt-dlp.exe", error);
    CHECK(!error);
    CHECK(create_reparse_file(fixture.directory / L"yt-dlp.exe"));
    auto reparseResolver = YouTubeResolverTestAccess::Create(fixture.directory);
    CHECK_EQ(ResolveError::HelperMissing,
             reparseResolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?success", {}).error);

    std::filesystem::remove(fixture.directory / L"yt-dlp.exe", error);
    error.clear();
    std::filesystem::create_directory(fixture.directory / L"yt-dlp.exe", error);
    CHECK(!error);
    auto directoryResolver = YouTubeResolverTestAccess::Create(fixture.directory);
    CHECK_EQ(ResolveError::HelperMissing,
             directoryResolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?success", {}).error);

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
             denoDirectoryResolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?success", {}).error);

    // The package-local cache directory is refused when it is a junction to
    // anywhere, so a redirected cache cannot make the helper load from outside.
    std::filesystem::remove_all(fixture.directory / L"deno.exe", error);
    CHECK(!error);
    write_binary_file(fixture.directory / L"deno.exe", "test-only placeholder");
    const std::filesystem::path cacheLink = fixture.directory / L"youtube-helper-cache";
    CHECK(create_junction(cacheLink, outsideDirectory));
    auto cacheLinkResolver = YouTubeResolverTestAccess::Create(fixture.directory);
    CHECK_EQ(ResolveError::HelperMissing,
             cacheLinkResolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?success", {}).error);
    CHECK(RemoveDirectoryW(cacheLink.c_str()) != FALSE);
    std::filesystem::remove_all(outsideDirectory, error);
    CHECK(!error);
}

void youtube_resolver_holds_verified_helpers_against_replacement_until_completion_test()
{
    ResolverFixture fixture;
    const size_t beforeProcesses = count_named_processes(L"yt-dlp.exe");
    auto resolver = YouTubeResolverTestAccess::Create(
        fixture.directory, std::chrono::seconds{5});
    ResolveResult result;
    std::thread worker([&] {
        result = resolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?hang", {});
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
    const ResolveResult result = resolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?envcapture-ytcacheaudit", {});

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

    const ResolveResult result = resolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?success-pluginaudit", {});

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
        active = resolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?hang", {});
    });
    Sleep(75);
    std::thread queuedThread([&] {
        queued = resolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?success", {});
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
    CHECK(resolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?success", {}).ok);
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
        active = resolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?hang", {});
    });
    Sleep(75);
    std::stop_source queuedStop;
    std::thread queuedThread([&] {
        queued = resolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?success", queuedStop.get_token());
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
        const ResolveResult result = resolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?hang", {});
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
        const ResolveResult result = resolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?hang", {});
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
                 timeoutResolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?hang", {}).error);
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
            cancelled = cancelResolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?hang", {});
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
                 overflowResolver->Resolve(L"https://youtu.be/dQw4w9WgXcQ?cap64plus", {}).error);
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
        true, false, 2, recreateRequested, ensureFeature, recreateFeature);
    CHECK(firstFailure.selected);
    CHECK_EQ(1, createAttempts);

    const auto automaticNextFrame = ngx_session_detail::PrepareFeatureForFrame(
        true, false, 3, recreateRequested, ensureFeature, recreateFeature);
    CHECK(automaticNextFrame.selected);
    CHECK_EQ(1, createAttempts);

    recreateRequested = true;
    const auto explicitRehook = ngx_session_detail::PrepareFeatureForFrame(
        true, false, 4, recreateRequested, ensureFeature, recreateFeature);
    CHECK(explicitRehook.selected);
    CHECK(!recreateRequested);
    CHECK_EQ(2, createAttempts);

    ngx_session_detail::PrepareFeatureForFrame(
        true, false, 5, recreateRequested, ensureFeature, recreateFeature);
    CHECK_EQ(2, createAttempts);
}

// A live feature must never be released on a frame count. The offline job's
// receipt gate re-presents one source frame until the neural add-on publishes a
// fresh evaluation, so a timed release lands inside that gate and tears down
// the worksets whose counter the gate is waiting for: that is the wedge issue 4
// reported on an RTX 4070 Ti and issue 3 on an RTX PRO 6000. The old policy
// released at present 60, which is exactly where a 120-present gate sits.
void ngx_live_feature_is_never_released_on_a_frame_count_test()
{
    bool recreateRequested = false;
    int creates = 0;
    int releases = 0;
    const auto ensureFeature = [&] { ++creates; return true; };
    const auto recreateFeature = [&] { ++releases; return true; };

    bool featureCreated = false;
    for (uint64_t frame = 1; frame <= 400; ++frame) {
        const auto setup = ngx_session_detail::PrepareFeatureForFrame(
            true, featureCreated, frame, recreateRequested, ensureFeature, recreateFeature);
        if (setup.selected) featureCreated = true;
    }
    CHECK_EQ(1, creates);
    CHECK_EQ(0, releases);

    // The explicit request is still the one way to obtain a hook-visible create,
    // and it stays a single shot.
    recreateRequested = true;
    ngx_session_detail::PrepareFeatureForFrame(
        true, featureCreated, 401, recreateRequested, ensureFeature, recreateFeature);
    CHECK_EQ(1, releases);
    ngx_session_detail::PrepareFeatureForFrame(
        true, featureCreated, 402, recreateRequested, ensureFeature, recreateFeature);
    CHECK_EQ(1, releases);
}

// A half-extracted package leaves a helper that is not a PE image. Windows
// answers such a CreateProcessW with a modal hard-error dialog on the calling
// thread and does not return until it is dismissed, which is exactly how a
// worker thread stops forever with nothing in the log. Every spawn site wraps
// itself in ScopedHardErrorSuppression so the call fails instead.
void spawning_a_corrupt_helper_fails_closed_without_a_hard_error_dialog_test()
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path() /
        (L"PolicyTests-CorruptHelper-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    CHECK(!directoryError);
    const std::filesystem::path helper = directory / L"yt-dlp.exe";
    write_binary_file(helper, "not a Windows executable");

    std::wstring command = L"\"" + helper.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const DWORD modeBefore = GetThreadErrorMode();
    const auto started = std::chrono::steady_clock::now();
    BOOL created = FALSE;
    DWORD error = 0;
    {
        const ScopedHardErrorSuppression noHardErrorDialog;
        CHECK((GetThreadErrorMode() & SEM_FAILCRITICALERRORS) != 0);
        created = CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        error = GetLastError();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(!created);
    // 216 is what a non-PE stub reports here and what the suppressed dialog was
    // about ("incompatibility with 64-bit versions of Windows"); a differently
    // malformed image reports 193. Either way the call answers instead of hanging.
    CHECK(error == DWORD{ERROR_EXE_MACHINE_TYPE_MISMATCH} || error == DWORD{ERROR_BAD_EXE_FORMAT});
    // Unsuppressed, the call blocks on the dialog rather than returning at all.
    CHECK(elapsed < std::chrono::seconds{5});
    // The mode belongs to the scope, not to the thread from here on.
    CHECK_EQ(modeBefore, GetThreadErrorMode());
    if (created) { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
    std::filesystem::remove_all(directory, directoryError);
}

constexpr test_support::TestCase kResolverAvailabilityCases[] = {
    TEST_CASE(youtube_resolver_waits_until_both_selected_streams_are_available_test),
    TEST_CASE(youtube_resolver_availability_wait_is_cancellable_and_deadline_bounded_test),
    TEST_CASE(youtube_resolver_waits_for_fractional_stream_availability_test),
    TEST_CASE(resolver_output_validates_stream_availability_metadata_test),
};

// Live neural playback advances by decoding a PAIR, so discarding a late frame
// costs what presenting one costs and falling behind is unrecoverable by
// walking. These pin the two ways out: thin the presentation, or stop walking
// and seek. Measured symptom the policy exists for, from the player's own
// health line on an RTX 5090 at 2560x1440: a 119.88 fps source presented 0.31
// fps with 203 frames discarded in 11.6 s.
void playback_cadence_thins_presentation_before_it_seeks_test()
{
    using namespace playback_cadence;
    constexpr double k120 = 1.0 / 119.88;   // 8.34 ms
    constexpr double k60 = 1.0 / 59.9401;   // 16.68 ms

    // On time: every pair is presented and the cadence stays where it is.
    const auto onTime = Decide(0.0, k120, 1, 0);
    CHECK(onTime.action == Action::Present);
    CHECK_EQ(1u, onTime.stride);

    // More than two frames late: the cadence widens. The frame in hand is still
    // presented, because the stride in force for it was 1 - widening takes
    // effect from the next one rather than skipping a beat already committed to.
    const auto late = Decide(3.0 * k120, k120, 1, 0);
    CHECK(late.action == Action::Present);
    CHECK_EQ(2u, late.stride);

    // At stride 2 the phase decides, which is what makes the result EVEN: one
    // pair in two, so 119.88 fps becomes 59.94 fps of unbroken motion rather
    // than a stutter.
    CHECK(Decide(0.0, k120, 2, 0).action == Action::Present);
    CHECK(Decide(0.0, k120, 2, 1).action == Action::Skip);
    CHECK(Decide(0.0, k120, 2, 2).action == Action::Present);
    CHECK(Decide(0.0, k120, 2, 3).action == Action::Skip);

    // KEEPING UP narrows the cadence, one step at a time. Zero lateness is the
    // case that matters and the one an earlier version got wrong: it asked for
    // the pipeline to run EARLY before giving a frame back, and nothing that is
    // merely keeping up ever runs early, so the stride ratcheted to its cap on
    // one hiccup and stayed there - measured at 15 frames presented out of 120
    // advanced every two seconds, which is a source being followed exactly and
    // shown at an eighth of its rate.
    CHECK_EQ(3u, Decide(0.0, k120, 4, 0).stride);
    CHECK_EQ(3u, Decide(-1.0 * k120, k120, 4, 0).stride);
    CHECK_EQ(3u, Decide(0.25 * k120, k120, 4, 0).stride);
    // Inside the band it holds, which is what stops it oscillating between two
    // cadences - more visible than the coarser one held steady.
    CHECK_EQ(4u, Decide(1.0 * k120, k120, 4, 0).stride);
    CHECK_EQ(4u, Decide(2.0 * k120, k120, 4, 0).stride);

    // The floor and the ceiling.
    CHECK_EQ(kMaxStride, Decide(5.0 * k120, k120, kMaxStride, 0).stride);
    CHECK_EQ(1u, Decide(-5.0 * k120, k120, 1, 0).stride);
    CHECK_EQ(1u, Decide(0.0, k120, 1, 0).stride);

    // A second behind is past the point where walking is the cheaper way to
    // arrive: a seek covers any distance for about half a second here, while
    // walking costs a pair decode per frame - 120 of them for this one second.
    // It outranks the stride entirely; the position is the thing that is wrong.
    for (const uint32_t stride : {1u, 2u, kMaxStride}) {
        // Not named `far`: windows.h still defines that as a macro.
        const auto adrift = Decide(kReanchorSeconds, k120, stride, 0);
        CHECK(adrift.action == Action::Reanchor);
        CHECK_EQ(stride, adrift.stride);
    }
    CHECK(Decide(0.99 * kReanchorSeconds, k120, 1, 0).action != Action::Reanchor);

    // 60 fps is the same policy with twice the room: two frames late there is
    // 33 ms, where at 120 it is 17.
    CHECK_EQ(2u, Decide(3.0 * k60, k60, 1, 0).stride);
    CHECK(Decide(2.0 * k60, k60, 1, 0).action == Action::Present);
    CHECK_EQ(3u, Decide(0.0, k60, 4, 0).stride);

    // Degenerate inputs present rather than invent a cadence.
    CHECK(Decide(0.0, 0.0, 4, 1).action == Action::Present);
    CHECK_EQ(1u, Decide(0.0, 0.0, 4, 1).stride);
    CHECK(Decide(std::numeric_limits<double>::quiet_NaN(), k120, 1, 0).action == Action::Present);
}

// The phase rule, run as a sequence rather than asserted a frame at a time,
// because the way it failed was invisible frame by frame: a caller that reset
// the phase whenever it presented satisfied every single-frame expectation and
// still presented EVERY pair, because (0 % stride) is always 0. It shipped, and
// the only trace was a health line reading stride=1in8 beside dropped=0.
void playback_cadence_phase_presents_exactly_one_pair_in_stride_test()
{
    using namespace playback_cadence;
    constexpr double k120 = 1.0 / 119.88;
    for (const uint32_t stride : {1u, 2u, 3u, 5u, kMaxStride}) {
        uint32_t phase = 0, presented = 0, skipped = 0, longestRun = 0, run = 0;
        for (uint32_t frame = 0; frame < 240; ++frame) {
            // One frame late: inside the hold band, so the cadence stays put
            // and only the phase moves. Zero would NARROW it, which is correct
            // behaviour and the wrong fixture for a phase test.
            const auto decision = Decide(1.0 * k120, k120, stride, phase);
            CHECK_EQ(stride, decision.stride);
            if (decision.action == Action::Present) {
                ++presented; longestRun = std::max(longestRun, run); run = 0;
            } else {
                ++skipped; ++run;
            }
            phase = NextPhase(phase, stride);
        }
        // Exactly one in `stride`, and the skips evenly spaced between them -
        // an even cadence is the entire point, since 120 fps shown every other
        // frame is smooth 60 while the same count shown in bursts is not.
        CHECK_EQ(240u / stride, presented);
        CHECK_EQ(240u - 240u / stride, skipped);
        CHECK_EQ(stride - 1u, longestRun);
    }
}

// Stride removes the presentation cost and nothing else: every pair is still
// decoded. So a source whose frame interval is shorter than one pair's DECODE
// can never be followed live, however coarse the cadence gets, and that is a
// thing to say at attach time rather than let a viewer discover as a frozen
// picture. Measured here: two concurrent 2560x1440 decoders sustain about
// 139 fps each, so a pair costs roughly 7.2 ms.
void playback_cadence_reports_a_rate_no_cadence_can_follow_test()
{
    using namespace playback_cadence;
    constexpr double kPairDecode = 0.0072;
    CHECK(CanFollowLive(1.0 / 59.9401, kPairDecode));   // 16.68 ms, comfortable
    CHECK(CanFollowLive(1.0 / 119.88, kPairDecode));    // 8.34 ms, and only just
    CHECK(!CanFollowLive(1.0 / 240.0, kPairDecode));    // 4.17 ms, never
    // An unmeasured cost is not evidence that the source cannot be followed.
    CHECK(CanFollowLive(1.0 / 119.88, 0.0));
    CHECK(CanFollowLive(0.0, kPairDecode));
    CHECK(CanFollowLive(1.0 / 119.88, std::numeric_limits<double>::infinity()));
}

// The flag that killed neural rendering, and that the whole suite stayed green
// through. It is attractive - it is the only way SetMaximumFrameLatency does
// anything, and it hands back an object a message loop can block on instead of
// spinning a core. But the RenoDX add-on hooks this swapchain, and with the
// flag set its inline NR path allocates a fresh workset per evaluation,
// exhausts its pool in three frames, and logs "NR workset pool exhausted;
// preserving game output" while every later frame passes through untouched:
// frames=0/0, and "A frame was not produced by feature 18".
//
// Nothing else in the suite can see that, so this asserts the one property
// directly. The player does not need the flag: its loop waits on a
// high-resolution timer, which measured lower CPU than the swapchain object.
void swapchain_never_asks_for_the_frame_latency_waitable_object_test()
{
    using d3d12_renderer_detail::SwapchainFlags;
    for (const bool tearing : {false, true}) {
        const UINT flags = SwapchainFlags(tearing);
        CHECK((flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) == 0u);
        // Tearing is the one flag this chain is allowed to carry, and it has to
        // survive - it is what lets an unthrottled present reach the panel.
        CHECK_EQ(tearing ? UINT(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u, flags);
    }
}

// Six controls with measured tooltips are the right surface for someone who
// knows what they do, and the wrong first contact - issue #1 is someone who
// never reached them. Presets answer "what should I pick" once.
//
// Two properties matter more than the values themselves. A preset whose value
// falls outside what LoadNeuralSettings clamps to would not survive a restart:
// the user picks it, the ini is written, and the next launch reads back
// something else and reports Custom. And the default preset has to BE the
// shipped default, or a first run silently starts somewhere the screenshots
// and the measurements were not taken.
void neural_presets_round_trip_and_default_to_the_shipped_settings_test()
{
    using namespace neural_presets;
    CHECK(kPresetCount >= 2);

    // The recommended default is the settings the project actually measures.
    CHECK(kPresets[kDefaultPresetIndex].settings == NeuralSettings{});
    CHECK_EQ(size_t{0}, kDefaultPresetIndex);
    CHECK(kPresets[kDefaultPresetIndex].label.find("recommended") != std::string_view::npos);

    for (size_t index = 0; index < kPresetCount; ++index) {
        const Preset& preset = kPresets[index];
        CHECK(!preset.key.empty());
        CHECK(!preset.label.empty());
        CHECK(!preset.description.empty());

        // Inside the ranges NeuralSettings.cpp clamps to on load. Outside them
        // a preset cannot round-trip through the ini.
        CHECK(preset.settings.intensity >= 0.0f && preset.settings.intensity <= 2.0f);
        CHECK(preset.settings.localTone >= 0.0f && preset.settings.localTone <= 2.0f);
        CHECK(preset.settings.localStructure >= 0.0f && preset.settings.localStructure <= 2.0f);
        CHECK(preset.settings.skinStructure >= -1.0f && preset.settings.skinStructure <= 1.0f);
        CHECK(preset.settings.colorStrength >= 0.0f && preset.settings.colorStrength <= 1.0f);
        CHECK(preset.settings.preset >= 0 && preset.settings.preset <= 3);
        CHECK(preset.settings.style >= 0 && preset.settings.style <= 2);

        // Recognised by both lookups, and by its own key.
        CHECK_EQ(index, IndexOf(preset.settings));
        CHECK_EQ(index, IndexOfKey(preset.key));
    }

    // Distinct keys, labels and settings: two presets that resolve to the same
    // settings would make IndexOf ambiguous and the UI show the wrong one.
    for (size_t a = 0; a < kPresetCount; ++a)
        for (size_t b = a + 1; b < kPresetCount; ++b) {
            CHECK(kPresets[a].key != kPresets[b].key);
            CHECK(kPresets[a].label != kPresets[b].label);
            CHECK(!(kPresets[a].settings == kPresets[b].settings));
        }

    // Editing a preset is the expected path, and lands on Custom rather than
    // on a neighbouring preset.
    NeuralSettings edited = kPresets[kDefaultPresetIndex].settings;
    edited.intensity = 1.37f;
    CHECK_EQ(kPresetCount, IndexOf(edited));
    CHECK_EQ(kPresetCount, IndexOfKey("no-such-preset"));
    CHECK_EQ(kPresetCount, IndexOfKey(""));
}

// The comparison reference conversion runs on every presented frame once the
// viewer picks Blend, Split or Wipe, which a scalar double pass over 3.69 Mpx
// cannot do inside a 16.68 ms budget. Speeding it up is only allowed if the
// picture does not change, so this pins the output against an independent
// transcription of the original scalar arithmetic - not against the optimised
// code's own idea of what it should produce.
void nv12_reference_conversion_is_bit_identical_to_the_scalar_original_test()
{
    // The arithmetic exactly as it was written inline in main.cpp, kept here
    // as the thing the fast path has to agree with.
    const auto scalar = [](const uint8_t* nv12, uint32_t width, uint32_t height,
                           std::vector<uint8_t>& bgra) {
        bgra.resize(size_t(width) * height * 4u);
        const uint8_t* luma = nv12;
        const uint8_t* chroma = nv12 + size_t(width) * height;
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* chromaRow = chroma + size_t(y / 2u) * width;
            uint8_t* out = bgra.data() + size_t(y) * width * 4u;
            for (uint32_t x = 0; x < width; ++x) {
                const double luminance = (double(luma[size_t(y) * width + x]) - 16.0) / 219.0;
                const double blueDiff = (double(chromaRow[(x & ~1u)]) - 128.0) / 224.0;
                const double redDiff = (double(chromaRow[(x & ~1u) + 1u]) - 128.0) / 224.0;
                const double red = luminance + 1.5748 * redDiff;
                const double green = luminance - 0.1873 * blueDiff - 0.4681 * redDiff;
                const double blue = luminance + 1.8556 * blueDiff;
                const auto clamp8 = [](double value) {
                    return uint8_t(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
                };
                out[size_t(x) * 4u + 0u] = clamp8(blue);
                out[size_t(x) * 4u + 1u] = clamp8(green);
                out[size_t(x) * 4u + 2u] = clamp8(red);
                out[size_t(x) * 4u + 3u] = 255u;
            }
        }
    };

    // Wide enough to cross several parallel row bands, and the content walks
    // the whole 0..255 range on all three planes so every table entry and both
    // clamp arms are exercised - the out-of-gamut corners are where a
    // fixed-point rewrite would have drifted.
    constexpr uint32_t width = 64, height = 96;
    std::vector<uint8_t> frame(size_t(width) * height + size_t(width) * height / 2u);
    uint32_t state = 12345u;
    for (auto& byte : frame) {
        state = state * 1664525u + 1013904223u;
        byte = uint8_t(state >> 24);
    }

    std::vector<uint8_t> expected, actual;
    scalar(frame.data(), width, height, expected);
    nv12::ToBgraBt709Limited(frame.data(), width, height, actual);
    CHECK_EQ(expected.size(), actual.size());
    CHECK(expected == actual);

    // The extremes on their own, so a failure points at a value rather than at
    // a pseudo-random offset.
    for (const uint8_t fill : {uint8_t{0}, uint8_t{16}, uint8_t{128}, uint8_t{235}, uint8_t{255}}) {
        std::vector<uint8_t> flat(frame.size(), fill);
        scalar(flat.data(), width, height, expected);
        nv12::ToBgraBt709Limited(flat.data(), width, height, actual);
        CHECK(expected == actual);
    }

    // Refusals are part of the contract: odd geometry has no half-resolution
    // chroma plane, and the caller relies on the buffer being left alone.
    std::vector<uint8_t> untouched{1, 2, 3};
    nv12::ToBgraBt709Limited(frame.data(), 63, 96, untouched);
    CHECK_EQ(size_t{3}, untouched.size());
    nv12::ToBgraBt709Limited(nullptr, width, height, untouched);
    CHECK_EQ(size_t{3}, untouched.size());
}

// LookupRender verifies the full SHA-256 of the payload and both sidecars
// before any of this runs, so the entry is known intact. The player then
// ProbeMedia'd it and treated any failure as the entry being wrong - including
// the probe not running at all, which MediaPipeline reports for something as
// ordinary as the helper not resolving. Quarantine moves the entry to
// staging/invalid-cache-*, and the sweep deletes any `invalid*` name
// unconditionally on the next manager construction. An antivirus holding the
// unsigned ffprobe.exe therefore destroyed hours of hash-verified GPU time as
// the user opened each of their rendered videos.
void a_probe_that_could_not_run_does_not_condemn_a_cached_render_test()
{
    using namespace cached_render;
    const Evidence good{true, true, true, true};
    CHECK(Judge(good) == Verdict::Serve);

    // The probe ran and contradicted the manifest: the entry really is wrong.
    Evidence wrongGeometry = good; wrongGeometry.geometryMatches = false;
    CHECK(Judge(wrongGeometry) == Verdict::Discard);
    Evidence wrongDuration = good; wrongDuration.durationMatches = false;
    CHECK(Judge(wrongDuration) == Verdict::Discard);

    // The probe could not run. Nothing was learned, so the entry is not served
    // and not destroyed either.
    Evidence noProbe = good; noProbe.probeRan = false;
    CHECK(Judge(noProbe) == Verdict::Unverified);

    // Not even when the probe-only fields are default-false, which is what they
    // are whenever ProbeMedia fails closed - the case that caused the loss.
    CHECK(Judge(Evidence{false, true, false, false}) == Verdict::Unverified);

    // The manifest comparison needs no child process. A mismatch there
    // condemns the entry whether or not the probe ran, so a stale entry is
    // still retired while ffprobe is broken.
    CHECK(Judge(Evidence{false, false, false, false}) == Verdict::Discard);
    Evidence wrongManifest = good; wrongManifest.manifestMatches = false;
    CHECK(Judge(wrongManifest) == Verdict::Discard);
}

// Two player instances shared cacheRoot/live. Enabling neural rendering in the
// second ran remove_all over the first instance's finalized segments, and the
// first kept reporting them covered because NeuralSegmentIndex is in-memory
// arithmetic - so it seeked into "covered" ground and failed to open the file.
// Both then wrote job1/neural-00000.mkv into the same directory, because the
// run id is a per-process counter.
//
// Separately, m_cacheRoot is only assigned when a writable root was prepared.
// Without one the path became the relative "live" and that same remove_all ran
// against the process working directory - on a portable install on a read-only
// share, which is the exact case the LocalAppData fallback exists for.
void live_session_directory_is_per_process_and_never_relative_test()
{
    using live_session::SessionDirectory;
    const std::filesystem::path root = L"D:\\cache\\v1";

    // Named after the process, so one instance's removal cannot reach another's.
    const auto mine = SessionDirectory(root, 4242);
    const auto theirs = SessionDirectory(root, 9001);
    CHECK(mine != theirs);
    CHECK(mine.is_absolute());
    CHECK_EQ(root / L"live" / L"pid4242", mine);

    // Both still live under the root, so Clear and the size accounting can find
    // them by walking one directory.
    CHECK_EQ(root / L"live", mine.parent_path());
    CHECK_EQ(root / L"live", theirs.parent_path());

    // No root means no live directory - not a relative one. The caller has to
    // read this as "no live session is possible"; anything else would put a
    // recursive delete on the working directory.
    CHECK(SessionDirectory({}, 4242).empty());
    CHECK(!SessionDirectory({}, 4242).is_absolute());

    // Same process, same answer: the directory is recomputed on every session
    // and has to agree with the one the previous session removed.
    CHECK_EQ(mine, SessionDirectory(root, 4242));
}

// The player runs on an audio master clock. When the reader thread ends - pipe
// EOF, the ffmpeg child dying, waveOutWrite failing after a device change - the
// queued buffers drain and waveOutGetPosition freezes, but hasAudioData stays
// set until Stop(), so PositionSeconds kept returning the frozen value. The
// presentation gate holds each frame until the clock reaches its due time, so
// video stopped for the rest of the file: a 60 s video with a 10 s audio track
// played 10.7 s and then showed nothing, with no error and no log line.
void audio_clock_stops_being_the_master_once_it_stops_advancing_test()
{
    using namespace audio_clock;
    StallState state;

    // A clock that advances is a clock, however slowly. waveOutGetPosition
    // quantizes to about 10 ms, so repeats between moves are normal and must
    // not on their own condemn it.
    CHECK(Usable(state, 1.000, 100.0, true));
    CHECK(Usable(state, 1.000, 100.005, true));
    CHECK(Usable(state, 1.010, 100.010, true));
    CHECK(Usable(state, 1.010, 100.4, true));
    CHECK(Usable(state, 1.020, 100.5, true));

    // Standing still past the window is not.
    CHECK(Usable(state, 1.020, 100.5 + kStallSeconds * 0.99, true));
    CHECK(!Usable(state, 1.020, 100.5 + kStallSeconds, true));
    CHECK(!Usable(state, 1.020, 200.0, true));

    // Moving again restores it: the player already switches between the audio
    // and steady clocks when audio starts and stops, and one underrun should
    // not demote audio for the rest of a film.
    CHECK(Usable(state, 1.030, 200.1, true));
    CHECK(Usable(state, 1.030, 200.2, true));

    // A paused clock standing still is correct, not stalled - and the window
    // starts again from the resume rather than counting the pause against it.
    StallState paused;
    CHECK(Usable(paused, 5.0, 0.0, true));
    for (double t = 0.0; t <= 60.0; t += 1.0) CHECK(Usable(paused, 5.0, t, false));
    // Resumed at t=60 and still standing still: inside the window from the
    // resume, not from whenever the clock last moved an hour of pause ago.
    CHECK(Usable(paused, 5.0, 60.0 + kStallSeconds * 0.99, true));
    CHECK(!Usable(paused, 5.0, 60.0 + kStallSeconds, true));

    // A seek moves the position backwards. That is movement.
    StallState seeking;
    CHECK(Usable(seeking, 90.0, 0.0, true));
    CHECK(!Usable(seeking, 90.0, kStallSeconds, true));
    CHECK(Usable(seeking, 12.0, kStallSeconds, true));

    // Reset is what Start and Seek use, and it must not leave the old
    // stand-still time behind for the new session to inherit.
    StallState reused;
    CHECK(Usable(reused, 7.0, 500.0, true));
    CHECK(!Usable(reused, 7.0, 500.0 + kStallSeconds, true));
    Reset(reused);
    CHECK(Usable(reused, 7.0, 500.0 + kStallSeconds, true));
}

// A resident helper serves several jobs from one process. Whenever the next
// job's geometry, fps, source layout, colour conversion or capture format
// differs from the last, Initialize calls Release, which shuts the capture
// worker down - and the worker then has to serve the new job. It did not: the
// quit latch was never cleared, so the restarted thread exited after one copy
// and left a joinable-but-finished thread that the Post after it declined to
// replace. That Post notified nobody and the next Join waited forever, wedging
// the helper on the third captured frame of the second job while it held the
// D3D12 device and about a GiB of DLSS feature memory. There is no overall
// timeout on the parent side, so the render never completed.
void deferred_capture_serves_a_second_job_after_a_shutdown_test()
{
    struct View { int id{}; };
    struct Copy {
        void operator()(const View& view, std::vector<uint8_t>& pixels) const
        {
            pixels.assign(4, static_cast<uint8_t>(view.id));
        }
    };
    using Worker = DeferredCaptureWorker<View, Copy>;

    // Leaked deliberately if the sequence wedges: a deadlocked worker can never
    // be destroyed, and ~DeferredCaptureWorker would block the test process on
    // the join its own Shutdown can no longer reach.
    auto* worker = new Worker();

    std::promise<std::vector<uint8_t>> result;
    auto finished = result.get_future();
    std::thread runner([worker, &result] {
        std::vector<uint8_t> pixels;
        // Job 1, then the Release between jobs.
        worker->Post(View{1}, std::vector<uint8_t>(4));
        worker->Join(pixels);
        worker->Shutdown();

        // Job 2. The first Post restarts a thread; the second is the one that
        // used to find it finished-but-joinable and start nothing.
        worker->Post(View{2}, std::vector<uint8_t>(4));
        worker->Join(pixels);
        worker->Post(View{3}, std::vector<uint8_t>(4));
        worker->Join(pixels);
        result.set_value(pixels);
    });

    if (finished.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        CHECK(false && "second job wedged: Join never returned");
        runner.detach();
        return;
    }
    runner.join();

    // The bytes have to come from the third Post, not a stale buffer left by
    // the second: a worker that never ran would return the previous contents.
    const auto pixels = finished.get();
    CHECK_EQ(size_t{4}, pixels.size());
    if (pixels.size() == 4) CHECK_EQ(3, int(pixels[0]));
    delete worker;
}

constexpr test_support::TestCase kCases[] = {
    TEST_CASE(harness_isolates_a_failing_case_from_the_ones_after_it_test),
    TEST_CASE(youtube_bitrate_selection_uses_real_helper_without_network_test),
    TEST_CASE(runtime_shutdown_releases_player_before_media_foundation_and_com_test),
    TEST_CASE(runtime_shutdown_rethrows_only_after_single_ordered_cleanup_test),
    TEST_CASE(toolbar_layout_selects_stable_action_sets_for_width_modes_test),
    TEST_CASE(toolbar_layout_preserves_group_separation_test),
    TEST_CASE(toolbar_layout_scales_hit_height_and_avoids_overlap_test),
    TEST_CASE(toolbar_hit_testing_is_half_open_and_boundary_stable_test),
    TEST_CASE(minimum_toolbar_client_width_owns_required_target_floor_across_dpi_test),
    TEST_CASE(volume_slider_never_intersects_compact_or_threshold_toolbar_test),
    TEST_CASE(toolbar_focus_order_includes_idle_open_and_skips_disabled_actions_test),
    TEST_CASE(frame_generation_pill_follows_media_and_availability_only_test),
    TEST_CASE(open_action_content_keeps_idle_and_toolbar_copy_distinct_test),
    TEST_CASE(focused_toolbar_action_reconciles_layout_and_availability_changes_test),
    TEST_CASE(idle_surface_exposes_file_and_disabled_youtube_without_focusing_it_test),
    TEST_CASE(dpi_change_suggested_rect_respects_new_monitor_minimum_track_size_test),
    TEST_CASE(player_status_formats_exact_runtime_and_playback_states_test),
    TEST_CASE(playback_timeline_follows_the_presented_frame_test),
    TEST_CASE(playback_lateness_is_bounded_to_one_and_a_half_frames_test),
    TEST_CASE(long_media_title_is_bounded_with_a_real_ellipsis_test),
    TEST_CASE(recovery_copy_and_rehook_confirmation_are_actionable_test),
    TEST_CASE(unchanged_hover_action_has_no_dirty_rectangles_test),
    TEST_CASE(changed_hover_action_dirties_only_present_old_and_new_actions_test),
    TEST_CASE(hover_resolution_tracks_layout_action_changes_and_disappearance_test),
    TEST_CASE(current_cursor_hover_clears_when_cursor_query_is_unavailable_test),
    TEST_CASE(paint_buffer_layout_uses_only_the_clipped_nonzero_paint_rectangle_test),
    TEST_CASE(tabler_glyph_mapping_uses_the_pinned_css_codepoints_test),
    TEST_CASE(native_button_palette_has_distinct_interaction_states_test),
    TEST_CASE(active_button_small_text_meets_wcag_contrast_test),
    TEST_CASE(failed_icon_font_uses_label_only_presentation_test),
    TEST_CASE(button_content_layout_preserves_required_insets_and_icon_gap_at_every_dpi_test),
    TEST_CASE(button_content_layout_centers_combined_icon_and_label_without_outline_contact_test),
    TEST_CASE(prerender_surface_layout_keeps_progress_cancel_and_text_inside_client_bounds_test),
    TEST_CASE(advanced_menu_contains_clear_neural_cache_and_no_removed_quality_commands_test),
    TEST_CASE(feature_menu_uses_distinct_controls_and_honest_availability_test),
    TEST_CASE(debug_view_popup_contains_all_existing_views_and_selection_test),
    TEST_CASE(range_preview_and_comparison_menus_route_keys_and_gate_availability_test),
    TEST_CASE(player_menu_is_english_only_and_retains_advanced_commands_test),
    TEST_CASE(youtube_source_quality_menu_is_distinct_radio_group_and_updates_test),
    TEST_CASE(youtube_availability_drives_real_menu_and_idle_action_consistently_test),
    TEST_CASE(youtube_resolution_generation_accepts_only_the_current_completion_test),
    TEST_CASE(youtube_resolution_disables_only_conflicting_source_actions_test),
    TEST_CASE(youtube_resolution_error_mapping_is_actionable_and_distinct_test),
    TEST_CASE(youtube_source_forces_ffmpeg_and_never_allows_media_foundation_fallback_test),
    TEST_CASE(youtube_resolution_cancellation_runs_stop_cancel_join_in_order_test),
    TEST_CASE(youtube_display_and_log_labels_never_expose_direct_urls_test),
    TEST_CASE(youtube_real_menu_and_ctrl_l_route_share_the_enabled_action_test),
    TEST_CASE(fixed_youtube_examples_are_complete_safe_and_menu_routable_test),
    TEST_CASE(youtube_completion_registry_is_scalar_once_only_and_spoof_safe_test),
    TEST_CASE(youtube_completion_registry_post_failure_and_concurrency_are_owned_test),
    TEST_CASE(youtube_renderer_transaction_validates_every_open_seek_and_quality_candidate_geometry_test),
    TEST_CASE(youtube_renderer_transaction_validates_before_atomic_handoff_and_rolls_back_test),
    TEST_CASE(youtube_candidate_seek_render_failure_preserves_all_active_state_before_commit_test),
    TEST_CASE(youtube_network_read_decisions_are_identical_and_once_only_at_both_positions_test),
    TEST_CASE(youtube_async_transaction_coalesces_and_discards_stale_work_before_handoff_test),
    TEST_CASE(youtube_stale_and_cancelled_prepared_seek_ownership_is_destroyed_once_test),
    TEST_CASE(youtube_decoder_probe_and_frame_reads_are_bounded_nonblocking_test),
    TEST_CASE(video_decoder_prefers_video_duration_tag_over_longer_container_test),
    TEST_CASE(youtube_decoder_partial_stall_cancel_and_exit_leave_no_children_test),
    TEST_CASE(youtube_decoder_discards_only_expected_trailing_partial_frame_test),
    TEST_CASE(youtube_decoder_background_seek_trickles_and_cancels_boundedly_test),
    TEST_CASE(video_decoder_close_releases_a_blocked_blocking_read_test),
    TEST_CASE(video_decoder_hardware_failure_falls_back_to_software_test),
    TEST_CASE(video_decoder_remembers_dead_hardware_paths_test),
    TEST_CASE(video_decoder_drains_complete_raw_frame_buffered_after_child_exit_test),
    TEST_CASE(video_decoder_background_queue_is_bounded_to_four_frames_test),
    TEST_CASE(video_decoder_close_returns_promptly_when_local_queue_thread_is_blocked_on_pipe_read_test),
    TEST_CASE(video_decoder_open_sequential_selects_nv12_for_even_geometry_test),
    TEST_CASE(video_decoder_open_sequential_can_keep_bgra_for_even_geometry_test),
    TEST_CASE(video_decoder_open_sequential_stays_bgra_for_odd_geometry_test),
    TEST_CASE(video_decoder_open_sequential_refuses_nv12_for_an_unconvertible_source_test),
    TEST_CASE(video_decoder_open_sequential_keeps_nv12_for_a_declared_source_test),
    TEST_CASE(video_decoder_swap_carries_probe_derived_state_test),
    TEST_CASE(source_nv12_conversion_constants_are_the_shipped_coefficients_test),
    TEST_CASE(source_nv12_conversion_compiles_a_distinct_program_per_arm_test),
    TEST_CASE(video_decoder_forward_seek_reuses_child_and_delivers_the_same_frame_as_a_restart_test),
    TEST_CASE(video_decoder_resume_failures_are_bounded_and_leak_free_for_local_and_network_startup_test),
    TEST_CASE(youtube_audio_held_pipe_stop_destroy_and_failure_fallback_are_bounded_test),
    TEST_CASE(youtube_audio_failed_waits_and_query_retire_reader_without_termination_or_leaks_test),
    TEST_CASE(youtube_prepared_audio_starts_silent_and_handoff_has_no_overlap_test),
    TEST_CASE(youtube_prepared_handoff_shows_candidate_and_retires_every_old_owner_before_activation_test),
    TEST_CASE(youtube_prepared_handoff_sizes_and_shows_real_candidate_before_owned_retirement_test),
    TEST_CASE(youtube_prepared_window_api_failures_are_reported_before_commit_test),
    TEST_CASE(youtube_destroyed_window_and_visibility_failure_leave_active_state_unchanged_test),
    TEST_CASE(youtube_candidate_render_failure_releases_window_handle_and_prepared_processes_test),
    TEST_CASE(legacy_language_configuration_is_ignored_and_english_lookup_remains_builtin_test),
    TEST_CASE(eviction_removes_entries_that_can_never_match_a_key_again_test),
    TEST_CASE(eviction_keeps_everything_reusable_while_the_disk_has_room_test),
    TEST_CASE(eviction_frees_the_least_recently_used_until_the_floor_is_met_test),
    TEST_CASE(eviction_never_touches_an_active_entry_test),
    TEST_CASE(eviction_frees_what_it_can_when_the_floor_is_unreachable_test),
    TEST_CASE(a_zero_floor_evicts_only_the_dead_test),
    TEST_CASE(module_path_grows_past_max_path_test),
    TEST_CASE(module_path_never_accepts_a_filled_buffer_test),
    TEST_CASE(module_path_reports_a_failed_query_test),
    TEST_CASE(module_directory_is_the_parent_of_the_module_test),
    TEST_CASE(module_directory_answers_for_this_process_test),
    TEST_CASE(renderer_recovery_rebuilds_into_a_fresh_window_test),
    TEST_CASE(renderer_recovery_keeps_a_retained_renderers_window_test),
    TEST_CASE(renderer_recovery_destroys_the_old_window_once_its_renderer_is_gone_test),
    TEST_CASE(renderer_recovery_without_a_window_does_not_initialize_test),
    TEST_CASE(renderer_recovery_reports_the_new_window_after_a_failed_initialize_test),
    TEST_CASE(gpu_teardown_fence_signal_failure_stops_before_event_registration_test),
    TEST_CASE(gpu_teardown_fence_signal_failure_maps_to_device_removed_when_device_reason_failed_test),
    TEST_CASE(gpu_teardown_fence_event_registration_failure_stops_before_wait_test),
    TEST_CASE(gpu_teardown_fence_wait_failure_is_bounded_and_reported_test),
    TEST_CASE(gpu_teardown_fence_timeout_is_bounded_and_reported_test),
    TEST_CASE(gpu_render_fence_wait_uses_the_render_budget_and_reports_device_loss_first_test),
    TEST_CASE(gpu_teardown_fence_ignores_old_event_wake_until_new_target_completes_test),
    TEST_CASE(gpu_teardown_fence_consecutive_timeout_does_not_let_old_registration_complete_new_target_test),
    TEST_CASE(gpu_teardown_fence_device_removed_sentinel_is_not_completion_test),
    TEST_CASE(gpu_teardown_fence_stale_wakes_share_one_absolute_timeout_budget_test),
    TEST_CASE(renderer_non_teardown_wait_failure_is_propagated_test),
    TEST_CASE(renderer_safe_owner_releases_owned_resources_only_after_completed_or_removed_drain_test),
    TEST_CASE(renderer_safe_owner_retains_resources_after_live_device_drain_failure_test),
    TEST_CASE(renderer_second_retained_renderer_ends_the_process_test),
    TEST_CASE(renderer_frame_signal_failure_is_cached_without_advancing_tracking_test),
    TEST_CASE(renderer_frame_signal_device_removal_is_cached_and_safe_owner_releases_test),
    TEST_CASE(renderer_frame_signal_device_loss_code_latches_device_removed_test),
    TEST_CASE(renderer_frame_signal_success_advances_tracking_once_test),
    TEST_CASE(renderer_cache_capture_requires_a_successful_neural_evaluation_test),
    TEST_CASE(renderer_cache_capture_returns_exact_tight_bgra_geometry_test),
    TEST_CASE(renderer_cache_capture_wait_failure_never_exposes_partial_bytes_test),
    TEST_CASE(renderer_cache_capture_does_not_apply_playback_color_adjustments_test),
    TEST_CASE(gpu_classification_table_test),
    TEST_CASE(adapter_luid_identity_compares_parts_not_model_names_test),
    TEST_CASE(detected_high_performance_gpu_carries_the_luid_of_the_adapter_it_describes_test),
    TEST_CASE(nvidia_driver_version_is_read_out_of_the_dxgi_quad_test),
    TEST_CASE(neural_driver_floor_separates_the_failing_machine_from_the_working_ones_test),
    TEST_CASE(neural_addon_policy_test),
    TEST_CASE(neural_addon_is_gated_by_the_driver_floor_not_by_the_generation_test),
    TEST_CASE(render_pace_prior_zero_means_unmeasured_not_unsupported_test),
    TEST_CASE(ada_render_pace_prior_forecasts_both_ends_of_the_measured_bracket_test),
    TEST_CASE(frame_generation_plan_follows_the_panel_not_just_the_source_test),
    TEST_CASE(frame_generation_reaches_every_multiplier_the_verified_ceiling_admits_test),
    TEST_CASE(display_cadence_spread_is_one_refresh_period_or_nothing_test),
    TEST_CASE(refresh_switch_offer_names_the_mode_that_removes_the_pulldown_test),
    TEST_CASE(neural_prerender_defaults_prefer_1080p_and_preserve_explicit_output_test),
    TEST_CASE(neural_playback_lifecycle_accepts_its_generation_and_reaches_ready_test),
    TEST_CASE(neural_playback_lifecycle_runs_render_validate_then_ready_test),
    TEST_CASE(neural_completion_publishes_only_after_probe_and_manifest_validation_test),
    TEST_CASE(neural_publish_tolerance_admits_one_muxer_rounding_per_joined_segment_test),
    TEST_CASE(render_range_residual_below_one_frame_is_coverage_not_work_test),
    TEST_CASE(coverage_merge_sorts_drops_degenerate_and_joins_touching_spans_test),
    TEST_CASE(uncovered_spans_of_nothing_is_everything_and_of_everything_is_nothing_test),
    TEST_CASE(uncovered_spans_find_the_hole_between_regions_and_the_lead_in_before_the_first_test),
    TEST_CASE(uncovered_spans_clip_coverage_to_the_range_and_ignore_coverage_outside_it_test),
    TEST_CASE(uncovered_spans_drop_sub_frame_holes_but_keep_a_hole_one_frame_wide_test),
    TEST_CASE(next_render_target_clips_the_hole_under_the_playhead_to_the_playhead_test),
    TEST_CASE(next_render_target_prefers_the_nearest_hole_ahead_then_the_earliest_behind_test),
    TEST_CASE(next_render_target_without_a_hole_has_nothing_to_render_test),
    TEST_CASE(span_containing_returns_the_playable_region_not_a_later_disjoint_one_test),
    TEST_CASE(covered_duration_counts_only_rendered_video_inside_the_range_test),
    TEST_CASE(neural_cancel_and_failure_offer_original_only_without_partial_cache_test),
    TEST_CASE(neural_pause_suspends_rendering_and_resumes_without_advancing_test),
    TEST_CASE(neural_recovery_resolves_to_rendering_failed_or_retry_exhausted_test),
    TEST_CASE(neural_failure_kind_selects_the_lifecycle_state_test),
    TEST_CASE(neural_progress_phase_drives_the_lifecycle_through_pause_and_recovery_test),
    TEST_CASE(dlss_toggle_in_cached_playback_changes_comparison_view_not_renderer_feature_test),
    TEST_CASE(neural_runtime_layout_is_absent_complete_or_fail_closed_test),
    TEST_CASE(default_neural_carrier_uses_native_resolution_dlaa_test),
    TEST_CASE(windows_command_line_quoting_round_trip_test),
    TEST_CASE(runtime_argument_parsing_preserves_user_arguments_and_strips_markers_test),
    TEST_CASE(restart_argument_lifecycle_and_create_process_command_line_test),
    TEST_CASE(advanced_safe_mode_normal_invocation_adds_safe_mode_test),
    TEST_CASE(advanced_safe_mode_cancel_keeps_current_open_without_launch_test),
    TEST_CASE(advanced_safe_mode_launch_failure_keeps_current_open_test),
    TEST_CASE(advanced_safe_mode_launch_success_closes_with_sanitized_arguments_test),
    TEST_CASE(disabled_addons_creates_missing_addon_section_test),
    TEST_CASE(disabled_addons_updates_empty_and_populated_lists_test),
    TEST_CASE(disabled_addons_preserves_mixed_line_endings_and_unrelated_sections_test),
    TEST_CASE(disabled_addons_removes_only_exact_target_entries_test),
    TEST_CASE(disabled_addons_collapses_only_exact_target_duplicates_test),
    TEST_CASE(disabled_addons_matches_trimmed_tokens_without_changing_retained_whitespace_test),
    TEST_CASE(reshade_68_disabled_addon_token_conformance_test),
    TEST_CASE(reshade_68_aliases_migrate_to_one_canonical_token_test),
    TEST_CASE(reshade_68_section_and_key_lookup_are_case_sensitive_test),
    TEST_CASE(reshade_68_utf8_bom_is_ignored_for_lookup_and_preserved_test),
    TEST_CASE(disabled_addons_insertion_uses_target_section_line_ending_test),
    TEST_CASE(neural_addon_runtime_settings_enable_neural_and_disable_upscaling_test),
    TEST_CASE(neural_addon_runtime_settings_are_created_without_enabling_upscaling_test),
    TEST_CASE(neural_addon_runtime_settings_fail_closed_on_duplicate_managed_keys_test),
    TEST_CASE(reshade_trailing_section_text_uses_reshade_section_boundaries_test),
    TEST_CASE(configure_neural_addon_is_idempotent_test),
    TEST_CASE(configure_neural_addon_reports_semantic_state_across_text_canonicalization_test),
    TEST_CASE(configure_neural_addon_safe_then_normal_observes_reshade_state_test),
    TEST_CASE(evaluated_config_update_observes_actual_final_bytes_test),
    TEST_CASE(configure_neural_addon_fails_closed_for_malformed_ini_test),
    TEST_CASE(configure_neural_addon_rejects_non_regular_path_before_replacement_test),
    TEST_CASE(youtube_url_validation_accepts_only_supported_video_routes_test),
    TEST_CASE(youtube_url_validation_rejects_unsafe_or_unselected_inputs_test),
    TEST_CASE(youtube_watch_query_requires_one_unambiguous_lowercase_v_field_test),
    TEST_CASE(youtube_video_id_must_be_exactly_eleven_characters_test),
    TEST_CASE(youtube_url_validation_enforces_exact_2048_character_boundary_test),
    TEST_CASE(resolver_output_accepts_one_https_googlevideo_url_and_trims_crlf_test),
    TEST_CASE(resolver_output_accepts_separate_https_video_and_audio_urls_test),
    TEST_CASE(resolver_output_accepts_authoritative_duration_before_stream_urls_test),
    TEST_CASE(resolver_output_rejects_invalid_duration_live_status_and_metadata_framing_test),
    TEST_CASE(resolver_output_rejects_empty_multiple_oversize_or_untrusted_urls_test),
    TEST_CASE(resolver_output_enforces_raw_16k_and_single_trailing_line_ending_test),
    TEST_CASE(resolver_nonzero_exit_returns_fixed_generic_non_url_detail_test),
    TEST_CASE(youtube_resolver_windows_argument_quoting_covers_empty_spaces_quotes_and_slashes_test),
    TEST_CASE(youtube_resolver_argument_vector_is_exact_and_ordered_test),
    TEST_CASE(youtube_source_quality_selectors_pin_exact_rungs_and_cap_auto_at_1440_test),
    TEST_CASE(resolver_metadata_reports_selected_height_video_bitrate_and_age_limit_test),
    TEST_CASE(youtube_resolver_success_uses_beside_app_helpers_and_exact_child_arguments_test),
    TEST_CASE(youtube_resolver_waits_until_both_selected_streams_are_available_test),
    TEST_CASE(youtube_resolver_availability_wait_is_cancellable_and_deadline_bounded_test),
    TEST_CASE(youtube_resolver_waits_for_fractional_stream_availability_test),
    TEST_CASE(resolver_output_validates_stream_availability_metadata_test),
    TEST_CASE(youtube_resolver_requires_duration_metadata_before_acquisition_test),
    TEST_CASE(youtube_resolver_reports_missing_and_unstartable_helpers_without_sensitive_data_test),
    TEST_CASE(youtube_resolver_maps_nonzero_exit_and_output_overflow_precisely_test),
    TEST_CASE(youtube_resolver_honors_stop_token_and_explicit_cancel_with_bounded_wait_test),
    TEST_CASE(youtube_resolver_times_out_and_kills_its_descendant_job_tree_test),
    TEST_CASE(youtube_resolver_repeated_runs_leave_process_handle_count_stable_test),
    TEST_CASE(youtube_resolver_rejects_reparse_points_and_nonregular_helpers_before_execution_test),
    TEST_CASE(youtube_resolver_holds_verified_helpers_against_replacement_until_completion_test),
    TEST_CASE(youtube_resolver_forces_package_local_deno_cache_over_parent_override_test),
    TEST_CASE(youtube_resolver_disables_default_plugin_execution_from_inherited_config_test),
    TEST_CASE(youtube_resolver_serializes_queued_resolve_and_cancel_does_not_poison_reuse_test),
    TEST_CASE(youtube_resolver_queued_stop_token_cancels_before_launch_test),
    TEST_CASE(youtube_resolver_injected_startup_and_drain_failures_cleanup_boundedly_test),
    TEST_CASE(youtube_resolver_repeated_owned_pipe_failures_cannot_hide_two_handle_leaks_test),
    TEST_CASE(youtube_resolver_repeated_timeout_cancel_overflow_cycles_are_leak_free_test),
    TEST_CASE(ngx_same_device_overlapping_sessions_initialize_and_shutdown_once_test),
    TEST_CASE(ngx_failed_initialization_never_acquires_a_session_test),
    TEST_CASE(ngx_failed_candidate_setup_releases_only_its_overlapping_lease_test),
    TEST_CASE(ngx_distinct_devices_own_independent_sessions_test),
    TEST_CASE(ngx_create_failure_is_not_retried_until_explicit_reset_test),
    TEST_CASE(ngx_renderer_frame_state_prioritizes_explicit_rehook_after_create_failure_test),
    TEST_CASE(ngx_live_feature_is_never_released_on_a_frame_count_test),
    TEST_CASE(spawning_a_corrupt_helper_fails_closed_without_a_hard_error_dialog_test),
    TEST_CASE(live_session_sizes_the_cushion_from_the_render_drain_test),
    TEST_CASE(playback_cadence_thins_presentation_before_it_seeks_test),
    TEST_CASE(playback_cadence_phase_presents_exactly_one_pair_in_stride_test),
    TEST_CASE(playback_cadence_reports_a_rate_no_cadence_can_follow_test),
    TEST_CASE(deferred_capture_serves_a_second_job_after_a_shutdown_test),
    TEST_CASE(audio_clock_stops_being_the_master_once_it_stops_advancing_test),
    TEST_CASE(live_session_directory_is_per_process_and_never_relative_test),
    TEST_CASE(a_probe_that_could_not_run_does_not_condemn_a_cached_render_test),
    TEST_CASE(nv12_reference_conversion_is_bit_identical_to_the_scalar_original_test),
    TEST_CASE(neural_presets_round_trip_and_default_to_the_shipped_settings_test),
    TEST_CASE(swapchain_never_asks_for_the_frame_latency_waitable_object_test),
};


} // namespace

int wmain(int argc, wchar_t* argv[])
{
    const std::wstring executableName=current_test_executable().filename().wstring();
    if(_wcsicmp(executableName.c_str(),L"ffprobe.exe")==0||_wcsicmp(executableName.c_str(),L"ffmpeg.exe")==0)return run_fake_media_child(argc,argv);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--resolver-availability-tests") {
        test_support::run_cases(kResolverAvailabilityCases, std::size(kResolverAvailabilityCases), {});
        return test_support::failure_count == 0 ? 0 : 1;
    }
    // `--only=<text>` runs the cases whose name contains the text.
    std::string only;
    if (argc == 2 && std::wstring_view(argv[1]).starts_with(L"--only=")) {
        for (const wchar_t character : std::wstring_view(argv[1]).substr(7)) only.push_back(static_cast<char>(character));
        if (only.empty()) {
            std::cerr << "--only= needs part of a case name\n";
            return EXIT_FAILURE;
        }
    } else if (argc > 1) {
        return run_fake_resolver_child(argc, argv);
    }
    const size_t ran = test_support::run_cases(kCases, std::size(kCases), only).ran;
    if (ran == 0) {
        std::cerr << "no case name contains '" << only << "'\n";
        return EXIT_FAILURE;
    }

    if (test_support::failure_count != 0) {
        std::cerr << test_support::failure_count << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "PolicyTests: " << ran << " cases, all assertions passed\n";
    return EXIT_SUCCESS;
}
