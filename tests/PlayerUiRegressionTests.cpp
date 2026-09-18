#include <windows.h>
#include <string>
#include <vector>
#include "TestSupport.h"

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

struct PlayerAppTestAccess {
    static void Run()
    {
        PlayerApp app(AppOptions{});
        CheckCacheSettings(app);
        // Saved playback choices must survive a reload, with invalid volume clamped.
        app.m_volume = 0.35f; app.m_muted = true; app.m_fill = true;
        app.m_neuralRequested = false; app.m_upscaleTargetHeight = 2160;
        app.m_youtubeSourceQuality = YouTubeSourceQuality::P1440;
        app.m_renderGuides = GuideControls{false, true};
        app.m_neuralSettings.intensity = 1.5f; app.m_neuralSettings.preset = 2; app.m_neuralSettings.autoMask = false;
        app.m_comparison.mode = ComparisonMode::Wipe; app.m_comparison.amount = 0.3f; app.m_comparison.splitX = 0.8f; app.m_comparison.zoomScale = 2.0f;
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
        CHECK(std::abs(app.m_comparison.amount - 0.3f) < 0.001f && std::abs(app.m_comparison.splitX - 0.8f) < 0.001f);
        CHECK_EQ(app.m_comparison.zoomScale, 2.0f);
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
        app.WriteIniFloat(L"Comparison", L"Amount", 4.0f);
        app.LoadVideoSettings();
        CHECK(app.m_comparison.mode == ComparisonMode::Neural);
        CHECK_EQ(app.m_comparison.amount, 1.0f);
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
        WNDCLASSW windowClass{};
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
        CheckRecentRolloverKeepsTheCache(app);

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
        const auto upscalingContent = app.ButtonContent(ToolbarAction::ToggleUpscaling);
        CHECK(!upscalingContent.enabled);
        const auto frameGenerationContent = app.ButtonContent(ToolbarAction::ToggleFrameGeneration);
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

        const HMENU featureMenu = GetMenu(app.m_hwnd);
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
        CheckMarkersAndTimecode(app);
        CheckComparisonAvailability(app);
        CheckNeuralStrengthDial(app);
        CheckLiveBufferingPlayIntent(app);
        CheckNeuralToggleQueuedDuringSeek(app);
        CheckNeuralSettingsDialog(app);
        CheckSettingsAheadNotice(app);
        CheckEncoderSettingsDialog(app);

        CheckSettingsDialogTipsSurviveASecondDialog(app);
        CheckLiveExportEntry(app);
        CheckDroppedPreviewJob(app, windowClass);
        CheckUnloadDropsDeferredToggle(app);
        CheckLiveJobDirectoryFailure(app);
        CheckLiveRenderFailureLimit(app);
        CheckJobSourceKeyGuard(app);
        CheckStreamConversionUsesTheAcquiredCopy(app);
        CheckLiveOutOfSyncHandsBack(app);
        CheckPairStallBoundIgnoresAPause(app);
        CheckLivePaceConfirmation(app);
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
        CheckSourceMenus(app, false);
        CHECK(app.NeuralJobActive());
        app.CancelNeuralJob();
        CheckSourceMenus(app, true);

        CheckLoadingFeedback(app);

        HMENU menu = GetMenu(app.m_hwnd);
        SetMenu(app.m_hwnd, nullptr);
        CHECK(DestroyMenu(menu));
        CHECK(DestroyWindow(app.m_hwnd));
        app.m_hwnd = nullptr;
        CHECK(UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance));
        CheckFullscreenLifecycle();
    }

private:
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
        app.m_comparison.amount = 0.5f;
        app.HandleCommand(IDM_COMPARE_BLEND_MORE);
        app.HandleCommand(IDM_COMPARE_BLEND_MORE);
        CHECK(std::abs(app.m_comparison.amount - 0.7f) < 0.001f);
        for (int step = 0; step < 12; ++step) app.HandleCommand(IDM_COMPARE_BLEND_LESS);
        CHECK_EQ(app.m_comparison.amount, 0.0f);
        app.HandleCommand(IDM_COMPARE_ZOOM);
        CHECK_EQ(app.m_comparison.zoomScale, 2.0f);
        CHECK((GetMenuState(menu, IDM_COMPARE_ZOOM, MF_BYCOMMAND) & MF_CHECKED) != 0);
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
        app.HandleCommand(IDM_COMPARE_ZOOM);
        CHECK_EQ(app.m_comparison.zoomScale, 1.0f);
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
        const auto root = std::filesystem::temp_directory_path() / L"dlss5-stream-source-test";
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

    static void CheckNeuralSettingsDialog(PlayerApp& app)
    {
        app.m_neuralSettings = {}; app.m_renderGuides = {};
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
        // Guide switches persist for the next render and reach the live guide generator.
        CHECK(app.m_guides.Controls().depth);
        SendMessageW(GetDlgItem(dialog, IDC_NS_GUIDE_DEPTH), BM_SETCHECK, BST_UNCHECKED, 0);
        app.NeuralWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_NS_GUIDE_DEPTH, BN_CLICKED), 0);
        CHECK((app.m_renderGuides == GuideControls{true, false}));
        CHECK(!app.m_guides.Controls().depth);
        CHECK(app.m_guideReset && app.m_dlssReset);
        // Every control the dialog offers carries help text, and the text is the
        // localized tip rather than an empty tool.
        const auto tipHost = app.m_tipHosts.find(dialog);
        CHECK(tipHost != app.m_tipHosts.end());
        if (tipHost != app.m_tipHosts.end()) {
            const int tools = int(SendMessageW(tipHost->second, TTM_GETTOOLCOUNT, 0, 0));
            CHECK(tools >= 12);
            for (const int id : {IDC_NS_INTENSITY, IDC_NS_STRUCTURE, IDC_NS_TONE, IDC_NS_SKIN,
                                 IDC_NS_STYLE, IDC_NS_AUTOMASK, IDC_NS_GUIDE_MV, IDC_NS_GUIDE_DEPTH,
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
        }
        app.NeuralWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_NS_RESET, BN_CLICKED), 0);
        CHECK(app.m_neuralSettings == NeuralSettings{});
        CHECK(app.m_renderGuides.IsDefault());
        CHECK(app.m_guides.Controls().depth);
        CHECK_EQ(int(SendMessageW(GetDlgItem(dialog, IDC_NS_INTENSITY), TBM_GETPOS, 0, 0)), 100);
        CHECK_EQ(int(SendMessageW(GetDlgItem(dialog, IDC_NS_GUIDE_DEPTH), BM_GETCHECK, 0, 0)), BST_CHECKED);
        app.NeuralWndProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_NS_CLOSE, BN_CLICKED), 0);
        CHECK(app.m_neuralWnd == nullptr);
        CHECK(!IsWindow(dialog));
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
        RECT client{};
        CHECK(GetClientRect(dialog, &client) != FALSE);
        CHECK_EQ(int(client.right), PlayerApp::kEncoderDesignW);
        CHECK_EQ(int(client.bottom), PlayerApp::kEncoderDesignH);
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

int main()
{
    // The decoder describes a file through Media Foundation when no ffprobe
    // is beside it; the player starts both of these before its first window.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) ||
        FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        std::cerr << "Media Foundation could not start.\n";
        return EXIT_FAILURE;
    }
    PlayerAppTestAccess::Run();
    MFShutdown();
    CoUninitialize();
    std::cout << "Player UI regression failures: " << test_support::failure_count << '\n';
    return test_support::failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
