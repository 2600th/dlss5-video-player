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
        app.HandleCommand(IDM_QUALITY_PERFORMANCE);
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
    }

private:
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
