#include <windows.h>
#include <string>
#include <vector>
#include "TestSupport.h"

namespace {
std::vector<std::wstring> drawnText;
int messageBoxes = 0;

int WINAPI CaptureDrawText(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT format)
{
    drawnText.emplace_back(text, count < 0 ? std::wcslen(text) : static_cast<size_t>(count));
    return DrawTextW(dc, text, count, rect, format);
}

int WINAPI CaptureMessageBox(HWND, LPCWSTR, LPCWSTR, UINT)
{
    ++messageBoxes;
    return IDOK;
}
} // namespace

// Observe the Win32 boundary, leaving player layout and lifecycle code intact.
#define DrawTextW CaptureDrawText
#define MessageBoxW CaptureMessageBox
#include "../src/main.cpp"
#undef MessageBoxW
#undef DrawTextW

struct PlayerAppTestAccess {
    static void Run()
    {
        PlayerApp app(AppOptions{});
        CheckCacheSettings(app);
        // Saved playback choices must survive a reload, with invalid volume clamped.
        app.m_volume = 0.35f; app.m_muted = true; app.m_fill = true;
        app.m_neuralRequested = false; app.m_upscaleTargetHeight = 2160;
        app.m_youtubeSourceQuality = YouTubeSourceQuality::P1440;
        const auto savedCacheRoot=app.SettingsPath().parent_path()/L"shared-cache-location";
        app.m_cacheRoot=savedCacheRoot;
        app.SaveVideoSettings();
        app.m_cacheRoot.clear();
        app.m_volume = 1.0f; app.m_muted = false; app.m_fill = false;
        app.m_neuralRequested = true; app.m_upscaleTargetHeight = 1440;
        app.m_youtubeSourceQuality = YouTubeSourceQuality::Auto;
        app.LoadVideoSettings();
        CHECK_EQ(app.m_cacheRoot,savedCacheRoot);
        CHECK(std::abs(app.m_volume - 0.35f) < 0.001f);
        CHECK(app.m_muted && app.m_fill && !app.m_neuralRequested);
        CHECK_EQ(app.m_upscaleTargetHeight, 2160u);
        CHECK(app.m_youtubeSourceQuality == YouTubeSourceQuality::P1440);
        app.WriteIniFloat(L"Playback", L"Volume", 2.0f);
        app.LoadVideoSettings();
        CHECK_EQ(app.m_volume, 1.0f);
        app.m_muted = false; app.m_fill = false; app.m_neuralRequested = true;
        app.m_upscaleTargetHeight = 1440; app.m_youtubeSourceQuality = YouTubeSourceQuality::Auto;
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
        app.m_recent.reset();app.m_pendingCacheEvictions.clear();
        std::filesystem::remove(recentFile);

        // This must remain a synchronized neural/original comparison, never an
        // ambiguous label for runtime Super Resolution.
        const auto comparisonContent = app.ButtonContent(ToolbarAction::ToggleNeuralRendering);
        CHECK(comparisonContent.label.find(L"Neural Rendering") != std::wstring::npos);
        CHECK(comparisonContent.label.find(L"DLSS") == std::wstring::npos);
        CHECK(!comparisonContent.enabled);
        CHECK(!comparisonContent.active);
        CHECK(app.m_neuralRequested);
        CHECK(!app.m_upscalingRequested);
        CHECK_EQ(app.m_upscaleTargetHeight,1440u);
        app.HandleCommand(IDM_UPSCALE_2160);
        CHECK_EQ(app.m_upscaleTargetHeight,2160u);
        CHECK(!app.m_upscalingRequested);
        app.HandleCommand(IDM_UPSCALE_1440);
        CHECK_EQ(app.m_upscaleTargetHeight,1440u);
        const auto upscalingContent = app.ButtonContent(ToolbarAction::ToggleUpscaling);
        CHECK_EQ(std::wstring(L"DLSS Upscaling · Unavailable"), upscalingContent.label);
        CHECK(!upscalingContent.enabled);
        const auto frameGenerationContent = app.ButtonContent(ToolbarAction::ToggleFrameGeneration);
        CHECK_EQ(std::wstring(L"Frame Generation · Unavailable"), frameGenerationContent.label);
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
        const std::wstring cachedStatusBeforeFeatureStatusFix = app.BuildStatusText();
        CHECK(cachedStatusBeforeFeatureStatusFix.find(L"DLSS SR unavailable") != std::wstring::npos);
        CHECK(cachedStatusBeforeFeatureStatusFix.find(L"FG unavailable") != std::wstring::npos);

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
        CHECK_EQ(std::wstring(L"Neural Rendering · Seeking · On"), seekingOnContent.label);
        CHECK(seekingOnContent.active);
        CHECK((GetMenuState(featureMenu, IDM_NEURAL_RENDERING, MF_BYCOMMAND) & MFS_CHECKED) != 0);
        app.m_neuralRequested = false;
        app.m_comparisonView = ComparisonView::Original;
        app.SyncFeatureMenuState();
        const auto seekingOffContent = app.ButtonContent(ToolbarAction::ToggleNeuralRendering);
        CHECK_EQ(std::wstring(L"Neural Rendering · Seeking · Off"), seekingOffContent.label);
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
        CHECK_EQ(std::wstring(L"Neural Rendering · Preparing cache"), preparingContent.label);

        struct FeatureLabel { UiIcon icon; const std::wstring& label; int widthDip; };
        const std::array featureLabels{
            FeatureLabel{preparingContent.icon, preparingContent.label, 270},
            FeatureLabel{seekingOffContent.icon, seekingOffContent.label, 270},
            FeatureLabel{upscalingContent.icon, upscalingContent.label, 230},
            FeatureLabel{frameGenerationContent.icon, frameGenerationContent.label, 320},
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
        drawnText.clear();
        app.RenderUi(dc, RECT{0, 0, 800, 600});
        CHECK(Contains(L"1920 \u00d7 1080"));
        CHECK(Contains(L"Preparing encoder\u2026"));
        CHECK(Contains(L"Elapsed 00:00 \u00b7 ETA 00:05"));
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
        CHECK(DeleteDC(dc));

        // Cancelled and failed completions must restore the native menu, not
        // merely make the toolbar's computed availability true again.
        CompleteTerminalJob(app, true);
        CompleteTerminalJob(app, false);
        CHECK_EQ(1, messageBoxes);

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
        app.m_upscaleTargetHeight=2160;
        app.SyncFeatureMenuState();
        MoveFullscreenPointer(app,app.m_hwnd);
        CHECK((GetMenuState(menu,IDM_UPSCALE_2160,MF_BYCOMMAND)&MF_CHECKED)!=0);

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
        MoveFullscreenPointer(app,app.m_hwnd);
        app.m_fullscreenLastInput=Clock::now()-std::chrono::seconds(3);
        Sleep(300);
        MSG timerMessage{};bool receivedTimer=false;
        while(PeekMessageW(&timerMessage,app.m_hwnd,WM_TIMER,WM_TIMER,PM_REMOVE)){
            receivedTimer|=timerMessage.wParam==timer;
            DispatchMessageW(&timerMessage);
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

    static uint64_t QueueCompletion(PlayerApp& app, uint64_t generation, bool cancelled)
    {
        auto completion = std::make_unique<NeuralJobCompletion>();
        completion->generation = generation;
        completion->result.cancelled = cancelled;
        completion->result.detail = L"Controlled render failure";
        uint64_t token = 0;
        CHECK(app.m_neuralCompletions.RegisterAndPost(std::move(completion),
            [&](uint64_t registered) { token = registered; return true; }));
        return token;
    }

    static void CompleteTerminalJob(PlayerApp& app, bool cancelled)
    {
        const uint64_t generation = app.m_neuralLifecycle.Begin();
        app.m_neuralWorker = std::jthread([] {});
        app.SyncSourceActionAvailability();
        CheckSourceMenus(app, false);
        const uint64_t token = QueueCompletion(app, generation, cancelled);
        app.CompleteNeuralJob(token);
        CHECK(!app.NeuralJobActive());
        CHECK(!app.m_neuralWorker.joinable());
        CHECK_EQ(cancelled ? NeuralPlaybackState::OriginalOnly : NeuralPlaybackState::Failed,
                 app.m_neuralLifecycle.state);
        CheckSourceMenus(app, true);
    }
};

int main()
{
    PlayerAppTestAccess::Run();
    std::cout << "Player UI regression failures: " << test_support::failure_count << '\n';
    return test_support::failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
