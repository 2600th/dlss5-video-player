#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <wrl/client.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>
#include <algorithm>
#include <memory>
#include <cmath>
#include <utility>
#include <iterator>
#include <cstdint>
#include <vector>
#include <cwctype>
#include <cstdlib>
#include <optional>
#include <functional>
#include <thread>
#include <system_error>
#include "VideoDecoder.h"
#include "D3D12Renderer.h"
#include "TemporalGuides.h"
#include "AudioPlayer.h"
#include "Localization.h"
#include "AppMenu.h"
#include "UiLayout.h"
#include "UiResources.h"
#include "Log.h"
#include "ReShadeConfig.h"
#include "RuntimePolicy.h"
#include "RuntimeLifetime.h"
#include "YouTubeResolver.h"
#include "ExampleVideos.h"
#include "CompletionRegistry.h"
#include "NetworkMediaTransaction.h"
#include "PlaybackTiming.h"
#include "LiveSessionPolicy.h"
#include "NeuralCache.h"
#include "RecentMedia.h"
#include "MediaPipeline.h"
#include "OfflineNeuralRenderer.h"
#include "NeuralWorker.h"
#include "NeuralPreflight.h"
#include "NeuralReceipt.h"
#include "NeuralSettings.h"
#include "RangeSelection.h"
#include "RuntimeLock.h"
#include "UpscalingPolicy.h"
#include "SynchronizedPlayback.h"
#include "resources.h"

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
using namespace app_menu;
static constexpr int CONTROL_H_DIP = 112;
// Older source entries could be truncated despite a successful FFmpeg exit.
static constexpr const char* kCompleteSourcePolicy = "source-complete-v5-highest-bitrate";

static UINT ActiveWindowDpi(HWND window)
{
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (getDpiForWindow) {
        const UINT dpi = getDpiForWindow(window);
        if (dpi != 0) return dpi;
    }

    HDC dc = GetDC(window);
    if (!dc) return USER_DEFAULT_SCREEN_DPI;
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(window, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : USER_DEFAULT_SCREEN_DPI;
}

static void EnablePerMonitorDpiAwareness()
{
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    static const auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (!setContext || !setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();
    }
}

static POINT MinimumPlayerWindowTrackSize(HWND window, UINT dpi)
{
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    const HMENU menu = GetMenu(window);
    const BOOL hasMenu = menu != nullptr;
    int minimumClientWidth = MinimumToolbarClientWidth(dpi);
    if (menu) {
        int menuWidth = 0;
        const int itemCount = GetMenuItemCount(menu);
        for (int index = 0; index < itemCount; ++index) {
            RECT item{};
            if (GetMenuItemRect(window, menu, static_cast<UINT>(index), &item)) {
                menuWidth += item.right - item.left;
            }
        }
        // AdjustWindowRectExForDpi accounts for one menu row, not a wrapped menu.
        // Keep the top-level menu on one row so its non-client conversion stays exact.
        minimumClientWidth = std::max(minimumClientWidth,
                                      menuWidth + MulDiv(8, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI));
    }
    const int minimumClientHeight = MinimumIdleClientHeight(dpi);
    const RECT client{0, 0, minimumClientWidth, minimumClientHeight};
    RECT outer = client;

    using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const auto adjustForDpi = reinterpret_cast<AdjustWindowRectExForDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
    BOOL adjusted = adjustForDpi && adjustForDpi(&outer, style, hasMenu, exStyle, dpi);
    if (!adjusted) {
        outer = client;
        adjusted = AdjustWindowRectEx(&outer, style, hasMenu, exStyle);
    }
    if (!adjusted) return POINT{minimumClientWidth, minimumClientHeight};
    return POINT{outer.right - outer.left, outer.bottom - outer.top};
}

static const wchar_t* kVideoPatterns =
    L"*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi;*.wmv;*.asf;*.flv;*.f4v;"
    L"*.ts;*.m2ts;*.mts;*.mpg;*.mpeg;*.mpe;*.vob;*.ogv;*.ogg;*.3gp;*.3g2;"
    L"*.mxf;*.nut;*.rm;*.rmvb;*.divx;*.dv;*.y4m;*.ivf;*.hevc;*.h265;*.h264;*.264;*.av1;*.vp9;"
    L"*.gif;*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.webp";

static constexpr int HK_PLAY_PAUSE = 9001;
static constexpr int HK_BACK_10 = 9002;
static constexpr int HK_FORWARD_10 = 9003;
static constexpr int HK_MUTE = 9004;
static constexpr int HK_DLSS = 9005;
static constexpr int HK_MEDIA_PLAY_PAUSE = 9006;
static constexpr int HK_ADJUSTMENTS = 9007;

static constexpr int IDC_ADJ_BRIGHTNESS = 7101;
static constexpr int IDC_ADJ_CONTRAST = 7102;
static constexpr int IDC_ADJ_SATURATION = 7103;
static constexpr int IDC_ADJ_GAMMA = 7104;
static constexpr int IDC_ADJ_TEMPERATURE = 7105;
static constexpr int IDC_ADJ_TINT = 7106;
static constexpr int IDC_ADJ_RESET = 7110;
static constexpr int IDC_ADJ_CLOSE = 7111;

static constexpr int IDC_NS_INTENSITY = 7301;
static constexpr int IDC_NS_STRUCTURE = 7302;
static constexpr int IDC_NS_TONE = 7303;
static constexpr int IDC_NS_SKIN = 7304;
// 7305 and 7306 were the colour-strength slider and the render-preset combo,
// both removed after measurement showed the runtime ignores them.
static constexpr int IDC_NS_STYLE = 7307;
static constexpr int IDC_NS_AUTOMASK = 7308;
static constexpr int IDC_NS_GUIDE_MV = 7311;
static constexpr int IDC_NS_GUIDE_DEPTH = 7312;
static constexpr int IDC_NS_RESET = 7320;
static constexpr int IDC_NS_APPLY = 7321;
static constexpr int IDC_NS_CLOSE = 7322;

static constexpr int IDC_TIMECODE_EDIT = 7501;
static constexpr int IDC_TIMECODE_SET_IN = 7502;
static constexpr int IDC_TIMECODE_SET_OUT = 7503;
static constexpr int IDC_TIMECODE_ERROR = 7504;

static constexpr int IDC_YOUTUBE_URL = 7201;
static constexpr int IDC_YOUTUBE_PASTE = 7202;
static constexpr int IDC_YOUTUBE_NOTE = 7203;
static constexpr int IDC_YOUTUBE_ERROR = 7204;
static constexpr UINT WM_YOUTUBE_RESOLVED = WM_APP + 41;
static constexpr UINT WM_NEURAL_PROGRESS = WM_APP + 42;
static constexpr UINT WM_NEURAL_COMPLETE = WM_APP + 43;
static constexpr UINT WM_EXPORT_COMPLETE = WM_APP + 44;

struct YouTubeUrlDialogState {
    const Localizer* localizer{};
    HFONT font{};
    HWND edit{};
    HWND error{};
    bool accepted{false};
    bool done{false};
    std::wstring url;
};

static int DialogDip(HWND window, int value)
{
    return MulDiv(value, static_cast<int>(ActiveWindowDpi(window)), USER_DEFAULT_SCREEN_DPI);
}

static std::wstring ReadWindowText(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

static void SetControlFont(HWND control, HFONT font)
{
    if (control && font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

static void PasteClipboardText(HWND edit)
{
    if (!edit || !OpenClipboard(edit)) return;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
        const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
        if (text) {
            SetWindowTextW(edit, text);
            SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
}

static LRESULT CALLBACK YouTubeUrlDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<YouTubeUrlDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<YouTubeUrlDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE: {
        const int pad = DialogDip(window, 20);
        const int labelHeight = DialogDip(window, 22);
        const int editHeight = DialogDip(window, 30);
        const int buttonWidth = DialogDip(window, 82);
        const int buttonHeight = DialogDip(window, 32);
        RECT client{};
        GetClientRect(window, &client);
        HWND label = CreateWindowExW(0, L"STATIC", state->localizer->Get(L"youtube.dialog.url").c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT, pad, pad, client.right - 2 * pad, labelHeight,
            window, nullptr, nullptr, nullptr);
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            pad, pad + labelHeight, client.right - 3 * pad - buttonWidth, editHeight,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_YOUTUBE_URL)), nullptr, nullptr);
        SendMessageW(state->edit, EM_SETLIMITTEXT, 2048, 0);
        HWND paste = CreateWindowExW(0, L"BUTTON", state->localizer->Get(L"youtube.dialog.paste").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            client.right - pad - buttonWidth, pad + labelHeight, buttonWidth, editHeight,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_YOUTUBE_PASTE)), nullptr, nullptr);
        HWND note = CreateWindowExW(0, L"STATIC", state->localizer->Get(L"youtube.dialog.note").c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT, pad, pad + labelHeight + editHeight + DialogDip(window, 10),
            client.right - 2 * pad, labelHeight, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_YOUTUBE_NOTE)), nullptr, nullptr);
        state->error = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
            pad, pad + labelHeight + editHeight + DialogDip(window, 38), client.right - 2 * pad,
            DialogDip(window, 38), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_YOUTUBE_ERROR)), nullptr, nullptr);
        HWND play = CreateWindowExW(0, L"BUTTON", state->localizer->Get(L"youtube.dialog.play").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            client.right - pad - buttonWidth * 2 - DialogDip(window, 10), client.bottom - pad - buttonHeight,
            buttonWidth, buttonHeight, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), nullptr, nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", state->localizer->Get(L"youtube.dialog.cancel").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            client.right - pad - buttonWidth, client.bottom - pad - buttonHeight,
            buttonWidth, buttonHeight, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), nullptr, nullptr);
        for (HWND control : {label, state->edit, paste, note, state->error, play, cancel}) {
            SetControlFont(control, state->font);
        }
        SetFocus(state->edit);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_YOUTUBE_PASTE) {
            PasteClipboardText(state->edit);
            SetWindowTextW(state->error, L"");
            SetFocus(state->edit);
            return 0;
        }
        if (LOWORD(wParam) == IDOK) {
            HWND edit = GetDlgItem(window, IDC_YOUTUBE_URL);
            const std::wstring candidate = ReadWindowText(edit);
            const bool supported = IsSupportedYouTubeUrl(candidate);
            LOG("YouTube URL validation " << (supported ? "accepted." : "rejected."));
            if (!supported) {
                SetWindowTextW(state->error, state->localizer->Get(L"youtube.dialog.invalid").c_str());
                SetFocus(edit);
                SendMessageW(edit, EM_SETSEL, 0, static_cast<LPARAM>(-1));
                return 0;
            }
            state->url = candidate;
            state->accepted = true;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if (reinterpret_cast<HWND>(lParam) == state->error) {
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(180, 36, 36));
            SetBkColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        state->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// Registers `windowProc` under `className` once, creates a centered owned
// popup of `clientWidth` x `clientHeight` DIPs and runs a nested message loop
// until `done` is set. Enter presses the default button (IDOK) and Escape
// cancels; the owner is disabled for the duration.
static bool RunOwnedModalDialog(HWND owner, const wchar_t* className, WNDPROC windowProc, const wchar_t* title,
                                int clientWidth, int clientHeight, void* state, HWND& edit, const bool& done)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW dialogClass{};
    dialogClass.lpfnWndProc = windowProc;
    dialogClass.hInstance = instance;
    dialogClass.lpszClassName = className;
    dialogClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    dialogClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&dialogClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    RECT bounds{0, 0, DialogDip(owner, clientWidth), DialogDip(owner, clientHeight)};
    AdjustWindowRectEx(&bounds, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                       WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT);
    RECT ownerBounds{};
    GetWindowRect(owner, &ownerBounds);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const int ownerWidth = static_cast<int>(ownerBounds.right - ownerBounds.left);
    const int ownerHeight = static_cast<int>(ownerBounds.bottom - ownerBounds.top);
    const int x = static_cast<int>(ownerBounds.left) + std::max(0, (ownerWidth - width) / 2);
    const int y = static_cast<int>(ownerBounds.top) + std::max(0, (ownerHeight - height) / 2);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, className,
        title, WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height,
        owner, nullptr, instance, state);
    if (!dialog) return false;

    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);
    SetFocus(edit);
    MSG message{};
    bool repostQuit = false;
    int quitCode = 0;
    while (!done) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            if (result == 0) {
                repostQuit = true;
                quitCode = static_cast<int>(message.wParam);
            }
            if (IsWindow(dialog)) DestroyWindow(dialog);
            break;
        }
        if (message.message == WM_KEYDOWN &&
            (message.hwnd == dialog || IsChild(dialog, message.hwnd))) {
            if (message.wParam == VK_RETURN) {
                SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
                             reinterpret_cast<LPARAM>(edit));
                continue;
            }
            if (message.wParam == VK_ESCAPE) {
                SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                continue;
            }
        }
        // Activating the disabled owner (Alt+Tab) still routes key messages to
        // it; the player's accelerators must not fire behind the dialog.
        if (message.message >= WM_KEYFIRST && message.message <= WM_KEYLAST &&
            (message.hwnd == owner || IsChild(owner, message.hwnd))) {
            SetFocus(edit);
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    SetFocus(owner);
    if (repostQuit) PostQuitMessage(quitCode);
    return true;
}

static std::optional<std::wstring> PromptForYouTubeUrl(HWND owner, const Localizer& localizer,
                                                       HFONT font)
{
    YouTubeUrlDialogState state{&localizer, font};
    const std::wstring title = localizer.Get(L"youtube.dialog.title");
    if (!RunOwnedModalDialog(owner, L"DLSSVideoYouTubeUrlDialogV1", YouTubeUrlDialogProc, title.c_str(),
                             520, 220, &state, state.edit, state.done)) return std::nullopt;
    if (!state.accepted) return std::nullopt;
    return state.url;
}

enum class TimecodeAction { Go, SetIn, SetOut };

struct TimecodeDialogState {
    const Localizer* localizer{};
    HFONT font{};
    std::wstring initial;
    // Returns false when the text is not a timecode; the dialog then stays open.
    std::function<bool(const std::wstring&, TimecodeAction)> apply;
    HWND edit{};
    HWND error{};
    bool done{false};
};

static LRESULT CALLBACK TimecodeDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<TimecodeDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<TimecodeDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE: {
        const int pad = DialogDip(window, 20);
        const int labelHeight = DialogDip(window, 22);
        const int editHeight = DialogDip(window, 30);
        const int buttonWidth = DialogDip(window, 82);
        const int buttonHeight = DialogDip(window, 32);
        const int gap = DialogDip(window, 10);
        RECT client{};
        GetClientRect(window, &client);
        HWND label = CreateWindowExW(0, L"STATIC", state->localizer->Get(L"timecode.label").c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT, pad, pad, client.right - 2 * pad, labelHeight,
            window, nullptr, nullptr, nullptr);
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state->initial.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            pad, pad + labelHeight, client.right - 2 * pad, editHeight,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TIMECODE_EDIT)), nullptr, nullptr);
        SendMessageW(state->edit, EM_SETLIMITTEXT, 64, 0);
        state->error = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
            pad, pad + labelHeight + editHeight + gap, client.right - 2 * pad, DialogDip(window, 38), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TIMECODE_ERROR)), nullptr, nullptr);
        const int buttonTop = client.bottom - pad - buttonHeight;
        int right = client.right - pad;
        const auto button = [&](const wchar_t* key, int id, DWORD style) {
            HWND control = CreateWindowExW(0, L"BUTTON", state->localizer->Get(key).c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, right - buttonWidth, buttonTop, buttonWidth, buttonHeight,
                window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
            right -= buttonWidth + gap;
            return control;
        };
        HWND cancel = button(L"timecode.cancel", IDCANCEL, BS_PUSHBUTTON);
        HWND setOut = button(L"timecode.set_out", IDC_TIMECODE_SET_OUT, BS_PUSHBUTTON);
        HWND setIn = button(L"timecode.set_in", IDC_TIMECODE_SET_IN, BS_PUSHBUTTON);
        HWND go = button(L"timecode.go", IDOK, BS_DEFPUSHBUTTON);
        for (HWND control : {label, state->edit, state->error, go, setIn, setOut, cancel}) {
            SetControlFont(control, state->font);
        }
        SendMessageW(state->edit, EM_SETSEL, 0, static_cast<LPARAM>(-1));
        SetFocus(state->edit);
        return 0;
    }
    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        if (id == IDCANCEL) { DestroyWindow(window); return 0; }
        if (id != IDOK && id != IDC_TIMECODE_SET_IN && id != IDC_TIMECODE_SET_OUT) break;
        const TimecodeAction action = id == IDOK ? TimecodeAction::Go : id == IDC_TIMECODE_SET_IN ? TimecodeAction::SetIn : TimecodeAction::SetOut;
        if (!state->apply(ReadWindowText(state->edit), action)) {
            SetWindowTextW(state->error, state->localizer->Get(L"timecode.invalid").c_str());
            SetFocus(state->edit);
            SendMessageW(state->edit, EM_SETSEL, 0, static_cast<LPARAM>(-1));
            return 0;
        }
        DestroyWindow(window);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        if (reinterpret_cast<HWND>(lParam) == state->error) {
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(180, 36, 36));
            SetBkColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        state->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

static void PromptForTimecode(HWND owner, const Localizer& localizer, HFONT font, std::wstring initial,
                              std::function<bool(const std::wstring&, TimecodeAction)> apply)
{
    TimecodeDialogState state{&localizer, font, std::move(initial), std::move(apply)};
    const std::wstring title = localizer.Get(L"timecode.title");
    RunOwnedModalDialog(owner, L"DLSSVideoTimecodeDialogV1", TimecodeDialogProc, title.c_str(),
                        440, 190, &state, state.edit, state.done);
}

struct YouTubeCompletion {
    uint64_t generation{};
    ResolveResult result;
    std::wstring displayTitle;
    std::wstring pageUrl;
    std::wstring mediaErrorKey;
    std::unique_ptr<VideoDecoder> decoder;
    std::unique_ptr<AudioPlayer> audio;
    VideoFrame firstFrame;
    NetworkRenderConfiguration configuration;
    NetworkCommitKind commitKind{NetworkCommitKind::InitialOpen};
    YouTubeSourceQuality sourceQuality{YouTubeSourceQuality::Auto};
    bool requestedQualityExplicit{false};
    NVSDK_NGX_PerfQuality_Value requestedQuality{DefaultNeuralCarrierQuality()};
    bool resumeAfterSeek{true};
    double seekSeconds{0.0};
};

struct NeuralProgressMessage {
    uint64_t generation{};
    NeuralRenderProgress progress;
    uint32_t width{};
    uint32_t height{};
};

struct NeuralJobCompletion {
    uint64_t generation{};
    NeuralRenderResult result;
    std::filesystem::path sourcePath;
    std::filesystem::path neuralPath;
    std::wstring displayTitle;
    std::wstring pageUrl;
    YouTubeSourceQuality sourceQuality{YouTubeSourceQuality::Auto};
    MediaSourceKind sourceKind{MediaSourceKind::LocalFile};
    bool cacheHit{};
    bool cachedSourceUnavailable{};
    // A prepared open stops after the cache lookup: it acquires and identifies
    // the source, then hands the original to the player without rendering.
    bool preparedOnly{};
    std::string sourceKey, renderKey;
    NeuralRenderRange range;
    // The settings and guides the render identity was built from.
    NeuralSettings settings;
    GuideControls guides;
    std::filesystem::path receiptPath;
};

struct ExportCompletion {
    MaterializeResult result;
    std::filesystem::path output;
};

struct PreparedRendererCandidate {
    HWND window{};
    D3D12RendererOwner renderer;
    TemporalGuideGenerator guides;
    NetworkRenderConfiguration configuration;
    ~PreparedRendererCandidate(){renderer.reset();if(window)DestroyWindow(window);}
};

struct AppOptions {
    uint32_t maxW=3840, maxH=2160;
    bool outputExplicit=false;
    NVSDK_NGX_PerfQuality_Value quality=DefaultNeuralCarrierQuality();
    bool qualityExplicit=true;
    bool safeMode=false;
    bool addonBootstrapRestarted=false;
    bool neuralAddonRequested=false;
    bool neuralAddonConfigured=false;
    bool argumentsOk=false;
    DetectedGpu detectedGpu;
    std::vector<std::wstring> userArguments;
    std::wstring argumentError;
    std::wstring file;
};

static AppOptions ParseArgs() {
    AppOptions o; int argc=0; LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    const RuntimeArguments runtimeArguments=ParseRuntimeArguments(argc,argv);
    if(argv)LocalFree(argv);
    if(!runtimeArguments.ok){o.argumentError=runtimeArguments.error;return o;}
    o.argumentsOk=true;
    o.safeMode=runtimeArguments.safeMode;
    o.addonBootstrapRestarted=runtimeArguments.addonBootstrapRestarted;
    o.userArguments=runtimeArguments.userArguments;
    for(size_t i=0;i<o.userArguments.size();++i) {
        const std::wstring& a=o.userArguments[i];
        if(a==L"--safe-mode") {
            continue;
        } else if(a==L"--output" && i+1<o.userArguments.size()) {
            std::wstring v=o.userArguments[++i]; auto x=v.find(L'x'); if(x==std::wstring::npos) x=v.find(L'X');
            if(x!=std::wstring::npos) { o.maxW=std::max(64,_wtoi(v.substr(0,x).c_str())); o.maxH=std::max(64,_wtoi(v.substr(x+1).c_str())); o.outputExplicit=true; }
        } else if(a==L"--quality") {
            o.argumentsOk=false;o.argumentError=L"The legacy --quality option was removed. Neural rendering preserves source resolution; choose Super Resolution output in the DLSS menu.";return o;
        } else if(!a.empty() && a[0]!=L'-') o.file=a;
    }
    return o;
}

enum class StartupResult { Continue, ExitSuccess, ExitFailure };

static std::string WideToUtf8(std::wstring_view value) {
    if(value.empty()) return {};
    const int size=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(size<=0) return "<wide-string conversion failed>";
    std::string result(static_cast<size_t>(size),'\0');
    if(WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),size,nullptr,nullptr)!=size) return "<wide-string conversion failed>";
    return result;
}

static std::wstring Win32Error(std::wstring_view operation) {
    return std::wstring(operation)+L" failed (Win32 error "+std::to_wstring(GetLastError())+L")";
}

static bool CurrentExecutablePath(std::filesystem::path& executable,std::wstring& error) {
    std::wstring path(32768,L'\0');
    const DWORD length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(length==0 || length>=path.size()) { error=Win32Error(L"Resolving the player executable"); return false; }
    path.resize(length); executable=std::filesystem::path(path);
    if(!executable.is_absolute()) { error=L"The player executable path was not absolute"; return false; }
    return true;
}

static bool LaunchSameExecutable(const std::vector<std::wstring>& arguments,std::wstring& error) {
    std::filesystem::path executable;
    if(!CurrentExecutablePath(executable,error)) return false;
    std::wstring commandLine=BuildWindowsCommandLine(executable.native(),arguments);
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    if(!CreateProcessW(executable.c_str(),commandLine.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)) {
        error=Win32Error(L"Starting the player"); return false;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return true;
}

static StartupResult FailBootstrap(std::wstring_view technicalError) {
    LOG("Neural addon bootstrap failed: " << WideToUtf8(technicalError));
    MessageBoxW(nullptr,L"The experimental neural add-on could not be configured safely. The player will close.\n\nSee DLSSVideoPlayer.log for details.",L"DLSS Video Player",MB_OK|MB_ICONERROR);
    return StartupResult::ExitFailure;
}

static StartupResult RunNeuralAddonBootstrap(AppOptions& options) {
    std::filesystem::path executable;
    std::wstring pathError;
    if(!CurrentExecutablePath(executable,pathError)) return FailBootstrap(pathError);

    options.detectedGpu=DetectHighPerformanceGpu();
    if(std::filesystem::exists(executable.parent_path()/L"dxgi.dll"))
        return FailBootstrap(L"Old runtime layout: extract the new build to a separate folder; dxgi.dll must only be inside neural-runtime.");
    const std::filesystem::path runtimeDirectory=executable.parent_path()/L"neural-runtime";
    bool layoutInspectionFailed=false;
    const auto isRegularRuntimeFile=[&](const wchar_t* name) {
        const std::filesystem::path file=runtimeDirectory/name;
        const DWORD attributes=GetFileAttributesW(file.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES)
            return (attributes&FILE_ATTRIBUTE_DIRECTORY)==0;
        const DWORD error=GetLastError();
        if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)
            layoutInspectionFailed=true;
        return false;
    };
    const bool hasConfig=isRegularRuntimeFile(L"ReShade.ini");
    const bool hasProxy=isRegularRuntimeFile(L"dxgi.dll");
    const bool hasAddon=isRegularRuntimeFile(L"renodx-dlss5.addon64");
    const bool hasRuntime=isRegularRuntimeFile(L"nvngx_dlssnr.dll");
    if(layoutInspectionFailed) return FailBootstrap(L"Unable to inspect the experimental neural runtime layout");

    const NeuralRuntimeLayout layout=ClassifyNeuralRuntimeLayout(
        hasConfig,hasProxy,hasAddon,hasRuntime);
    if(layout==NeuralRuntimeLayout::Absent) {
        options.neuralAddonRequested=false;
        options.neuralAddonConfigured=false;
        LOG("Experimental neural runtime absent; continuing in native DLAA developer mode.");
        return StartupResult::Continue;
    }
    if(layout==NeuralRuntimeLayout::Incomplete || !isRegularRuntimeFile(L"NeuralWorker.exe"))
        return FailBootstrap(L"The experimental neural runtime is incomplete; require ReShade.ini, dxgi.dll, renodx-dlss5.addon64, and nvngx_dlssnr.dll together");

    options.neuralAddonRequested=NeuralAddonDesired(options.detectedGpu.generation,options.safeMode);
    options.neuralAddonConfigured=options.neuralAddonRequested;
    LOG("Isolated neural helper available; player remains hook-free. GPU=" << WideToUtf8(options.detectedGpu.description));
    return StartupResult::Continue;
}

static std::wstring PickVideoFileFallback(HWND owner, const Localizer& loc) {
    wchar_t path[32768]{};
    std::wstring filter;
    filter += loc.Get(L"dialog.all_ffmpeg"); filter.push_back(L'\0');
    filter += L"*.*"; filter.push_back(L'\0');
    filter += loc.Get(L"dialog.supported"); filter.push_back(L'\0');
    filter += kVideoPatterns; filter.push_back(L'\0');
    filter += loc.Get(L"dialog.all"); filter.push_back(L'\0');
    filter += L"*.*"; filter.push_back(L'\0'); filter.push_back(L'\0');
    const std::wstring title = loc.Get(L"dialog.title");
    OPENFILENAMEW o{}; o.lStructSize=sizeof(o); o.hwndOwner=owner; o.lpstrFile=path; o.nMaxFile=static_cast<DWORD>(std::size(path));
    o.lpstrFilter=filter.c_str(); o.nFilterIndex=1; o.lpstrTitle=title.c_str();
    o.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_EXPLORER|OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&o)?path:L"";
}

static std::filesystem::path PickExportFile(HWND owner, std::wstring_view title, bool photo, bool animation) {
    wchar_t path[32768]{};
    std::wstring suggested(title.empty()?L"neural-video":std::wstring(title));
    for(wchar_t& c:suggested)if(c==L'<'||c==L'>'||c==L':'||c==L'"'||c==L'/'||c==L'\\'||c==L'|'||c==L'?'||c==L'*')c=L'_';
    suggested+=L"-neural";wcsncpy_s(path,suggested.c_str(),_TRUNCATE);
    const wchar_t filter[]=L"Matroska video (*.mkv)\0*.mkv\0MP4 video (*.mp4)\0*.mp4\0Animated GIF (*.gif)\0*.gif\0PNG photo (*.png)\0*.png\0JPEG photo (*.jpg)\0*.jpg;*.jpeg\0\0";
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=owner;dialog.lpstrFile=path;dialog.nMaxFile=static_cast<DWORD>(std::size(path));dialog.lpstrFilter=filter;dialog.nFilterIndex=photo?4:animation?3:1;dialog.lpstrDefExt=photo?L"png":animation?L"gif":L"mkv";dialog.lpstrTitle=L"Save the converted video to a new file";dialog.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST;
    return GetSaveFileNameW(&dialog)?std::filesystem::path(path):std::filesystem::path{};
}

static std::wstring PickVideoFile(HWND owner, const Localizer& loc) {
    ComPtr<IFileOpenDialog> dlg;
    HRESULT hr=CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dlg));
    if(SUCCEEDED(hr) && dlg) {
        const std::wstring allFfmpeg=loc.Get(L"dialog.all_ffmpeg"), supported=loc.Get(L"dialog.supported"), all=loc.Get(L"dialog.all"), title=loc.Get(L"dialog.title");
        COMDLG_FILTERSPEC specs[3]={{allFfmpeg.c_str(),L"*.*"},{supported.c_str(),kVideoPatterns},{all.c_str(),L"*.*"}};
        dlg->SetFileTypes(3,specs); dlg->SetFileTypeIndex(1); dlg->SetTitle(title.c_str());
        FILEOPENDIALOGOPTIONS opts{}; if(SUCCEEDED(dlg->GetOptions(&opts))) dlg->SetOptions(opts|FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_PATHMUSTEXIST);
        hr=dlg->Show(owner);
        if(hr==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return L"";
        if(SUCCEEDED(hr)) {
            ComPtr<IShellItem> item; if(SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR p=nullptr; if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&p)) && p) {
                    std::wstring result(p); CoTaskMemFree(p); return result;
                }
            }
        }
    }
    return PickVideoFileFallback(owner,loc);
}

static std::wstring TimeText(double sec) {
    if(!std::isfinite(sec)||sec<0) sec=0; int s=int(sec+0.5),h=s/3600; s%=3600; int m=s/60; s%=60; wchar_t b[64];
    if(h) swprintf_s(b,L"%d:%02d:%02d",h,m,s); else swprintf_s(b,L"%02d:%02d",m,s); return b;
}

// One complete local copy of a YouTube source in the cache: identity, lookup,
// download, validation and promotion. A render and the background acquisition
// that starts when a range is marked both go through this, so they can never
// disagree about the key or download the same source twice.
struct SourceAcquisition {
    std::string key;
    std::filesystem::path path;   // empty when nothing was acquired
    std::wstring detail;
    bool cancelled{};
};

// Published by the background acquisition; a render waits on `finished` and
// then reuses `key`.
struct SourcePrefetchState {
    std::atomic<bool> finished{false};
    std::string key;
};

static SourceAcquisition AcquireYouTubeSource(NeuralCacheManager& cache,
                                              const std::filesystem::path& moduleDirectory,
                                              const std::wstring& mediaUrl,
                                              const std::wstring& audioUrl,
                                              const std::wstring& pageUrl,
                                              YouTubeSourceQuality sourceQuality,
                                              double expectedDurationSeconds,
                                              const std::function<void()>& onDownloadStart,
                                              std::stop_token stop)
{
    SourceAcquisition result{};
    const std::string videoId=CanonicalYouTubeVideoId(pageUrl);
    const std::string streamIdentity=StableYouTubeStreamIdentity(mediaUrl,audioUrl);
    if(videoId.empty()||streamIdentity.empty()||!std::isfinite(expectedDurationSeconds)||expectedDurationSeconds<=0.0){
        result.detail=L"The YouTube source format or duration is unavailable.";return result;
    }
    NeuralCacheIdentity sourceIdentity{};
    sourceIdentity.sourceDigest="youtube="+videoId+"|quality="+std::to_string(static_cast<int>(sourceQuality))+"|"+streamIdentity;
    sourceIdentity.applicationVersion=DLSS_VIDEO_PLAYER_VERSION;sourceIdentity.quality=kCompleteSourcePolicy;
    result.key=BuildNeuralCacheKey(sourceIdentity);
    LOG("Checking source cache key="<<result.key);
    if(const auto cached=cache.LookupSource(result.key)){
        const ProbeResult cachedProbe=ProbeMedia(moduleDirectory,cached->payloadPath,stop,MediaProbeMode::CachedMetadata);
        if(stop.stop_requested()){result.cancelled=true;result.detail=L"Neural render was cancelled.";return result;}
        const int64_t durationTolerance=std::max<int64_t>(1,cached->manifest.duration100ns/static_cast<int64_t>(std::max<uint64_t>(1,cached->manifest.frameCount))+1);
        const bool valid=cachedProbe.ok&&cached->manifest.encoder==kCompleteSourcePolicy&&cachedProbe.width==cached->manifest.width&&cachedProbe.height==cached->manifest.height&&std::llabs(cachedProbe.duration100ns-cached->manifest.duration100ns)<=durationTolerance&&std::abs(cachedProbe.duration100ns/10000000.0-expectedDurationSeconds)<=1.0;
        if(valid){result.path=cached->payloadPath;LOG("Source cache hit: content hash and metadata verified; download skipped.");return result;}
        if(!cache.Quarantine(*cached)){result.detail=L"The invalid source cache entry could not be quarantined.";return result;}
    }
    LOG("Source cache miss or invalid entry; acquiring source.");
    if(onDownloadStart)onDownloadStart();
    auto staging=cache.BeginSourceStaging(result.key);
    if(!staging){result.detail=L"Source cache staging could not be created.";return result;}
    MaterializeRequest request{mediaUrl,audioUrl,*staging/L"source.mkv",expectedDurationSeconds};
    MediaMaterializer materializer(moduleDirectory);
    auto materialized=materializer.Run(request,stop);
    if(!materialized.ok&&materialized.error!=MaterializeError::Cancelled&&!pageUrl.empty()&&!stop.stop_requested()){
        // Resolved googlevideo URLs expire while a video plays. Re-resolve the
        // page once and retry the download.
        LOG("Source acquisition failed; re-resolving the page URL once.");
        YouTubeResolver refresher;const ResolveResult refreshed=refresher.Resolve(pageUrl,sourceQuality,stop);
        const std::string refreshedIdentity=refreshed.error==ResolveError::None?StableYouTubeStreamIdentity(refreshed.mediaUrl,refreshed.audioUrl):std::string{};
        if(!refreshedIdentity.empty()){
            // The identity names the selected formats, so a different one is a
            // different source: stage it under its own key instead of promoting
            // it as the first attempt's.
            NeuralCacheIdentity retryIdentity=sourceIdentity;
            retryIdentity.sourceDigest="youtube="+videoId+"|quality="+std::to_string(static_cast<int>(sourceQuality))+"|"+refreshedIdentity;
            const std::string retryKey=BuildNeuralCacheKey(retryIdentity);
            if(retryKey!=result.key){
                LOG("Re-resolved source identity changed; staging under key="<<retryKey);
                cache.MarkInvalid(*staging);
                auto retryStaging=cache.BeginSourceStaging(retryKey);
                if(!retryStaging){result.detail=L"Source cache staging could not be created.";return result;}
                staging=std::move(retryStaging);result.key=retryKey;
            }
            request.videoUrl=refreshed.mediaUrl;request.audioUrl=refreshed.audioUrl;request.output=*staging/L"source.mkv";
            materialized=materializer.Run(request,stop);
        }
    }
    if(!materialized.ok){cache.MarkInvalid(*staging);result.cancelled=materialized.error==MaterializeError::Cancelled;result.detail=materialized.detail;return result;}
    const ProbeResult sourceProbe=ProbeMedia(moduleDirectory,*staging/L"source.mkv",stop);
    NeuralCacheManifest sourceManifest{};sourceManifest.encoder=kCompleteSourcePolicy;sourceManifest.width=sourceProbe.width;sourceManifest.height=sourceProbe.height;
    sourceManifest.frameCount=sourceProbe.frameCount;sourceManifest.duration100ns=sourceProbe.duration100ns;
    if(!sourceProbe.ok||!cache.PromoteSource(result.key,*staging,sourceManifest)){cache.MarkInvalid(*staging);result.detail=L"The acquired source failed validation.";return result;}
    const auto promoted=cache.LookupSource(result.key);
    if(!promoted){result.detail=L"The acquired source was not reusable.";return result;}
    result.path=promoted->payloadPath;
    return result;
}

// How a neural job relates to what is on screen: an offline job replaces
// playback, a live session renders behind it, and a preview re-renders the one
// frame the player is paused on.
enum class NeuralJobKind { Offline, Live, Preview };

class PlayerApp {
#ifdef PLAYER_APP_TESTING
    friend struct PlayerAppTestAccess;
#endif
public:
    explicit PlayerApp(AppOptions o):m_opt(std::move(o)),m_youtubeSourceQuality(YouTubeSourceQuality::Auto),m_neuralPauseEvent(CreateEventW(nullptr,TRUE,FALSE,nullptr)){}
    ~PlayerApp(){if(m_activityTimer&&m_hwnd)KillTimer(m_hwnd,m_activityTimer);CancelExport();CancelNeuralJob(false);CancelYouTubeResolution(false);SaveVideoSettings();if(m_adjustWnd)DestroyWindow(m_adjustWnd);if(m_neuralWnd)DestroyWindow(m_neuralWnd);UnregisterOverlayHotkeys();Unload(); if(m_font)DeleteObject(m_font); if(m_fontSmall)DeleteObject(m_fontSmall); if(m_iconFont)DeleteObject(m_iconFont); if(m_neuralPauseEvent)CloseHandle(m_neuralPauseEvent);}

    bool Create(HINSTANCE hi) {
        m_loc.Initialize();
        if(!m_uiResources.Load(hi))LOG("Embedded Tabler icon font unavailable; continuing with label-only controls.");
        LoadVideoSettings();
        NeuralCacheManager historyCache(m_cacheRoot);
        if(historyCache.Valid()){
            m_cacheRoot=historyCache.Root();
            SaveCacheSettings();
            LOG("Neural cache directory: "<<WideToUtf8(m_cacheRoot.wstring()));
            m_recent=std::make_unique<RecentMediaHistory>(historyCache.Root()/L"recent-videos.dat");
            if(!m_recent->Load())LOG("Recent videos could not be loaded; existing file preserved until next successful playback.");
        }
        INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_BAR_CLASSES};InitCommonControlsEx(&icc);
        WNDCLASSW r{}; r.style=CS_DBLCLKS|CS_OWNDC; r.lpfnWndProc=RenderWndProcStatic; r.hInstance=hi; r.lpszClassName=L"DLSSVideoRenderClassV11"; r.hCursor=LoadCursor(nullptr,IDC_ARROW); r.hbrBackground=nullptr; RegisterClassW(&r);
        WNDCLASSW v{}; v.lpfnWndProc=ViewportWndProcStatic; v.hInstance=hi; v.lpszClassName=L"DLSSVideoViewportClassV11"; v.hCursor=LoadCursor(nullptr,IDC_ARROW); v.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH); RegisterClassW(&v);
        WNDCLASSW a{}; a.lpfnWndProc=AdjustWndProcStatic; a.hInstance=hi; a.lpszClassName=L"DLSSVideoAdjustmentsClassV11"; a.hCursor=LoadCursor(nullptr,IDC_ARROW); a.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1); RegisterClassW(&a);
        WNDCLASSEXW w{}; w.cbSize=sizeof(w); w.lpfnWndProc=WndProcStatic; w.hInstance=hi; w.lpszClassName=L"DLSSVideoPlayerV11Class"; w.hCursor=LoadCursor(nullptr,IDC_ARROW); w.hbrBackground=CreateSolidBrush(RGB(18,19,21));
        w.hIcon=static_cast<HICON>(LoadImageW(hi,MAKEINTRESOURCEW(IDI_DLSS_VIDEO_PLAYER),IMAGE_ICON,GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),LR_SHARED));
        w.hIconSm=static_cast<HICON>(LoadImageW(hi,MAKEINTRESOURCEW(IDI_DLSS_VIDEO_PLAYER),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
        RegisterClassExW(&w);
        RECT rc{0,0,1440,880}; AdjustWindowRect(&rc,WS_OVERLAPPEDWINDOW,TRUE);
        const std::wstring appTitle=m_loc.Get(L"app.title");
        m_hwnd=CreateWindowExW(WS_EX_ACCEPTFILES,w.lpszClassName,appTitle.c_str(),WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,rc.right-rc.left,rc.bottom-rc.top,nullptr,app_menu::CreateMenuBar(m_loc,YouTubePlaybackAvailable()),hi,this);
        if(!m_hwnd) return false;
        ReadAnimationPreference();
        app_menu::UpdateYouTubeQualitySelection(GetMenu(m_hwnd),m_youtubeSourceQuality);
        UpdateRecentMenu();
        RegisterOverlayHotkeys();
        BOOL dark=TRUE; DwmSetWindowAttribute(m_hwnd,20,&dark,sizeof(dark)); DWORD corner=2; DwmSetWindowAttribute(m_hwnd,33,&corner,sizeof(corner));
        m_viewport=CreateWindowExW(0,v.lpszClassName,nullptr,WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,100,100,m_hwnd,nullptr,hi,nullptr);
        m_renderWnd=CreateWindowExW(WS_EX_ACCEPTFILES,L"DLSSVideoRenderClassV11",nullptr,WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,0,0,100,100,m_viewport,nullptr,hi,this);
        UpdateFontsForDpi(ActiveWindowDpi(m_hwnd));
        DragAcceptFiles(m_hwnd,TRUE); DragAcceptFiles(m_renderWnd,TRUE); ShowWindow(m_viewport,SW_HIDE); Layout(); UpdateTitle();
        if(!m_opt.file.empty()){if(IsSupportedYouTubeUrl(m_opt.file))StartYouTubeResolution(m_opt.file,L"",m_youtubeSourceQuality);else Load(m_opt.file);} // No startup file picker: the player opens idle by default.
        return true;
    }

    // Only a live network stream needs the non-blocking read and the
    // re-resolving seek path. A synchronized cache pair and an acquired local
    // copy of a stream are ordinary files, whatever the source identity says.
    bool NetworkPlayback()const{return m_sourceKind==MediaSourceKind::YouTube&&!m_cachedPlayback&&!m_cachedSourceFile;}
    // Decode semantics of the loaded payload: an acquired copy of a stream is a
    // local file even though its identity stays networked.
    MediaSourceKind DecodeKind()const{return m_cachedSourceFile?MediaSourceKind::LocalFile:m_sourceKind;}


    void Tick() {
        PruneRecentCache();
        ReapSourcePrefetch();
        UpdateLiveSession();
        if(m_seekPending) {
            const double target=m_pendingSeekSec; const bool resume=m_seekResumePlaying;
            m_seekPending=false; PerformSeek(target,resume); return;
        }
        if(m_loaded&&!m_playing&&!m_seeking&&m_renderer){
            const auto nowClock=Clock::now();
            if(std::chrono::duration<double>(nowClock-m_lastStaticPresent).count()>=1.0/60.0){
                m_renderer->PresentCurrent();
                m_lastStaticPresent=nowClock;
            }
        }
        if(m_loaded&&m_cachedPlayback&&m_playing&&!m_haveNext&&!m_seeking){
            if(!ReadNextCachedFrame())return;
        }
        if(m_loaded&&m_playing&&NetworkPlayback()&&!m_haveNext&&!m_seeking){
            if(ApplyNetworkRead(m_decoder.ReadNextAvailable(m_next),NetworkReadPosition::BeforeRender)!=NetworkReadAction::UseFrame)return;
        }
        if(!m_loaded||!m_playing||!m_haveNext||m_seeking) return;
        double now=Position(); const double frameDur=1.0/std::max(1.0,m_decoder.FrameRate());
        bool dropped=false;
        while(m_haveNext) {
            double due=double(m_next.timestamp100ns)*1e-7;
            if(now-due <= playback_timing::LateFrameThreshold(frameDur)) break;
            VideoFrame skip=std::move(m_next); (void)skip; ++m_droppedFrames; dropped=true;
            if(m_cachedPlayback){if(!ReadNextCachedFrame())break;}
            else if(NetworkPlayback()){if(ApplyNetworkRead(m_decoder.ReadNextAvailable(m_next),NetworkReadPosition::BeforeRender)!=NetworkReadAction::UseFrame)break;}
            else if(!m_decoder.ReadNext(m_next)){m_haveNext=false;break;}
        }
        if(dropped){m_guides.Reset();m_guideReset=true;m_dlssReset=true;}
        if(!m_haveNext){if(!NetworkPlayback()){m_playing=false;Audio().Pause(true);}InvalidateControls();InvalidatePlaybackProgress();UpdateCachedStatus();return;}
        double due=double(m_next.timestamp100ns)*1e-7;
        if(now+0.001<due) return;
        if(RenderVideoFrame(m_next,m_next.discontinuity||m_guideReset)) {
            m_previewShown=false;
            RememberRenderedCachedPair();
            if(m_cachedPlayback)++m_cachedPresentedFrames;
            ++m_fpsWindowFrames;
            const auto fpsNow=Clock::now();
            const double fpsElapsed=std::chrono::duration<double>(fpsNow-m_fpsWindowStart).count();
            if(fpsElapsed>=0.75){m_submitFps=double(m_fpsWindowFrames)/fpsElapsed;m_fpsWindowFrames=0;m_fpsWindowStart=fpsNow;}
        }
        m_currentSec=due; m_guideReset=false; m_dlssReset=false;
        if(m_cachedPlayback)m_haveNext=false;
        else if(NetworkPlayback())ApplyNetworkRead(m_decoder.ReadNextAvailable(m_next),NetworkReadPosition::AfterRender);
        else if(!m_decoder.ReadNext(m_next)){m_haveNext=false;m_playing=false;Audio().Pause(true);}
        InvalidatePlaybackProgress();
        UpdateCachedStatus();
    }

    bool Running()const{return m_running;}
    bool NeedsRealtimeTick()const{return m_loaded;}
    DWORD TickSleepMs()const{return (m_loaded&&!m_playing&&!m_seekPending&&!m_seeking)?8u:0u;}


private:
    static constexpr UINT_PTR kActivityTimerId=0xD155;
    static constexpr UINT_PTR kFullscreenTimerId=0xD156;
    static constexpr UINT_PTR kPreviewTimerId=0xD157;
    static constexpr auto kFullscreenIdleDelay=std::chrono::milliseconds(2500);
    bool ActivityBusy()const{return NeuralJobActive()||m_youtubeLifecycle.IsResolving();}
    // A job that renders behind the loaded media: the player keeps the window,
    // and only its own panel and lanes report progress.
    bool JobBehindPlayback()const{return m_liveSession||m_previewJob;}

    void UpdateRecentMenu(){
        if(!m_hwnd||!m_recent)return;
        std::vector<std::wstring> titles;for(const auto& entry:m_recent->Entries())titles.push_back(entry.title.empty()?entry.source:entry.title);
        app_menu::UpdateRecentVideos(GetMenu(m_hwnd),titles,!ActivityBusy());
        DrawMenuBar(m_hwnd);
    }
    void RecordRecent(const NeuralJobCompletion& completion,bool preserveCache=false){
        if(!m_recent)return;
        RecentMediaEntry entry{};entry.youtube=completion.sourceKind==MediaSourceKind::YouTube;
        entry.id=entry.youtube?CanonicalYouTubeVideoId(completion.pageUrl):WideToUtf8(std::filesystem::absolute(completion.sourcePath).lexically_normal().wstring());
        entry.title=DisplayTitleForSource(completion.sourceKind,completion.displayTitle);entry.source=entry.youtube?completion.pageUrl:std::filesystem::absolute(completion.sourcePath).lexically_normal().wstring();entry.sourceKey=completion.sourceKey;entry.renderKey=completion.renderKey;entry.sourceQuality=static_cast<int>(completion.sourceQuality);
        if(preserveCache)for(const auto& previous:m_recent->Entries()){
            const bool sameVideo=previous.youtube==entry.youtube&&((entry.youtube&&previous.id==entry.id&&previous.sourceQuality==entry.sourceQuality)||(!entry.youtube&&_wcsicmp(previous.source.c_str(),entry.source.c_str())==0));
            if(sameVideo&&(entry.sourceKey.empty()||entry.sourceKey==previous.sourceKey)){entry.sourceKey=previous.sourceKey;entry.renderKey=previous.renderKey;break;}
        }
        auto evicted=m_recent->Remember(std::move(entry));
        if(m_recent->Save())m_pendingCacheEvictions.insert(m_pendingCacheEvictions.end(),evicted.begin(),evicted.end());
        else LOG("Recent video history could not be saved; cached media was preserved.");
        UpdateRecentMenu();
    }
    void RecordOriginalRecent(){
        if(!m_recent||m_path.empty()||(m_sourceKind==MediaSourceKind::YouTube&&m_youtubePageUrl.empty()))return;
        NeuralJobCompletion completion{};completion.sourcePath=m_path;completion.displayTitle=m_displayTitle;completion.sourceKind=m_sourceKind;completion.pageUrl=m_youtubePageUrl;completion.sourceQuality=m_youtubeSourceQuality;RecordRecent(completion,true);
    }
    bool LoadOriginalFallback(const NeuralJobCompletion& completion){
        // An acquired YouTube source is a local file on disk from here on.
        if(!LoadOriginal(completion.sourcePath.wstring(),completion.displayTitle,completion.sourceKind,
                         completion.sourceKind==MediaSourceKind::YouTube))return false;
        m_youtubePageUrl=completion.pageUrl;m_youtubeSourceQuality=completion.sourceQuality;
        auto original=completion;original.renderKey.clear();RecordRecent(original,true);
        UpdateYouTubeQualitySelection(GetMenu(m_hwnd),m_youtubeSourceQuality);return true;
    }
    void PruneRecentCache(){
        if(m_pendingCacheEvictions.empty()||ActivityBusy()||m_exportWorker.joinable())return;
        NeuralCacheManager cache(m_cacheRoot);if(!cache.Valid()){m_pendingCacheEvictions.clear();return;}
        const auto& keep=m_recent->Entries();
        const auto referenced=[&](const std::string& key,bool render){return key.empty()||std::any_of(keep.begin(),keep.end(),[&](const auto& item){return (render?item.renderKey:item.sourceKey)==key;});};
        for(const auto& item:m_pendingCacheEvictions){if(!referenced(item.sourceKey,false))cache.RemoveSource(item.sourceKey);if(!referenced(item.renderKey,true))cache.RemoveRender(item.renderKey);}
        m_pendingCacheEvictions.clear();
    }
    void OpenRecent(size_t index){
        if(!m_recent||index>=m_recent->Entries().size()||ActivityBusy())return;
        const auto entry=m_recent->Entries()[index];
        if(entry.youtube){if(NeuralPreRenderEnabled()&&!entry.sourceKey.empty())StartNeuralJob(entry.source,{},entry.title,entry.source,MediaSourceKind::YouTube,static_cast<YouTubeSourceQuality>(entry.sourceQuality),entry.sourceKey,0.0,{},true);else StartYouTubeResolution(entry.source,entry.title,static_cast<YouTubeSourceQuality>(entry.sourceQuality));return;}
        if(!std::filesystem::is_regular_file(entry.source)){MessageBoxW(m_hwnd,L"This local video has moved or is no longer available.",T(L"app.title").c_str(),MB_OK|MB_ICONINFORMATION);return;}
        Load(entry.source,entry.title,MediaSourceKind::LocalFile);
    }
    void ExportCachedVideo(){
        if(!m_cachedPlayback||m_neuralPath.empty()||m_exportWorker.joinable())return;
        // The file on disk carries the settings it was rendered with. Saving it
        // after the user changed them would write something they never saw.
        if(m_cachedSettings!=m_neuralSettings||m_cachedGuides!=m_renderGuides){
            const std::wstring message=T(L"export.settings_changed"),caption=T(L"app.title");
            if(MessageBoxW(m_hwnd,message.c_str(),caption.c_str(),MB_YESNO|MB_ICONQUESTION)==IDYES)RenderRangeOfCurrentSource(m_cachedRange);
            return;
        }
        const auto output=PickExportFile(m_hwnd,m_displayTitle,m_decoder.IsStillImage(),m_decoder.IsAnimation());if(output.empty())return;
        CachedExportRequest request{m_neuralPath,std::filesystem::path(m_path),output};if(!m_cachedRange.Whole()){request.rangeStartSeconds=double(m_cachedRange.start100ns)*1e-7;request.rangeDurationSeconds=double(m_cachedRange.end100ns-m_cachedRange.start100ns)*1e-7;}
        const auto helpers=ExecutableDirectory();HWND target=m_hwnd;auto* completions=&m_exportCompletions;
        try{m_exportWorker=std::jthread([target,request,helpers,completions](std::stop_token stop){auto completion=std::make_unique<ExportCompletion>();completion->output=request.output;completion->result=CachedVideoExporter(helpers).Run(request,stop);completions->RegisterAndPost(std::move(completion),[&](uint64_t token){return PostMessageW(target,WM_EXPORT_COMPLETE,static_cast<WPARAM>(token),0)!=FALSE;});});}
        catch(const std::system_error&){MessageBoxW(m_hwnd,L"The export worker could not start. Try again.",L"Export failed",MB_OK|MB_ICONERROR);return;}
        SyncFeatureMenuState();UpdateCachedStatus();
    }
    void CancelExport(){if(m_exportWorker.joinable()){m_exportWorker.request_stop();m_exportWorker.join();m_exportWorker=std::jthread{};}m_exportCompletions.Clear();if(m_hwnd&&IsWindow(m_hwnd)){SyncFeatureMenuState();UpdateCachedStatus();}}
    void CompleteExport(uint64_t token){auto completion=m_exportCompletions.Take(token);if(!completion)return;if(m_exportWorker.joinable()){m_exportWorker.join();m_exportWorker=std::jthread{};}SyncFeatureMenuState();UpdateCachedStatus();if(completion->result.ok){const std::wstring message=L"Exported to:\n"+completion->output.wstring();MessageBoxW(m_hwnd,message.c_str(),L"Export complete",MB_OK|MB_ICONINFORMATION);}else if(completion->result.error!=MaterializeError::Cancelled)MessageBoxW(m_hwnd,completion->result.detail.c_str(),L"Export failed",MB_OK|MB_ICONERROR);}
    uint64_t ActivityElapsedMs()const{
        return m_activityBusy?static_cast<uint64_t>(std::max<int64_t>(0,std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-m_activityStarted).count())):0;
    }
    void ReadAnimationPreference(){
        BOOL enabled=TRUE;
        if(SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&enabled,0))m_activityMotionEnabled=enabled!=FALSE;
        if(m_activityTimer){KillTimer(m_hwnd,m_activityTimer);m_activityTimer=0;}
        SyncActivityFeedback();
    }
    void SyncActivityFeedback(){
        if(!m_hwnd)return;
        const bool busy=ActivityBusy();
        if(busy&&!m_activityBusy)m_activityStarted=Clock::now();
        m_activityBusy=busy;
        const bool visible=IsWindowVisible(m_hwnd)&&!IsIconic(m_hwnd);
        if(busy&&visible){
            if(!m_activityTimer)m_activityTimer=SetTimer(m_hwnd,kActivityTimerId,m_activityMotionEnabled?50:1000,nullptr);
        }else if(m_activityTimer){KillTimer(m_hwnd,m_activityTimer);m_activityTimer=0;}
    }
    void AnimateActivity(){
        SyncActivityFeedback();
        if(!m_activityTimer)return;
        // An active session keeps the player on screen: only the buffering panel
        // and the status line animate, never the full-window pre-render surface.
        if(JobBehindPlayback()){RefreshBufferOverlay();const RECT dirty=StatusRect();InvalidateRect(m_hwnd,&dirty,FALSE);return;}
        if(m_loaded&&!NeuralJobActive()){const RECT dirty=StatusRect();InvalidateRect(m_hwnd,&dirty,FALSE);return;}
        RECT c{};GetClientRect(m_hwnd,&c);
        const auto surface=LayoutPreRenderSurface(c.right-c.left,c.bottom-c.top,ActiveWindowDpi(m_hwnd));
        for(const RECT& dirty:{surface.spinner,surface.progressTrack,surface.elapsedEta})InvalidateRect(m_hwnd,&dirty,FALSE);
    }
    void DrawActivitySpinner(HDC dc,RECT bounds,unsigned step){
        if(bounds.right<=bounds.left||bounds.bottom<=bounds.top)return;
        const int saved=SaveDC(dc);if(!saved)return;
        SelectObject(dc,GetStockObject(NULL_PEN));SelectObject(dc,GetStockObject(DC_BRUSH));
        const double side=std::min(bounds.right-bounds.left,bounds.bottom-bounds.top);
        const double cx=(bounds.left+bounds.right)/2.0,cy=(bounds.top+bounds.bottom)/2.0;
        const int dot=std::max(1,int(side*0.075));
        for(unsigned index=0;index<12;++index){
            const double angle=(double(index)/12.0)*6.283185307179586-1.570796326794897;
            const double strength=0.2+0.8*double((index+12-step)%12)/11.0;
            const auto channel=[&](int bg,int fg){return BYTE(bg+(fg-bg)*strength);};
            SetDCBrushColor(dc,RGB(channel(18,55),channel(19,139),channel(21,226)));
            const int x=int(std::lround(cx+std::cos(angle)*side*0.36)),y=int(std::lround(cy+std::sin(angle)*side*0.36));
            Ellipse(dc,x-dot,y-dot,x+dot+1,y+dot+1);
        }
        RestoreDC(dc,saved);
    }
    std::wstring T(const wchar_t* key)const{return m_loc.Get(key);}
    AudioPlayer& Audio(){return m_networkAudio?*m_networkAudio:m_audio;}
    const AudioPlayer& Audio()const{return m_networkAudio?*m_networkAudio:m_audio;}
    bool ReadNextCachedFrame(){
        const auto read=m_synchronizedPlayback.ReadNextAvailable();
        if(read==SynchronizedReadResult::PairReady){const VideoFrame* visible=m_synchronizedPlayback.VisibleFrame();if(!visible)return false;m_next=*visible;m_haveNext=true;return true;}
        if(read==SynchronizedReadResult::NotReady)return false;
        // The playhead reached the render head: wait for the next segment
        // instead of ending playback.
        if(read==SynchronizedReadResult::WaitingForRender){EnterLiveBuffering();return false;}
        m_haveNext=false;m_playing=false;Audio().Pause(true);if(read==SynchronizedReadResult::EndOfStream)LOG("Cached playback completed: presented="<<m_cachedPresentedFrames<<" dropped="<<m_droppedFrames);
        if(read==SynchronizedReadResult::OutOfSync||read==SynchronizedReadResult::Error){const std::wstring message=T(read==SynchronizedReadResult::OutOfSync?L"neural.sync.warning":L"error.decode"),caption=T(L"app.title");MessageBoxW(m_hwnd,message.c_str(),caption.c_str(),MB_OK|MB_ICONERROR);}
        InvalidateControls();InvalidatePlaybackProgress();return false;
    }
    void RememberRenderedCachedPair(){if(!m_cachedPlayback)return;if(const auto* pair=m_synchronizedPlayback.CurrentPair()){m_lastOriginalFrame=pair->original;m_lastNeuralFrame=pair->neural;m_havePresentedPair=true;}}
    NetworkReadAction ApplyNetworkRead(VideoReadResult result,NetworkReadPosition position){
        const NetworkReadDecision decision=m_networkReadState.Resolve(result,position);
        switch(decision.action){
        case NetworkReadAction::UseFrame:m_haveNext=true;m_waitingForNetworkFrame=false;break;
        case NetworkReadAction::Wait:m_haveNext=false;m_waitingForNetworkFrame=true;break;
        case NetworkReadAction::StopClean:case NetworkReadAction::StopCancelled:case NetworkReadAction::StopError:
            m_haveNext=false;m_waitingForNetworkFrame=false;m_playing=false;Audio().Pause(true);
            if(decision.notify){const std::wstring message=T(decision.messageKey.data()),caption=T(L"app.title");MessageBoxW(m_hwnd,message.c_str(),caption.c_str(),MB_OK|MB_ICONERROR);}
            InvalidateControls();InvalidatePlaybackProgress();UpdateCachedStatus();break;
        }
        return decision.action;
    }

    int Dip(int value)const{return MulDiv(value,static_cast<int>(ActiveWindowDpi(m_hwnd)),USER_DEFAULT_SCREEN_DPI);}
    bool ControlsVisible()const{return !m_loaded||!m_fullscreen||!m_fullscreenControlsHidden;}
    int ControlHeight()const{return ControlsVisible()?Dip(CONTROL_H_DIP):0;}
    void UpdateFontsForDpi(UINT dpi){
        const UINT activeDpi=dpi==0?USER_DEFAULT_SCREEN_DPI:dpi;
        HFONT regular=CreateFontW(-MulDiv(16,static_cast<int>(activeDpi),USER_DEFAULT_SCREEN_DPI),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        HFONT smallFont=CreateFontW(-MulDiv(14,static_cast<int>(activeDpi),USER_DEFAULT_SCREEN_DPI),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        HFONT icons=m_uiResources.CreateIconFont(activeDpi);
        if(regular){if(m_font)DeleteObject(m_font);m_font=regular;}
        if(smallFont){if(m_fontSmall)DeleteObject(m_fontSmall);m_fontSmall=smallFont;}
        if(icons){if(m_iconFont)DeleteObject(m_iconFont);m_iconFont=icons;}
        else{if(m_iconFont)DeleteObject(m_iconFont);m_iconFont=nullptr;if(m_uiResources.IsLoaded()&&!m_iconFallbackLogged){LOG("Tabler icon font creation failed; continuing with label-only controls.");m_iconFallbackLogged=true;}}
    }

    std::filesystem::path SettingsPath()const{
        wchar_t p[32768]{};DWORD n=GetModuleFileNameW(nullptr,p,static_cast<DWORD>(std::size(p)));
        if(!n||n>=std::size(p))return std::filesystem::current_path()/L"DLSSVideoPlayer.ini";
        return std::filesystem::path(p).parent_path()/L"DLSSVideoPlayer.ini";
    }

    float ReadIniFloat(const wchar_t* section,const wchar_t* key,float fallback)const{
        wchar_t def[64]{},buf[128]{};swprintf_s(def,L"%.6f",fallback);
        const auto path=SettingsPath();GetPrivateProfileStringW(section,key,def,buf,static_cast<DWORD>(std::size(buf)),path.c_str());
        wchar_t* end=nullptr;double v=wcstod(buf,&end);return (end&&end!=buf&&std::isfinite(v))?float(v):fallback;
    }

    void WriteIniFloat(const wchar_t* section,const wchar_t* key,float value)const{
        wchar_t buf[64]{};swprintf_s(buf,L"%.6f",value);const auto path=SettingsPath();WritePrivateProfileStringW(section,key,buf,path.c_str());
    }

    static bool SameCachePath(const std::filesystem::path& a,const std::filesystem::path& b){
        if(a.empty()||b.empty())return false;
        std::error_code ea,eb;
        const auto ca=std::filesystem::weakly_canonical(a,ea),cb=std::filesystem::weakly_canonical(b,eb);
        return !ea&&!eb&&_wcsicmp(ca.c_str(),cb.c_str())==0;
    }
    void SaveCacheSettings()const{
        if(m_cacheRoot.empty())return;
        const auto portable=ExecutableDirectory()/L"cache"/L"v1";
        const auto legacy=NeuralCacheManager::LegacyDefaultRoot();
        const auto resolvedLegacy=NeuralCacheManager::ResolvedLegacyDefaultRoot();
        const auto isLegacy=[&](const std::filesystem::path& root){
            return (legacy&&SameCachePath(root,*legacy))||(resolvedLegacy&&SameCachePath(root,*resolvedLegacy));
        };
        std::wstring savedDirectory(32768,L'\0');
        const DWORD savedLength=GetPrivateProfileStringW(L"Storage",L"CacheDirectory",L"",savedDirectory.data(),static_cast<DWORD>(savedDirectory.size()),SettingsPath().c_str());
        savedDirectory.resize(savedLength);
        const auto savedRoot=std::filesystem::path(savedDirectory);
        const bool explicitRoot=GetPrivateProfileIntW(L"Storage",L"CacheDirectoryAutomatic",-1,SettingsPath().c_str())==0&&
            (SameCachePath(m_cacheRoot,savedRoot)||(isLegacy(m_cacheRoot)&&isLegacy(savedRoot)));
        const bool automatic=!explicitRoot&&(SameCachePath(m_cacheRoot,portable)||isLegacy(m_cacheRoot));
        WritePrivateProfileStringW(L"Storage",L"CacheDirectory",m_cacheRoot.c_str(),SettingsPath().c_str());
        WritePrivateProfileStringW(L"Storage",L"CacheDirectoryAutomatic",automatic?L"1":L"0",SettingsPath().c_str());
    }
    void LoadVideoSettings(){
        std::wstring cacheDirectory(32768,L'\0');
        const DWORD cacheLength=GetPrivateProfileStringW(L"Storage",L"CacheDirectory",L"",cacheDirectory.data(),static_cast<DWORD>(cacheDirectory.size()),SettingsPath().c_str());
        cacheDirectory.resize(cacheLength);m_cacheRoot=std::filesystem::path(cacheDirectory);
        if(!m_cacheRoot.is_absolute()||cacheLength>=32767)m_cacheRoot.clear();
        const auto legacy=NeuralCacheManager::LegacyDefaultRoot();
        const UINT automatic=GetPrivateProfileIntW(L"Storage",L"CacheDirectoryAutomatic",-1,SettingsPath().c_str());
        // Previous releases saved their default as an absolute path. Re-select
        // automatic storage on startup so a portable folder can move with its EXE.
        bool oldAutomatic=automatic==UINT(-1)&&legacy&&SameCachePath(m_cacheRoot,*legacy);
        if(automatic==UINT(-1)&&!m_cacheRoot.empty()&&!oldAutomatic){
            const auto resolvedLegacy=NeuralCacheManager::ResolvedLegacyDefaultRoot();
            oldAutomatic=resolvedLegacy&&SameCachePath(m_cacheRoot,*resolvedLegacy);
        }
        if(automatic==1||oldAutomatic)m_cacheRoot.clear();
        m_volume=std::clamp(ReadIniFloat(L"Playback",L"Volume",1.0f),0.0f,1.0f);
        m_muted=ReadIniFloat(L"Playback",L"Muted",0.0f)==1.0f;
        m_fill=ReadIniFloat(L"Playback",L"Fill",0.0f)==1.0f;
        m_neuralRequested=ReadIniFloat(L"Playback",L"NeuralView",1.0f)!=0.0f;
        m_upscalingRequested=ReadIniFloat(L"Playback",L"SuperResolution",0.0f)==1.0f;
        m_upscaleTargetHeight=ReadIniFloat(L"Playback",L"UpscaleHeight",1440.0f)==2160.0f?2160:1440;
        const float quality=ReadIniFloat(L"Playback",L"YouTubeQuality",0.0f);
        m_youtubeSourceQuality=YouTubeSourceQuality::Auto;
        for(const auto value:{YouTubeSourceQuality::Auto,YouTubeSourceQuality::P1080,YouTubeSourceQuality::P1440,YouTubeSourceQuality::P2160})
            if(quality==static_cast<float>(value))m_youtubeSourceQuality=value;
        m_colorSettings.brightness=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Brightness",0.0f),-2.0f,2.0f);
        m_colorSettings.contrast=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Contrast",1.0f),0.0f,3.0f);
        m_colorSettings.saturation=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Saturation",1.0f),0.0f,3.0f);
        m_colorSettings.gamma=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Gamma",1.0f),0.25f,3.0f);
        m_colorSettings.temperature=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Temperature",0.0f),-1.0f,1.0f);
        m_colorSettings.tint=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Tint",0.0f),-1.0f,1.0f);
        m_renderGuides.motionVectors=GetPrivateProfileIntW(L"NeuralGuides",L"MotionVectors",1,SettingsPath().c_str())!=0;
        m_renderGuides.depth=GetPrivateProfileIntW(L"NeuralGuides",L"Depth",1,SettingsPath().c_str())!=0;
        m_neuralSettings={};LoadNeuralSettings(SettingsPath(),m_neuralSettings);
        const UINT mode=GetPrivateProfileIntW(L"Comparison",L"Mode",0,SettingsPath().c_str());
        m_comparison={};
        for(const auto value:{ComparisonMode::Blend,ComparisonMode::SplitVertical,ComparisonMode::Wipe})
            if(mode==static_cast<UINT>(value))m_comparison.mode=value;
        m_comparison.amount=std::clamp(ReadIniFloat(L"Comparison",L"Amount",0.5f),0.0f,1.0f);
        m_comparison.splitX=std::clamp(ReadIniFloat(L"Comparison",L"SplitX",0.5f),0.0f,1.0f);
        m_comparison.zoomScale=ReadIniFloat(L"Comparison",L"ZoomScale",1.0f)>=kZoomScale?kZoomScale:1.0f;
    }

    void SaveVideoSettings()const{
        SaveCacheSettings();
        WriteIniFloat(L"Playback",L"Volume",m_volume);
        WriteIniFloat(L"Playback",L"Muted",m_muted?1.0f:0.0f);
        WriteIniFloat(L"Playback",L"Fill",m_fill?1.0f:0.0f);
        WriteIniFloat(L"Playback",L"NeuralView",m_neuralRequested?1.0f:0.0f);
        WriteIniFloat(L"Playback",L"SuperResolution",m_upscalingRequested?1.0f:0.0f);
        WriteIniFloat(L"Playback",L"UpscaleHeight",static_cast<float>(m_upscaleTargetHeight));
        WriteIniFloat(L"Playback",L"YouTubeQuality",static_cast<float>(m_youtubeSourceQuality));
        WriteIniFloat(L"VideoAdjustments",L"Brightness",m_colorSettings.brightness);
        WriteIniFloat(L"VideoAdjustments",L"Contrast",m_colorSettings.contrast);
        WriteIniFloat(L"VideoAdjustments",L"Saturation",m_colorSettings.saturation);
        WriteIniFloat(L"VideoAdjustments",L"Gamma",m_colorSettings.gamma);
        WriteIniFloat(L"VideoAdjustments",L"Temperature",m_colorSettings.temperature);
        WriteIniFloat(L"VideoAdjustments",L"Tint",m_colorSettings.tint);
        WritePrivateProfileStringW(L"NeuralGuides",L"MotionVectors",m_renderGuides.motionVectors?L"1":L"0",SettingsPath().c_str());
        WritePrivateProfileStringW(L"NeuralGuides",L"Depth",m_renderGuides.depth?L"1":L"0",SettingsPath().c_str());
        SaveNeuralSettings(SettingsPath(),m_neuralSettings);
        WritePrivateProfileStringW(L"Comparison",L"Mode",std::to_wstring(static_cast<int>(m_comparison.mode)).c_str(),SettingsPath().c_str());
        WriteIniFloat(L"Comparison",L"Amount",m_comparison.amount);
        WriteIniFloat(L"Comparison",L"SplitX",m_comparison.splitX);
        WriteIniFloat(L"Comparison",L"ZoomScale",m_comparison.zoomScale);
    }

    void ApplyVideoAdjustments(bool refreshPaused=true){
        if(m_renderer){
            m_renderer->SetColorSettings(m_colorSettings);
            if(refreshPaused&&!m_playing&&!m_seeking)m_renderer->PresentCurrent();
        }
    }

    // Blend/Split/Wipe compare the neural member against the original of the
    // same pair; that only exists during cached playback with the neural view.
    bool ComparisonModesAvailable()const{return m_loaded&&m_cachedPlayback&&m_neuralRequested;}
    ComparisonSettings EffectiveComparison()const{ComparisonSettings effective=m_comparison;if(!ComparisonModesAvailable())effective.mode=ComparisonMode::Neural;return effective;}
    static UINT CommandForComparisonMode(ComparisonMode mode){switch(mode){case ComparisonMode::Blend:return IDM_COMPARE_BLEND;case ComparisonMode::SplitVertical:return IDM_COMPARE_SPLIT;case ComparisonMode::Wipe:return IDM_COMPARE_WIPE;default:return IDM_COMPARE_NEURAL;}}
    // Uploads the original member the presentation shader compares against.
    // Only modes that read the reference pay for the source-size copy.
    void UploadComparisonReference(const VideoFrame& original){
        if(!m_renderer||original.bgra.empty()||EffectiveComparison().mode==ComparisonMode::Neural)return;
        m_renderer->UploadReferenceFrame(original.bgra.data(),original.bgra.size());
    }
    void ApplyComparison(bool refreshPaused=true){
        if(m_renderer){
            m_renderer->SetComparison(EffectiveComparison());
            if(refreshPaused&&!m_playing&&!m_seeking){if(m_havePresentedPair)UploadComparisonReference(m_lastOriginalFrame);m_renderer->PresentCurrent();}
        }
        SyncFeatureMenuState();
    }
    void SetComparisonMode(ComparisonMode mode){if(!ComparisonModesAvailable()||mode==ComparisonMode::Original)return;m_comparison.mode=mode;ApplyComparison();}
    void AdjustBlendAmount(float delta){if(!ComparisonModesAvailable())return;m_comparison.amount=std::clamp(std::round((m_comparison.amount+delta)*10.0f)/10.0f,0.0f,1.0f);ApplyComparison();}
    // The divider is an image-UV position; while zoomed the shader shows
    // uv=(screen-center)/zoom+center, so invert that to keep it under the pointer.
    void SetSplitFromRenderX(int x){
        RECT client{};if(!m_renderWnd||!GetClientRect(m_renderWnd,&client)||client.right<=0)return;
        const float screen=std::clamp(float(x)/float(client.right),0.0f,1.0f);
        m_comparison.splitX=std::clamp((screen-m_comparison.zoomCenterX)/std::max(m_comparison.zoomScale,1.0f)+m_comparison.zoomCenterX,0.0f,1.0f);
        ApplyComparison();
    }
    bool SplitDragActive()const{const ComparisonMode mode=EffectiveComparison().mode;return mode==ComparisonMode::SplitVertical||mode==ComparisonMode::Wipe;}
    void ToggleZoom(){
        if(!m_loaded||!m_renderer)return;
        const bool zoomed=m_comparison.zoomScale>1.0f;
        m_comparison.zoomScale=zoomed?1.0f:kZoomScale;
        RECT client{};
        if(!zoomed&&m_renderMouseKnown&&GetClientRect(m_renderWnd,&client)&&client.right>0&&client.bottom>0&&PtIn(client,m_renderMouse.x,m_renderMouse.y)){
            m_comparison.zoomCenterX=float(m_renderMouse.x)/float(client.right);m_comparison.zoomCenterY=float(m_renderMouse.y)/float(client.bottom);
        }else{m_comparison.zoomCenterX=0.5f;m_comparison.zoomCenterY=0.5f;}
        ApplyComparison();
    }
    void RenderMouseDown(HWND source,LPARAM position){
        m_fullscreenKeyboardFocus=false;SetFocus(m_hwnd);
        if(!SplitDragActive())return;
        m_dragSplit=true;SetCapture(source);SetSplitFromRenderX(GET_X_LPARAM(position));
    }
    void RenderMouseMove(HWND source,LPARAM position){
        FullscreenPointerMoved(source,position);
        m_renderMouse={GET_X_LPARAM(position),GET_Y_LPARAM(position)};m_renderMouseKnown=true;
        if(m_dragSplit&&GetCapture()==source)SetSplitFromRenderX(m_renderMouse.x);
    }
    void RenderMouseUp(HWND source){if(!m_dragSplit)return;m_dragSplit=false;if(GetCapture()==source)ReleaseCapture();}

    void SyncFeatureMenuState(){
        if(!m_hwnd)return;
        const bool neuralAvailable=ToolbarActionEnabled(ToolbarAction::ToggleNeuralRendering);
        const bool neuralActive=neuralAvailable&&m_comparisonView==ComparisonView::Neural;
        if(HMENU menu=GetMenu(m_hwnd)){
            app_menu::UpdateFeatureAvailability(menu,m_neuralRequested,neuralAvailable,neuralActive,
                                                ToolbarActionEnabled(ToolbarAction::ToggleUpscaling),UpscalingActive(),false,false);
            CheckMenuRadioItem(menu,IDM_UPSCALE_1440,IDM_UPSCALE_2160,
                m_upscaleTargetHeight==2160?IDM_UPSCALE_2160:IDM_UPSCALE_1440,MF_BYCOMMAND);
            const UINT outputState=(m_seeking||m_seekPending||NeuralJobActive()||m_youtubeLifecycle.IsResolving())?MF_GRAYED:MF_ENABLED;
            EnableMenuItem(menu,IDM_UPSCALE_1440,MF_BYCOMMAND|outputState);
            EnableMenuItem(menu,IDM_UPSCALE_2160,MF_BYCOMMAND|outputState);
            EnableMenuItem(menu,IDM_EXPORT_CACHED_VIDEO,MF_BYCOMMAND|((m_cachedPlayback&&!m_neuralPath.empty()&&!m_exportWorker.joinable()&&!ActivityBusy())?MF_ENABLED:MF_GRAYED));
            EnableMenuItem(menu,IDM_CANCEL_EXPORT,MF_BYCOMMAND|(m_exportWorker.joinable()?MF_ENABLED:MF_GRAYED));
            CheckMenuRadioItem(menu,IDM_ASPECT_FIT,IDM_ASPECT_FILL,m_fill?IDM_ASPECT_FILL:IDM_ASPECT_FIT,MF_BYCOMMAND);
            app_menu::UpdateRenderActionAvailability(menu,m_loaded,RangeRenderAvailable(),NeuralJobActive(),NeuralJobPaused(),!m_cachedReceiptPath.empty());
            app_menu::UpdateComparisonMenu(menu,ComparisonModesAvailable(),m_loaded&&m_renderer!=nullptr,CommandForComparisonMode(m_comparison.mode),m_comparison.zoomScale>1.0f);
            DrawMenuBar(m_hwnd);
        }
    }

    void InvalidateControls(){
        if(!m_hwnd)return;SyncFeatureMenuState();RECT c{};GetClientRect(m_hwnd,&c);
        if(!m_loaded){InvalidateRect(m_hwnd,nullptr,FALSE);return;}
        RECT bar{0,std::max<LONG>(0,c.bottom-ControlHeight()),c.right,c.bottom};InvalidateRect(m_hwnd,&bar,FALSE);
    }

    void SetTrack(HWND h,int id,int lo,int hi,int pos){
        HWND t=GetDlgItem(h,id);if(!t)return;SendMessageW(t,TBM_SETRANGE,TRUE,MAKELPARAM(lo,hi));SendMessageW(t,TBM_SETPOS,TRUE,pos);
    }

    void SetAdjustmentValue(HWND h,int id,const std::wstring& value){
        HWND v=GetDlgItem(h,id+100);if(v)SetWindowTextW(v,value.c_str());
    }

    static std::wstring SignedValue(float v,const wchar_t* suffix=L""){
        wchar_t b[64]{};swprintf_s(b,L"%+.2f%ls",v,suffix);return b;
    }

    static std::wstring PlainValue(float v,const wchar_t* suffix=L""){
        wchar_t b[64]{};swprintf_s(b,L"%.2f%ls",v,suffix);return b;
    }

    void UpdateAdjustmentValueLabels(HWND h){
        SetAdjustmentValue(h,IDC_ADJ_BRIGHTNESS,SignedValue(m_colorSettings.brightness,L" EV"));
        SetAdjustmentValue(h,IDC_ADJ_CONTRAST,PlainValue(m_colorSettings.contrast));
        SetAdjustmentValue(h,IDC_ADJ_SATURATION,PlainValue(m_colorSettings.saturation));
        SetAdjustmentValue(h,IDC_ADJ_GAMMA,PlainValue(m_colorSettings.gamma));
        SetAdjustmentValue(h,IDC_ADJ_TEMPERATURE,SignedValue(m_colorSettings.temperature));
        SetAdjustmentValue(h,IDC_ADJ_TINT,SignedValue(m_colorSettings.tint));
    }

    void SyncAdjustmentControls(HWND h){
        SetTrack(h,IDC_ADJ_BRIGHTNESS,0,400,int(std::lround((m_colorSettings.brightness+2.0f)*100.0f)));
        SetTrack(h,IDC_ADJ_CONTRAST,0,300,int(std::lround(m_colorSettings.contrast*100.0f)));
        SetTrack(h,IDC_ADJ_SATURATION,0,300,int(std::lround(m_colorSettings.saturation*100.0f)));
        SetTrack(h,IDC_ADJ_GAMMA,25,300,int(std::lround(m_colorSettings.gamma*100.0f)));
        SetTrack(h,IDC_ADJ_TEMPERATURE,0,200,int(std::lround((m_colorSettings.temperature+1.0f)*100.0f)));
        SetTrack(h,IDC_ADJ_TINT,0,200,int(std::lround((m_colorSettings.tint+1.0f)*100.0f)));
        UpdateAdjustmentValueLabels(h);
    }

    void ReadAdjustmentControls(HWND h){
        auto pos=[&](int id)->int{HWND t=GetDlgItem(h,id);return t?int(SendMessageW(t,TBM_GETPOS,0,0)):0;};
        m_colorSettings.brightness=float(pos(IDC_ADJ_BRIGHTNESS))/100.0f-2.0f;
        m_colorSettings.contrast=float(pos(IDC_ADJ_CONTRAST))/100.0f;
        m_colorSettings.saturation=float(pos(IDC_ADJ_SATURATION))/100.0f;
        m_colorSettings.gamma=std::max(0.25f,float(pos(IDC_ADJ_GAMMA))/100.0f);
        m_colorSettings.temperature=float(pos(IDC_ADJ_TEMPERATURE))/100.0f-1.0f;
        m_colorSettings.tint=float(pos(IDC_ADJ_TINT))/100.0f-1.0f;
        UpdateAdjustmentValueLabels(h);ApplyVideoAdjustments(true);
    }

    // A tooltip host lives for as long as its dialog. Tool text is kept in
    // m_tipText because TTM_ADDTOOL stores the pointer it is given.
    HWND EnsureTipHost(HWND dialog){
        if(m_tipWnd&&IsWindow(m_tipWnd)&&GetParent(m_tipWnd)==dialog)return m_tipWnd;
        m_tipText.clear();
        m_tipWnd=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
            CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,dialog,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(m_tipWnd){
            SendMessageW(m_tipWnd,TTM_SETMAXTIPWIDTH,0,420);
            SendMessageW(m_tipWnd,TTM_SETDELAYTIME,TTDT_AUTOPOP,MAKELPARAM(30000,0));
        }
        return m_tipWnd;
    }
    void AddTip(HWND dialog,HWND control,const wchar_t* tipKey){
        if(!tipKey||!control)return;
        HWND host=EnsureTipHost(dialog);if(!host)return;
        m_tipText.push_back(std::make_unique<std::wstring>(T(tipKey)));
        // The app runs on comctl32 v5 (no v6 manifest), which rejects the v6
        // struct size, so ask for the version the classic control understands.
        TTTOOLINFOW info{};info.cbSize=TTTOOLINFOW_V2_SIZE;info.uFlags=TTF_IDISHWND|TTF_SUBCLASS;info.hwnd=dialog;
        info.uId=reinterpret_cast<UINT_PTR>(control);info.lpszText=m_tipText.back()->data();
        SendMessageW(host,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
    }
    void CreateAdjustmentRow(HWND h,int id,const wchar_t* labelKey,int y,const wchar_t* tipKey=nullptr){
        HFONT f=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND label=CreateWindowExW(0,L"STATIC",T(labelKey).c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,16,y,116,20,h,nullptr,nullptr,nullptr);
        HWND track=CreateWindowExW(0,TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_NOTICKS,132,y-6,236,30,h,(HMENU)(INT_PTR)id,nullptr,nullptr);
        HWND value=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_RIGHT,370,y,64,20,h,(HMENU)(INT_PTR)(id+100),nullptr,nullptr);
        SendMessageW(label,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(track,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(value,WM_SETFONT,(WPARAM)f,TRUE);
        AddTip(h,label,tipKey);AddTip(h,track,tipKey);AddTip(h,value,tipKey);
    }

    void BuildAdjustmentControls(HWND h){
        CreateAdjustmentRow(h,IDC_ADJ_BRIGHTNESS,L"adjustments.brightness",28);
        CreateAdjustmentRow(h,IDC_ADJ_CONTRAST,L"adjustments.contrast",78);
        CreateAdjustmentRow(h,IDC_ADJ_SATURATION,L"adjustments.saturation",128);
        CreateAdjustmentRow(h,IDC_ADJ_GAMMA,L"adjustments.gamma",178);
        CreateAdjustmentRow(h,IDC_ADJ_TEMPERATURE,L"adjustments.temperature",228);
        CreateAdjustmentRow(h,IDC_ADJ_TINT,L"adjustments.tint",278);
        HFONT f=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND note=CreateWindowExW(0,L"STATIC",T(L"adjustments.note").c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,16,322,418,38,h,nullptr,nullptr,nullptr);SendMessageW(note,WM_SETFONT,(WPARAM)f,TRUE);
        HWND reset=CreateWindowExW(0,L"BUTTON",T(L"adjustments.reset").c_str(),WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,252,368,86,30,h,(HMENU)(INT_PTR)IDC_ADJ_RESET,nullptr,nullptr);
        HWND close=CreateWindowExW(0,L"BUTTON",T(L"adjustments.close").c_str(),WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,348,368,86,30,h,(HMENU)(INT_PTR)IDC_ADJ_CLOSE,nullptr,nullptr);
        SendMessageW(reset,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(close,WM_SETFONT,(WPARAM)f,TRUE);
        SyncAdjustmentControls(h);
    }

    void ShowAdjustments(){
        if(m_adjustWnd&&IsWindow(m_adjustWnd)){ShowWindow(m_adjustWnd,SW_SHOWNORMAL);SetForegroundWindow(m_adjustWnd);return;}
        RECT pr{};GetWindowRect(m_hwnd,&pr);const int w=466,h=452,pw=int(pr.right-pr.left),ph=int(pr.bottom-pr.top);int x=int(pr.left)+std::max(0,(pw-w)/2),y=int(pr.top)+std::max(0,(ph-h)/2);
        m_adjustWnd=CreateWindowExW(WS_EX_TOOLWINDOW,L"DLSSVideoAdjustmentsClassV11",T(L"adjustments.title").c_str(),
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_VISIBLE,x,y,w,h,m_hwnd,nullptr,GetModuleHandleW(nullptr),this);
    }

    LRESULT AdjustWndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        switch(m){
        case WM_CREATE:BuildAdjustmentControls(h);return 0;
        case WM_HSCROLL:ReadAdjustmentControls(h);return 0;
        case WM_COMMAND:
            if(LOWORD(w)==IDC_ADJ_RESET){m_colorSettings={};SyncAdjustmentControls(h);ApplyVideoAdjustments(true);SaveVideoSettings();return 0;}
            if(LOWORD(w)==IDC_ADJ_CLOSE){DestroyWindow(h);return 0;}
            break;
        case WM_CLOSE:DestroyWindow(h);return 0;
        case WM_DESTROY:SaveVideoSettings();if(h==m_adjustWnd)m_adjustWnd=nullptr;return 0;
        }
        return DefWindowProcW(h,m,w,l);
    }

    void UpdateNeuralSettingValueLabels(HWND h){
        SetAdjustmentValue(h,IDC_NS_INTENSITY,PlainValue(m_neuralSettings.intensity));
        SetAdjustmentValue(h,IDC_NS_STRUCTURE,PlainValue(m_neuralSettings.localStructure));
        SetAdjustmentValue(h,IDC_NS_TONE,PlainValue(m_neuralSettings.localTone));
        SetAdjustmentValue(h,IDC_NS_SKIN,SignedValue(m_neuralSettings.skinStructure));
    }

    void SyncNeuralSettingControls(HWND h){
        SetTrack(h,IDC_NS_INTENSITY,0,200,int(std::lround(m_neuralSettings.intensity*100.0f)));
        SetTrack(h,IDC_NS_STRUCTURE,0,200,int(std::lround(m_neuralSettings.localStructure*100.0f)));
        SetTrack(h,IDC_NS_TONE,0,200,int(std::lround(m_neuralSettings.localTone*100.0f)));
        SetTrack(h,IDC_NS_SKIN,0,200,int(std::lround((m_neuralSettings.skinStructure+1.0f)*100.0f)));
        const auto select=[&](int id,int index){if(HWND combo=GetDlgItem(h,id))SendMessageW(combo,CB_SETCURSEL,static_cast<WPARAM>(index),0);};
        select(IDC_NS_STYLE,std::clamp(m_neuralSettings.style,0,2));
        const auto check=[&](int id,bool on){if(HWND box=GetDlgItem(h,id))SendMessageW(box,BM_SETCHECK,on?BST_CHECKED:BST_UNCHECKED,0);};
        check(IDC_NS_AUTOMASK,m_neuralSettings.autoMask);check(IDC_NS_GUIDE_MV,m_renderGuides.motionVectors);check(IDC_NS_GUIDE_DEPTH,m_renderGuides.depth);
        UpdateNeuralSettingValueLabels(h);
    }

    void ReadNeuralSettingControls(HWND h){
        auto pos=[&](int id)->int{HWND t=GetDlgItem(h,id);return t?int(SendMessageW(t,TBM_GETPOS,0,0)):0;};
        auto sel=[&](int id,int fallback)->int{HWND c=GetDlgItem(h,id);const int index=c?int(SendMessageW(c,CB_GETCURSEL,0,0)):CB_ERR;return index==CB_ERR?fallback:index;};
        auto checked=[&](int id)->bool{HWND b=GetDlgItem(h,id);return b&&SendMessageW(b,BM_GETCHECK,0,0)==BST_CHECKED;};
        m_neuralSettings.intensity=float(pos(IDC_NS_INTENSITY))/100.0f;
        m_neuralSettings.localStructure=float(pos(IDC_NS_STRUCTURE))/100.0f;
        m_neuralSettings.localTone=float(pos(IDC_NS_TONE))/100.0f;
        m_neuralSettings.skinStructure=float(pos(IDC_NS_SKIN))/100.0f-1.0f;
        m_neuralSettings.style=sel(IDC_NS_STYLE,m_neuralSettings.style);
        m_neuralSettings.autoMask=checked(IDC_NS_AUTOMASK);
        const GuideControls guides{checked(IDC_NS_GUIDE_MV),checked(IDC_NS_GUIDE_DEPTH)};
        if(guides!=m_renderGuides){m_renderGuides=guides;ApplyLiveGuideControls();}
        UpdateNeuralSettingValueLabels(h);
        // Sliders fire continuously; the preview waits for them to settle.
        SchedulePausedSettingsPreview();
    }

    // The persisted guide switches also drive the live (non-cached) guide
    // generator so the debug views reflect them without a re-render.
    void ApplyLiveGuideControls(){m_guides.SetControls(m_renderGuides);m_guideReset=true;m_dlssReset=true;UpdateTitle();}

    void CreateNeuralCombo(HWND h,int id,const wchar_t* labelKey,int y,std::initializer_list<const wchar_t*> items,const wchar_t* tipKey=nullptr){
        HFONT f=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND label=CreateWindowExW(0,L"STATIC",T(labelKey).c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,16,y,116,20,h,nullptr,nullptr,nullptr);
        HWND combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,132,y-3,160,200,h,(HMENU)(INT_PTR)id,nullptr,nullptr);
        for(const wchar_t* item:items)SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(item));
        SendMessageW(label,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(combo,WM_SETFONT,(WPARAM)f,TRUE);
        AddTip(h,label,tipKey);AddTip(h,combo,tipKey);
    }

    HWND CreateNeuralCheck(HWND h,int id,const wchar_t* labelKey,int x,int y,int width,const wchar_t* tipKey=nullptr){
        HWND box=CreateWindowExW(0,L"BUTTON",T(labelKey).c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,x,y,width,22,h,(HMENU)(INT_PTR)id,nullptr,nullptr);
        SendMessageW(box,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);AddTip(h,box,tipKey);return box;
    }

    // Color strength and render preset are deliberately absent: measured on the
    // pinned runtime they change nothing (0 differing bytes across four preset
    // pairs and two colour baselines, with the add-on echoing the value back),
    // while every change still costs a full re-render. They remain in
    // NeuralSettings and in DLSSVideoPlayer.ini so runtime-comparison work can
    // still drive them; see docs/BENCHMARK.md.
    void BuildNeuralSettingControls(HWND h){
        CreateAdjustmentRow(h,IDC_NS_INTENSITY,L"neural.settings.intensity",28,L"neural.tip.intensity");
        CreateAdjustmentRow(h,IDC_NS_STRUCTURE,L"neural.settings.structure",78,L"neural.tip.structure");
        CreateAdjustmentRow(h,IDC_NS_TONE,L"neural.settings.tone",128,L"neural.tip.tone");
        CreateAdjustmentRow(h,IDC_NS_SKIN,L"neural.settings.skin",178,L"neural.tip.skin");
        CreateNeuralCombo(h,IDC_NS_STYLE,L"neural.settings.style",228,{L"Default",L"Natural",L"Cinematic"},L"neural.tip.style");
        CreateNeuralCheck(h,IDC_NS_AUTOMASK,L"neural.settings.automask",132,266,236,L"neural.tip.automask");
        HFONT f=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND guides=CreateWindowExW(0,L"STATIC",T(L"neural.settings.guides").c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,16,304,116,20,h,nullptr,nullptr,nullptr);SendMessageW(guides,WM_SETFONT,(WPARAM)f,TRUE);
        CreateNeuralCheck(h,IDC_NS_GUIDE_MV,L"neural.settings.guide_mv",132,302,116,L"neural.tip.guide_mv");
        CreateNeuralCheck(h,IDC_NS_GUIDE_DEPTH,L"neural.settings.guide_depth",252,302,80,L"neural.tip.guide_depth");
        HWND note=CreateWindowExW(0,L"STATIC",T(L"neural.settings.note").c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,16,338,418,38,h,nullptr,nullptr,nullptr);SendMessageW(note,WM_SETFONT,(WPARAM)f,TRUE);
        HWND reset=CreateWindowExW(0,L"BUTTON",T(L"neural.settings.reset").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,120,384,86,30,h,(HMENU)(INT_PTR)IDC_NS_RESET,nullptr,nullptr);
        HWND apply=CreateWindowExW(0,L"BUTTON",T(L"neural.settings.apply").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,216,384,122,30,h,(HMENU)(INT_PTR)IDC_NS_APPLY,nullptr,nullptr);
        HWND close=CreateWindowExW(0,L"BUTTON",T(L"neural.settings.close").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,348,384,86,30,h,(HMENU)(INT_PTR)IDC_NS_CLOSE,nullptr,nullptr);
        SendMessageW(reset,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(apply,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(close,WM_SETFONT,(WPARAM)f,TRUE);
        AddTip(h,reset,L"neural.tip.reset");AddTip(h,apply,L"neural.tip.apply");
        SyncNeuralSettingControls(h);
    }

    void ShowNeuralSettings(){
        if(m_neuralWnd&&IsWindow(m_neuralWnd)){ShowWindow(m_neuralWnd,SW_SHOWNORMAL);SetForegroundWindow(m_neuralWnd);return;}
        static constexpr const wchar_t* kClassName=L"DLSSVideoNeuralSettingsClassV11";
        WNDCLASSW n{};n.lpfnWndProc=NeuralWndProcStatic;n.hInstance=GetModuleHandleW(nullptr);n.lpszClassName=kClassName;n.hCursor=LoadCursor(nullptr,IDC_ARROW);n.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
        if(!RegisterClassW(&n)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
        RECT pr{};GetWindowRect(m_hwnd,&pr);const int w=466,h=468,pw=int(pr.right-pr.left),ph=int(pr.bottom-pr.top);int x=int(pr.left)+std::max(0,(pw-w)/2),y=int(pr.top)+std::max(0,(ph-h)/2);
        m_neuralWnd=CreateWindowExW(WS_EX_TOOLWINDOW,kClassName,T(L"neural.settings.title").c_str(),
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_VISIBLE,x,y,w,h,m_hwnd,nullptr,GetModuleHandleW(nullptr),this);
    }

    // Apply changes what the player is showing now: an active session restarts
    // at the playhead with the new settings, a paused frame is re-previewed at
    // once. Writing a converted file is a separate, explicit action.
    void ApplyNeuralSettings(){
        SaveVideoSettings();
        if(m_liveSession){
            // The restart must not leave a queued seek behind: it would run
            // after the new session paused for its buffer and resume the
            // original underneath the loader.
            const double at=Position();const bool wasPlaying=m_playing||m_liveResumePlaying;
            LOG("Neural settings applied; restarting the active session at "<<at<<" s (playing="<<wasPlaying<<").");
            DetachLivePlayback();
            StopLiveNeuralSession(false);
            PerformSeek(at,false);
            StartLiveNeuralSession();
            m_liveResumePlaying=wasPlaying;
            return;
        }
        StartPausedSettingsPreview();
    }

    LRESULT NeuralWndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        switch(m){
        case WM_CREATE:BuildNeuralSettingControls(h);return 0;
        case WM_HSCROLL:ReadNeuralSettingControls(h);return 0;
        case WM_COMMAND:{
            const int id=LOWORD(w);const int code=HIWORD(w);
            if(id==IDC_NS_RESET){m_neuralSettings={};m_renderGuides={};ApplyLiveGuideControls();SyncNeuralSettingControls(h);SaveVideoSettings();SchedulePausedSettingsPreview();return 0;}
            if(id==IDC_NS_APPLY){ApplyNeuralSettings();return 0;}
            if(id==IDC_NS_CLOSE){DestroyWindow(h);return 0;}
            if((id==IDC_NS_STYLE&&code==CBN_SELCHANGE)||((id==IDC_NS_AUTOMASK||id==IDC_NS_GUIDE_MV||id==IDC_NS_GUIDE_DEPTH)&&code==BN_CLICKED)){ReadNeuralSettingControls(h);return 0;}
            break;
        }
        case WM_CLOSE:DestroyWindow(h);return 0;
        case WM_DESTROY:SaveVideoSettings();if(h==m_neuralWnd){m_neuralWnd=nullptr;m_tipWnd=nullptr;m_tipText.clear();}return 0;
        }
        return DefWindowProcW(h,m,w,l);
    }

    static LRESULT CALLBACK NeuralWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        PlayerApp* a=nullptr;
        if(m==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
        else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        return a?a->NeuralWndProc(h,m,w,l):DefWindowProcW(h,m,w,l);
    }

    static LRESULT CALLBACK WndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        PlayerApp* a=nullptr;
        if(m==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
        else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        return a?a->WndProc(h,m,w,l):DefWindowProcW(h,m,w,l);
    }
    static LRESULT CALLBACK ViewportWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        auto* app=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(GetParent(h),GWLP_USERDATA));
        if(app&&m==WM_MOUSEMOVE)app->FullscreenPointerMoved(h,l);
        if(app&&m==WM_LBUTTONDOWN){app->m_fullscreenKeyboardFocus=false;SetFocus(app->m_hwnd);return 0;}
        switch(m){
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{
            PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);RECT r{};GetClientRect(h,&r);
            FillRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));EndPaint(h,&ps);return 0;
        }}
        return DefWindowProcW(h,m,w,l);
    }

    static LRESULT CALLBACK AdjustWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        PlayerApp* a=nullptr;
        if(m==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
        else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        return a?a->AdjustWndProc(h,m,w,l):DefWindowProcW(h,m,w,l);
    }

    static LRESULT CALLBACK RenderWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        PlayerApp* a=nullptr;
        if(m==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
        else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(a){
            if(m==WM_ERASEBKGND)return 1;
            if(m==WM_PAINT){PAINTSTRUCT ps{};BeginPaint(h,&ps);EndPaint(h,&ps);return 0;}
            if(m==WM_MOUSEMOVE){a->RenderMouseMove(h,l);return 0;}
            if(m==WM_LBUTTONDOWN){a->RenderMouseDown(h,l);return 0;}
            if(m==WM_LBUTTONUP){a->RenderMouseUp(h);return 0;}
            if(m==WM_CAPTURECHANGED){a->m_dragSplit=false;return 0;}
            if(m==WM_LBUTTONDBLCLK){a->ToggleFullscreen();return 0;}
            if(m==WM_MOUSEWHEEL||m==WM_KEYDOWN||m==WM_SYSKEYDOWN)return SendMessageW(a->m_hwnd,m,w,l);
            if(m==WM_DROPFILES)return SendMessageW(a->m_hwnd,m,w,l); // main window owns DragFinish().
        }
        return DefWindowProcW(h,m,w,l);
    }

    bool Load(const std::wstring& source,const std::wstring& displayTitle=L"",MediaSourceKind sourceKind=MediaSourceKind::LocalFile) {
        // Preview first: identify the source and replay a validated cache entry
        // when one exists, otherwise open the original and let the user choose
        // what to render. Opening a file never starts a whole-video render.
        if(sourceKind==MediaSourceKind::LocalFile&&NeuralPreRenderEnabled()){
            StartNeuralJob(source,{},displayTitle,{},sourceKind,m_youtubeSourceQuality,{},0.0,{},true);
            return true;
        }
        return LoadOriginal(source,displayTitle,sourceKind);
    }

    bool LoadOriginal(const std::wstring& source,const std::wstring& displayTitle=L"",MediaSourceKind sourceKind=MediaSourceKind::LocalFile,bool localPayload=false) {
        if(source.empty())return false;
        CancelNeuralJob(false);
        CancelYouTubeResolution();
        Unload();
        LOG("Opening " << (localPayload ? std::string_view("acquired source copy") : SafeSourceLogLabel(sourceKind)) << ".");
        if(!m_decoder.Open(source,localPayload?MediaSourceKind::LocalFile:sourceKind)){std::wstring e=T(sourceKind==MediaSourceKind::YouTube?L"youtube.error.ffmpeg":L"error.decode"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);return false;}
        m_dar=m_decoder.DisplayAspectRatio(); if(!std::isfinite(m_dar)||m_dar<0.2)m_dar=double(m_decoder.Width())/std::max(1u,m_decoder.Height());
        const auto ow=m_decoder.Width(),oh=m_decoder.Height();
        m_activeQuality=DefaultNeuralCarrierQuality();
        const auto [guideW,guideH]=TemporalGuideGenerator::AnalysisGrid(m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate());
        ShowWindow(m_viewport,SW_SHOW); Layout();
        m_renderer=MakeD3D12Renderer();
        if(!m_renderer->Initialize(m_renderWnd,m_decoder.Width(),m_decoder.Height(),ow,oh,guideW,guideH,m_activeQuality)){std::wstring e=T(L"error.renderer"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);m_renderer.reset();m_decoder.Close();ShowWindow(m_viewport,SW_HIDE);return false;}
        m_renderer->SetDLSS(false);m_renderer->SetColorSettings(m_colorSettings);m_renderer->SetComparison(EffectiveComparison());
        VideoFrame first; if(!m_decoder.ReadNext(first)){std::wstring e=T(L"error.frame"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);Unload();return false;}
        m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;RenderVideoFrame(first,true);m_currentSec=double(first.timestamp100ns)*1e-7;
        m_haveNext=m_decoder.ReadNext(m_next);if(!m_decoder.IsStillImage())Audio().Start(source,m_currentSec);Audio().SetVolume(m_muted?0.0f:m_volume);m_playing=!m_decoder.IsStillImage();m_playStartSec=m_currentSec;m_playStart=Clock::now();m_loaded=true;m_path=source;m_sourceKind=sourceKind;m_cachedSourceFile=localPayload;m_displayTitle=DisplayTitleForSource(sourceKind,displayTitle);if(m_displayTitle.empty()&&sourceKind==MediaSourceKind::LocalFile){m_displayTitle=std::filesystem::path(source).stem().wstring();if(m_displayTitle.empty())m_displayTitle=std::filesystem::path(source).filename().wstring();}m_droppedFrames=0;m_seekPending=false;m_seeking=false;m_fpsWindowStart=Clock::now();m_fpsWindowFrames=0;m_submitFps=0.0;
        RestoreUpscaling();UpdateTitle();UpdateCachedStatus();Layout();RecordOriginalRecent();SyncFeatureMenuState();InvalidateRect(m_hwnd,nullptr,TRUE);return true;
    }

    void Unload() {
        if(m_liveSession){CancelNeuralJob(false);ReleaseLiveSession();}
        DropRetainedLiveSegments();
        m_lastPlaybackFrame={};m_upscalingError.clear();m_neuralPath.clear();m_cachedRange={};m_cachedReceiptPath.clear();m_cachedSettings={};m_cachedGuides={};m_markers={};m_dragSplit=false;m_renderMouseKnown=false;
        m_seekPending=false;m_seeking=false;Audio().Stop();m_networkAudio.reset();m_renderer.reset();m_decoder.Close();m_synchronizedPlayback.Close();m_cachedPlayback=false;m_cachedSourceFile=false;m_cachedPresentedFrames=0;m_havePresentedPair=false;m_lastOriginalFrame={};m_lastNeuralFrame={};m_guides.Reset();m_haveNext=false;m_waitingForNetworkFrame=false;m_networkReadState.Reset();m_next=VideoFrame{};m_loaded=false;m_playing=false;m_currentSec=0;m_lastRenderedTs=-1;m_path.clear();m_youtubeAudioUrl.clear();m_youtubePageUrl.clear();m_displayTitle.clear();m_sourceKind=MediaSourceKind::LocalFile;m_cachedStatus.clear();
        if(m_viewport)ShowWindow(m_viewport,SW_HIDE);Layout();UpdateTitle(); if(m_hwnd)InvalidateRect(m_hwnd,nullptr,TRUE);
    }

    bool UpscalingActive()const{return m_renderer&&m_renderer->DLSSEnabled();}
    bool UpscalingAvailable()const{
        return m_loaded&&m_renderer&&m_renderer->DLSSAvailable()&&
            !m_lastPlaybackFrame.bgra.empty()&&
            (UpscalingActive()||UpscalingTarget(m_decoder.Width(),m_decoder.Height(),m_upscaleTargetHeight).grows);
    }
    std::wstring UpscalingStatus()const{
        if(!m_upscalingError.empty())return m_upscalingError;
        if(UpscalingActive())return L"DLSS SR on · "+std::to_wstring(m_renderer->OutputW())+L"×"+std::to_wstring(m_renderer->OutputH());
        if(m_loaded&&m_decoder.Width()&&m_decoder.Height()&&!UpscalingTarget(m_decoder.Width(),m_decoder.Height(),m_upscaleTargetHeight).grows)
            return L"DLSS SR off (source meets output)";
        return UpscalingAvailable()?L"DLSS SR off":L"DLSS SR unavailable";
    }
    bool EnableUpscaling(uint32_t height){
        if(!m_loaded||!m_renderer||m_lastPlaybackFrame.bgra.empty())return false;
        const auto size=UpscalingTarget(m_decoder.Width(),m_decoder.Height(),height);
        if(!size.grows)return false;
        // Pause only the clocks while building a replacement. Decoders, cache,
        // queued frames and comparison selection remain untouched.
        const bool playing=m_playing;const double position=Position();
        Audio().Pause(true);m_playing=false;
        auto candidate=std::make_unique<PreparedRendererCandidate>();
        candidate->window=CreateWindowExW(WS_EX_ACCEPTFILES,L"DLSSVideoRenderClassV11",nullptr,
            WS_CHILD|WS_CLIPSIBLINGS,0,0,100,100,m_viewport,nullptr,GetModuleHandleW(nullptr),this);
        bool ready=false;
        if(candidate->window){
            candidate->renderer=MakeD3D12Renderer();
            const auto [gw,gh]=TemporalGuideGenerator::AnalysisGrid(m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate());
            if(candidate->renderer->Initialize(candidate->window,m_decoder.Width(),m_decoder.Height(),
                size.width,size.height,gw,gh,NVSDK_NGX_PerfQuality_Value_MaxQuality,true)&&
                candidate->renderer->DLSSAvailable()){
                candidate->renderer->SetColorSettings(m_colorSettings);candidate->renderer->SetComparison(EffectiveComparison());
                GuideFrame guide;
                candidate->guides.SetControls(m_guides.Controls());
                if(candidate->guides.Generate(m_lastPlaybackFrame.bgra.data(),m_decoder.Width(),m_decoder.Height(),
                    m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate(),
                    IdentityOf(m_lastPlaybackFrame,m_historyGeneration,0,HistoryReset::FirstFrame),guide)){
                    ready=candidate->renderer->RenderFrame(m_lastPlaybackFrame.bgra.data(),m_lastPlaybackFrame.bgra.size(),
                        guide.guideGridRGBA32F.data(),guide.guideGridRGBA32F.size()*sizeof(float),guide.gridW,guide.gridH,
                        true,float(1000.0/std::max(1.0,m_decoder.FrameRate())))&&candidate->renderer->LastFrameUsedDLSS();
                }
            }
        }
        if(ready){
            auto old=std::move(m_renderer);const HWND oldWindow=m_renderWnd;
            m_renderer=std::move(candidate->renderer);m_renderWnd=candidate->window;candidate->window=nullptr;
            m_guides=std::move(candidate->guides);m_guideReset=false;m_dlssReset=false;
            Layout();ShowWindow(m_renderWnd,SW_SHOW);old.reset();if(oldWindow)DestroyWindow(oldWindow);
            m_upscalingError.clear();m_upscalingRequested=true;m_upscaleTargetHeight=height;
            LOG("Playback SR enabled: "<<m_decoder.Width()<<"x"<<m_decoder.Height()<<" -> "<<size.width<<"x"<<size.height);
        }else{
            m_upscalingError=L"SR could not start; previous playback preserved";
            LOG("Playback SR candidate rejected; existing renderer preserved.");
        }
        candidate.reset();m_currentSec=position;m_playStartSec=position;m_playStart=Clock::now();m_playing=playing;Audio().Pause(!playing);
        UpdateCachedStatus();InvalidateControls();return ready;
    }
    void RestoreUpscaling(){
        if(m_upscalingRequested&&UpscalingTarget(m_decoder.Width(),m_decoder.Height(),m_upscaleTargetHeight).grows)
            EnableUpscaling(m_upscaleTargetHeight);
    }
    void ToggleUpscaling(){
        if(!ToolbarActionEnabled(ToolbarAction::ToggleUpscaling))return;
        if(UpscalingActive()){
            m_upscalingRequested=false;m_renderer->SetDLSS(false);m_upscalingError.clear();
            RenderVideoFrame(m_lastPlaybackFrame,true);UpdateCachedStatus();InvalidateControls();
        }else if(!EnableUpscaling(m_upscaleTargetHeight)){
            MessageBoxW(m_hwnd,L"DLSS upscaling could not start at this output size. Your current video is unchanged. See DLSSVideoPlayer.log for details.",L"DLSS Upscaling",MB_OK|MB_ICONINFORMATION);
        }
    }
    void SetUpscaleTarget(uint32_t height){
        if((height!=1440&&height!=2160)||m_seeking||m_seekPending||NeuralJobActive()||m_youtubeLifecycle.IsResolving())return;
        if(UpscalingActive()){
            if(UpscalingTarget(m_decoder.Width(),m_decoder.Height(),height).grows){if(!EnableUpscaling(height))return;}
            else{m_renderer->SetDLSS(false);RenderVideoFrame(m_lastPlaybackFrame,true);}
        }
        m_upscaleTargetHeight=height;m_upscalingError.clear();UpdateCachedStatus();InvalidateControls();
    }

    bool RenderVideoFrame(const VideoFrame& f,bool resetGuide) {
        if(!m_renderer)return false; GuideFrame g;
        // Translate the legacy reset flags into a named reason: a fresh load is
        // the first frame; a seek/reload reset outranks a decoder discontinuity,
        // which outranks a dropped frame.
        HistoryReset reason=HistoryReset::None;
        if(m_lastRenderedTs<0)reason=HistoryReset::FirstFrame;
        else if(m_guideReset||m_dlssReset)reason=HistoryReset::Seek;
        else if(resetGuide)reason=f.discontinuity?HistoryReset::Seek:HistoryReset::Drop;
        if(!m_guides.Generate(f.bgra.data(),m_decoder.Width(),m_decoder.Height(),m_renderer->DLSSInputW(),m_renderer->DLSSInputH(),m_decoder.FrameRate(),IdentityOf(f,m_historyGeneration,0,reason),g))return false;
        m_historyGeneration=g.id.historyGeneration;
        float ms=float(1000.0/std::max(1.0,m_decoder.FrameRate()));
        if(m_lastRenderedTs>=0 && f.timestamp100ns>m_lastRenderedTs){double d=double(f.timestamp100ns-m_lastRenderedTs)*1e-4;if(d>0.1&&d<500.0)ms=float(d);}
        bool r=m_dlssReset||!g.hasHistory;
        if(m_cachedPlayback){if(const auto* pair=m_synchronizedPlayback.CurrentPair())UploadComparisonReference(pair->original);}
        bool ok=m_renderer->RenderFrame(f.bgra.data(),f.bgra.size(),g.guideGridRGBA32F.data(),g.guideGridRGBA32F.size()*sizeof(float),g.gridW,g.gridH,r,ms);
        if(ok){
            m_lastPlaybackFrame=f;
            if(m_renderer->DLSSEnabled()&&!m_renderer->LastFrameUsedDLSS()){
                m_renderer->SetDLSS(false);m_upscalingRequested=false;
                m_upscalingError=L"SR failed; original scaling restored";
                LOG("Runtime SR evaluation failed; disabled without stopping playback.");
                InvalidateControls();
            }
        }
        m_lastRenderedTs=f.timestamp100ns;m_lastGlobalX=g.globalMotionX;m_lastGlobalY=g.globalMotionY;return ok;
    }

    static std::pair<uint32_t,uint32_t> RecommendedDecodeSize(uint32_t nw,uint32_t nh,uint32_t ow,uint32_t oh,NVSDK_NGX_PerfQuality_Value q) {
        if(!nw||!nh||!ow||!oh||q==NVSDK_NGX_PerfQuality_Value_DLAA)return{nw,nh};
        double scale=2.0/3.0;
        if(q==NVSDK_NGX_PerfQuality_Value_Balanced)scale=0.58;
        else if(q==NVSDK_NGX_PerfQuality_Value_MaxPerf)scale=0.50;
        else if(q==NVSDK_NGX_PerfQuality_Value_UltraPerformance)scale=1.0/3.0;
        uint32_t tw=std::max(2u,uint32_t(std::lround(double(ow)*scale))&~1u);
        uint32_t th=std::max(2u,uint32_t(std::lround(double(oh)*scale))&~1u);
        // Never decode-upscale a smaller movie just to feed DLSS. The renderer/NGX
        // policy will preserve the genuine reconstruction distance for low-res sources.
        if(uint64_t(nw)*nh<=uint64_t(tw)*th)return{nw,nh};
        return{tw,th};
    }

    static NVSDK_NGX_PerfQuality_Value AutoQuality(uint32_t sw,uint32_t sh,uint32_t ow,uint32_t oh,double fps) {
        if(!sw||!sh||!ow||!oh) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        const double scale=std::sqrt((double(sw)*double(sh))/(double(ow)*double(oh)));
        // Realtime policy: when the movie already matches the output resolution, DLAA
        // needlessly evaluates DLSS at full output resolution.  Auto instead performs a
        // genuine DLSS upscale.  4K high-frame-rate video starts at Balanced; otherwise
        // Quality. Users can still explicitly select DLAA from the DLSS menu.
        if(scale>=0.90) {
            const uint64_t outPixels=uint64_t(ow)*uint64_t(oh);
            if(outPixels>=uint64_t(3840)*2160 && fps>=45.0) return NVSDK_NGX_PerfQuality_Value_Balanced;
            return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        }
        struct C{double s;NVSDK_NGX_PerfQuality_Value q;};
        const C cands[]={{2.0/3.0,NVSDK_NGX_PerfQuality_Value_MaxQuality},{0.58,NVSDK_NGX_PerfQuality_Value_Balanced},{0.50,NVSDK_NGX_PerfQuality_Value_MaxPerf},{1.0/3.0,NVSDK_NGX_PerfQuality_Value_UltraPerformance}};
        double best=1e9;NVSDK_NGX_PerfQuality_Value q=NVSDK_NGX_PerfQuality_Value_MaxQuality;
        for(const auto& c:cands){double e=std::abs(std::log(std::max(scale,0.05)/c.s));if(e<best){best=e;q=c.q;}}
        return q;
    }
    static const wchar_t* QualityNameW(NVSDK_NGX_PerfQuality_Value q){switch(q){case NVSDK_NGX_PerfQuality_Value_MaxPerf:return L"Performance";case NVSDK_NGX_PerfQuality_Value_Balanced:return L"Balanced";case NVSDK_NGX_PerfQuality_Value_UltraPerformance:return L"UltraPerf";case NVSDK_NGX_PerfQuality_Value_DLAA:return L"DLAA";default:return L"Quality";}}
    static const char* QualityNameA(NVSDK_NGX_PerfQuality_Value q){switch(q){case NVSDK_NGX_PerfQuality_Value_MaxPerf:return "Performance";case NVSDK_NGX_PerfQuality_Value_Balanced:return "Balanced";case NVSDK_NGX_PerfQuality_Value_UltraPerformance:return "UltraPerf";case NVSDK_NGX_PerfQuality_Value_DLAA:return "DLAA";default:return "Quality";}}

    static std::pair<uint32_t,uint32_t> OutputForAspect(double dar,uint32_t maxW,uint32_t maxH) {
        double box=double(maxW)/maxH;uint32_t w,h;if(dar>=box){w=maxW;h=uint32_t(std::lround(double(w)/dar));}else{h=maxH;w=uint32_t(std::lround(double(h)*dar));}
        w=std::max(64u,w&~1u);h=std::max(64u,h&~1u);return{w,h};
    }

    double Position() const {
        if(!m_loaded)return 0;if(!m_playing)return m_currentSec;
        double audio=Audio().PositionSeconds();
        if(audio>=0.0){double d=m_decoder.DurationSeconds();return d>0?std::clamp(audio,0.0,d):audio;}
        double s=m_playStartSec+std::chrono::duration<double>(Clock::now()-m_playStart).count();double d=m_decoder.DurationSeconds();return d>0?std::clamp(s,0.0,d):std::max(0.0,s);
    }

    // A cached range entry only holds [start,end); seeking outside it would
    // desynchronize the pair, so the timeline is clamped to the last range frame.
    double ClampSeek(double sec)const{
        double low=0.0,high=m_decoder.DurationSeconds();
        // Seeking to the container end has no frame to decode: the restarted
        // decoder returns nothing and the seek pays for a second restart.
        if(const int64_t last=LastFramePts(m_decoder.FrameRate(),SourceDuration100ns());last>0)high=std::min(high,double(last)*1e-7);
        // An active session can only serve what is rendered: seeking stops at the
        // head, and a target past it would wait for frames that do not exist yet.
        if(m_cachedPlayback&&m_liveSession&&m_liveSegments){low=double(m_cachedRange.start100ns)*1e-7;high=std::max(low,double(m_liveSegments->Head100ns())*1e-7-1.0/std::max(1.0,m_decoder.FrameRate()));}
        else if(m_cachedPlayback&&!m_cachedRange.Whole()){low=double(m_cachedRange.start100ns)*1e-7;high=std::max(low,double(m_cachedRange.end100ns)*1e-7-1.0/std::max(1.0,m_decoder.FrameRate()));}
        if(high>0)return std::clamp(sec,low,high);return std::max(low,sec);
    }

    void RequestSeek(double sec) {
        const bool resume=m_seekPending?m_seekResumePlaying:m_playing; RequestSeek(sec,resume);
    }

    void RequestSeek(double sec,bool resumeAfter) {
        if(m_decoder.IsStillImage()){sec=0.0;resumeAfter=false;}
        if(!m_loaded)return;sec=ClampSeek(sec);if(NetworkPlayback()){StartYouTubeSeek(sec,resumeAfter);return;}
        if(!m_seekPending) m_currentSec=Position();
        m_pendingSeekSec=sec;m_seekResumePlaying=resumeAfter;m_seekPending=true;m_playing=false;Audio().Pause(true);m_seekPreview=sec;InvalidateControls();InvalidatePlaybackProgress();UpdateCachedStatus();
    }

    bool PerformSeek(double sec,bool resumeAfter) {
        if(!m_loaded||m_seeking)return false;SetSeeking(true);m_previewShown=false;CancelPausedSettingsPreview();sec=ClampSeek(sec);LOG("Seek begin target="<<sec<<" resume="<<resumeAfter);
        // Seek is deliberately transactional and performed from Tick(), never from a mouse message.
        // Shut down the audio producer first, wait for GPU work, then restart the video decoder.
        Audio().Stop();
        if(m_renderer&&m_renderer->WaitGPU()!=d3d12_renderer_detail::FenceWaitResult::Completed){
            LOG("Seek aborted after GPU synchronization failure.");
            Unload();
            return false;
        }
        if(m_cachedPlayback){
            m_haveNext=false;m_next=VideoFrame{};m_synchronizedPlayback.SetPaused(false);
            if(!m_synchronizedPlayback.SeekSeconds(sec)||!m_synchronizedPlayback.VisibleFrame()){
                // A live pair only holds what is rendered. Outside it the
                // original takes the frame back and the session rebases there.
                if(m_liveSession){LOG("Live seek to "<<sec<<" s is not rendered; handing playback back to the original.");DetachLivePlayback();SetSeeking(false);RequestSeek(sec,resumeAfter);return false;}
                LOG("Cached seek failed transactionally; invalidating synchronized playback.");Unload();return false;
            }
            m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;const VideoFrame frame=*m_synchronizedPlayback.VisibleFrame();
            if(!RenderVideoFrame(frame,true)){m_playing=false;m_synchronizedPlayback.SetPaused(true);SetSeeking(false);return false;}RememberRenderedCachedPair();
            // A drag preview would respawn the audio helper on every step; the
            // release restarts it once.
            m_currentSec=double(frame.timestamp100ns)*1e-7;
            if(!m_dragSeek){const bool audioOk=Audio().Start(m_path,m_currentSec);if(audioOk){Audio().SetVolume(m_muted?0.0f:m_volume);Audio().Pause(!resumeAfter);}}
            m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=resumeAfter;m_synchronizedPlayback.SetPaused(!resumeAfter);m_guideReset=false;m_dlssReset=false;SetSeeking(false);UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();return true;
        }
        m_haveNext=false;m_next=VideoFrame{};
        auto readAt=[&](double target,VideoFrame& frame)->bool{
            if(!m_decoder.SeekSeconds(target))return false;
            if(m_decoder.ReadNext(frame))return true;
            const double dur=m_decoder.DurationSeconds(),fd=1.0/std::max(1.0,m_decoder.FrameRate());
            if(dur>0.0&&target>0.0){const double safe=std::max(0.0,std::min(target,dur-fd*1.5));if(safe<target&&m_decoder.SeekSeconds(safe)&&m_decoder.ReadNext(frame))return true;}
            return false;
        };
        VideoFrame f; bool got=readAt(sec,f);
        if(!got){
            LOG("Seek decoder restart failed; reopening the same file for recovery.");
            m_decoder.Close(); if(m_decoder.Open(m_path,DecodeKind()))got=readAt(sec,f);
        }
        if(!got){
            LOG("Seek failed without crashing; playback remains paused.");m_playing=false;SetSeeking(false);m_currentSec=sec;UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();return false;
        }
        m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;
        if(!RenderVideoFrame(f,true)){LOG("Seek frame render failed.");m_playing=false;SetSeeking(false);return false;}
        m_currentSec=double(f.timestamp100ns)*1e-7;m_haveNext=m_decoder.ReadNext(m_next);
        if(!m_dragSeek){const bool audioOk=Audio().Start(m_path,m_currentSec);if(audioOk){Audio().SetVolume(m_muted?0.0f:m_volume);Audio().Pause(!resumeAfter);}else LOG("Seek: no audio stream/output; using steady-clock video pacing.");}
        m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=resumeAfter&&m_haveNext;m_guideReset=false;m_dlssReset=false;SetSeeking(false);UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();LOG("Seek complete actual="<<m_currentSec);return true;
    }

    void SetPaused(bool pause){if(!m_loaded||m_seeking)return;if(pause==!m_playing)return;if(pause){m_currentSec=playback_timing::PausePosition(m_currentSec);m_playing=false;Audio().Pause(true);if(m_cachedPlayback)m_synchronizedPlayback.SetPaused(true);}else{if(!m_cachedPlayback&&!NetworkPlayback()&&!m_haveNext&&m_decoder.DurationSeconds()>0){RequestSeek(0,true);return;}m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=true;Audio().Pause(false);if(m_cachedPlayback)m_synchronizedPlayback.SetPaused(false);}InvalidateControls();InvalidatePlaybackProgress();}
    // Space pauses playback during an active session; the render behind it keeps
    // filling the buffer, and only an offline job takes the pause event.
    void TogglePause(){if(NeuralJobActive()&&!JobBehindPlayback()){SetNeuralJobPaused(!NeuralJobPaused());return;}if(m_liveBuffering){m_liveResumePlaying=!m_liveResumePlaying;InvalidateControls();return;}CancelPausedSettingsPreview();SetPaused(m_playing);}
    void StepCachedFrame(){
        if(!m_loaded||!m_cachedPlayback||m_playing||m_seeking)return;
        Audio().Pause(true);
        VideoFrame frame;
        if(m_haveNext){frame=std::move(m_next);m_next={};m_haveNext=false;}
        else{
            if(!m_synchronizedPlayback.Step())return;
            const auto read=m_synchronizedPlayback.ReadNextAvailable();
            if(read!=SynchronizedReadResult::PairReady||!m_synchronizedPlayback.VisibleFrame())return;
            frame=*m_synchronizedPlayback.VisibleFrame();
        }
        if(!RenderVideoFrame(frame,frame.discontinuity))return;
        RememberRenderedCachedPair();++m_cachedPresentedFrames;
        m_currentSec=double(frame.timestamp100ns)*1e-7;
        Audio().Stop();
        if(Audio().Start(m_path,m_currentSec)){
            Audio().SetVolume(m_muted?0.0f:m_volume);Audio().Pause(true);
        }
        m_playStartSec=m_currentSec;m_playStart=Clock::now();
        InvalidatePlaybackProgress();UpdateCachedStatus();
    }
    // Stop ends an active session the same way the toggle does: the render dies
    // and the original keeps the frame, instead of a cancelled job leaving the
    // session state behind.
    void StopPlayback(){if(m_liveSession){StopLiveNeuralSession(true);return;}if(NeuralJobActive()){CancelNeuralJob();return;}if(m_youtubeLifecycle.IsResolving()){CancelYouTubeResolution();return;}RequestSeek(0,false);}

    void UpdateTitle(){
        if(!m_hwnd)return;
        const std::wstring appTitle=T(L"app.title");
        const std::wstring title=BuildPlayerWindowTitle(appTitle,m_loaded?m_displayTitle:L"",120);
        if(title==m_cachedWindowTitle)return;
        m_cachedWindowTitle=title;SetWindowTextW(m_hwnd,title.c_str());
    }

    void Layout(){
        if(!m_hwnd||!m_viewport||!m_renderWnd)return;RECT c{};GetClientRect(m_hwnd,&c);int W=static_cast<int>(std::max<LONG>(1,c.right-c.left)),H=static_cast<int>(std::max<LONG>(1,c.bottom-c.top));
        if(!m_loaded){MoveWindow(m_viewport,0,0,W,H,TRUE);ReconcileFocusForCurrentLayout();RefreshHoverForCurrentLayout();InvalidateControls();return;}
        int areaH=std::max(1,H-ControlHeight());MoveWindow(m_viewport,0,0,W,areaH,TRUE);double ar=m_dar>0?m_dar:16.0/9.0;double areaAr=double(W)/areaH;int rw=0,rh=0;
        if(m_fill){if(areaAr>ar){rw=W;rh=int(std::lround(W/ar));}else{rh=areaH;rw=int(std::lround(areaH*ar));}}else{if(areaAr>ar){rh=areaH;rw=int(std::lround(areaH*ar));}else{rw=W;rh=int(std::lround(W/ar));}}
        SetWindowPos(m_renderWnd,nullptr,(W-rw)/2,(areaH-rh)/2,std::max(1,rw),std::max(1,rh),SWP_NOZORDER|SWP_NOACTIVATE);
        ReconcileFocusForCurrentLayout();RefreshHoverForCurrentLayout();InvalidateRect(m_viewport,nullptr,FALSE);InvalidateControls();
    }

    RECT TimelineRect()const{if(!ControlsVisible())return {};RECT c{};GetClientRect(m_hwnd,&c);return RECT{Dip(18),c.bottom-Dip(28),c.right-Dip(18),c.bottom-Dip(14)};}
    IdleSurfaceLayout IdleLayout()const{RECT c{};GetClientRect(m_hwnd,&c);return LayoutIdleSurface(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd));}
    std::vector<ToolbarItem> ToolbarItems()const{if(!ControlsVisible())return {};RECT c{};GetClientRect(m_hwnd,&c);return LayoutToolbar(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd));}
    std::vector<ToolbarItem> FocusableItems()const{if(m_loaded)return ToolbarItems();const auto idle=IdleLayout();return{idle.actions.begin(),idle.actions.end()};}
    ToolbarAvailability ToolbarState()const{return{m_loaded,m_seeking||m_seekPending,m_renderer!=nullptr,YouTubePlaybackAvailable(),m_youtubeLifecycle.IsResolving()||(NeuralJobActive()&&!JobBehindPlayback()),m_cachedPlayback&&m_havePresentedPair&&m_renderer!=nullptr,m_liveSession||LiveSessionAvailable(),UpscalingAvailable(),false};}
    std::optional<RECT> VolumeRect()const{if(!ControlsVisible())return std::nullopt;RECT c{};GetClientRect(m_hwnd,&c);const auto items=ToolbarItems();return LayoutVolumeSlider(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd),items);}
    bool PtIn(const RECT&r,int x,int y)const{return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}

    RECT TimeTextRect()const{RECT c{};GetClientRect(m_hwnd,&c);return RECT{Dip(16),c.bottom-Dip(55),Dip(142),c.bottom-Dip(32)};}
    RECT StatusRect()const{RECT c{};GetClientRect(m_hwnd,&c);return RECT{Dip(143),c.bottom-Dip(55),VolumeRect()?c.right-Dip(203):c.right-Dip(16),c.bottom-Dip(32)};}
    void InvalidatePlaybackProgress(){
        if(!m_hwnd||!m_loaded||!ControlsVisible())return;
        RECT timeline=TimelineRect();InflateRect(&timeline,Dip(6),Dip(4));InvalidateRect(m_hwnd,&timeline,FALSE);
        const RECT time=TimeTextRect();InvalidateRect(m_hwnd,&time,FALSE);
    }
    void InvalidateVolumeControls(){
        if(!m_hwnd)return;const auto volume=VolumeRect();if(!volume)return;
        RECT dirty=*volume;InflateRect(&dirty,Dip(7),Dip(9));RECT c{};GetClientRect(m_hwnd,&c);dirty.right=c.right-Dip(16);InvalidateRect(m_hwnd,&dirty,FALSE);
    }
    void InvalidateToolbarAction(ToolbarAction action){
        if(!m_hwnd||action==ToolbarAction::None)return;const auto items=FocusableItems();
        for(const auto& item:items)if(item.action==action){InvalidateRect(m_hwnd,&item.bounds,FALSE);return;}
    }
    void UpdateCachedStatus(){
        const std::wstring status=BuildStatusText();if(status==m_cachedStatus)return;
        m_cachedStatus=status;if(m_hwnd){if(m_loaded){const RECT dirty=StatusRect();InvalidateRect(m_hwnd,&dirty,FALSE);}else InvalidateRect(m_hwnd,nullptr,FALSE);}
    }
    ToolbarAction ToolbarActionAt(int x,int y)const{
        const auto items=FocusableItems();return ResolveToolbarHover(items,POINT{x,y},ToolbarState());
    }
    void SetHoverAction(ToolbarAction action){
        if(action==m_hoverAction)return;const auto items=FocusableItems();
        const auto dirty=HoverDirtyRectangles(items,m_hoverAction,action);m_hoverAction=action;
        for(const RECT& rect:dirty)InvalidateRect(m_hwnd,&rect,FALSE);
    }
    void RefreshHoverForCurrentLayout(){
        POINT cursor{};std::optional<POINT> clientPoint;
        if(GetCursorPos(&cursor)&&ScreenToClient(m_hwnd,&cursor))clientPoint=cursor;
        const auto items=FocusableItems();SetHoverAction(ResolveToolbarHoverForCursor(items,clientPoint,ToolbarState()));
    }

    void ReconcileFocusForCurrentLayout(){
        const auto items=FocusableItems();m_focusedToolbarAction=ReconcileFocusedToolbarAction(items,m_focusedToolbarAction,ToolbarState());
    }

    void SetSeeking(bool seeking){
        if(m_seeking==seeking)return;m_seeking=seeking;ReconcileFocusForCurrentLayout();RefreshHoverForCurrentLayout();InvalidateControls();
    }

    struct ToolbarButtonContent{UiIcon icon;std::wstring label;bool enabled;bool active;};

    ToolbarButtonContent ButtonContent(ToolbarAction action,bool idleSurface=false)const{
        const bool rendererReady=m_renderer!=nullptr;
        const bool enabled=IsToolbarActionEnabled(action,ToolbarState());
        switch(action){
        case ToolbarAction::Open:return{UiIcon::Open,T(OpenActionLabelKey(idleSurface).data()),enabled,false};
        case ToolbarAction::OpenYouTube:return{UiIcon::YouTube,T(L"idle.youtube"),enabled,false};
        case ToolbarAction::Back10:return{UiIcon::Rewind,L"10s",enabled,false};
        case ToolbarAction::PlayPause:return{m_playing?UiIcon::Pause:UiIcon::Play,m_playing?L"Pause":L"Play",enabled,m_playing};
        case ToolbarAction::Stop:return{UiIcon::Stop,L"Stop",enabled,false};
        case ToolbarAction::Forward10:return{UiIcon::FastForward,L"10s",enabled,false};
        case ToolbarAction::Mute:return{m_muted?UiIcon::VolumeOff:UiIcon::Volume,m_muted?L"Sound":L"Mute",enabled,m_muted};
        case ToolbarAction::ToggleNeuralRendering:{const bool active=(m_cachedPlayback&&m_comparisonView==ComparisonView::Neural)||m_previewShown;const bool cachedPair=m_cachedPlayback&&m_havePresentedPair&&rendererReady;const std::wstring label=m_previewJob?L"Neural Rendering · Previewing settings":m_previewShown?L"Neural Rendering · Settings preview":enabled?(active?L"Neural Rendering · On":L"Neural Rendering · Off"):(cachedPair?std::wstring(L"Neural Rendering · Seeking · ")+(active?L"On":L"Off"):(NeuralJobActive()?L"Neural Rendering · Preparing cache":L"Neural Rendering · No cache"));return{UiIcon::Sparkles,label,enabled,active};}
        case ToolbarAction::ToggleUpscaling:return{UiIcon::Sparkles,UpscalingAvailable()?(UpscalingActive()?L"DLSS Upscaling · On":L"DLSS Upscaling · Off"):L"DLSS Upscaling · Unavailable",enabled,UpscalingActive()};
        case ToolbarAction::ToggleFrameGeneration:return{UiIcon::Sparkles,L"Frame Generation · Unavailable",false,false};
        case ToolbarAction::Aspect:return{UiIcon::Crop,m_fill?L"Fit":L"Fill",enabled,m_fill};
        case ToolbarAction::Adjustments:return{UiIcon::Adjustments,L"Color",enabled,m_adjustWnd!=nullptr};
        case ToolbarAction::DebugView:{const bool active=rendererReady&&m_renderer->GetDebugView()!=D3D12Renderer::DebugView::Final;return{UiIcon::Debug,L"Debug",enabled,active};}
        case ToolbarAction::Fullscreen:return{UiIcon::Maximize,L"Full",enabled,m_fullscreen};
        case ToolbarAction::None:break;
        }
        return{UiIcon::Warning,L"Unavailable",false,false};
    }

    bool ToolbarActionEnabled(ToolbarAction action)const{return IsToolbarActionEnabled(action,ToolbarState());}

    void DrawButton(HDC dc,ToolbarAction action,UiIcon icon,const std::wstring&label,const RECT&r,bool enabled,bool active,bool hover,bool pressed,bool focus,bool compact){
        const ButtonVisual visual=ResolveButtonVisual(ButtonState{enabled,active,hover,pressed,focus});
        HBRUSH brush=CreateSolidBrush(visual.fill);HPEN pen=CreatePen(PS_SOLID,1,visual.border);
        const HGDIOBJ oldBrush=SelectObject(dc,brush),oldPen=SelectObject(dc,pen);
        const int radius=std::max(1,Dip(kToolbarCornerRadiusDip));
        RoundRect(dc,r.left,r.top,r.right,r.bottom,radius*2,radius*2);
        SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(brush);DeleteObject(pen);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,visual.text);
        const wchar_t glyph=GlyphForIcon(icon);const bool showIcon=ResolveButtonPresentation(m_iconFont!=nullptr)==ButtonPresentation::IconAndLabel&&glyph!=L'\0';
        const bool stacked=compact&&showIcon;SIZE iconSize{},textSize{};
        if(showIcon){const HGDIOBJ measured=SelectObject(dc,m_iconFont);GetTextExtentPoint32W(dc,&glyph,1,&iconSize);SelectObject(dc,measured);}
        const HFONT textFont=m_fontSmall?m_fontSmall:m_font;const HGDIOBJ measuredText=SelectObject(dc,textFont);
        GetTextExtentPoint32W(dc,label.c_str(),static_cast<int>(label.size()),&textSize);SelectObject(dc,measuredText);
        const bool featureLabel=action==ToolbarAction::ToggleNeuralRendering||action==ToolbarAction::ToggleUpscaling||action==ToolbarAction::ToggleFrameGeneration;
        const int neededWidth=iconSize.cx+textSize.cx+Dip(2*kButtonHorizontalInsetDip+kButtonIconLabelGapDip);
        const bool showText=!showIcon||featureLabel||neededWidth<=r.right-r.left;
        const ButtonContentLayout content=LayoutButtonContent(r,iconSize,showText?textSize:SIZE{},stacked&&showText,ActiveWindowDpi(m_hwnd));
        if(showIcon){const HGDIOBJ oldFont=SelectObject(dc,m_iconFont);RECT iconRect=content.icon;DrawTextW(dc,&glyph,1,&iconRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);SelectObject(dc,oldFont);}
        if(showText){const HGDIOBJ oldFont=SelectObject(dc,textFont);RECT textRect=content.text;
            DrawTextW(dc,label.c_str(),-1,&textRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            SelectObject(dc,oldFont);}
        if(visual.drawFocus&&action!=ToolbarAction::None){RECT focusRect=r;InflateRect(&focusRect,-Dip(3),-Dip(3));DrawFocusRect(dc,&focusRect);}
    }

    void DrawSolidEllipse(HDC dc,const RECT& bounds,COLORREF color,const char* stage){
        const int saved=SaveDC(dc);
        if(saved==0)LOG(stage<<" SaveDC failed winerr="<<GetLastError());
        HBRUSH brush=CreateSolidBrush(color);
        if(!brush){LOG(stage<<" CreateSolidBrush failed winerr="<<GetLastError());Ellipse(dc,bounds.left,bounds.top,bounds.right,bounds.bottom);if(saved!=0)RestoreDC(dc,saved);return;}
        SetLastError(ERROR_SUCCESS);const HGDIOBJ previous=SelectObject(dc,brush);
        if(!previous||previous==HGDI_ERROR){const DWORD error=GetLastError();LOG(stage<<" SelectObject(brush) failed winerr="<<error);Ellipse(dc,bounds.left,bounds.top,bounds.right,bounds.bottom);if(saved!=0)RestoreDC(dc,saved);if(!DeleteObject(brush))LOG(stage<<" DeleteObject(unselected brush) failed winerr="<<GetLastError());return;}
        Ellipse(dc,bounds.left,bounds.top,bounds.right,bounds.bottom);
        SetLastError(ERROR_SUCCESS);const HGDIOBJ restored=SelectObject(dc,previous);const bool explicitRestore=restored&&restored!=HGDI_ERROR;if(!explicitRestore)LOG(stage<<" restore previous brush failed winerr="<<GetLastError());
        bool stateRestore=false;if(saved!=0){SetLastError(ERROR_SUCCESS);stateRestore=RestoreDC(dc,saved)!=FALSE;if(!stateRestore)LOG(stage<<" RestoreDC failed winerr="<<GetLastError());}
        if(!explicitRestore&&!stateRestore){SetLastError(ERROR_SUCCESS);const HGDIOBJ emergency=SelectObject(dc,GetStockObject(NULL_BRUSH));if(!emergency||emergency==HGDI_ERROR)LOG(stage<<" emergency brush deselection failed winerr="<<GetLastError());}
        SetLastError(ERROR_SUCCESS);if(!DeleteObject(brush))LOG(stage<<" DeleteObject(brush) failed winerr="<<GetLastError());
    }

    void RenderUi(HDC dc,const RECT& c){
        m_neuralCancelBounds={};
        HBRUSH windowBg=CreateSolidBrush(ui_palette::Window);FillRect(dc,&c,windowBg);DeleteObject(windowBg);
        // An active session renders behind live playback, so it never takes the
        // window: its feedback is the buffering panel and the coverage lane.
        if((NeuralJobActive()&&!JobBehindPlayback())||(!m_loaded&&m_youtubeLifecycle.IsResolving())){
            const bool neural=NeuralJobActive();
            const PreRenderSurfaceLayout surface=LayoutPreRenderSurface(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd));m_neuralCancelBounds=surface.cancelButton;
            const bool paused=neural&&m_neuralLifecycle.state==NeuralPlaybackState::Paused;
            const uint64_t completed=neural?m_neuralProgress.completedFrames:0,total=neural?m_neuralProgress.totalFrames:0;
            const auto visual=ResolveActivityVisual(surface.progressTrack,ActivityElapsedMs(),completed,total,
                neural&&m_neuralProgress.phase==NeuralRenderPhase::NeuralRendering,m_activityMotionEnabled&&!paused);
            DrawActivitySpinner(dc,surface.spinner,visual.spinnerStep);
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(242,243,245));HGDIOBJ oldFont=SelectObject(dc,m_font);
            std::wstring title=neural?(m_pendingNeuralTitle.empty()?L"Preparing playback":m_pendingNeuralTitle):(m_pendingYouTubeTitle.empty()?L"YouTube video":m_pendingYouTubeTitle);RECT row=surface.title;DrawTextW(dc,title.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            SelectObject(dc,m_fontSmall);SetTextColor(dc,ui_palette::SecondaryText);
            const std::wstring phase=!neural?L"Loading YouTube video":paused?T(L"neural.phase.paused"):NeuralPhaseText(m_neuralProgress);row=surface.phase;DrawTextW(dc,phase.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            const std::wstring resolution=neural?(m_neuralSourceWidth?std::to_wstring(m_neuralSourceWidth)+L" × "+std::to_wstring(m_neuralSourceHeight):L"Reading source metadata…"):L"Finding a playable source…";row=surface.resolution;DrawTextW(dc,resolution.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            const std::wstring frames=total?(!visual.indeterminate?std::to_wstring(visual.percent)+L"% · ":L"")+std::to_wstring(completed)+L" / "+std::to_wstring(total)+L" frames":L"";row=surface.frameCount;DrawTextW(dc,frames.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            const auto elapsed=std::max<uint64_t>(ActivityElapsedMs()/1000,neural?static_cast<uint64_t>(std::max<int64_t>(0,m_neuralProgress.elapsed.count()/1000)):0);
            const auto eta=neural?std::chrono::duration_cast<std::chrono::seconds>(m_neuralProgress.estimatedRemaining).count():0;
            const std::wstring timing=L"Elapsed "+TimeText(double(elapsed))+(eta>0?L" · ETA "+TimeText(double(eta)):L"");row=surface.elapsedEta;DrawTextW(dc,timing.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            const std::wstring bytes=!neural?L"Playback starts when the video is ready":m_neuralProgress.phase==NeuralRenderPhase::CheckingCache?T(L"neural.cache.checking"):(m_neuralProgress.bytes?std::to_wstring(m_neuralProgress.bytes/(1024*1024))+L" MiB written":L"Preparing encoder…");row=surface.size;DrawTextW(dc,bytes.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            HBRUSH track=CreateSolidBrush(RGB(68,71,77));FillRect(dc,&surface.progressTrack,track);DeleteObject(track);HBRUSH progressBrush=CreateSolidBrush(ui_palette::PrimaryBlue);FillRect(dc,&visual.fill,progressBrush);DeleteObject(progressBrush);
            SelectObject(dc,oldFont);DrawButton(dc,ToolbarAction::None,UiIcon::Stop,T(L"neural.cancel"),surface.cancelButton,true,false,PtIn(surface.cancelButton,m_mouseX,m_mouseY),false,false,false);return;
        }
        if(!m_loaded){
            const IdleSurfaceLayout idle=IdleLayout();
            SetBkMode(dc,TRANSPARENT);
            SetTextColor(dc,RGB(242,243,245));auto of=SelectObject(dc,m_font);std::wstring tt=T(L"idle.title");RECT title=idle.title;DrawTextW(dc,tt.c_str(),-1,&title,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            SetTextColor(dc,ui_palette::SecondaryText);SelectObject(dc,m_fontSmall);std::wstring ss=m_youtubeLifecycle.IsResolving()?m_cachedStatus:T(L"idle.subtitle");RECT subtitle=idle.subtitle;DrawTextW(dc,ss.c_str(),-1,&subtitle,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);SelectObject(dc,of);
            for(const auto& item:idle.actions){const auto content=ButtonContent(item.action,true);DrawButton(dc,item.action,content.icon,content.label,item.bounds,content.enabled,false,content.enabled&&m_hoverAction==item.action,m_pressedToolbarAction==item.action,GetFocus()==m_hwnd&&m_focusedToolbarAction==item.action,false);}
            if(!YouTubePlaybackAvailable()){SetBkMode(dc,TRANSPARENT);SetTextColor(dc,ui_palette::SecondaryText);of=SelectObject(dc,m_fontSmall);const bool compactReason=idle.subtitle.top==idle.subtitle.bottom;std::wstring reason=T(compactReason?L"idle.youtube_unavailable_compact":L"idle.youtube_unavailable");RECT reasonRect=idle.youtubeReason;DrawTextW(dc,reason.c_str(),-1,&reasonRect,DT_CENTER|DT_TOP|DT_WORDBREAK|DT_END_ELLIPSIS|DT_NOPREFIX);SelectObject(dc,of);}return;
        }
        if(!ControlsVisible())return;
        RECT bar{0,c.bottom-ControlHeight(),c.right,c.bottom};HBRUSH bg=CreateSolidBrush(ui_palette::ControlSurface);FillRect(dc,&bar,bg);DeleteObject(bg);HPEN line=CreatePen(PS_SOLID,1,RGB(54,56,61));auto op=SelectObject(dc,line);MoveToEx(dc,0,bar.top,nullptr);LineTo(dc,c.right,bar.top);SelectObject(dc,op);DeleteObject(line);
        const auto toolbarItems=ToolbarItems();
        for(const auto& item:toolbarItems){const auto content=ButtonContent(item.action);const bool hover=content.enabled&&m_hoverAction==item.action;DrawButton(dc,item.action,content.icon,content.label,item.bounds,content.enabled,content.active,hover,m_pressedToolbarAction==item.action,GetFocus()==m_hwnd&&m_focusedToolbarAction==item.action,item.compact);}
        const auto volumeRect=LayoutVolumeSlider(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd),toolbarItems);if(volumeRect){const RECT& vr=*volumeRect;HPEN vp=CreatePen(PS_SOLID,std::max(1,Dip(4)),RGB(94,98,105));op=SelectObject(dc,vp);MoveToEx(dc,vr.left,(vr.top+vr.bottom)/2,nullptr);LineTo(dc,vr.right,(vr.top+vr.bottom)/2);SelectObject(dc,op);DeleteObject(vp);int vx=vr.left+int((vr.right-vr.left)*(m_muted?0.0f:m_volume));const int knob=std::max(3,Dip(5));DrawSolidEllipse(dc,RECT{vx-knob,(vr.top+vr.bottom)/2-knob,vx+knob,(vr.top+vr.bottom)/2+knob},RGB(230,232,235),"Volume knob");}
        double shown=playback_timing::TimelinePosition(m_dragSeek,m_seekPreview,m_seekPending,m_pendingSeekSec,m_currentSec);RECT tr=TimelineRect();HBRUSH tb=CreateSolidBrush(RGB(68,71,77));FillRect(dc,&tr,tb);DeleteObject(tb);double d=m_decoder.DurationSeconds(),f=d>0?std::clamp(shown/d,0.0,1.0):0;
        const auto markerX=[&](int64_t pts){return tr.left+int(std::lround((tr.right-tr.left)*(d>0?std::clamp(double(pts)*1e-7/d,0.0,1.0):0.0)));};
        // Three lanes in one track, so no state hides another: the In/Out
        // selection on top (violet, the loudest because it is what the render
        // acts on), played progress in the middle, and the part of the source
        // that already has cached neural frames along the bottom (teal).
        const LONG height=tr.bottom-tr.top,coverageLane=std::max<LONG>(2,height/4);
        // A session paints coverage up to the render head, because that is how
        // far playback can go; a finished entry paints its whole range.
        const int64_t liveHead=m_liveSession&&m_liveSegments?m_liveSegments->Head100ns():0;
        RECT rendered=tr;const bool renderedSpan=m_cachedPlayback||liveHead>m_liveRange.start100ns;
        if(renderedSpan){
            if(m_liveSession){rendered.left=markerX(m_liveRange.start100ns);rendered.right=std::max<LONG>(rendered.left+1,markerX(liveHead));}
            else if(!m_cachedRange.Whole()){rendered.left=markerX(m_cachedRange.start100ns);rendered.right=std::max<LONG>(rendered.left+1,markerX(m_cachedRange.end100ns));}
            RECT band{rendered.left,tr.bottom-coverageLane,rendered.right,tr.bottom};
            HBRUSH nb=CreateSolidBrush(ui_palette::NeuralCoverage);FillRect(dc,&band,nb);DeleteObject(nb);
        }
        // Played progress is clamped to the rendered span only while playback is
        // actually on it; a session that has not attached yet still plays the
        // original anywhere in the source.
        const bool clampProgress=m_cachedPlayback;
        RECT done{clampProgress?rendered.left:tr.left,tr.top,0,renderedSpan?tr.bottom-coverageLane:tr.bottom};
        done.right=std::clamp<LONG>(static_cast<LONG>(tr.left+std::lround((tr.right-tr.left)*f)),done.left,clampProgress?rendered.right:tr.right);
        HBRUSH db=CreateSolidBrush(ui_palette::PrimaryBlue);FillRect(dc,&done,db);DeleteObject(db);
        if(renderedSpan&&(m_liveSession||!m_cachedRange.Whole())){HBRUSH eb=CreateSolidBrush(ui_palette::NeuralCoverage);RECT startEdge{rendered.left,tr.top,rendered.left+std::max(1,Dip(1)),tr.bottom},endEdge{rendered.right-std::max(1,Dip(1)),tr.top,rendered.right,tr.bottom};FillRect(dc,&startEdge,eb);FillRect(dc,&endEdge,eb);DeleteObject(eb);}
        // The selection is drawn last and fills the track, so it reads at a
        // glance; the coverage lane stays visible beneath it.
        if(m_markers.in100ns&&m_markers.out100ns&&*m_markers.out100ns>*m_markers.in100ns){
            RECT span{markerX(*m_markers.in100ns),tr.top,markerX(*m_markers.out100ns),renderedSpan?tr.bottom-coverageLane:tr.bottom};
            if(span.right<=span.left)span.right=span.left+std::max(1,Dip(1));
            HBRUSH sb=CreateSolidBrush(ui_palette::MarkedRange);FillRect(dc,&span,sb);DeleteObject(sb);
            RECT rail{span.left,tr.top,span.right,tr.top+std::max<LONG>(1,Dip(2))};HBRUSH rb=CreateSolidBrush(ui_palette::MarkedRangeEdge);FillRect(dc,&rail,rb);DeleteObject(rb);
        }
        const int tickWidth=std::max(2,Dip(3)),tickRise=Dip(8);
        const auto markerTick=[&](int64_t pts,COLORREF color){const int x=markerX(pts);RECT tick{x-tickWidth/2,tr.top-tickRise,x-tickWidth/2+tickWidth,tr.bottom+std::max(1,Dip(2))};HBRUSH mb=CreateSolidBrush(color);FillRect(dc,&tick,mb);DeleteObject(mb);};
        if(m_markers.in100ns)markerTick(*m_markers.in100ns,RGB(96,220,130));if(m_markers.out100ns)markerTick(*m_markers.out100ns,RGB(255,168,64));
        const int knobR=std::max(4,Dip(6));int kx=done.right;DrawSolidEllipse(dc,RECT{kx-knobR,tr.top-Dip(2),kx+knobR,tr.bottom+Dip(2)},RGB(246,246,248),"Timeline knob");
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(206,208,212));auto of=SelectObject(dc,m_fontSmall);std::wstring time=TimeText(shown)+L" / "+TimeText(d);TextOutW(dc,Dip(18),c.bottom-Dip(50),time.c_str(),int(time.size()));
        RECT sr{Dip(145),c.bottom-Dip(53),volumeRect?c.right-Dip(205):c.right-Dip(18),c.bottom-Dip(34)};
        if(m_youtubeLifecycle.IsResolving()){
            const RECT spinner{sr.left,sr.top,sr.left+Dip(18),sr.top+Dip(18)};
            DrawActivitySpinner(dc,spinner,ResolveActivityVisual({},ActivityElapsedMs(),0,0,false,m_activityMotionEnabled).spinnerStep);sr.left+=Dip(25);
        }
        DrawTextW(dc,m_cachedStatus.c_str(),-1,&sr,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);if(volumeRect){const RECT& vr=*volumeRect;std::wstring vol=m_muted?T(L"status.muted"):(T(L"status.volume")+L" "+std::to_wstring(int(m_volume*100))+L"%");TextOutW(dc,vr.right+Dip(8),vr.top-Dip(6),vol.c_str(),int(vol.size()));}SelectObject(dc,of);
    }

    void Paint(){
        PAINTSTRUCT ps{};SetLastError(ERROR_SUCCESS);HDC windowDc=BeginPaint(m_hwnd,&ps);
        if(!windowDc){const DWORD error=GetLastError();LOG("BeginPaint failed winerr="<<error);return;}
        const auto finishPaint=[&](){SetLastError(ERROR_SUCCESS);if(!EndPaint(m_hwnd,&ps)){const DWORD error=GetLastError();LOG("EndPaint failed winerr="<<error);}};
        RECT client{};if(!GetClientRect(m_hwnd,&client)){const DWORD error=GetLastError();LOG("GetClientRect during paint failed winerr="<<error);finishPaint();return;}
        const auto layout=LayoutPaintBuffer(client,ps.rcPaint);if(!layout){finishPaint();return;}

        HDC memoryDc=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ previousBitmap=nullptr;POINT previousOrigin{};bool bitmapSelected=false,viewportAdjusted=false,buffered=false;const char* failedStage=nullptr;DWORD failedError=ERROR_SUCCESS;
        const auto recordFailure=[&](const char* stage){if(!failedStage){failedStage=stage;failedError=GetLastError();}};

        SetLastError(ERROR_SUCCESS);memoryDc=CreateCompatibleDC(windowDc);if(!memoryDc)recordFailure("CreateCompatibleDC");
        if(!failedStage){SetLastError(ERROR_SUCCESS);bitmap=CreateCompatibleBitmap(windowDc,layout->width,layout->height);if(!bitmap)recordFailure("CreateCompatibleBitmap");}
        if(!failedStage){SetLastError(ERROR_SUCCESS);previousBitmap=SelectObject(memoryDc,bitmap);if(!previousBitmap||previousBitmap==HGDI_ERROR)recordFailure("SelectObject(bitmap)");else bitmapSelected=true;}
        if(!failedStage){SetLastError(ERROR_SUCCESS);if(!SetViewportOrgEx(memoryDc,layout->viewportOrigin.x,layout->viewportOrigin.y,&previousOrigin))recordFailure("SetViewportOrgEx");else viewportAdjusted=true;}
        if(!failedStage){RenderUi(memoryDc,client);SetLastError(ERROR_SUCCESS);if(!SetViewportOrgEx(memoryDc,previousOrigin.x,previousOrigin.y,nullptr))recordFailure("restore viewport origin");else viewportAdjusted=false;}
        if(!failedStage){SetLastError(ERROR_SUCCESS);if(!BitBlt(windowDc,layout->paintBounds.left,layout->paintBounds.top,layout->width,layout->height,memoryDc,0,0,SRCCOPY))recordFailure("BitBlt");else buffered=true;}

        if(viewportAdjusted){SetLastError(ERROR_SUCCESS);if(SetViewportOrgEx(memoryDc,previousOrigin.x,previousOrigin.y,nullptr))viewportAdjusted=false;else{const DWORD error=GetLastError();LOG("Buffered parent paint cleanup failed at viewport restore winerr="<<error);}}
        if(bitmapSelected){SetLastError(ERROR_SUCCESS);const HGDIOBJ restored=SelectObject(memoryDc,previousBitmap);if(restored&&restored!=HGDI_ERROR)bitmapSelected=false;else{const DWORD error=GetLastError();LOG("Buffered parent paint cleanup failed at bitmap deselection winerr="<<error);}}
        if(memoryDc){SetLastError(ERROR_SUCCESS);if(DeleteDC(memoryDc)){memoryDc=nullptr;bitmapSelected=false;}else{const DWORD error=GetLastError();LOG("Buffered parent paint cleanup failed at DeleteDC winerr="<<error);}}
        if(bitmap&&!bitmapSelected){SetLastError(ERROR_SUCCESS);if(DeleteObject(bitmap))bitmap=nullptr;else{const DWORD error=GetLastError();LOG("Buffered parent paint cleanup failed at DeleteObject(bitmap) winerr="<<error);}}
        if(bitmapSelected)LOG("Buffered parent paint retained a still-selected bitmap after DeleteDC failure");
        if(!buffered){LOG("Buffered parent paint fallback after "<<(failedStage?failedStage:"unknown failure")<<" winerr="<<failedError);RenderUi(windowDc,client);}
        finishPaint();
    }

    void RegisterOverlayHotkeys(){
        // WM_HOTKEY is posted by Windows independently of the swapchain WndProc.
        // This remains usable while ReShade owns/captures normal mouse/keyboard input.
        auto reg=[&](int id,UINT mods,UINT vk,const char* name){if(!RegisterHotKey(m_hwnd,id,mods|MOD_NOREPEAT,vk))LOG("Overlay hotkey unavailable: "<<name<<" winerr="<<GetLastError());};
        reg(HK_PLAY_PAUSE,MOD_CONTROL|MOD_ALT,VK_SPACE,"Ctrl+Alt+Space");
        reg(HK_BACK_10,MOD_CONTROL|MOD_ALT,VK_LEFT,"Ctrl+Alt+Left");
        reg(HK_FORWARD_10,MOD_CONTROL|MOD_ALT,VK_RIGHT,"Ctrl+Alt+Right");
        reg(HK_MUTE,MOD_CONTROL|MOD_ALT,'M',"Ctrl+Alt+M");
        reg(HK_DLSS,MOD_CONTROL|MOD_ALT,'D',"Ctrl+Alt+D");
        reg(HK_ADJUSTMENTS,MOD_CONTROL|MOD_ALT,'C',"Ctrl+Alt+C");
        if(!RegisterHotKey(m_hwnd,HK_MEDIA_PLAY_PAUSE,MOD_NOREPEAT,VK_MEDIA_PLAY_PAUSE))LOG("Media Play/Pause hotkey unavailable winerr="<<GetLastError());
    }
    void UnregisterOverlayHotkeys(){if(!m_hwnd)return;for(int id:{HK_PLAY_PAUSE,HK_BACK_10,HK_FORWARD_10,HK_MUTE,HK_DLSS,HK_ADJUSTMENTS,HK_MEDIA_PLAY_PAUSE})UnregisterHotKey(m_hwnd,id);}
    void HandleHotkey(int id){
        switch(id){case HK_PLAY_PAUSE:case HK_MEDIA_PLAY_PAUSE:TogglePause();break;case HK_BACK_10:RequestSeek(Position()-10);break;case HK_FORWARD_10:RequestSeek(Position()+10);break;case HK_MUTE:ToggleMute();break;case HK_DLSS:ToggleNeuralRendering();break;case HK_ADJUSTMENTS:ShowAdjustments();break;}
    }

    void OpenFromDialog(){if(!ToolbarActionEnabled(ToolbarAction::Open))return;auto p=PickVideoFile(m_hwnd,m_loc);if(!p.empty())Load(p);}
    void SyncSourceActionAvailability(){
        if(!m_hwnd)return;const ToolbarAvailability state=ToolbarState();HMENU menu=GetMenu(m_hwnd);
        SyncActivityFeedback();UpdateRecentMenu();
        if(menu){app_menu::UpdateSourceActionAvailability(menu,IsToolbarActionEnabled(ToolbarAction::Open,state),IsToolbarActionEnabled(ToolbarAction::OpenYouTube,state));DrawMenuBar(m_hwnd);}
        ReconcileFocusForCurrentLayout();RefreshHoverForCurrentLayout();InvalidateControls();UpdateCachedStatus();
    }
    void DrainYouTubeCompletions(){
        m_youtubeCompletions.Clear();
        if(!m_hwnd)return;MSG message{};while(PeekMessageW(&message,m_hwnd,WM_YOUTUBE_RESOLVED,WM_YOUTUBE_RESOLVED,PM_REMOVE)){}
    }
    void CancelYouTubeResolution(bool updateUi=true){
        const bool active=m_youtubeLifecycle.IsResolving()||m_youtubeWorker.joinable();
        if(!active)return;m_youtubeLifecycle.Invalidate();
        if(m_youtubeWorker.joinable()){
            ExecuteYouTubeCancellationSequence(
                [&]{m_youtubeWorker.request_stop();},
                [&]{if(m_youtubeResolver)m_youtubeResolver->Cancel();},
                [&]{m_youtubeWorker.join();});
            m_youtubeWorker=std::jthread{};
        }
        DrainYouTubeCompletions();m_pendingYouTubeTitle.clear();
        if(updateUi)SyncSourceActionAvailability();
        LOG("YouTube resolution cancelled and worker stopped.");
    }

    bool NeuralPreRenderEnabled()const{return m_opt.neuralAddonConfigured&&!m_opt.safeMode;}
    bool NeuralJobActive()const{switch(m_neuralLifecycle.state){case NeuralPlaybackState::Acquiring:case NeuralPlaybackState::Rendering:case NeuralPlaybackState::Validating:case NeuralPlaybackState::Cancelling:case NeuralPlaybackState::Paused:case NeuralPlaybackState::Recovering:return true;default:return m_neuralWorker.joinable();}}
    bool NeuralJobPaused()const{return m_neuralPauseEvent&&WaitForSingleObject(m_neuralPauseEvent,0)==WAIT_OBJECT_0;}
    void SetNeuralJobPaused(bool paused){
        if(!NeuralJobActive()||!m_neuralPauseEvent||paused==NeuralJobPaused())return;
        if(paused)SetEvent(m_neuralPauseEvent);else ResetEvent(m_neuralPauseEvent);
        m_neuralLifecycle.Transition(paused?NeuralPlaybackState::Paused:NeuralPlaybackState::Rendering);
        if(!paused&&m_neuralProgress.phase==NeuralRenderPhase::Paused)m_neuralProgress.phase=NeuralRenderPhase::NeuralRendering;
        LOG("Neural pre-render "<<(paused?"paused":"resumed")<<" by the user; state="<<WideToUtf8(NeuralPlaybackStateName(m_neuralLifecycle.state)));
        if(m_hwnd){SyncFeatureMenuState();InvalidateRect(m_hwnd,nullptr,FALSE);}
    }
    // Source-cache key of the loaded YouTube source when its owned entry is
    // still in the recent history; nullopt for a stream without one.
    std::optional<std::string> CachedYouTubeSourceKey()const{
        if(!m_recent||m_youtubePageUrl.empty())return std::nullopt;
        const auto id=CanonicalYouTubeVideoId(m_youtubePageUrl);
        for(const auto& entry:m_recent->Entries())if(entry.youtube&&entry.id==id&&entry.sourceQuality==static_cast<int>(m_youtubeSourceQuality)&&!entry.sourceKey.empty())return entry.sourceKey;
        return std::nullopt;
    }
    bool SourcePrefetchActive()const{return m_prefetchState&&!m_prefetchState->finished.load(std::memory_order_acquire);}
    // Downloads the playing stream into the source cache while playback
    // continues, so a render starts on a local file instead of waiting for the
    // whole video. Only a live stream needs it, and only once per source.
    void EnsureSourcePrefetch(){
        if(!m_loaded||!NeuralPreRenderEnabled()||!NetworkPlayback())return;
        if(m_prefetchWorker.joinable()||CachedYouTubeSourceKey())return;
        if(m_path.empty()||m_youtubePageUrl.empty())return;
        const double duration=m_decoder.DurationSeconds();
        if(!std::isfinite(duration)||duration<=0.0)return;
        auto state=std::make_shared<SourcePrefetchState>();
        const std::wstring media=m_path,audio=m_youtubeAudioUrl,page=m_youtubePageUrl;
        const auto quality=m_youtubeSourceQuality;const auto cacheRoot=m_cacheRoot,moduleDirectory=ExecutableDirectory();
        try{
            m_prefetchWorker=std::jthread([state,cacheRoot,moduleDirectory,media,audio,page,quality,duration](std::stop_token stop){
                NeuralCacheManager cache(cacheRoot);
                if(cache.Valid()){
                    const SourceAcquisition acquired=AcquireYouTubeSource(cache,moduleDirectory,media,audio,page,quality,duration,{},stop);
                    if(!acquired.path.empty())state->key=acquired.key;
                }
                state->finished.store(true,std::memory_order_release);
            });
        }catch(const std::system_error&){LOG("Background source acquisition could not be started.");return;}
        m_prefetchState=state;m_prefetchPageUrl=page;m_prefetchQuality=quality;m_prefetchTitle=m_displayTitle;
        LOG("Range marked on a stream; acquiring its source in the background.");
        UpdateCachedStatus();
    }
    // UI-thread side of the background acquisition: adopt a finished copy into
    // the recent history so the next render reuses it, and stop one that no
    // longer belongs to the loaded source.
    void ReapSourcePrefetch(){
        if(!m_prefetchWorker.joinable())return;
        if(!m_prefetchState||m_prefetchState->finished.load(std::memory_order_acquire)){
            m_prefetchWorker.join();
            const std::string key=m_prefetchState?m_prefetchState->key:std::string{};
            const std::wstring page=m_prefetchPageUrl,title=m_prefetchTitle;const auto quality=m_prefetchQuality;
            m_prefetchState.reset();m_prefetchPageUrl.clear();m_prefetchTitle.clear();
            if(key.empty()){LOG("Background source acquisition finished without a reusable copy.");UpdateCachedStatus();return;}
            LOG("Background source acquisition complete; a render will reuse it.");
            NeuralJobCompletion owned{};owned.sourceKind=MediaSourceKind::YouTube;owned.pageUrl=page;owned.displayTitle=title;owned.sourceQuality=quality;owned.sourceKey=key;
            RecordRecent(owned,true);UpdateCachedStatus();return;
        }
        // A different video is playing now: the download is worthless. An offline
        // job owns the prefetch it will consume, but a live session renders what is
        // already on screen, so a download for some other page is still worthless.
        if(m_loaded&&(!NeuralJobActive()||m_liveSession)&&m_youtubePageUrl!=m_prefetchPageUrl){
            LOG("Loaded source changed; stopping the background acquisition.");
            m_prefetchWorker.request_stop();m_prefetchWorker.join();m_prefetchState.reset();m_prefetchPageUrl.clear();m_prefetchTitle.clear();UpdateCachedStatus();
        }
    }
    bool RangeRenderAvailable()const{
        if(!m_loaded||NeuralJobActive()||m_youtubeLifecycle.IsResolving()||!NeuralPreRenderEnabled()||m_path.empty())return false;
        if(m_sourceKind!=MediaSourceKind::YouTube)return true;
        // A stream is rendered from its own acquired copy: either the cached one
        // or a fresh acquisition, which needs the page URL and a real duration.
        return CachedYouTubeSourceKey().has_value()||(!m_youtubePageUrl.empty()&&m_decoder.DurationSeconds()>0.0);
    }
    // Renders [start,end) of the source that is loaded now. A YouTube source
    // reuses its owned source-cache entry when the recent history still names
    // one; otherwise the job acquires the source before rendering the range.
    bool RenderRangeOfCurrentSource(NeuralRenderRange range,NeuralJobKind kind=NeuralJobKind::Offline){
        if(!RangeRenderAvailable()){LOG("Render request ignored: loaded="<<m_loaded<<" job="<<NeuralJobActive()<<" resolving="<<m_youtubeLifecycle.IsResolving()<<" prerender="<<NeuralPreRenderEnabled()<<" haveSource="<<!m_path.empty()<<" cachedSourceKey="<<CachedYouTubeSourceKey().has_value()<<" duration="<<m_decoder.DurationSeconds());return false;}
        if(m_sourceKind==MediaSourceKind::YouTube){
            const std::wstring page=m_youtubePageUrl,title=m_displayTitle;
            if(const auto sourceKey=CachedYouTubeSourceKey()){
                StartNeuralJob(page,{},title,page,MediaSourceKind::YouTube,m_youtubeSourceQuality,*sourceKey,0.0,range,false,kind);return true;
            }
            const std::wstring media=m_path,audio=m_youtubeAudioUrl;const double duration=m_decoder.DurationSeconds();
            StartNeuralJob(media,audio,title,page,MediaSourceKind::YouTube,m_youtubeSourceQuality,{},duration,range,false,kind);return true;
        }
        const std::wstring source=m_path,title=m_displayTitle;
        StartNeuralJob(source,{},title,{},MediaSourceKind::LocalFile,m_youtubeSourceQuality,{},0.0,range,false,kind);return true;
    }
    int64_t SourceDuration100ns()const{return static_cast<int64_t>(std::llround(m_decoder.DurationSeconds()*1e7));}
    int64_t Position100ns()const{return static_cast<int64_t>(std::llround(Position()*1e7));}
    void PreviewCurrentFrame(){if(!m_loaded)return;RenderRangeOfCurrentSource(SingleFrameRange(Position100ns(),m_decoder.FrameRate(),SourceDuration100ns()));}
    void PreviewClip(){if(!m_loaded)return;RenderRangeOfCurrentSource(ClipPreviewRange(Position100ns(),m_decoder.FrameRate(),SourceDuration100ns()));}
    void RenderMarkedRange(){
        if(!m_loaded)return;
        const auto range=RangeFromMarkers(m_markers,m_decoder.FrameRate(),SourceDuration100ns());
        if(!range){const std::wstring message=T(L"range.invalid"),caption=T(L"app.title");MessageBoxW(m_hwnd,message.c_str(),caption.c_str(),MB_OK|MB_ICONINFORMATION);return;}
        RenderRangeOfCurrentSource(*range);
    }
    void RenderWholeSource(){if(m_loaded)RenderRangeOfCurrentSource(NeuralRenderRange{});}
    // Active neural rendering ------------------------------------------------
    // Fitted over three range renders of one clip: 29.1 ms per frame (34.4 fps
    // at 1080p) plus 7.3 s of fixed cost per job. Chunked jobs therefore cannot
    // keep up with 30 fps playback (break-even is a 57 s chunk): the
    // session is one long job that publishes finalized segments while it runs,
    // and playback follows the render head, buffering when it catches up.
    static constexpr double kLiveSegmentSeconds=2.0;
    static constexpr double kLiveStartLead=live_session::kStartLead;
    static constexpr double kLiveResumeLead=live_session::kResumeLead;
    bool LiveSessionAvailable()const{return RangeRenderAvailable()&&!m_cachedPlayback&&!m_decoder.IsStillImage();}
    double LiveHeadSeconds()const{return m_liveSegments?double(m_liveSegments->Head100ns())*1e-7:0.0;}
    double LiveLeadSeconds()const{return live_session::Lead(LiveSessionView());}
    bool LiveSessionFinished()const{return m_liveSegments&&m_liveSegments->Finished();}
    // Rendering is linear in pixel count, so a source too large for this GPU can
    // be recognised before a single frame is rendered. Saying so beats letting the
    // user watch a loader that will never clear.
    bool ConfirmLiveSessionPace(double fps){
        const auto forecast=playback_timing::ForecastLiveRender(m_decoder.Width(),m_decoder.Height(),fps);
        if(forecast.keepsUp)return true;
        LOG("Active neural session forecast: "<<m_decoder.Width()<<"x"<<m_decoder.Height()<<" at "<<fps
            <<" fps renders at about "<<forecast.renderFps<<" fps ("<<forecast.realtimeRatio<<"x realtime).");
        wchar_t text[512];
        swprintf_s(text,T(L"neural.live.slow").c_str(),m_decoder.Width(),m_decoder.Height(),fps,
                   forecast.renderFps,forecast.realtimeRatio);
        return MessageBoxW(m_hwnd,text,T(L"neural.live.title").c_str(),MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)==IDYES;
    }
    // One render job from the playhead: to the Out marker when the playhead sits
    // inside a marked range, otherwise to the end of the source.
    // What makes retained coverage reusable: same source, same neural settings,
    // same guides. Anything else and the frames on disk are not the frames the
    // user would get now.
    std::string LiveRetentionKey()const{
        return WideToUtf8(m_path)+"|"+CanonicalNeuralSettings(m_neuralSettings)+"|"+CanonicalGuideControls(m_renderGuides);
    }
    void DropRetainedLiveSegments(){
        m_retainedSegments.reset();m_retainedRange={};m_retainedKey.clear();
        if(!m_retainedDirectory.empty()){std::error_code ec;std::filesystem::remove_all(m_retainedDirectory,ec);m_retainedDirectory.clear();}
    }
    void StartLiveNeuralSession(){
        if(!LiveSessionAvailable()){LOG("Active neural session refused: loaded="<<m_loaded<<" cached="<<m_cachedPlayback<<" renderable="<<RangeRenderAvailable());return;}
        const double fps=m_decoder.FrameRate();const int64_t duration=SourceDuration100ns();
        if(!(fps>0.0)||duration<=0)return;
        const int64_t at=SnapToFrame(Position100ns());
        NeuralRenderRange range{at,duration};
        if(const auto marked=RangeFromMarkers(m_markers,fps,duration);marked&&at>=marked->start100ns&&at<marked->end100ns)range.end100ns=marked->end100ns;
        if(range.end100ns<=range.start100ns)return;
        if(!ConfirmLiveSessionPace(fps))return;
        // Frames rendered before the last toggle-off are still on disk. Adopting
        // them means the render resumes at the head instead of redoing work, and
        // playback can start on them immediately instead of buffering a lead.
        const std::string key=LiveRetentionKey();
        const bool adopt=m_retainedSegments&&m_retainedKey==key&&!m_retainedSegments->Empty()&&
                         at>=m_retainedSegments->Start100ns()&&at<m_retainedSegments->Head100ns();
        if(!adopt)DropRetainedLiveSegments();
        if(adopt){
            m_liveSegments=m_retainedSegments;m_liveDirectory=m_retainedDirectory;
            range.start100ns=m_retainedSegments->Start100ns();
            if(m_retainedRange.end100ns>range.end100ns)range.end100ns=m_retainedRange.end100ns;
            m_retainedSegments.reset();m_retainedDirectory.clear();m_retainedKey.clear();m_retainedRange={};
        }else{
            m_liveDirectory=m_cacheRoot/L"live";
            std::error_code ec;std::filesystem::remove_all(m_liveDirectory,ec);std::filesystem::create_directories(m_liveDirectory,ec);
            if(ec){LOG("Active neural session could not create its segment directory.");m_liveDirectory.clear();return;}
            m_liveSegments=std::make_shared<NeuralSegmentIndex>();
        }
        m_liveRange=range;m_liveSession=true;m_liveAttached=false;m_livePaintedHead=0;m_liveStartTick=GetTickCount64();m_neuralRequested=true;
        const int64_t renderFrom=std::max(range.start100ns,m_liveSegments->Head100ns());
        if(renderFrom<range.end100ns)m_liveSegments->Unfinish();
        EnterLiveBuffering();
        if(renderFrom>=range.end100ns){
            // The retained coverage already reaches the end of this range: there
            // is nothing to render, so play it and stop waiting for a head.
            m_liveSegments->Finish();
            LOG("Active neural session replaying "<<m_liveSegments->Count()<<" retained segments through "<<double(range.end100ns)*1e-7<<" s; nothing left to render.");
        }else if(!RenderRangeOfCurrentSource(NeuralRenderRange{renderFrom,range.end100ns},NeuralJobKind::Live)){StopLiveNeuralSession(true);return;}
        else LOG("Active neural session started at "<<double(renderFrom)*1e-7<<" s through "<<double(range.end100ns)*1e-7<<" s"
                 <<(renderFrom>range.start100ns?std::string("; resumed on ")+std::to_string(m_liveSegments->Count())+" retained segments from "+std::to_string(double(range.start100ns)*1e-7)+" s":std::string{})<<".");
        SyncFeatureMenuState();UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();
    }
    // Drops the session state. Rendered segments are kept for the next toggle
    // unless the caller says the frames can no longer be trusted.
    void ReleaseLiveSession(bool retainSegments=false){
        const bool retain=retainSegments&&m_liveSegments&&!m_liveSegments->Empty()&&!m_liveDirectory.empty();
        if(retain){
            m_retainedSegments=m_liveSegments;m_retainedDirectory=m_liveDirectory;m_retainedRange=m_liveRange;m_retainedKey=LiveRetentionKey();
            LOG("Retained "<<m_retainedSegments->Count()<<" rendered segments covering "<<double(m_retainedSegments->Start100ns())*1e-7
                <<"-"<<double(m_retainedSegments->Head100ns())*1e-7<<" s for the next toggle.");
        }
        m_liveSession=false;m_liveAttached=false;m_liveBuffering=false;m_liveResumePlaying=false;m_livePaintedHead=0;m_liveStartTick=0;m_liveRange={};
        HideBufferOverlay();
        m_liveSegments.reset();
        if(retain){m_liveDirectory.clear();return;}
        if(!m_liveDirectory.empty()){std::error_code ec;std::filesystem::remove_all(m_liveDirectory,ec);m_liveDirectory.clear();}
    }
    // Stops the render and, when playback already followed it, hands the same
    // frame back to the original stream.
    void StopLiveNeuralSession(bool keepPlaying){
        if(!m_liveSession)return;
        const double at=Position();const bool wasPlaying=m_playing||m_liveResumePlaying;const bool attached=m_liveAttached;
        const uint64_t presented=m_cachedPresentedFrames,dropped=m_droppedFrames;
        m_liveSession=false;m_liveBuffering=false;HideBufferOverlay();
        CancelNeuralJob(false);
        if(attached){
            m_haveNext=false;m_next=VideoFrame{};
            m_synchronizedPlayback.Close();m_cachedPlayback=false;m_havePresentedPair=false;m_lastOriginalFrame={};m_lastNeuralFrame={};m_cachedRange={};m_cachedPresentedFrames=0;m_comparisonView=ComparisonView::Original;
            if(m_renderer)m_renderer->SetComparison(EffectiveComparison());
        }
        ReleaseLiveSession(true);
        m_neuralRequested=false;
        if(attached&&keepPlaying&&m_loaded)RequestSeek(at,wasPlaying);
        // Buffering paused the original; stopping before the session ever
        // attached has to hand that playback back.
        else if(keepPlaying&&m_loaded&&wasPlaying&&!m_playing)SetPaused(false);
        SyncFeatureMenuState();SyncSourceActionAvailability();UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();
        LOG("Active neural session stopped at "<<at<<" s; presented="<<presented<<" dropped="<<dropped);
    }
    // The job ended while its output is still playing: publish or report, but
    // never reload playback from the cache entry it just wrote.
    void CompleteLiveNeuralJob(const NeuralJobCompletion& completion){
        const bool covered=m_liveSegments&&!m_liveSegments->Empty();
        if(m_liveSegments)m_liveSegments->Finish();
        if(completion.result.ok){
            m_neuralLifecycle.Transition(NeuralPlaybackState::Ready);RecordRecent(completion);
            // Playback stays on the segments, but the published entry is what
            // "Save converted video" and the receipt need.
            m_neuralPath=completion.neuralPath;m_cachedReceiptPath=completion.receiptPath;m_cachedSettings=completion.settings;m_cachedGuides=completion.guides;
            LOG("Active neural session rendered "<<completion.result.frameCount<<" frames and published its cache entry; save="<<(m_cachedPlayback&&!m_neuralPath.empty()&&!m_exportWorker.joinable()&&!ActivityBusy())<<" entry="<<(m_neuralPath.empty()?std::string("(none)"):WideToUtf8(m_neuralPath.wstring())));
        }else{
            TransitionToFailure(completion.result.failure);
            LOG("Active neural session ended early: kind="<<NeuralRenderFailureName(completion.result.failure)<<" covered="<<covered<<" detail="<<WideToUtf8(completion.result.detail));
            if(!covered){StopLiveNeuralSession(true);return;}
        }
        SyncFeatureMenuState();SyncSourceActionAvailability();UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();
    }
    // Moves playback from the original stream onto the growing render, at the
    // frame that is on screen.
    bool AttachLiveNeural(){
        if(!m_liveSession||m_liveAttached||!m_loaded||!m_liveSegments)return false;
        const double at=std::max(Position(),double(m_liveRange.start100ns)*1e-7);
        if(!m_liveSegments->Containing(static_cast<int64_t>(std::llround(at*1e7))))return false;
        const bool wasPlaying=m_playing||m_liveResumePlaying;
        Audio().Stop();m_haveNext=false;m_next=VideoFrame{};
        if(!m_synchronizedPlayback.OpenLive(m_path,m_liveSegments,SynchronizedRange{m_liveRange.start100ns,m_liveRange.end100ns})){LOG("Active neural playback could not open the live pair.");return false;}
        if(!m_synchronizedPlayback.SeekSeconds(at)||!m_synchronizedPlayback.VisibleFrame()){LOG("Active neural playback could not position the live pair at "<<at<<" s.");m_synchronizedPlayback.Close();return false;}
        // The view has to switch before the frame is read: VisibleFrame returns
        // whichever side the view selects, and reading it first presented the
        // ORIGINAL frame. Playing hid that - the next pair arrived a frame later
        // - but a paused player kept re-presenting it and looked unchanged.
        m_comparisonView=ComparisonView::Neural;
        if(!m_synchronizedPlayback.SetView(ComparisonView::Neural)){LOG("Active neural playback could not select the rendered view.");m_synchronizedPlayback.Close();m_comparisonView=ComparisonView::Original;return false;}
        if(m_renderer)m_renderer->SetComparison(EffectiveComparison());
        const VideoFrame frame=*m_synchronizedPlayback.VisibleFrame();
        m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;
        m_cachedPlayback=true;m_cachedRange=m_liveRange;m_cachedSettings=m_neuralSettings;m_cachedGuides=m_renderGuides;m_cachedReceiptPath.clear();m_neuralPath.clear();
        if(!RenderVideoFrame(frame,true)){LOG("Active neural playback could not present its first pair.");m_synchronizedPlayback.Close();m_cachedPlayback=false;m_comparisonView=ComparisonView::Original;return false;}
        RememberRenderedCachedPair();m_cachedPresentedFrames=1;m_currentSec=double(frame.timestamp100ns)*1e-7;m_guideReset=false;m_dlssReset=false;
        if(Audio().Start(m_path,m_currentSec)){Audio().SetVolume(m_muted?0.0f:m_volume);Audio().Pause(!wasPlaying);}
        m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=wasPlaying;m_synchronizedPlayback.SetPaused(!wasPlaying);
        m_liveAttached=true;
        LOG("Active neural playback attached at "<<m_currentSec<<" s with "<<LiveLeadSeconds()<<" s buffered.");
        SyncFeatureMenuState();UpdateCachedStatus();InvalidateControls();return true;
    }
    void EnterLiveBuffering(){
        if(!m_liveSession||m_liveBuffering)return;
        m_liveResumePlaying=m_playing||m_liveResumePlaying;
        if(m_playing){m_currentSec=Position();m_playing=false;}
        m_liveBuffering=true;Audio().Pause(true);if(m_cachedPlayback)m_synchronizedPlayback.SetPaused(true);
        LOG("Neural buffer empty at "<<m_currentSec<<" s; head="<<LiveHeadSeconds()<<" s.");
        ShowBufferOverlay();InvalidateControls();UpdateCachedStatus();
    }
    void ExitLiveBuffering(){
        if(!m_liveBuffering)return;
        LOG("Neural buffer filled at "<<m_currentSec<<" s with "<<LiveLeadSeconds()<<" s ahead; resume="<<m_liveResumePlaying);
        m_liveBuffering=false;HideBufferOverlay();
        if(m_liveResumePlaying){m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=true;Audio().Pause(false);if(m_cachedPlayback)m_synchronizedPlayback.SetPaused(false);}
        m_liveResumePlaying=false;InvalidateControls();UpdateCachedStatus();
    }
    // Neural settings preview -------------------------------------------------
    // A settings change is only worth a render when the picture is standing
    // still, so the paused frame is re-rendered once the sliders settle. A
    // repeat of the same settings is a cache hit and comes back immediately.
    static constexpr UINT kPreviewSettleMs=700;
    bool PausedPreviewAvailable()const{
        return m_loaded&&!m_playing&&!m_seeking&&!m_seekPending&&!m_liveSession&&!m_previewJob&&m_renderer&&!m_decoder.IsStillImage()&&RangeRenderAvailable();
    }
    void SchedulePausedSettingsPreview(){
        if(!m_hwnd||!m_loaded)return;
        if(m_previewTimer){KillTimer(m_hwnd,m_previewTimer);m_previewTimer=0;}
        if(m_playing||m_liveSession)return;
        m_previewTimer=SetTimer(m_hwnd,kPreviewTimerId,kPreviewSettleMs,nullptr);
    }
    // A seek or a resume makes a queued preview meaningless: it would render a
    // frame the player has already left.
    void CancelPausedSettingsPreview(){
        if(m_previewTimer&&m_hwnd){KillTimer(m_hwnd,m_previewTimer);}
        m_previewTimer=0;m_previewQueued=false;
    }
    void StartPausedSettingsPreview(){
        CancelPausedSettingsPreview();
        // A change made while a preview renders is not dropped: the newest
        // settings are previewed once the running one lands.
        if(m_previewJob){m_previewQueued=true;return;}
        if(!PausedPreviewAvailable())return;
        const double fps=m_decoder.FrameRate();const int64_t duration=SourceDuration100ns();
        if(!(fps>0.0)||duration<=0)return;
        const NeuralRenderRange range=SingleFrameRange(SnapToFrame(Position100ns()),fps,duration);
        if(range.end100ns<=range.start100ns)return;
        m_previewRange=range;m_previewJob=true;
        ShowBufferOverlay();
        if(!RenderRangeOfCurrentSource(range,NeuralJobKind::Preview)){m_previewJob=false;HideBufferOverlay();return;}
        LOG("Neural settings preview started for frame at "<<double(range.start100ns)*1e-7<<" s: "<<CanonicalNeuralSettings(m_neuralSettings));
        UpdateCachedStatus();InvalidateControls();
    }
    // Presents the rendered frame in place of the paused one; playback stays
    // exactly where it was, so the next setting can be compared against it.
    void CompletePausedPreview(const NeuralJobCompletion& completion){
        m_previewJob=false;HideBufferOverlay();
        const bool stillHere=m_loaded&&!m_playing&&SnapToFrame(Position100ns())==m_previewRange.start100ns;
        if(!completion.result.ok||completion.neuralPath.empty()){
            LOG("Neural settings preview failed: "<<NeuralRenderFailureName(completion.result.failure)<<" detail="<<WideToUtf8(completion.result.detail));
            m_previewQueued=false;
            TransitionToFailure(completion.result.failure);SyncSourceActionAvailability();UpdateCachedStatus();InvalidateControls();return;
        }
        m_neuralLifecycle.Transition(NeuralPlaybackState::Ready);
        if(stillHere){
            VideoDecoder preview;VideoFrame frame;
            if(preview.Open(completion.neuralPath.wstring(),MediaSourceKind::LocalFile)&&preview.ReadNext(frame)){
                frame.timestamp100ns=m_previewRange.start100ns;
                m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;
                if(RenderVideoFrame(frame,true)){m_lastNeuralFrame=frame;m_previewShown=true;LOG("Neural settings preview presented for the paused frame.");}
                m_guideReset=false;m_dlssReset=false;
            }else LOG("Neural settings preview could not decode its rendered frame.");
            preview.Close();
        }else LOG("Neural settings preview discarded: the paused frame moved.");
        SyncSourceActionAvailability();UpdateCachedStatus();InvalidateControls();
        if(m_previewQueued){m_previewQueued=false;LOG("Neural settings changed during the preview; previewing the newest settings.");StartPausedSettingsPreview();}
    }
    // Playback leaves the live pair and continues on the original decoder; the
    // session keeps rendering and re-attaches when it covers the playhead again.
    void DetachLivePlayback(){
        if(!m_liveAttached)return;
        m_liveAttached=false;m_haveNext=false;m_next=VideoFrame{};
        m_synchronizedPlayback.Close();m_cachedPlayback=false;m_havePresentedPair=false;m_lastOriginalFrame={};m_lastNeuralFrame={};m_cachedRange={};m_cachedPresentedFrames=0;m_comparisonView=ComparisonView::Original;
        if(m_renderer)m_renderer->SetComparison(EffectiveComparison());
    }
    // A committed seek out of the rendered part restarts the session there:
    // waiting for the head to travel to a distant playhead would take minutes.
    bool LiveSessionNeedsRebase()const{
        if(!m_liveSession)return false;
        return live_session::NeedsRebase(LiveSessionView());
    }
    live_session::SessionView LiveSessionView()const{
        return {Position(),double(m_liveRange.start100ns)*1e-7,LiveHeadSeconds(),m_liveAttached,LiveSessionFinished(),
                m_dragSeek||m_seeking||m_seekPending};
    }
    // UI-thread side of the session: adopt new coverage, start playing once the
    // lead-in is buffered, resume after a rebuffer, and rebase after a seek.
    void UpdateLiveSession(){
        if(!m_liveSession)return;
        if(LiveSessionNeedsRebase()){
            LOG("Active neural session rebased to "<<Position()<<" s after a seek out of its range.");
            StopLiveNeuralSession(true);StartLiveNeuralSession();return;
        }
        const int64_t head=m_liveSegments?m_liveSegments->Head100ns():0;
        if(head!=m_livePaintedHead){m_livePaintedHead=head;InvalidatePlaybackProgress();RefreshBufferOverlay();UpdateCachedStatus();}
        const live_session::SessionView view=LiveSessionView();
        if(!m_liveAttached){
            if(live_session::ShouldAttach(view)&&AttachLiveNeural())ExitLiveBuffering();
            return;
        }
        if(m_liveBuffering&&live_session::ShouldResume(view))ExitLiveBuffering();
    }
    // Buffering panel. A popup owned by the main window, because the video is a
    // D3D12 child window that a sibling would have to fight for z-order.
    static LRESULT CALLBACK BufferWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l){
        if(m==WM_NCCREATE){SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));return DefWindowProcW(h,m,w,l);}
        auto* app=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_PAINT&&app){PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);RECT c{};GetClientRect(h,&c);app->PaintBufferOverlay(dc,c);EndPaint(h,&ps);return 0;}
        return DefWindowProcW(h,m,w,l);
    }
    void ShowBufferOverlay(){
        if(!m_hwnd||!m_renderWnd)return;
        if(!m_bufferWnd){
            static constexpr const wchar_t* kClassName=L"DLSSVideoBufferClassV1";
            const HINSTANCE instance=reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(m_hwnd,GWLP_HINSTANCE));
            WNDCLASSW cls{};cls.lpfnWndProc=BufferWndProcStatic;cls.hInstance=instance;cls.lpszClassName=kClassName;cls.hCursor=LoadCursor(nullptr,IDC_ARROW);cls.hbrBackground=nullptr;
            if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
            m_bufferWnd=CreateWindowExW(WS_EX_LAYERED|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,kClassName,L"",WS_POPUP,0,0,10,10,m_hwnd,nullptr,instance,this);
            if(!m_bufferWnd)return;
            SetLayeredWindowAttributes(m_bufferWnd,0,236,LWA_ALPHA);
        }
        PositionBufferOverlay(true);
        ShowWindow(m_bufferWnd,SW_SHOWNOACTIVATE);
    }
    void PositionBufferOverlay(bool force=false){
        if(!m_bufferWnd||!m_renderWnd)return;
        RECT video{};if(!GetWindowRect(m_renderWnd,&video))return;
        if(!force&&EqualRect(&video,&m_bufferAnchor))return;
        m_bufferAnchor=video;
        const int width=std::min<LONG>(Dip(360),std::max<LONG>(Dip(200),video.right-video.left-Dip(32)));
        const int height=Dip(112);
        SetWindowPos(m_bufferWnd,HWND_TOP,static_cast<int>(video.left+((video.right-video.left)-width)/2),static_cast<int>(video.top+((video.bottom-video.top)-height)/2),width,height,SWP_NOACTIVATE);
    }
    void HideBufferOverlay(){if(m_bufferWnd&&IsWindowVisible(m_bufferWnd))ShowWindow(m_bufferWnd,SW_HIDE);}
    void RefreshBufferOverlay(){if(m_bufferWnd&&IsWindowVisible(m_bufferWnd)){PositionBufferOverlay();InvalidateRect(m_bufferWnd,nullptr,FALSE);}}
    void PaintBufferOverlay(HDC dc,const RECT& c){
        HBRUSH panel=CreateSolidBrush(ui_palette::ControlSurface);FillRect(dc,&c,panel);DeleteObject(panel);
        HBRUSH edge=CreateSolidBrush(ui_palette::NeuralCoverage);FrameRect(dc,&c,edge);DeleteObject(edge);
        const RECT spinner{c.left+Dip(16),c.top+Dip(16),c.left+Dip(40),c.top+Dip(40)};
        DrawActivitySpinner(dc,spinner,ResolveActivityVisual({},ActivityElapsedMs(),0,0,false,m_activityMotionEnabled).spinnerStep);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(242,243,245));HGDIOBJ oldFont=SelectObject(dc,m_font);
        RECT title{c.left+Dip(50),c.top+Dip(14),c.right-Dip(16),c.top+Dip(38)};
        const std::wstring heading=m_previewJob?T(L"neural.preview.title"):m_liveAttached?T(L"neural.live.buffering"):T(L"neural.live.title");
        DrawTextW(dc,heading.c_str(),-1,&title,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        SelectObject(dc,m_fontSmall);SetTextColor(dc,ui_palette::SecondaryText);
        const double lead=LiveLeadSeconds(),target=m_liveAttached?kLiveResumeLead:kLiveStartLead;
        wchar_t detail[128]={};
        if(m_previewJob)swprintf_s(detail,L"%s",T(L"neural.preview.detail").c_str());
        else swprintf_s(detail,L"%.1f s %s · %llu frames rendered",lead,T(L"neural.live.lead").c_str(),static_cast<unsigned long long>(m_neuralProgress.completedFrames));
        RECT line{c.left+Dip(50),c.top+Dip(38),c.right-Dip(16),c.top+Dip(58)};
        DrawTextW(dc,detail,-1,&line,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        SelectObject(dc,oldFont);
        RECT track{c.left+Dip(16),c.bottom-Dip(30),c.right-Dip(16),c.bottom-Dip(22)};
        HBRUSH trackBrush=CreateSolidBrush(RGB(68,71,77));FillRect(dc,&track,trackBrush);DeleteObject(trackBrush);
        RECT fill=track;fill.right=track.left+static_cast<LONG>(std::lround((track.right-track.left)*std::clamp(m_previewJob?0.0:(target>0.0?lead/target:0.0),0.0,1.0)));
        if(fill.right>fill.left){HBRUSH fillBrush=CreateSolidBrush(ui_palette::PrimaryBlue);FillRect(dc,&fill,fillBrush);DeleteObject(fillBrush);}
    }
    void MarkersChanged(){UpdateCachedStatus();InvalidatePlaybackProgress();}
    // Markers sit on the decoder's frame grid so their timecodes and the
    // rendered range name exact frames.
    int64_t SnapToFrame(int64_t pts)const{const double fps=m_decoder.FrameRate();return fps>0?FramePts(FrameIndexNearest(pts,fps),fps):pts;}
    // A marker always stays inside the source: the CFR grid runs up to one frame
    // past the container duration, and a marker in that remainder describes a
    // range with nothing to decode. In names a frame, so it stops at the last
    // one. Out is the exclusive end, and playback stops on the last frame, so an
    // Out there means "to the end" - otherwise the final frame could never be
    // marked for rendering.
    void SetMarker(bool in,std::optional<int64_t> pts){
        if(!m_loaded)return;
        if(pts){
            const int64_t duration=SourceDuration100ns(),last=LastFramePts(m_decoder.FrameRate(),duration);
            int64_t value=std::max<int64_t>(0,SnapToFrame(*pts));
            if(last>0)value=in?std::min(value,last):(value>=last?duration:value);
            pts=value;
        }
        (in?m_markers.in100ns:m_markers.out100ns)=pts;MarkersChanged();
        // Marking is the first sign that a render is coming, and a streamed
        // source has to be acquired before it can be rendered. Start that
        // download now, while the user is still choosing the other marker.
        if(pts)EnsureSourcePrefetch();
    }
    void ClearMarkers(){if(!m_loaded)return;m_markers={};MarkersChanged();}
    // Timecode dialog handler: false leaves the dialog open with its error.
    bool ApplyTimecodeText(const std::wstring& text,TimecodeAction action){
        if(!m_loaded)return false;
        const auto parsed=ParseTimecode(text,m_decoder.FrameRate());if(!parsed)return false;
        const int64_t duration=SourceDuration100ns();const int64_t pts=duration>0?std::clamp<int64_t>(*parsed,0,duration):*parsed;
        switch(action){
        case TimecodeAction::Go:RequestSeek(double(pts)*1e-7);break;
        case TimecodeAction::SetIn:SetMarker(true,pts);break;
        case TimecodeAction::SetOut:SetMarker(false,pts);break;
        }
        return true;
    }
    void ShowTimecodeDialog(){
        if(!m_loaded)return;
        PromptForTimecode(m_hwnd,m_loc,m_font,FormatTimecode(Position100ns(),m_decoder.FrameRate(),true),[this](const std::wstring& text,TimecodeAction action){return ApplyTimecodeText(text,action);});
    }
    void OpenRenderReceipt(){
        if(m_cachedReceiptPath.empty())return;
        const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(m_hwnd,L"open",m_cachedReceiptPath.c_str(),nullptr,nullptr,SW_SHOWNORMAL));
        if(result<=32)LOG("Opening the render receipt failed: code="<<result<<" path="<<WideToUtf8(m_cachedReceiptPath.wstring()));
    }
    static std::filesystem::path ExecutableDirectory(){std::filesystem::path executable;std::wstring error;return CurrentExecutablePath(executable,error)?executable.parent_path():std::filesystem::path{};}
    static std::string GpuPathName(GpuGeneration generation){return generation==GpuGeneration::Rtx40Ada?"rtx40":generation==GpuGeneration::Rtx50Blackwell?"rtx50":"unsupported";}
    static const wchar_t* NeuralPhaseTextKey(NeuralRenderPhase phase){switch(phase){case NeuralRenderPhase::CheckingCache:return L"neural.phase.cache";case NeuralRenderPhase::Acquiring:return L"neural.phase.acquiring";case NeuralRenderPhase::Decoding:case NeuralRenderPhase::NeuralRendering:return L"neural.phase.rendering";case NeuralRenderPhase::Encoding:return L"neural.phase.encoding";case NeuralRenderPhase::Validating:return L"neural.phase.validating";case NeuralRenderPhase::Ready:return L"neural.phase.ready";case NeuralRenderPhase::Preflight:return L"neural.phase.preflight";case NeuralRenderPhase::Paused:return L"neural.phase.paused";case NeuralRenderPhase::Recovering:return L"neural.phase.recovering";}return L"neural.phase.acquiring";}
    std::wstring NeuralPhaseText(const NeuralRenderProgress& progress)const{std::wstring text=T(NeuralPhaseTextKey(progress.phase));if(progress.phase==NeuralRenderPhase::Recovering){const auto failure=NeuralRenderFailureName(progress.recovering);text+=L" (attempt "+std::to_wstring(progress.retries)+L" \u00b7 "+std::wstring(failure.begin(),failure.end())+L")";}return text;}
    static const wchar_t* NeuralFailureTextKey(NeuralRenderFailure failure){switch(failure){case NeuralRenderFailure::GpuStall:return L"neural.failure.gpu-stall";case NeuralRenderFailure::DeviceRemoved:return L"neural.failure.device-removed";case NeuralRenderFailure::WorkerCrashed:return L"neural.failure.worker-crashed";case NeuralRenderFailure::RetryExhausted:return L"neural.failure.retry-exhausted";case NeuralRenderFailure::Preflight:return L"neural.failure.preflight";case NeuralRenderFailure::Identity:return L"neural.failure.identity";case NeuralRenderFailure::Protocol:return L"neural.failure.protocol";default:return nullptr;}}
    // Lands the lifecycle in the failure's state. RetryExhausted is only
    // reachable through Recovering; a crash-relaunch that ran out of retries
    // mid-render steps through it rather than degrading to plain Failed.
    void TransitionToFailure(NeuralRenderFailure failure){
        const NeuralPlaybackState next=StateForFailure(failure);
        if(m_neuralLifecycle.Transition(next))return;
        if(next==NeuralPlaybackState::RetryExhausted&&m_neuralLifecycle.Transition(NeuralPlaybackState::Recovering)&&m_neuralLifecycle.Transition(next))return;
        m_neuralLifecycle.Transition(NeuralPlaybackState::Failed);
    }
    void DrainNeuralMessages(){m_neuralProgressMessages.Clear();m_neuralCompletions.Clear();if(!m_hwnd)return;MSG message{};while(PeekMessageW(&message,m_hwnd,WM_NEURAL_PROGRESS,WM_NEURAL_COMPLETE,PM_REMOVE)){};}
    void CancelNeuralJob(bool updateUi=true){
        if(!NeuralJobActive())return;
        m_neuralLifecycle.Transition(NeuralPlaybackState::Cancelling);if(m_neuralPauseEvent)ResetEvent(m_neuralPauseEvent);
        if(m_neuralWorker.joinable()){m_neuralWorker.request_stop();m_neuralWorker.join();m_neuralWorker=std::jthread{};}
        std::unique_ptr<NeuralJobCompletion> cancelledCompletion;
        if(m_hwnd){MSG message{};while(PeekMessageW(&message,m_hwnd,WM_NEURAL_PROGRESS,WM_NEURAL_COMPLETE,PM_REMOVE)){
            if(message.message==WM_NEURAL_PROGRESS)m_neuralProgressMessages.Remove(static_cast<uint64_t>(message.wParam));
            else if(message.message==WM_NEURAL_COMPLETE)cancelledCompletion=m_neuralCompletions.Take(static_cast<uint64_t>(message.wParam));
        }}
        m_neuralProgressMessages.Clear();m_neuralCompletions.Clear();m_neuralLifecycle.Invalidate();
        if(updateUi){m_neuralProgress={};m_neuralCancelBounds={};InvalidateRect(m_hwnd,nullptr,FALSE);SyncSourceActionAvailability();
            if(cancelledCompletion&&!cancelledCompletion->sourcePath.empty()&&std::filesystem::is_regular_file(cancelledCompletion->sourcePath)){
                m_neuralLifecycle.Transition(NeuralPlaybackState::OriginalOnly);
                LoadOriginalFallback(*cancelledCompletion);
            }
        }
        LOG("Neural pre-render cancelled and worker stopped.");
    }
    // `prepareOnly` acquires and identifies the source, replays a validated
    // cache entry when one exists, and otherwise stops before the feature-18
    // probe so the player can open the original for range selection.
    // A Live or Preview job renders behind the loaded media instead of
    // replacing it: Live streams segments, Preview renders one paused frame.
    void StartNeuralJob(const std::wstring& mediaUrl,const std::wstring& audioUrl,const std::wstring& displayTitle,const std::wstring& pageUrl,MediaSourceKind sourceKind,YouTubeSourceQuality sourceQuality,const std::string& reuseSourceKey={},double expectedDurationSeconds=0.0,NeuralRenderRange range={},bool prepareOnly=false,NeuralJobKind kind=NeuralJobKind::Offline){
        if(mediaUrl.empty())return;
        CancelNeuralJob(false);
        if(kind==NeuralJobKind::Offline)Unload();
        const uint64_t generation=m_neuralLifecycle.Begin();m_neuralProgress={};m_neuralProgress.phase=NeuralRenderPhase::CheckingCache;m_pendingNeuralTitle=DisplayTitleForSource(sourceKind,displayTitle);m_neuralSourceWidth=0;m_neuralSourceHeight=0;if(m_neuralPauseEvent)ResetEvent(m_neuralPauseEvent);
        SyncSourceActionAvailability();InvalidateRect(m_hwnd,nullptr,FALSE);
        try{
            HWND target=m_hwnd;const auto gpu=m_opt.detectedGpu.generation;const auto moduleDirectory=ExecutableDirectory();const auto cacheRoot=m_cacheRoot;const GuideControls guides=m_renderGuides;const NeuralSettings settings=m_neuralSettings;const HANDLE pauseEvent=m_neuralPauseEvent;
            // The background acquisition of this very source, when one is in
            // flight: the job waits for it rather than downloading again.
            const std::shared_ptr<SourcePrefetchState> prefetch=(sourceKind==MediaSourceKind::YouTube&&!pageUrl.empty()&&pageUrl==m_prefetchPageUrl)?m_prefetchState:nullptr;
            CompletionRegistry<NeuralProgressMessage>* progressMessages=&m_neuralProgressMessages;CompletionRegistry<NeuralJobCompletion>* completions=&m_neuralCompletions;
            // An active session renders into its own directory of segment files;
            // the cache entry is the concatenation published when the job ends.
            // A resumed session keeps the earlier job's files, so every job gets
            // its own subdirectory and its segments are appended after the ones
            // already published.
            const std::shared_ptr<NeuralSegmentIndex> liveIndex=kind==NeuralJobKind::Live?m_liveSegments:nullptr;
            std::filesystem::path liveDirectory;
            const size_t liveIndexBase=liveIndex?liveIndex->Count():0u;
            if(liveIndex){
                liveDirectory=m_liveDirectory/(L"job"+std::to_wstring(++m_liveJobSerial));
                std::error_code ec;std::filesystem::create_directories(liveDirectory,ec);
                if(ec){LOG("Active neural session could not create the segment directory for this job.");return;}
            }
            const uint32_t segmentFrames=kind==NeuralJobKind::Live?static_cast<uint32_t>(std::max<long>(1,std::lround(m_decoder.FrameRate()*kLiveSegmentSeconds))):0u;
            m_neuralWorker=std::jthread([target,generation,mediaUrl,audioUrl,displayTitle,pageUrl,sourceKind,sourceQuality,gpu,moduleDirectory,progressMessages,completions,reuseSourceKey,cacheRoot,expectedDurationSeconds,range,guides,settings,pauseEvent,prepareOnly,prefetch,liveIndex,liveDirectory,liveIndexBase,segmentFrames](std::stop_token stop){
                auto completion=std::make_unique<NeuralJobCompletion>();completion->generation=generation;completion->displayTitle=displayTitle;completion->pageUrl=pageUrl;completion->sourceKind=sourceKind;completion->sourceQuality=sourceQuality;
                uint32_t progressWidth=0,progressHeight=0;
                auto postProgress=[&](const NeuralRenderProgress& progress){auto message=std::make_unique<NeuralProgressMessage>();message->generation=generation;message->progress=progress;message->width=progressWidth;message->height=progressHeight;progressMessages->RegisterAndPost(std::move(message),[&](uint64_t token){return PostMessageW(target,WM_NEURAL_PROGRESS,static_cast<WPARAM>(token),0)!=FALSE;});};
                NeuralCacheManager cache(cacheRoot);if(!cache.Valid()){completion->result.detail=L"The neural cache directory is unavailable.";goto finish;}
                {
                    std::filesystem::path sourcePath;
                    if(sourceKind==MediaSourceKind::YouTube){
                        std::string reuseKey=reuseSourceKey;
                        if(reuseKey.empty()&&prefetch){
                            // A background acquisition started when the user marked a
                            // range. Wait for it instead of downloading the same
                            // source a second time.
                            if(!prefetch->finished.load(std::memory_order_acquire)){
                                LOG("Waiting for the background source acquisition.");
                                NeuralRenderProgress acquiring{};acquiring.phase=NeuralRenderPhase::Acquiring;postProgress(acquiring);
                                while(!prefetch->finished.load(std::memory_order_acquire)&&!stop.stop_requested())
                                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }
                            if(stop.stop_requested()){completion->result.cancelled=true;completion->result.detail=L"Neural render was cancelled.";goto finish;}
                            reuseKey=prefetch->key;
                        }
                        if(!reuseKey.empty()){
                            const auto cached=cache.LookupSource(reuseKey);
                            if(!cached||cached->manifest.encoder!=kCompleteSourcePolicy){completion->cachedSourceUnavailable=true;goto finish;}
                            sourcePath=cached->payloadPath;completion->sourceKey=reuseKey;
                            LOG("Owned source cache verified; network resolution skipped.");
                        }else{
                            const SourceAcquisition acquired=AcquireYouTubeSource(cache,moduleDirectory,mediaUrl,audioUrl,pageUrl,sourceQuality,expectedDurationSeconds,
                                [&]{NeuralRenderProgress acquiring{};acquiring.phase=NeuralRenderPhase::Acquiring;postProgress(acquiring);},stop);
                            if(!acquired.key.empty())completion->sourceKey=acquired.key;
                            if(acquired.path.empty()){completion->result.cancelled=acquired.cancelled;completion->result.detail=acquired.detail;goto finish;}
                            sourcePath=acquired.path;
                        }
                    }else sourcePath=std::filesystem::absolute(std::filesystem::path(mediaUrl));
                    completion->sourcePath=sourcePath;
                    const auto sourceDigest=Sha256File(sourcePath,stop);if(!sourceDigest){completion->result.cancelled=stop.stop_requested();completion->result.detail=L"The source digest could not be computed.";goto finish;}
                    VideoDecoder metadata;if(!metadata.Open(sourcePath.wstring(),MediaSourceKind::LocalFile,stop)){completion->result.detail=L"The source could not be decoded for neural rendering.";goto finish;}
                    const uint32_t width=metadata.NativeWidth(),height=metadata.NativeHeight();const double fps=metadata.FrameRate(),duration=metadata.DurationSeconds();metadata.Close();
                    if(!width||!height||!std::isfinite(fps)||fps<=0.0||!std::isfinite(duration)||duration<=0.0){completion->result.detail=L"The source metadata is incomplete.";goto finish;}progressWidth=width;progressHeight=height;
                    NeuralRenderProgress checking{};checking.phase=NeuralRenderPhase::CheckingCache;postProgress(checking);
                    const int64_t sourceDuration100ns=static_cast<int64_t>(std::llround(duration*10000000.0));
                    const int64_t frameDurationTolerance=static_cast<int64_t>(std::ceil(10000000.0/fps));
                    if(!range.Whole()&&(range.start100ns<0||range.end100ns<=range.start100ns||range.start100ns>=sourceDuration100ns)){completion->result.failure=NeuralRenderFailure::Source;completion->result.detail=L"The requested render range lies outside the source.";goto finish;}
                    // A range render is exactly [start,end) long; a whole render matches the source.
                    const int64_t expectedDuration100ns=range.Whole()?sourceDuration100ns:range.end100ns-range.start100ns;
                    const auto runtimeDirectory=moduleDirectory/L"neural-runtime";
                    const auto runtimeDigest=BuildRuntimeDigest(runtimeDirectory,LockedRuntimeFileNames(),stop);if(!runtimeDigest){completion->result.cancelled=stop.stop_requested();completion->result.detail=L"The configured neural runtime is incomplete.";goto finish;}
                    // The lock names every drifted file; a mismatch is refused, never repaired by swapping runtimes.
                    const std::vector<RuntimeLockCheck> lockChecks=VerifyRuntimeLock(runtimeDirectory,EmbeddedRuntimeLock(),stop);
                    if(stop.stop_requested()){completion->result.cancelled=true;completion->result.detail=L"Neural render was cancelled.";goto finish;}
                    if(!RuntimeLockSatisfied(lockChecks)){const std::wstring drift=DescribeRuntimeLockDrift(lockChecks);LOG("Neural runtime lock drift; render refused: "<<WideToUtf8(drift));completion->result.failure=NeuralRenderFailure::Preflight;completion->result.detail=L"The neural runtime does not match the locked stack: "+drift;goto finish;}
                    // One writer at a time: the settings written below and the
                    // helper's proxy log are shared per runtime directory, so a
                    // second player instance must not interleave with this job.
                    NeuralRuntimeLease runtimeLease(runtimeDirectory);
                    if(!runtimeLease.Held()){completion->result.failure=NeuralRenderFailure::Preflight;completion->result.detail=L"Another neural render is using the experimental runtime. Wait for it to finish, then try again.";LOG("Neural runtime is in use by another render; refusing to share it.");goto finish;}
                    const auto overrides=NeuralAddonOverridesFor(settings);const auto configured=ConfigureNeuralAddon(runtimeDirectory/L"ReShade.ini",true,overrides);
                    if(!configured.ok){completion->result.detail=L"The neural settings could not be prepared.";goto finish;}
                    std::wstring settingsError;const auto settingsSnapshot=ReadNeuralAddonSettingsSnapshot(runtimeDirectory/L"ReShade.ini",&settingsError);
                    if(!settingsSnapshot){completion->result.detail=settingsError.empty()?L"The neural settings could not be read.":settingsError;goto finish;}
                    const auto settingsDigest=Sha256Bytes(*settingsSnapshot);if(!settingsDigest){completion->result.detail=L"The neural settings digest could not be computed.";goto finish;}
                    NeuralCacheIdentity identity{*sourceDigest,width,height,DLSS_VIDEO_PLAYER_VERSION,GpuPathName(gpu),*runtimeDigest,"DLAA|strict-timeline-v3|armed-inline-interception-v3",false,*settingsDigest,range,guides.IsDefault()?std::string{}:CanonicalGuideControls(guides)};const std::string renderKey=BuildNeuralCacheKey(identity);completion->renderKey=renderKey;completion->range=range;completion->settings=settings;completion->guides=guides;
                    LOG("Checking neural cache key="<<renderKey<<" range=["<<range.start100ns<<","<<range.end100ns<<") guides="<<CanonicalGuideControls(guides)<<" settings="<<CanonicalNeuralSettings(settings));
                    if(const auto cached=cache.LookupRender(renderKey)){
                        // LookupRender already verifies the full payload hash and
                        // strict feature-18 manifest. Do not decode every frame again.
                        const ProbeResult cachedProbe=ProbeMedia(moduleDirectory,cached->payloadPath,stop,MediaProbeMode::CachedMetadata);
                        if(stop.stop_requested()){completion->result.cancelled=true;completion->result.detail=L"Neural render was cancelled.";goto finish;}
                        const int64_t durationTolerance=std::max<int64_t>(1,cached->manifest.duration100ns/static_cast<int64_t>(cached->manifest.frameCount)+1);
                        const bool valid=cachedProbe.ok&&cached->manifest.sourceDigest==*sourceDigest&&cached->manifest.runtimeDigest==*runtimeDigest&&cached->manifest.settingsDigest==*settingsDigest&&cached->manifest.rangeStart100ns==range.start100ns&&cached->manifest.rangeEnd100ns==range.end100ns&&cached->manifest.guides==identity.guides&&cachedProbe.width==width&&cachedProbe.height==height&&cachedProbe.width==cached->manifest.width&&cachedProbe.height==cached->manifest.height&&std::llabs(cachedProbe.duration100ns-cached->manifest.duration100ns)<=durationTolerance&&std::llabs(cachedProbe.duration100ns-expectedDuration100ns)<=frameDurationTolerance;
                        if(valid){completion->result.ok=true;completion->result.frameCount=cached->manifest.frameCount;completion->result.duration100ns=cached->manifest.duration100ns;completion->result.jobId=cached->manifest.jobId;completion->result.historyResets=cached->manifest.historyResets;completion->result.firstTimestamp100ns=cached->manifest.rangeStart100ns;completion->sourcePath=sourcePath;completion->neuralPath=cached->payloadPath;completion->cacheHit=true;completion->range={cached->manifest.rangeStart100ns,cached->manifest.rangeEnd100ns};if(!cached->manifest.receiptDigest.empty()&&std::filesystem::is_regular_file(cached->directory/L"receipt.json"))completion->receiptPath=cached->directory/L"receipt.json";goto finish;}
                        if(!cache.Quarantine(*cached)){completion->result.detail=L"The invalid neural cache entry could not be quarantined.";goto finish;}
                    }
                    if(prepareOnly){completion->preparedOnly=true;LOG("Neural cache miss; opening the original for range selection.");goto finish;}
                    LOG("Neural cache miss or invalid entry; starting a new render.");
                    // The feature-18 probe needs the GPU; only a cache miss pays for it.
                    NeuralRenderProgress preflighting{};preflighting.phase=NeuralRenderPhase::Preflight;postProgress(preflighting);
                    const auto workerExecutable=runtimeDirectory/L"NeuralWorker.exe";
                    const NeuralPreflightResult preflight=RunNeuralPreflight(workerExecutable,stop);
                    if(preflight.cancelled||stop.stop_requested()){completion->result.cancelled=true;completion->result.detail=L"Neural render was cancelled.";goto finish;}
                    if(!preflight.ok){completion->result.failure=NeuralRenderFailure::Preflight;completion->result.detail=preflight.detail.empty()?L"The neural runtime preflight failed.":preflight.detail;LOG("Neural preflight failed: "<<WideToUtf8(completion->result.detail)<<(preflight.json.empty()?"":" receipt=")<<preflight.json);goto finish;}
                    const auto staging=cache.BeginRenderStaging(renderKey);if(!staging){completion->result.detail=L"Neural render staging could not be created.";goto finish;}
                    {std::ofstream settingsFile(*staging/L"neural-settings.ini",std::ios::binary|std::ios::trunc);settingsFile.write(settingsSnapshot->data(),static_cast<std::streamsize>(settingsSnapshot->size()));if(!settingsFile){cache.MarkInvalid(*staging);completion->result.detail=L"The neural settings snapshot could not be staged.";goto finish;}}
                    NeuralRenderRequest request{nullptr,sourcePath,liveIndex?liveDirectory/L"neural.mkv":*staging/L"neural.mkv",width,height,fps,duration};request.jobId=generation;request.range=range;request.prerollFrames=PrerollFramesFor(range,fps);request.guides=guides;request.pauseEvent=pauseEvent;request.segmentFrames=liveIndex?segmentFrames:0u;
                    NeuralRenderReceiptInputs receipt{preflight.json,lockChecks,request,{},renderKey,*settingsDigest,*runtimeDigest,std::chrono::system_clock::now(),{}};
                    NeuralSegmentSink sink{};
                    if(liveIndex){
                        // Every finalized segment is playable on arrival; the
                        // player reads them behind the render head.
                        sink.onSegment=[&](const NeuralRenderSegment& segment){
                            NeuralSegment entry{};entry.path=liveDirectory/segment.fileName;entry.index=liveIndexBase+segment.index;entry.firstFrameNumber=segment.firstFrameNumber;entry.firstTimestamp100ns=segment.firstTimestamp100ns;entry.end100ns=segment.end100ns;entry.frameCount=segment.frameCount;
                            LOG("Neural segment "<<entry.index<<" frames="<<segment.frameCount<<" firstFrame="<<segment.firstFrameNumber<<" span=["<<double(segment.firstTimestamp100ns)*1e-7<<","<<double(segment.end100ns)*1e-7<<") file="<<WideToUtf8(segment.fileName));
                            liveIndex->Append(std::move(entry));
                        };
                        // A relaunched worker republishes from its own index 0,
                        // so only this job's segments are discarded; coverage a
                        // previous job left behind stays valid.
                        sink.onRestart=[&]{LOG("Neural render restarted from zero; discarding this job's published segments.");liveIndex->TruncateTo(liveIndexBase);};
                    }
                    completion->result=RunNeuralWorker(workerExecutable,request,postProgress,stop,sink);
                    if(liveIndex)liveIndex->Finish();
                    receipt.result=completion->result;receipt.finished=std::chrono::system_clock::now();
                    LOG("Neural render receipt: "<<SummarizeNeuralReceiptForLog(receipt));
                    if(!completion->result.ok){cache.MarkInvalid(*staging);goto finish;}
                    if(liveIndex){
                        // The session's cache entry is one file, joined from the
                        // segments playback is still reading.
                        std::vector<std::filesystem::path> parts;parts.reserve(liveIndex->Count());
                        for(size_t index=0;index<liveIndex->Count();++index)if(const auto segment=liveIndex->At(index))parts.push_back(segment->path);
                        if(parts.empty()||ConcatenateMedia(moduleDirectory,parts,*staging/L"neural.mkv",stop)!=EncodeError::None){cache.MarkInvalid(*staging);completion->result.ok=false;completion->result.detail=L"The rendered segments could not be joined into a cache entry.";goto finish;}
                    }
                    const auto finalSettings=ReadNeuralAddonSettingsSnapshot(runtimeDirectory/L"ReShade.ini");if(!finalSettings||*finalSettings!=*settingsSnapshot){cache.MarkInvalid(*staging);completion->result.ok=false;completion->result.detail=L"Neural settings changed during rendering. Try the render again.";goto finish;}
                    const std::string receiptJson=BuildNeuralRenderReceiptJson(receipt);const auto receiptDigest=Sha256Bytes(receiptJson);
                    {std::ofstream receiptFile(*staging/L"receipt.json",std::ios::binary|std::ios::trunc);receiptFile.write(receiptJson.data(),static_cast<std::streamsize>(receiptJson.size()));if(!receiptFile||!receiptDigest){cache.MarkInvalid(*staging);completion->result.ok=false;completion->result.detail=L"The neural render receipt could not be staged.";goto finish;}}
                    const ProbeResult probe=ProbeMedia(moduleDirectory,*staging/L"neural.mkv",stop);
                    if(stop.stop_requested()){cache.MarkInvalid(*staging);completion->result.cancelled=true;completion->result.ok=false;completion->result.detail=L"Neural render was cancelled.";goto finish;}
                    NeuralCacheManifest manifest{};manifest.sourceDigest=*sourceDigest;manifest.runtimeDigest=*runtimeDigest;manifest.encoder=completion->result.encoder==EncoderKind::HevcNvenc?"hevc_nvenc":"h264_software";manifest.width=width;manifest.height=height;manifest.frameCount=completion->result.frameCount;manifest.duration100ns=completion->result.duration100ns;manifest.nativeEvaluations=completion->result.nativeEvaluations;manifest.verifiedNeuralFrames=completion->result.verifiedNeuralFrames;manifest.observedFeature18Evaluations=completion->result.evidence.highestObservedEvaluation;manifest.feature18Created=completion->result.evidence.feature18Created;manifest.feature18ArmedBeforeCapture=completion->result.feature18ArmedBeforeCapture;manifest.upscaling=false;
                    manifest.settingsDigest=*settingsDigest;manifest.rangeStart100ns=range.start100ns;manifest.rangeEnd100ns=range.end100ns;manifest.guides=identity.guides;manifest.jobId=generation;manifest.historyResets=completion->result.historyResets;manifest.receiptDigest=*receiptDigest;
                    const bool probeMatches=probe.ok&&probe.width==width&&probe.height==height&&probe.frameCount==completion->result.frameCount&&std::llabs(probe.duration100ns-completion->result.duration100ns)<=frameDurationTolerance&&std::llabs(probe.duration100ns-expectedDuration100ns)<=frameDurationTolerance&&std::llabs(completion->result.duration100ns-expectedDuration100ns)<=frameDurationTolerance;
                    NeuralCacheManifest publishCandidate=manifest;publishCandidate.kind=NeuralCacheEntryKind::Render;publishCandidate.state=NeuralCacheState::Complete;publishCandidate.neuralDigest=std::string(64,'0');
                    if(!CanPublishNeuralCompletion(completion->result.ok,probeMatches,IsReusableNeuralCacheManifest(publishCandidate))||!cache.PromoteRender(renderKey,*staging,manifest)){cache.MarkInvalid(*staging);completion->result.ok=false;completion->result.detail=L"The neural video failed final cache validation.";goto finish;}
                    if(const auto promoted=cache.LookupRender(renderKey)){completion->neuralPath=promoted->payloadPath;completion->receiptPath=promoted->directory/L"receipt.json";}else{completion->result.ok=false;completion->result.detail=L"The neural cache entry could not be reopened.";}
                }
            finish:
                completions->RegisterAndPost(std::move(completion),[&](uint64_t token){return PostMessageW(target,WM_NEURAL_COMPLETE,static_cast<WPARAM>(token),0)!=FALSE;});
            });
        }catch(const std::system_error&){m_neuralLifecycle.Invalidate();SyncSourceActionAvailability();const std::wstring message=L"The neural pre-render worker could not start.";MessageBoxW(m_hwnd,message.c_str(),T(L"app.title").c_str(),MB_OK|MB_ICONERROR);}
    }
    bool LoadCachedPlayback(const NeuralJobCompletion& completion){
        Unload();
        if(!m_decoder.Open(completion.sourcePath.wstring(),MediaSourceKind::LocalFile)||!m_synchronizedPlayback.Open(completion.sourcePath,completion.neuralPath,{},SynchronizedRange{completion.range.start100ns,completion.range.end100ns})){Unload();return false;}
        m_dar=m_decoder.DisplayAspectRatio();if(!std::isfinite(m_dar)||m_dar<0.2)m_dar=double(m_decoder.Width())/std::max(1u,m_decoder.Height());
        const auto [guideW,guideH]=TemporalGuideGenerator::AnalysisGrid(m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate());
        ShowWindow(m_viewport,SW_SHOW);Layout();m_renderer=MakeD3D12Renderer();
        if(!m_renderer||!m_renderer->Initialize(m_renderWnd,m_decoder.Width(),m_decoder.Height(),m_decoder.Width(),m_decoder.Height(),guideW,guideH,DefaultNeuralCarrierQuality())){Unload();return false;}
        m_renderer->SetDLSS(false);m_renderer->SetColorSettings(m_colorSettings);m_renderer->SetComparison(EffectiveComparison());m_activeQuality=DefaultNeuralCarrierQuality();
        const ComparisonView desiredView=m_neuralRequested?ComparisonView::Neural:ComparisonView::Original;
        // Decoder start-up (and any hardware-decode fallback) is asynchronous:
        // wait for the first pair within the same bound the seek path uses.
        SynchronizedReadResult firstRead=SynchronizedReadResult::NotReady;
        for(const auto deadline=Clock::now()+std::chrono::seconds(10);firstRead==SynchronizedReadResult::NotReady&&Clock::now()<deadline;){firstRead=m_synchronizedPlayback.ReadNextAvailable();if(firstRead==SynchronizedReadResult::NotReady)std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        if(firstRead!=SynchronizedReadResult::PairReady||!m_synchronizedPlayback.SetView(desiredView)||!m_synchronizedPlayback.VisibleFrame()){LOG("Cached playback could not produce its first synchronized pair (result="<<static_cast<int>(firstRead)<<").");Unload();return false;}
        m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;
        const VideoFrame first=*m_synchronizedPlayback.VisibleFrame();if(!RenderVideoFrame(first,true)){Unload();return false;}
        m_neuralPath=completion.neuralPath;m_cachedRange=completion.range;m_cachedReceiptPath=completion.receiptPath;m_cachedSettings=completion.settings;m_cachedGuides=completion.guides;m_currentSec=double(first.timestamp100ns)*1e-7;m_haveNext=false;m_cachedPlayback=true;m_comparisonView=desiredView;m_cachedPresentedFrames=1;RememberRenderedCachedPair();
        if(!m_decoder.IsStillImage())m_audio.Start(completion.sourcePath.wstring(),m_currentSec);m_audio.SetVolume(m_muted?0.0f:m_volume);m_playing=!m_decoder.IsStillImage();m_synchronizedPlayback.SetPaused(m_decoder.IsStillImage());m_playStartSec=m_currentSec;m_playStart=Clock::now();
        m_loaded=true;m_path=completion.sourcePath.wstring();m_sourceKind=completion.sourceKind;m_youtubePageUrl=completion.pageUrl;m_youtubeSourceQuality=completion.sourceQuality;m_displayTitle=DisplayTitleForSource(completion.sourceKind,completion.displayTitle);if(m_displayTitle.empty())m_displayTitle=completion.sourcePath.stem().wstring();
        m_droppedFrames=0;m_seekPending=false;m_seeking=false;m_fpsWindowStart=Clock::now();m_fpsWindowFrames=0;m_submitFps=0.0;m_guideReset=false;m_dlssReset=false;
        RestoreUpscaling();UpdateTitle();UpdateCachedStatus();Layout();SyncFeatureMenuState();InvalidateRect(m_hwnd,nullptr,TRUE);return true;
    }
    void CompleteNeuralProgress(uint64_t token){
        auto message=m_neuralProgressMessages.Take(token);if(!message||!m_neuralLifecycle.Accept(message->generation))return;
        // A pause report that arrives after the user already resumed is stale.
        if(message->progress.phase==NeuralRenderPhase::Paused&&!NeuralJobPaused())message->progress.phase=NeuralRenderPhase::NeuralRendering;
        m_neuralProgress=message->progress;m_neuralSourceWidth=message->width;m_neuralSourceHeight=message->height;
        const NeuralPlaybackState next=StateForProgressPhase(message->progress.phase,m_neuralLifecycle.state);
        // The worker may still report a frame that was in flight when the user
        // paused; the pause event, not that report, decides when rendering resumes.
        const bool holdPause=m_neuralLifecycle.state==NeuralPlaybackState::Paused&&next==NeuralPlaybackState::Rendering&&NeuralJobPaused();
        if(!holdPause)m_neuralLifecycle.Transition(next);
        InvalidateRect(m_hwnd,nullptr,FALSE);
    }
    void CompleteNeuralJob(uint64_t token){
        auto completion=m_neuralCompletions.Take(token);if(!completion||!m_neuralLifecycle.Accept(completion->generation))return;
        if(m_neuralWorker.joinable()){m_neuralWorker.join();m_neuralWorker=std::jthread{};}m_neuralProgressMessages.Clear();m_pendingNeuralTitle.clear();m_neuralCancelBounds={};
        // An active session already plays what the job produced: keep the media
        // loaded and only record how the job ended.
        if(m_previewJob)return CompletePausedPreview(*completion);
        if(m_liveSession)return CompleteLiveNeuralJob(*completion);
        if(completion->cachedSourceUnavailable){m_neuralLifecycle.Transition(NeuralPlaybackState::Failed);StartYouTubeResolution(completion->pageUrl,completion->displayTitle,completion->sourceQuality,0.0,true,NetworkCommitKind::InitialOpen,false);return;}
        if(completion->result.cancelled){m_neuralLifecycle.Transition(NeuralPlaybackState::OriginalOnly);if(!completion->sourcePath.empty()&&std::filesystem::is_regular_file(completion->sourcePath))LoadOriginalFallback(*completion);else InvalidateRect(m_hwnd,nullptr,FALSE);SyncSourceActionAvailability();return;}
        // Prepared: the source is cached and identified, no render exists yet.
        // Play the original so In/Out, previews and range renders are available.
        if(completion->preparedOnly){m_neuralLifecycle.Transition(NeuralPlaybackState::OriginalOnly);if(!LoadOriginalFallback(*completion))InvalidateRect(m_hwnd,nullptr,FALSE);SyncSourceActionAvailability();return;}
        if(completion->result.ok&&!completion->neuralPath.empty()&&LoadCachedPlayback(*completion)){m_neuralLifecycle.Transition(NeuralPlaybackState::Ready);RecordRecent(*completion);SyncSourceActionAvailability();if(completion->cacheHit)LOG("Verified neural cache hit opened without re-rendering.");else LOG("Verified neural cache playback opened.");return;}
        TransitionToFailure(completion->result.failure);LOG("Neural pre-render failed: kind="<<NeuralRenderFailureName(completion->result.failure)<<" state="<<WideToUtf8(NeuralPlaybackStateName(m_neuralLifecycle.state))<<" detail="<<WideToUtf8(completion->result.detail));
        SyncSourceActionAvailability();
        if(!completion->sourcePath.empty()&&std::filesystem::is_regular_file(completion->sourcePath)){m_neuralLifecycle.Transition(NeuralPlaybackState::OriginalOnly);LoadOriginalFallback(*completion);}
        else{std::wstring detail=completion->result.detail.empty()?L"Neural pre-render failed before playback could start.":completion->result.detail;if(const wchar_t* kind=NeuralFailureTextKey(completion->result.failure))detail=T(kind)+L"\n\n"+detail;MessageBoxW(m_hwnd,detail.c_str(),T(L"app.title").c_str(),MB_OK|MB_ICONERROR);InvalidateRect(m_hwnd,nullptr,FALSE);}
    }
    static void PrepareYouTubeMedia(YouTubeCompletion& completion,std::stop_token stop,[[maybe_unused]] uint32_t maxW,[[maybe_unused]] uint32_t maxH,[[maybe_unused]] bool qualityExplicit,[[maybe_unused]] NVSDK_NGX_PerfQuality_Value explicitQuality){
        if(!completion.result.ok||stop.stop_requested())return;
        completion.decoder=std::make_unique<VideoDecoder>();
        if(!completion.decoder->Open(completion.result.mediaUrl,MediaSourceKind::YouTube,stop)){completion.mediaErrorKey=stop.stop_requested()?L"youtube.error.cancelled":L"youtube.error.ffmpeg";return;}
        double dar=completion.decoder->DisplayAspectRatio();if(!std::isfinite(dar)||dar<0.2)dar=double(completion.decoder->Width())/std::max(1u,completion.decoder->Height());
        const auto ow=completion.decoder->Width(),oh=completion.decoder->Height();
        const auto quality=DefaultNeuralCarrierQuality();
        // Start the final decoder process at the requested network seek after any resize restart.
        if(completion.seekSeconds>0.0&&!completion.decoder->SeekSeconds(completion.seekSeconds)){completion.mediaErrorKey=L"youtube.error.ffmpeg";return;}
        const auto [guideW,guideH]=TemporalGuideGenerator::AnalysisGrid(completion.decoder->Width(),completion.decoder->Height(),completion.decoder->FrameRate());
        completion.configuration={completion.decoder->NativeWidth(),completion.decoder->NativeHeight(),completion.decoder->Width(),completion.decoder->Height(),completion.decoder->Width(),completion.decoder->Height(),ow,oh,guideW,guideH,static_cast<int>(quality)};
        const auto firstDeadline=Clock::now()+std::chrono::seconds(20);
        for(;;){
            const VideoReadResult read=completion.decoder->ReadNextAvailable(completion.firstFrame,stop);
            if(read==VideoReadResult::FrameReady)break;
            if(read==VideoReadResult::Cancelled||stop.stop_requested()){completion.mediaErrorKey=L"youtube.error.cancelled";break;}
            if(read==VideoReadResult::Stalled){completion.mediaErrorKey=L"youtube.error.media_stalled";break;}
            if(read==VideoReadResult::Error||read==VideoReadResult::EndOfStream){completion.mediaErrorKey=L"youtube.error.ffmpeg";break;}
            if(Clock::now()>=firstDeadline){completion.mediaErrorKey=L"youtube.error.media_timeout";completion.decoder->Close();break;}
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if(completion.mediaErrorKey.empty()){
            completion.audio=std::make_unique<AudioPlayer>();completion.audio->Start(completion.result.audioUrl,double(completion.firstFrame.timestamp100ns)*1e-7,AudioStartState::Paused);
        }
    }
    NetworkRenderConfiguration ActiveNetworkConfiguration()const{
        if(!m_loaded||!m_renderer)return{};const auto [guideW,guideH]=TemporalGuideGenerator::AnalysisGrid(m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate());
        return{m_decoder.NativeWidth(),m_decoder.NativeHeight(),m_decoder.Width(),m_decoder.Height(),m_renderer->DLSSInputW(),m_renderer->DLSSInputH(),m_renderer->OutputW(),m_renderer->OutputH(),guideW,guideH,static_cast<int>(m_activeQuality)};
    }
    void StartYouTubeResolution(const std::wstring& url,const std::wstring& displayTitle=L"",YouTubeSourceQuality sourceQuality=YouTubeSourceQuality::Auto,double seekSeconds=0.0,bool resumeAfter=true,NetworkCommitKind commitKind=NetworkCommitKind::InitialOpen,bool allowCachedSource=true){
        if(!IsSupportedYouTubeUrl(url))return;
        CancelYouTubeResolution();
        if(allowCachedSource&&commitKind==NetworkCommitKind::InitialOpen&&NeuralPreRenderEnabled()&&m_recent){
            const auto id=CanonicalYouTubeVideoId(url);
            for(const auto& entry:m_recent->Entries())if(entry.youtube&&entry.id==id&&entry.sourceQuality==static_cast<int>(sourceQuality)&&!entry.sourceKey.empty()){
                const auto sourceKey=entry.sourceKey;const auto title=displayTitle.empty()?entry.title:displayTitle;
                StartNeuralJob(url,{},title,url,MediaSourceKind::YouTube,sourceQuality,sourceKey,0.0,{},true);return;
            }
        }
        if(!m_youtubeResolver)m_youtubeResolver=std::make_unique<YouTubeResolver>();
        const uint64_t generation=m_youtubeLifecycle.Begin();
        m_pendingYouTubeTitle=DisplayTitleForSource(MediaSourceKind::YouTube,displayTitle);
        SyncSourceActionAvailability();
        LOG("YouTube resolution started.");
        try{
            HWND target=m_hwnd;YouTubeResolver* resolver=m_youtubeResolver.get();const std::wstring title=m_pendingYouTubeTitle;
            const uint32_t maxW=m_opt.maxW,maxH=m_opt.maxH;const bool qualityExplicit=m_opt.qualityExplicit;const auto explicitQuality=m_opt.quality;
            CompletionRegistry<YouTubeCompletion>* completions=&m_youtubeCompletions;
            m_youtubeWorker=std::jthread([target,resolver,completions,generation,url,title,sourceQuality,seekSeconds,resumeAfter,commitKind,maxW,maxH,qualityExplicit,explicitQuality](std::stop_token stop){
                auto completion=std::make_unique<YouTubeCompletion>();completion->generation=generation;completion->displayTitle=title;completion->pageUrl=url;completion->sourceQuality=sourceQuality;completion->seekSeconds=seekSeconds;completion->resumeAfterSeek=resumeAfter;completion->commitKind=commitKind;
                completion->requestedQualityExplicit=qualityExplicit;completion->requestedQuality=explicitQuality;
                completion->result=resolver->Resolve(url,sourceQuality,stop);
                PrepareYouTubeMedia(*completion,stop,maxW,maxH,qualityExplicit,explicitQuality);
                completions->RegisterAndPost(std::move(completion),[&](uint64_t token){return PostMessageW(target,WM_YOUTUBE_RESOLVED,static_cast<WPARAM>(token),0)!=FALSE;});
            });
        }catch(const std::system_error&){
            m_youtubeLifecycle.Invalidate();m_pendingYouTubeTitle.clear();SyncSourceActionAvailability();
            const std::wstring message=T(L"youtube.error.start_failed"),caption=T(L"app.title");MessageBoxW(m_hwnd,message.c_str(),caption.c_str(),MB_OK|MB_ICONERROR);
            LOG("YouTube worker could not be started.");
        }
    }
    void StartYouTubeSeek(double seconds,bool resumeAfter,NetworkCommitKind commitKind=NetworkCommitKind::Seek,std::optional<std::pair<bool,NVSDK_NGX_PerfQuality_Value>> qualityOverride=std::nullopt){
        if(!m_loaded||!NetworkPlayback()||m_path.empty())return;
        CancelYouTubeResolution();const uint64_t generation=m_youtubeLifecycle.Begin();SyncSourceActionAvailability();
        const std::wstring source=m_path,audioSource=m_youtubeAudioUrl,pageUrl=m_youtubePageUrl,title=m_displayTitle;const YouTubeSourceQuality sourceQuality=m_youtubeSourceQuality;const uint32_t maxW=m_opt.maxW,maxH=m_opt.maxH;const bool qualityExplicit=qualityOverride?qualityOverride->first:m_opt.qualityExplicit;const auto explicitQuality=qualityOverride?qualityOverride->second:m_opt.quality;const NetworkRenderConfiguration activeConfiguration=ActiveNetworkConfiguration();
        try{
            HWND target=m_hwnd;CompletionRegistry<YouTubeCompletion>* completions=&m_youtubeCompletions;
            m_youtubeWorker=std::jthread([target,completions,generation,source,audioSource,pageUrl,title,sourceQuality,seconds,resumeAfter,commitKind,maxW,maxH,qualityExplicit,explicitQuality,activeConfiguration](std::stop_token stop){
                auto completion=std::make_unique<YouTubeCompletion>();completion->generation=generation;completion->displayTitle=title;completion->pageUrl=pageUrl;completion->sourceQuality=sourceQuality;completion->commitKind=commitKind;completion->requestedQualityExplicit=qualityExplicit;completion->requestedQuality=explicitQuality;completion->resumeAfterSeek=resumeAfter;completion->seekSeconds=seconds;completion->result.ok=true;completion->result.error=ResolveError::None;completion->result.mediaUrl=source;completion->result.audioUrl=audioSource;
                PrepareYouTubeMedia(*completion,stop,maxW,maxH,qualityExplicit,explicitQuality);
                if(commitKind==NetworkCommitKind::Seek&&NetworkConfigurationMatchesExceptInput(activeConfiguration,completion->configuration)){completion->configuration.inputWidth=activeConfiguration.inputWidth;completion->configuration.inputHeight=activeConfiguration.inputHeight;}
                completions->RegisterAndPost(std::move(completion),[&](uint64_t token){return PostMessageW(target,WM_YOUTUBE_RESOLVED,static_cast<WPARAM>(token),0)!=FALSE;});
            });
        }catch(const std::system_error&){m_youtubeLifecycle.Invalidate();SyncSourceActionAvailability();const std::wstring message=T(L"youtube.error.start_failed"),caption=T(L"app.title");MessageBoxW(m_hwnd,message.c_str(),caption.c_str(),MB_OK|MB_ICONERROR);}
    }
    void ActivateYouTube(){
        if(!ToolbarActionEnabled(ToolbarAction::OpenYouTube))return;
        const auto url=PromptForYouTubeUrl(m_hwnd,m_loc,m_font);if(url)StartYouTubeResolution(*url,L"",m_youtubeSourceQuality);
    }
    void ActivateExampleVideo(const ExampleVideo& example){
        if(!ToolbarActionEnabled(ToolbarAction::OpenYouTube))return;
        StartYouTubeResolution(std::wstring(example.url),std::wstring(example.title),m_youtubeSourceQuality);
    }
    std::unique_ptr<PreparedRendererCandidate> CreateRendererCandidate(const YouTubeCompletion& completion){
        auto candidate=std::make_unique<PreparedRendererCandidate>();candidate->configuration=completion.configuration;
        candidate->window=CreateWindowExW(WS_EX_ACCEPTFILES,L"DLSSVideoRenderClassV11",nullptr,WS_CHILD|WS_CLIPSIBLINGS,0,0,100,100,m_viewport,nullptr,GetModuleHandleW(nullptr),this);
        if(!candidate->window)return{};
        candidate->renderer=MakeD3D12Renderer();
        const auto quality=static_cast<NVSDK_NGX_PerfQuality_Value>(completion.configuration.quality);
        if(!candidate->renderer->Initialize(candidate->window,completion.configuration.decodeWidth,completion.configuration.decodeHeight,completion.configuration.outputWidth,completion.configuration.outputHeight,completion.configuration.guideWidth,completion.configuration.guideHeight,quality))return{};
        candidate->renderer->SetDLSS(false);candidate->renderer->SetColorSettings(m_colorSettings);candidate->renderer->SetComparison(EffectiveComparison());
        if(m_renderer)candidate->renderer->SetDebugView(m_renderer->GetDebugView());
        candidate->configuration.inputWidth=candidate->renderer->DLSSInputW();candidate->configuration.inputHeight=candidate->renderer->DLSSInputH();
        return candidate;
    }
    bool ValidatePreparedFrame(const YouTubeCompletion& completion,D3D12Renderer& renderer,TemporalGuideGenerator& guides){
        if(!NetworkPreparedGeometryIsValid(completion.configuration,completion.decoder->Width(),completion.decoder->Height(),completion.firstFrame.bgra.size()))return false;
        const FrameIdentity identity=IdentityOf(completion.firstFrame,0,0,HistoryReset::FirstFrame);
        GuideFrame guide;if(!guides.Generate(completion.firstFrame.bgra.data(),completion.configuration.decodeWidth,completion.configuration.decodeHeight,renderer.DLSSInputW(),renderer.DLSSInputH(),completion.decoder->FrameRate(),identity,guide))return false;
        const float frameMs=float(1000.0/std::max(1.0,completion.decoder->FrameRate()));
        return renderer.RenderFrame(completion.firstFrame.bgra.data(),completion.firstFrame.bgra.size(),identity,guide,frameMs);
    }
    bool InstallPreparedYouTube(YouTubeCompletion& completion,std::unique_ptr<PreparedRendererCandidate> candidate){
        const std::wstring source=std::move(completion.result.mediaUrl),audioSource=std::move(completion.result.audioUrl),pageUrl=std::move(completion.pageUrl),title=std::move(completion.displayTitle);const bool shouldPlay=completion.commitKind==NetworkCommitKind::InitialOpen?true:completion.resumeAfterSeek;HWND oldRenderWindow=nullptr;D3D12RendererOwner oldRenderer;std::unique_ptr<AudioPlayer> oldNetworkAudio;
        const bool viewportWasVisible=IsWindowVisible(m_viewport)!=FALSE;
        const bool committed=CommitPreparedAudioHandoff(
            [&]{
                m_decoder.Swap(*completion.decoder);oldNetworkAudio=std::move(m_networkAudio);m_networkAudio=std::move(completion.audio);
                oldRenderWindow=m_renderWnd;oldRenderer=std::move(m_renderer);m_renderWnd=candidate->window;candidate->window=nullptr;m_renderer=std::move(candidate->renderer);completion.configuration=candidate->configuration;
            },
            [&]{
                if(!IsWindow(m_viewport))return false;
                ShowWindow(m_viewport,SW_SHOW);
                const bool shown=IsWindowVisible(m_viewport)&&
                                 ShowPreparedRenderWindow(m_viewport,candidate->window);
                if(!shown&&!viewportWasVisible)ShowWindow(m_viewport,SW_HIDE);
                return shown;
            },
            [&]{
                if(oldNetworkAudio)oldNetworkAudio.reset();else m_audio.Stop();
                completion.decoder.reset();oldRenderer.reset();
                if(oldRenderWindow&&oldRenderWindow!=m_renderWnd)DestroyWindow(oldRenderWindow);
            },
            [&]{
                m_activeQuality=static_cast<NVSDK_NGX_PerfQuality_Value>(completion.configuration.quality);m_opt.qualityExplicit=completion.requestedQualityExplicit;m_opt.quality=completion.requestedQuality;
                m_dar=m_decoder.DisplayAspectRatio();if(!std::isfinite(m_dar)||m_dar<0.2)m_dar=double(m_decoder.Width())/std::max(1u,m_decoder.Height());
                m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=completion.firstFrame.timestamp100ns;m_lastPlaybackFrame=completion.firstFrame;m_lastGlobalX=0;m_lastGlobalY=0;
                m_currentSec=double(completion.firstFrame.timestamp100ns)*1e-7;m_haveNext=false;m_waitingForNetworkFrame=true;m_networkReadState.Reset();m_playing=shouldPlay;m_playStartSec=m_currentSec;m_playStart=Clock::now();m_loaded=true;m_path=source;m_youtubeAudioUrl=audioSource;m_youtubePageUrl=pageUrl;m_youtubeSourceQuality=completion.sourceQuality;m_sourceKind=MediaSourceKind::YouTube;m_displayTitle=DisplayTitleForSource(MediaSourceKind::YouTube,title);m_droppedFrames=completion.commitKind==NetworkCommitKind::InitialOpen?0:m_droppedFrames;m_seekPending=false;m_seeking=false;m_fpsWindowStart=Clock::now();m_fpsWindowFrames=0;m_submitFps=0.0;
                Audio().SetVolume(m_muted?0.0f:m_volume);Audio().Pause(!shouldPlay);
            });
        if(!committed)return false;
        RecordOriginalRecent();
        RestoreUpscaling();
        UpdateYouTubeQualitySelection(GetMenu(m_hwnd),m_youtubeSourceQuality);DrawMenuBar(m_hwnd);
        UpdateTitle();UpdateCachedStatus();Layout();InvalidateRect(m_hwnd,nullptr,TRUE);
        return true;
    }
    void CompleteYouTubeResolution(uint64_t token){
        std::unique_ptr<YouTubeCompletion> completion=m_youtubeCompletions.Take(token);if(!completion)return;
        if(!m_youtubeLifecycle.Complete(completion->generation))return;
        if(m_youtubeWorker.joinable()){m_youtubeWorker.join();m_youtubeWorker=std::jthread{};}
        m_pendingYouTubeTitle.clear();SyncSourceActionAvailability();
        if(!completion->result.ok){
            if(completion->result.error==ResolveError::Cancelled)return;
            const std::wstring message=T(YouTubeResolveErrorMessageKey(completion->result.error).data()),caption=T(L"app.title");
            MessageBoxW(m_hwnd,message.c_str(),caption.c_str(),MB_OK|MB_ICONERROR);LOG("YouTube resolution failed without exposing source details.");return;
        }
        // Play the resolved stream now. Acquiring a local copy is the render's
        // job, so opening a video never waits for a whole-video download.
        if(!completion->mediaErrorKey.empty()){
            if(completion->mediaErrorKey==L"youtube.error.cancelled")return;
            const std::wstring message=T(completion->mediaErrorKey.c_str()),caption=T(L"app.title");MessageBoxW(m_hwnd,message.c_str(),caption.c_str(),MB_OK|MB_ICONERROR);LOG("YouTube media preparation failed without exposing source details.");return;
        }
        if(!completion->decoder||completion->firstFrame.bgra.empty())return;
        LOG("YouTube resolution and background media preparation completed.");
        bool candidateCreated=false;
        const bool committed=ExecuteNetworkCandidateTransaction<PreparedRendererCandidate>(
            [&]{auto candidate=CreateRendererCandidate(*completion);candidateCreated=candidate!=nullptr;return candidate;},
            [&](PreparedRendererCandidate& candidate){return ValidatePreparedFrame(*completion,*candidate.renderer,candidate.guides);},
            [&](std::unique_ptr<PreparedRendererCandidate> candidate){return InstallPreparedYouTube(*completion,std::move(candidate));});
        if(!committed){const std::wstring message=T(candidateCreated?L"error.frame":L"error.renderer"),caption=T(L"app.title");MessageBoxW(m_hwnd,message.c_str(),caption.c_str(),MB_ICONERROR);LOG("Prepared YouTube renderer transaction rolled back; active state preserved.");}
    }
    PlayerRuntimeStatus RuntimeStatus()const{
        return ResolvePlayerRuntimeStatus(m_opt.safeMode,m_opt.neuralAddonConfigured,m_renderer&&m_renderer->DLSSEnabled(),m_renderer&&m_renderer->DLSSFeatureCreated());
    }
    // Lead-in state of an active session, in front of everything else because it
    // is why playback is waiting.
    std::wstring LiveSessionStatusText()const{
        wchar_t lead[64]={};swprintf_s(lead,L"%.1f s",LiveLeadSeconds());
        std::wstring text=(m_liveBuffering?T(L"neural.live.buffering"):T(L"neural.live.title"))+L" \u00b7 "+lead+L" "+T(L"neural.live.lead")+
            L" \u00b7 head "+FormatTimecode(m_liveSegments?m_liveSegments->Head100ns():0,m_decoder.FrameRate(),true);
        // The forecast is a constant for one GPU; this is what the render is
        // actually managing here, including whatever else the machine is doing.
        if(const double ratio=LiveRealtimeRatio();ratio>0.0&&ratio<0.98){
            wchar_t pace[48]={};swprintf_s(pace,L" \u00b7 %.2fx real time",ratio);text+=pace;
        }
        return text;
    }
    // Video seconds covered per second of wall clock.
    double LiveRealtimeRatio()const{
        if(!m_liveSession||!m_liveSegments||!m_liveStartTick)return 0.0;
        return live_session::RealtimeRatio(double(m_liveSegments->Head100ns()-m_liveRange.start100ns)*1e-7,
                                           double(GetTickCount64()-m_liveStartTick)/1000.0);
    }
    std::wstring BuildStatusText()const{
        if(m_exportWorker.joinable())return L"Exporting processed media - File > Cancel export to stop";
        PlayerStatusSnapshot status{};if(m_youtubeLifecycle.IsResolving()){status.activity=PlayerStatusActivity::ResolvingYouTube;return BuildPlayerStatusText(status);}if(!m_loaded||!m_renderer)return{};
        if(m_cachedPlayback){std::wstring text=(m_liveSession?std::wstring{}:std::wstring(L"Neural cached playback · "))+T(m_comparisonView==ComparisonView::Neural?L"neural.view.rendered":L"neural.view.original")+L" · "+UpscalingStatus()+L" · FG unavailable · Source "+std::to_wstring(m_decoder.NativeWidth())+L"×"+std::to_wstring(m_decoder.NativeHeight())+L" · FPS "+std::to_wstring(static_cast<int>(std::lround(m_submitFps)))+L" rendered / "+std::to_wstring(static_cast<int>(std::lround(m_decoder.FrameRate())))+L" source · Dropped "+std::to_wstring(m_droppedFrames);if(!m_cachedRange.Whole())text+=L" · Range "+FormatTimecode(m_cachedRange.start100ns,m_decoder.FrameRate(),true)+L"\u2013"+FormatTimecode(m_cachedRange.end100ns,m_decoder.FrameRate(),true);if(const std::wstring markers=MarkerStatusText();!markers.empty())text+=L" \u00b7 "+markers;text+=L" · "+NeuralSettingsSummary(m_cachedSettings,m_cachedGuides);if(m_liveSession)text=LiveSessionStatusText()+L" · "+text;if(m_seeking||m_seekPending)text=T(L"status.seeking")+L" · "+text;return text;}
        const PlayerRuntimeStatus runtime=RuntimeStatus();status.mediaLoaded=true;status.runtimeConfiguration=runtime.configuration;status.dlssState=runtime.dlssState;status.sourceWidth=m_decoder.NativeWidth();status.sourceHeight=m_decoder.NativeHeight();status.inputWidth=m_renderer->DLSSInputW();status.inputHeight=m_renderer->DLSSInputH();status.outputWidth=m_renderer->OutputW();status.outputHeight=m_renderer->OutputH();status.quality=QualityNameW(m_activeQuality);status.renderedFps=m_submitFps;status.sourceFps=m_decoder.FrameRate();status.droppedFrames=m_droppedFrames;
        status.upscalingStatus=UpscalingStatus();std::wstring text=BuildPlayerStatusText(status);
        // Lead with what was marked, or with how to mark, because the runtime
        // detail behind it is what a narrow window truncates.
        if(const std::wstring markers=MarkerStatusText();!markers.empty())text=markers+L" \u00b7 "+text;
        else if(!m_liveSession&&RangeRenderAvailable())text=T(L"status.render_hint")+L" \u00b7 "+text;
        if(m_liveSession)text=LiveSessionStatusText()+L" \u00b7 "+text;
        if(SourcePrefetchActive())text=T(L"status.preparing_source")+L" \u00b7 "+text;
        if(m_seeking||m_seekPending)text=T(L"status.seeking")+L" \u00b7 "+text;return text;
    }
    // Short canonical of the settings a cache entry was rendered with; the
    // full record is its receipt.json.
    static std::wstring NeuralSettingsSummary(const NeuralSettings& settings,const GuideControls& guides){
        std::wstring text=L"NR "+PlainValue(settings.intensity)+L"/struct "+PlainValue(settings.localStructure)+L"/tone "+PlainValue(settings.localTone);
        if(!guides.IsDefault()){const std::string canonical=CanonicalGuideControls(guides);text+=L"/"+std::wstring(canonical.begin(),canonical.end());}
        return text;
    }
    // Marker timecodes without a leading separator; callers place it.
    std::wstring MarkerStatusText()const{
        std::wstring text;const double fps=m_decoder.FrameRate();
        if(m_markers.in100ns)text=L"In "+FormatTimecode(*m_markers.in100ns,fps,true);
        if(m_markers.out100ns)text+=(text.empty()?std::wstring{}:std::wstring(L" \u00b7 "))+L"Out "+FormatTimecode(*m_markers.out100ns,fps,true);
        return text;
    }
    void RestartInSafeMode(){
        const std::wstring confirmation=T(L"safe_mode.confirm"),title=T(L"menu.safe_mode");
        const int answer=MessageBoxW(m_hwnd,confirmation.c_str(),title.c_str(),MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2);
        std::wstring launchError;
        const SafeModeRestartOutcome outcome=ExecuteAdvancedSafeModeRestart(
            answer==IDYES,
            m_opt.userArguments,
            [&](const std::vector<std::wstring>& arguments){return LaunchSameExecutable(arguments,launchError);});
        if(outcome==SafeModeRestartOutcome::LaunchFailed){
            LOG("Safe-mode restart failed: "<<WideToUtf8(launchError));
            const std::wstring message=T(L"safe_mode.launch_failed"),appTitle=T(L"app.title");
            MessageBoxW(m_hwnd,message.c_str(),appTitle.c_str(),MB_OK|MB_ICONERROR);
            return;
        }
        if(outcome==SafeModeRestartOutcome::CloseCurrent)DestroyWindow(m_hwnd);
    }
    void ClearNeuralCache(){
        if(ActivityBusy()||m_exportWorker.joinable()){MessageBoxW(m_hwnd,L"Finish or cancel rendering and export before clearing the cache.",T(L"menu.clear_neural_cache").c_str(),MB_OK|MB_ICONINFORMATION);return;}
        NeuralCacheManager cache(m_cacheRoot);if(!cache.Valid()){MessageBoxW(m_hwnd,L"The neural cache directory is unavailable.",T(L"menu.clear_neural_cache").c_str(),MB_OK|MB_ICONERROR);return;}
        const uintmax_t bytes=cache.SizeBytes();const std::wstring prompt=L"Close playback and delete "+std::to_wstring(bytes/(1024*1024))+L" MiB of neural cache data? Local original files will be kept.";
        if(MessageBoxW(m_hwnd,prompt.c_str(),T(L"menu.clear_neural_cache").c_str(),MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
        Unload();
        if(!cache.Clear())MessageBoxW(m_hwnd,L"The neural cache could not be fully cleared.",T(L"menu.clear_neural_cache").c_str(),MB_OK|MB_ICONERROR);
        if(m_recent){auto entries=m_recent->Entries();for(auto it=entries.rbegin();it!=entries.rend();++it){it->sourceKey.clear();it->renderKey.clear();m_recent->Remember(*it);}m_recent->Save();}
        m_pendingCacheEvictions.clear();SyncSourceActionAvailability();
    }
    void ShowDebugMenu(const RECT& anchor){
        UINT selected=IDM_VIEW_FINAL;
        if(m_renderer){switch(m_renderer->GetDebugView()){case D3D12Renderer::DebugView::Input:selected=IDM_VIEW_INPUT;break;case D3D12Renderer::DebugView::MotionVectors:selected=IDM_VIEW_MV;break;case D3D12Renderer::DebugView::Depth:selected=IDM_VIEW_DEPTH;break;case D3D12Renderer::DebugView::Final:break;}}
        HMENU menu=app_menu::CreateDebugViewMenu(selected);if(!menu)return;
        POINT point{anchor.left,anchor.top};ClientToScreen(m_hwnd,&point);
        const UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_LEFTALIGN|TPM_BOTTOMALIGN|TPM_RIGHTBUTTON,point.x,point.y,0,m_hwnd,nullptr);
        DestroyMenu(menu);if(command)HandleCommand(command);
    }

    void ActivateToolbarAction(ToolbarAction action,const RECT& anchor){
        if(!ToolbarActionEnabled(action))return;
        switch(action){
        case ToolbarAction::Open:OpenFromDialog();break;
        case ToolbarAction::OpenYouTube:ActivateYouTube();break;
        case ToolbarAction::Back10:RequestSeek(Position()-10);break;
        case ToolbarAction::PlayPause:TogglePause();break;
        case ToolbarAction::Stop:StopPlayback();break;
        case ToolbarAction::Forward10:RequestSeek(Position()+10);break;
        case ToolbarAction::Mute:ToggleMute();break;
        case ToolbarAction::ToggleNeuralRendering:ToggleNeuralRendering();break;
        case ToolbarAction::ToggleUpscaling:ToggleUpscaling();break;
        case ToolbarAction::ToggleFrameGeneration:break;
        case ToolbarAction::Aspect:m_fill=!m_fill;Layout();break;
        case ToolbarAction::Adjustments:ShowAdjustments();break;
        case ToolbarAction::DebugView:ShowDebugMenu(anchor);break;
        case ToolbarAction::Fullscreen:ToggleFullscreen();break;
        case ToolbarAction::None:break;
        }
    }

    void FocusNextToolbarAction(bool reverse){
        RevealFullscreenControls();m_fullscreenKeyboardFocus=m_fullscreen;
        const auto items=FocusableItems();m_focusedToolbarAction=NextFocusableToolbarAction(items,m_focusedToolbarAction,reverse,ToolbarState());InvalidateControls();
    }

    void ActivateFocusedToolbarAction(){
        if(m_focusedToolbarAction==ToolbarAction::None)return;
        const auto items=FocusableItems();for(const auto& item:items)if(item.action==m_focusedToolbarAction){ActivateToolbarAction(item.action,item.bounds);return;}
    }

    void MouseDown(int x,int y){
        SetFocus(m_hwnd);
        m_fullscreenKeyboardFocus=false;
        if(ActivityBusy()&&PtIn(m_neuralCancelBounds,x,y)){if(m_liveSession)StopLiveNeuralSession(true);else if(NeuralJobActive())CancelNeuralJob();else CancelYouTubeResolution();return;}
        if(!ControlsVisible())return;
        if(!m_loaded){const auto items=FocusableItems();const ToolbarAction action=ResolveToolbarHover(items,POINT{x,y},ToolbarState());if(action!=ToolbarAction::None){m_focusedToolbarAction=action;m_pressedToolbarAction=action;SetCapture(m_hwnd);for(const auto& item:items)if(item.action==action){InvalidateRect(m_hwnd,&item.bounds,FALSE);break;}}return;}
        if(!m_seeking){RECT tr=TimelineRect();if(PtIn(tr,x,y)){m_dragSeek=true;m_dragWasPlaying=m_playing;m_lastScrubSeek={};m_seekPreview=SecondsFromX(x);SetCapture(m_hwnd);InvalidateControls();return;}const auto vr=VolumeRect();if(vr&&PtIn(*vr,x,y)){m_dragVolume=true;SetCapture(m_hwnd);SetVolumeFromX(x);return;}}
        const auto items=ToolbarItems();const ToolbarAction action=ResolveToolbarHover(items,POINT{x,y},ToolbarState());if(action!=ToolbarAction::None){m_focusedToolbarAction=action;m_pressedToolbarAction=action;SetCapture(m_hwnd);InvalidateControls();}
    }

    void MouseUp(int x,int y){
        if(m_dragSeek){
            const double target=m_seekPreview;m_dragSeek=false;if(GetCapture()==m_hwnd)ReleaseCapture();
            // The scrub already decoded this frame: restore sound and playback
            // instead of paying for an identical seek on release.
            const double frame=1.0/std::max(1.0,m_decoder.FrameRate());
            if(!m_seeking&&!m_seekPending&&std::abs(target-m_currentSec)<=frame*0.5){
                EndScrub();InvalidateControls();InvalidatePlaybackProgress();return;
            }
            RequestSeek(target,m_dragWasPlaying);return;
        }
        if(m_dragVolume){m_dragVolume=false;if(GetCapture()==m_hwnd)ReleaseCapture();return;}
        const ToolbarAction pressed=m_pressedToolbarAction;if(pressed==ToolbarAction::None)return;
        m_pressedToolbarAction=ToolbarAction::None;if(GetCapture()==m_hwnd)ReleaseCapture();
        if(!m_loaded){const auto items=FocusableItems();for(const auto& item:items)if(item.action==pressed){InvalidateRect(m_hwnd,&item.bounds,FALSE);if(PtIn(item.bounds,x,y)&&ToolbarActionEnabled(pressed))ActivateToolbarAction(pressed,item.bounds);break;}return;}
        const auto items=ToolbarItems();const ToolbarAction released=ResolveToolbarHover(items,POINT{x,y},ToolbarState());InvalidateControls();if(released==pressed){for(const auto& item:items)if(item.action==pressed){ActivateToolbarAction(pressed,item.bounds);break;}}
    }
    // Dragging the timeline shows the frame under the cursor instead of the one
    // playback stopped on. A network stream re-resolves on every seek, so it
    // keeps the old behaviour of one seek on release.
    void ScrubToPreview(){
        if(!m_loaded||NetworkPlayback()||m_seeking||m_seekPending)return;
        const auto now=Clock::now();
        if(now-m_lastScrubSeek<std::chrono::milliseconds(200))return;
        m_lastScrubSeek=now;RequestSeek(m_seekPreview,false);
    }
    double SecondsFromX(int x)const{RECT r=TimelineRect();const LONG span=(r.right>r.left)?(r.right-r.left):LONG(1);double t=double(LONG(x)-r.left)/double(span);return std::clamp(t,0.0,1.0)*m_decoder.DurationSeconds();}
    void SetVolumeFromX(int x){const auto volumeRect=VolumeRect();if(!volumeRect)return;const RECT& r=*volumeRect;const LONG span=(r.right>r.left)?(r.right-r.left):LONG(1);const float volume=float(std::clamp(double(LONG(x)-r.left)/double(span),0.0,1.0));const bool changed=volume!=m_volume||m_muted;if(!changed)return;m_volume=volume;m_muted=false;Audio().SetVolume(m_volume);InvalidateToolbarAction(ToolbarAction::Mute);InvalidateVolumeControls();}
    void ToggleMute(){m_muted=!m_muted;Audio().SetVolume(m_muted?0.0f:m_volume);InvalidateToolbarAction(ToolbarAction::Mute);InvalidateVolumeControls();}
    // With a rendered pair loaded the toggle switches which member is presented.
    // Without one it is how an active session is started, and how it is stopped.
    void ToggleNeuralRendering(){
        if(!ToolbarActionEnabled(ToolbarAction::ToggleNeuralRendering)){
            LOG("Neural rendering toggle ignored: loaded="<<m_loaded<<" renderer="<<(m_renderer!=nullptr)<<" seeking="<<(m_seeking||m_seekPending)
                <<" cachedPair="<<(m_cachedPlayback&&m_havePresentedPair)<<" sessionAvailable="<<LiveSessionAvailable()
                <<" prerender="<<NeuralPreRenderEnabled()<<" stillImage="<<m_decoder.IsStillImage());
            return;
        }
        if(m_liveSession){StopLiveNeuralSession(true);return;}
        if(!m_cachedPlayback){StartLiveNeuralSession();return;}
        m_neuralRequested=!m_neuralRequested;const ComparisonView next=m_neuralRequested?ComparisonView::Neural:ComparisonView::Original;if(!m_synchronizedPlayback.SetView(next)){m_neuralRequested=!m_neuralRequested;return;}m_comparisonView=next;if(m_renderer)m_renderer->SetComparison(EffectiveComparison());const VideoFrame* presented=next==ComparisonView::Neural?&m_lastNeuralFrame:&m_lastOriginalFrame;m_guides.Reset();m_guideReset=true;m_dlssReset=true;RenderVideoFrame(*presented,true);m_guideReset=false;m_dlssReset=false;if(m_haveNext){if(const auto* pair=m_synchronizedPlayback.CurrentPair())m_next=next==ComparisonView::Neural?pair->neural:pair->original;}UpdateCachedStatus();InvalidateControls();
    }
    void Rehook(){if(!m_renderer)return;const std::wstring message=T(L"rehook.confirm"),title=T(L"rehook.title");const int answer=MessageBoxW(m_hwnd,message.c_str(),title.c_str(),MB_YESNOCANCEL|MB_ICONWARNING|MB_DEFBUTTON2);ExecuteGuardedRehook(answer,[&]{m_renderer->RequestDLSSRecreate();m_dlssReset=true;});}
    void SetYouTubeSourceQuality(YouTubeSourceQuality quality){if(quality==m_youtubeSourceQuality)return;if(m_loaded&&m_sourceKind==MediaSourceKind::YouTube&&!m_youtubePageUrl.empty()){StartYouTubeResolution(m_youtubePageUrl,m_displayTitle,quality,Position(),m_playing,NetworkCommitKind::QualityReload);return;}m_youtubeSourceQuality=quality;UpdateYouTubeQualitySelection(GetMenu(m_hwnd),quality);DrawMenuBar(m_hwnd);}
    void SetDebug(D3D12Renderer::DebugView v){if(m_renderer){m_renderer->SetDebugView(v);if(!m_playing)m_renderer->PresentCurrent();InvalidateControls();}}
    // A drag leaves the audio helper stopped; the frame under the cursor is
    // already on screen, so only sound and the play state have to come back.
    void EndScrub(){
        if(!m_loaded)return;
        LOG("Scrub ended: pending="<<(m_seekPending||m_seeking)<<" resume="<<m_dragWasPlaying<<" at="<<m_currentSec);
        // A scrub seek still queued will restart audio and playback itself; it
        // only needs to know the play state the drag started from.
        if(m_seekPending||m_seeking){m_seekResumePlaying=m_dragWasPlaying;return;}
        if(Audio().Start(m_path,m_currentSec)){Audio().SetVolume(m_muted?0.0f:m_volume);Audio().Pause(!m_dragWasPlaying);}
        if(m_dragWasPlaying){m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=true;if(m_cachedPlayback)m_synchronizedPlayback.SetPaused(false);}
    }
    void ToggleDebug(D3D12Renderer::DebugView v){if(!m_renderer)return;m_renderer->SetDebugView(m_renderer->GetDebugView()==v?D3D12Renderer::DebugView::Final:v);if(!m_playing)m_renderer->PresentCurrent();InvalidateControls();}
    void StopFullscreenTimer(){
        if(m_fullscreenTimer){KillTimer(m_hwnd,m_fullscreenTimer);m_fullscreenTimer=0;}
    }
    void RestoreFullscreenMenu(){
        if(m_fullscreenMenu&&GetMenu(m_hwnd)!=m_fullscreenMenu){
            SetMenu(m_hwnd,m_fullscreenMenu);
            // Source and feature availability may have changed while detached.
            UpdateYouTubeQualitySelection(m_fullscreenMenu,m_youtubeSourceQuality);
            SyncSourceActionAvailability();
            DrawMenuBar(m_hwnd);
        }
    }
    void RevealFullscreenControls(){
        if(!m_fullscreen)return;
        m_fullscreenLastInput=Clock::now();
        if(m_fullscreenControlsHidden){
            m_fullscreenControlsHidden=false;
            RestoreFullscreenMenu();Layout();InvalidateRect(m_hwnd,nullptr,FALSE);
        }
        if(!m_fullscreenTimer)m_fullscreenTimer=SetTimer(m_hwnd,kFullscreenTimerId,250,nullptr);
    }
    void FullscreenPointerMoved(HWND source,LPARAM position){
        if(!m_fullscreen)return;
        POINT screen{GET_X_LPARAM(position),GET_Y_LPARAM(position)};
        if(!ClientToScreen(source,&screen))return;
        // Resizing child windows can synthesize WM_MOUSEMOVE at a stationary
        // pointer. Only physical movement should reveal or restart the timer.
        if(m_fullscreenPointerKnown&&screen.x==m_fullscreenPointer.x&&screen.y==m_fullscreenPointer.y)return;
        m_fullscreenPointer=screen;m_fullscreenPointerKnown=true;
        if(m_fullscreenKeyboardFocus){m_fullscreenKeyboardFocus=false;m_focusedToolbarAction=ToolbarAction::None;}
        RevealFullscreenControls();
    }
    void AutoHideFullscreenControls(){
        if(!m_fullscreen||m_fullscreenControlsHidden)return;
        const HWND capture=GetCapture();
        const bool interacting=m_dragSeek||m_dragVolume||m_pressedToolbarAction!=ToolbarAction::None||
            (capture&&(capture==m_hwnd||IsChild(m_hwnd,capture)))||m_fullscreenMenuLoop||
            m_fullscreenKeyboardFocus||!IsWindowEnabled(m_hwnd)||
            (m_adjustWnd&&IsWindowVisible(m_adjustWnd))||(m_neuralWnd&&IsWindowVisible(m_neuralWnd));
        if(interacting){m_fullscreenLastInput=Clock::now();return;}
        if(Clock::now()-m_fullscreenLastInput<kFullscreenIdleDelay)return;
        m_fullscreenControlsHidden=true;m_focusedToolbarAction=ToolbarAction::None;
        m_hoverAction=ToolbarAction::None;m_pressedToolbarAction=ToolbarAction::None;
        StopFullscreenTimer();SetMenu(m_hwnd,nullptr);DrawMenuBar(m_hwnd);
        Layout();InvalidateRect(m_hwnd,nullptr,FALSE);
    }
    void ToggleFullscreen(){
        if(!m_fullscreen){
            m_savedStyle=GetWindowLongW(m_hwnd,GWL_STYLE);GetWindowRect(m_hwnd,&m_savedRect);
            m_fullscreenMenu=GetMenu(m_hwnd);m_fullscreen=true;m_fullscreenControlsHidden=true;
            m_fullscreenKeyboardFocus=false;m_focusedToolbarAction=ToolbarAction::None;
            m_hoverAction=ToolbarAction::None;m_pressedToolbarAction=ToolbarAction::None;
            m_dragSeek=false;m_dragVolume=false;if(GetCapture()==m_hwnd)ReleaseCapture();
            m_fullscreenPointerKnown=GetCursorPos(&m_fullscreenPointer)!=FALSE;
            SetMenu(m_hwnd,nullptr);
            MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(m_hwnd,MONITOR_DEFAULTTONEAREST),&mi);
            SetWindowLongW(m_hwnd,GWL_STYLE,m_savedStyle&~(WS_CAPTION|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_SYSMENU));
            SetWindowPos(m_hwnd,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_FRAMECHANGED|SWP_NOACTIVATE);
        }else{
            m_fullscreen=false;m_fullscreenControlsHidden=false;m_fullscreenKeyboardFocus=false;
            StopFullscreenTimer();RestoreFullscreenMenu();m_fullscreenMenu=nullptr;
            SetWindowLongW(m_hwnd,GWL_STYLE,m_savedStyle);
            SetWindowPos(m_hwnd,nullptr,m_savedRect.left,m_savedRect.top,m_savedRect.right-m_savedRect.left,m_savedRect.bottom-m_savedRect.top,SWP_NOZORDER|SWP_FRAMECHANGED|SWP_NOACTIVATE);
        }
        Layout();InvalidateRect(m_hwnd,nullptr,FALSE);
    }

    LRESULT WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        switch(m){
        case WM_ERASEBKGND:return 1;
        case WM_YOUTUBE_RESOLVED:CompleteYouTubeResolution(static_cast<uint64_t>(w));return 0;
        case WM_NEURAL_PROGRESS:CompleteNeuralProgress(static_cast<uint64_t>(w));return 0;
        case WM_NEURAL_COMPLETE:CompleteNeuralJob(static_cast<uint64_t>(w));return 0;
        case WM_EXPORT_COMPLETE:CompleteExport(static_cast<uint64_t>(w));return 0;
        case WM_TIMER:if(w==kActivityTimerId){AnimateActivity();return 0;}if(w==kFullscreenTimerId){AutoHideFullscreenControls();return 0;}if(w==kPreviewTimerId){StartPausedSettingsPreview();return 0;}break;
        case WM_ENTERMENULOOP:m_fullscreenMenuLoop=true;RevealFullscreenControls();break;
        case WM_EXITMENULOOP:m_fullscreenMenuLoop=false;m_fullscreenLastInput=Clock::now();break;
        case WM_SYSKEYDOWN:if(w==VK_MENU)RevealFullscreenControls();break;
        case WM_SYSCOMMAND:if((w&0xfff0)==SC_KEYMENU)RevealFullscreenControls();break;
        case WM_NCDESTROY:
            StopFullscreenTimer();
            if(m_fullscreenMenu){if(IsMenu(m_fullscreenMenu)&&GetMenu(h)!=m_fullscreenMenu)DestroyMenu(m_fullscreenMenu);m_fullscreenMenu=nullptr;}
            break;
        case WM_SETTINGCHANGE:ReadAnimationPreference();InvalidateRect(h,nullptr,FALSE);break;
        case WM_SHOWWINDOW:SyncActivityFeedback();break;
        case WM_DESTROY:CancelExport();if(m_activityTimer){KillTimer(h,m_activityTimer);m_activityTimer=0;}CancelNeuralJob(false);DrainNeuralMessages();CancelYouTubeResolution(false);DrainYouTubeCompletions();m_running=false;PostQuitMessage(0);return 0;
        case WM_CLOSE:CancelExport();CancelNeuralJob(false);CancelYouTubeResolution(false);DestroyWindow(h);return 0;
        case WM_GETMINMAXINFO:{
            auto* info=reinterpret_cast<MINMAXINFO*>(l);
            if(info){const UINT dpi=ActiveWindowDpi(h);const POINT minimum=MinimumPlayerWindowTrackSize(h,dpi);info->ptMinTrackSize.x=std::max<LONG>(info->ptMinTrackSize.x,minimum.x);info->ptMinTrackSize.y=std::max<LONG>(info->ptMinTrackSize.y,minimum.y);}
            return 0;
        }
        case WM_DPICHANGED:{
            const UINT dpi=HIWORD(w);
            UpdateFontsForDpi(dpi);
            const auto* suggested=reinterpret_cast<const RECT*>(l);
            if(suggested){const RECT target=ClampWindowRectToMinimumTrackSize(*suggested,MinimumPlayerWindowTrackSize(h,dpi));SetWindowPos(h,nullptr,target.left,target.top,target.right-target.left,target.bottom-target.top,SWP_NOZORDER|SWP_NOACTIVATE);}
            Layout();InvalidateRect(h,nullptr,FALSE);return 0;
        }
        case WM_SIZE:Layout();SyncActivityFeedback();return 0;
        case WM_PAINT:Paint();return 0;
        case WM_NCMOUSEMOVE:{
            POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
            if(ScreenToClient(h,&point))FullscreenPointerMoved(h,MAKELPARAM(point.x,point.y));
            break;
        }
        case WM_MOUSEMOVE:{FullscreenPointerMoved(h,l);m_mouseX=GET_X_LPARAM(l);m_mouseY=GET_Y_LPARAM(l);if(!m_trackingMouse){TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,h,0};m_trackingMouse=TrackMouseEvent(&tracking)!=FALSE;}if(m_dragSeek&&GetCapture()==h){const double preview=SecondsFromX(m_mouseX);if(preview!=m_seekPreview){m_seekPreview=preview;InvalidatePlaybackProgress();ScrubToPreview();}}if(m_dragVolume&&GetCapture()==h)SetVolumeFromX(m_mouseX);SetHoverAction(ToolbarActionAt(m_mouseX,m_mouseY));return 0;}
        case WM_MOUSELEAVE:m_trackingMouse=false;m_mouseX=-999;m_mouseY=-999;SetHoverAction(ToolbarAction::None);return 0;
        case WM_LBUTTONDOWN:MouseDown(GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
        case WM_LBUTTONUP:MouseUp(GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
        case WM_CAPTURECHANGED:if(m_dragSeek){m_dragSeek=false;EndScrub();InvalidateControls();}if(m_dragVolume)m_dragVolume=false;if(m_pressedToolbarAction!=ToolbarAction::None){m_pressedToolbarAction=ToolbarAction::None;InvalidateControls();}return 0;
        case WM_SETFOCUS:InvalidateControls();return 0;
        case WM_KILLFOCUS:InvalidateControls();return 0;
        case WM_DROPFILES:{HDROP d=reinterpret_cast<HDROP>(w);wchar_t p[32768]{};UINT count=DragQueryFileW(d,0xFFFFFFFF,nullptr,0);if(count>0&&DragQueryFileW(d,0,p,static_cast<UINT>(std::size(p))))Load(p);DragFinish(d);return 0;}
        case WM_MOUSEWHEEL:{if(m_loaded){const float step=(GET_WHEEL_DELTA_WPARAM(w)>0)?0.05f:-0.05f;const float volume=std::clamp(m_volume+step,0.0f,1.0f);const bool changed=m_muted||volume!=m_volume;if(changed){m_muted=false;m_volume=volume;Audio().SetVolume(m_volume);InvalidateToolbarAction(ToolbarAction::Mute);InvalidateVolumeControls();}}return 0;}
        case WM_COMMAND:HandleCommand(LOWORD(w));return 0;
        case WM_HOTKEY:HandleHotkey(int(w));return 0;
        case WM_KEYDOWN:
            if(w==VK_F10)RevealFullscreenControls();
            if(w==VK_TAB){FocusNextToolbarAction((GetKeyState(VK_SHIFT)&0x8000)!=0);return 0;}if(w==VK_RETURN&&m_focusedToolbarAction!=ToolbarAction::None){ActivateFocusedToolbarAction();return 0;}if(app_menu::RoutesToOpenYouTube(app_menu::PlayerCommandRoute::KeyDown,static_cast<UINT>(w),(GetKeyState(VK_CONTROL)&0x8000)!=0)){ActivateYouTube();return 0;}if((GetKeyState(VK_CONTROL)&0x8000)&&w=='O'){OpenFromDialog();return 0;}if((GetKeyState(VK_CONTROL)&0x8000)&&w=='E'){ShowAdjustments();return 0;}if(const auto command=app_menu::CommandForPlayerKey(static_cast<UINT>(w),(GetKeyState(VK_CONTROL)&0x8000)!=0,(GetKeyState(VK_SHIFT)&0x8000)!=0)){HandleCommand(*command);return 0;}if(w==VK_SPACE){TogglePause();return 0;}if(w==VK_OEM_PERIOD){StepCachedFrame();return 0;}if(w==VK_LEFT){RequestSeek(Position()-10);return 0;}if(w==VK_RIGHT){RequestSeek(Position()+10);return 0;}if(w==VK_F11){ToggleFullscreen();return 0;}if(app_menu::RoutesToRehook(app_menu::PlayerCommandRoute::KeyDown,static_cast<UINT>(w))){Rehook();return 0;}if(w=='S'){StopPlayback();return 0;}if(w=='A'){m_fill=!m_fill;Layout();return 0;}if(w=='D'){ToggleNeuralRendering();return 0;}if(w=='M'){ToggleMute();return 0;}if(w=='1'){SetDebug(D3D12Renderer::DebugView::Final);return 0;}if(w=='2'){SetDebug(D3D12Renderer::DebugView::Input);return 0;}if(w=='3'){SetDebug(D3D12Renderer::DebugView::MotionVectors);return 0;}if(w=='4'){SetDebug(D3D12Renderer::DebugView::Depth);return 0;}if(w==VK_ESCAPE&&m_liveSession){StopLiveNeuralSession(true);return 0;}if(w==VK_ESCAPE&&NeuralJobActive()){CancelNeuralJob();return 0;}if(w==VK_ESCAPE&&m_youtubeLifecycle.IsResolving()){CancelYouTubeResolution();return 0;}if(w==VK_ESCAPE&&m_fullscreen){ToggleFullscreen();return 0;}break;
        }
        return DefWindowProcW(h,m,w,l);
    }

    void HandleCommand(UINT id){
        if(id>=IDM_RECENT_VIDEO_FIRST&&id<IDM_RECENT_VIDEO_FIRST+5){OpenRecent(id-IDM_RECENT_VIDEO_FIRST);return;}
        if(app_menu::RoutesToRehook(app_menu::PlayerCommandRoute::NativeMenu,id)){Rehook();return;}
        if(app_menu::RoutesToOpenYouTube(app_menu::PlayerCommandRoute::NativeMenu,id,false)){ActivateYouTube();return;}
        if(const ExampleVideo* example=app_menu::ExampleVideoForCommand(id)){ActivateExampleVideo(*example);return;}
        if(const auto quality=app_menu::YouTubeQualityForCommand(id)){SetYouTubeSourceQuality(*quality);return;}
        switch(id){
        case IDM_OPEN:OpenFromDialog();break;case IDM_EXIT:DestroyWindow(m_hwnd);break;case IDM_PLAY:TogglePause();break;case IDM_STOP:StopPlayback();break;case IDM_BACK10:RequestSeek(Position()-10);break;case IDM_FWD10:RequestSeek(Position()+10);break;case IDM_MUTE:ToggleMute();break;case IDM_NEURAL_RENDERING:ToggleNeuralRendering();break;
        case IDM_DLSS_UPSCALING:ToggleUpscaling();break;
        case IDM_UPSCALE_1440:SetUpscaleTarget(1440);break;
        case IDM_UPSCALE_2160:SetUpscaleTarget(2160);break;
        case IDM_FRAME_GENERATION:break;
        case IDM_EXPORT_CACHED_VIDEO:ExportCachedVideo();break;
        case IDM_CANCEL_EXPORT:CancelExport();break;
        case IDM_VIEW_FINAL:SetDebug(D3D12Renderer::DebugView::Final);break;case IDM_VIEW_INPUT:SetDebug(D3D12Renderer::DebugView::Input);break;case IDM_VIEW_MV:SetDebug(D3D12Renderer::DebugView::MotionVectors);break;case IDM_VIEW_DEPTH:SetDebug(D3D12Renderer::DebugView::Depth);break;case IDM_VIDEO_ADJUSTMENTS:ShowAdjustments();break;case IDM_ASPECT_FIT:m_fill=false;Layout();break;case IDM_ASPECT_FILL:m_fill=true;Layout();break;case IDM_FULLSCREEN:ToggleFullscreen();break;case IDM_ADVANCED_SAFE_MODE:RestartInSafeMode();break;case IDM_CLEAR_NEURAL_CACHE:ClearNeuralCache();break;
        case IDM_MARK_IN:SetMarker(true,Position100ns());break;case IDM_MARK_OUT:SetMarker(false,Position100ns());break;case IDM_CLEAR_MARKS:ClearMarkers();break;case IDM_GOTO_TIMECODE:ShowTimecodeDialog();break;
        case IDM_PAUSE_NEURAL_RENDER:if(NeuralJobActive())SetNeuralJobPaused(!NeuralJobPaused());break;
        case IDM_PREVIEW_FRAME:PreviewCurrentFrame();break;case IDM_PREVIEW_CLIP:PreviewClip();break;case IDM_RENDER_RANGE:RenderMarkedRange();break;case IDM_RENDER_WHOLE:RenderWholeSource();break;
        case IDM_NEURAL_SETTINGS:ShowNeuralSettings();break;case IDM_OPEN_RENDER_RECEIPT:OpenRenderReceipt();break;
        case IDM_COMPARE_NEURAL:SetComparisonMode(ComparisonMode::Neural);break;case IDM_COMPARE_BLEND:SetComparisonMode(ComparisonMode::Blend);break;case IDM_COMPARE_SPLIT:SetComparisonMode(ComparisonMode::SplitVertical);break;case IDM_COMPARE_WIPE:SetComparisonMode(ComparisonMode::Wipe);break;
        case IDM_COMPARE_BLEND_LESS:AdjustBlendAmount(-0.1f);break;case IDM_COMPARE_BLEND_MORE:AdjustBlendAmount(0.1f);break;case IDM_COMPARE_ZOOM:ToggleZoom();break;
        }
    }

    std::unique_ptr<RecentMediaHistory> m_recent;
    std::filesystem::path m_cacheRoot;
    std::vector<RecentMediaEntry> m_pendingCacheEvictions;
    std::filesystem::path m_neuralPath;
    CompletionRegistry<ExportCompletion> m_exportCompletions;
    std::jthread m_exportWorker;
    AppOptions m_opt;Localizer m_loc;UiResources m_uiResources;D3D12Renderer::ColorSettings m_colorSettings{};NVSDK_NGX_PerfQuality_Value m_activeQuality=DefaultNeuralCarrierQuality();HWND m_hwnd=nullptr,m_viewport=nullptr,m_renderWnd=nullptr,m_adjustWnd=nullptr;HFONT m_font=nullptr,m_fontSmall=nullptr,m_iconFont=nullptr;
    bool m_running=true,m_loaded=false,m_playing=false,m_haveNext=false,m_waitingForNetworkFrame=false,m_fill=false,m_fullscreen=false,m_dragSeek=false,m_dragVolume=false,m_muted=false,m_seekPending=false,m_seekResumePlaying=false,m_seeking=false,m_trackingMouse=false,m_iconFallbackLogged=false,m_neuralRequested=true;
    // Timeline scrubbing: what playback was doing before the drag, and when the
    // last preview seek went out so a drag cannot queue one per mouse move.
    bool m_dragWasPlaying=false;
    Clock::time_point m_lastScrubSeek{};
    ToolbarAction m_pressedToolbarAction=ToolbarAction::None,m_focusedToolbarAction=ToolbarAction::None,m_hoverAction=ToolbarAction::None;
    HMENU m_fullscreenMenu=nullptr;
    UINT_PTR m_fullscreenTimer=0;
    bool m_fullscreenControlsHidden=false,m_fullscreenMenuLoop=false,m_fullscreenKeyboardFocus=false,m_fullscreenPointerKnown=false;
    POINT m_fullscreenPointer{};
    Clock::time_point m_fullscreenLastInput=Clock::now();
    LONG m_savedStyle=0;RECT m_savedRect{};double m_dar=16.0/9.0,m_currentSec=0,m_playStartSec=0,m_seekPreview=0,m_pendingSeekSec=0;float m_volume=1.0f,m_lastGlobalX=0,m_lastGlobalY=0;int m_mouseX=-999,m_mouseY=-999;
    Clock::time_point m_playStart=Clock::now(),m_fpsWindowStart=Clock::now(),m_lastStaticPresent=Clock::now();double m_submitFps=0.0;uint64_t m_fpsWindowFrames=0;std::wstring m_path,m_youtubeAudioUrl,m_youtubePageUrl,m_displayTitle,m_cachedStatus,m_cachedWindowTitle,m_pendingYouTubeTitle,m_pendingNeuralTitle;YouTubeSourceQuality m_youtubeSourceQuality=YouTubeSourceQuality::P1080;MediaSourceKind m_sourceKind=MediaSourceKind::LocalFile;VideoDecoder m_decoder;VideoFrame m_next;D3D12RendererOwner m_renderer;TemporalGuideGenerator m_guides;AudioPlayer m_audio;std::unique_ptr<AudioPlayer>m_networkAudio;NetworkReadState m_networkReadState;YouTubeResolutionLifecycle m_youtubeLifecycle;std::unique_ptr<YouTubeResolver>m_youtubeResolver;CompletionRegistry<YouTubeCompletion>m_youtubeCompletions;std::jthread m_youtubeWorker;
    NeuralPlaybackLifecycle m_neuralLifecycle;NeuralRenderProgress m_neuralProgress;CompletionRegistry<NeuralProgressMessage>m_neuralProgressMessages;CompletionRegistry<NeuralJobCompletion>m_neuralCompletions;std::jthread m_neuralWorker;SynchronizedPlayback m_synchronizedPlayback;ComparisonView m_comparisonView=ComparisonView::Original;bool m_cachedPlayback=false,m_havePresentedPair=false;uint64_t m_cachedPresentedFrames=0;VideoFrame m_lastOriginalFrame,m_lastNeuralFrame;RECT m_neuralCancelBounds{};uint32_t m_neuralSourceWidth=0,m_neuralSourceHeight=0;
    // The loaded original is the acquired local copy of a network source: its
    // identity stays YouTube, but decode, seek and audio are local-file work.
    bool m_cachedSourceFile=false;
    // Background acquisition of a streamed source, started when a range is
    // marked so a render does not wait for the whole download.
    std::shared_ptr<SourcePrefetchState> m_prefetchState;
    std::jthread m_prefetchWorker;
    std::wstring m_prefetchPageUrl,m_prefetchTitle;
    YouTubeSourceQuality m_prefetchQuality{YouTubeSourceQuality::Auto};
    bool m_guideReset=true,m_dlssReset=true;int64_t m_lastRenderedTs=-1;uint64_t m_droppedFrames=0;uint32_t m_historyGeneration=0;
    bool m_upscalingRequested=false;
    UINT_PTR m_activityTimer=0;
    bool m_activityBusy=false,m_activityMotionEnabled=true;
    Clock::time_point m_activityStarted=Clock::now();
    uint32_t m_upscaleTargetHeight=1440;
    std::wstring m_upscalingError;
    VideoFrame m_lastPlaybackFrame;
    // Read when a job starts; changing them only affects the next render.
    GuideControls m_renderGuides;
    NeuralSettings m_neuralSettings;
    // Frame-accurate in/out markers on the loaded source's timeline.
    RangeMarkers m_markers;
    // Presentation comparison of a synchronized pair (persisted in [Comparison]).
    static constexpr float kZoomScale=2.0f;
    ComparisonSettings m_comparison;
    HWND m_neuralWnd=nullptr;
    // Tooltip host for whichever settings dialog is open, and the strings it
    // points at: TTM_ADDTOOL keeps the pointer rather than copying the text.
    HWND m_tipWnd=nullptr;
    std::vector<std::unique_ptr<std::wstring>> m_tipText;
    POINT m_renderMouse{};
    bool m_renderMouseKnown=false,m_dragSplit=false;
    // Settings the playing cache entry was rendered with (its receipt has the full record).
    NeuralSettings m_cachedSettings;
    GuideControls m_cachedGuides;
    // Range of the playing cache entry (Whole() for full renders) and its
    // receipt.json beside the payload (empty when the entry has none).
    NeuralRenderRange m_cachedRange;
    std::filesystem::path m_cachedReceiptPath;
    // Manual-reset event shared with the render helper: signalled means pause.
    HANDLE m_neuralPauseEvent=nullptr;
    // Active neural rendering: the render job runs while playback continues and
    // the player consumes finalized segments as they land. m_liveDirectory holds
    // those segments; the promoted cache entry is a separate concatenated copy.
    // Turning the toggle off keeps them in the retained slot, so turning it back
    // on resumes at the head instead of rendering the same frames again. Each
    // job writes into its own subdirectory of m_liveDirectory.
    std::shared_ptr<NeuralSegmentIndex> m_liveSegments;
    bool m_liveSession=false,m_liveAttached=false,m_liveBuffering=false,m_liveResumePlaying=false;
    NeuralRenderRange m_liveRange{};
    std::shared_ptr<NeuralSegmentIndex> m_retainedSegments;
    std::filesystem::path m_retainedDirectory;
    NeuralRenderRange m_retainedRange{};
    std::string m_retainedKey;
    uint32_t m_liveJobSerial=0;
    // Debounced re-render of the paused frame after a neural settings change.
    bool m_previewJob=false,m_previewShown=false,m_previewQueued=false;
    NeuralRenderRange m_previewRange{};
    UINT_PTR m_previewTimer=0;
    std::filesystem::path m_liveDirectory;
    int64_t m_livePaintedHead=0;ULONGLONG m_liveStartTick=0;
    HWND m_bufferWnd=nullptr;
    RECT m_bufferAnchor{};
};

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int)
{
    EnablePerMonitorDpiAwareness();
    AppOptions options=ParseArgs();
    if(!options.argumentsOk){LOG("Invalid command line: "<<WideToUtf8(options.argumentError));MessageBoxW(nullptr,options.argumentError.c_str(),L"DLSS Video Player",MB_OK|MB_ICONERROR);return 1;}
    const StartupResult startup=RunNeuralAddonBootstrap(options);
    if(startup==StartupResult::ExitSuccess)return 0;
    if(startup==StartupResult::ExitFailure)return 1;
    const NeuralRenderDefaults renderDefaults=ResolveNeuralRenderDefaults(
        options.neuralAddonConfigured,options.outputExplicit,options.maxW,options.maxH);
    options.maxW=renderDefaults.width;options.maxH=renderDefaults.height;
    if(options.neuralAddonConfigured&&!options.outputExplicit)
        LOG("Neural pre-render defaults active: YouTube Auto prefers exact 1080p; local files retain native source resolution and spatial upscaling stays off.");
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE)))return 1;
    if(FAILED(MFStartup(MF_VERSION,MFSTARTUP_FULL))){CoUninitialize();return 1;}

    return RunPlayerRuntime(
        [&]() -> int {
            PlayerApp app(std::move(options));
            if(!app.Create(hi))return 1;

            MSG msg{};
            bool quit=false;
            while(app.Running()&&!quit){
                while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
                    if(msg.message==WM_QUIT){quit=true;break;}
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if(quit)break;
                app.Tick();
                if(app.NeedsRealtimeTick())Sleep(app.TickSleepMs());
                else WaitMessage();
            }
            return 0;
        },
        [] { MFShutdown(); },
        [] { CoUninitialize(); });
}
