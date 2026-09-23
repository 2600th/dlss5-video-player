#include <windows.h>
#include <string>
#include <string_view>
#include <filesystem>
#include <vector>
#include "TestSupport.h"
#include "TestEnvironment.h"
#include "GpuTestGate.h"

namespace {
std::vector<std::wstring> drawnText;
std::wstring lastMessageBox;
int messageBoxes = 0;
// What every captured box answers; a yes/no question reads IDYES as yes.
int messageBoxAnswer = IDOK;

int WINAPI CaptureDrawText(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT format)
{
    drawnText.emplace_back(text, count < 0 ? std::wcslen(text) : static_cast<size_t>(count));
    return DrawTextW(dc, text, count, rect, format);
}

int WINAPI CaptureMessageBox(HWND, LPCWSTR text, LPCWSTR, UINT)
{
    lastMessageBox = text ? text : L"";
    ++messageBoxes;
    return messageBoxAnswer;
}
} // namespace

// Observe the Win32 boundary, leaving player layout and lifecycle code intact.
#define DrawTextW CaptureDrawText
#define MessageBoxW CaptureMessageBox
#include "../src/main.cpp"
#undef MessageBoxW
#undef DrawTextW

namespace {
// Serves numbered 1x1 frames from `firstFrame` on, without end, as either
// member of a live pair.
class NumberedFrameSource final : public ISynchronizedFrameSource {
public:
    explicit NumberedFrameSource(uint64_t firstFrame) : first_(firstFrame) {}
    bool Open(const std::filesystem::path&, std::stop_token) override { index_ = 0; return true; }
    void Close() override {}
    VideoReadResult Read(VideoFrame& frame, std::stop_token) override
    {
        frame = VideoFrame{};
        frame.bgra = {0, 0, 0, 255};
        frame.frameNumber = first_ + index_++;
        frame.timestamp100ns = int64_t(frame.frameNumber) * 333333;
        return VideoReadResult::FrameReady;
    }
    bool SeekSeconds(double seconds) override { index_ = uint64_t(seconds * 30.0); return true; }
    uint32_t Width() const override { return 1; }
    uint32_t Height() const override { return 1; }
    double FrameRate() const override { return 30.0; }
    double DurationSeconds() const override { return 3600.0; }
private:
    uint64_t first_, index_{};
};

// A segment member that never hands a frame over. Live pairing reports
// SynchronizedReadResult::NotReady for exactly this - a segment decoder that is
// reopening - so it is what a wedged pair looks like from ReadNextCachedFrame.
class NeverReadyFrameSource final : public ISynchronizedFrameSource {
public:
    bool Open(const std::filesystem::path&, std::stop_token) override { return true; }
    void Close() override {}
    VideoReadResult Read(VideoFrame&, std::stop_token) override { return VideoReadResult::NotReady; }
    bool SeekSeconds(double) override { return true; }
    uint32_t Width() const override { return 1; }
    uint32_t Height() const override { return 1; }
    double FrameRate() const override { return 30.0; }
    double DurationSeconds() const override { return 3600.0; }
};

// An uncompressed RGB32 AVI of a few frames: the smallest file Media
// Foundation describes without a codec or a helper process, which is how a
// test gives the player's decoder a real geometry and frame rate.
void WriteTinyAvi(const std::filesystem::path& path, uint32_t width, uint32_t height, uint32_t fps)
{
    constexpr uint32_t frames = 3;
    const uint32_t frameBytes = width * height * 4;
    std::vector<uint8_t> file;
    const auto put32 = [&](uint32_t value) { for (int shift = 0; shift < 32; shift += 8) file.push_back(uint8_t(value >> shift)); };
    const auto put16 = [&](uint16_t value) { file.push_back(uint8_t(value)); file.push_back(uint8_t(value >> 8)); };
    const auto tag = [&](const char* fourcc) { file.insert(file.end(), fourcc, fourcc + 4); };
    const auto patch = [&](size_t at) { const uint32_t size = uint32_t(file.size() - at - 4); for (int shift = 0; shift < 32; shift += 8) file[at + size_t(shift / 8)] = uint8_t(size >> shift); };
    tag("RIFF"); const size_t riff = file.size(); put32(0); tag("AVI ");
    tag("LIST"); const size_t hdrl = file.size(); put32(0); tag("hdrl");
    tag("avih"); put32(56);
    put32(1000000 / fps); put32(frameBytes * fps); put32(0); put32(0x10 /* AVIF_HASINDEX */);
    put32(frames); put32(0); put32(1); put32(frameBytes); put32(width); put32(height);
    for (int reserved = 0; reserved < 4; ++reserved) put32(0);
    tag("LIST"); const size_t strl = file.size(); put32(0); tag("strl");
    tag("strh"); put32(56);
    tag("vids"); tag("DIB "); put32(0); put16(0); put16(0); put32(0);
    put32(1); put32(fps); put32(0); put32(frames); put32(frameBytes); put32(uint32_t(-1)); put32(frameBytes);
    put16(0); put16(0); put16(uint16_t(width)); put16(uint16_t(height));
    tag("strf"); put32(40);
    put32(40); put32(width); put32(height); put16(1); put16(32); put32(0 /* BI_RGB */); put32(frameBytes);
    for (int reserved = 0; reserved < 4; ++reserved) put32(0);
    patch(strl); patch(hdrl);
    tag("LIST"); const size_t movi = file.size(); put32(0); tag("movi");
    const size_t moviStart = file.size() - 4;
    std::vector<uint32_t> offsets;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        offsets.push_back(uint32_t(file.size() - moviStart));
        tag("00db"); put32(frameBytes);
        file.insert(file.end(), frameBytes, uint8_t(frame));
    }
    patch(movi);
    tag("idx1"); put32(16 * frames);
    for (const uint32_t offset : offsets) { tag("00db"); put32(0x10 /* AVIIF_KEYFRAME */); put32(offset); put32(frameBytes); }
    patch(riff);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
}
} // namespace

// A 24-bit bottom-up BMP of `width` x `height` whose left half is black and right
// half white: the smallest file WIC decodes without an encoder of our own.
static void write_half_white_bmp(const std::filesystem::path& path, uint32_t width, uint32_t height)
{
    const uint32_t stride = (width * 3 + 3) & ~3u;
    std::vector<uint8_t> file(54 + size_t(stride) * height, 0);
    const auto put32 = [&](size_t at, uint32_t value) { for (int shift = 0; shift < 32; shift += 8) file[at + size_t(shift / 8)] = uint8_t(value >> shift); };
    file[0] = 'B'; file[1] = 'M'; put32(2, uint32_t(file.size())); put32(10, 54);
    put32(14, 40); put32(18, width); put32(22, height); file[26] = 1; file[28] = 24; put32(34, uint32_t(stride) * height);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            for (int channel = 0; channel < 3; ++channel)
                file[54 + size_t(y) * stride + size_t(x) * 3 + size_t(channel)] = x >= width / 2 ? 255 : 0;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
}

// Names each case after the member function that runs it, so a failure line
// reads [toolbar_pills_and_progress_panel_test] rather than a file and line
// in a two-thousand-line function.
#define UI_CASE(function) ::test_support::TestCase{#function, &PlayerAppTestAccess::function}

struct PlayerAppTestAccess {
    // Every case below shares this one PlayerApp, in the order the registry
    // lists them. That is deliberate, not laziness: the suite asserts against
    // state the previous case left behind, and the destructor-order contract
    // described in saved_settings_survive_a_reload depends on when this app is
    // destroyed relative to the one CheckFullscreenLifecycle owns. Splitting
    // the cases apart changed none of that - it only means a crash or a failed
    // REQUIRE now names the case it happened in and lets the rest run.
    //
    // Only what genuinely outlives a single case lives here. Everything else
    // stayed a local in the case that uses it.
    struct Fixture {
        PlayerApp app{AppOptions{}};
        WNDCLASSW windowClass{};
        // Snapshots taken while the toolbar is in a particular state and
        // compared against the measured pill widths much later, in
        // toolbar_pills_and_progress_panel.
        PlayerApp::ToolbarButtonContent upscalingContent{};
        PlayerApp::ToolbarButtonContent frameGenerationContent{};
        HMENU featureMenu = nullptr;
    };
    static inline Fixture* fixture = nullptr;
    // Set from argv by the --gpu run; empty on the portable run, which never
    // reaches the cases that read them.
    static inline std::filesystem::path gpuFfmpegDirectory;
    static inline std::filesystem::path gpuWorkDirectory;

    static void cache_settings_round_trip_test()
    {
        PlayerApp& app = fixture->app;

        CheckCacheSettings(app);
    }

    static void saved_settings_survive_a_reload_test()
    {
        PlayerApp& app = fixture->app;

        // Saved playback choices must survive a reload, with invalid volume clamped.
        app.m_volume = 0.35f; app.m_muted = true; app.m_fill = true;
        app.m_neuralRequested = false; app.m_upscaleTargetHeight = 2160;
        app.m_youtubeSourceQuality = YouTubeSourceQuality::P1440;
        app.m_renderGuides = GuideControls{false, true};
        app.m_neuralSettings.intensity = 1.5f; app.m_neuralSettings.preset = 2; app.m_neuralSettings.autoMask = false;
        app.m_comparison.mode = ComparisonMode::Wipe; app.m_comparison.swap = true; app.m_comparison.splitX = 0.8f; app.m_zoomStep = 3;
        app.m_comparison.strength = 0.4f;
        const auto savedCacheRoot=app.SettingsPath().parent_path()/L"shared-cache-location";
        app.m_cacheRoot=savedCacheRoot;
        app.SaveVideoSettings();
        app.m_cacheRoot.clear();
        app.m_volume = 1.0f; app.m_muted = false; app.m_fill = false;
        app.m_neuralRequested = true; app.m_upscaleTargetHeight = 1440;
        app.m_youtubeSourceQuality = YouTubeSourceQuality::Auto;
        app.m_renderGuides = {}; app.m_neuralSettings = {}; app.m_comparison = {};
        app.LoadVideoSettings();
        CHECK_EQ(app.m_cacheRoot,savedCacheRoot);
        CHECK(std::abs(app.m_volume - 0.35f) < 0.001f);
        CHECK(app.m_muted && app.m_fill && !app.m_neuralRequested);
        CHECK_EQ(app.m_upscaleTargetHeight, 2160u);
        CHECK(app.m_youtubeSourceQuality == YouTubeSourceQuality::P1440);
        CHECK((app.m_renderGuides == GuideControls{false, true}));
        CHECK(app.m_neuralSettings.intensity == 1.5f && app.m_neuralSettings.preset == 2 && !app.m_neuralSettings.autoMask);
        CHECK(app.m_comparison.mode == ComparisonMode::Wipe);
        CHECK(app.m_comparison.swap && std::abs(app.m_comparison.splitX - 0.8f) < 0.001f);
        CHECK_EQ(app.m_zoomStep, 3);
        // Every later reload in this case would bring the zoom back; the rest of the
        // suite expects Fit.
        app.m_zoomStep = 0;
        WritePrivateProfileStringW(L"Comparison", L"ZoomStep", nullptr, app.SettingsPath().c_str());
        // The presentation-only strength dial rides the same save/load as the image
        // adjustments it sits with, clamps to the 0..2 the shader composites over, and
        // reads back as 1 (the neural frame untouched) when the key is absent.
        CHECK(std::abs(app.m_comparison.strength - 0.4f) < 0.001f);
        // The upscaling target persists as two independent facts, and the split
        // is the whole reason an existing install can reach Auto at all. Every
        // release before this one wrote UpscaleHeight on every save, so a 1440
        // in the file is the old default and not a choice; only the absence of
        // UpscaleAuto identifies such a file, and absence has to mean Auto.
        WritePrivateProfileStringW(L"Playback", L"UpscaleAuto", nullptr, app.SettingsPath().c_str());
        app.WriteIniFloat(L"Playback", L"UpscaleHeight", 1440.0f);
        app.m_upscaleAuto = false;
        app.LoadVideoSettings();
        CHECK(app.m_upscaleAuto);
        CHECK_EQ(app.m_upscaleTargetHeight, 1440u);
        // A rung pinned on this version survives the reload that the legacy
        // 1440 must not.
        app.m_upscaleAuto = false; app.m_upscaleTargetHeight = 2160;
        app.SaveVideoSettings();
        app.m_upscaleAuto = true; app.m_upscaleTargetHeight = 1080;
        app.LoadVideoSettings();
        CHECK(!app.m_upscaleAuto);
        CHECK_EQ(app.m_upscaleTargetHeight, 2160u);
        // Returning to Auto keeps the pinned rung underneath it, so switching
        // away and back does not silently retarget a later manual pick.
        app.m_upscaleAuto = true;
        app.SaveVideoSettings();
        app.m_upscaleAuto = false; app.m_upscaleTargetHeight = 1080;
        app.LoadVideoSettings();
        CHECK(app.m_upscaleAuto);
        CHECK_EQ(app.m_upscaleTargetHeight, 2160u);
        // A rung the build does not offer is not a selection: it falls back to
        // the member default rather than reaching UpscalingTarget, which would
        // refuse it anyway and report SR as merely unavailable.
        app.WriteIniFloat(L"Playback", L"UpscaleHeight", 999.0f);
        app.m_upscaleTargetHeight = 2160;
        app.LoadVideoSettings();
        CHECK_EQ(app.m_upscaleTargetHeight, 2160u);
        // The Mix this build wrote is read back, and wins over the legacy key it replaced.
        CHECK_EQ(app.ReadIniFloat(L"Comparison", L"Mix", -1.0f), 0.4f);
        app.WriteIniFloat(L"VideoAdjustments", L"NeuralStrength", 1.7f);
        app.LoadVideoSettings();
        CHECK(std::abs(app.m_comparison.strength - 0.4f) < 0.001f);
        // A file from before the Mix has only NeuralStrength, which is read as the Mix,
        // clamped to the 0..2 the shader composites over, and 1 when absent.
        WritePrivateProfileStringW(L"Comparison", L"Mix", nullptr, app.SettingsPath().c_str());
        app.WriteIniFloat(L"VideoAdjustments", L"NeuralStrength", 9.0f);
        app.LoadVideoSettings();
        CHECK_EQ(app.m_comparison.strength, 2.0f);
        app.WriteIniFloat(L"VideoAdjustments", L"NeuralStrength", -3.0f);
        app.LoadVideoSettings();
        CHECK_EQ(app.m_comparison.strength, 0.0f);
        WritePrivateProfileStringW(L"VideoAdjustments", L"NeuralStrength", nullptr, app.SettingsPath().c_str());
        app.LoadVideoSettings();
        CHECK_EQ(app.m_comparison.strength, 1.0f);
        // Original is a view, not a comparison mode; an out-of-range mode falls back to Neural.
        WritePrivateProfileStringW(L"Comparison", L"Mode", L"1", app.SettingsPath().c_str());
        app.LoadVideoSettings();
        CHECK(app.m_comparison.mode == ComparisonMode::Neural);
        // Blend is the Mix now: Blend at 0.3 opens as the neural view at a Mix of 0.3.
        WritePrivateProfileStringW(L"Comparison", L"Mode", L"2", app.SettingsPath().c_str());
        app.WriteIniFloat(L"Comparison", L"Amount", 0.3f);
        app.LoadVideoSettings();
        CHECK(app.m_comparison.mode == ComparisonMode::Neural);
        CHECK(std::abs(app.m_comparison.strength - 0.3f) < 0.001f);
        app.m_comparison = {};
        // Absent guide keys mean every guide is on, matching a fresh install.
        for (const wchar_t* key : {L"MotionVectors", L"Depth"})
            WritePrivateProfileStringW(L"NeuralGuides", key, nullptr, app.SettingsPath().c_str());
        app.LoadVideoSettings();
        CHECK(app.m_renderGuides.IsDefault());
        app.WriteIniFloat(L"Playback", L"Volume", 2.0f);
        app.LoadVideoSettings();
        CHECK_EQ(app.m_volume, 1.0f);
        app.m_muted = false; app.m_fill = false; app.m_neuralRequested = true;
        // m_upscaleAuto is reset with the rest of the persisted playback state,
        // and for the same reason: this save is what the next run of this exe
        // loads. It is not reset because the suite was failing - it was not.
        // CheckFullscreenLifecycle owns a second PlayerApp nested inside this
        // scope (called at the end of Run), so that one's destructor saves
        // UpscaleAuto=0 and THIS one's destructor overwrites it with 1
        // afterwards. The pass therefore depends on destructor order, and both
        // ends are pinned so it stops depending on it.
        app.m_upscaleAuto = true;
        app.m_upscaleTargetHeight = 1440; app.m_youtubeSourceQuality = YouTubeSourceQuality::Auto; app.m_neuralSettings = {}; app.m_comparison = {};
        app.m_cacheRoot.clear();
        WritePrivateProfileStringW(L"Storage",L"CacheDirectory",nullptr,app.SettingsPath().c_str());
        app.SaveVideoSettings();
    }

    static void hidden_window_and_menu_bar_open_test()
    {
        PlayerApp& app = fixture->app;
        WNDCLASSW& windowClass = fixture->windowClass;

        windowClass = WNDCLASSW{};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"DLSSPlayerUiRegressionWindow";
        CHECK(RegisterClassW(&windowClass) != 0);
        // Hidden window: native menus are exercised without taking focus.
        app.m_hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"UI regression",
            WS_OVERLAPPEDWINDOW, 0, 0, 800, 600, nullptr,
            app_menu::CreateMenuBar(app.m_loc, true), windowClass.hInstance, nullptr);
        CHECK(app.m_hwnd != nullptr);
        CHECK(app.m_uiResources.Load(GetModuleHandleW(nullptr)));
        app.UpdateFontsForDpi(96);
    }

    static void old_source_policy_requests_a_fresh_resolution_test()
    {
        PlayerApp& app = fixture->app;

        // Old source policies must request a fresh resolution; current sources
        // must reach decoding instead of being rejected by the policy gate.
        const auto policyRoot=app.SettingsPath().parent_path()/L"bitrate-policy-cache";
        app.m_cacheRoot=policyRoot;
        {
            NeuralCacheManager cache(policyRoot);
            const std::string key(64,'c');
            for(const auto& policy : {"source-complete-v4", "source-complete-v5-highest-bitrate"}) {
                cache.RemoveSource(key);
                const auto staging=cache.BeginSourceStaging(key);
                CHECK(staging.has_value());
                if(!staging)continue;
                {std::ofstream payload(*staging/L"source.mkv",std::ios::binary);payload<<"policy fixture";}
                NeuralCacheManifest manifest{};manifest.encoder=policy;
                manifest.width=1920;manifest.height=1080;manifest.frameCount=1;manifest.duration100ns=333333;
                CHECK(cache.PromoteSource(key,*staging,manifest));
                app.StartNeuralJob(L"https://youtu.be/VQRLujxTm3c",{},L"Policy fixture",
                    L"https://youtu.be/VQRLujxTm3c",MediaSourceKind::YouTube,YouTubeSourceQuality::P1080,key);
                if(app.m_neuralWorker.joinable())app.m_neuralWorker.join();
                MSG message{};
                const bool posted=PeekMessageW(&message,app.m_hwnd,WM_NEURAL_COMPLETE,WM_NEURAL_COMPLETE,PM_REMOVE)!=FALSE;
                CHECK(posted);
                if(posted){
                    auto completion=app.m_neuralCompletions.Take(static_cast<uint64_t>(message.wParam));
                    CHECK(completion!=nullptr);
                    if(completion)CHECK_EQ(std::string_view(policy)=="source-complete-v4",completion->cachedSourceUnavailable);
                }
                app.CancelNeuralJob(false);
                CHECK(cache.RemoveSource(key));
            }
        }
        app.m_cacheRoot.clear();
        std::filesystem::remove_all(policyRoot);

        const auto recentFile=app.SettingsPath().parent_path()/L"recent-regression.dat";
        app.m_recent=std::make_unique<RecentMediaHistory>(recentFile);
        NeuralJobCompletion remembered{};remembered.sourceKind=MediaSourceKind::YouTube;
        remembered.pageUrl=kExampleVideos[0].url;
        remembered.displayTitle=L"Trailer & comparison";remembered.sourceQuality=YouTubeSourceQuality::P1080;
        remembered.sourceKey=std::string(64,'a');remembered.renderKey=std::string(64,'b');
        app.RecordRecent(remembered);
        CHECK_EQ(app.m_recent->Entries().size(),size_t{1});
        remembered.sourceKey.clear();remembered.renderKey.clear();
        app.RecordRecent(remembered,true);
        CHECK_EQ(app.m_recent->Entries().front().renderKey,std::string(64,'b'));
        remembered.sourceQuality=YouTubeSourceQuality::P2160;
        app.RecordRecent(remembered,true);
        CHECK(app.m_recent->Entries().front().sourceKey.empty());
        CHECK(app.m_recent->Entries().front().renderKey.empty());
        CHECK((GetMenuState(GetMenu(app.m_hwnd),IDM_RECENT_VIDEO_FIRST,MF_BYCOMMAND)&MF_GRAYED)==0);
        app.m_neuralLifecycle.Begin();app.UpdateRecentMenu();
        CHECK((GetMenuState(GetMenu(app.m_hwnd),IDM_RECENT_VIDEO_FIRST,MF_BYCOMMAND)&MF_GRAYED)!=0);
        app.m_neuralLifecycle.Invalidate();
        // Opening the same example must take the owned source-cache path before
        // starting the network resolver. A missing cache falls back later.
        remembered.sourceQuality=YouTubeSourceQuality::P1080;
        remembered.sourceKey=std::string(64,'a');remembered.renderKey=std::string(64,'b');
        app.RecordRecent(remembered);
        app.m_youtubeSourceQuality=YouTubeSourceQuality::P1080;
        app.m_opt.neuralAddonConfigured=true;
        app.ActivateExampleVideo(kExampleVideos[0]);
        CHECK(app.NeuralJobActive());
        CHECK(!app.m_youtubeLifecycle.IsResolving());
        app.CancelNeuralJob(false);app.CancelYouTubeResolution(false);
        app.m_neuralProgress={};app.m_pendingNeuralTitle.clear();
        app.m_opt.neuralAddonConfigured=false;app.m_youtubeSourceQuality=YouTubeSourceQuality::Auto;
        app.m_recent.reset();
        std::filesystem::remove(recentFile);
    }

    static void recent_rollover_keeps_the_cache_test()
    {
        PlayerApp& app = fixture->app;

        CheckRecentRolloverKeepsTheCache(app);
    }

    static void neural_button_label_and_upscaling_rungs_test()
    {
        PlayerApp& app = fixture->app;
        PlayerApp::ToolbarButtonContent& upscalingContent = fixture->upscalingContent;
        PlayerApp::ToolbarButtonContent& frameGenerationContent = fixture->frameGenerationContent;


        // This must remain a synchronized neural/original comparison, never an
        // ambiguous label for runtime Super Resolution.
        const auto comparisonContent = app.ButtonContent(ToolbarAction::ToggleNeuralRendering);
        CHECK(comparisonContent.label.find(L"Neural Rendering") != std::wstring::npos);
        CHECK(comparisonContent.label.find(L"DLSS") == std::wstring::npos);
        CHECK(!comparisonContent.enabled);
        CHECK(!comparisonContent.active);
        CHECK(app.m_neuralRequested);
        CHECK(!app.m_upscalingRequested);
        // Auto is the fresh default, and it is a state of its own: the manual
        // rung underneath it must not move until a rung is actually picked.
        CHECK(app.m_upscaleAuto);
        CHECK_EQ(app.m_upscaleTargetHeight,1440u);
        app.HandleCommand(IDM_UPSCALE_2160);
        CHECK(!app.m_upscaleAuto);
        CHECK_EQ(app.m_upscaleTargetHeight,2160u);
        CHECK(!app.m_upscalingRequested);
        app.HandleCommand(IDM_UPSCALE_1080);
        CHECK_EQ(app.m_upscaleTargetHeight,1080u);
        app.HandleCommand(IDM_UPSCALE_1440);
        CHECK(!app.m_upscaleAuto);
        CHECK_EQ(app.m_upscaleTargetHeight,1440u);
        app.HandleCommand(IDM_UPSCALE_AUTO);
        CHECK(app.m_upscaleAuto);
        CHECK_EQ(app.m_upscaleTargetHeight,1440u);
        // Hold repeated frames is off on a fresh install - measured, it changes
        // nothing visible on the runtime tested - and the menu toggles and persists it.
        CHECK(!app.m_frameGenHoldDuplicates);
        app.HandleCommand(IDM_FRAMEGEN_HOLD_DUPLICATES);
        CHECK(app.m_frameGenHoldDuplicates);
        CHECK_EQ(GetPrivateProfileIntW(L"FrameGeneration",L"HoldDuplicates",0,app.SettingsPath().c_str()),UINT{1});
        app.HandleCommand(IDM_FRAMEGEN_HOLD_DUPLICATES);
        CHECK(!app.m_frameGenHoldDuplicates);
        upscalingContent = app.ButtonContent(ToolbarAction::ToggleUpscaling);
        CHECK(!upscalingContent.enabled);
        frameGenerationContent = app.ButtonContent(ToolbarAction::ToggleFrameGeneration);
        CHECK(!frameGenerationContent.enabled);
        const bool initialQualityExplicit = app.m_opt.qualityExplicit;
        const auto initialQuality = app.m_opt.quality;
        app.HandleCommand(IDM_NEURAL_RENDERING);
        app.HandleCommand(IDM_DLSS_UPSCALING);
        app.HandleCommand(IDM_FRAME_GENERATION);
        app.HandleCommand(333); // Removed legacy quality command remains inert.
        CHECK(app.m_neuralRequested);
        CHECK_EQ(initialQualityExplicit, app.m_opt.qualityExplicit);
        CHECK_EQ(initialQuality, app.m_opt.quality);

    }

    static void feature_menu_mirrors_the_toolbar_test()
    {
        PlayerApp& app = fixture->app;
        HMENU& featureMenu = fixture->featureMenu;

        featureMenu = GetMenu(app.m_hwnd);
        CHECK((GetMenuState(featureMenu, IDM_NEURAL_RENDERING, MF_BYCOMMAND) & MFS_CHECKED) != 0);
        CHECK((GetMenuState(featureMenu, IDM_NEURAL_RENDERING, MF_BYCOMMAND) & (MFS_DISABLED | MFS_GRAYED)) != 0);
        CHECK((GetMenuState(featureMenu, IDM_DLSS_UPSCALING, MF_BYCOMMAND) & (MFS_DISABLED | MFS_GRAYED)) != 0);
        CHECK((GetMenuState(featureMenu, IDM_FRAME_GENERATION, MF_BYCOMMAND) & (MFS_DISABLED | MFS_GRAYED)) != 0);

        // Menu state must use the same eligibility gate as the toolbar. A
        // renderer object is enough here because no renderer work is invoked.
        app.m_loaded = true;
        app.m_cachedPlayback = true;
        app.m_havePresentedPair = true;
        app.m_renderer = MakeD3D12Renderer();
        app.m_seeking = false;
        app.SyncFeatureMenuState();
        CHECK(app.ToolbarActionEnabled(ToolbarAction::ToggleNeuralRendering));
        CHECK((GetMenuState(featureMenu, IDM_NEURAL_RENDERING, MF_BYCOMMAND) &
               (MFS_DISABLED | MFS_GRAYED)) == 0);

        app.m_seeking = true;
        app.SyncFeatureMenuState();
        CHECK(!app.ToolbarActionEnabled(ToolbarAction::ToggleNeuralRendering));
        CHECK((GetMenuState(featureMenu, IDM_NEURAL_RENDERING, MF_BYCOMMAND) &
               (MFS_DISABLED | MFS_GRAYED)) != 0);
        const bool requestedBeforeSeekCommand = app.m_neuralRequested;
        app.HandleCommand(IDM_NEURAL_RENDERING);
        CHECK_EQ(requestedBeforeSeekCommand, app.m_neuralRequested);
        // The bar carries both feature segments verbatim - it does not compose
        // their copy - so that is what the test states. The old assertions
        // pinned "DLSS SR unavailable" and "FG unavailable", which said nothing
        // a viewer relies on and failed the moment the copy was rewritten.
        const std::wstring cachedStatus = app.BuildStatusText();
        const std::wstring upscalingSegment = app.UpscalingStatus();
        const std::wstring frameGenerationSegment = app.FrameGenerationStatus();
        CHECK(!upscalingSegment.empty());
        CHECK(!frameGenerationSegment.empty());
        CHECK(cachedStatus.find(upscalingSegment) != std::wstring::npos);
        CHECK(cachedStatus.find(frameGenerationSegment) != std::wstring::npos);
    }

    static void markers_and_timecode_test()
    {
        PlayerApp& app = fixture->app;

        CheckMarkersAndTimecode(app);
    }

    static void comparison_availability_test()
    {
        PlayerApp& app = fixture->app;

        CheckComparisonAvailability(app);
    }

    static void neural_strength_dial_test()
    {
        PlayerApp& app = fixture->app;

        CheckNeuralStrengthDial(app);
    }

    static void live_buffering_play_intent_test()
    {
        PlayerApp& app = fixture->app;

        CheckLiveBufferingPlayIntent(app);
    }

    static void neural_toggle_queued_during_seek_test()
    {
        PlayerApp& app = fixture->app;

        CheckNeuralToggleQueuedDuringSeek(app);
    }

    static void export_stages_dialog_test()
    {
        PlayerApp& app = fixture->app;

        CheckExportStagesDialog(app);
    }

    static void neural_settings_dialog_test()
    {
        PlayerApp& app = fixture->app;

        CheckNeuralSettingsDialog(app);
    }

    static void settings_ahead_notice_test()
    {
        PlayerApp& app = fixture->app;

        CheckSettingsAheadNotice(app);
    }

    static void render_report_test()
    {
        PlayerApp& app = fixture->app;

        CheckRenderReport(app);
    }

    static void encoder_settings_dialog_test()
    {
        PlayerApp& app = fixture->app;

        CheckEncoderSettingsDialog(app);
    }

    static void settings_dialog_tips_survive_a_second_dialog_test()
    {
        PlayerApp& app = fixture->app;

        CheckSettingsDialogTipsSurviveASecondDialog(app);
    }

    static void live_export_entry_test()
    {
        PlayerApp& app = fixture->app;

        CheckLiveExportEntry(app);
    }

    static void dropped_preview_job_test()
    {
        PlayerApp& app = fixture->app;
        WNDCLASSW& windowClass = fixture->windowClass;

        CheckDroppedPreviewJob(app, windowClass);
    }

    static void unload_drops_deferred_toggle_test()
    {
        PlayerApp& app = fixture->app;

        CheckUnloadDropsDeferredToggle(app);
    }

    static void live_job_directory_failure_test()
    {
        PlayerApp& app = fixture->app;

        CheckLiveJobDirectoryFailure(app);
    }

    static void live_render_failure_limit_test()
    {
        PlayerApp& app = fixture->app;

        CheckLiveRenderFailureLimit(app);
    }

    static void job_source_key_guard_test()
    {
        PlayerApp& app = fixture->app;

        CheckJobSourceKeyGuard(app);
    }

    static void stream_conversion_uses_the_acquired_copy_test()
    {
        PlayerApp& app = fixture->app;

        CheckStreamConversionUsesTheAcquiredCopy(app);
    }

    static void live_out_of_sync_hands_back_test()
    {
        PlayerApp& app = fixture->app;

        CheckLiveOutOfSyncHandsBack(app);
    }

    static void pair_stall_bound_ignores_a_pause_test()
    {
        PlayerApp& app = fixture->app;

        CheckPairStallBoundIgnoresAPause(app);
    }

    static void nv12_reference_inverse_is_bt709_limited_test()
    {
        CheckNv12ReferenceInverseIsBt709Limited();
    }

    static void live_pace_confirmation_test()
    {
        PlayerApp& app = fixture->app;

        CheckLivePaceConfirmation(app);
    }

    static void toolbar_pills_and_progress_panel_test()
    {
        PlayerApp& app = fixture->app;
        PlayerApp::ToolbarButtonContent& upscalingContent = fixture->upscalingContent;
        PlayerApp::ToolbarButtonContent& frameGenerationContent = fixture->frameGenerationContent;
        HMENU& featureMenu = fixture->featureMenu;

        app.m_seeking = false;
        app.m_cachedPlayback = false;
        app.m_havePresentedPair = false;
        app.m_renderer.reset();
        app.m_loaded = false;
        app.SyncFeatureMenuState();
        const auto noCacheContent = app.ButtonContent(ToolbarAction::ToggleNeuralRendering);
        CHECK(noCacheContent.label.find(L"No cache") != std::wstring::npos);
        app.m_neuralRequested = false;
        const auto noCacheOffContent = app.ButtonContent(ToolbarAction::ToggleNeuralRendering);
        CHECK(noCacheOffContent.label.find(L"No cache") != std::wstring::npos);

        app.m_loaded = true;
        app.m_cachedPlayback = true;
        app.m_havePresentedPair = true;
        app.m_renderer = MakeD3D12Renderer();
        app.m_seeking = true;
        app.m_neuralRequested = true;
        app.m_comparisonView = ComparisonView::Neural;
        app.SyncFeatureMenuState();
        const auto seekingOnContent = app.ButtonContent(ToolbarAction::ToggleNeuralRendering);
        CHECK(seekingOnContent.active);
        CHECK((GetMenuState(featureMenu, IDM_NEURAL_RENDERING, MF_BYCOMMAND) & MFS_CHECKED) != 0);
        app.m_neuralRequested = false;
        app.m_comparisonView = ComparisonView::Original;
        app.SyncFeatureMenuState();
        const auto seekingOffContent = app.ButtonContent(ToolbarAction::ToggleNeuralRendering);
        CHECK(!seekingOffContent.active);
        CHECK((GetMenuState(featureMenu, IDM_NEURAL_RENDERING, MF_BYCOMMAND) & MFS_CHECKED) == 0);

        HDC dc = CreateCompatibleDC(nullptr);
        CHECK(dc != nullptr);
        drawnText.clear();
        app.DrawButton(dc, ToolbarAction::Open, UiIcon::Open, L"Open",
                       RECT{0, 0, 36, 36}, true, false, false, false, false, true);
        CHECK(!Contains(L"Open"));

        app.m_loaded = false;
        app.m_cachedPlayback = false;
        app.m_havePresentedPair = false;
        app.m_renderer.reset();
        app.m_seeking = false;
        app.m_neuralLifecycle.Begin();
        const auto preparingContent = app.ButtonContent(ToolbarAction::ToggleNeuralRendering);
        CHECK(!preparingContent.enabled);

        // Each pill's width comes from the toolbar itself. The literals here
        // were 270/230/320 dip, and the frame-generation pill is no longer
        // 320: a label wider than its real pill would have passed unseen.
        const auto widePills = LayoutToolbar(1600, 180, 96);
        const auto pillWidthDip = [&widePills](ToolbarAction action) {
            const auto found = std::find_if(widePills.begin(), widePills.end(),
                                            [action](const ToolbarItem& item) {
                                                return item.action == action;
                                            });
            CHECK(found != widePills.end());
            return found == widePills.end()
                       ? 0
                       : static_cast<int>(found->bounds.right - found->bounds.left);
        };
        struct FeatureLabel { UiIcon icon; const std::wstring& label; int widthDip; };
        const std::array featureLabels{
            FeatureLabel{preparingContent.icon, preparingContent.label,
                         pillWidthDip(ToolbarAction::ToggleNeuralRendering)},
            FeatureLabel{seekingOffContent.icon, seekingOffContent.label,
                         pillWidthDip(ToolbarAction::ToggleNeuralRendering)},
            FeatureLabel{upscalingContent.icon, upscalingContent.label,
                         pillWidthDip(ToolbarAction::ToggleUpscaling)},
            FeatureLabel{frameGenerationContent.icon, frameGenerationContent.label,
                         pillWidthDip(ToolbarAction::ToggleFrameGeneration)},
        };
        for (const UINT dpi : {96u, 120u, 144u, 192u}) {
            app.UpdateFontsForDpi(dpi);
            for (const auto& feature : featureLabels) {
                const HFONT font = app.m_fontSmall ? app.m_fontSmall : app.m_font;
                const HGDIOBJ previous = SelectObject(dc, font);
                SIZE textSize{};
                CHECK(GetTextExtentPoint32W(dc, feature.label.c_str(),
                                            static_cast<int>(feature.label.size()), &textSize));
                SelectObject(dc, previous);
                const wchar_t glyph = GlyphForIcon(feature.icon);
                const HGDIOBJ previousIcon = SelectObject(dc, app.m_iconFont);
                SIZE iconSize{};
                CHECK(GetTextExtentPoint32W(dc, &glyph, 1, &iconSize));
                SelectObject(dc, previousIcon);
                const RECT button{0, 0, MulDiv(feature.widthDip, static_cast<int>(dpi), 96),
                                  MulDiv(kToolbarMinHitHeightDip, static_cast<int>(dpi), 96)};
                const auto bounds = LayoutButtonContent(button, iconSize, textSize, false, dpi);
                CHECK_EQ(textSize.cx, bounds.text.right - bounds.text.left);
                CHECK(bounds.text.left - bounds.icon.right >= MulDiv(kButtonIconLabelGapDip, static_cast<int>(dpi), 96));
                CHECK(bounds.icon.left - button.left >= MulDiv(kButtonHorizontalInsetDip, static_cast<int>(dpi), 96));
                CHECK(bounds.text.left - button.left >= MulDiv(kButtonHorizontalInsetDip, static_cast<int>(dpi), 96));
                CHECK(button.right - bounds.text.right >= MulDiv(kButtonHorizontalInsetDip, static_cast<int>(dpi), 96));
            }
        }
        app.m_neuralLifecycle.Begin();
        app.m_neuralSourceWidth = 1920;
        app.m_neuralSourceHeight = 1080;
        app.m_neuralProgress.totalFrames = 1800;
        app.m_neuralProgress.estimatedRemaining = std::chrono::seconds(5);
        // Stated rather than inherited from the struct's default: the panel's
        // encoder and download lines are per-phase answers.
        app.m_neuralProgress.phase = NeuralRenderPhase::NeuralRendering;
        drawnText.clear();
        app.RenderUi(dc, RECT{0, 0, 800, 600});
        CHECK(Contains(L"1920 \u00d7 1080"));
        CHECK(Contains(L"Preparing encoder\u2026"));
        CHECK(Contains(L"Elapsed 00:00 \u00b7 ETA 00:05"));
        // Acquisition has no frames and no encoder yet, so it reports the copy
        // itself. Without this the panel sat still for the whole download and a
        // reporter read a working acquisition as a hang.
        app.m_neuralProgress.phase = NeuralRenderPhase::Acquiring;
        app.m_neuralProgress.bytes = 42u * 1024u * 1024u;
        app.m_neuralProgress.acquiredSeconds = 30.0;
        app.m_neuralProgress.expectedSeconds = 120.0;
        drawnText.clear();
        app.RenderUi(dc, RECT{0, 0, 800, 600});
        CHECK(Contains(L"Downloading the source \u00b7 42 MiB \u00b7 25%"));
        CHECK(Contains(L"25% of the source copied"));
        CHECK(!Contains(L"Preparing encoder\u2026"));
        app.m_neuralProgress = {};
        app.m_neuralProgress.phase = NeuralRenderPhase::NeuralRendering;
        app.m_neuralProgress.totalFrames = 1800;
        app.m_neuralSourceWidth = 0;
        drawnText.clear();
        app.RenderUi(dc, RECT{0, 0, 800, 600});
        CHECK(Contains(L"Reading source metadata\u2026"));
        app.m_neuralProgress.phase=NeuralRenderPhase::CheckingCache;
        drawnText.clear();
        app.RenderUi(dc, RECT{0, 0, 800, 600});
        CHECK(Contains(L"Checking saved video"));
        CHECK(Contains(L"Verifying cache; no re-encoding"));
        CHECK(!Contains(L"Preparing encoder\u2026"));

        // Pausing holds the lifecycle and the spinner until the user resumes,
        // even when the helper still reports a frame that was already in
        // flight; Space drives the same path while a job is active.
        CHECK(app.m_neuralLifecycle.Transition(NeuralPlaybackState::Rendering));
        CHECK(!app.NeuralJobPaused());
        app.WndProc(app.m_hwnd, WM_KEYDOWN, VK_SPACE, 0);
        CHECK(app.NeuralJobPaused());
        CHECK_EQ(NeuralPlaybackState::Paused, app.m_neuralLifecycle.state);
        CHECK(app.NeuralJobActive());
        drawnText.clear();
        app.RenderUi(dc, RECT{0, 0, 800, 600});
        CHECK(Contains(L"Paused"));
        QueueProgress(app, NeuralRenderPhase::NeuralRendering);
        CHECK_EQ(NeuralPlaybackState::Paused, app.m_neuralLifecycle.state);
        app.SetNeuralJobPaused(false);
        CHECK(!app.NeuralJobPaused());
        CHECK_EQ(NeuralPlaybackState::Rendering, app.m_neuralLifecycle.state);
        QueueProgress(app, NeuralRenderPhase::Paused);
        CHECK_EQ(NeuralPlaybackState::Rendering, app.m_neuralLifecycle.state);
        QueueProgress(app, NeuralRenderPhase::Recovering, NeuralRenderFailure::GpuStall, 2);
        CHECK_EQ(NeuralPlaybackState::Recovering, app.m_neuralLifecycle.state);
        drawnText.clear();
        app.RenderUi(dc, RECT{0, 0, 800, 600});
        CHECK(Contains(L"Recovering (attempt 2 \u00b7 gpu-stall)"));
        QueueProgress(app, NeuralRenderPhase::Encoding);
        CHECK_EQ(NeuralPlaybackState::Rendering, app.m_neuralLifecycle.state);
        app.SetNeuralJobPaused(true);
        app.CancelNeuralJob(false);
        CHECK(!app.NeuralJobPaused());
        CHECK(!app.NeuralJobActive());
        app.m_neuralProgress = {};
        CHECK(DeleteDC(dc));

        // Cancelled and failed completions must restore the native menu, not
        // merely make the toolbar's computed availability true again. Only the
        // failure reports itself.
        const int boxesBeforeTerminalJobs = messageBoxes;
        CompleteTerminalJob(app, true);
        CompleteTerminalJob(app, false);
        CHECK_EQ(boxesBeforeTerminalJobs + 1, messageBoxes);
        // Retry exhaustion keeps its own terminal state and the fallback
        // message names the failure kind before the helper's detail.
        CompleteTerminalJob(app, false, NeuralRenderFailure::RetryExhausted);
        CHECK_EQ(boxesBeforeTerminalJobs + 2, messageBoxes);
        CHECK(lastMessageBox.find(L"gave up after retrying") != std::wstring::npos);
        CHECK(lastMessageBox.find(L"Controlled render failure") != std::wstring::npos);

        // An obsolete completion must not unlock a newer active render.
        const uint64_t oldGeneration = app.m_neuralLifecycle.Begin();
        app.m_neuralLifecycle.Begin();
        app.SyncSourceActionAvailability();
        const uint64_t staleToken = QueueCompletion(app, oldGeneration, true);
        app.CompleteNeuralJob(staleToken);
    }

    // Measured with the fonts the player draws with, at every dpi it scales
    // to, because the widths in StatusChipPolicy.h and UiLayout.h are only
    // true of those: a chip or a narrowed pill too small for its text would
    // pass every layout test and still be cut on screen.
    static void status_chips_and_narrow_pills_fit_test()
    {
        PlayerApp& app = fixture->app;
        HDC dc = CreateCompatibleDC(nullptr);
        REQUIRE(dc != nullptr);
        const auto measure = [&](HFONT font, std::wstring_view text) {
            const HGDIOBJ previous = SelectObject(dc, font);
            SIZE size{};
            CHECK(GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size));
            SelectObject(dc, previous);
            return size;
        };
        // Every state a narrowed feature pill can show. The neural and
        // upscaling arms are literals in ButtonContent; frame generation's
        // come from the localizer, through the same cut the pill makes.
        std::vector<std::wstring> states{L"Queued for the seek", L"Previewing settings", L"Settings preview",
                                         L"Seeking · Off", L"Preparing cache", L"No cache", L"On", L"Off",
                                         L"No video", L"Starting up", L"No DLSS", L"No frame yet",
                                         L"Panel too small", L"Meets output"};
        for (const wchar_t* key : {L"framegen.pill.generate", L"framegen.pill.cancel", L"framegen.pill.busy",
                                   L"framegen.pill.unavailable", L"framegen.pill.get_copy", L"framegen.pill.copying"}) {
            const std::wstring label = app.T(key);
            CHECK(FeaturePillStateLabel(label) != label);
            states.emplace_back(FeaturePillStateLabel(label));
        }
        const wchar_t glyph = GlyphForIcon(UiIcon::Sparkles);
        struct ChipCase { status_chips::Chip chip; const wchar_t* widest; };
        const std::array chips{
            ChipCase{status_chips::Chip::Render, L"Render 99% · ETA 99:59:59"},
            ChipCase{status_chips::Chip::Fps, L"240 / 240 fps"},
            ChipCase{status_chips::Chip::Dropped, L"Dropped 99999"},
        };
        for (const UINT dpi : {96u, 120u, 144u, 192u}) {
            app.UpdateFontsForDpi(dpi);
            const HFONT font = app.m_fontSmall ? app.m_fontSmall : app.m_font;
            const SIZE icon = measure(app.m_iconFont, std::wstring_view(&glyph, 1));
            const int pill = MulDiv(kFeaturePillNarrowWidthDip, static_cast<int>(dpi), 96);
            const int chrome = icon.cx + MulDiv(2 * kButtonHorizontalInsetDip + kButtonIconLabelGapDip, static_cast<int>(dpi), 96);
            for (const auto& state : states) CHECK(measure(font, state).cx + chrome <= pill);
            for (const auto& chip : chips) {
                const int inner = MulDiv(status_chips::WidthDip(chip.chip) - 2 * status_chips::kChipPaddingDip,
                                         static_cast<int>(dpi), 96);
                CHECK(measure(font, chip.widest).cx <= inner);
            }
        }
        app.UpdateFontsForDpi(96);

        // A narrowed pill paints its state and not a truncated name.
        drawnText.clear();
        app.DrawButton(dc, ToolbarAction::ToggleNeuralRendering, UiIcon::Sparkles,
                       L"Neural Rendering · Queued for the seek", RECT{0, 0, kFeaturePillNarrowWidthDip, 36},
                       true, false, false, false, false, false);
        CHECK(Contains(L"Queued for the seek"));
        drawnText.clear();
        app.DrawButton(dc, ToolbarAction::ToggleNeuralRendering, UiIcon::Sparkles,
                       L"Neural Rendering · On", RECT{0, 0, 270, 36}, true, true, false, false, false, false);
        CHECK(Contains(L"Neural Rendering · On"));

        // The bar paints the chips it holds, beside the line and not over it.
        // The case before this one leaves a job's lifecycle running, which
        // would put the whole-window progress panel up instead of the bar.
        const NeuralPlaybackLifecycle lifecycle = app.m_neuralLifecycle;
        app.m_neuralLifecycle.state = NeuralPlaybackState::Idle;
        const bool loaded = app.m_loaded, hadRenderer = app.m_renderer != nullptr;
        app.m_loaded = true;
        if (!hadRenderer) app.m_renderer = MakeD3D12Renderer();
        app.m_cachedChips = status_chips::Build(true, {true, 0.42, 75.0}, 58.6, 60.0, 2);
        app.m_cachedStatus = L"Status line";
        drawnText.clear();
        RECT client{};
        GetClientRect(app.m_hwnd, &client);
        app.RenderUi(dc, client);
        CHECK(Contains(L"Render 42% · ETA 1:15"));
        CHECK(Contains(L"59 / 60 fps"));
        CHECK(Contains(L"Dropped 2"));
        CHECK(Contains(L"Status line"));
        const auto row = app.StatusRowLayout();
        for (const RECT& chip : row.chips) {
            CHECK(chip.right > chip.left);
            CHECK(chip.left >= row.text.right);
        }
        app.m_loaded = loaded;
        app.m_neuralLifecycle = lifecycle;
        if (!hadRenderer) app.m_renderer.reset();
        app.m_cachedChips = {};
        app.m_cachedStatus.clear();
        CHECK(DeleteDC(dc));
    }

    // Drains the worker's answers until `done` holds or ten seconds pass; the
    // worker posts a bare message and the player takes what is ready.
    static bool PumpTimelineMedia(PlayerApp& app, const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            MSG message{};
            if (PeekMessageW(&message, app.m_hwnd, WM_TIMELINE_MEDIA, WM_TIMELINE_MEDIA, PM_REMOVE)) app.CompleteTimelineMedia();
            else Sleep(10);
        }
        return done();
    }

    // The render map end to end on real files: the greyed bar of a source with
    // no length, the hover facts of a cached render, a thumbnail and a chapter
    // list fetched by the staged FFmpeg tools off the UI thread.
    static void timeline_render_map_test()
    {
        PlayerApp& app = fixture->app;
        const bool loaded = app.m_loaded, cached = app.m_cachedPlayback, hadRenderer = app.m_renderer != nullptr;
        const auto range = app.m_cachedRange;
        const auto kind = app.m_sourceKind;
        const std::wstring path = app.m_path;
        const NeuralPlaybackLifecycle lifecycle = app.m_neuralLifecycle;
        app.m_neuralLifecycle.state = NeuralPlaybackState::Idle;
        if (!hadRenderer) app.m_renderer = MakeD3D12Renderer();
        app.m_loaded = true;
        app.m_cachedPlayback = false;

        // No length: the bar takes no click, and the hover says how to seek.
        // A closed decoder still remembers what it last described, so the
        // fixture's is set aside for an empty one and put back at the end.
        VideoDecoder saved;
        saved.Swap(app.m_decoder);
        const RECT track = app.TimelineRect();
        CHECK_EQ(LONG(app.Dip(18)), track.bottom - track.top);
        const POINT middle{(track.left + track.right) / 2, (track.top + track.bottom) / 2};
        app.MouseDown(middle.x, middle.y);
        CHECK(!app.m_dragSeek);
        app.MouseUp(middle.x, middle.y);
        app.UpdateTimelineHover(middle.x, middle.y);
        CHECK(app.m_previewText.find(L"Length unknown") == 0);
        CHECK(!app.m_previewKey.has_value());
        app.UpdateTimelineHover(middle.x, track.top - app.Dip(20));
        CHECK(!app.m_timelineHoverX.has_value());

        const auto directory = app.SettingsPath().parent_path();
        const auto avi = directory / L"timeline-64x48.avi";
        WriteTinyAvi(avi, 64, 48, 30);
        app.m_sourceKind = MediaSourceKind::LocalFile;
        app.m_path = avi.wstring();
        REQUIRE(app.m_decoder.OpenMetadata(avi.wstring()));
        REQUIRE(app.m_decoder.DurationSeconds() > 0.0);
        // The file stands in for its own neural render: what is checked is
        // that a moment inside a cached range asks the render, not the source.
        app.m_cachedPlayback = true;
        app.m_cachedRange = {};
        app.m_neuralPath = avi;
        app.SyncTimelineMedia();
        CHECK(app.m_timelineFile == avi);
        app.UpdateTimelineHover(middle.x, middle.y);
        CHECK(app.m_previewText.find(L" · Rendered") != std::wstring::npos);
        REQUIRE(app.m_previewKey.has_value());
        CHECK_EQ(int64_t{1}, *app.m_previewKey % 2);
        const bool haveTools = std::filesystem::exists(PlayerApp::ExecutableDirectory() / L"ffmpeg.exe") &&
                               std::filesystem::exists(PlayerApp::ExecutableDirectory() / L"ffprobe.exe");
        if (haveTools) {
            const int64_t key = *app.m_previewKey;
            CHECK(PumpTimelineMedia(app, [&] { return app.m_thumbnails.Find(key) != nullptr; }));
            if (const auto* thumbnail = app.m_thumbnails.Find(key)) {
                const SIZE expected = app.TimelineThumbnailSize();
                CHECK_EQ(expected.cx, thumbnail->size.cx);
                CHECK_EQ(expected.cy, thumbnail->size.cy);
                CHECK_EQ(size_t(expected.cx) * size_t(expected.cy) * 4u, thumbnail->bgra.size());
            }

            // A chaptered copy, made by the same ffmpeg: two chapters of 50 ms.
            const auto meta = directory / L"timeline-chapters.txt";
            {
                std::ofstream out(meta, std::ios::binary);
                out << ";FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=50\ntitle=Opening, cold\n"
                       "[CHAPTER]\nTIMEBASE=1/1000\nSTART=50\nEND=100\ntitle=Credits\n";
            }
            const auto chaptered = directory / L"timeline-chapters.mkv";
            std::filesystem::remove(chaptered);
            CHECK(RunToolCapture(PlayerApp::ExecutableDirectory() / L"ffmpeg.exe",
                                 {L"-v", L"error", L"-nostdin", L"-y", L"-i", avi.wstring(), L"-i", meta.wstring(),
                                  L"-map", L"0:v", L"-map_metadata", L"1", L"-map_chapters", L"1", L"-c:v", L"ffv1",
                                  chaptered.wstring()},
                                 {}, std::chrono::seconds(20), 1u << 20).has_value());
            app.m_path = chaptered.wstring();
            REQUIRE(app.m_decoder.OpenMetadata(chaptered.wstring()));
            app.m_cachedPlayback = false;
            app.SyncTimelineMedia();
            CHECK(app.m_chapters.empty());
            CHECK(PumpTimelineMedia(app, [&] { return app.m_chapters.size() == 2; }));
            if (app.m_chapters.size() == 2) {
                CHECK(app.m_chapters[0].title == L"Opening, cold");
                CHECK(app.m_chapters[1].title == L"Credits");
                CHECK(app.TimelineHoverFacts(0.07).chapter == L"Credits");
                CHECK(!app.TimelineHoverFacts(0.07).rendered.has_value());
            }
            // Another file retires the old one's chapters and thumbnails.
            const uint64_t generation = app.m_timelineGeneration;
            app.m_path.clear();
            app.SyncTimelineMedia();
            CHECK(app.m_timelineGeneration != generation);
            CHECK(app.m_chapters.empty());
            CHECK_EQ(size_t{0}, app.m_thumbnails.Size());
            std::filesystem::remove(chaptered);
            std::filesystem::remove(meta);
        }

        app.ClearTimelineHover();
        app.m_decoder.Close();
        app.m_decoder.Swap(saved);
        std::filesystem::remove(avi);
        app.m_neuralPath.clear();
        app.m_path = path;
        app.m_sourceKind = kind;
        app.m_cachedRange = range;
        app.m_cachedPlayback = cached;
        app.m_loaded = loaded;
        app.m_neuralLifecycle = lifecycle;
        if (!hadRenderer) app.m_renderer.reset();
        app.SyncTimelineMedia();
    }

    // ? and F1 open the cheat sheet from anywhere, Esc and the same keys put
    // it away, and Help > Keyboard shortcuts is the menu route to it.
    static void keyboard_cheat_sheet_test()
    {
        PlayerApp& app = fixture->app;
        REQUIRE(!app.m_shortcutSheetOpen);
        app.WndProc(app.m_hwnd, WM_KEYDOWN, VK_F1, 0);
        CHECK(app.m_shortcutSheetOpen);
        CHECK(!app.m_shortcutGroups.empty());
        size_t rows = 0;
        for (const auto& group : app.m_shortcutGroups) rows += group.rows.size();
        CHECK(rows >= 30);
        app.WndProc(app.m_hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        CHECK(!app.m_shortcutSheetOpen);
        app.WndProc(app.m_hwnd, WM_CHAR, L'?', 0);
        CHECK(app.m_shortcutSheetOpen);
        app.WndProc(app.m_hwnd, WM_CHAR, L'?', 0);
        CHECK(!app.m_shortcutSheetOpen);
        // The same keys pressed while the picture holds the keyboard: the render
        // window passed F1 on as a key but kept the ? character, so ? did nothing.
        if (app.m_renderWnd) {
            SendMessageW(app.m_renderWnd, WM_CHAR, L'?', 0);
            CHECK(app.m_shortcutSheetOpen);
            SendMessageW(app.m_renderWnd, WM_KEYDOWN, VK_F1, 0);
            CHECK(!app.m_shortcutSheetOpen);
        }
        app.HandleCommand(IDM_KEYBOARD_SHORTCUTS);
        CHECK(app.m_shortcutSheetOpen);
        // Esc closes the sheet and does nothing else: no fullscreen to leave,
        // no job to cancel, even if one were running.
        const bool fullscreen = app.m_fullscreen;
        app.WndProc(app.m_hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        CHECK(!app.m_shortcutSheetOpen);
        CHECK_EQ(fullscreen, app.m_fullscreen);
    }

    // Every settings dialog is laid out in design units at its own dpi, draws
    // in its own dialog font, is dark, and follows a move to another dpi.
    static void settings_dialogs_are_dpi_scaled_and_dark_test()
    {
        PlayerApp& app = fixture->app;
        struct DialogCase { void (PlayerApp::*show)(); HWND PlayerApp::*window; int designW, designH; int button; int defaultButton; };
        const std::array cases{
            DialogCase{&PlayerApp::ShowAdjustments, &PlayerApp::m_adjustWnd, PlayerApp::kAdjustDesignW, PlayerApp::kAdjustDesignH, IDC_ADJ_RESET, IDC_ADJ_CLOSE},
            DialogCase{&PlayerApp::ShowNeuralSettings, &PlayerApp::m_neuralWnd, PlayerApp::kNeuralDesignW, PlayerApp::kNeuralDesignH, IDC_NS_RESET, IDC_NS_APPLY},
            DialogCase{&PlayerApp::ShowEncoderSettings, &PlayerApp::m_encoderWnd, PlayerApp::kEncoderDesignW, PlayerApp::kEncoderDesignH, IDC_ES_RESET, IDC_ES_CLOSE},
            DialogCase{&PlayerApp::ShowExportStages, &PlayerApp::m_exportStagesWnd, PlayerApp::kExportDesignW, PlayerApp::kExportDesignH, IDC_EX_CLOSE, IDC_EX_RUN},
        };
        for (const auto& dialogCase : cases) {
            (app.*dialogCase.show)();
            const HWND dialog = app.*dialogCase.window;
            REQUIRE(dialog != nullptr);
            const UINT dpi = ActiveWindowDpi(dialog);
            RECT client{};
            GetClientRect(dialog, &client);
            CHECK_EQ(LONG(MulDiv(dialogCase.designW, int(dpi), 96)), client.right);
            CHECK_EQ(LONG(MulDiv(dialogCase.designH, int(dpi), 96)), client.bottom);
            // Its own font at its own dpi, never the 96-dpi stock one.
            const HWND button = GetDlgItem(dialog, dialogCase.button);
            REQUIRE(button != nullptr);
            const HFONT font = reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0));
            CHECK(font != GetStockObject(DEFAULT_GUI_FONT));
            LOGFONTW logical{};
            CHECK(GetObjectW(font, sizeof(logical), &logical) != 0);
            CHECK_EQ(LONG(dark_mode::DialogFontHeight(dpi)), logical.lfHeight);
            // Owner-drawn and dark; the default button is marked for the accent.
            CHECK_EQ(LONG_PTR(BS_OWNERDRAW), GetWindowLongPtrW(button, GWL_STYLE) & BS_TYPEMASK);
            CHECK(GetPropW(GetDlgItem(dialog, dialogCase.defaultButton), kDefaultButtonProperty) != nullptr);
            CHECK(GetPropW(button, kDefaultButtonProperty) == nullptr);
            HDC dc = CreateCompatibleDC(nullptr);
            REQUIRE(dc != nullptr);
            const LRESULT brush = SendMessageW(dialog, WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc), 0);
            CHECK_EQ(reinterpret_cast<LRESULT>(DarkDialogBrush()), brush);
            CHECK_EQ(dark_mode::Text, GetTextColor(dc));
            const HDC screen = GetDC(nullptr);
            HBITMAP bitmap = CreateCompatibleBitmap(screen, 200, 60);
            ReleaseDC(nullptr, screen);
            const HGDIOBJ previous = SelectObject(dc, bitmap);
            DRAWITEMSTRUCT item{};
            item.CtlType = ODT_BUTTON; item.CtlID = UINT(dialogCase.button); item.hwndItem = button; item.hDC = dc;
            item.rcItem = RECT{0, 0, 120, 40};
            drawnText.clear();
            CHECK_EQ(LRESULT{TRUE}, SendMessageW(dialog, WM_DRAWITEM, WPARAM(dialogCase.button), reinterpret_cast<LPARAM>(&item)));
            CHECK(!drawnText.empty());
            SelectObject(dc, previous);
            DeleteObject(bitmap);
            DeleteDC(dc);

            // Moving to a monitor at 1.5x: the controls, the font and the
            // client all follow, from the dialog's own baseline.
            const UINT larger = dpi * 3 / 2;
            RECT button96{};
            GetWindowRect(button, &button96);
            RECT window{};
            GetWindowRect(dialog, &window);
            const DWORD style = DWORD(GetWindowLongPtrW(dialog, GWL_STYLE)), exStyle = DWORD(GetWindowLongPtrW(dialog, GWL_EXSTYLE));
            // What Windows would suggest: the client scaled to the new dpi. The
            // frame is added at the monitor's real dpi, because only the
            // message is faked here and the frame Windows draws is not.
            RECT suggested{0, 0, MulDiv(dialogCase.designW, int(larger), 96), MulDiv(dialogCase.designH, int(larger), 96)};
            AdjustWindowRectForDpi(suggested, style, FALSE, exStyle, dpi);
            OffsetRect(&suggested, window.left - suggested.left, window.top - suggested.top);
            SendMessageW(dialog, WM_DPICHANGED, MAKEWPARAM(larger, larger), reinterpret_cast<LPARAM>(&suggested));
            RECT buttonLarger{};
            GetWindowRect(button, &buttonLarger);
            CHECK(std::abs((buttonLarger.right - buttonLarger.left) - MulDiv(button96.right - button96.left, int(larger), int(dpi))) <= 1);
            const HFONT largerFont = reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0));
            CHECK(GetObjectW(largerFont, sizeof(logical), &logical) != 0);
            CHECK_EQ(LONG(dark_mode::DialogFontHeight(larger)), logical.lfHeight);
            GetClientRect(dialog, &client);
            CHECK(std::abs(client.right - MulDiv(dialogCase.designW, int(larger), 96)) <= 1);
            // And the bottom-right buttons stayed anchored to the new corner.
            POINT corner{buttonLarger.right, buttonLarger.bottom};
            ScreenToClient(dialog, &corner);
            CHECK(corner.x <= client.right && corner.y <= client.bottom);
            CHECK(client.bottom - corner.y <= MulDiv(60, int(larger), 96));
            DestroyWindow(dialog);
            CHECK(app.*dialogCase.window == nullptr);
            CHECK(app.m_dialogFonts.find(dialog) == app.m_dialogFonts.end());
        }
    }

    // The modal prompts share the chrome: owner-drawn buttons, dark colours,
    // and a font of their own once a dpi change replaces the one lent to them.
    static void modal_prompts_are_dark_and_follow_the_dpi_test()
    {
        PlayerApp& app = fixture->app;
        WNDCLASSW prompt{};
        prompt.lpfnWndProc = TimecodeDialogProc;
        prompt.hInstance = GetModuleHandleW(nullptr);
        prompt.lpszClassName = L"DLSSPlayerUiRegressionTimecode";
        prompt.hbrBackground = DarkDialogBrush();
        CHECK(RegisterClassW(&prompt) != 0);
        TimecodeDialogState state{&app.m_loc, app.m_font, L"00:00:01:00", [](const std::wstring&, TimecodeAction) { return true; }};
        const HWND dialog = CreateWindowExW(0, prompt.lpszClassName, L"Timecode", WS_POPUP | WS_CAPTION,
                                            0, 0, 440, 190, nullptr, nullptr, prompt.hInstance, &state);
        REQUIRE(dialog != nullptr);
        const HWND go = GetDlgItem(dialog, IDOK);
        REQUIRE(go != nullptr);
        CHECK_EQ(LONG_PTR(BS_OWNERDRAW), GetWindowLongPtrW(go, GWL_STYLE) & BS_TYPEMASK);
        CHECK(GetPropW(go, kDefaultButtonProperty) != nullptr);
        HDC dc = CreateCompatibleDC(nullptr);
        REQUIRE(dc != nullptr);
        CHECK_EQ(reinterpret_cast<LRESULT>(DarkDialogBrush()),
                 SendMessageW(dialog, WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(state.error)));
        CHECK_EQ(dark_mode::ErrorText, GetTextColor(dc));
        CHECK_EQ(reinterpret_cast<LRESULT>(DarkFieldBrush()),
                 SendMessageW(dialog, WM_CTLCOLOREDIT, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(state.edit)));
        DeleteDC(dc);
        RECT before{};
        GetWindowRect(go, &before);
        const UINT from = state.dpi, to = from * 2;
        RECT window{};
        GetWindowRect(dialog, &window);
        SendMessageW(dialog, WM_DPICHANGED, MAKEWPARAM(to, to), reinterpret_cast<LPARAM>(&window));
        RECT after{};
        GetWindowRect(go, &after);
        CHECK_EQ(before.right - before.left, (after.right - after.left) / 2);
        CHECK(state.ownedFont != nullptr);
        CHECK_EQ(reinterpret_cast<LRESULT>(state.ownedFont), SendMessageW(go, WM_GETFONT, 0, 0));
        CHECK_EQ(to, state.dpi);
        DestroyWindow(dialog);
        CHECK(state.done);
        CHECK(state.ownedFont == nullptr);
        UnregisterClassW(prompt.lpszClassName, prompt.hInstance);
    }

    // The menu bar is drawn dark through the undocumented UAH messages, and a
    // message that does not check out hands the bar back to Windows for good.
    static void dark_menu_bar_test()
    {
        PlayerApp& app = fixture->app;
        app.m_darkMenuFailed = false;
        HDC dc = CreateCompatibleDC(nullptr);
        REQUIRE(dc != nullptr);
        const HDC screen = GetDC(nullptr);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, 200, 40);
        ReleaseDC(nullptr, screen);
        const HGDIOBJ previous = SelectObject(dc, bitmap);
        dark_mode::UAHDRAWMENUITEM item{};
        item.um.hmenu = GetMenu(app.m_hwnd);
        item.um.hdc = dc;
        item.umi.iPosition = 0;
        item.dis.rcItem = RECT{0, 0, 60, 20};
        item.dis.itemState = ODS_HOTLIGHT;
        drawnText.clear();
        CHECK_EQ(LRESULT{TRUE}, app.WndProc(app.m_hwnd, dark_mode::WM_UAHDRAWMENUITEM, 0, reinterpret_cast<LPARAM>(&item)));
        CHECK(Contains(L"File"));
        CHECK_EQ(ui_palette::Hover, GetPixel(dc, 2, 2));
        dark_mode::UAHMENU bar{GetMenu(app.m_hwnd), dc, 0};
        CHECK(app.WndProc(app.m_hwnd, dark_mode::WM_UAHDRAWMENU, 0, reinterpret_cast<LPARAM>(&bar)) == TRUE);
        CHECK(!app.m_darkMenuFailed);
        // A structure that names another menu is not trusted, and latches the
        // dark drawing off: the next well-formed message is not drawn either.
        HMENU stranger = CreateMenu();
        item.um.hmenu = stranger;
        drawnText.clear();
        CHECK(app.WndProc(app.m_hwnd, dark_mode::WM_UAHDRAWMENUITEM, 0, reinterpret_cast<LPARAM>(&item)) != TRUE);
        CHECK(app.m_darkMenuFailed);
        item.um.hmenu = GetMenu(app.m_hwnd);
        CHECK(app.WndProc(app.m_hwnd, dark_mode::WM_UAHDRAWMENUITEM, 0, reinterpret_cast<LPARAM>(&item)) != TRUE);
        CHECK(!Contains(L"File"));
        DestroyMenu(stranger);
        app.m_darkMenuFailed = false;
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }

    // The media controls' and the thumbnail's presses take the player's own
    // paths; the thumbnail's icons are the toolbar's glyphs with real alpha.
    static void media_controls_and_thumbnail_buttons_test()
    {
        PlayerApp& app = fixture->app;
        // Nothing loaded: every press is a no-op, and the bar says so.
        const bool loaded = app.m_loaded, playing = app.m_playing;
        app.m_loaded = false;
        app.WndProc(app.m_hwnd, WM_MEDIA_BUTTON, media_transport::kSmtcPlay, 0);
        CHECK(!app.m_playing);
        for (const auto& button : app.CurrentThumbBar()) CHECK(!button.enabled);

        // The side-by-side switch flips between the Compare menu's Neural and
        // Split, and only where the menu's modes are available.
        const bool hadRenderer = app.m_renderer != nullptr;
        if (!hadRenderer) app.m_renderer = MakeD3D12Renderer();
        const bool cached = app.m_cachedPlayback, requested = app.m_neuralRequested;
        const ComparisonMode mode = app.m_comparison.mode;
        app.m_loaded = true; app.m_cachedPlayback = true; app.m_neuralRequested = true;
        app.m_comparison.mode = ComparisonMode::Neural;
        REQUIRE(app.ComparisonModesAvailable());
        app.WndProc(app.m_hwnd, WM_COMMAND, MAKEWPARAM(IDM_COMPARE_TOGGLE, THBN_CLICKED), 0);
        CHECK(app.m_comparison.mode == ComparisonMode::SideBySide);
        CHECK(app.CurrentThumbBar()[2].enabled);
        CHECK(std::wstring_view(app.CurrentThumbBar()[2].tipKey) == L"thumb.compare_off");
        app.HandleCommand(IDM_COMPARE_TOGGLE);
        CHECK(app.m_comparison.mode == ComparisonMode::Neural);
        app.m_neuralRequested = false;
        app.HandleCommand(IDM_COMPARE_TOGGLE);
        CHECK(app.m_comparison.mode == ComparisonMode::Neural);
        app.m_comparison.mode = mode; app.m_cachedPlayback = cached; app.m_neuralRequested = requested;
        app.m_loaded = loaded; app.m_playing = playing;
        if (!hadRenderer) app.m_renderer.reset();

        // The WinRT half, against this hidden window: combase resolves, the
        // interop factory hands back controls, and every setter runs. Status
        // stays Closed, so the controls stay disabled and nothing reaches the
        // shared machine's media flyout while the suite runs.
        {
            MediaTransportControls controls;
            const bool attached = controls.Attach(app.m_hwnd, WM_MEDIA_BUTTON, WM_MEDIA_SEEK);
            std::cout << "SMTC on a hidden window: " << (attached ? "attached" : "unavailable") << '\n';
            CHECK(attached == controls.Attached());
            controls.SetStatus(media_transport::Status::Closed);
            controls.SetTitle(L"UI regression");
            controls.SetTimeline(12.0, 3.0);
            controls.SetTitle(L"");
            controls.Detach();
            CHECK(!controls.Attached());
        }

        // A glyph icon is white where the glyph is and transparent elsewhere.
        const HICON icon = RenderGlyphIcon(GlyphForIcon(UiIcon::Play), 32);
        REQUIRE(icon != nullptr);
        ICONINFO info{};
        REQUIRE(GetIconInfo(icon, &info));
        BITMAP bitmap{};
        CHECK(GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) != 0);
        CHECK_EQ(LONG{32}, bitmap.bmWidth);
        std::vector<uint32_t> pixels(32 * 32);
        BITMAPINFO header{};
        header.bmiHeader.biSize = sizeof(header.bmiHeader); header.bmiHeader.biWidth = 32; header.bmiHeader.biHeight = -32;
        header.bmiHeader.biPlanes = 1; header.bmiHeader.biBitCount = 32; header.bmiHeader.biCompression = BI_RGB;
        HDC dc = CreateCompatibleDC(nullptr);
        CHECK(GetDIBits(dc, info.hbmColor, 0, 32, pixels.data(), &header, DIB_RGB_COLORS) == 32);
        DeleteDC(dc);
        size_t opaque = 0, clear = 0;
        for (const uint32_t pixel : pixels) {
            const uint32_t alpha = pixel >> 24;
            if (alpha == 255) ++opaque;
            if (alpha == 0) ++clear;
            // Premultiplied white: no channel above its alpha.
            CHECK((pixel & 0xffu) <= alpha);
        }
        CHECK(opaque > 20);
        CHECK(clear > 400);
        DeleteObject(info.hbmColor);
        DeleteObject(info.hbmMask);
        DestroyIcon(icon);
    }

    // The start screen end to end: a real render published into a scratch
    // cache, the worker's facts about it (a frame from the staged ffmpeg and a
    // coverage badge), the panel and the tiles painted from them, and tiles
    // that hit-test to what they show.
    static void start_screen_test()
    {
        PlayerApp& app = fixture->app;
        REQUIRE(!app.m_loaded);
        const auto directory = app.SettingsPath().parent_path() / L"start-screen";
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        const auto avi = directory / L"clip.avi";
        // 96x64: the cache refuses a render under 64 pixels on a side.
        WriteTinyAvi(avi, 96, 64, 30);
        const std::string renderKey(64, 'c'), sourceKey(64, 'd');
        {
            NeuralCacheManager cache(directory / L"cache");
            REQUIRE(cache.Valid());
            const auto staging = cache.BeginRenderStaging(renderKey);
            REQUIRE(staging.has_value());
            std::filesystem::copy_file(avi, *staging / L"neural.mkv");
            const std::string receipt = "{\"receipt\":1}";
            { std::ofstream out(*staging / L"receipt.json", std::ios::binary); out << receipt; }
            NeuralCacheManifest manifest{};
            manifest.kind = NeuralCacheEntryKind::Render; manifest.state = NeuralCacheState::Staging;
            manifest.sourceDigest = std::string(64, 'a'); manifest.runtimeDigest = std::string(64, 'b');
            manifest.encoder = "hevc_nvenc"; manifest.width = 96; manifest.height = 64;
            manifest.frameCount = 3; manifest.duration100ns = 1000000;
            manifest.nativeEvaluations = 3; manifest.verifiedNeuralFrames = 3; manifest.observedFeature18Evaluations = 1;
            manifest.feature18Created = true; manifest.feature18ArmedBeforeCapture = true;
            manifest.receiptDigest = Sha256Bytes(receipt).value_or("");
            REQUIRE(cache.PromoteRender(renderKey, *staging, manifest));
        }
        // The read-only peek the worker uses: the manifest and where the payload
        // is, with no last-use mark - a tile on screen must not reorder eviction.
        {
            const auto entryDirectory = directory / L"cache" / L"renders" / std::wstring(renderKey.begin(), renderKey.end());
            std::error_code error;
            const auto writtenBefore = std::filesystem::last_write_time(entryDirectory, error);
            REQUIRE(!error);
            const auto peeked = NeuralCacheManager::Peek(directory / L"cache", NeuralCacheEntryKind::Render, renderKey);
            REQUIRE(peeked.has_value());
            CHECK(peeked->payloadPath == entryDirectory / L"neural.mkv");
            CHECK_EQ(96u, peeked->manifest.width);
            CHECK(std::filesystem::last_write_time(entryDirectory, error) == writtenBefore);
            CHECK(!NeuralCacheManager::Peek(directory / L"cache", NeuralCacheEntryKind::Source, renderKey).has_value());
            CHECK(!NeuralCacheManager::Peek(directory / L"cache", NeuralCacheEntryKind::Render, "..").has_value());
            CHECK(!NeuralCacheManager::Peek({}, NeuralCacheEntryKind::Render, renderKey).has_value());
        }

        // The worker, run here on this thread: the runtime verdict, then the
        // render's badge and frame.
        auto answers = std::make_shared<StartScreenAnswers>();
        answers->generation = 1;
        StartScreenRequest request{PlayerApp::ExecutableDirectory(), directory / L"cache", {{renderKey, sourceKey, false}}, 96};
        GatherStartScreen(request, answers, 1, app.m_hwnd, WM_START_SCREEN, {});
        CHECK(answers->runtime != start_screen::RuntimeState::Checking);
        REQUIRE(answers->renders.count(renderKey) == 1);
        CHECK(answers->renders[renderKey].badge == L"Rendered 100%");
        if (std::filesystem::exists(PlayerApp::ExecutableDirectory() / L"ffmpeg.exe")) {
            REQUIRE(answers->renders[renderKey].thumbnail.has_value());
            CHECK_EQ(LONG{96}, answers->renders[renderKey].thumbnail->size.cx);
            CHECK_EQ(LONG{64}, answers->renders[renderKey].thumbnail->size.cy);
        }
        MSG message{};
        while (PeekMessageW(&message, app.m_hwnd, WM_START_SCREEN, WM_START_SCREEN, PM_REMOVE)) {}
        // A newer request retires the older one's answers mid-flight.
        answers->generation = 2;
        GatherStartScreen(request, answers, 1, app.m_hwnd, WM_START_SCREEN, {});
        CHECK(!PeekMessageW(&message, app.m_hwnd, WM_START_SCREEN, WM_START_SCREEN, PM_REMOVE));

        // The screen, at the default window size, painted from those answers.
        auto recent = std::move(app.m_recent);
        const auto previousAnswers = app.m_startAnswers;
        app.m_recent = std::make_unique<RecentMediaHistory>(directory / L"recent.dat");
        RecentMediaEntry entry{};
        entry.title = L"Start screen clip"; entry.source = avi.wstring();
        // A local entry carries no source key: the file itself is the source.
        entry.renderKey = renderKey;
        app.m_recent->Remember(entry);
        answers->generation = 1;
        app.m_startAnswers = answers;
        RECT window{};
        GetWindowRect(app.m_hwnd, &window);
        SetWindowPos(app.m_hwnd, nullptr, 0, 0, 1460, 1000, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        const auto layout = app.StartLayout();
        REQUIRE(layout.full);
        REQUIRE(layout.recentTiles.size() == 1);
        CHECK(!layout.trailerTiles.empty());
        // The default fixture has no GPU, so the panel fails that check and
        // offers safe mode.
        CHECK(layout.safeMode.right > layout.safeMode.left);
        // An earlier case leaves a job's lifecycle running, which would put
        // the whole-window progress panel up instead of the idle screen.
        const NeuralPlaybackLifecycle lifecycle = app.m_neuralLifecycle;
        app.m_neuralLifecycle.state = NeuralPlaybackState::Idle;
        HDC dc = CreateCompatibleDC(nullptr);
        REQUIRE(dc != nullptr);
        RECT client{};
        GetClientRect(app.m_hwnd, &client);
        drawnText.clear();
        app.RenderUi(dc, client);
        app.m_neuralLifecycle = lifecycle;
        CHECK(Contains(L"GPU"));
        CHECK(Contains(L"Neural runtime"));
        CHECK(Contains(L"Start screen clip"));
        CHECK(Contains(L"Rendered 100%"));
        CHECK(Contains(app.T(L"start.recent").c_str()));
        CHECK(Contains(app.T(L"start.trailers").c_str()));
        CHECK(Contains(app.T(L"start.hint").c_str()));
        CHECK(Contains(app.T(L"start.safe_mode").c_str()));
        CHECK(Contains(std::wstring(kExampleVideos[0].title).c_str()));
        DeleteDC(dc);
        // Tiles and the link hit-test to what they show - and to nothing
        // while a job's progress panel covers them.
        const RECT tile = layout.recentTiles[0];
        const NeuralPlaybackLifecycle busy = app.m_neuralLifecycle;
        app.m_neuralLifecycle.state = NeuralPlaybackState::Rendering;
        CHECK(app.StartScreenHit((tile.left + tile.right) / 2, (tile.top + tile.bottom) / 2).first == PlayerApp::StartHover::None);
        app.m_neuralLifecycle.state = NeuralPlaybackState::Idle;
        const auto recentHit = app.StartScreenHit((tile.left + tile.right) / 2, (tile.top + tile.bottom) / 2);
        CHECK(recentHit.first == PlayerApp::StartHover::Recent);
        CHECK_EQ(size_t{0}, recentHit.second);
        const RECT trailer = layout.trailerTiles.back();
        const auto trailerHit = app.StartScreenHit((trailer.left + trailer.right) / 2, trailer.top + 2);
        CHECK(trailerHit.first == PlayerApp::StartHover::Trailer);
        CHECK(app.StartScreenHit(layout.hint.left + 1, layout.hint.top + 1).first == PlayerApp::StartHover::None);
        // The safe-mode link asks first; this suite answers no.
        const int boxes = messageBoxes;
        CHECK(app.ActivateStartScreen(layout.safeMode.left + 2, (layout.safeMode.top + layout.safeMode.bottom) / 2));
        CHECK_EQ(boxes + 1, messageBoxes);
        app.m_neuralLifecycle = busy;

        SetWindowPos(app.m_hwnd, nullptr, 0, 0, window.right - window.left, window.bottom - window.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        app.m_startAnswers = previousAnswers;
        app.m_recent = std::move(recent);
        std::filesystem::remove_all(directory);
    }

    static void source_menus_are_disabled_without_media_test()
    {
        PlayerApp& app = fixture->app;

        CheckSourceMenus(app, false);
    }

    static void source_menus_return_after_a_cancelled_job_test()
    {
        PlayerApp& app = fixture->app;

        CHECK(app.NeuralJobActive());
        app.CancelNeuralJob();
        CheckSourceMenus(app, true);
    }

    static void loading_feedback_test()
    {
        PlayerApp& app = fixture->app;

        CheckLoadingFeedback(app);
    }

    // Every presented pair used to be copied twice - into m_next when it was
    // read, and into m_lastPlaybackFrame when it was rendered - about 11 MB per
    // pair at 1440p, including the pairs the cadence skips. Both now share the
    // pair. A decoded frame is taken by swap, handing its old buffer back to the
    // decoder, and only a frame owned by nobody else is copied.
    static void playback_frames_are_shared_not_copied_test()
    {
        PlayerApp& app = fixture->app;
        const bool loaded = app.m_loaded, cached = app.m_cachedPlayback, requested = app.m_neuralRequested, seeking = app.m_seeking;
        NumberedFrameSource original(0);
        app.m_synchronizedPlayback = SynchronizedPlayback(original, [] {
            return std::unique_ptr<ISynchronizedFrameSource>(std::make_unique<NumberedFrameSource>(0));
        });
        auto segments = std::make_shared<NeuralSegmentIndex>();
        NeuralSegment part{}; part.path = L"segment.mkv"; part.runId = 1;
        part.frameCount = 6000; part.end100ns = int64_t(part.frameCount) * 333333;
        segments->Append(part);
        CHECK(app.m_synchronizedPlayback.OpenLive(L"original.mkv", segments, SynchronizedRange{}, {},
                                                  VideoDecoder::KnownMedia{1, 1, 30.0, 3600.0, {}}));
        app.m_loaded = true; app.m_seeking = false; app.m_seekPending = false; app.m_sourceKind = MediaSourceKind::LocalFile;
        app.m_liveSession = true; app.m_liveAttached = true; app.m_liveBuffering = false; app.m_liveResumePlaying = false;
        app.m_liveSegments = segments; app.m_liveDirectory.clear(); app.m_liveRange = NeuralRenderRange{0, part.end100ns};
        app.m_cachedPlayback = true; app.m_neuralRequested = true; app.m_haveNext = false; app.m_playing = true;
        app.m_pairStall = {}; app.m_pairStallRead = {};

        CHECK(app.ReadNextCachedFrame());
        CHECK(app.m_haveNext);
        CHECK(app.m_nextPairFrame != nullptr);
        CHECK(&app.NextFrame() == app.m_synchronizedPlayback.VisibleFrame());
        CHECK(app.m_next.bgra.empty());
        app.RememberPlaybackFrame(app.NextFrame());
        CHECK(app.m_lastPlaybackFrame.get() == app.m_nextPairFrame.get());
        // The remembered frame outlives the next read, which replaces the pair.
        const uint64_t kept = app.m_lastPlaybackFrame->frameNumber;
        app.m_haveNext = false; app.m_nextPairFrame.reset();
        CHECK(app.ReadNextCachedFrame());
        CHECK_EQ(kept, app.m_lastPlaybackFrame->frameNumber);
        CHECK(app.NextFrame().frameNumber != kept);

        // A decoded frame is taken, not copied...
        app.m_next = VideoFrame{}; app.m_next.bgra.assign(64, 7); app.m_next.frameNumber = 42;
        const uint8_t* first = app.m_next.bgra.data();
        app.RememberPlaybackFrame(app.m_next);
        CHECK(app.m_lastPlaybackFrame->bgra.data() == first);
        CHECK_EQ(uint64_t{42}, app.m_lastPlaybackFrame->frameNumber);
        // ...and the one after it hands that buffer back to be decoded into.
        app.m_next = VideoFrame{}; app.m_next.bgra.assign(64, 9); app.m_next.frameNumber = 43;
        const uint8_t* second = app.m_next.bgra.data();
        app.RememberPlaybackFrame(app.m_next);
        CHECK(app.m_lastPlaybackFrame->bgra.data() == second);
        CHECK_EQ(uint64_t{43}, app.m_lastPlaybackFrame->frameNumber);
        CHECK(app.m_next.bgra.data() == first);
        // Anything else is copied, since nothing says its owner is done with it.
        VideoFrame other; other.bgra.assign(64, 3); other.frameNumber = 99;
        app.RememberPlaybackFrame(other);
        CHECK(app.m_lastPlaybackFrame->bgra == other.bgra);
        CHECK(app.m_lastPlaybackFrame->bgra.data() != other.bgra.data());
        // Re-rendering the remembered frame keeps it.
        const VideoFrame* remembered = app.m_lastPlaybackFrame.get();
        app.RememberPlaybackFrame(*remembered);
        CHECK(app.m_lastPlaybackFrame.get() == remembered);

        app.m_synchronizedPlayback = SynchronizedPlayback{};
        app.m_next = VideoFrame{}; app.m_nextPairFrame.reset(); app.m_haveNext = false;
        app.m_lastPlaybackFrame.reset(); app.m_ownedPlaybackFrame.reset();
        app.m_liveSession = false; app.m_liveAttached = false; app.m_playing = false; app.m_seekPending = false;
        app.m_neuralNotice.clear(); app.m_liveSegments.reset(); app.m_liveRange = {};
        app.m_pairStall = {}; app.m_pairStallRead = {};
        app.m_loaded = loaded; app.m_cachedPlayback = cached; app.m_neuralRequested = requested; app.m_seeking = seeking;
        app.SyncFeatureMenuState();
    }

    // A live retarget stops the running job and returns: the worker is retired
    // by its own completion message rather than joined on the UI thread, and
    // nothing else starts until it has gone.
    static void retarget_retires_the_worker_test()
    {
        PlayerApp& app = fixture->app;
        CHECK(!app.NeuralJobActive());
        std::atomic<bool> release{false};
        const auto startWorker = [&] {
            // A helper slow to notice the token: it runs until released.
            app.m_neuralWorker = std::jthread([&release](std::stop_token) {
                while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
        };
        const uint64_t generation = app.m_neuralLifecycle.Begin();
        startWorker();
        app.CancelNeuralJob(false, true);
        CHECK(!app.NeuralJobActive());
        CHECK(!app.m_neuralWorker.joinable());
        CHECK(app.NeuralWorkerRetiring());
        // The next render is held back while it winds down.
        const bool loaded = app.m_loaded; app.m_loaded = true;
        CHECK(!app.RangeRenderAvailable());
        app.m_loaded = loaded;
        // A completion from some other job does not retire it.
        CompleteStale(app, generation + 1000);
        CHECK(app.NeuralWorkerRetiring());
        release = true;
        app.CompleteNeuralJob(QueueCompletion(app, generation, true));
        CHECK(!app.NeuralWorkerRetiring());
        CHECK(!app.NeuralJobActive());

        // A worker whose completion was drained by the cancel that retired it
        // is still reaped, once it has exited, by the next Tick.
        release = false;
        app.m_neuralLifecycle.Begin();
        startWorker();
        app.CancelNeuralJob(false, true);
        CHECK(app.NeuralWorkerRetiring());
        app.ReapRetiringNeuralWorker();
        CHECK(app.NeuralWorkerRetiring());
        release = true;
        for (int attempt = 0; attempt < 200 && app.NeuralWorkerRetiring(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            app.ReapRetiringNeuralWorker();
        }
        CHECK(!app.NeuralWorkerRetiring());

        // A cancel that waits joins a retiring worker first, whatever is active.
        release = false;
        app.m_neuralLifecycle.Begin();
        startWorker();
        app.CancelNeuralJob(false, true);
        CHECK(app.NeuralWorkerRetiring());
        std::jthread releaser([&release] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            release = true;
        });
        app.CancelNeuralJob(false);
        CHECK(!app.NeuralWorkerRetiring());
        app.DrainNeuralMessages();
    }

    // A paused frame is presented when something invalidated it, not at 60 Hz.
    static void paused_frame_presents_on_invalidation_test()
    {
        PlayerApp& app = fixture->app;
        const bool loaded = app.m_loaded, playing = app.m_playing, seeking = app.m_seeking;
        const bool seekPending = app.m_seekPending;
        if (!app.m_renderer) app.m_renderer = MakeD3D12Renderer();
        app.m_loaded = true; app.m_playing = false; app.m_seeking = false; app.m_seekPending = false;
        app.Tick();
        const uint64_t presents = app.m_staticPresents;
        for (int tick = 0; tick < 4; ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            app.Tick();
        }
        CHECK_EQ(presents, app.m_staticPresents);
        // A setting the present pass reads...
        app.m_renderer->SetComparison(app.m_renderer->GetComparison());
        app.Tick();
        CHECK_EQ(presents + 1, app.m_staticPresents);
        app.Tick();
        CHECK_EQ(presents + 1, app.m_staticPresents);
        // ...and the window under the frame.
        app.m_staticPresentPending = true;
        app.Tick();
        CHECK_EQ(presents + 2, app.m_staticPresents);
        app.Tick();
        CHECK_EQ(presents + 2, app.m_staticPresents);
        app.m_loaded = loaded; app.m_playing = playing; app.m_seeking = seeking; app.m_seekPending = seekPending;
    }

    // A menu, a window drag and a message box each run a modal loop that never
    // returns to the main pump, so Tick stopped and the video froze under a
    // playing audio track. A timer the modal loop dispatches drives Tick until
    // the loop ends, and a Tick is never entered twice.
    static void modal_loops_keep_ticking_test()
    {
        PlayerApp& app = fixture->app;
        CHECK(app.m_hwnd != nullptr);
        const bool loaded = app.m_loaded;
        app.m_loaded = false;
        CHECK_EQ(app.m_modalTickTimer, UINT_PTR{0});
        app.WndProc(app.m_hwnd, WM_ENTERMENULOOP, FALSE, 0);
        CHECK(app.m_modalTickTimer != 0);
        // The modal timer's WM_TIMER runs a Tick: a pending seek is consumed
        // (and refused, since nothing is loaded).
        app.m_seekPending = true;
        app.WndProc(app.m_hwnd, WM_TIMER, PlayerApp::kModalTickTimerId, 0);
        CHECK(!app.m_seekPending);
        // ...but not from inside a Tick: a message box raised by one dispatches
        // the same timer.
        app.m_seekPending = true; app.m_inTick = true;
        app.WndProc(app.m_hwnd, WM_TIMER, PlayerApp::kModalTickTimerId, 0);
        CHECK(app.m_seekPending);
        app.m_inTick = false;
        app.WndProc(app.m_hwnd, WM_EXITMENULOOP, FALSE, 0);
        CHECK_EQ(app.m_modalTickTimer, UINT_PTR{0});
        // A stray WM_TIMER after the loop ended is not a Tick.
        app.WndProc(app.m_hwnd, WM_TIMER, PlayerApp::kModalTickTimerId, 0);
        CHECK(app.m_seekPending);
        app.m_seekPending = false;

        app.WndProc(app.m_hwnd, WM_ENTERSIZEMOVE, 0, 0);
        CHECK(app.m_modalTickTimer != 0);
        app.WndProc(app.m_hwnd, WM_EXITSIZEMOVE, 0, 0);
        CHECK_EQ(app.m_modalTickTimer, UINT_PTR{0});

        // A message box or owned dialog does not tick: it is raised from inside
        // a command handler, where a Tick could act on half-updated state.
        app.WndProc(app.m_hwnd, WM_ENTERIDLE, MSGF_DIALOGBOX, 0);
        CHECK_EQ(app.m_modalTickTimer, UINT_PTR{0});
        // The main pump's Tick still ends a timer a modal loop left running.
        app.WndProc(app.m_hwnd, WM_ENTERSIZEMOVE, 0, 0);
        CHECK(app.m_modalTickTimer != 0);
        app.Tick();
        CHECK_EQ(app.m_modalTickTimer, UINT_PTR{0});
        app.m_loaded = loaded;
    }

    static void window_and_menu_teardown_test()
    {
        PlayerApp& app = fixture->app;
        WNDCLASSW& windowClass = fixture->windowClass;

        HMENU menu = GetMenu(app.m_hwnd);
        SetMenu(app.m_hwnd, nullptr);
        CHECK(DestroyMenu(menu));
        CHECK(DestroyWindow(app.m_hwnd));
        app.m_hwnd = nullptr;
        CHECK(UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance));
    }

    static void fullscreen_lifecycle_test()
    {
        CheckFullscreenLifecycle();
    }

    // ---- gpu: the prepared network renderer path ---------------------------
    // Registered as NetworkPreparedRendererSmoke, run by passing --gpu to this
    // same binary rather than by compiling main.cpp into a second target.
    //
    // Why these exist: nothing in this suite opened a network source, and the
    // prepared-renderer commit path is reached by NOTHING ELSE. Four defects
    // lived there at once - the open did not ask for NV12, the candidate
    // renderer was never told the layout, the geometry check measured every
    // frame at four bytes per pixel, and the guide generator was handed the
    // frame without its layout - and every one of them was invisible to 23
    // green tests. The kind is YouTube on both cases because that is the kind
    // the real path passes; the source is a local clip because the transport
    // is not what broke.
    static std::filesystem::path GpuWorkDirectory()
    {
        return gpuWorkDirectory.empty() ? std::filesystem::path(L"network-prepared") : gpuWorkDirectory;
    }

    // A clip with a colour description this project's GPU conversion either
    // implements or deliberately refuses. Generated rather than committed: two
    // ffmpeg filters describe it completely.
    static bool GenerateClip(const std::filesystem::path& out, bool bt709Limited)
    {
        std::error_code error;
        std::filesystem::create_directories(out.parent_path(), error);
        if (std::filesystem::exists(out, error)) return true;
        const std::wstring colour = bt709Limited
            ? std::wstring(L"-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv")
            // Not merely "different": BT.601 limited is a description
            // SourceNv12ConversionFor ACCEPTS for an export, but the PLAYBACK
            // gate refuses it because only the BT.709 limited inverse is
            // written. That is the branch this clip has to land on.
            : std::wstring(L"-colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m -color_range tv");
        std::wstring command = L"\"" + (gpuFfmpegDirectory / L"ffmpeg.exe").wstring() + L"\""
            L" -v error -nostdin -y -f lavfi -i testsrc2=s=1280x720:r=30:d=1"
            L" -c:v libx264 -pix_fmt yuv420p " + colour + L" \"" + out.wstring() + L"\"";
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
        WaitForSingleObject(pi.hProcess, 60000);
        DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return code == 0 && std::filesystem::exists(out, error);
    }

    // Everything CreateRendererCandidate needs from a PlayerApp that never ran
    // Create(): the render window class it instantiates, and a parent for it.
    static bool PrepareViewport(PlayerApp& app)
    {
        static bool registered = false;
        if (!registered) {
            WNDCLASSW r{}; r.style = CS_DBLCLKS | CS_OWNDC;
            r.lpfnWndProc = PlayerApp::RenderWndProcStatic;
            r.hInstance = GetModuleHandleW(nullptr);
            r.lpszClassName = L"DLSSVideoRenderClassV11";
            r.hCursor = LoadCursor(nullptr, IDC_ARROW);
            // A class this process already registered is not an error.
            registered = RegisterClassW(&r) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        if (!registered) return false;
        if (!app.m_viewport) {
            app.m_viewport = CreateWindowExW(0, L"STATIC", nullptr, WS_POPUP,
                                             0, 0, 320, 180, nullptr, nullptr,
                                             GetModuleHandleW(nullptr), nullptr);
        }
        return app.m_viewport != nullptr;
    }

    // Drives the REAL preparation function, then the real candidate build and
    // the real validation, and reports the layout each stage settled on.
    static bool RunPreparedPath(const std::filesystem::path& clip,
                                VideoPixelLayout& decoderLayout,
                                VideoPixelLayout& rendererLayout,
                                bool& validated)
    {
        PlayerApp app{AppOptions{}};
        if (!PrepareViewport(app)) { std::cerr << "stage=viewport winerr=" << GetLastError() << '\n'; return false; }
        YouTubeCompletion completion;
        completion.result.ok = true;
        completion.result.mediaUrl = clip.wstring();
        // PrepareYouTubeMedia is what a dropped preferNv12 would regress, so
        // this calls it rather than opening a decoder itself.
        PlayerApp::PrepareYouTubeMedia(completion, {}, 0, 0, false, DefaultNeuralCarrierQuality());
        if (!completion.mediaErrorKey.empty() || !completion.decoder ||
            completion.firstFrame.bgra.empty()) {
            std::wcerr << L"stage=prepare error=" << completion.mediaErrorKey
                       << L" decoder=" << (completion.decoder != nullptr)
                       << L" frameBytes=" << completion.firstFrame.bgra.size() << L'\n';
            return false;
        }
        decoderLayout = completion.decoder->PixelLayout();
        auto candidate = app.CreateRendererCandidate(completion);
        if (!candidate || !candidate->renderer) { std::cerr << "stage=candidate\n"; return false; }
        rendererLayout = candidate->renderer->ActiveSourceLayout();
        TemporalGuideGenerator guides;
        validated = app.ValidatePreparedFrame(completion, *candidate->renderer, guides);
        return true;
    }

    static void network_prepared_pair_agrees_on_nv12_test()
    {
        const auto clip = GpuWorkDirectory() / L"prepared-bt709.mp4";
        REQUIRE(GenerateClip(clip, true));
        VideoPixelLayout decoderLayout = VideoPixelLayout::Bgra;
        VideoPixelLayout rendererLayout = VideoPixelLayout::Bgra;
        bool validated = false;
        REQUIRE(RunPreparedPath(clip, decoderLayout, rendererLayout, validated));
        // The open asked for NV12 and a BT.709 limited even-geometry source
        // qualifies, so this is the assertion a dropped preferNv12 fails.
        CHECK(decoderLayout == VideoPixelLayout::Nv12);
        // The candidate was told the layout before Initialize. Without that it
        // stays on its Bgra default and every frame is refused below.
        CHECK(rendererLayout == VideoPixelLayout::Nv12);
        // The geometry check measured w*h*3/2, and the guide generator was
        // handed the frame's own layout.
        CHECK(validated);
    }

    static void network_prepared_falls_back_to_bgra_off_bt709_test()
    {
        const auto clip = GpuWorkDirectory() / L"prepared-bt601.mp4";
        REQUIRE(GenerateClip(clip, false));
        VideoPixelLayout decoderLayout = VideoPixelLayout::Nv12;
        VideoPixelLayout rendererLayout = VideoPixelLayout::Nv12;
        bool validated = false;
        REQUIRE(RunPreparedPath(clip, decoderLayout, rendererLayout, validated));
        // The playback gate takes BT.709 limited and nothing else, because that
        // is the only inverse written for the comparison reference. This pins
        // the refusal so NV12 cannot be made unconditional later.
        CHECK(decoderLayout == VideoPixelLayout::Bgra);
        CHECK(rendererLayout == VideoPixelLayout::Bgra);
        // ...and the path still works, which is the half of a fallback that is
        // easy to break and easy to forget to assert.
        CHECK(validated);
    }

    static constexpr ::test_support::TestCase kGpuCases[] = {
        UI_CASE(network_prepared_pair_agrees_on_nv12_test),
        UI_CASE(network_prepared_falls_back_to_bgra_off_bt709_test),
    };

    static constexpr ::test_support::TestCase kCases[] = {
        UI_CASE(cache_settings_round_trip_test),
        UI_CASE(saved_settings_survive_a_reload_test),
        UI_CASE(hidden_window_and_menu_bar_open_test),
        UI_CASE(old_source_policy_requests_a_fresh_resolution_test),
        UI_CASE(recent_rollover_keeps_the_cache_test),
        UI_CASE(neural_button_label_and_upscaling_rungs_test),
        UI_CASE(feature_menu_mirrors_the_toolbar_test),
        UI_CASE(markers_and_timecode_test),
        UI_CASE(comparison_availability_test),
        UI_CASE(neural_strength_dial_test),
        UI_CASE(live_buffering_play_intent_test),
        UI_CASE(neural_toggle_queued_during_seek_test),
        UI_CASE(export_stages_dialog_test),
        UI_CASE(neural_settings_dialog_test),
        UI_CASE(settings_ahead_notice_test),
        UI_CASE(render_report_test),
        UI_CASE(encoder_settings_dialog_test),
        UI_CASE(settings_dialog_tips_survive_a_second_dialog_test),
        UI_CASE(live_export_entry_test),
        UI_CASE(dropped_preview_job_test),
        UI_CASE(unload_drops_deferred_toggle_test),
        UI_CASE(live_job_directory_failure_test),
        UI_CASE(live_render_failure_limit_test),
        UI_CASE(job_source_key_guard_test),
        UI_CASE(stream_conversion_uses_the_acquired_copy_test),
        UI_CASE(live_out_of_sync_hands_back_test),
        UI_CASE(pair_stall_bound_ignores_a_pause_test),
        UI_CASE(nv12_reference_inverse_is_bt709_limited_test),
        UI_CASE(live_pace_confirmation_test),
        UI_CASE(toolbar_pills_and_progress_panel_test),
        UI_CASE(status_chips_and_narrow_pills_fit_test),
        UI_CASE(timeline_render_map_test),
        UI_CASE(keyboard_cheat_sheet_test),
        UI_CASE(settings_dialogs_are_dpi_scaled_and_dark_test),
        UI_CASE(modal_prompts_are_dark_and_follow_the_dpi_test),
        UI_CASE(dark_menu_bar_test),
        UI_CASE(media_controls_and_thumbnail_buttons_test),
        UI_CASE(start_screen_test),
        UI_CASE(source_menus_are_disabled_without_media_test),
        UI_CASE(source_menus_return_after_a_cancelled_job_test),
        UI_CASE(loading_feedback_test),
        UI_CASE(modal_loops_keep_ticking_test),
        UI_CASE(retarget_retires_the_worker_test),
        UI_CASE(playback_frames_are_shared_not_copied_test),
        UI_CASE(paused_frame_presents_on_invalidation_test),
        UI_CASE(window_and_menu_teardown_test),
        UI_CASE(fullscreen_lifecycle_test),
    };


    // Helpers below are invoked by the cases above, in registry order.
    static void CheckMarkersAndTimecode(PlayerApp& app)
    {
        // Loaded cached playback with a bare renderer object; the closed
        // decoder reports 30 fps and an unknown duration.
        app.m_seeking = false; app.m_seekPending = false; app.m_playing = false;
        app.m_currentSec = 1.51; // Off-grid position snaps to frame 45 at 30 fps.
        app.HandleCommand(IDM_MARK_IN);
        CHECK(app.m_markers.in100ns.has_value() && !app.m_markers.out100ns.has_value());
        CHECK_EQ(app.m_markers.in100ns.value_or(0), int64_t{15000000});
        CHECK(app.BuildStatusText().find(L" \u00b7 In 00:00:01:15") != std::wstring::npos);
        CHECK(app.BuildStatusText().find(L"Out ") == std::wstring::npos);
        // The timecode dialog handler: non-drop, frame and millisecond forms
        // land on the frame grid; invalid text is refused without side effects.
        CHECK(app.ApplyTimecodeText(L"00:00:03:00", TimecodeAction::SetOut));
        CHECK_EQ(app.m_markers.out100ns.value_or(0), int64_t{30000000});
        CHECK(app.BuildStatusText().find(L" \u00b7 In 00:00:01:15 \u00b7 Out 00:00:03:00") != std::wstring::npos);
        const auto range = RangeFromMarkers(app.m_markers, app.m_decoder.FrameRate(), 100000000);
        CHECK(range.has_value());
        if (range) { CHECK_EQ(range->start100ns, int64_t{15000000}); CHECK_EQ(range->end100ns, int64_t{30000000}); }
        CHECK(!app.ApplyTimecodeText(L"nonsense", TimecodeAction::Go));
        CHECK(!app.ApplyTimecodeText(L"", TimecodeAction::SetIn));
        CHECK(!app.m_seekPending);
        CHECK_EQ(app.m_markers.in100ns.value_or(0), int64_t{15000000});
        CHECK(app.ApplyTimecodeText(L"f75", TimecodeAction::Go));
        CHECK(app.m_seekPending);
        CHECK(std::abs(app.m_pendingSeekSec - 2.5) < 1e-9);
        app.m_seekPending = false;
        // Markers only render as a range when In precedes Out.
        CHECK(app.ApplyTimecodeText(L"0:00.500", TimecodeAction::SetOut));
        CHECK(!RangeFromMarkers(app.m_markers, app.m_decoder.FrameRate(), 100000000).has_value());
        app.HandleCommand(IDM_CLEAR_MARKS);
        CHECK(!app.m_markers.in100ns.has_value() && !app.m_markers.out100ns.has_value());
        CHECK(app.BuildStatusText().find(L"In ") == std::wstring::npos);
        // Cached range playback names its range and the settings it was rendered with.
        app.m_cachedRange = NeuralRenderRange{15000000, 30000000};
        app.m_cachedSettings.intensity = 1.25f; app.m_cachedGuides.depth = false;
        const std::wstring rangeStatus = app.BuildStatusText();
        CHECK(rangeStatus.find(L"Range 00:00:01:15\u201300:00:03:00") != std::wstring::npos);
        CHECK(rangeStatus.find(L"NR 1.25/struct 1.00/tone 1.00/mv=1,depth=0") != std::wstring::npos);
        // Seeks stay inside the cached range; Stop returns to its first frame.
        CHECK(std::abs(app.ClampSeek(0.0) - 1.5) < 1e-9);
        CHECK(std::abs(app.ClampSeek(9.0) - (3.0 - 1.0 / 30.0)) < 1e-9);
        CHECK(std::abs(app.ClampSeek(2.0) - 2.0) < 1e-9);
        app.m_cachedRange = {}; app.m_cachedSettings = {}; app.m_cachedGuides = {};
        CHECK(app.BuildStatusText().find(L"Range ") == std::wstring::npos);
        CHECK(app.BuildStatusText().find(L"NR 1.00/struct 1.00/tone 1.00 ") == std::wstring::npos);
        // Markers belong to the loaded source.
        app.HandleCommand(IDM_MARK_OUT);
        app.Unload();
        CHECK(!app.m_markers.out100ns.has_value());
        app.m_loaded = true; app.m_cachedPlayback = true; app.m_havePresentedPair = true;
        app.m_renderer = MakeD3D12Renderer();
    }

    // The compare bar and the press-and-hold A/B, with a pair resident and the neural
    // view on (CheckComparisonAvailability sets that up).
    static void CheckCompareBarAndPeek(PlayerApp& app)
    {
        const ComparisonSettings entry = app.m_comparison;
        const int entryZoom = app.m_zoomStep;
        app.m_zoomStep = 0;
        // Blend has no row of its own; a stray command gets the neural view it became.
        app.m_comparison.mode = ComparisonMode::Wipe;
        app.HandleCommand(IDM_COMPARE_BLEND);
        CHECK(app.m_comparison.mode == ComparisonMode::Neural);
        app.HandleCommand(IDM_COMPARE_ORIGINAL);
        CHECK(app.m_comparison.mode == ComparisonMode::Original);
        CHECK(app.m_renderer->GetComparison().mode == ComparisonMode::Original);
        // C walks the bar's modes and wraps; Shift+C walks back.
        app.HandleCommand(IDM_COMPARE_NEXT_MODE);
        CHECK(app.m_comparison.mode == ComparisonMode::SplitVertical);
        app.HandleCommand(IDM_COMPARE_NEXT_MODE); app.HandleCommand(IDM_COMPARE_NEXT_MODE);
        CHECK(app.m_comparison.mode == ComparisonMode::Difference);
        app.HandleCommand(IDM_COMPARE_NEXT_MODE);
        CHECK(app.m_comparison.mode == ComparisonMode::SideBySide);
        app.HandleCommand(IDM_COMPARE_NEXT_MODE);
        CHECK(app.m_comparison.mode == ComparisonMode::Quad);
        app.HandleCommand(IDM_COMPARE_NEXT_MODE);
        CHECK(app.m_comparison.mode == ComparisonMode::Neural);
        app.HandleCommand(IDM_COMPARE_PREVIOUS_MODE);
        CHECK(app.m_comparison.mode == ComparisonMode::Quad);
        app.HandleCommand(IDM_COMPARE_DIFFERENCE);
        // The Difference view: its gain walks the ladder and its tag names the gain and
        // the channels, so the atlas is redrawn when either changes.
        CHECK(app.m_renderer->GetComparison().mode == ComparisonMode::Difference);
        CHECK_EQ(4.0f, app.m_comparison.differenceGain);
        const uint64_t revision = app.m_labelTextRevision;
        app.HandleCommand(IDM_COMPARE_DIFFERENCE_MORE);
        CHECK_EQ(8.0f, app.m_renderer->GetComparison().differenceGain);
        CHECK(app.m_labelTextRevision != revision);
        CHECK(app.LabelAtlasTexts()[2].find(L"\u00d78") != std::wstring::npos);
        CHECK(app.LabelAtlasTexts()[2].find(L"LUMA") != std::wstring::npos);
        app.HandleCommand(IDM_COMPARE_DIFFERENCE_LUMA);
        CHECK(!app.m_renderer->GetComparison().differenceLuma);
        CHECK(app.LabelAtlasTexts()[2].find(L"COLOR") != std::wstring::npos);
        CHECK((GetMenuState(GetMenu(app.m_hwnd), IDM_COMPARE_DIFFERENCE, MF_BYCOMMAND) & MF_CHECKED) != 0);
        CHECK(app.BuildLabelAtlas(96).widths[2] > app.BuildLabelAtlas(96).widths[1]);
        app.HandleCommand(IDM_COMPARE_DIFFERENCE_LUMA); app.HandleCommand(IDM_COMPARE_DIFFERENCE_LESS);
        app.HandleCommand(IDM_COMPARE_PREVIOUS_MODE);
        CHECK(app.m_comparison.mode == ComparisonMode::Wipe);
        app.HandleCommand(IDM_COMPARE_SWAP);
        CHECK(app.m_comparison.swap && app.m_renderer->GetComparison().swap);
        CHECK((GetMenuState(GetMenu(app.m_hwnd), IDM_COMPARE_SWAP, MF_BYCOMMAND) & MF_CHECKED) != 0);
        app.HandleCommand(IDM_COMPARE_SWAP);
        CHECK(!app.m_comparison.swap);

        // Holding still on the picture shows the original until release, whatever the
        // mode, and leaves the mode alone.
        app.m_comparison.mode = ComparisonMode::Neural; app.ApplyComparison(false);
        app.RenderMouseDown(app.m_renderWnd, MAKELPARAM(20, 20));
        CHECK(app.EffectiveComparison().mode == ComparisonMode::Neural);
        app.PeekHoldElapsed();
        CHECK(app.m_peekOriginal);
        CHECK(app.EffectiveComparison().mode == ComparisonMode::Original);
        CHECK(app.m_renderer->GetComparison().mode == ComparisonMode::Original);
        CHECK(app.m_comparison.mode == ComparisonMode::Neural);
        app.RenderMouseUp(app.m_renderWnd);
        CHECK(!app.m_peekOriginal);
        CHECK(app.m_renderer->GetComparison().mode == ComparisonMode::Neural);
        // In split, a click still moves the divider; a drag past the slop before the
        // hold is a drag, and the late timer does not turn it into a peek.
        app.m_comparison.mode = ComparisonMode::SplitVertical; app.m_comparison.splitX = 0.5f; app.ApplyComparison(false);
        // A render window of a known size for the divider to be measured against; this
        // fixture has none of its own.
        const HWND fixtureRender = app.m_renderWnd;
        app.m_renderWnd = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD, 0, 0, 400, 200, app.m_hwnd, nullptr, nullptr, nullptr);
        RECT client{}; GetClientRect(app.m_renderWnd, &client);
        CHECK_EQ(400L, client.right);
        app.RenderMouseDown(app.m_renderWnd, MAKELPARAM(client.right / 4, client.bottom / 2));
        CHECK(std::abs(app.m_comparison.splitX - 0.25f) < 0.02f);
        app.RenderMouseMove(app.m_renderWnd, MAKELPARAM(client.right * 3 / 4, client.bottom / 2));
        CHECK(std::abs(app.m_comparison.splitX - 0.75f) < 0.02f);
        app.PeekHoldElapsed();
        CHECK(!app.m_peekOriginal);
        app.RenderMouseUp(app.m_renderWnd);
        // Losing capture mid-peek ends the peek.
        app.RenderMouseDown(app.m_renderWnd, MAKELPARAM(10, 10));
        app.PeekHoldElapsed();
        CHECK(app.m_peekOriginal);
        app.RenderCaptureLost();
        CHECK(!app.m_peekOriginal);

        // The loupe follows the pointer over the picture and needs the original.
        app.m_comparison.mode = ComparisonMode::Neural; app.m_comparison.strength = 1.0f;
        app.HandleCommand(IDM_COMPARE_LOUPE);
        CHECK(app.m_loupe);
        app.RenderMouseMove(app.m_renderWnd, MAKELPARAM(200, 150));
        ComparisonSettings shown = app.EffectiveComparison();
        CHECK(shown.loupe);
        CHECK(std::abs(shown.loupeU - 200.5f / 400.0f) < 1e-4f);
        CHECK(shown.loupeRadius > 0.0f && shown.loupeLeftY < 150.0f);
        CHECK(ComparisonReadsReference(shown));
        CHECK(app.m_renderer->GetComparison().loupe);
        app.RenderMouseLeft();
        CHECK(!app.EffectiveComparison().loupe);
        app.HandleCommand(IDM_COMPARE_LOUPE);
        CHECK(!app.m_loupe);
        // Ctrl+wheel over the picture zooms at the pointer, and the point under it stays.
        // Straight into WndProc: this fixture's window class does not route to it.
        RECT screen{}; GetWindowRect(app.m_renderWnd, &screen);
        const int volumeBefore = int(app.m_volume * 100.0f);
        app.WndProc(app.m_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA), MAKELPARAM(screen.left + 100, screen.top + 50));
        CHECK(app.m_zoomStep > 0);
        CHECK_EQ(volumeBefore, int(app.m_volume * 100.0f));
        const ComparisonSettings zoomed = app.EffectiveComparison();
        CHECK(std::abs(compare_zoom::ImageAt(zoomed.zoomCenterX, zoomed.zoomScale, 100.5f / 400.0f) - 100.5f / 400.0f) < 1e-4f);
        // Middle-drag pans the zoomed picture with the pointer.
        const float centreBefore = app.m_comparison.zoomCenterX;
        app.RenderMiddleDown(app.m_renderWnd, MAKELPARAM(100, 50));
        app.RenderMouseMove(app.m_renderWnd, MAKELPARAM(140, 50));
        app.RenderMiddleUp(app.m_renderWnd);
        CHECK(app.m_comparison.zoomCenterX < centreBefore);
        app.WndProc(app.m_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WORD(-WHEEL_DELTA)), MAKELPARAM(screen.left + 100, screen.top + 50));
        app.WndProc(app.m_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WORD(-WHEEL_DELTA)), MAKELPARAM(screen.left + 100, screen.top + 50));
        CHECK_EQ(0, app.m_zoomStep);
        // 2x2: the zoom ladder measures the pane, the loupe finds the point in the pane
        // under the pointer, and the fourth pane's Mix is named in its tag.
        app.HandleCommand(IDM_COMPARE_QUAD);
        CHECK(app.m_renderer->GetComparison().mode == ComparisonMode::Quad);
        CHECK_EQ(200, app.ZoomViewWidth());
        app.HandleCommand(IDM_COMPARE_LOUPE);
        app.RenderMouseMove(app.m_renderWnd, MAKELPARAM(299, 149));
        shown = app.EffectiveComparison();
        CHECK(shown.loupe);
        CHECK(std::abs(shown.loupeU - 0.5f) < 0.01f && std::abs(shown.loupeV - 0.5f) < 0.01f);
        app.HandleCommand(IDM_COMPARE_LOUPE);
        const uint64_t before = app.m_labelTextRevision;
        app.HandleCommand(IDM_COMPARE_SECOND_MIX_FIRST + 2);
        CHECK_EQ(0.75f, app.m_renderer->GetComparison().secondMix);
        CHECK(app.m_labelTextRevision != before);
        CHECK(app.LabelAtlasTexts()[3].find(L"75%") != std::wstring::npos);
        CHECK((GetMenuState(GetMenu(app.m_hwnd), IDM_COMPARE_SECOND_MIX_FIRST + 2, MF_BYCOMMAND) & MF_CHECKED) != 0);
        app.HandleCommand(IDM_COMPARE_SECOND_MIX_FIRST + 1);
        // Side by side: letterboxed halves, so the bars under the pointer have no loupe.
        app.HandleCommand(IDM_COMPARE_SIDE_BY_SIDE);
        app.HandleCommand(IDM_COMPARE_LOUPE);
        app.RenderMouseMove(app.m_renderWnd, MAKELPARAM(100, 10));
        CHECK(!app.EffectiveComparison().loupe);
        app.RenderMouseMove(app.m_renderWnd, MAKELPARAM(100, 100));
        CHECK(app.EffectiveComparison().loupe);
        app.HandleCommand(IDM_COMPARE_LOUPE);
        app.HandleCommand(IDM_COMPARE_NEURAL);
        CHECK_EQ(400, app.ZoomViewWidth());
        DestroyWindow(app.m_renderWnd); app.m_renderWnd = fixtureRender;

        // View > 1:1 pixels is a third answer beside Fit and Fill, and A leaves it.
        app.HandleCommand(IDM_ASPECT_ONE_TO_ONE);
        CHECK(app.m_onePixel && !app.m_fill);
        CHECK((GetMenuState(GetMenu(app.m_hwnd), IDM_ASPECT_ONE_TO_ONE, MF_BYCOMMAND) & MF_CHECKED) != 0);
        CHECK((GetMenuState(GetMenu(app.m_hwnd), IDM_ASPECT_FIT, MF_BYCOMMAND) & MF_CHECKED) == 0);
        CHECK(app.ButtonContent(ToolbarAction::Aspect).active);
        app.HandleCommand(IDM_ASPECT_FIT);
        CHECK(!app.m_onePixel && !app.m_fill);
        CHECK((GetMenuState(GetMenu(app.m_hwnd), IDM_ASPECT_FIT, MF_BYCOMMAND) & MF_CHECKED) != 0);

        // The bar: shown only where a neural member can exist, above the toolbar.
        const bool configured = app.m_opt.neuralAddonConfigured;
        const int plainHeight = app.ControlHeight();
        app.m_opt.neuralAddonConfigured = true;
        CHECK(app.CompareBarVisible());
        CHECK_EQ(app.ControlHeight(), plainHeight + app.Dip(compare_bar::kBarHeightDip));
        const auto layout = app.CompareBarLayout();
        RECT main{}; GetClientRect(app.m_hwnd, &main);
        CHECK_EQ(layout.bar.top, main.bottom - app.ControlHeight());
        // Clicking a mode segment selects it; clicking the track sets the Mix.
        for (const auto& item : layout.items) {
            if (item.part == compare_bar::Part::Mode && item.index == 2) {
                CHECK(app.CompareBarMouseDown((item.bounds.left + item.bounds.right) / 2, (item.bounds.top + item.bounds.bottom) / 2));
                CHECK(app.m_comparison.mode == app.CompareBarModes()[2]);
            }
        }
        const int quarter = layout.mixTrack.left + (layout.mixTrack.right - layout.mixTrack.left) / 4;
        CHECK(app.CompareBarMouseDown(quarter, (layout.bar.top + layout.bar.bottom) / 2));
        CHECK(app.m_dragMix);
        CHECK(std::abs(app.m_comparison.strength - 0.5f) < 0.051f);
        app.MouseUp(quarter, layout.bar.top + 2);
        CHECK(!app.m_dragMix);
        // A press on the row that hits nothing is still the row's.
        CHECK(app.CompareBarMouseDown(1, layout.bar.top + 1));
        // The tags the compositor draws: four non-empty rows, flag rule opaque at the left
        // edge, plate translucent beside it, nothing past each tag's width.
        const auto atlas = app.BuildLabelAtlas(96);
        CHECK(!atlas.pixels.empty());
        CHECK(atlas.widths[0] > 0 && atlas.widths[1] > 0 && atlas.widths[2] > 0 && atlas.widths[3] > 0);
        CHECK_EQ(size_t(atlas.width) * atlas.height * 4, atlas.pixels.size());
        if (!atlas.pixels.empty() && atlas.widths[1] + 1 < atlas.width) {
            const auto alpha = [&](uint32_t x, uint32_t y) { return atlas.pixels[(size_t(y) * atlas.width + x) * 4 + 3]; };
            CHECK_EQ(255, int(alpha(0, 1)));
            CHECK(alpha(atlas.widths[0] - 1, 1) > 150 && alpha(atlas.widths[0] - 1, 1) < 255);
            CHECK_EQ(0, int(alpha(atlas.width - 1, atlas.rowHeight + 1)));
        }
        app.m_opt.neuralAddonConfigured = configured;
        app.m_comparison = entry; app.m_zoomStep = entryZoom; app.m_comparison.strength = 1.0f; app.ApplyComparison(false);
    }

    // The spatial mask on the Mix: loaded for a source, remembered for it, restored when
    // it comes back, and forgotten on Clear.
    static void CheckComparisonMask(PlayerApp& app)
    {
        const std::wstring path = app.m_path, page = app.m_youtubePageUrl;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const auto image = app.SettingsPath().parent_path() / L"ui-mask.bmp";
        write_half_white_bmp(image, 16, 8);
        app.m_youtubePageUrl.clear();
        app.m_path = L"C:\\videos\\masked.mp4";
        app.SyncMaskToSource();
        CHECK(app.m_maskSource.pixels.empty());
        CHECK(!app.EffectiveComparison().mask);
        CHECK(app.LoadMask(image, 0, false, true));
        CHECK_EQ(16u, app.m_maskFeathered.width);
        CHECK(app.EffectiveComparison().mask);
        CHECK(ComparisonReadsReference(app.EffectiveComparison()));
        wchar_t saved[512]{};
        GetPrivateProfileStringW(L"ComparisonMasks", app.m_maskSourceKey.c_str(), L"", saved, 512, app.SettingsPath().c_str());
        const auto record = compare_mask::Parse(saved);
        CHECK(record.has_value());
        if (record) { CHECK(record->path == image.wstring()); CHECK_EQ(0, record->feather); CHECK(!record->invert); }
        // Feather and invert are remembered too; the feather softens the edge.
        const auto hard = app.m_maskFeathered.pixels;
        app.SetMaskFeather(3);
        CHECK_EQ(16, app.m_maskFeather);
        CHECK(app.m_maskFeathered.pixels != hard);
        app.ToggleMaskInvert();
        CHECK(app.EffectiveComparison().maskInvert);
        // Another source has no mask; coming back restores this one as it was left.
        app.m_path = L"C:\\videos\\other.mp4";
        app.SyncMaskToSource();
        CHECK(app.m_maskSource.pixels.empty());
        CHECK(!app.EffectiveComparison().mask);
        app.m_path = L"C:\\Videos\\MASKED.mp4";
        app.SyncMaskToSource();
        CHECK(!app.m_maskSource.pixels.empty());
        CHECK_EQ(16, app.m_maskFeather);
        CHECK(app.m_maskInvert);
        CHECK((GetMenuState(GetMenu(app.m_hwnd), IDM_COMPARE_MASK_INVERT, MF_BYCOMMAND) & MF_CHECKED) != 0);
        // Clear forgets it for this source.
        app.HandleCommand(IDM_COMPARE_MASK_CLEAR);
        CHECK(app.m_maskSource.pixels.empty());
        GetPrivateProfileStringW(L"ComparisonMasks", app.m_maskSourceKey.c_str(), L"", saved, 512, app.SettingsPath().c_str());
        CHECK(std::wstring(saved).empty());
        // A remembered mask whose file is gone is left alone, and the source plays unmasked.
        CHECK(app.LoadMask(image, 8, false, true));
        std::error_code ignored; std::filesystem::remove(image, ignored);
        app.m_path = L"C:\\videos\\other.mp4"; app.SyncMaskToSource();
        app.m_path = L"C:\\videos\\masked.mp4"; app.SyncMaskToSource();
        CHECK(app.m_maskSource.pixels.empty());
        WritePrivateProfileStringW(L"ComparisonMasks", nullptr, nullptr, app.SettingsPath().c_str());
        app.m_path = path; app.m_youtubePageUrl = page; app.SyncMaskToSource();
        if (SUCCEEDED(com)) CoUninitialize();
    }

    // Save comparison image: the footer under the picture, and what it says. The GPU
    // read-back itself (CaptureComposedView) needs a device and is left to a visual pass.
    static void CheckSavedComparisonComposition(PlayerApp& app)
    {
        CHECK((GetMenuState(GetMenu(app.m_hwnd), IDM_SAVE_COMPARISON_IMAGE, MF_BYCOMMAND) & (MF_GRAYED | MF_DISABLED)) == 0);
        const ComparisonSettings entry = app.m_comparison;
        app.m_comparison.mode = ComparisonMode::SplitVertical; app.m_comparison.splitX = 0.5f; app.m_comparison.swap = true;
        const auto facts = app.ComparisonProvenance();
        CHECK(facts.application.find(L"DLSS 5 Video Player") == 0);
        CHECK(facts.view.find(L"Split 50% swapped") != std::wstring::npos);
        CHECK(facts.view.find(L"Mix 100%") != std::wstring::npos);
        CHECK(facts.settings.rfind(L"sha256:", 0) == 0 && facts.settings.size() == 7 + 16);
        CHECK(!facts.runtime.empty() && facts.saved.size() == 19);
        app.m_comparison = entry;
        // 40x20 of one colour in, the same picture out as BGR, with the footer below it:
        // the ground, and the flag rule at its left edge.
        constexpr uint32_t width = 40, height = 20;
        std::vector<uint8_t> rgba(size_t(width) * height * 4);
        for (size_t at = 0; at < size_t(width) * height; ++at) { rgba[at * 4] = 200; rgba[at * 4 + 1] = 100; rgba[at * 4 + 2] = 50; rgba[at * 4 + 3] = 255; }
        uint32_t total = 0, stride = 0;
        const auto bgr = app.ComposeComparisonImage(rgba, width, height, compare_provenance::FooterLines(facts), total, stride);
        CHECK(!bgr.empty());
        CHECK(total > height);
        CHECK_EQ(size_t(stride) * total, bgr.size());
        if (!bgr.empty() && total > height + 2) {
            CHECK_EQ(50, int(bgr[0])); CHECK_EQ(100, int(bgr[1])); CHECK_EQ(200, int(bgr[2]));
            const size_t rule = size_t(height + 2) * stride;
            CHECK_EQ(26, int(bgr[rule])); CHECK_EQ(255, int(bgr[rule + 2]));
            const size_t ground = size_t(total - 1) * stride + size_t(width - 1) * 3;
            CHECK_EQ(5, int(bgr[ground + 2]));
        }
        CHECK(app.ComposeComparisonImage({}, width, height, {}, total, stride).empty());
    }

    static void CheckComparisonAvailability(PlayerApp& app)
    {
        const HMENU menu = GetMenu(app.m_hwnd);
        const auto grayed = [&](UINT command) {
            return (GetMenuState(menu, command, MF_BYCOMMAND) & (MFS_DISABLED | MFS_GRAYED)) != 0;
        };
        app.m_neuralRequested = true; app.m_comparisonView = ComparisonView::Neural;
        app.SyncFeatureMenuState();
        CHECK(app.ComparisonModesAvailable());
        CHECK(!grayed(IDM_COMPARE_SPLIT) && !grayed(IDM_COMPARE_ZOOM));
        app.HandleCommand(IDM_COMPARE_SPLIT);
        CHECK(app.m_comparison.mode == ComparisonMode::SplitVertical);
        CHECK(app.m_renderer->GetComparison().mode == ComparisonMode::SplitVertical);
        CHECK((GetMenuState(menu, IDM_COMPARE_SPLIT, MF_BYCOMMAND) & MF_CHECKED) != 0);
        // [ and ] step the Mix, which is the strength the renderer composites with.
        app.m_comparison.strength = 0.5f;
        app.HandleCommand(IDM_COMPARE_BLEND_MORE);
        app.HandleCommand(IDM_COMPARE_BLEND_MORE);
        CHECK(std::abs(app.m_comparison.strength - 0.7f) < 0.001f);
        CHECK(std::abs(app.m_renderer->GetComparison().strength - 0.7f) < 0.001f);
        for (int step = 0; step < 12; ++step) app.HandleCommand(IDM_COMPARE_BLEND_LESS);
        CHECK_EQ(app.m_comparison.strength, 0.0f);
        app.m_comparison.strength = 1.0f;
        CheckCompareBarAndPeek(app);
        CheckComparisonMask(app);
        CheckSavedComparisonComposition(app);
        // Z steps along the ladder; with no render window or output to measure it is
        // multiples of Fit, where 1:1 does not magnify and is skipped.
        app.HandleCommand(IDM_COMPARE_ZOOM);
        CHECK_EQ(app.m_zoomStep, 2);
        CHECK_EQ(app.EffectiveComparison().zoomScale, 2.0f);
        CHECK(!grayed(IDM_COMPARE_ZOOM_OUT));
        // The original view has no pair member to compare against: items gray
        // out and presentation is forced to Neural while the choice is kept.
        app.m_neuralRequested = false; app.m_comparisonView = ComparisonView::Original;
        app.SyncFeatureMenuState();
        CHECK(!app.ComparisonModesAvailable());
        CHECK(grayed(IDM_COMPARE_SPLIT) && grayed(IDM_COMPARE_BLEND_MORE));
        CHECK(!grayed(IDM_COMPARE_ZOOM));
        CHECK(app.EffectiveComparison().mode == ComparisonMode::Neural);
        CHECK(app.m_comparison.mode == ComparisonMode::SplitVertical);
        app.HandleCommand(IDM_COMPARE_WIPE);
        CHECK(app.m_comparison.mode == ComparisonMode::SplitVertical);
        app.HandleCommand(IDM_COMPARE_ZOOM_FIT);
        CHECK_EQ(app.m_zoomStep, 0);
        CHECK_EQ(app.EffectiveComparison().zoomScale, 1.0f);
        app.m_neuralRequested = true; app.m_comparisonView = ComparisonView::Neural;
        app.m_cachedPlayback = false;
        app.SyncFeatureMenuState();
        CHECK(grayed(IDM_COMPARE_SPLIT));
        app.m_cachedPlayback = true;
        app.HandleCommand(IDM_COMPARE_NEURAL);
        CHECK(app.m_comparison.mode == ComparisonMode::Neural);
        app.m_comparison = {};
        app.SyncFeatureMenuState();
    }

    // A press on play while a live session buffers cannot be obeyed yet, only
    // remembered. It used to be remembered invisibly: the control still read
    // "Play", so the obvious second press cancelled the first and the picture
    // never started when the lead arrived. The intent has to be on screen.
    static void CheckLiveBufferingPlayIntent(PlayerApp& app)
    {
        const bool liveSession = app.m_liveSession, liveBuffering = app.m_liveBuffering;
        const bool resume = app.m_liveResumePlaying, playing = app.m_playing, loaded = app.m_loaded;
        app.m_loaded = true; app.m_playing = false;
        app.m_liveSession = true; app.m_liveBuffering = true; app.m_liveResumePlaying = false;

        const auto paused = app.ButtonContent(ToolbarAction::PlayPause);
        CHECK_EQ(std::wstring(L"Play"), paused.label);
        CHECK(!paused.active);
        CHECK(app.LiveSessionStatusText().find(app.T(L"neural.live.will_stay_paused")) != std::wstring::npos);

        app.TogglePause();
        CHECK(app.m_liveResumePlaying);
        CHECK(!app.m_playing);  // still buffering: the press is a promise, not a start
        const auto pending = app.ButtonContent(ToolbarAction::PlayPause);
        CHECK_EQ(std::wstring(L"Pause"), pending.label);
        CHECK(pending.active);
        CHECK(app.LiveSessionStatusText().find(app.T(L"neural.live.will_play")) != std::wstring::npos);

        app.TogglePause();  // a second press is a real cancel, and says so
        CHECK(!app.m_liveResumePlaying);
        CHECK_EQ(std::wstring(L"Play"), app.ButtonContent(ToolbarAction::PlayPause).label);
        CHECK(app.LiveSessionStatusText().find(app.T(L"neural.live.will_stay_paused")) != std::wstring::npos);

        app.m_liveSession = liveSession; app.m_liveBuffering = liveBuffering;
        app.m_liveResumePlaying = resume; app.m_playing = playing; app.m_loaded = loaded;
    }

    // Pressing the neural toggle right after a scrub used to do nothing at all:
    // the request was refused for the seek in flight and dropped, and the log
    // was the only place it showed. It is queued now, the label says so, and a
    // seek that lands somewhere the toggle cannot act drops it explicitly.
    static void CheckNeuralToggleQueuedDuringSeek(PlayerApp& app)
    {
        const bool loaded = app.m_loaded, cached = app.m_cachedPlayback;
        const bool pair = app.m_havePresentedPair, seeking = app.m_seeking;
        app.m_loaded = true; app.m_cachedPlayback = true; app.m_havePresentedPair = true;
        app.m_seeking = true; app.m_neuralToggleDeferred = false;
        CHECK(!app.ToolbarActionEnabled(ToolbarAction::ToggleNeuralRendering));

        app.ToggleNeuralRendering();
        CHECK(app.m_neuralToggleDeferred);
        CHECK(!app.ToolbarActionEnabled(ToolbarAction::ToggleNeuralRendering));

        // The seek landed on something unrenderable: drop the press, do not
        // leave it queued for the next unrelated seek.
        app.m_cachedPlayback = false; app.m_havePresentedPair = false; app.m_loaded = false;
        app.SetSeeking(false);
        CHECK(!app.m_neuralToggleDeferred);
        CHECK(app.ButtonContent(ToolbarAction::ToggleNeuralRendering).label.find(L"Queued") == std::wstring::npos);

        app.m_loaded = loaded; app.m_cachedPlayback = cached;
        app.m_havePresentedPair = pair; app.m_seeking = seeking;
    }

    // A session fills its range one hole at a time and every job publishes a
    // cache entry of its own hole, while "Save converted video" writes the entry
    // under the session's whole range. An entry that is one hole must not be on
    // offer: a session that rendered [30,60) and then [0,30) exported its 30 s
    // tail labelled as the film. Only a job that rendered the whole range does.
    static void CheckLiveExportEntry(PlayerApp& app)
    {
        const bool liveSession = app.m_liveSession, cached = app.m_cachedPlayback, requested = app.m_neuralRequested;
        app.m_liveSession = true; app.m_liveAttached = true; app.m_cachedPlayback = true;
        app.m_liveRange = NeuralRenderRange{0, 600000000}; app.m_liveDirectory.clear();
        app.m_liveRenderFailures = 0; app.m_liveTargetRevision = 0;
        const auto entry = app.SettingsPath().parent_path() / L"live-export-entry.mkv";
        {std::ofstream file(entry, std::ios::binary); file << "entry";}
        const auto exportEnabled = [&] {
            return (GetMenuState(GetMenu(app.m_hwnd), IDM_EXPORT_CACHED_VIDEO, MF_BYCOMMAND) & (MF_GRAYED | MF_DISABLED)) == 0;
        };
        const auto segment = [&](uint64_t run, int64_t start, int64_t end) {
            NeuralSegment part{}; part.path = entry; part.runId = run;
            part.firstTimestamp100ns = start; part.end100ns = end; part.frameCount = uint64_t((end - start) / 333333);
            app.m_liveSegments->Append(part);
        };
        NeuralJobCompletion completion{}; completion.result.ok = true; completion.neuralPath = entry;

        app.m_liveSegments = std::make_shared<NeuralSegmentIndex>();
        segment(1, 300000000, 600000000);
        completion.range = NeuralRenderRange{300000000, 600000000};
        app.CompleteLiveNeuralJob(completion);
        CHECK(app.m_liveSession);
        CHECK(app.m_neuralPath.empty());
        CHECK(!exportEnabled());
        // The second job fills the opening: the range is rendered end to end
        // now, but this entry is still one hole of it.
        segment(2, 0, 300000000);
        completion.range = NeuralRenderRange{0, 300000000};
        app.CompleteLiveNeuralJob(completion);
        CHECK(app.m_neuralPath.empty());
        CHECK(!exportEnabled());

        app.m_liveSegments = std::make_shared<NeuralSegmentIndex>();
        segment(3, 0, 600000000);
        completion.range = NeuralRenderRange{0, 600000000};
        app.CompleteLiveNeuralJob(completion);
        CHECK_EQ(app.m_neuralPath, entry);
        CHECK(exportEnabled());

        app.m_liveSegments.reset(); app.m_liveRange = {}; app.m_liveAttached = false;
        app.m_liveSession = liveSession; app.m_cachedPlayback = cached; app.m_neuralRequested = requested;
        app.m_neuralPath.clear(); app.m_cachedReceiptPath.clear(); app.m_cachedSettings = {}; app.m_cachedGuides = {};
        app.m_neuralNotice.clear(); app.SyncFeatureMenuState();
        std::filesystem::remove(entry);
    }

    // A settings preview that is dropped - Stop, the cancel button, a file
    // change - used to leave m_previewJob set and the buffering panel up: no
    // later preview could start and the panel never came down. The job is a
    // real lifecycle with a worker, cancelled through the real path.
    static void CheckDroppedPreviewJob(PlayerApp& app, const WNDCLASSW& windowClass)
    {
        const bool loaded = app.m_loaded, cached = app.m_cachedPlayback, pair = app.m_havePresentedPair, seeking = app.m_seeking;
        app.m_renderWnd = CreateWindowExW(0, windowClass.lpszClassName, nullptr, WS_CHILD, 0, 0, 320, 180,
                                          app.m_hwnd, nullptr, windowClass.hInstance, nullptr);
        CHECK(app.m_renderWnd != nullptr);
        const auto startPreview = [&] {
            app.m_previewJob = true; app.m_previewQueued = true;
            app.ShowBufferOverlay();
            app.m_neuralLifecycle.Begin();
            app.m_neuralWorker = std::jthread([] {});
            CHECK(app.NeuralJobActive());
            CHECK(app.JobBehindPlayback());
            CHECK(app.m_bufferWnd != nullptr && IsWindowVisible(app.m_bufferWnd));
        };
        startPreview();
        app.CancelNeuralJob(false);
        CHECK(!app.NeuralJobActive());
        CHECK(!app.m_previewJob && !app.m_previewQueued);
        CHECK(!app.JobBehindPlayback());
        CHECK(!IsWindowVisible(app.m_bufferWnd));

        // Unloading the file drops a running preview the same way.
        startPreview();
        app.Unload();
        CHECK(!app.NeuralJobActive());
        CHECK(!app.m_previewJob && !app.m_previewQueued);
        CHECK(!IsWindowVisible(app.m_bufferWnd));

        DestroyWindow(app.m_bufferWnd); app.m_bufferWnd = nullptr;
        DestroyWindow(app.m_renderWnd); app.m_renderWnd = nullptr;
        app.m_loaded = loaded; app.m_cachedPlayback = cached; app.m_havePresentedPair = pair; app.m_seeking = seeking;
        app.m_renderer = MakeD3D12Renderer();
    }

    // The toggle queued behind a seek belongs to the file the seek was in. It
    // used to survive Unload and fire on the next file's first seek.
    static void CheckUnloadDropsDeferredToggle(PlayerApp& app)
    {
        const bool loaded = app.m_loaded, cached = app.m_cachedPlayback, pair = app.m_havePresentedPair, seeking = app.m_seeking;
        app.m_loaded = true; app.m_cachedPlayback = true; app.m_havePresentedPair = true; app.m_seeking = true;
        app.ToggleNeuralRendering();
        CHECK(app.m_neuralToggleDeferred);
        app.Unload();
        CHECK(!app.m_neuralToggleDeferred);
        app.m_loaded = loaded; app.m_cachedPlayback = cached; app.m_havePresentedPair = pair; app.m_seeking = seeking;
        app.m_renderer = MakeD3D12Renderer();
    }

    // A live job whose segment directory cannot be created returned after the
    // lifecycle had already marked a job as running: the spinner stayed on and
    // every source and render action stayed greyed out for the rest of the
    // file, with nothing to say why.
    static void CheckLiveJobDirectoryFailure(PlayerApp& app)
    {
        // A regular file where the session directory should be: no job
        // subdirectory can be created under it.
        const auto blocker = app.SettingsPath().parent_path() / L"live-directory-blocker";
        {std::ofstream file(blocker, std::ios::binary); file << "not a directory";}
        app.m_liveSegments = std::make_shared<NeuralSegmentIndex>();
        app.m_liveDirectory = blocker;
        app.m_neuralNotice.clear();
        CHECK(!app.StartNeuralJob(L"C:\\missing\\source.mp4", {}, L"Blocked", {}, MediaSourceKind::LocalFile,
                                  YouTubeSourceQuality::Auto, {}, 0.0, NeuralRenderRange{0, 10000000}, false, NeuralJobKind::Live));
        CHECK(!app.NeuralJobActive());
        CHECK(!app.m_neuralWorker.joinable());
        CHECK(!app.m_neuralNotice.empty());
        CheckSourceMenus(app, true);
        app.m_liveSegments.reset(); app.m_liveDirectory.clear(); app.m_neuralNotice.clear();
        std::filesystem::remove(blocker);
    }

    // Two jobs that end without adding coverage stop the session filling holes,
    // and a session that can no longer fill the hole the playhead is in hands
    // the original back with a notice rather than sitting behind the buffering
    // panel for good. Driven through the completion handler and the tick.
    static void CheckLiveRenderFailureLimit(PlayerApp& app)
    {
        const bool loaded = app.m_loaded, cached = app.m_cachedPlayback, requested = app.m_neuralRequested, seeking = app.m_seeking;
        app.m_loaded = true; app.m_playing = false; app.m_currentSec = 0.0;
        app.m_seeking = false; app.m_seekPending = false; app.m_dragSeek = false; app.m_lastSeekTick = 0;
        app.m_liveSession = true; app.m_liveAttached = false; app.m_liveBuffering = false; app.m_cachedPlayback = false;
        app.m_liveRange = NeuralRenderRange{0, 600000000}; app.m_liveTarget = {}; app.m_liveDirectory.clear();
        app.m_liveSegments = std::make_shared<NeuralSegmentIndex>();
        NeuralSegment tail{}; tail.path = L"tail.mkv"; tail.runId = 1;
        tail.firstTimestamp100ns = 300000000; tail.end100ns = 600000000; tail.frameCount = 900;
        app.m_liveSegments->Append(tail);
        // The job started from this coverage; ending with it unchanged is a job
        // that rendered nothing, however it reports itself.
        app.m_liveTargetRevision = app.m_liveSegments->Revision();
        app.m_liveRenderFailures = 0; app.m_neuralNotice.clear();
        NeuralJobCompletion fruitless{}; fruitless.result.ok = true;
        for (int strike = 1; strike <= PlayerApp::kLiveRenderFailureLimit; ++strike) {
            app.CompleteLiveNeuralJob(fruitless);
            CHECK(app.m_liveSession);
            app.UpdateLiveSession();
            CHECK_EQ(strike < PlayerApp::kLiveRenderFailureLimit, app.m_liveSession);
        }
        CHECK(!app.m_neuralNotice.empty());
        CHECK(!app.m_liveBuffering);
        app.DropRetainedLiveSegments();
        app.m_liveRenderFailures = 0; app.m_neuralNotice.clear(); app.m_liveRange = {};
        app.m_loaded = loaded; app.m_cachedPlayback = cached; app.m_neuralRequested = requested; app.m_seeking = seeking;
    }

    // A live pair that fell out of sync used to put a modal up from inside
    // Tick: it pumped the job's completion, which tore the session and its
    // renderer down beneath the caller, and the box came back on every Play.
    // The session ends, the original takes the same frame back with the play
    // state the session had, and the reason goes to the status bar. The pair
    // is the real SynchronizedPlayback over sources whose frames never match,
    // so its own resync guard is what reports the desync.
    static void CheckLiveOutOfSyncHandsBack(PlayerApp& app)
    {
        const bool loaded = app.m_loaded, cached = app.m_cachedPlayback, requested = app.m_neuralRequested, seeking = app.m_seeking;
        NumberedFrameSource original(0);
        app.m_synchronizedPlayback = SynchronizedPlayback(original, [] {
            return std::unique_ptr<ISynchronizedFrameSource>(std::make_unique<NumberedFrameSource>(1'000'000));
        });
        auto segments = std::make_shared<NeuralSegmentIndex>();
        NeuralSegment part{}; part.path = L"segment.mkv"; part.runId = 1;
        part.frameCount = 6000; part.end100ns = int64_t(part.frameCount) * 333333;
        segments->Append(part);
        CHECK(app.m_synchronizedPlayback.OpenLive(L"original.mkv", segments, SynchronizedRange{}, {},
                                                  VideoDecoder::KnownMedia{1, 1, 30.0, 3600.0, {}}));
        app.m_loaded = true; app.m_seeking = false; app.m_seekPending = false; app.m_sourceKind = MediaSourceKind::LocalFile;
        app.m_liveSession = true; app.m_liveAttached = true; app.m_liveBuffering = false; app.m_liveResumePlaying = false;
        app.m_liveSegments = segments; app.m_liveDirectory.clear(); app.m_liveRange = NeuralRenderRange{0, part.end100ns};
        app.m_cachedPlayback = true; app.m_neuralRequested = true; app.m_haveNext = false; app.m_playing = true;
        app.m_neuralNotice.clear();
        const int boxes = messageBoxes;

        CHECK(!app.ReadNextCachedFrame());
        CHECK_EQ(boxes, messageBoxes);
        CHECK(!app.m_liveSession);
        CHECK(!app.m_liveAttached);
        CHECK(!app.m_cachedPlayback);
        CHECK(!app.m_neuralNotice.empty());
        // The original takes the frame back through a seek that carries the
        // play state the session had.
        CHECK(app.m_seekPending);
        CHECK(app.m_seekResumePlaying);

        app.m_synchronizedPlayback = SynchronizedPlayback{};
        app.m_seekPending = false; app.m_playing = false; app.m_neuralNotice.clear(); app.m_liveSegments.reset(); app.m_liveRange = {};
        app.m_loaded = loaded; app.m_cachedPlayback = cached; app.m_neuralRequested = requested; app.m_seeking = seeking;
        app.SyncFeatureMenuState();
    }

    // A pair that never assembles is bounded at three seconds. That bound reads
    // a wall clock, and only Tick's playing branch reads pairs at all, so a
    // pause between one NotReady and the next used to run the clock with no
    // reads under it: three seconds paused on a segment boundary, and then the
    // first ordinary warm-up read on resume measured as a wedge and handed the
    // session back. The window covers a CONTIGUOUS run of reads now.
    // The comparison reference is a BGRA-only texture, so an NV12 source has to
    // come back through one CPU inverse. Playback admits NV12 only for BT.709
    // limited (VideoDecoder's playbackConvertible gate), which is why exactly
    // one set of coefficients is written and why it is worth pinning against
    // hand-computed values: a wrong matrix here is not a crash, it is a
    // side-by-side whose left half is quietly the wrong colour.
    static void CheckNv12ReferenceInverseIsBt709Limited()
    {
        constexpr uint32_t w = 2, h = 2;
        // Y plane then one interleaved UV pair, which both chroma-subsampled
        // columns and both rows share at this size.
        const auto convert = [&](uint8_t luma, uint8_t blueDiff, uint8_t redDiff) {
            std::vector<uint8_t> nv12(size_t(w) * h + size_t(w) * h / 2u, 0);
            for (size_t i = 0; i < size_t(w) * h; ++i) nv12[i] = luma;
            nv12[size_t(w) * h + 0] = blueDiff;
            nv12[size_t(w) * h + 1] = redDiff;
            std::vector<uint8_t> bgra;
            Nv12ToBgraBt709Limited(nv12.data(), w, h, bgra);
            CHECK_EQ(size_t(w) * h * 4u, bgra.size());
            return bgra;
        };
        // Not named `near`: windows.h still defines that as a macro, the same
        // way it defines `far`.
        const auto within = [](uint8_t actual, int expected) {
            return std::abs(int(actual) - expected) <= 3;
        };

        // Limited-range black and white sit at 16 and 235, not 0 and 255. Taking
        // them for full range is the classic washed-out/crushed failure.
        const auto black = convert(16, 128, 128);
        CHECK(within(black[0], 0) && within(black[1], 0) && within(black[2], 0));
        CHECK_EQ(uint8_t{255}, black[3]);
        const auto white = convert(235, 128, 128);
        CHECK(within(white[0], 255) && within(white[1], 255) && within(white[2], 255));
        // Mid grey: (126-16)/219 is 0.502 of the way up.
        const auto grey = convert(126, 128, 128);
        CHECK(within(grey[0], 128) && within(grey[1], 128) && within(grey[2], 128));

        // Pure Rec.709 red, forward-computed: Y=16+0.2126*219=63,
        // Cb=128-0.1146*224=102, Cr=128+0.5*224=240. Under BT.601 coefficients
        // the same triple decodes visibly greener, which is the whole point of
        // pinning it.
        const auto red = convert(63, 102, 240);
        CHECK(within(red[2], 255));  // R
        CHECK(within(red[1], 0));    // G
        CHECK(within(red[0], 0));    // B

        // Odd geometry has no half-resolution chroma plane and is refused
        // rather than read past the end.
        std::vector<uint8_t> scratch{1, 2, 3};
        Nv12ToBgraBt709Limited(nullptr, w, h, scratch);
        CHECK_EQ(size_t{3}, scratch.size());
        std::vector<uint8_t> odd(64, 0), out{9};
        Nv12ToBgraBt709Limited(odd.data(), 3, 3, out);
        CHECK_EQ(size_t{1}, out.size());
    }

    static void CheckPairStallBoundIgnoresAPause(PlayerApp& app)
    {
        const bool loaded = app.m_loaded, cached = app.m_cachedPlayback, requested = app.m_neuralRequested, seeking = app.m_seeking;
        NumberedFrameSource original(0);
        app.m_synchronizedPlayback = SynchronizedPlayback(original, [] {
            return std::unique_ptr<ISynchronizedFrameSource>(std::make_unique<NeverReadyFrameSource>());
        });
        auto segments = std::make_shared<NeuralSegmentIndex>();
        NeuralSegment part{}; part.path = L"segment.mkv"; part.runId = 1;
        part.frameCount = 6000; part.end100ns = int64_t(part.frameCount) * 333333;
        segments->Append(part);
        CHECK(app.m_synchronizedPlayback.OpenLive(L"original.mkv", segments, SynchronizedRange{}, {},
                                                  VideoDecoder::KnownMedia{1, 1, 30.0, 3600.0, {}}));
        app.m_loaded = true; app.m_seeking = false; app.m_seekPending = false;
        app.m_sourceKind = MediaSourceKind::LocalFile;
        app.m_liveSession = true; app.m_liveAttached = true; app.m_liveBuffering = false;
        app.m_liveResumePlaying = false; app.m_liveSegments = segments; app.m_liveDirectory.clear();
        app.m_liveRange = NeuralRenderRange{0, part.end100ns};
        app.m_cachedPlayback = true; app.m_neuralRequested = true; app.m_haveNext = false; app.m_playing = true;
        app.m_neuralNotice.clear(); app.m_pairStall = {}; app.m_pairStallRead = {};
        const auto backdate = [](Clock::time_point& point, double seconds) {
            point -= std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
        };

        // One NotReady starts the wait and nothing else happens.
        CHECK(!app.ReadNextCachedFrame());
        CHECK(app.m_pairStall != Clock::time_point{});
        CHECK(app.m_liveAttached);

        // Four seconds later, with four seconds since the last READ: that is a
        // pause across the wait, not a wait. The session survives it, and the
        // second read starts a wait of its own.
        backdate(app.m_pairStall, 4.0);
        backdate(app.m_pairStallRead, 4.0);
        CHECK(!app.ReadNextCachedFrame());
        CHECK(app.m_liveSession);
        CHECK(app.m_liveAttached);
        CHECK(app.m_cachedPlayback);
        CHECK(!app.m_seekPending);

        // The same four seconds with the reads contiguous IS the wedge: the
        // attachment goes and the original takes the picture back through a
        // seek carrying the play state. The session itself stays, to re-attach
        // when its coverage reaches the playhead again.
        backdate(app.m_pairStall, 4.0);
        CHECK(!app.ReadNextCachedFrame());
        CHECK(!app.m_liveAttached);
        CHECK(!app.m_cachedPlayback);
        CHECK(app.m_seekPending);
        CHECK(app.m_seekResumePlaying);

        app.m_synchronizedPlayback = SynchronizedPlayback{};
        app.m_liveSession = false; app.m_seekPending = false; app.m_playing = false;
        app.m_neuralNotice.clear(); app.m_liveSegments.reset(); app.m_liveRange = {};
        app.m_pairStall = {}; app.m_pairStallRead = {};
        app.m_loaded = loaded; app.m_cachedPlayback = cached; app.m_neuralRequested = requested; app.m_seeking = seeking;
        app.SyncFeatureMenuState();
    }

    // A render costs minutes of GPU time and an acquired copy costs a download;
    // both are keyed by content and settings, so both are reusable in every
    // later session that opens that video. They used to be deleted by the
    // RECENT MENU rolling over: the list holds five videos, `Remember` reported
    // the sixth pushing the first off the end, and `PruneRecentCache` turned
    // that report into RemoveSource/RemoveRender. Opening a sixth video threw
    // the first one's work away, and revisiting it did all of it again.
    // Nothing evicts on the list's behalf now; "Clear neural cache" is the only
    // bound, and it reports what it is about to delete.
    static void CheckRecentRolloverKeepsTheCache(PlayerApp& app)
    {
        const auto root = app.SettingsPath().parent_path() / L"recent-rollover-cache";
        const auto recentFile = app.SettingsPath().parent_path() / L"recent-rollover.dat";
        std::filesystem::remove_all(root);
        std::filesystem::remove(recentFile);
        const auto cacheRoot = app.m_cacheRoot;
        app.m_cacheRoot = root;
        app.m_recent = std::make_unique<RecentMediaHistory>(recentFile);

        // The copy the first video leaves behind, published for real so the
        // assertion below is a cache lookup rather than a file test.
        NeuralCacheManager cache(root);
        CHECK(cache.Valid());
        const std::string firstKey(64, 'a');
        const auto staging = cache.BeginSourceStaging(firstKey);
        CHECK(staging.has_value());
        if (staging) {
            { std::ofstream payload(*staging / L"source.mkv", std::ios::binary); payload << "rollover fixture"; }
            NeuralCacheManifest manifest{};
            manifest.encoder = "source-complete-v5-highest-bitrate";
            manifest.width = 1920; manifest.height = 1080;
            manifest.frameCount = 1; manifest.duration100ns = 333333;
            CHECK(cache.PromoteSource(firstKey, *staging, manifest));
        }
        CHECK(cache.LookupSource(firstKey).has_value());

        // Six streams, which is one more than the list holds. A LOCAL entry
        // cannot carry a source key at all - Normalize refuses one - so these
        // are the case where an acquired copy is at stake beside the render.
        const char ids[] = {'a', 'b', 'c', 'd', 'e', 'f'};
        for (size_t index = 0; index < sizeof(ids); ++index) {
            NeuralJobCompletion entry{};
            entry.sourceKind = MediaSourceKind::YouTube;
            entry.pageUrl = L"https://www.youtube.com/watch?v=" + std::wstring(11, wchar_t(ids[index]));
            entry.displayTitle = L"Rollover " + std::to_wstring(index);
            entry.sourceQuality = YouTubeSourceQuality::P1080;
            entry.sourceKey = std::string(64, ids[index]);
            entry.renderKey = std::string(64, char('0' + index));
            app.RecordRecent(entry);
        }
        // The MENU dropped the first one, which is the list doing its job...
        const auto& entries = app.m_recent->Entries();
        CHECK_EQ(entries.size(), size_t{5});
        CHECK(std::none_of(entries.begin(), entries.end(), [&](const RecentMediaEntry& item) {
            return item.sourceKey == firstKey;
        }));
        // ...and the work it named is still there, through the pump that used
        // to be where the deletion happened.
        app.Tick();
        CHECK(cache.LookupSource(firstKey).has_value());

        app.m_recent.reset();
        app.m_cacheRoot = cacheRoot;
        std::filesystem::remove(recentFile);
        std::filesystem::remove_all(root);
    }

    // The forecast question is asked once per source and geometry, not once
    // per session start: a YouTube seek restarts the session and used to put
    // the same question up again. A yes stands until the geometry changes or
    // the file is unloaded; a no is not remembered and says so in the status
    // bar. The decoder describes a tiny AVI so the forecast has a geometry to
    // measure against.
    static void CheckLivePaceConfirmation(PlayerApp& app)
    {
        const bool loaded = app.m_loaded, cached = app.m_cachedPlayback, pair = app.m_havePresentedPair, seeking = app.m_seeking;
        const auto sd = app.SettingsPath().parent_path() / L"pace-64x48.avi";
        const auto hd = app.SettingsPath().parent_path() / L"pace-96x64.avi";
        WriteTinyAvi(sd, 64, 48, 30);
        WriteTinyAvi(hd, 96, 64, 30);
        // The source identity stays fixed while the geometry it is loaded at
        // changes, which is what a YouTube quality reload does.
        const auto load = [&](const std::filesystem::path& file, uint32_t width) {
            app.m_sourceKind = MediaSourceKind::LocalFile; app.m_path = L"C:\\pace\\video.mkv";
            CHECK(app.m_decoder.OpenMetadata(file.wstring()));
            CHECK_EQ(width, app.m_decoder.Width());
        };
        // One measured pace at the smaller geometry, 100 ms a frame against a
        // 30 fps source; the single-sample rule extrapolates the larger one
        // as slower still.
        app.m_renderPace = {}; app.m_renderPace.Record({64, 48, 100.0});
        app.m_livePaceConfirmedKey.clear(); app.m_neuralNotice.clear();
        load(sd, 64);
        const int boxes = messageBoxes;
        messageBoxAnswer = IDNO;
        CHECK(!app.ConfirmLiveSessionPace(30.0));
        CHECK_EQ(boxes + 1, messageBoxes);
        CHECK(!app.m_neuralNotice.empty());
        // A no is not remembered: the next request asks again.
        CHECK(!app.ConfirmLiveSessionPace(30.0));
        CHECK_EQ(boxes + 2, messageBoxes);
        messageBoxAnswer = IDYES;
        app.m_neuralNotice.clear();
        CHECK(app.ConfirmLiveSessionPace(30.0));
        CHECK_EQ(boxes + 3, messageBoxes);
        CHECK(app.m_neuralNotice.empty());
        // A yes stands for this source at this geometry.
        CHECK(app.ConfirmLiveSessionPace(30.0));
        CHECK_EQ(boxes + 3, messageBoxes);
        // The same source at another geometry is another forecast.
        load(hd, 96);
        CHECK(app.ConfirmLiveSessionPace(30.0));
        CHECK_EQ(boxes + 4, messageBoxes);
        // Unloading forgets the answer, even for the same source at the same geometry.
        app.Unload();
        load(hd, 96);
        CHECK(app.ConfirmLiveSessionPace(30.0));
        CHECK_EQ(boxes + 5, messageBoxes);

        messageBoxAnswer = IDOK;
        app.m_decoder.Close(); app.m_path.clear(); app.m_renderPace = {}; app.m_livePaceConfirmedKey.clear(); app.m_neuralNotice.clear();
        std::filesystem::remove(sd); std::filesystem::remove(hd);
        app.m_loaded = loaded; app.m_cachedPlayback = cached; app.m_havePresentedPair = pair; app.m_seeking = seeking;
        app.m_renderer = MakeD3D12Renderer();
    }

    // The running job's source key is offered to playback and to the next
    // render only for the video it was reported for: two 1440p trailers share
    // a geometry, so without the page guard the second video would have been
    // handed the first one's copy. Driven through the real progress handler.
    static void CheckJobSourceKeyGuard(PlayerApp& app)
    {
        const std::wstring first = L"https://youtu.be/first00000A", second = L"https://youtu.be/second0000B";
        const std::string key(64, 'f');
        app.m_recent.reset(); app.m_youtubePageUrl = first;
        const uint64_t generation = app.m_neuralLifecycle.Begin();
        const auto report = [&](const std::wstring& page) {
            auto message = std::make_unique<NeuralProgressMessage>();
            message->generation = generation; message->progress.phase = NeuralRenderPhase::Decoding;
            message->sourcePath = L"C:\\cache\\first.mkv"; message->sourceKey = key; message->pageUrl = page;
            uint64_t token = 0;
            CHECK(app.m_neuralProgressMessages.RegisterAndPost(std::move(message),
                [&](uint64_t registered) { token = registered; return true; }));
            app.CompleteNeuralProgress(token);
        };
        report(first);
        CHECK(app.CachedYouTubeSourceKey() == std::optional<std::string>(key));
        // Another video is loaded now: the key is not its own.
        app.m_youtubePageUrl = second;
        CHECK(!app.CachedYouTubeSourceKey());
        // A report for a video that is not the loaded one is not adopted either.
        report(first);
        CHECK(!app.CachedYouTubeSourceKey());
        app.m_neuralLifecycle.Invalidate(); app.m_neuralProgress = {};
        app.m_jobSourcePath.clear(); app.m_jobSourceKey.clear(); app.m_jobSourcePageUrl.clear(); app.m_youtubePageUrl.clear();
    }

    // Converting a stream reads its acquired copy, and a settled acquisition is
    // visible the moment it settles. Both halves shipped broken.
    //
    // `m_path` on a YouTube source is the signed googlevideo URL the decoder is
    // reading, and the conversion hands a second path to the muxer for the
    // audio, subtitles and chapters. That path used to be `m_path`, which is not
    // a file: MuxVideoWithSourceStreams requires a regular file on both inputs,
    // so a 2560x1440 trailer generated 3132 frames over 63 s and threw all of
    // them away at the last stage with "the encoder refused the specification".
    //
    // The second half is the memo under CachedYouTubeSourceKey. A copy caught
    // mid-promotion - payload moved into place, manifest not yet written -
    // authenticates as missing while its size and write time are already final,
    // so remembering that verdict pinned it for the rest of the session and a
    // finished download went on reading as "needs a local copy".
    static void CheckStreamConversionUsesTheAcquiredCopy(PlayerApp& app)
    {
        std::error_code ec;
        // Per process: a fixed name was removed out from under a concurrent run.
        const auto root = test_support::FixtureTempRoot() /
            (L"dlss5-stream-source-test-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::remove_all(root, ec);
        NeuralCacheManager cache(root);
        CHECK(cache.Valid());
        const std::string key(64, 'a');
        const auto staging = cache.BeginSourceStaging(key);
        CHECK(staging.has_value());
        if (!staging) return;
        { std::ofstream payloadFile(*staging / L"source.mkv", std::ios::binary); payloadFile << "acquired copy"; }
        NeuralCacheManifest manifest{};
        manifest.encoder = kCompleteSourcePolicy;
        manifest.width = 2560; manifest.height = 1440;
        manifest.frameCount = 3133; manifest.duration100ns = 1044900000;
        CHECK(cache.PromoteSource(key, *staging, manifest));
        const auto payload = cache.SourcePayloadPath(key);
        CHECK(payload.has_value());
        if (!payload) return;

        const auto savedCacheRoot = app.m_cacheRoot;
        app.m_cacheRoot = cache.Root();
        app.m_recent = std::make_unique<RecentMediaHistory>(cache.Root() / L"recent-videos.dat");
        NeuralJobCompletion owned{};
        owned.sourceKind = MediaSourceKind::YouTube;
        owned.pageUrl = L"https://www.youtube.com/watch?v=EAEYZDgHNv8";
        owned.displayTitle = L"Mafia";
        owned.sourceQuality = YouTubeSourceQuality::Auto;
        owned.sourceKey = key;
        app.RecordRecent(owned, true);

        app.m_loaded = true;
        app.m_sourceKind = MediaSourceKind::YouTube;
        app.m_youtubeSourceQuality = YouTubeSourceQuality::Auto;
        app.m_youtubePageUrl = owned.pageUrl;
        app.m_cachedSourceFile = false;
        // What playback is actually reading: a signed URL, not a file.
        app.m_path = L"https://rr3---sn-4g5e6nz6.googlevideo.com/videoplayback?expire=1758200000&ei=x";
        app.InvalidateFrameGenerationCopy();
        const uint64_t cacheBuilds = app.m_sourceCacheBuilds;

        CHECK(app.CachedYouTubeSourceKey() == std::optional<std::string>(key));
        CHECK_EQ(payload->wstring(), app.FrameGenerationStreamSource());
        CHECK_EQ(payload->wstring(), app.FrameGenerationInputSource().path);
        CHECK(!app.FrameGenerationInputSource().neural);
        // Once playback has moved onto that copy, the loaded path IS the file and
        // the streams come from it.
        app.m_cachedSourceFile = true;
        app.m_path = payload->wstring();
        CHECK_EQ(payload->wstring(), app.FrameGenerationStreamSource());
        app.m_cachedSourceFile = false;
        app.m_path = L"https://rr3---sn-4g5e6nz6.googlevideo.com/videoplayback?expire=1758200000&ei=x";

        // The mid-promotion shape: the payload is final, the manifest is not
        // there yet, and the acquisition that will write it is still running.
        const auto manifestPath = payload->parent_path() / L"manifest.json";
        const auto hidden = payload->parent_path() / L"manifest.pending";
        std::filesystem::rename(manifestPath, hidden, ec);
        CHECK(!ec);
        app.InvalidateFrameGenerationCopy();
        app.m_prefetchState = std::make_shared<SourcePrefetchState>();
        CHECK(!app.CachedYouTubeSourceKey());
        std::filesystem::rename(hidden, manifestPath, ec);
        CHECK(!ec);
        app.m_prefetchState.reset();
        // Nothing is invalidated here on purpose: this is the call the player
        // makes on its next toolbar paint, and before the fix it answered from a
        // memo keyed on a size and write time that never changed again.
        CHECK(app.CachedYouTubeSourceKey() == std::optional<std::string>(key));
        CHECK_EQ(payload->wstring(), app.FrameGenerationStreamSource());

        // Every question above, and a burst of the per-paint ones below, went
        // through ONE cache manager. Each lookup used to build its own before
        // the memo check: six directory creations, a probe file and a staging
        // sweep per toolbar button per paint.
        for (int paint = 0; paint < 32; ++paint) {
            CHECK(app.CachedYouTubeSourceKey() == std::optional<std::string>(key));
            CHECK(app.AcquiredSourceCopyPath() == std::optional<std::filesystem::path>(*payload));
        }
        CHECK_EQ(cacheBuilds + 1, app.m_sourceCacheBuilds);
        // It is the loaded source's, and goes with it.
        app.Unload();
        CHECK(!app.m_sourceCache);
        app.m_renderer = MakeD3D12Renderer();

        app.m_loaded = false; app.m_sourceKind = MediaSourceKind::LocalFile;
        app.m_path.clear(); app.m_youtubePageUrl.clear(); app.m_recent.reset();
        app.InvalidateFrameGenerationCopy();
        app.m_cacheRoot = savedCacheRoot;
        std::filesystem::remove_all(root, ec);
    }

    // The neural strength dial is presentation state: it must reach the renderer and the
    // frame already on screen through the comparison path, share the adjustments dialog's
    // reset and save, and degrade to 1 when no original is resident to composite against.
    static void CheckNeuralStrengthDial(PlayerApp& app)
    {
        CHECK_EQ(ComparisonSettings{}.strength, 1.0f);
        CHECK_EQ(ComparisonSettings{}.ratioGuard, 2.0f);
        const bool cachedPlayback = app.m_cachedPlayback;
        app.m_comparison = {}; app.m_cachedPlayback = true; app.m_neuralRequested = true;
        app.ShowAdjustments();
        CHECK(app.m_adjustWnd != nullptr);
        if (!app.m_adjustWnd) return;
        const HWND dialog = app.m_adjustWnd;
        const HWND track = GetDlgItem(dialog, IDC_ADJ_NEURAL_STRENGTH);
        CHECK(track != nullptr);
        if (!track) return;
        // The extra row must not push the note or the buttons out of the client area.
        RECT client{};
        CHECK(GetClientRect(dialog, &client) != FALSE);
        CHECK_EQ(int(client.bottom), PlayerApp::kAdjustDesignH);
        CHECK_EQ(int(SendMessageW(track, TBM_GETPOS, 0, 0)), 100);
        SendMessageW(track, TBM_SETPOS, TRUE, 160);
        app.AdjustWndProc(dialog, WM_HSCROLL, 0, 0);
        CHECK(std::abs(app.m_comparison.strength - 1.6f) < 0.001f);
        CHECK(std::abs(app.m_renderer->GetComparison().strength - 1.6f) < 0.001f);
        CHECK_EQ(std::wstring(L"1.60"), ReadText(GetDlgItem(dialog, IDC_ADJ_NEURAL_STRENGTH + 100)));
        // The control cannot ask for a strength outside the range the shader composites
        // over: the trackbar clamps both ends to 0..2.
        SendMessageW(track, TBM_SETPOS, TRUE, 900);
        app.AdjustWndProc(dialog, WM_HSCROLL, 0, 0);
        CHECK_EQ(app.m_comparison.strength, 2.0f);
        SendMessageW(track, TBM_SETPOS, TRUE, -400);
        app.AdjustWndProc(dialog, WM_HSCROLL, 0, 0);
        CHECK_EQ(app.m_comparison.strength, 0.0f);
        // Without a resident original the composite has nothing to mix against, so the
        // effective strength falls back to 1 and the picture stays what it is today.
        app.m_comparison.strength = 0.5f;
        app.m_cachedPlayback = false;
        CHECK_EQ(app.EffectiveComparison().strength, 1.0f);
        app.m_cachedPlayback = true;
        CHECK_EQ(app.EffectiveComparison().strength, 0.5f);
        // The dial carries help text, like every control that needs explaining.
        const auto tipHost = app.m_tipHosts.find(dialog);
        CHECK(tipHost != app.m_tipHosts.end());
        if (tipHost != app.m_tipHosts.end()) {
            wchar_t text[512] = {};
            TTTOOLINFOW info{};
            info.cbSize = TTTOOLINFOW_V2_SIZE;
            info.hwnd = dialog;
            info.uId = reinterpret_cast<UINT_PTR>(track);
            info.lpszText = text;
            SendMessageW(tipHost->second, TTM_GETTEXTW, UINT_PTR{512}, reinterpret_cast<LPARAM>(&info));
            CHECK(wcslen(text) > 20);
        }
        // One reset restores every adjustment, the dial included, without disturbing the
        // comparison mode it shares a struct with, and saves through the same path.
        app.m_comparison.mode = ComparisonMode::Wipe;
        app.m_colorSettings.saturation = 2.0f;
        app.AdjustWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_ADJ_RESET, BN_CLICKED), 0);
        CHECK_EQ(app.m_comparison.strength, 1.0f);
        CHECK_EQ(app.m_colorSettings.saturation, 1.0f);
        CHECK(app.m_comparison.mode == ComparisonMode::Wipe);
        CHECK_EQ(int(SendMessageW(track, TBM_GETPOS, 0, 0)), 100);
        CHECK_EQ(app.ReadIniFloat(L"VideoAdjustments", L"NeuralStrength", -1.0f), 1.0f);
        app.AdjustWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_ADJ_CLOSE, BN_CLICKED), 0);
        CHECK(app.m_adjustWnd == nullptr);
        CHECK(!IsWindow(dialog));
        app.m_comparison = {}; app.m_cachedPlayback = cachedPlayback;
    }

    // The export dialog, driven the way a user drives it.
    //
    // Every control is clicked and read back, because a control this dialog
    // builds but leaves out of its WM_COMMAND router is drawn, movable and
    // inert - which is exactly what shipped when stacking was added to the
    // neural settings dialog, and was invisible until a test pressed it.
    static void CheckExportStagesDialog(PlayerApp& app)
    {
        app.m_exportSelection = {};
        app.ShowExportStages();
        CHECK(app.m_exportStagesWnd != nullptr);
        if (!app.m_exportStagesWnd) return;
        const HWND dialog = app.m_exportStagesWnd;
        const auto click = [&](int id) {
            HWND box = GetDlgItem(dialog, id);
            CHECK(box != nullptr);
            if (!box) return;
            SendMessageW(box, BM_SETCHECK,
                         SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
            app.ExportStagesWndProc(dialog, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
        };
        const auto select = [&](int id, int index) {
            HWND combo = GetDlgItem(dialog, id);
            CHECK(combo != nullptr);
            if (!combo) return;
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
            app.ExportStagesWndProc(dialog, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), 0);
        };

        // Nothing selected is a refusal, and the Export button says so by being
        // unavailable rather than by failing minutes into a render.
        CHECK(!app.CurrentExportPlan().valid);
        CHECK(IsWindowEnabled(GetDlgItem(dialog, IDC_EX_RUN)) == FALSE);

        click(IDC_EX_NEURAL);
        CHECK(app.m_exportSelection.neural);
        // The plan's own arithmetic is pinned in PolicyTests against explicit
        // geometry. What belongs HERE is that the dialog's controls reach the
        // selection the plan is built from - this fixture has no media loaded,
        // so every plan it can build is refused for want of a source, which
        // would make a validity assertion here a test of the fixture.
        const auto planFor = [&](uint32_t sourceWidth, uint32_t sourceHeight) {
            return PlanExport(app.m_exportSelection, sourceWidth, sourceHeight, 30.0, 2, false);
        };
        CHECK(planFor(1280, 720).valid);
        CHECK_EQ(uint32_t{1280}, planFor(1280, 720).outputWidth);   // neural alone does not grow

        // The rung is dead UI until there is something to upscale.
        CHECK(IsWindowEnabled(GetDlgItem(dialog, IDC_EX_RESOLUTION)) == FALSE);
        click(IDC_EX_UPSCALE);
        CHECK(app.m_exportSelection.upscale);
        CHECK(IsWindowEnabled(GetDlgItem(dialog, IDC_EX_RESOLUTION)) != FALSE);
        select(IDC_EX_RESOLUTION, 2);   // 2160p
        CHECK_EQ(uint32_t{2160}, app.m_exportSelection.targetHeight);

        // Both stages are still ONE pass: two would run the model at source
        // size and upscale afterwards, which is the Neural Upstream order
        // rather than NVIDIA's.
        const ExportPlan both = planFor(1280, 720);
        CHECK(both.valid);
        CHECK(both.requireNeural);
        CHECK_EQ(uint32_t{3840}, both.outputWidth);   // the 2160p rung just chosen
        CHECK_EQ(uint32_t{1}, ExportStageCount(both));

        click(IDC_EX_FRAMEGEN);
        CHECK(app.m_exportSelection.frameGeneration);
        const ExportPlan all = planFor(1280, 720);
        CHECK_EQ(uint32_t{2}, ExportStageCount(all));
        CHECK_EQ(60.0, all.outputFps);

        // Turning a stage back off has to reach the plan too; a checkbox that
        // only ever latches on is the same inert-control bug wearing a hat.
        click(IDC_EX_UPSCALE);
        CHECK(!app.m_exportSelection.upscale);
        CHECK_EQ(uint32_t{1280}, planFor(1280, 720).outputWidth);
        CHECK(IsWindowEnabled(GetDlgItem(dialog, IDC_EX_RESOLUTION)) == FALSE);

        DestroyWindow(dialog);
        CHECK(app.m_exportStagesWnd == nullptr);
    }

    static void CheckNeuralSettingsDialog(PlayerApp& app)
    {
        app.m_neuralSettings = {}; app.m_renderGuides = {}; app.m_temporalSettings = {};
        app.ShowNeuralSettings();
        CHECK(app.m_neuralWnd != nullptr);
        if (!app.m_neuralWnd) return;
        const HWND dialog = app.m_neuralWnd;
        const auto setTrack = [&](int id, int pos) { SendMessageW(GetDlgItem(dialog, id), TBM_SETPOS, TRUE, pos); };
        setTrack(IDC_NS_INTENSITY, 150); setTrack(IDC_NS_SKIN, 25);
        app.NeuralWndProc(dialog, WM_HSCROLL, 0, 0);
        CHECK(std::abs(app.m_neuralSettings.intensity - 1.5f) < 0.001f);
        CHECK(std::abs(app.m_neuralSettings.skinStructure + 0.75f) < 0.001f);
        CHECK_EQ(std::wstring(L"1.50"), ReadText(GetDlgItem(dialog, IDC_NS_INTENSITY + 100)));
        // Colour strength and the render preset are not offered: the runtime
        // ignores them, so the dialog must not present them as quality controls.
        CHECK(GetDlgItem(dialog, 7305) == nullptr);
        CHECK(GetDlgItem(dialog, 7306) == nullptr);
        SendMessageW(GetDlgItem(dialog, IDC_NS_STYLE), CB_SETCURSEL, 2, 0);
        app.NeuralWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_NS_STYLE, CBN_SELCHANGE), 0);
        CHECK_EQ(app.m_neuralSettings.style, 2);
        SendMessageW(GetDlgItem(dialog, IDC_NS_AUTOMASK), BM_SETCHECK, BST_UNCHECKED, 0);
        app.NeuralWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_NS_AUTOMASK, BN_CLICKED), 0);
        CHECK(!app.m_neuralSettings.autoMask);
        // Stacking, which arrived with RenoDX 6.x. The combo lists 1..4 and the
        // setting is the count, so index 2 must read back as three passes - an
        // off-by-one here would silently render a different video than the one
        // the dialog says it is rendering, and the cache would key it as that.
        CHECK_EQ(app.m_neuralSettings.passes, 1);
        CHECK(IsWindowEnabled(GetDlgItem(dialog, IDC_NS_CHAINED)) == FALSE);
        SendMessageW(GetDlgItem(dialog, IDC_NS_PASSES), CB_SETCURSEL, 2, 0);
        app.NeuralWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_NS_PASSES, CBN_SELCHANGE), 0);
        CHECK_EQ(app.m_neuralSettings.passes, 3);
        // Chained history governs passes 2+, so it is dead UI at one pass and
        // live above it rather than a switch that quietly does nothing.
        CHECK(IsWindowEnabled(GetDlgItem(dialog, IDC_NS_CHAINED)) != FALSE);
        CHECK(app.m_neuralSettings.chainedHistory);
        SendMessageW(GetDlgItem(dialog, IDC_NS_CHAINED), BM_SETCHECK, BST_UNCHECKED, 0);
        app.NeuralWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_NS_CHAINED, BN_CLICKED), 0);
        CHECK(!app.m_neuralSettings.chainedHistory);
        // Both reach the add-on: the override list is what ConfigureNeuralAddon
        // writes into the runtime's ReShade.ini, and a control the dialog edits
        // but never sends is the failure this pins.
        {
            const auto overrides = NeuralAddonOverridesFor(app.m_neuralSettings);
            const auto valueOf = [&](std::string_view key) {
                for (const auto& entry : overrides)
                    if (entry.first == key) return entry.second;
                return std::string("<missing>");
            };
            CHECK_EQ(std::string("3"), valueOf("NRPasses"));
            CHECK_EQ(std::string("0"), valueOf("NRChainedHistory"));
        }
        // Guide switches persist for the next render and reach the live guide generator.
        CHECK(app.m_guides.Controls().depth);
        SendMessageW(GetDlgItem(dialog, IDC_NS_GUIDE_DEPTH), BM_SETCHECK, BST_UNCHECKED, 0);
        app.NeuralWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_NS_GUIDE_DEPTH, BN_CLICKED), 0);
        CHECK((app.m_renderGuides == GuideControls{true, false}));
        CHECK(!app.m_guides.Controls().depth);
        CHECK(app.m_guideReset && app.m_dlssReset);
        // The Scene cuts ladder: the combo index is the rung, and the choice reaches
        // the live guide generator as well as the next render.
        CHECK_EQ(int(SendMessageW(GetDlgItem(dialog, IDC_NS_SCENE_CUTS), CB_GETCOUNT, 0, 0)), 4);
        CHECK_EQ(int(SendMessageW(GetDlgItem(dialog, IDC_NS_SCENE_CUTS), CB_GETCURSEL, 0, 0)), 0);
        SendMessageW(GetDlgItem(dialog, IDC_NS_SCENE_CUTS), CB_SETCURSEL, 3, 0);
        app.NeuralWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_NS_SCENE_CUTS, CBN_SELCHANGE), 0);
        CHECK(app.m_temporalSettings.sceneCuts == scene_cut::Sensitivity::Off);
        CHECK(app.m_guides.SceneCutSensitivity() == scene_cut::Sensitivity::Off);
        // Temporal stability: Off first and selected on a fresh install.
        CHECK_EQ(int(SendMessageW(GetDlgItem(dialog, IDC_NS_STABILITY), CB_GETCOUNT, 0, 0)), 4);
        CHECK_EQ(int(SendMessageW(GetDlgItem(dialog, IDC_NS_STABILITY), CB_GETCURSEL, 0, 0)), 0);
        SendMessageW(GetDlgItem(dialog, IDC_NS_STABILITY), CB_SETCURSEL, 2, 0);
        app.NeuralWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_NS_STABILITY, CBN_SELCHANGE), 0);
        CHECK(app.m_temporalSettings.stability == TemporalStability::Medium);
        // Every control the dialog offers carries help text, and the text is the
        // localized tip rather than an empty tool.
        const auto tipHost = app.m_tipHosts.find(dialog);
        CHECK(tipHost != app.m_tipHosts.end());
        if (tipHost != app.m_tipHosts.end()) {
            const int tools = int(SendMessageW(tipHost->second, TTM_GETTOOLCOUNT, 0, 0));
            CHECK(tools >= 12);
            for (const int id : {IDC_NS_INTENSITY, IDC_NS_STRUCTURE, IDC_NS_TONE, IDC_NS_SKIN,
                                 IDC_NS_STYLE, IDC_NS_AUTOMASK, IDC_NS_GUIDE_MV, IDC_NS_GUIDE_DEPTH,
                                 IDC_NS_PASSES, IDC_NS_CHAINED, IDC_NS_SCENE_CUTS, IDC_NS_STABILITY,
                                 IDC_NS_APPLY, IDC_NS_RESET}) {
                wchar_t text[512] = {};
                TTTOOLINFOW info{};
                info.cbSize = TTTOOLINFOW_V2_SIZE;
                info.hwnd = dialog;
                info.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(dialog, id));
                info.lpszText = text;
                SendMessageW(tipHost->second, TTM_GETTEXTW, UINT_PTR{512}, reinterpret_cast<LPARAM>(&info));
                CHECK(wcslen(text) > 20);
            }
        }
        // Apply saves the values even when nothing can be rendered right now.
        app.m_opt.neuralAddonConfigured = false;
        app.NeuralWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_NS_APPLY, BN_CLICKED), 0);
        CHECK(!app.NeuralJobActive());
        {
            NeuralSettings saved;
            CHECK(LoadNeuralSettings(app.SettingsPath(), saved));
            CHECK(saved == app.m_neuralSettings);
            CHECK_EQ(GetPrivateProfileIntW(L"NeuralGuides", L"Depth", 1, app.SettingsPath().c_str()), UINT{0});
            // By name, so a later build that reorders the rungs reads the same choice.
            wchar_t cuts[32] = {};
            GetPrivateProfileStringW(L"Temporal", L"SceneCuts", L"", cuts, 32, app.SettingsPath().c_str());
            CHECK_EQ(std::wstring(L"off"), std::wstring(cuts));
            wchar_t stability[32] = {};
            GetPrivateProfileStringW(L"Temporal", L"Stability", L"", stability, 32, app.SettingsPath().c_str());
            CHECK_EQ(std::wstring(L"medium"), std::wstring(stability));
        }
        app.NeuralWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_NS_RESET, BN_CLICKED), 0);
        CHECK(app.m_neuralSettings == NeuralSettings{});
        CHECK_EQ(app.m_neuralSettings.passes, 1);
        CHECK(app.m_neuralSettings.chainedHistory);
        CHECK_EQ(int(SendMessageW(GetDlgItem(dialog, IDC_NS_PASSES), CB_GETCURSEL, 0, 0)), 0);
        CHECK(IsWindowEnabled(GetDlgItem(dialog, IDC_NS_CHAINED)) == FALSE);
        CHECK(app.m_renderGuides.IsDefault());
        CHECK(app.m_guides.Controls().depth);
        CHECK(app.m_temporalSettings.IsDefault());
        CHECK(app.m_guides.SceneCutSensitivity() == scene_cut::Sensitivity::Default);
        CHECK_EQ(int(SendMessageW(GetDlgItem(dialog, IDC_NS_SCENE_CUTS), CB_GETCURSEL, 0, 0)), 0);
        CHECK_EQ(int(SendMessageW(GetDlgItem(dialog, IDC_NS_STABILITY), CB_GETCURSEL, 0, 0)), 0);
        CHECK_EQ(int(SendMessageW(GetDlgItem(dialog, IDC_NS_INTENSITY), TBM_GETPOS, 0, 0)), 100);
        CHECK_EQ(int(SendMessageW(GetDlgItem(dialog, IDC_NS_GUIDE_DEPTH), BM_GETCHECK, 0, 0)), BST_CHECKED);
        app.NeuralWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_NS_CLOSE, BN_CLICKED), 0);
        CHECK(app.m_neuralWnd == nullptr);
        CHECK(!IsWindow(dialog));
    }

    // The render report (P2.11) is the receipt's metrics in words: the numbers it
    // prints are the receipt's, the settings are the ones the render was made with,
    // and a receipt without metrics - every render made before them - says so
    // rather than printing zeros.
    static void CheckRenderReport(PlayerApp& app)
    {
        (void)app;
        const std::string receipt =
            "{\"schema\":1,\"request\":{\"temporal\":\"cuts=more,stability=medium\"},"
            "\"result\":{\"ok\":true,\"metrics\":{\"frames\":120,\"pairs\":117,\"shots\":3,"
            "\"sourceWarpError\":2.5,\"outputWarpError\":3.25,\"flickerAdded\":0.75,"
            "\"sourceSigma\":4,\"outputSigma\":3.5,\"sigmaAdded\":-0.5,"
            "\"lumaShift\":-1.2,\"colorDelta\":6.4}}}";
        const std::wstring text = PlayerApp::RenderReportText(Localizer{}, receipt, L"Clip");
        for (const wchar_t* expected : {L"Clip", L"120", L"117", L"+0.75", L"2.50", L"3.25", L"-0.50",
                                        L"6.40", L"-1.20", L"more", L"medium"})
            CHECK(text.find(expected) != std::wstring::npos);
        const std::string legacy = "{\"schema\":1,\"request\":{},\"result\":{\"ok\":true,\"metrics\":null}}";
        CHECK_EQ(Localizer{}.Get(L"report.unmeasured"), PlayerApp::RenderReportText(Localizer{}, legacy, L"Clip"));
        CHECK_EQ(Localizer{}.Get(L"report.unavailable"), PlayerApp::RenderReportText(Localizer{}, "{truncated", L"Clip"));
    }

    // A settings change the player cannot preview - the picture is playing - used to
    // leave the previous render on screen with nothing saying so. The status notice
    // is the only thing that can say it, so its whole lifecycle is pinned here.
    static void CheckSettingsAheadNotice(PlayerApp& app)
    {
        const std::wstring ahead = app.T(L"neural.settings.ahead");
        CHECK(!ahead.empty());
        app.m_cachedPlayback = true; app.m_liveSession = false; app.m_previewShown = false;
        app.m_playing = true; app.m_neuralNotice.clear();
        app.m_cachedSettings = {}; app.m_cachedGuides = {};
        app.m_neuralSettings = {}; app.m_renderGuides = {};
        app.NoteSettingsAheadOfRender();
        CHECK(app.m_neuralNotice.empty());

        // Apply while playing: the ini is written, no preview can run, and the
        // picture is still the previous render.
        app.m_neuralSettings.intensity = 1.5f;
        app.ApplyNeuralSettings();
        CHECK(!app.NeuralJobActive());
        CHECK_EQ(ahead, app.m_neuralNotice);

        // A shown preview is the picture catching up. The cache entry is still the
        // old render, so the settings comparison alone would keep claiming otherwise.
        app.m_previewShown = true;
        app.NoteSettingsAheadOfRender();
        CHECK(app.m_neuralNotice.empty());
        // Both ways out of a preview - the next played frame and a seek - go through
        // one guard, so the notice returns with the stale render either way.
        app.LeaveSettingsPreviewFrame();
        CHECK(!app.m_previewShown);
        CHECK_EQ(ahead, app.m_neuralNotice);
        // Coming back to the settings that produced the picture clears it with no render.
        app.m_neuralSettings = {};
        app.NoteSettingsAheadOfRender();
        CHECK(app.m_neuralNotice.empty());

        // A guide switch counts as a change, and a failure notice outranks this one.
        app.m_renderGuides = GuideControls{false, true};
        app.NoteSettingsAheadOfRender();
        CHECK_EQ(ahead, app.m_neuralNotice);
        app.m_neuralNotice = L"driver below floor";
        app.NoteSettingsAheadOfRender();
        CHECK_EQ(std::wstring(L"driver below floor"), app.m_neuralNotice);

        app.m_neuralNotice.clear(); app.m_renderGuides = {}; app.m_neuralSettings = {};
        app.m_cachedPlayback = false; app.m_playing = false;
    }

    static void CheckEncoderSettingsDialog(PlayerApp& app)
    {
        app.m_gpuColorConversion = false; app.m_gpuSourceConversion = false; app.m_nvencPreset = 7;
        app.ShowEncoderSettings();
        CHECK(app.m_encoderWnd != nullptr);
        if (!app.m_encoderWnd) return;
        const HWND dialog = app.m_encoderWnd;
        CHECK(GetDlgItem(dialog, IDC_ES_GPU_CONVERT) != nullptr);
        CHECK(GetDlgItem(dialog, IDC_ES_GPU_SOURCE) != nullptr);
        // The window is sized from the design client size through the DPI-aware
        // frame conversion, so the client area must come back exactly - on a scaled
        // monitor the 96-dpi conversion left it ~30px short and the bottom-anchored
        // buttons overlapped the note text.
        // The design size is in 96-dpi units, so it is scaled to the dialog's dpi.
        RECT client{};
        CHECK(GetClientRect(dialog, &client) != FALSE);
        const int dpi = static_cast<int>(ActiveWindowDpi(dialog));
        CHECK_EQ(int(client.right), MulDiv(PlayerApp::kEncoderDesignW, dpi, 96));
        CHECK_EQ(int(client.bottom), MulDiv(PlayerApp::kEncoderDesignH, dpi, 96));
        CHECK(GetDlgItem(dialog, IDC_ES_NVENC_PRESET) != nullptr);
        SendMessageW(GetDlgItem(dialog, IDC_ES_GPU_SOURCE), BM_SETCHECK, BST_CHECKED, 0);
        app.EncoderWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_ES_GPU_SOURCE, BN_CLICKED), 0);
        // Index 6 is p7, which is NOT the default any more - see
        // EncoderSpec::nvencPreset - so this still proves the combo drives the
        // value rather than agreeing with it by accident.
        SendMessageW(GetDlgItem(dialog, IDC_ES_NVENC_PRESET), CB_SETCURSEL, 6, 0);
        app.EncoderWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_ES_NVENC_PRESET, CBN_SELCHANGE), 0);
        CHECK(app.m_gpuSourceConversion);
        CHECK_EQ(app.m_nvencPreset, uint32_t{7});
        // Read saves via SaveVideoSettings(), since nothing needs re-rendering.
        CHECK_EQ(GetPrivateProfileIntW(L"Encoding", L"GpuSourceConversion", 0, app.SettingsPath().c_str()), UINT{1});
        CHECK_EQ(GetPrivateProfileIntW(L"Encoding", L"NvencPreset", 1, app.SettingsPath().c_str()), UINT{7});
        app.EncoderWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_ES_RESET, BN_CLICKED), 0);
        CHECK(!app.m_gpuColorConversion);
        CHECK(!app.m_gpuSourceConversion);
        CHECK_EQ(app.m_nvencPreset, uint32_t{5});
        CHECK_EQ(int(SendMessageW(GetDlgItem(dialog, IDC_ES_GPU_SOURCE), BM_GETCHECK, 0, 0)), BST_UNCHECKED);
        app.EncoderWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_ES_CLOSE, BN_CLICKED), 0);
        CHECK(app.m_encoderWnd == nullptr);
        CHECK(!IsWindow(dialog));
    }

    // Both settings dialogs are modeless and can be open together, and TTM_ADDTOOL
    // keeps the pointer it is handed rather than copying the string: the dialog that
    // opens second must not free the first dialog's tip text while the first
    // dialog's tooltips are still subclassed onto its controls.
    static void CheckSettingsDialogTipsSurviveASecondDialog(PlayerApp& app)
    {
        app.ShowNeuralSettings();
        const HWND neural = app.m_neuralWnd;
        CHECK(neural != nullptr);
        if (!neural) return;
        app.ShowEncoderSettings();
        CHECK(app.m_encoderWnd != nullptr);
        const auto host = app.m_tipHosts.find(neural);
        CHECK(host != app.m_tipHosts.end());
        if (host != app.m_tipHosts.end()) {
            wchar_t text[512]{};
            TTTOOLINFOW info{};
            info.cbSize = TTTOOLINFOW_V2_SIZE;
            info.hwnd = neural;
            info.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(neural, IDC_NS_RESET));
            info.lpszText = text;
            SendMessageW(host->second, TTM_GETTEXTW, UINT_PTR{512}, reinterpret_cast<LPARAM>(&info));
            // The string the first dialog registered must still read back verbatim
            // after the second dialog built its own tools.
            CHECK_EQ(std::wstring{text}, app.T(L"neural.tip.reset"));
        }
        if (app.m_encoderWnd) DestroyWindow(app.m_encoderWnd);
        DestroyWindow(neural);
        CHECK(app.m_neuralWnd == nullptr);
        CHECK(app.m_encoderWnd == nullptr);
    }

    static std::wstring ReadText(HWND window)
    {
        wchar_t text[64]{};
        GetWindowTextW(window, text, 64);
        return text;
    }

    static void CheckCacheSettings(PlayerApp& app)
    {
        const auto settings=app.SettingsPath();
        const auto writeRoot=[&](const std::filesystem::path& root,const wchar_t* automatic){
            CHECK(WritePrivateProfileStringW(L"Storage",L"CacheDirectory",root.c_str(),settings.c_str()));
            CHECK(WritePrivateProfileStringW(L"Storage",L"CacheDirectoryAutomatic",automatic,settings.c_str()));
        };
        const auto automaticFlag=[&]{
            return GetPrivateProfileIntW(L"Storage",L"CacheDirectoryAutomatic",-1,settings.c_str());
        };

        const auto legacy=NeuralCacheManager::LegacyDefaultRoot();
        CHECK(legacy.has_value());
        if(legacy){
            // Old versions persisted their default without an ownership flag.
            writeRoot(*legacy,nullptr);
            app.LoadVideoSettings();
            CHECK(app.m_cacheRoot.empty());
            // An explicitly selected default-looking root still belongs to the
            // user and must retain its custom setting when saved again.
            writeRoot(*legacy,L"0");app.LoadVideoSettings();
            CHECK_EQ(app.m_cacheRoot,*legacy);
            app.SaveCacheSettings();CHECK_EQ(automaticFlag(),UINT{0});

            if(const auto physical=ExistingWritableCacheRoot(*legacy)){
                // MSIX can merge logical reads with package-private writes;
                // previous releases persisted the physical writable location.
                writeRoot(*physical,nullptr);app.LoadVideoSettings();
                CHECK(app.m_cacheRoot.empty());
                app.m_cacheRoot=*physical;app.SaveCacheSettings();
                CHECK_EQ(automaticFlag(),UINT{1});
                writeRoot(*physical,L"0");app.LoadVideoSettings();
                CHECK_EQ(app.m_cacheRoot,*physical);
                app.SaveCacheSettings();CHECK_EQ(automaticFlag(),UINT{0});
                // Cache initialization resolves the logical explicit path to
                // its physical write location without changing user intent.
                writeRoot(*legacy,L"0");app.LoadVideoSettings();
                app.m_cacheRoot=*physical;app.SaveCacheSettings();
                CHECK_EQ(automaticFlag(),UINT{0});
                app.LoadVideoSettings();CHECK_EQ(app.m_cacheRoot,*physical);
            }
        }
        const auto movedRoot=settings.parent_path()/L"previous-installation"/L"cache"/L"v1";
        writeRoot(movedRoot,L"1");
        app.LoadVideoSettings();
        CHECK(app.m_cacheRoot.empty());

        const auto existingCustom=settings.parent_path();
        const auto missingCustom=settings.parent_path()/
            (L"uncreated-explicit-cache-"+std::to_wstring(GetCurrentProcessId()));
        CHECK(std::filesystem::exists(existingCustom));
        CHECK(!std::filesystem::exists(missingCustom));
        for(const auto& custom:{existingCustom,missingCustom}){
            writeRoot(custom,L"0");
            app.LoadVideoSettings();
            CHECK_EQ(app.m_cacheRoot,custom);
            app.SaveCacheSettings();
            CHECK_EQ(automaticFlag(),UINT{0});
            app.m_cacheRoot.clear();app.LoadVideoSettings();
            CHECK_EQ(app.m_cacheRoot,custom);
        }
        app.m_cacheRoot=app.ExecutableDirectory()/L"cache"/L"v1";
        app.SaveCacheSettings();
        CHECK_EQ(automaticFlag(),UINT{1});
        app.LoadVideoSettings();
        CHECK(app.m_cacheRoot.empty());

        // The UI test executable has its own settings file. Leave storage empty
        // for the existing playback and cache-lifecycle regression fixtures.
        app.m_cacheRoot.clear();
        CHECK(WritePrivateProfileStringW(L"Storage",L"CacheDirectory",nullptr,settings.c_str()));
        CHECK(WritePrivateProfileStringW(L"Storage",L"CacheDirectoryAutomatic",nullptr,settings.c_str()));
    }

    static std::optional<std::filesystem::path> ExistingWritableCacheRoot(const std::filesystem::path& root)
    {
        std::error_code error;
        if(!std::filesystem::is_directory(root,error))return std::nullopt;
        const auto probe=root/(L".ui-migration-probe-"+std::to_wstring(GetCurrentProcessId())+
            L"-"+std::to_wstring(GetTickCount64()));
        const HANDLE file=CreateFileW(probe.c_str(),GENERIC_WRITE,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        CHECK(file!=INVALID_HANDLE_VALUE);
        if(file==INVALID_HANDLE_VALUE)return std::nullopt;
        std::wstring finalPath(32768,L'\0');
        const DWORD length=GetFinalPathNameByHandleW(file,finalPath.data(),
            static_cast<DWORD>(finalPath.size()),FILE_NAME_NORMALIZED);
        CloseHandle(file);
        CHECK(length>0&&length<finalPath.size());
        if(!length||length>=finalPath.size())return std::nullopt;
        finalPath.resize(length);
        const auto resolved=std::filesystem::canonical(std::filesystem::path(finalPath).parent_path(),error);
        CHECK(!error);
        return error?std::nullopt:std::optional<std::filesystem::path>(resolved);
    }

    static void CheckFullscreenLifecycle()
    {
        PlayerApp app(AppOptions{});
        const HINSTANCE instance=GetModuleHandleW(nullptr);
        WNDCLASSW mainClass{};mainClass.hInstance=instance;
        mainClass.lpszClassName=L"DLSSFullscreenRegressionMain";
        mainClass.lpfnWndProc=PlayerApp::WndProcStatic;
        CHECK(RegisterClassW(&mainClass));
        WNDCLASSW viewportClass=mainClass;
        viewportClass.lpszClassName=L"DLSSFullscreenRegressionViewport";
        viewportClass.lpfnWndProc=PlayerApp::ViewportWndProcStatic;
        CHECK(RegisterClassW(&viewportClass));
        WNDCLASSW renderClass=mainClass;
        renderClass.lpszClassName=L"DLSSFullscreenRegressionRender";
        renderClass.lpfnWndProc=PlayerApp::RenderWndProcStatic;
        CHECK(RegisterClassW(&renderClass));
        const HMENU menu=app_menu::CreateMenuBar(app.m_loc,true);
        app.m_hwnd=CreateWindowExW(0,mainClass.lpszClassName,L"Fullscreen regression",
            WS_OVERLAPPEDWINDOW,100,100,800,600,nullptr,menu,instance,&app);
        CHECK(app.m_hwnd);
        const POINT minimum=MinimumPlayerWindowTrackSize(app.m_hwnd,ActiveWindowDpi(app.m_hwnd));
        CHECK(SetWindowPos(app.m_hwnd,nullptr,100,100,std::max<LONG>(800,minimum.x),std::max<LONG>(600,minimum.y),
                           SWP_NOZORDER|SWP_NOACTIVATE));
        app.m_viewport=CreateWindowExW(0,viewportClass.lpszClassName,nullptr,
            WS_CHILD|WS_VISIBLE,0,0,100,100,app.m_hwnd,nullptr,instance,nullptr);
        app.m_renderWnd=CreateWindowExW(0,renderClass.lpszClassName,nullptr,
            WS_CHILD|WS_VISIBLE,0,0,100,100,app.m_viewport,nullptr,instance,&app);
        CHECK(app.m_viewport&&app.m_renderWnd);
        app.m_loaded=true;app.Layout();
        RECT before{};GetWindowRect(app.m_hwnd,&before);
        const LONG style=GetWindowLongW(app.m_hwnd,GWL_STYLE);

        SendMessageW(app.m_hwnd,WM_KEYDOWN,VK_F11,0);
        CHECK(app.m_fullscreen);
        CHECK(GetMenu(app.m_hwnd)==nullptr);
        // Entering fullscreen samples the REAL cursor (GetCursorPos), and every
        // pointer move below is measured from that sample. Where the desk's
        // cursor happened to be decided the outcome: with no readable cursor (a
        // locked or disconnected session, a runner without an input desktop) the
        // sample stayed unknown, so the synthesized "stationary" move read as
        // movement and revealed the controls. Pin it inside the picture instead.
        {
            RECT picture{};GetWindowRect(app.m_renderWnd,&picture);
            app.m_fullscreenPointer=POINT{(picture.left+picture.right)/2,(picture.top+picture.bottom)/2};
            app.m_fullscreenPointerKnown=true;
        }
        RECT client{},viewport{};
        GetClientRect(app.m_hwnd,&client);GetClientRect(app.m_viewport,&viewport);
        CHECK(EqualRect(&client,&viewport));
        CHECK(app.ToolbarItems().empty());
        CHECK(app.FocusableItems().empty());
        CHECK(!app.VolumeRect().has_value());
        const RECT timeline=app.TimelineRect();
        CHECK(IsRectEmpty(&timeline));
        SendMessageW(app.m_hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(client.right/2,client.bottom-20));
        CHECK(!app.m_dragSeek&&!app.m_dragVolume);
        CHECK_EQ(app.m_pressedToolbarAction,ToolbarAction::None);
        SendMessageW(app.m_hwnd,WM_LBUTTONUP,0,MAKELPARAM(client.right/2,client.bottom-20));

        HDC dc=CreateCompatibleDC(nullptr);
        drawnText.clear();app.RenderUi(dc,client);
        CHECK(drawnText.empty());
        CHECK(DeleteDC(dc));

        // Child resizing can synthesize motion without the physical pointer
        // moving. That message must not immediately undo fullscreen hiding.
        POINT pointer=app.m_fullscreenPointer;
        ScreenToClient(app.m_renderWnd,&pointer);
        SendMessageW(app.m_renderWnd,WM_MOUSEMOVE,0,MAKELPARAM(pointer.x,pointer.y));
        CHECK(GetMenu(app.m_hwnd)==nullptr);

        // Real movement over the render child restores the actual menu and
        // available controls, then an idle timer removes them again.
        MoveFullscreenPointer(app,app.m_renderWnd);
        CHECK_EQ(GetMenu(app.m_hwnd),menu);
        CHECK(!app.ToolbarItems().empty());
        GetClientRect(app.m_hwnd,&client);GetClientRect(app.m_viewport,&viewport);
        CHECK(viewport.bottom<client.bottom);
        CHECK(app.m_fullscreenTimer!=0);
        const UINT_PTR timer=app.m_fullscreenTimer;
        SendMessageW(app.m_hwnd,WM_TIMER,timer,0);
        CHECK_EQ(GetMenu(app.m_hwnd),menu); // No early hide.
        app.m_fullscreenLastInput=Clock::now()-std::chrono::seconds(3);
        SendMessageW(app.m_hwnd,WM_NCMOUSEMOVE,HTMENU,
            MAKELPARAM(app.m_fullscreenPointer.x+7,app.m_fullscreenPointer.y+5));
        SendMessageW(app.m_hwnd,WM_TIMER,timer,0);
        CHECK_EQ(GetMenu(app.m_hwnd),menu); // Native-menu movement is activity too.
        ExpireFullscreenIdle(app);
        CHECK(GetMenu(app.m_hwnd)==nullptr);
        CHECK_EQ(app.m_fullscreenTimer,UINT_PTR{0});

        // Letterbox bars belong to a different child window.
        MoveFullscreenPointer(app,app.m_viewport);
        CHECK_EQ(GetMenu(app.m_hwnd),menu);
        // A late worker update while the menu is detached must be reconciled.
        ExpireFullscreenIdle(app);
        app.m_upscaleAuto=false;app.m_upscaleTargetHeight=2160;
        app.SyncFeatureMenuState();
        MoveFullscreenPointer(app,app.m_hwnd);
        CHECK((GetMenuState(menu,IDM_UPSCALE_2160,MF_BYCOMMAND)&MF_CHECKED)!=0);
        // Handed back before this app's destructor saves. That destructor runs
        // BEFORE the caller's, so leaving the pinned rung here would write
        // UpscaleAuto=0 and rely on the caller's later save to undo it - which
        // it does today, by ordering alone.
        app.HandleCommand(IDM_UPSCALE_AUTO);
        CHECK(app.m_upscaleAuto);

        // Do not hide underneath pointer capture, a native menu, a modal
        // dialog (disabled owner), adjustments, or keyboard navigation.
        // Hosted Windows runners can expose a 1024-pixel desktop, where the
        // toolbar intentionally omits the volume slider. Exercise the capture
        // guard directly so this check does not depend on monitor width.
        SetCapture(app.m_hwnd);CHECK_EQ(GetCapture(),app.m_hwnd);
        ExpireFullscreenIdle(app);CHECK_EQ(GetMenu(app.m_hwnd),menu);
        CHECK(ReleaseCapture());
        SendMessageW(app.m_hwnd,WM_ENTERMENULOOP,FALSE,0);
        ExpireFullscreenIdle(app);CHECK_EQ(GetMenu(app.m_hwnd),menu);
        SendMessageW(app.m_hwnd,WM_EXITMENULOOP,FALSE,0);
        EnableWindow(app.m_hwnd,FALSE);
        ExpireFullscreenIdle(app);CHECK_EQ(GetMenu(app.m_hwnd),menu);
        EnableWindow(app.m_hwnd,TRUE);
        app.m_adjustWnd=CreateWindowExW(0,L"STATIC",L"Adjustment regression",WS_POPUP,
            -30000,-30000,10,10,app.m_hwnd,nullptr,instance,nullptr);
        ShowWindow(app.m_adjustWnd,SW_SHOWNOACTIVATE);
        ExpireFullscreenIdle(app);CHECK_EQ(GetMenu(app.m_hwnd),menu);
        CHECK(DestroyWindow(app.m_adjustWnd));app.m_adjustWnd=nullptr;
        ExpireFullscreenIdle(app);CHECK(GetMenu(app.m_hwnd)==nullptr);
        SendMessageW(app.m_renderWnd,WM_KEYDOWN,VK_TAB,0);
        CHECK_EQ(GetMenu(app.m_hwnd),menu);
        CHECK(app.m_focusedToolbarAction!=ToolbarAction::None);
        ExpireFullscreenIdle(app);CHECK_EQ(GetMenu(app.m_hwnd),menu);
        MoveFullscreenPointer(app,app.m_renderWnd);
        ExpireFullscreenIdle(app);CHECK(GetMenu(app.m_hwnd)==nullptr);

        // Use a genuine queued Win32 timer once as well as deterministic idle
        // deadlines, proving registration and dispatch through the window proc.
        // The timer fires at 250 ms; a loaded runner can hold it back, so the
        // wait is bounded generously rather than sized to the interval.
        MoveFullscreenPointer(app,app.m_hwnd);
        app.m_fullscreenLastInput=Clock::now()-std::chrono::seconds(3);
        MSG timerMessage{};bool receivedTimer=false;
        for(const ULONGLONG deadline=GetTickCount64()+10000;!receivedTimer&&GetTickCount64()<deadline;){
            MsgWaitForMultipleObjects(0,nullptr,FALSE,100,QS_TIMER);
            while(PeekMessageW(&timerMessage,app.m_hwnd,WM_TIMER,WM_TIMER,PM_REMOVE)){
                receivedTimer|=timerMessage.wParam==timer;
                DispatchMessageW(&timerMessage);
            }
        }
        CHECK(receivedTimer);
        CHECK(GetMenu(app.m_hwnd)==nullptr);

        SendMessageW(app.m_hwnd,WM_KEYDOWN,VK_F11,0);
        CHECK(!app.m_fullscreen);
        CHECK_EQ(GetMenu(app.m_hwnd),menu);
        CHECK_EQ(GetWindowLongW(app.m_hwnd,GWL_STYLE),style);
        RECT restored{};GetWindowRect(app.m_hwnd,&restored);
        CHECK(EqualRect(&before,&restored));
        CHECK(!app.ToolbarItems().empty());
        SendMessageW(app.m_hwnd,WM_TIMER,timer,0);
        CHECK_EQ(GetMenu(app.m_hwnd),menu); // Stale timers cannot hide windowed UI.
        SendMessageW(app.m_hwnd,WM_KEYDOWN,VK_F11,0);
        CHECK(GetMenu(app.m_hwnd)==nullptr);
        app.m_loaded=false;
        CHECK(DestroyWindow(app.m_hwnd));
        CHECK(!IsMenu(menu)); // Detached menu remains owned until destruction.
        app.m_hwnd=nullptr;app.m_viewport=nullptr;app.m_renderWnd=nullptr;
        MSG quit{};PeekMessageW(&quit,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE);
        CHECK(UnregisterClassW(renderClass.lpszClassName,instance));
        CHECK(UnregisterClassW(viewportClass.lpszClassName,instance));
        CHECK(UnregisterClassW(mainClass.lpszClassName,instance));
    }

    static void MoveFullscreenPointer(PlayerApp& app,HWND target)
    {
        POINT pointer=app.m_fullscreenPointer;
        pointer.x+=17;pointer.y+=11;
        ScreenToClient(target,&pointer);
        SendMessageW(target,WM_MOUSEMOVE,0,MAKELPARAM(pointer.x,pointer.y));
    }

    static void ExpireFullscreenIdle(PlayerApp& app)
    {
        app.m_fullscreenLastInput=Clock::now()-std::chrono::seconds(3);
        SendMessageW(app.m_hwnd,WM_TIMER,PlayerApp::kFullscreenTimerId,0);
    }

    static void CheckLoadingFeedback(PlayerApp& app)
    {
        // A wall-clock-only animation must never manufacture completed work.
        const RECT track{20, 100, 420, 110};
        const auto first = ResolveActivityVisual(track, 0, 0, 0, false, true);
        const auto later = ResolveActivityVisual(track, 400, 0, 0, false, true);
        CHECK(first.indeterminate);
        CHECK(first.fill.right > first.fill.left);
        CHECK(first.fill.left != later.fill.left);
        CHECK(first.spinnerStep != later.spinnerStep);
        const auto measured = ResolveActivityVisual(track, 400, 30, 120, true, true);
        CHECK(!measured.indeterminate);
        CHECK_EQ(measured.percent, 25u);
        CHECK_EQ(measured.fill.right, 120L);
        CHECK_EQ(ResolveActivityVisual(track, 1000, 999, 120, true, true).fill.right, 420L);
        const auto reduced = ResolveActivityVisual(track, 400, 0, 0, false, false);
        CHECK_EQ(reduced.spinnerStep, 0u);
        CHECK_EQ(reduced.fill.left, ResolveActivityVisual(track, 800, 0, 0, false, false).fill.left);
        for (const UINT dpi : {96u, 144u, 192u}) {
            const auto layout = LayoutPreRenderSurface(MulDiv(640,dpi,96), MulDiv(420,dpi,96), dpi);
            CHECK(layout.spinner.bottom <= layout.title.top);
            CHECK(layout.spinner.top >= 0);
            CHECK(layout.spinner.right > layout.spinner.left);
        }

        // Exercise the real timer and repaint path without taking focus.
        SetWindowPos(app.m_hwnd, nullptr, -30000, -30000, 800, 600, SWP_NOZORDER|SWP_NOACTIVATE);
        ShowWindow(app.m_hwnd, SW_SHOWNOACTIVATE);
        app.m_youtubeLifecycle.Begin();
        app.SyncSourceActionAvailability();
        CHECK(app.m_activityTimer != 0);
        ValidateRect(app.m_hwnd, nullptr);
        app.WndProc(app.m_hwnd, WM_TIMER, app.m_activityTimer, 0);
        CHECK(GetUpdateRect(app.m_hwnd, nullptr, FALSE));
        HDC dc = CreateCompatibleDC(nullptr);
        drawnText.clear();
        app.RenderUi(dc, RECT{0,0,800,600});
        CHECK(Contains(L"Loading YouTube video"));
        CHECK(!Contains(L"0 frames"));
        app.m_loaded=true;
        app.RenderUi(dc, RECT{0,0,800,600});
        CHECK(IsRectEmpty(&app.m_neuralCancelBounds));
        app.m_loaded=false;
        app.CancelYouTubeResolution();
        CHECK_EQ(app.m_activityTimer, UINT_PTR{0});
        app.m_neuralLifecycle.Begin();
        app.SyncSourceActionAvailability();
        CHECK(app.m_activityTimer != 0);
        app.m_neuralProgress.phase=NeuralRenderPhase::NeuralRendering;
        app.m_neuralProgress.completedFrames=30;
        app.m_neuralProgress.totalFrames=120;
        drawnText.clear();
        app.RenderUi(dc, RECT{0,0,800,600});
        CHECK(Contains(L"25% · 30 / 120 frames"));
        ShowWindow(app.m_hwnd, SW_HIDE);
        app.SyncActivityFeedback();
        CHECK_EQ(app.m_activityTimer, UINT_PTR{0});
        app.CancelNeuralJob();
        CHECK_EQ(app.m_activityTimer, UINT_PTR{0});
        CHECK(DeleteDC(dc));
    }

    static bool Contains(const wchar_t* text)
    {
        return std::find(drawnText.begin(), drawnText.end(), text) != drawnText.end();
    }

    static void CheckSourceMenus(PlayerApp& app, bool enabled)
    {
        const HMENU menu = GetMenu(app.m_hwnd);
        const auto checkCommand = [&](UINT command) {
            const UINT state = GetMenuState(menu, command, MF_BYCOMMAND);
            CHECK(state != static_cast<UINT>(-1));
            CHECK_EQ(enabled, (state & (MF_DISABLED | MF_GRAYED)) == 0);
        };
        checkCommand(IDM_OPEN);
        checkCommand(IDM_OPEN_YOUTUBE);
        for (size_t index = 0; index < kExampleVideos.size(); ++index)
            checkCommand(IDM_EXAMPLE_VIDEO_FIRST + static_cast<UINT>(index));
    }

    static uint64_t QueueCompletion(PlayerApp& app, uint64_t generation, bool cancelled,
                                    NeuralRenderFailure failure = NeuralRenderFailure::None)
    {
        auto completion = std::make_unique<NeuralJobCompletion>();
        completion->generation = generation;
        completion->result.cancelled = cancelled;
        completion->result.failure = failure;
        completion->result.detail = L"Controlled render failure";
        uint64_t token = 0;
        CHECK(app.m_neuralCompletions.RegisterAndPost(std::move(completion),
            [&](uint64_t registered) { token = registered; return true; }));
        return token;
    }

    static void CompleteStale(PlayerApp& app, uint64_t generation)
    {
        app.CompleteNeuralJob(QueueCompletion(app, generation, true));
    }

    static void QueueProgress(PlayerApp& app, NeuralRenderPhase phase,
                              NeuralRenderFailure recovering = NeuralRenderFailure::None, uint32_t retries = 0)
    {
        auto message = std::make_unique<NeuralProgressMessage>();
        message->generation = app.m_neuralLifecycle.generation;
        message->progress.phase = phase;
        message->progress.recovering = recovering;
        message->progress.retries = retries;
        uint64_t token = 0;
        CHECK(app.m_neuralProgressMessages.RegisterAndPost(std::move(message),
            [&](uint64_t registered) { token = registered; return true; }));
        app.CompleteNeuralProgress(token);
    }

    static void CompleteTerminalJob(PlayerApp& app, bool cancelled,
                                    NeuralRenderFailure failure = NeuralRenderFailure::None)
    {
        const uint64_t generation = app.m_neuralLifecycle.Begin();
        app.m_neuralWorker = std::jthread([] {});
        app.SyncSourceActionAvailability();
        CheckSourceMenus(app, false);
        const uint64_t token = QueueCompletion(app, generation, cancelled, failure);
        app.CompleteNeuralJob(token);
        CHECK(!app.NeuralJobActive());
        CHECK(!app.m_neuralWorker.joinable());
        CHECK_EQ(cancelled ? NeuralPlaybackState::OriginalOnly : StateForFailure(failure),
                 app.m_neuralLifecycle.state);
        CheckSourceMenus(app, true);
    }
};

int main(int argc, char** argv)
{
    // --gpu selects the prepared-network-renderer cases, registered separately
    // as NetworkPreparedRendererSmoke. Same binary deliberately: a second target
    // would compile main.cpp again, and this file is already the reason the
    // build amplifies (see 2.12). Remaining arguments are ffmpeg's directory and
    // a scratch directory, which only those cases read.
    test_support::ContainChildProcesses();
    const bool gpuOnly = argc > 1 && std::string_view(argv[1]) == "--gpu";
    if (gpuOnly) {
        if (argc > 2) PlayerAppTestAccess::gpuFfmpegDirectory = std::filesystem::path(argv[2]);
        if (argc > 3) PlayerAppTestAccess::gpuWorkDirectory = std::filesystem::path(argv[3]);
    }
    // The decoder describes a file through Media Foundation when no ffprobe
    // is beside it; the player starts both of these before its first window.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) ||
        FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        std::cerr << "Media Foundation could not start.\n";
        return EXIT_FAILURE;
    }

    size_t ran = 0;
    {
        // Scoped so the fixture's PlayerApp is destroyed - saving its settings -
        // before Media Foundation shuts down, exactly as it was when this was
        // one function.
        PlayerAppTestAccess::Fixture fixture;
        PlayerAppTestAccess::fixture = &fixture;
        if (gpuOnly) {
            // Opened here, not at the top: the gate needs COM, and a machine
            // with no adapter must report the skip rather than fail.
            if (const int skip = gpu_test_gate::SkipWithoutGpu()) {
                PlayerAppTestAccess::fixture = nullptr;
                MFShutdown();
                CoUninitialize();
                return skip;
            }
            ran = ::test_support::run_cases(PlayerAppTestAccess::kGpuCases,
                                            std::size(PlayerAppTestAccess::kGpuCases), {}).ran;
        } else {
            ran = ::test_support::run_cases(PlayerAppTestAccess::kCases,
                                            std::size(PlayerAppTestAccess::kCases), {}).ran;
        }
        PlayerAppTestAccess::fixture = nullptr;
    }

    MFShutdown();
    CoUninitialize();
    std::cout << "Player UI regression: " << ran << " cases, "
              << test_support::assertion_count << " assertions, failures: "
              << test_support::failure_count << '\n';
    return test_support::failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
