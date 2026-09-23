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
#include <map>
#include <cwctype>
#include <cstdlib>
#include <optional>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <system_error>
#include "VideoDecoder.h"
#include "PlatformPaths.h"
#include "D3D12Renderer.h"
#include "TemporalGuides.h"
#include "AudioPlayer.h"
#include "Localization.h"
#include "AppMenu.h"
#include "UiLayout.h"
#include "UiResources.h"
#include "HexText.h"
#include "Utf8Text.h"
#include "Log.h"
#include "HardErrorSuppression.h"
#include "ReShadeConfig.h"
#include "CacheEvictionPolicy.h"
#include "RendererRecoveryPolicy.h"
#include "RuntimePolicy.h"
#include "RuntimeLifetime.h"
#include "YouTubeResolver.h"
#include "ExampleVideos.h"
#include "CompletionRegistry.h"
#include "NetworkMediaTransaction.h"
#include "PlaybackCadence.h"
#include "PlaybackTiming.h"
#include "LiveSessionPolicy.h"
#include "CachedRenderVerdict.h"
#include "CrashDump.h"
#include "Nv12Convert.h"
#include "FrameRatePolicy.h"
#include "DroppedFilesPolicy.h"
#include "StatusNotePolicy.h"
#include "FrameGenerationPass.h"
#include "NeuralCache.h"
#include "SourceDigestMemo.h"

// The loaded source's SHA-256, computed once per file rather than once per
// job. Shared with the neural worker by value: that thread captures nothing
// owned by the window, so the memo cannot live behind `this`.
struct SharedSourceDigest {
    std::mutex mutex;
    source_digest::Entry entry;
};

// The size and write time are re-read on every call, so a file that changes
// under the player is re-hashed rather than served stale. That matters more
// than it would for an ordinary memo: the digest is a render-cache key term,
// and a stale one hands back the render of a different file.
inline std::optional<std::string> MemoisedSourceDigest(SharedSourceDigest& memo,
                                                       const std::filesystem::path& path,
                                                       std::stop_token stop)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return Sha256File(path, stop);
    const auto written = std::filesystem::last_write_time(path, error);
    if (error) return Sha256File(path, stop);
    const int64_t writeTime = written.time_since_epoch().count();
    std::scoped_lock lock(memo.mutex);
    return source_digest::Lookup(memo.entry, path.wstring(), size, writeTime,
                                 [&] { return Sha256File(path, stop); });
}
#include "RecentMedia.h"
#include "MediaPipeline.h"
#include "OfflineNeuralRenderer.h"
#include "NeuralWorker.h"
#include "NeuralPreflight.h"
#include "NeuralReceipt.h"
#include "NeuralSettings.h"
#include "NeuralPresets.h"
#include "PrecisionSleeper.h"
#include "RangeSelection.h"
#include "RuntimeLock.h"
#include "RuntimeModulePolicy.h"
#include "UpscalingPolicy.h"
#include "ExportPipeline.h"
#include "UpdateCheck.h"
#include "SynchronizedPlayback.h"
#include "StatusChipPolicy.h"
#include "TimelinePolicy.h"
#include "ShortcutSheetPolicy.h"
#include "resources.h"

using Clock = std::chrono::steady_clock;

// NV12 (BT.709, limited range) back to BGRA, for the ONE consumer that cannot
// take the GPU conversion: the comparison reference is a B8G8R8A8_UNORM texture
// uploaded from CPU bytes. Playback only decodes to NV12 when the source
// declared exactly this description - VideoDecoder's playbackConvertible gate -
// so this is the whole set of coefficients that path can ever need, rather than
// the first of four. The conversion itself lives in Nv12Convert.h, where it can
// be held bit-identical to the scalar original by test.
inline void Nv12ToBgraBt709Limited(const uint8_t* nv12, uint32_t width, uint32_t height,
                                   std::vector<uint8_t>& bgra)
{
    nv12::ToBgraBt709Limited(nv12, width, height, bgra);
}
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

// The mode of the monitor the window sits on. Both fields are zero when the
// monitor or its mode could not be read, which every consumer treats as "no
// answer" rather than as a value.
//
// MONITORINFO's rcMonitor is in virtual-desktop space, which a scaled display
// reports in logical pixels, so the adapter's own mode is what answers both
// questions: dmPelsHeight is what the panel scans out, and dmDisplayFrequency
// is how often. The height is the only one an upscale target can be judged
// against, and the refresh is the only one a generated frame rate can.
struct MonitorMode {
    uint32_t height{};
    double refreshHz{};
};

static MonitorMode CurrentMonitorMode(HWND window)
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                         reinterpret_cast<MONITORINFO*>(&info)))
        return {};
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return {};
    // 0 and 1 are the documented "hardware default" placeholders for
    // dmDisplayFrequency, not rates; neither is a cadence to plan against.
    const double refresh = mode.dmDisplayFrequency > 1 ? double(mode.dmDisplayFrequency) : 0.0;
    return {mode.dmPelsHeight, refresh};
}

// Every refresh this monitor can be set to at the resolution it is in now. A
// mode that changes the pixel grid is a different display rather than a cadence
// fix, so width, height and colour depth are held and only the rate varies.
// Interlaced modes are dropped outright: half a field is not a scan-out a
// generated frame can be planned against.
//
// This is an EnumDisplaySettingsW loop over every mode the adapter advertises,
// which is why no status refresh or paint may reach it - see MonitorModeCached
// for what that cost did to the toolbar - and it runs on the refusal path only.
static std::vector<double> AvailableRefreshRates(HWND window)
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                         reinterpret_cast<MONITORINFO*>(&info)))
        return {};
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &current)) return {};
    std::vector<double> rates;
    for (DWORD index = 0;; ++index) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(info.szDevice, index, &mode)) break;
        if (mode.dmPelsWidth != current.dmPelsWidth || mode.dmPelsHeight != current.dmPelsHeight)
            continue;
        if (mode.dmBitsPerPel != current.dmBitsPerPel) continue;
        if ((mode.dmDisplayFlags & DM_INTERLACED) != 0) continue;
        if (mode.dmDisplayFrequency <= 1) continue;
        const double hz = double(mode.dmDisplayFrequency);
        if (std::find(rates.begin(), rates.end(), hz) == rates.end()) rates.push_back(hz);
    }
    return rates;
}

// Dynamic, not persisted: flags 0 changes the mode for this session and leaves
// the registry alone, so the display control panel - or a reboot - undoes it.
// CDS_TEST runs first because a rate the monitor advertises can still be
// refused at this colour depth, and by the time a bad ChangeDisplaySettings
// call reports that, it has already blanked the panel to find out.
static bool SetMonitorRefresh(HWND window, double refreshHz)
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                         reinterpret_cast<MONITORINFO*>(&info)))
        return false;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return false;
    mode.dmDisplayFrequency = static_cast<DWORD>(std::lround(refreshHz));
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    if (ChangeDisplaySettingsExW(info.szDevice, &mode, nullptr, CDS_TEST, nullptr) !=
        DISP_CHANGE_SUCCESSFUL)
        return false;
    return ChangeDisplaySettingsExW(info.szDevice, &mode, nullptr, 0, nullptr) ==
           DISP_CHANGE_SUCCESSFUL;
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
static constexpr int IDC_ADJ_NEURAL_STRENGTH = 7107;
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
static constexpr int IDC_NS_PASSES = 7309;
static constexpr int IDC_NS_CHAINED = 7310;
static constexpr int IDC_NS_GUIDE_MV = 7311;
static constexpr int IDC_NS_GUIDE_DEPTH = 7312;
// 7313-7315 were the encoder controls, moved to their own dialog below.
static constexpr int IDC_NS_RESET = 7320;
static constexpr int IDC_NS_APPLY = 7321;
static constexpr int IDC_NS_CLOSE = 7322;

static constexpr int IDC_ES_GPU_CONVERT = 7401;
static constexpr int IDC_ES_GPU_SOURCE = 7402;
static constexpr int IDC_ES_NVENC_PRESET = 7403;
static constexpr int IDC_ES_RESET = 7404;
static constexpr int IDC_ES_CLOSE = 7405;
static constexpr int IDC_EX_UPSCALE = 7501;
static constexpr int IDC_EX_NEURAL = 7502;
static constexpr int IDC_EX_FRAMEGEN = 7503;
static constexpr int IDC_EX_RESOLUTION = 7504;
static constexpr int IDC_EX_MULTIPLIER = 7505;
static constexpr int IDC_EX_SUMMARY = 7506;
static constexpr int IDC_EX_RUN = 7507;
static constexpr int IDC_EX_CLOSE = 7508;

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
static constexpr UINT WM_UPDATE_CHECKED = WM_APP + 45;
static constexpr UINT WM_FRAMEGEN_PROGRESS = WM_APP + 46;
static constexpr UINT WM_STAGE_EXPORT_PROGRESS = WM_APP + 48;
static constexpr UINT WM_FRAMEGEN_COMPLETE = WM_APP + 47;
static constexpr UINT WM_TIMELINE_MEDIA = WM_APP + 49;

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
    // The local source this job renders from, once it has one, with the cache key
    // it lives under and the video it belongs to.
    //
    // An acquired copy of a stream is in the cache long before the job that
    // acquired it finishes, and the recent history - the player's only other
    // route to it - is not written until then. Playback needs it earlier: while a
    // stream is loaded, every seek is a re-resolution of a signed URL. The key
    // comes along because it is what a later render reuses and what keeps the
    // copy from being evicted; the page URL because a second video opened in the
    // same session must not be handed the first one's file.
    std::filesystem::path sourcePath;
    std::string sourceKey;
    std::wstring pageUrl;
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

// One cold start, shared by the job thread that measures the stack and the UI
// thread that puts the first neural frame on screen. Marks are segments: each
// phase ends where the previous one ended, so the timeline stays contiguous
// without either thread knowing what the other measured.
class NeuralColdStartRecord {
public:
    NeuralColdStartRecord():origin_(Clock::now()),mark_(origin_){}
    // Ends the phase that began at the previous mark. A phase that happens
    // twice - the helper relaunched after repairing its configuration - sums
    // its parts, so the discarded attempt is launch cost rather than a gap.
    void Mark(NeuralColdStartPhase phase){const std::lock_guard guard(mutex_);MarkLocked(phase);}
    // For a job that ended without ever reaching the phase this would follow:
    // a cache hit or a refusal did the player's own work and nothing else, and
    // that work is still worth naming.
    void MarkIfAbsent(NeuralColdStartPhase phase){
        const std::lock_guard guard(mutex_);
        if(!timeline_.Phase(phase))MarkLocked(phase);
    }
    // The helper's five phases, merged from the job thread the moment the
    // helper reports them - which is while the UI thread may be reading this
    // record to put the first frame on screen, so every entry point here locks.
    void Merge(const NeuralColdStartTimeline& helper){const std::lock_guard guard(mutex_);timeline_.Merge(helper);}
    // The first playable neural output is in the player's hands: segment zero
    // for an active session, the finished job for a cached or whole-file render.
    void Ready(){const std::lock_guard guard(mutex_);if(!ready_)ready_=Clock::now();}
    // The first neural frame is on screen. Attach is measured from Ready
    // because both stamps are the player's own; the helper's last phase and
    // this one are separated only by the metadata pipe's poll interval.
    void Presented(){
        const std::lock_guard guard(mutex_);if(presented_)return;
        const auto now=Clock::now();presented_=now;
        if(ready_)timeline_.Record(NeuralColdStartPhase::Attach,
            std::chrono::duration_cast<std::chrono::microseconds>(now-*ready_));
        timeline_.RecordTotal(std::chrono::duration_cast<std::chrono::microseconds>(now-origin_));
    }
    NeuralColdStartTimeline Snapshot()const{const std::lock_guard guard(mutex_);return timeline_;}
    // One line per render: whichever end finishes first reports, the other is
    // silent.
    bool ClaimReport(){const std::lock_guard guard(mutex_);return !std::exchange(reported_,true);}
    // Which helper the render's phases belong to, because the same five dashes
    // mean three different things. A render key already in the cache is
    // answered without starting a helper at all; a reused resident helper did
    // not pay neuralInit or featureArm because it had already paid them; and a
    // measurement that went missing is none of the above. Without this, all
    // three read as a broken instrument.
    void NoteNoHelper(std::string_view reason){const std::lock_guard guard(mutex_);helper_="none("+std::string(reason)+")";}
    void NoteHelper(std::string_view plan){const std::lock_guard guard(mutex_);helper_=plan;}
    std::string HelperNote()const{const std::lock_guard guard(mutex_);return helper_;}

private:
    using Clock=std::chrono::steady_clock;
    void MarkLocked(NeuralColdStartPhase phase){
        const auto now=Clock::now();
        auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(now-mark_);
        if(const auto already=timeline_.Phase(phase))elapsed+=*already;
        timeline_.Record(phase,elapsed);
        mark_=now;
    }
    mutable std::mutex mutex_;
    NeuralColdStartTimeline timeline_;
    Clock::time_point origin_,mark_;
    std::optional<Clock::time_point> ready_,presented_;
    bool reported_=false;
    std::string helper_;
};

struct ExportCompletion {
    MaterializeResult result;
    std::filesystem::path output;
};

// One frame-generation conversion: what it was asked for, and what came back.
// The multiplier is carried beside the result because the completion reports it
// to the user and the result alone does not say what was requested when the
// runtime's cap clamped it.
struct FrameGenerationCompletion {
    FrameGenerationResult result;
    std::filesystem::path output;
    // The file that was converted. A conversion takes minutes and the player
    // stays usable, so by the time it lands the user may be watching something
    // else entirely; the completion only takes playback over when this is still
    // the file on screen.
    std::wstring source;
    bool neuralInput{};
    uint32_t multiplier{};
    std::wstring title;
};

struct FrameGenerationProgressMessage {
    FrameGenerationProgress progress{};
};

struct UpdateCheckCompletion {
    UpdateFetchResult fetch;
    // A check the user asked for reports its outcome either way; the startup
    // check stays silent unless it has something to show.
    bool userRequested{false};
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

static std::string WideToUtf8(std::wstring_view value) { return utf8_text::FromWide(value); }

static std::wstring Utf8ToWide(std::string_view value) { return utf8_text::ToWide(value); }

// Substitutes into a localized format string. swprintf_s calls the invalid
// parameter handler instead of truncating, and one substitution here is a
// filesystem path, so the buffer is sized from the format and its arguments
// rather than guessed.
static std::wstring FormatLocalizedText(const std::wstring& format,const std::wstring& first,const std::wstring& second={}) {
    std::vector<wchar_t> text(format.size()+first.size()+second.size()+1);
    swprintf_s(text.data(),text.size(),format.c_str(),first.c_str(),second.c_str());
    return text.data();
}

static std::wstring Win32Error(std::wstring_view operation) {
    return std::wstring(operation)+L" failed (Win32 error "+std::to_wstring(GetLastError())+L")";
}

static bool CurrentExecutablePath(std::filesystem::path& executable,std::wstring& error) {
    const auto path=platform_paths::ModulePath();
    if(!path) { error=Win32Error(L"Resolving the player executable"); return false; }
    executable=*path;
    if(!executable.is_absolute()) { error=L"The player executable path was not absolute"; return false; }
    return true;
}

static bool LaunchSameExecutable(const std::vector<std::wstring>& arguments,std::wstring& error) {
    std::filesystem::path executable;
    if(!CurrentExecutablePath(executable,error)) return false;
    std::wstring commandLine=BuildWindowsCommandLine(executable.native(),arguments);
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    const ScopedHardErrorSuppression noHardErrorDialog;
    if(!CreateProcessW(executable.c_str(),commandLine.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)) {
        error=Win32Error(L"Starting the player"); return false;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return true;
}

static StartupResult FailBootstrap(std::wstring_view technicalError) {
    LOG("Neural addon bootstrap failed: " << WideToUtf8(technicalError));
    MessageBoxW(nullptr,L"The experimental neural add-on could not be configured safely. The player will close.\n\nSee DLSSVideoPlayer.log for details.",L"DLSS 5 Video Player",MB_OK|MB_ICONERROR);
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
    // GPU, driver and generation on one line: the field verification matrix
    // needs all three, and the driver floor message below only appears when
    // the driver is too old to say anything else.
    LOG("Isolated neural helper available; player remains hook-free. GPU=" << WideToUtf8(options.detectedGpu.description)
        << " driver=" << WideToUtf8(options.detectedGpu.driverVersion)
        << " generation=" << GpuGenerationPathName(options.detectedGpu.generation));
    switch(ClassifyNeuralDriver(options.detectedGpu.driverVersion)){
    case NeuralDriverSupport::BelowFloor:
        // Feature 18 is serviced by the driver's NGX core, so a driver older
        // than the floor refuses the create before this player is involved.
        LOG("NVIDIA driver "<<WideToUtf8(options.detectedGpu.driverVersion)<<" is below the neural-rendering floor "
            <<WideToUtf8(FormatNvidiaDriverVersion(kNeuralDriverFloor))<<"; neural rendering will be refused until the driver is updated to "
            <<WideToUtf8(FormatNvidiaDriverVersion(kNeuralDriverRecommended))<<" or newer.");
        break;
    case NeuralDriverSupport::Unknown:
        LOG("NVIDIA driver version could not be read from DXGI; the neural driver floor cannot be checked before the runtime probe.");
        break;
    case NeuralDriverSupport::Supported:
        break;
    }
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
    // Why it failed, in the acquisition's own words. Written before `finished`
    // is released and read after the join, so the release/acquire pair is what
    // publishes it. It used to be dropped on the floor: the log said
    // "finished without a reusable copy" and nothing else, which is not enough
    // to tell a throttled download from an expired URL from a full disk.
    std::wstring detail;
    bool cancelled{};
};

// The localizer belongs to the thread that owns the window, so a render job and
// the background acquisition carry the cache-failure sentences they may need.
// The message names the cache root the user can act on; the exact directory,
// the cause and the filesystem error are on one line in the log.
struct NeuralCacheFailureText {
    std::wstring staging,root,unwritable,invalidKey,createFailed,alreadyExists,outsideRoot;
    std::wstring Describe(const NeuralCacheManager& cache)const{
        const auto& failure=cache.LastFailure();
        return FormatLocalizedText(staging,(cache.Valid()?cache.Root():failure.path).wstring(),
                                   CauseText(failure));
    }
    std::wstring DescribeRoot(const NeuralCacheFailure& failure)const{
        return FormatLocalizedText(root,failure.path.wstring());
    }
private:
    const std::wstring& CauseName(NeuralCacheFailure::Cause cause)const{
        switch(cause){
        case NeuralCacheFailure::Cause::InvalidKey:return invalidKey;
        case NeuralCacheFailure::Cause::CreateFailed:return createFailed;
        case NeuralCacheFailure::Cause::AlreadyExists:return alreadyExists;
        case NeuralCacheFailure::Cause::OutsideRoot:return outsideRoot;
        case NeuralCacheFailure::Cause::NoWritableRoot:case NeuralCacheFailure::Cause::None:break;
        }
        return unwritable;
    }
    std::wstring CauseText(const NeuralCacheFailure& failure)const{
        std::wstring text=CauseName(failure.cause);
        if(failure.error)text+=L" (error "+std::to_wstring(failure.error.value())+L")";
        return text;
    }
};

static SourceAcquisition AcquireYouTubeSource(NeuralCacheManager& cache,
                                              const NeuralCacheFailureText& cacheFailureText,
                                              const std::filesystem::path& moduleDirectory,
                                              const std::wstring& mediaUrl,
                                              const std::wstring& audioUrl,
                                              const std::wstring& pageUrl,
                                              YouTubeSourceQuality sourceQuality,
                                              double expectedDurationSeconds,
                                              const std::function<void(const MediaDownloadProgress&)>& onDownload,
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
    if(const auto cached=cache.LookupSource(result.key,stop)){
        const ProbeResult cachedProbe=ProbeMedia(moduleDirectory,cached->payloadPath,stop,MediaProbeMode::CachedMetadata);
        if(stop.stop_requested()){result.cancelled=true;result.detail=L"Neural render was cancelled.";return result;}
        const int64_t durationTolerance=std::max<int64_t>(1,cached->manifest.duration100ns/static_cast<int64_t>(std::max<uint64_t>(1,cached->manifest.frameCount))+1);
        const bool valid=cachedProbe.ok&&cached->manifest.encoder==kCompleteSourcePolicy&&cachedProbe.width==cached->manifest.width&&cachedProbe.height==cached->manifest.height&&std::llabs(cachedProbe.duration100ns-cached->manifest.duration100ns)<=durationTolerance&&std::abs(cachedProbe.duration100ns/10000000.0-expectedDurationSeconds)<=1.0;
        if(valid){result.path=cached->payloadPath;LOG("Source cache hit: content hash and metadata verified; download skipped.");return result;}
        if(!cache.Quarantine(*cached)){result.detail=L"The invalid source cache entry could not be quarantined.";return result;}
    }
    // A cancelled lookup reads as a miss; it must not become a download.
    if(stop.stop_requested()){result.cancelled=true;result.detail=L"Neural render was cancelled.";return result;}
    LOG("Source cache miss or invalid entry; acquiring source.");
    if(onDownload)onDownload({});
    auto staging=cache.BeginSourceStaging(result.key);
    if(!staging){result.detail=cacheFailureText.Describe(cache);return result;}
    MaterializeRequest request{mediaUrl,audioUrl,*staging/L"source.mkv",expectedDurationSeconds};
    MediaMaterializer materializer(moduleDirectory);
    auto materialized=materializer.Run(request,stop,onDownload);
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
                if(!retryStaging){result.detail=cacheFailureText.Describe(cache);return result;}
                staging=std::move(retryStaging);result.key=retryKey;
            }
            request.videoUrl=refreshed.mediaUrl;request.audioUrl=refreshed.audioUrl;request.output=*staging/L"source.mkv";
            materialized=materializer.Run(request,stop,onDownload);
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

// Runs one of the staged FFmpeg tools and hands back what it wrote to stdout,
// or nothing when it could not start, failed, overran `limit` bytes or ran past
// `timeout`. For the timeline's two small questions only - a chapter list and
// one thumbnail - so it holds the whole answer in memory and never asks the
// user anything. The child is in a kill-on-close job, so a player that goes
// away mid-question takes it along.
static std::optional<std::string> RunToolCapture(const std::filesystem::path& executable,
                                                 const std::vector<std::wstring>& arguments,
                                                 std::stop_token stop, std::chrono::milliseconds timeout,
                                                 size_t limit)
{
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE readPipe=nullptr,writePipe=nullptr;
    if(!CreatePipe(&readPipe,&writePipe,&security,0))return std::nullopt;
    SetHandleInformation(readPipe,HANDLE_FLAG_INHERIT,0);
    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    if(job){JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits));}
    STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdOutput=writePipe;
    PROCESS_INFORMATION process{};
    std::wstring commandLine=BuildWindowsCommandLine(executable.native(),arguments);
    BOOL created=FALSE;
    {const ScopedHardErrorSuppression noHardErrorDialog;
     created=CreateProcessW(executable.c_str(),commandLine.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,
                            nullptr,executable.parent_path().c_str(),&startup,&process);}
    CloseHandle(writePipe);
    if(!created){CloseHandle(readPipe);if(job)CloseHandle(job);return std::nullopt;}
    if(job&&!AssignProcessToJobObject(job,process.hProcess)){CloseHandle(job);job=nullptr;}
    ResumeThread(process.hThread);CloseHandle(process.hThread);
    std::string output;bool failed=false;
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    char buffer[16384];
    for(bool exited=false;;){
        DWORD available=0;
        while(PeekNamedPipe(readPipe,nullptr,0,nullptr,&available,nullptr)&&available){
            DWORD read=0;
            if(!ReadFile(readPipe,buffer,std::min<DWORD>(available,DWORD(sizeof(buffer))),&read,nullptr)||!read)break;
            if(output.size()+read>limit){failed=true;break;}
            output.append(buffer,read);
        }
        if(failed||exited)break;
        exited=WaitForSingleObject(process.hProcess,10)==WAIT_OBJECT_0;
        if(!exited&&(stop.stop_requested()||std::chrono::steady_clock::now()>=deadline)){failed=true;break;}
    }
    DWORD exitCode=1;
    if(failed){if(job)TerminateJobObject(job,1);else TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,2000);}
    else GetExitCodeProcess(process.hProcess,&exitCode);
    CloseHandle(process.hProcess);CloseHandle(readPipe);if(job)CloseHandle(job);
    if(failed||exitCode!=0)return std::nullopt;
    return output;
}

// The timeline's two background questions about the loaded file: its chapter
// list, asked once per file, and a thumbnail for wherever the cursor rests.
// One thread, one question at a time, and the NEWEST thumbnail request wins:
// a sweep across the bar asks for dozens, and only the one under the cursor
// when the thread comes free is worth a process. So the cost is bounded to one
// short FFmpeg run at a time however fast the cursor moves. Measured here on a
// 1080p30 H.264 file with a 250-frame GOP: 0.38-0.43 s per thumbnail, 48 ms
// of it starting the process, and 51 ms for the chapter list.
class TimelineMediaWorker {
public:
    struct Thumbnail{uint64_t generation{};int64_t key{};SIZE size{};std::vector<uint8_t> bgra;};
    struct Chapters{uint64_t generation{};std::vector<timeline::Chapter> chapters;};

    explicit TimelineMediaWorker(std::filesystem::path toolDirectory={}):m_toolDirectory(std::move(toolDirectory)){}
    ~TimelineMediaWorker(){if(m_thread.joinable()){m_thread.request_stop();m_wake.notify_all();m_thread.join();}}
    TimelineMediaWorker(const TimelineMediaWorker&)=delete;
    TimelineMediaWorker& operator=(const TimelineMediaWorker&)=delete;

    // Results arrive as `message` posted to `window`, with nothing in it: the
    // taker drains whatever is ready and drops what belongs to another file.
    void RequestChapters(HWND window,UINT message,uint64_t generation,std::filesystem::path media){
        if(!Available(L"ffprobe.exe"))return;
        {std::scoped_lock lock(m_mutex);m_chapterRequest=Request{window,message,generation,0,std::move(media),0.0,{}};}
        Wake();
    }
    void RequestThumbnail(HWND window,UINT message,uint64_t generation,int64_t key,std::filesystem::path media,double seconds,SIZE size){
        if(!Available(L"ffmpeg.exe"))return;
        {std::scoped_lock lock(m_mutex);m_thumbnailRequest=Request{window,message,generation,key,std::move(media),seconds,size};}
        Wake();
    }
    std::vector<Thumbnail> TakeThumbnails(){std::scoped_lock lock(m_mutex);return std::exchange(m_thumbnails,{});}
    std::optional<Chapters> TakeChapters(){std::scoped_lock lock(m_mutex);return std::exchange(m_chapters,std::nullopt);}

private:
    struct Request{HWND window{};UINT message{};uint64_t generation{};int64_t key{};std::filesystem::path media;double seconds{};SIZE size{};};

    std::filesystem::path Tool(const wchar_t* name)const{
        if(!m_toolDirectory.empty())return m_toolDirectory/name;
        const auto directory=platform_paths::ModuleDirectory();
        return directory?*directory/name:std::filesystem::path{};
    }
    bool Available(const wchar_t* name)const{
        const auto tool=Tool(name);std::error_code error;
        return !tool.empty()&&std::filesystem::is_regular_file(tool,error);
    }
    void Wake(){
        if(!m_thread.joinable())m_thread=std::jthread([this](std::stop_token stop){Run(stop);});
        m_wake.notify_all();
    }
    void Run(std::stop_token stop){
        while(!stop.stop_requested()){
            std::optional<Request> chapters,thumbnail;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock,stop,[&]{return m_chapterRequest||m_thumbnailRequest;});
                if(stop.stop_requested())return;
                // The chapter list first: it is one question per file and the
                // markers it draws are on screen for the whole video.
                if(m_chapterRequest)chapters=std::exchange(m_chapterRequest,std::nullopt);
                else thumbnail=std::exchange(m_thumbnailRequest,std::nullopt);
            }
            if(chapters){
                const auto output=RunToolCapture(Tool(L"ffprobe.exe"),timeline::ChapterProbeArguments(chapters->media),
                                                 stop,std::chrono::seconds(10),1u<<20);
                {std::scoped_lock lock(m_mutex);
                 m_chapters=Chapters{chapters->generation,output?timeline::ParseFfprobeChapters(*output):std::vector<timeline::Chapter>{}};}
                PostMessageW(chapters->window,chapters->message,0,0);
            }
            if(thumbnail){
                const size_t bytes=size_t(thumbnail->size.cx)*size_t(thumbnail->size.cy)*4u;
                auto output=RunToolCapture(Tool(L"ffmpeg.exe"),
                    timeline::ThumbnailArguments(thumbnail->media,thumbnail->seconds,thumbnail->size),
                    stop,std::chrono::seconds(5),bytes);
                // A short frame is a failed decode, not a smaller picture.
                if(output&&output->size()==bytes){
                    {std::scoped_lock lock(m_mutex);
                     m_thumbnails.push_back(Thumbnail{thumbnail->generation,thumbnail->key,thumbnail->size,
                                                      std::vector<uint8_t>(output->begin(),output->end())});}
                    PostMessageW(thumbnail->window,thumbnail->message,0,0);
                }
            }
        }
    }

    std::filesystem::path m_toolDirectory;
    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::optional<Request> m_chapterRequest,m_thumbnailRequest;
    std::vector<Thumbnail> m_thumbnails;
    std::optional<Chapters> m_chapters;
    // Last, so it is joined before anything it reads is destroyed.
    std::jthread m_thread;
};

class PlayerApp {
#ifdef PLAYER_APP_TESTING
    friend struct PlayerAppTestAccess;
#endif
public:
    explicit PlayerApp(AppOptions o):m_opt(std::move(o)),m_youtubeSourceQuality(YouTubeSourceQuality::Auto),m_neuralPauseEvent(CreateEventW(nullptr,TRUE,FALSE,nullptr)){}
    ~PlayerApp(){if(m_activityTimer&&m_hwnd)KillTimer(m_hwnd,m_activityTimer);CancelExport();CancelFrameGeneration();CancelNeuralJob(false);CancelYouTubeResolution(false);SaveVideoSettings();if(m_adjustWnd)DestroyWindow(m_adjustWnd);if(m_neuralWnd)DestroyWindow(m_neuralWnd);if(m_encoderWnd)DestroyWindow(m_encoderWnd);UnregisterOverlayHotkeys();Unload(); if(m_font)DeleteObject(m_font); if(m_fontSmall)DeleteObject(m_fontSmall); if(m_iconFont)DeleteObject(m_iconFont); if(m_neuralPauseEvent)CloseHandle(m_neuralPauseEvent);}

    bool Create(HINSTANCE hi) {
        if(!m_uiResources.Load(hi))LOG("Embedded Tabler icon font unavailable; continuing with label-only controls.");
        LoadVideoSettings();
        NeuralCacheManager historyCache(m_cacheRoot);
        if(historyCache.Valid()){
            m_cacheRoot=historyCache.Root();
            SaveCacheSettings();
            LOG("Neural cache directory: "<<WideToUtf8(m_cacheRoot.wstring()));
            m_recent=std::make_unique<RecentMediaHistory>(historyCache.Root()/L"recent-videos.dat");
            if(!m_recent->Load())LOG("Recent videos could not be loaded; existing file preserved until next successful playback.");
        }else{
            // Nothing can be rendered or acquired until this is fixed, and the
            // idle surface is the only thing on screen at this point.
            m_cacheNotice=CacheFailureText().DescribeRoot(historyCache.LastFailure());
        }
        INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_BAR_CLASSES};InitCommonControlsEx(&icc);
        WNDCLASSW r{}; r.style=CS_DBLCLKS|CS_OWNDC; r.lpfnWndProc=RenderWndProcStatic; r.hInstance=hi; r.lpszClassName=L"DLSSVideoRenderClassV11"; r.hCursor=LoadCursor(nullptr,IDC_ARROW); r.hbrBackground=nullptr; RegisterClassW(&r);
        WNDCLASSW v{}; v.lpfnWndProc=ViewportWndProcStatic; v.hInstance=hi; v.lpszClassName=L"DLSSVideoViewportClassV11"; v.hCursor=LoadCursor(nullptr,IDC_ARROW); v.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH); RegisterClassW(&v);
        WNDCLASSEXW w{}; w.cbSize=sizeof(w); w.lpfnWndProc=WndProcStatic; w.hInstance=hi; w.lpszClassName=L"DLSSVideoPlayerV11Class"; w.hCursor=LoadCursor(nullptr,IDC_ARROW); w.hbrBackground=CreateSolidBrush(RGB(18,19,21));
        w.hIcon=static_cast<HICON>(LoadImageW(hi,MAKEINTRESOURCEW(IDI_DLSS_VIDEO_PLAYER),IMAGE_ICON,GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),LR_SHARED));
        w.hIconSm=static_cast<HICON>(LoadImageW(hi,MAKEINTRESOURCEW(IDI_DLSS_VIDEO_PLAYER),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
        RegisterClassExW(&w);
        // 1440x880 of client is the default, which with the frame, the title
        // bar and the menu is a 1440x939 window. On any display whose work area
        // is shorter than that - a 1366x768 laptop, a 1600x900 panel, a 1080p
        // screen with a tall taskbar - Windows places the window with its
        // bottom off screen, and the bottom 50 dip of this player's chrome is
        // the status line and the whole seek bar: the user loses scrubbing
        // entirely and never sees why. The work area is the ceiling, and the
        // window keeps its aspect while shrinking into it.
        RECT rc{0,0,1440,880}; AdjustWindowRect(&rc,WS_OVERLAPPEDWINDOW,TRUE);
        RECT work{};
        int windowX=CW_USEDEFAULT,windowY=CW_USEDEFAULT;
        if(SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0)){
            const LONG workWidth=work.right-work.left,workHeight=work.bottom-work.top;
            const LONG frameWidth=rc.right-rc.left,frameHeight=rc.bottom-rc.top;
            if(workWidth>0&&workHeight>0){
                if(frameWidth>workWidth||frameHeight>workHeight){
                    const double scale=std::min(double(workWidth)/double(frameWidth),
                                                double(workHeight)/double(frameHeight));
                    rc.right=rc.left+std::max<LONG>(640,LONG(std::lround(frameWidth*scale)));
                    rc.bottom=rc.top+std::max<LONG>(480,LONG(std::lround(frameHeight*scale)));
                }
                // Centred, not cascaded. CW_USEDEFAULT offsets each new window
                // down and right, so a frame sized to exactly the work area
                // still hangs its bottom - and therefore the seek bar - under
                // the taskbar.
                windowX=int(work.left+std::max<LONG>(0,(workWidth-(rc.right-rc.left))/2));
                windowY=int(work.top+std::max<LONG>(0,(workHeight-(rc.bottom-rc.top))/2));
            }
        }
        const std::wstring appTitle=m_loc.Get(L"app.title");
        m_hwnd=CreateWindowExW(WS_EX_ACCEPTFILES,w.lpszClassName,appTitle.c_str(),WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN,windowX,windowY,rc.right-rc.left,rc.bottom-rc.top,nullptr,app_menu::CreateMenuBar(m_loc,YouTubePlaybackAvailable()),hi,this);
        if(!m_hwnd) return false;
        ReadAnimationPreference();
        app_menu::UpdateYouTubeQualitySelection(GetMenu(m_hwnd),m_youtubeSourceQuality);
        UpdateRecentMenu();
        LoadUpdateSettings();
        MaybeStartUpdateCheck(false);
        RegisterOverlayHotkeys();
        BOOL dark=TRUE; DwmSetWindowAttribute(m_hwnd,20,&dark,sizeof(dark)); DWORD corner=2; DwmSetWindowAttribute(m_hwnd,33,&corner,sizeof(corner));
        m_viewport=CreateWindowExW(0,v.lpszClassName,nullptr,WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,100,100,m_hwnd,nullptr,hi,nullptr);
        m_renderWnd=CreateWindowExW(WS_EX_ACCEPTFILES,L"DLSSVideoRenderClassV11",nullptr,WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,0,0,100,100,m_viewport,nullptr,hi,this);
        UpdateFontsForDpi(ActiveWindowDpi(m_hwnd));
        DragAcceptFiles(m_hwnd,TRUE); DragAcceptFiles(m_renderWnd,TRUE); ShowWindow(m_viewport,SW_HIDE); Layout(); UpdateTitle();
        StartCacheEviction();
        if(!m_opt.file.empty()){if(IsSupportedYouTubeUrl(m_opt.file))StartYouTubeResolution(m_opt.file,L"",m_youtubeSourceQuality);else Load(m_opt.file);} // No startup file picker: the player opens idle by default.
        return true;
    }

    // Reclaims render entries that can never be served again, and - only when
    // the volume is genuinely short of space - the least recently used ones.
    //
    // At startup, once, on its own thread. A single driver update makes every
    // render unreachable, and "never served again" used to mean only "the
    // manifest is malformed": nothing recorded which driver, model store,
    // version or runtime an entry was keyed under, so the orphans passed the
    // gate and stayed until the disk ran short. Entries now record those terms,
    // and this process's own are resolved here - the same runtime digest and
    // model-store digest a render would key on, both memoised or cheap, off the
    // UI thread - so the orphans go at the next start. When either cannot be
    // resolved cleanly (no runtime staged, a model root that did not read
    // whole) nothing is judged by identity at all: a wrong "current" would
    // delete every render that still works.
    //
    // The file opened at startup is looked up while this runs. It is not passed
    // as active because its key is not known until its source is hashed;
    // instead a lookup marks the entry used under the cache root's lock before
    // it reads it, and eviction re-checks that mark under the same lock before
    // it removes anything, so an entry being opened is skipped rather than
    // deleted from under the lookup.
    //
    // Detached rather than joined: it walks directories, which on a large
    // cache on a cold disk is seconds, and none of it needs to happen before
    // the window appears. The manager is constructed inside the thread so
    // nothing is shared with the UI.
    void StartCacheEviction(){
        if(m_cacheRoot.empty())return;
        std::thread([root=m_cacheRoot,driverVersion=m_opt.detectedGpu.driverVersion,moduleDirectory=ExecutableDirectory()]{
            NeuralCacheManager cache(root);
            if(!cache.Valid())return;
            // A crashed session's live/pid<N> - gigabytes of segments - had no
            // owner left to remove it; this process only ever removes its own.
            cache.SweepLiveSessions();
            std::optional<cache_eviction::Identity> current;
            if(!driverVersion.empty()){
                const auto runtime=BuildRuntimeDigest(moduleDirectory/L"neural-runtime",LockedRuntimeFileNames());
                const NeuralModelStore models=ResolveNeuralModelStore(driverVersion);
                if(runtime&&NeuralModelStoreSettled(models))
                    current=cache_eviction::Identity{DLSS_VIDEO_PLAYER_VERSION,NeuralCacheInstallation(),*runtime,WideToUtf8(driverVersion),models.digest};
                else LOG("Cache eviction is not judging entries by identity: runtime="<<(runtime?"resolved":"unavailable")
                         <<" modelStore="<<(NeuralModelStoreSettled(models)?"settled":"unsettled")<<".");
            }
            cache.Evict({},cache_eviction::kDefaultFreeFloorBytes,current?&*current:nullptr);
        }).detach();
    }

    // Only a live network stream needs the non-blocking read and the
    // re-resolving seek path. A synchronized cache pair and an acquired local
    // copy of a stream are ordinary files, whatever the source identity says.
    bool NetworkPlayback()const{return m_sourceKind==MediaSourceKind::YouTube&&!m_cachedPlayback&&!m_cachedSourceFile;}
    // Decode semantics of the loaded payload: an acquired copy of a stream is a
    // local file even though its identity stays networked.
    MediaSourceKind DecodeKind()const{return m_cachedSourceFile?MediaSourceKind::LocalFile:m_sourceKind;}


    // The main pump's entry. Reaching it means no modal loop is running on this
    // thread - they do not return until they end - so whatever started the
    // modal tick timer has finished with it.
    void Tick(){StopModalTick();RunTick();}
    // A modal loop - a menu, a window move or resize - never returns to the
    // main pump, so Tick stopped while audio kept playing: the video froze for
    // as long as the menu was open, then a local file decoded and dropped the
    // whole backlog in one Tick, and a neural pair re-anchored, which counts
    // toward the "cannot follow" limit. A USER timer is dispatched by every
    // modal loop, so it drives Tick until the loop ends. Its ~16 ms resolution
    // judders a 60 fps source, which is acceptable for the length of a menu
    // visit; the main pump takes back over on return.
    //
    // Message boxes and owned dialogs deliberately do NOT tick: they are raised
    // from inside command handlers, often between two halves of a state change
    // (a confirm before an unload, an error mid-load), and a Tick there would
    // present or seek against half-updated state. Video holds for the length
    // of a dialog instead.
    void StartModalTick(){
        if(!m_modalTickTimer&&m_hwnd)m_modalTickTimer=SetTimer(m_hwnd,kModalTickTimerId,USER_TIMER_MINIMUM,nullptr);
    }
    void StopModalTick(){
        if(m_modalTickTimer){if(m_hwnd)KillTimer(m_hwnd,m_modalTickTimer);m_modalTickTimer=0;}
    }
    // Never re-entered. Tick itself can open a message box (a failed seek, a
    // lost device), whose modal loop dispatches the timer above; a nested Tick
    // would run a seek or a present in the middle of the one that raised it.
    void RunTick(){
        if(m_inTick)return;
        m_inTick=true;
        struct TickScope{bool& active;~TickScope(){active=false;}}scope{m_inTick};
        TickOnce();
    }
    void TickOnce() {
        ReapSourcePrefetch();
        ReapRetiringNeuralWorker();
        // Headphones unplugged, a default-device change, a driver restart: the
        // endpoint reports itself invalidated and audio restarts on the new
        // one. waveOut had no equivalent - the write failed, the reader thread
        // broke, and the film played on in silence. Cheap when nothing
        // happened, which is every tick but the one.
        Audio().ServiceDeviceChanges(m_loaded&&!Audio().Active()?Position():-1.0,m_playing);
        UpdateLiveSession();
        WatchNeuralJobProgress();
        if(m_seekPending) {
            const double target=m_pendingSeekSec; const bool resume=m_seekResumePlaying;
            m_seekPending=false; PerformSeek(target,resume); return;
        }
        // A paused frame is presented when something invalidated it - a window
        // resize or repaint, or a renderer setting the present pass reads - and
        // not otherwise. It used to be re-presented at 60 Hz whether or not
        // anything had changed: a full-screen draw and a Present per 16 ms of
        // every pause, for an image the compositor already holds.
        if(m_loaded&&!m_playing&&!m_seeking&&m_renderer&&(m_staticPresentPending||m_renderer->PresentationStale())){
            m_staticPresentPending=false;++m_staticPresents;
            if(!m_renderer->PresentCurrent()&&RecoverUnusableRenderer())return;
        }
        // Before every early return below, because the state worth reporting is
        // the one where nothing is reaching the screen. Reporting from the
        // present path instead meant the worst case - no frames presented at
        // all - was the quietest: one measured session logged a single line
        // covering 132 s and two presented frames.
        ReportPlaybackHealth();
        if(m_loaded&&m_cachedPlayback&&m_playing&&!m_haveNext&&!m_seeking){
            if(!ReadNextCachedFrame())return;
        }
        if(m_loaded&&m_playing&&NetworkPlayback()&&!m_haveNext&&!m_seeking){
            if(ApplyNetworkRead(m_decoder.ReadNextAvailable(m_next),NetworkReadPosition::BeforeRender)!=NetworkReadAction::UseFrame)return;
        }
        if(!m_loaded||!m_playing||!m_haveNext||m_seeking) return;
        double now=Position(); const double frameDur=1.0/std::max(1.0,m_decoder.FrameRate());
        bool dropped=false;
        // A neural pair is not a frame this loop can afford to discard. Every
        // other path advances by decoding ONE stream, so throwing a late frame
        // away is cheap; a neural session advances by decoding a PAIR, so a
        // discard costs exactly what a present costs and the loop below cannot
        // win a race it doubles the length of. See PlaybackCadence.h for the
        // measured collapse - 0.31 fps presented, 203 discarded in 11.6 s - and
        // for the two levers that replace it.
        // A local read that stops for any reason but the end of the file says so.
        // ReadNext's bool made a decode that died mid-file look exactly like the
        // last frame: playback stopped as if finished, with nothing on screen.
        const auto readLocal=[this]{
            const VideoReadResult read=m_decoder.ReadNextBlocking(m_next);
            if(read==VideoReadResult::FrameReady)return true;
            if(read==VideoReadResult::Error||read==VideoReadResult::Stalled){
                LOG("Playback stopped: decoding failed after "<<m_currentSec<<" s (result="<<static_cast<int>(read)<<").");
                m_decodeNotice=L"Playback stopped: the video could not be decoded past "+
                    FormatTimecode(static_cast<int64_t>(m_currentSec*1e7),m_decoder.FrameRate(),false)+L" (see the log)";
            }
            return false;
        };
        if(m_cachedPlayback){
            if(!CadenceAdvanceCachedFrame(now,frameDur))return;
        }else{
        while(m_haveNext) {
            double due=double(m_next.timestamp100ns)*1e-7;
            if(now-due <= playback_timing::LateFrameThreshold(frameDur)) break;
            VideoFrame skip=std::move(m_next); (void)skip; ++m_droppedFrames; dropped=true;
            if(NetworkPlayback()){if(ApplyNetworkRead(m_decoder.ReadNextAvailable(m_next),NetworkReadPosition::BeforeRender)!=NetworkReadAction::UseFrame)break;}
            else if(!readLocal()){m_haveNext=false;break;}
        }
        }
        if(dropped){m_guides.Reset();m_guideReset=true;m_dlssReset=true;}
        if(!m_haveNext){if(!NetworkPlayback()){m_playing=false;Audio().Pause(true);}InvalidateControls();InvalidatePlaybackProgress();UpdateCachedStatus();return;}
        const VideoFrame& next=NextFrame();
        double due=double(next.timestamp100ns)*1e-7;
        if(now+0.001<due) return;
        if(RenderVideoFrame(next,next.discontinuity||m_guideReset)) {
            LeaveSettingsPreviewFrame();
            RememberRenderedCachedPair();
            // Counted for plain playback too: a viewer reporting choppy playback
            // on an ordinary file produced no health line at all, because both
            // this and ReportPlaybackHealth were gated on a cached pair.
            ++m_cachedPresentedFrames;
            ++m_fpsWindowFrames;
            const auto fpsNow=Clock::now();
            const double fpsElapsed=std::chrono::duration<double>(fpsNow-m_fpsWindowStart).count();
            if(fpsElapsed>=0.75){m_submitFps=double(m_fpsWindowFrames)/fpsElapsed;m_fpsWindowFrames=0;m_fpsWindowStart=fpsNow;}
        }else if(RecoverUnusableRenderer())return;
        m_currentSec=due; m_guideReset=false; m_dlssReset=false;
        if(m_cachedPlayback){m_haveNext=false;m_nextPairFrame.reset();}
        else if(NetworkPlayback())ApplyNetworkRead(m_decoder.ReadNextAvailable(m_next),NetworkReadPosition::AfterRender);
        else if(!readLocal()){m_haveNext=false;m_playing=false;Audio().Pause(true);}
        InvalidatePlaybackProgress();
        UpdateCachedStatus();
    }

    bool Running()const{return m_running;}
    bool NeedsRealtimeTick()const{return m_loaded;}
    DWORD TickSleepMs()const{return (m_loaded&&!m_playing&&!m_seekPending&&!m_seeking)?8u:0u;}
    // Blocks until there is something to do, rather than yielding and coming
    // straight back. While playing, TickSleepMs() was 0, so this loop spun a
    // core at 100% and every wasted iteration re-walked the live coverage
    // spans under their mutex, read waveOutGetPosition under the audio
    // producer's lock, and - on a YouTube source - built a NeuralCacheManager
    // twice, which enumerates the staging directory.
    //
    // A high-resolution waitable timer, not the swapchain's frame-latency
    // object: asking DXGI for that one means creating the swapchain with
    // DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, and the neural
    // runtime's add-on hooks this swapchain and stops producing frames when
    // that flag is set - see the comment in D3D12Renderer::CreateDevice. The
    // timer needs nothing from the presentation path and is what the measured
    // 113% -> 18% of one core came from.
    //
    // 2 ms while playing: far below any frame interval (8.34 ms at 119.88 fps)
    // so a due frame is never missed by more than that, and far above the
    // ~0 ms the loop was effectively using.
    void WaitForNextTick(){
        const DWORD sleepMs=TickSleepMs();
        m_tickSleeper.SleepMs(int(sleepMs?sleepMs:2u));
    }


private:
    static constexpr UINT_PTR kActivityTimerId=0xD155;
    static constexpr UINT_PTR kFullscreenTimerId=0xD156;
    static constexpr UINT_PTR kPreviewTimerId=0xD157;
    static constexpr UINT_PTR kModalTickTimerId=0xD158;
    static constexpr UINT_PTR kChipFlashTimerId=0xD159;
    static constexpr auto kFullscreenIdleDelay=std::chrono::milliseconds(2500);
    // How long a live or cached pair may stay NotReady before the player stops
    // waiting for it. A segment source is reopened at every boundary and after
    // every seek; the slowest reopen measured here is a 2560x1440 file at
    // 0.26 s (see "Seek timing" in the log), so three seconds is a reopen that
    // is never going to finish rather than a slow one.
    static constexpr double kPairStallSeconds=3.0;
    // What makes two reads part of the SAME wait. The window above measures a
    // contiguous run of NotReady reads, and only Tick's playing branch reads at
    // all: a pause, a seek, a buffering wait or an unload lands between one
    // NotReady and the next and stops the reads without stopping the clock.
    // Without this the window ran across that gap, so pausing on a segment
    // boundary for three seconds made the ordinary decoder warm-up on resume -
    // one NotReady - measure as a wedged pair and hand the session back.
    // A gap this long cannot be an interval between ticks: the tick sleeps zero
    // while playing, and a stalled pair is read again within a millisecond.
    static constexpr double kPairStallGapSeconds=1.0;
    // Consecutive re-anchors before the session gives up following live. Three
    // is enough to ride out a transient - a segment boundary landing under a
    // burst of render work - and short enough that a source this machine simply
    // cannot follow says so within a few seconds instead of hitching for ever.
    static constexpr int kCadenceReanchorLimit=3;
    // A stage export belongs here for the same reason the other two do: it is
    // minutes of work with a panel on screen, and this predicate is what arms
    // the repaint timer that animates it. Without it the spinner never moved
    // and the elapsed clock read zero.
    bool ActivityBusy()const{return NeuralJobActive()||m_youtubeLifecycle.IsResolving()||m_stageExport.running;}
    // A job that renders behind the loaded media: the player keeps the window,
    // and only its own panel and lanes report progress.
    bool JobBehindPlayback()const{return m_liveSession||m_previewJob;}

    void UpdateRecentMenu(){
        if(!m_hwnd||!m_recent)return;
        std::vector<std::wstring> titles;for(const auto& entry:m_recent->Entries())titles.push_back(entry.title.empty()?entry.source:entry.title);
        app_menu::UpdateRecentVideos(GetMenu(m_hwnd),titles,!ActivityBusy());
        DrawMenuBar(m_hwnd);
    }
    void ForgetSourceDigest(){
        if(!m_sourceDigestMemo)return;
        std::scoped_lock lock(m_sourceDigestMemo->mutex);
        source_digest::Forget(m_sourceDigestMemo->entry);
    }

    // Playback > Audio track, rebuilt whenever the source changes. The list
    // is empty for a source with one unremarkable track, which is most of
    // them, and the menu shows a disabled placeholder for that.
    void UpdateAudioTrackMenu(){
        if(!m_hwnd)return;
        std::vector<std::wstring> labels;
        for(const auto& track:Audio().AudioTracks())
            labels.push_back(Utf8ToWide(audio_track::Describe(track)));
        app_menu::UpdateAudioTracks(GetMenu(m_hwnd),labels,Audio().SelectedAudioTrack());
        DrawMenuBar(m_hwnd);
    }
    void ChooseAudioTrack(int audioIndex){
        if(!Audio().SelectAudioTrack(audioIndex))return;
        UpdateAudioTrackMenu();
    }
    void RecordRecent(const NeuralJobCompletion& completion,bool preserveCache=false){
        if(!m_recent)return;
        RecentMediaEntry entry{};entry.youtube=completion.sourceKind==MediaSourceKind::YouTube;
        // A path that cannot be made absolute - a malformed name, a working
        // directory that vanished - is recorded as given rather than thrown at
        // the message loop.
        std::error_code pathError;
        std::filesystem::path source=entry.youtube?std::filesystem::path{}:std::filesystem::absolute(completion.sourcePath,pathError);
        if(pathError){LOG("Recent video path could not be resolved ("<<pathError.message()<<"); recording it as given.");source=completion.sourcePath;}
        const std::wstring sourceText=source.lexically_normal().wstring();
        entry.id=entry.youtube?CanonicalYouTubeVideoId(completion.pageUrl):WideToUtf8(sourceText);
        entry.title=DisplayTitleForSource(completion.sourceKind,completion.displayTitle);entry.source=entry.youtube?completion.pageUrl:sourceText;entry.sourceKey=completion.sourceKey;entry.renderKey=completion.renderKey;entry.sourceQuality=static_cast<int>(completion.sourceQuality);
        if(preserveCache)for(const auto& previous:m_recent->Entries()){
            const bool sameVideo=previous.youtube==entry.youtube&&((entry.youtube&&previous.id==entry.id&&previous.sourceQuality==entry.sourceQuality)||(!entry.youtube&&_wcsicmp(previous.source.c_str(),entry.source.c_str())==0));
            if(sameVideo&&(entry.sourceKey.empty()||entry.sourceKey==previous.sourceKey)){entry.sourceKey=previous.sourceKey;entry.renderKey=previous.renderKey;break;}
        }
        // Remember reports what fell off the five-entry list and what it
        // replaced. Nothing is done with either any more, and that is the whole
        // policy change: the Recent list is a five-item MENU and the cache is a
        // work product, so a menu rolling over was deleting renders that cost
        // minutes of GPU time each. Opening the sixth video threw away the
        // first one's render, and a session that revisited it rendered the same
        // seconds again from scratch. A render is keyed by its source and its
        // settings, so it stays reachable for as long as it stays on disk: the
        // same video opened with the same settings in any later session is a
        // cache hit. Retention is now bounded only by "Clear neural cache",
        // which reports what it is about to delete.
        (void)m_recent->Remember(std::move(entry));
        if(!m_recent->Save())LOG("Recent video history could not be saved.");
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
        UpdateYouTubeQualitySelection(GetMenu(m_hwnd),m_youtubeSourceQuality);NoteLoadedSourceQuality();return true;
    }
    void OpenRecent(size_t index){
        if(!m_recent||index>=m_recent->Entries().size()||ActivityBusy())return;
        const auto entry=m_recent->Entries()[index];
        if(entry.youtube){if(NeuralPreRenderEnabled()&&!entry.sourceKey.empty())StartNeuralJob(entry.source,{},entry.title,entry.source,MediaSourceKind::YouTube,static_cast<YouTubeSourceQuality>(entry.sourceQuality),entry.sourceKey,0.0,{},true);else StartYouTubeResolution(entry.source,entry.title,static_cast<YouTubeSourceQuality>(entry.sourceQuality));return;}
        std::error_code fileError;
        if(!std::filesystem::is_regular_file(entry.source,fileError)){MessageBoxW(m_hwnd,T(L"recent.missing").c_str(),T(L"app.title").c_str(),MB_OK|MB_ICONINFORMATION);return;}
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
        catch(const std::system_error&){MessageBoxW(m_hwnd,T(L"export.worker_failed").c_str(),T(L"export.title.failed").c_str(),MB_OK|MB_ICONERROR);return;}
        SyncFeatureMenuState();UpdateCachedStatus();
    }
    void CancelExport(){if(m_exportWorker.joinable()){m_exportWorker.request_stop();m_exportWorker.join();m_exportWorker=std::jthread{};}m_exportCompletions.Clear();
        // Cleared HERE and not only in CompleteExport: clearing the registry is
        // what makes the queued completion find nothing, so CompleteExport
        // returns before its own reset runs. Without this the export panel
        // covers the video until some later export happens to finish.
        m_stageExport={};SyncActivityFeedback();if(m_hwnd&&IsWindow(m_hwnd)){SyncFeatureMenuState();UpdateCachedStatus();}}
    void CompleteExport(uint64_t token){auto completion=m_exportCompletions.Take(token);if(!completion)return;
        // Whatever happened, the export is over and the panel says so.
        m_stageExport={};SyncActivityFeedback();if(m_exportWorker.joinable()){m_exportWorker.join();m_exportWorker=std::jthread{};}SyncFeatureMenuState();UpdateCachedStatus();if(completion->result.ok){const std::wstring message=L"Exported to:\n"+completion->output.wstring();MessageBoxW(m_hwnd,message.c_str(),T(L"export.title.complete").c_str(),MB_OK|MB_ICONINFORMATION);}else if(completion->result.error!=MaterializeError::Cancelled)MessageBoxW(m_hwnd,completion->result.detail.c_str(),T(L"export.title.failed").c_str(),MB_OK|MB_ICONERROR);}

    // Frame generation runs as a conversion, not as live presentation: the pass
    // writes a new file at the planned multiple of the source rate, the user
    // watches it progress, and the player then loads the result. Live pacing
    // would have to interleave generated frames into the playback clock, which
    // also owns audio sync, dropped-frame accounting and seeking; the
    // conversion needs none of that and is the shape the user asked for.
    //
    // The runtime's cap is measured once per process, on the first invocation,
    // because it decides which multiples are admissible at all and a wrong
    // guess would plan a rate the runtime then refuses. It is a create and
    // release of one NGX feature on a throwaway device, which is why a wait
    // cursor is enough and no background probe runs at startup.
    // What the conversion should read, and whether that is the neural render.
    //
    // A viewer watching the neural view with a render cached is watching the
    // neural carrier, and converting the ORIGINAL there would silently throw
    // that away - the file they get back would be smooth and un-neural, and
    // re-rendering neural afterwards costs four times the frames and a new
    // cache identity. The carrier is constant-rate at exactly the source rate
    // (SynchronizedPlayback::Open refuses a pair whose rates differ by more
    // than 0.01 fps), so it is as valid an input as the original.
    //
    // The pass hands the file to FFmpeg and to a decoder of its own, so a
    // network source only qualifies once the player is already playing an
    // acquired local copy of it. A live session or an export in flight is
    // excluded because both are already competing for the same GPU and helper
    // directory.
    struct FrameGenerationInput {
        std::wstring path;
        bool neural{};
    };
    FrameGenerationInput FrameGenerationInputSource()const{
        if(!m_loaded)return{};
        // The neural carrier qualifies only when it is the WHOLE source rendered
        // with the settings on screen. A range render covers m_cachedRange
        // alone, so converting it would hand back a clip-length file that then
        // gets adopted under the film's title; and a carrier rendered with
        // settings the user has since changed is not the picture they are
        // watching. The cached export refuses those same two cases, for the
        // same two reasons.
        // The view gate is the Neural Rendering TOGGLE, not an inspection
        // control: with a render cached, ToggleNeuralRendering switches this
        // view and nothing else, so "the neural view is on screen" and "neural
        // rendering is on" are the same state. Converting the carrier while it
        // is on is therefore both the user's expressed choice and NVIDIA's own
        // order - Super Resolution first, Frame Generation on the upscaled
        // result - and turning the toggle off is how someone asks for the
        // original instead. `framegen.confirm.neural` says which file it is.
        if(m_cachedPlayback&&!m_neuralPath.empty()&&m_comparisonView==ComparisonView::Neural&&
           CachedRangeCoversSource()&&m_cachedSettings==m_neuralSettings&&m_cachedGuides==m_renderGuides)
            return{m_neuralPath.wstring(),true};
        if(m_sourceKind==MediaSourceKind::LocalFile||m_cachedSourceFile)return{m_path,false};
        // A stream the cache already holds a complete copy of IS convertible,
        // whether or not playback has moved onto that copy. Requiring the
        // adoption - which only happens on a seek or around a render - is why
        // every YouTube video reported "needs a local copy" even when its copy
        // was sitting in the cache from an earlier render.
        if(const std::filesystem::path* copy=FrameGenerationAcquiredCopy())return{copy->wstring(),false};
        return{};
    }
    // Where the audio, subtitles and chapters are copied from, which is not
    // always the file the frames came from and is never the loaded path when
    // that path is a stream.
    //
    // `m_path` on an un-adopted YouTube source is a signed googlevideo URL, and
    // MuxVideoWithSourceStreams requires a regular file on both of its inputs.
    // Handing it the URL is how a 2560x1440 trailer spent 63 s of GPU work
    // generating 3132 frames and then threw all of them away at the last stage
    // with "the encoder refused the specification" - the muxer's InvalidSpecification,
    // reported as if the encode had been wrong. Playback only moves onto the
    // acquired copy on a seek or around a render, so the copy has to be found
    // the same way FrameGenerationInputSource finds it.
    std::wstring FrameGenerationStreamSource()const{
        if(m_sourceKind==MediaSourceKind::LocalFile||m_cachedSourceFile)return m_path;
        if(const std::filesystem::path* copy=FrameGenerationAcquiredCopy())return copy->wstring();
        return {};
    }
    // Whether the loaded file is one of this player's own frame-generation
    // conversions. Those are written to <cache>/frame-generation and nowhere
    // else, so the directory IS the provenance record and it survives the
    // session that made it - `m_frameGenLastOutput` does not.
    //
    // It matters because rendering one is the pipeline BACKWARDS. NVIDIA's
    // order is Super Resolution first and Frame Generation on the upscaled
    // result, which is what converting a neural render does; going the other
    // way asks the renderer for N times the frames - a 2x conversion doubles a
    // job already measured in minutes - and spends them upscaling frames that
    // were interpolated rather than photographed. The player cannot simply do
    // the right thing here: the original is not what is loaded, and picking a
    // different file to render is not a decision to take behind someone's back.
    // So it says so, once, and renders what was asked for.
    bool LoadedSourceIsGeneratedFrames()const{
        if(!m_loaded||m_path.empty()||m_sourceKind!=MediaSourceKind::LocalFile)return false;
        NeuralCacheManager cache(m_cacheRoot);
        if(!cache.Valid())return false;
        std::error_code error;
        const auto generated=std::filesystem::weakly_canonical(cache.Root()/L"frame-generation",error);
        if(error)return false;
        const auto loaded=std::filesystem::weakly_canonical(std::filesystem::path(m_path),error);
        if(error)return false;
        // Compared as paths, not as text: a prefix match on the string would
        // also accept a sibling directory whose name starts the same way.
        const auto relative=loaded.lexically_relative(generated);
        return !relative.empty()&&*relative.begin()!=L"..";
    }
    // Memoised, because FrameGenerationUiNow runs on every status refresh and
    // every toolbar paint, and the answer costs a cache-root validation and two
    // filesystem stats. Cleared whenever the loaded source changes or a
    // background acquisition finishes, which are the only two ways it can move.
    const std::filesystem::path* FrameGenerationAcquiredCopy()const{
        if(m_sourceKind!=MediaSourceKind::YouTube)return nullptr;
        if(!m_frameGenCopyChecked){
            m_frameGenCopyChecked=true;
            m_frameGenCopy=AcquiredSourceCopyPath();
        }
        return m_frameGenCopy?&*m_frameGenCopy:nullptr;
    }
    void InvalidateFrameGenerationCopy(){
        m_frameGenCopyChecked=false;m_frameGenCopy.reset();
        // The key memo is dropped with it. They answer the same question at two
        // depths, and a settled acquisition is the moment both answers move:
        // leaving the key memo alone is how a COMPLETED download left the player
        // insisting the stream had no local copy until it was restarted.
        m_sourceKeyMemo={};
    }
    // One state for the whole feature, computed once and read by the menu, the
    // toolbar pill and the status line. Three surfaces each deciding for
    // themselves is how the menu item stayed enabled while the pill greyed out
    // on a seek, and how "ready" was claimed on hardware that had never been
    // asked. Busy and Refused are deliberately different states: a player that
    // is merely occupied and a video that can never be converted both used to
    // read "FG unavailable".
    // Unchecked is Ready with one fact missing: the plan holds, and only the
    // runtime's own admission has not been measured yet, which happens inside
    // the click. The control is live in both - the states differ in what the
    // status line may promise, not in what the user can do.
    // NeedsSourceCopy and CopyingSource are the streaming cases. They are not
    // refusals: the pass converts a file, the player can keep one, and the
    // control's job in that state is to offer that rather than grey out - which
    // is what every YouTube trailer used to do, with the reason only in the
    // status line.
    enum class FrameGenerationUiState{Converting,Stopping,Busy,NoLocalCopy,NeedsSourceCopy,
                                      CopyingSource,Unchecked,Refused,Ready};
    struct FrameGenerationUi{
        FrameGenerationUiState state{FrameGenerationUiState::NoLocalCopy};
        frame_rate_policy::FrameGenerationPlan plan{};
    };
    FrameGenerationUi FrameGenerationUiNow()const{
        if(m_frameGenWorker.joinable())
            return{m_frameGenCancelling?FrameGenerationUiState::Stopping:FrameGenerationUiState::Converting,{}};
        if(!m_loaded)return{FrameGenerationUiState::NoLocalCopy,{}};
        if(FrameGenerationInputSource().path.empty()){
            // A stream with no copy yet is the ONE unavailable state the player
            // can do something about, so it is offered rather than greyed: the
            // acquisition that a render already uses can fetch the file, and
            // the conversion becomes available when it lands.
            if(m_sourceKind==MediaSourceKind::YouTube&&!m_youtubePageUrl.empty()&&
               !m_youtubeLifecycle.IsResolving()&&m_decoder.DurationSeconds()>0.0)
                return{SourcePrefetchActive()?FrameGenerationUiState::CopyingSource
                                             :FrameGenerationUiState::NeedsSourceCopy,{}};
            return{FrameGenerationUiState::NoLocalCopy,{}};
        }
        if(ActivityBusy()||m_exportWorker.joinable()||m_seeking||m_seekPending||!m_renderer)
            return{FrameGenerationUiState::Busy,{}};
        // No Checking state: see the hazard note above MaybeProbe's removal -
        // the cap is measured inside the click, not on a worker beside it.
        // Every refusal except RuntimeRefused is decidable without the GPU, so
        // the plan is made against the phase-verified ceiling before the runtime
        // has been asked. The answer is honest either way: what the driver
        // admits can only narrow it, and that narrowing is measured once.
        const auto plan=PlannedFrameGeneration(FrameGenerationPlanningCap());
        if(!plan.Generates())return{FrameGenerationUiState::Refused,plan};
        if(!m_frameGenCapability)return{FrameGenerationUiState::Unchecked,plan};
        return{FrameGenerationUiState::Ready,plan};
    }
    bool FrameGenerationAvailable()const{
        const auto state=FrameGenerationUiNow().state;
        // NeedsSourceCopy counts as available because the control does something
        // in that state: it offers the copy. The menu item and the pill read
        // this, and a control the user can act on must not be greyed.
        return state==FrameGenerationUiState::Ready||state==FrameGenerationUiState::Unchecked||
               state==FrameGenerationUiState::NeedsSourceCopy;
    }
    // The two facts VideoDecoder probes and this policy refuses on. Both
    // refusals were unreachable while this call declared constant-frame-rate
    // unconditionally and handed over the decoder's 30 fps fallback as if it
    // had been read off the file: a phone's variable-rate recording sailed
    // through and the pass emitted sourceFrames*multiplier at a rate the
    // file never had, which the audio it now carries would drift against.
    frame_rate_policy::SourceCadence SourceCadenceNow()const{
        return {m_decoder.FrameRateKnown()?m_decoder.FrameRate():0.0,
                m_decoder.IsStillImage(),m_decoder.ConstantFrameRate()};
    }
    frame_rate_policy::FrameGenerationPlan PlannedFrameGeneration(uint32_t multiFrameCountMax)const{
        return PlannedFrameGeneration(multiFrameCountMax,m_evenCadenceOnly);
    }
    frame_rate_policy::FrameGenerationPlan PlannedFrameGeneration(uint32_t multiFrameCountMax,
                                                                  bool requireEvenCadence)const{
        return frame_rate_policy::PlanFrameGeneration(SourceCadenceNow(),
                                                      MonitorModeCached().refreshHz,
                                                      multiFrameCountMax,requireEvenCadence);
    }
    static const wchar_t* FrameGenerationRefusalKey(frame_rate_policy::FrameGenerationRefusal refusal){
        using R=frame_rate_policy::FrameGenerationRefusal;
        switch(refusal){
            case R::None:return nullptr;
            case R::UnknownSourceRate:return L"framegen.refusal.unknown_rate";
            case R::StillImage:return L"framegen.refusal.still_image";
            case R::VariableFrameRate:return L"framegen.refusal.variable_rate";
            case R::UnknownRefresh:return L"framegen.refusal.unknown_refresh";
            case R::SourceMeetsRefresh:return L"framegen.refusal.meets_refresh";
            case R::RefreshBelowDouble:return L"framegen.refusal.below_double";
            case R::SourceCadenceEven:return L"framegen.refusal.source_even";
            case R::EvenCadenceRequired:return L"framegen.refusal.even_only";
            case R::RuntimeRefused:return L"framegen.refusal.runtime";
        }
        return nullptr;
    }
    std::wstring FrameGenerationRefusalText(frame_rate_policy::FrameGenerationRefusal refusal)const{
        const wchar_t* key=FrameGenerationRefusalKey(refusal);
        return key?T(key):std::wstring{};
    }
    // The status line's own form of the same refusal. It used to print
    // FrameGenerationRefusalName - the log slug, "no-even-multiple" - while a
    // written sentence for every one of those cases sat unused beside it.
    std::wstring FrameGenerationRefusalShort(frame_rate_policy::FrameGenerationRefusal refusal)const{
        const wchar_t* key=FrameGenerationRefusalKey(refusal);
        return key?T((std::wstring(key)+L".short").c_str()):std::wstring{};
    }
    // One formatter for every frame rate the user sees. A 23.976 fps source used
    // to be confirmed as "23.976 fps -> 47.952 fps", reported on the status line
    // as "48 fps" and named "...-2x48fps.mkv": three renderings of one number.
    // Integers print as integers, and a rate that is not one keeps three
    // decimals, which is all a container's rational rate is worth.
    static std::wstring FormatFrameRate(double fps){
        if(!std::isfinite(fps)||fps<=0.0)return L"?";
        wchar_t text[32]{};
        const double rounded=std::round(fps);
        if(std::abs(fps-rounded)<0.005)swprintf_s(text,L"%.0f",rounded);
        else swprintf_s(text,L"%.3f",fps);
        return text;
    }
    // The generated-frame budget the player plans against: three ceilings, and
    // the smallest wins. What this project has phase-verified, what the runtime
    // admits once that has been measured, and what the USER asked for -
    // m_frameGenPreference, 1 generated frame (2x) on a fresh install, 0 for
    // "as many as the display allows". A conversion is minutes of GPU work and
    // a large file, so taking the largest admissible multiple automatically is
    // the player deciding how much of both to spend.
    //
    // Before the runtime has been asked, the measured ceiling stands in for its
    // answer: it can only over-estimate that one number, and every refusal
    // except RuntimeRefused is already decidable without it.
    uint32_t FrameGenerationPlanningCap()const{
        const uint32_t measured=m_frameGenCapability?FrameGenerationCap()
                                                    :frame_rate_policy::kPhaseVerifiedMultiFrameCount;
        if(m_frameGenPreference==0u)return measured;
        return std::min(measured,m_frameGenPreference);
    }
    uint32_t FrameGenerationCap()const{
        if(!m_frameGenCapability||!m_frameGenCapability->available)return 0u;
        return std::min(m_frameGenCapability->multiFrameCountMax,
                        frame_rate_policy::kPhaseVerifiedMultiFrameCount);
    }
    // The multiple this display WOULD accept if the preference were lifted, or
    // 0 when the preference is not what is standing in the way. Without this a
    // 24 fps film on a 120 Hz panel reads "already lands evenly" - true, and it
    // sounds final - while 5x divides that refresh exactly and the setting says
    // 2x: a refusal the user can act on, reported as one they cannot.
    uint32_t FrameGenerationMultipleBeyondPreference()const{
        if(m_frameGenPreference==0u)return 0u;
        const uint32_t measured=m_frameGenCapability?FrameGenerationCap()
                                                    :frame_rate_policy::kPhaseVerifiedMultiFrameCount;
        if(measured<=m_frameGenPreference)return 0u;
        if(PlannedFrameGeneration(FrameGenerationPlanningCap()).Generates())return 0u;
        const auto unlimited=PlannedFrameGeneration(measured);
        return unlimited.Generates()?unlimited.multiplier:0u;
    }
    // The multiple an uneven cadence would allow while the even-cadence-only
    // setting withholds it, or 0 when that setting is not what stands in the
    // way. Same shape as the preference above and for the same reason: the two
    // refusals a user can lift have to name the thing to lift.
    uint32_t FrameGenerationMultipleWithoutEvenCadence()const{
        if(!m_evenCadenceOnly)return 0u;
        const auto uneven=PlannedFrameGeneration(FrameGenerationPlanningCap(),false);
        return uneven.Generates()?uneven.multiplier:0u;
    }
    // The mode this monitor could be switched to so the conversion lands on the
    // refresh exactly - the one move that removes an unevenness instead of
    // reducing it. Enumerating modes is expensive, so this is reached from the
    // refusal dialog and from nowhere that paints.
    frame_rate_policy::RefreshSwitchOffer FrameGenerationRefreshOffer()const{
        const std::vector<double> rates=AvailableRefreshRates(m_hwnd);
        if(rates.size()<2)return {};
        return frame_rate_policy::BetterRefreshForSource(SourceCadenceNow(),rates,
                                                         MonitorModeCached().refreshHz,
                                                         FrameGenerationPlanningCap(),
                                                         m_evenCadenceOnly);
    }
    // Changing the preference changes what the next conversion plans, nothing
    // that is already running: a conversion in flight keeps the multiple it was
    // started with, which is also the multiple its output file is named for.
    void SetFrameGenerationPreference(uint32_t generatedFrames){
        const uint32_t clamped=generatedFrames>frame_rate_policy::kPhaseVerifiedMultiFrameCount
            ?frame_rate_policy::kPhaseVerifiedMultiFrameCount:generatedFrames;
        if(m_frameGenPreference==clamped)return;
        m_frameGenPreference=clamped;
        LOG("Generated frames per source frame set to "<<(clamped==0u?std::string("the display's maximum")
                                                                     :std::to_string(clamped))
            <<"; the next conversion plans against it.");
        SaveVideoSettings();SyncFeatureMenuState();UpdateCachedStatus();InvalidateControls();
    }
    // Even cadence only: the rule this player shipped with, kept as a setting
    // for whoever would rather keep the source's own pacing than a finer one
    // that lands unevenly. Off by default, because the unevenness it avoids is
    // one refresh period wide whatever the rate - see FrameRatePolicy.h - while
    // the step it refuses is halved.
    void SetEvenCadenceOnly(bool enabled){
        if(m_evenCadenceOnly==enabled)return;
        m_evenCadenceOnly=enabled;
        LOG("Even cadence only "<<(enabled?"on":"off")<<"; the next conversion plans against it.");
        SaveVideoSettings();SyncFeatureMenuState();UpdateCachedStatus();InvalidateControls();
    }
    // What the status line says about frame generation when the conversion is
    // not the thing that owns the whole line. Every arm is a state the player
    // can actually be in, and each names the feature the way the menu and the
    // pill do rather than as "FG".
    std::wstring FrameGenerationStatus()const{
        const auto ui=FrameGenerationUiNow();
        switch(ui.state){
            case FrameGenerationUiState::Converting:return T(L"framegen.status.generating");
            case FrameGenerationUiState::Stopping:return T(L"framegen.status.stopping");
            case FrameGenerationUiState::Busy:return T(L"framegen.status.busy");
            case FrameGenerationUiState::NoLocalCopy:
                return T(L"framegen.status.off")+L" ("+T(L"framegen.refusal.no_local_copy.short")+L")";
            case FrameGenerationUiState::NeedsSourceCopy:return T(L"framegen.status.needs_copy");
            case FrameGenerationUiState::CopyingSource:return T(L"framegen.status.copying");
            case FrameGenerationUiState::Unchecked:return T(L"framegen.status.ready_unchecked");
            case FrameGenerationUiState::Refused:
                if(const uint32_t beyond=FrameGenerationMultipleBeyondPreference())
                    return T(L"framegen.status.off")+L" ("+
                           Format(T(L"framegen.refusal.preference.short"),m_frameGenPreference+1u,beyond)+L")";
                return T(L"framegen.status.off")+L" ("+FrameGenerationRefusalShort(ui.plan.refusal)+L")";
            case FrameGenerationUiState::Ready:break;
        }
        return T(L"framegen.status.ready")+std::to_wstring(ui.plan.multiplier)+L"\u00d7 \u2192 "+
               FormatFrameRate(ui.plan.targetFps)+L" fps";
    }
    // MEASURED HAZARD, and the reason there is no background probe here: the
    // probe creates a second D3D12 device and an NGX FrameGeneration feature,
    // and running it at load - 0.2 s after the renderer armed its own deferred
    // NGX SuperSampling create - froze presentation on the 19th frame of a
    // 600-frame clip while the decoder kept reading to the end. Two NGX feature
    // creates racing in one process is not a thing this player may do behind
    // the user's back. The admission is measured on the click that needs it,
    // where the work is the user's own and the wait cursor is the honest
    // signal; a conversion started from a settled playing session was measured
    // safe (the probe, the conversion and a live SR feature coexisted) and that
    // is the only shape this code takes.
    void ShowFrameGenerationRefusal(frame_rate_policy::FrameGenerationRefusal refusal){
        // A refusal the setting causes is a different sentence from one the
        // video or the display causes, because the user can act on it.
        if(const uint32_t beyond=FrameGenerationMultipleBeyondPreference()){
            const std::wstring text=Format(T(L"framegen.refusal.preference"),
                                           m_frameGenPreference+1u,beyond)+T(L"framegen.refusal.unchanged");
            LOG("Frame generation refused by the generated-frames preference: set to "
                <<(m_frameGenPreference+1u)<<"x, this display accepts "<<beyond<<"x");
            MessageBoxW(m_hwnd,text.c_str(),T(L"framegen.title").c_str(),MB_OK|MB_ICONINFORMATION);
            return;
        }
        std::wstring text=FrameGenerationRefusalText(refusal);
        if(text.empty())return;
        // The hex NVSDK_NGX_Result and the NGX key names the runtime answered
        // with go to the log, which already carries them. The one dialog a user
        // with an unsupported GPU ever sees was half API names and hex codes and
        // offered no next action - not even the driver update the runtime itself
        // was asking for.
        if(refusal==frame_rate_policy::FrameGenerationRefusal::RuntimeRefused){
            text+=T(L"framegen.driver_next_step");
            if(m_frameGenCapability&&!m_frameGenCapability->detail.empty())
                LOG("Frame generation runtime refusal detail: "<<WideToUtf8(m_frameGenCapability->detail));
        }
        LOG("Frame generation refused: "<<frame_rate_policy::FrameGenerationRefusalName(refusal)
            <<" source="<<m_decoder.FrameRate()<<" fps known="<<m_decoder.FrameRateKnown()
            <<" cfr="<<m_decoder.ConstantFrameRate()<<" refresh="<<MonitorModeCached().refreshHz
            <<" Hz cap="<<FrameGenerationPlanningCap());
        // Only a refusal about the grid has a display-side answer, and when the
        // monitor already offers a mode where the conversion lands on the
        // refresh exactly, that answer is better than anything this player can
        // do to the frames. It becomes the dialog's Yes rather than a second
        // box, and the mode change is where it stops: minutes of GPU work are
        // not something to start from a dialog the user opened to be told no.
        using R=frame_rate_policy::FrameGenerationRefusal;
        if(refusal==R::SourceMeetsRefresh||refusal==R::RefreshBelowDouble||
           refusal==R::SourceCadenceEven||refusal==R::EvenCadenceRequired){
            const auto offer=FrameGenerationRefreshOffer();
            if(offer.Offered()){
                const std::wstring rate=FormatFrameRate(offer.refreshHz);
                const std::wstring prompt=text+Format(T(L"framegen.mode_switch"),rate.c_str(),
                                                      offer.multiplier,
                                                      FormatFrameRate(offer.targetFps).c_str(),
                                                      rate.c_str());
                LOG("Frame generation offering a display mode: "<<offer.refreshHz<<" Hz for x"
                    <<offer.multiplier<<" -> "<<offer.targetFps<<" fps");
                if(MessageBoxW(m_hwnd,prompt.c_str(),T(L"framegen.title").c_str(),
                               MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES)return;
                if(!SetMonitorRefresh(m_hwnd,offer.refreshHz)){
                    LOG("Display mode change to "<<offer.refreshHz<<" Hz was refused by Windows.");
                    MessageBoxW(m_hwnd,T(L"framegen.mode_switch_failed").c_str(),
                                T(L"framegen.title").c_str(),MB_OK|MB_ICONINFORMATION);
                    return;
                }
                InvalidateMonitorMode();
                LOG("Display mode changed for frame generation: refresh is now "
                    <<MonitorModeCached().refreshHz<<" Hz");
                SyncFeatureMenuState();UpdateCachedStatus();InvalidateControls();
                return;
            }
        }
        text+=T(L"framegen.refusal.unchanged");
        MessageBoxW(m_hwnd,text.c_str(),T(L"framegen.title").c_str(),MB_OK|MB_ICONINFORMATION);
    }
    // The window title after a conversion is adopted. It used to gain a
    // lowercase " · neural · 60 fps", which is a fifth spelling of Neural
    // Rendering and never said the video had been converted at all.
    static std::wstring ConvertedTitle(const std::wstring& base,bool neural,double fps){
        if(base.empty())return base;
        return base+(neural?L" \u00b7 generated from the neural render \u00b7 ":L" \u00b7 generated \u00b7 ")+
               FormatFrameRate(fps)+L" fps";
    }
    // The converted file's name. Three things have to be true of it: a user can
    // recognise it in a folder, two different sources can never collide, and
    // the SAME request repeated lands on the same path - that last one is what
    // lets the "already converted" offer exist at all.
    //
    // The title carries recognition and the identity hash carries uniqueness.
    // Neither alone is enough: naming from the playing file's stem made every
    // neural conversion `neural-...mkv` and every acquired stream
    // `source-...mkv`, and for a YouTube stream the playing path is a signed
    // URL whose stem is neither a name nor a legal filename. Two films that
    // share a title stay apart because the hash is over the identity - the page
    // URL for a stream, the absolute path for a file.
    std::wstring FrameGenerationOutputName(bool neuralInput,
                                           const frame_rate_policy::FrameGenerationPlan& plan)const{
        std::wstring label;
        for(const wchar_t character:m_displayTitle){
            if(label.size()>=48)break;
            // Anything a filename may not carry, plus the separators this name
            // uses itself, collapse to one dash rather than vanishing.
            const bool illegal=character<32||wcschr(L"<>:\"/\\|?*.",character)!=nullptr;
            if(illegal||character==L' '){
                if(!label.empty()&&label.back()!=L'-')label.push_back(L'-');
            }else label.push_back(character);
        }
        while(!label.empty()&&label.back()==L'-')label.pop_back();
        if(label.empty())label=L"video";
        const std::wstring identity=m_sourceKind==MediaSourceKind::YouTube&&!m_youtubePageUrl.empty()
            ? m_youtubePageUrl
            : std::filesystem::absolute(std::filesystem::path(m_path)).wstring();
        // FNV-1a over the identity: eight hex digits is plenty to keep two
        // sources apart in one directory, and it is stable across runs so the
        // repeat-request case still finds the earlier file.
        uint64_t hash=1469598103934665603ull;
        for(const wchar_t character:identity){
            hash^=static_cast<uint64_t>(character);
            hash*=1099511628211ull;
        }
        wchar_t suffix[32]{};
        swprintf_s(suffix,L"-%08x-%s%ux%ufps.mkv",static_cast<unsigned>(hash&0xffffffffu),
                   neuralInput?L"neural-":L"",plan.multiplier,
                   static_cast<unsigned>(std::lround(plan.targetFps)));
        return label+suffix;
    }
    // Streaming source, no copy yet: the one unavailable-looking state the user
    // can act on. The acquisition is the SAME one a render of a stream uses -
    // the file lands in the source cache, playback is untouched while it
    // downloads, and the conversion becomes available the moment it completes,
    // because FrameGenerationInputSource then finds it.
    void OfferSourceCopyForFrameGeneration(){
        const std::wstring title=T(L"framegen.title");
        if(MessageBoxW(m_hwnd,T(L"framegen.needs_copy").c_str(),title.c_str(),
                       MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES)return;
        EnsureSourcePrefetch(/*forFrameGeneration=*/true);
        if(!SourcePrefetchActive()){
            LOG("Frame generation could not start the source acquisition for this stream.");
            MessageBoxW(m_hwnd,T(L"framegen.copy_failed").c_str(),title.c_str(),MB_OK|MB_ICONINFORMATION);
            return;
        }
        LOG("Frame generation asked for a local copy of this stream; acquisition started.");
        // No "it started" box: the status line already says the copy is being
        // kept and the pill reads Copying, and a second modal here stops the
        // tick - which means it pauses the video the user is watching.
        SyncFeatureMenuState();UpdateCachedStatus();InvalidateControls();
    }
    void StartFrameGeneration(){
        const std::wstring title=T(L"framegen.title");
        auto ui=FrameGenerationUiNow();
        if(ui.state==FrameGenerationUiState::Refused){ShowFrameGenerationRefusal(ui.plan.refusal);return;}
        // The streaming case is an offer, not a refusal: the pass converts a
        // file, and the player can keep one with the same acquisition a render
        // uses. Every YouTube video used to dead-end here.
        if(ui.state==FrameGenerationUiState::NeedsSourceCopy){OfferSourceCopyForFrameGeneration();return;}
        // Busy, NoLocalCopy, Converting and Stopping are states the menu item
        // and the pill are disabled in, so only a stale click or a keyboard
        // route arrives here and doing nothing is the whole correct behaviour.
        // Unchecked MUST pass: it is the state of every first click, because
        // the runtime's admission is measured below and nothing can be Ready
        // before that measurement exists. Gating it out made the first click on
        // a fresh process do nothing at all.
        if(ui.state!=FrameGenerationUiState::Ready&&ui.state!=FrameGenerationUiState::Unchecked)return;
        if(!m_frameGenCapability){
            // Measured here, inside the user's own action, with the cursor that
            // says so: a background probe beside the renderer froze playback.
            const HCURSOR previous=SetCursor(LoadCursorW(nullptr,IDC_WAIT));
            m_frameGenCapability=QueryFrameGenerationCapability();
            SetCursor(previous);
            LOG("Frame generation capability measured on demand: available="<<m_frameGenCapability->available
                <<" multiFrameCountMax="<<m_frameGenCapability->multiFrameCountMax
                <<" detail="<<WideToUtf8(m_frameGenCapability->detail));
            ui=FrameGenerationUiNow();
            if(ui.state==FrameGenerationUiState::Refused){ShowFrameGenerationRefusal(ui.plan.refusal);return;}
            if(ui.state!=FrameGenerationUiState::Ready)return;
        }
        const frame_rate_policy::FrameGenerationPlan plan=ui.plan;
        const FrameGenerationInput input=FrameGenerationInputSource();
        // The converted file is a playback artifact, not an export: it goes
        // where the other derived carriers go. Writing it beside the user's
        // source - which the first version did - drops a large MKV into their
        // library without a save dialog, lands in the acquired-copy directory
        // for a network source, and simply fails on a read-only share.
        NeuralCacheManager cache(m_cacheRoot);
        if(!cache.Valid()){
            MessageBoxW(m_hwnd,CacheFailureText().DescribeRoot(cache.LastFailure()).c_str(),title.c_str(),MB_OK|MB_ICONERROR);
            return;
        }
        const std::filesystem::path outputDirectory=cache.Root()/L"frame-generation";
        std::error_code directoryError;
        std::filesystem::create_directories(outputDirectory,directoryError);
        if(directoryError){
            LOG("Frame generation directory could not be created: "<<directoryError.message());
            MessageBoxW(m_hwnd,T(L"framegen.cache_failed").c_str(),title.c_str(),MB_OK|MB_ICONERROR);
            return;
        }
        const std::filesystem::path output=outputDirectory/FrameGenerationOutputName(input.neural,plan);
        std::error_code existsError;
        if(std::filesystem::is_regular_file(output,existsError)&&!existsError){
            // Minutes of GPU work and a large file already exist. Converting
            // again REPLACES that file, which is the only reason this is a
            // three-answer question rather than a confirmation.
            const std::wstring text=Format(T(L"framegen.exists"),output.wstring().c_str());
            const int answer=MessageBoxW(m_hwnd,text.c_str(),title.c_str(),MB_YESNOCANCEL|MB_ICONQUESTION);
            if(answer==IDCANCEL)return;
            if(answer==IDYES){
                LOG("Frame generation offer accepted an existing file: "<<WideToUtf8(output.wstring()));
                AdoptConvertedFile(output,m_displayTitle,input.neural,plan.targetFps);
                return;
            }
        }
        std::wstring prompt=Format(T(L"framegen.confirm"),plan.multiplier,
                                   FormatFrameRate(m_decoder.FrameRate()).c_str(),
                                   FormatFrameRate(plan.targetFps).c_str());
        // An uneven grid here is the one the source is already playing in - the
        // two hold lengths are one scan-out apart either way, which is why this
        // is a conversion the policy allows - but a viewer paying minutes of GPU
        // time for it is owed the fact rather than left to see it.
        if(!plan.cadence.even)
            prompt+=Format(T(L"framegen.confirm.uneven"),plan.cadence.shortHold,plan.cadence.longHold);
        if(input.neural)prompt+=T(L"framegen.confirm.neural");
        LOG("Frame generation offer: output="<<WideToUtf8(output.wstring())
            <<" input="<<(input.neural?"neural":"original"));
        // Yes/No with No focused, like every other consequential confirmation in
        // this player. OK/Cancel with OK focused put the most expensive action
        // in the product one stray Enter away.
        if(MessageBoxW(m_hwnd,prompt.c_str(),title.c_str(),MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES)return;

        FrameGenerationRequest request{};
        request.source=input.path;
        // Always the original, even when the frames come from the neural
        // carrier: this player writes those carriers video-only, so the audio,
        // subtitles and chapters have to come from the file they were rendered
        // from. The carrier only qualifies when it covers the whole source, so
        // the two are the same length and the copy stays a copy. For a stream
        // that is the acquired copy and never the signed URL - see
        // FrameGenerationStreamSource.
        request.streamSource=FrameGenerationStreamSource();
        request.output=output;
        request.multiplier=plan.multiplier;
        request.nvencPreset=m_nvencPreset;
        m_frameGenProgress={};
        m_frameGenMultiplier=plan.multiplier;
        m_frameGenTargetFps=plan.targetFps;
        m_frameGenCancelling=false;
        m_frameGenStarted=Clock::now();
        const auto helpers=ExecutableDirectory();HWND target=m_hwnd;
        auto* completions=&m_frameGenCompletions;auto* progressMessages=&m_frameGenProgressMessages;
        const std::wstring displayTitle=m_displayTitle;
        const bool neuralInput=input.neural;
        try{
            m_frameGenWorker=std::jthread([target,request,helpers,completions,progressMessages,displayTitle,neuralInput](std::stop_token stop){
                auto completion=std::make_unique<FrameGenerationCompletion>();
                completion->output=request.output;
                completion->multiplier=request.multiplier;
                completion->source=request.source;
                completion->neuralInput=neuralInput;
                completion->title=displayTitle;
                completion->result=FrameGenerationPass(helpers).Run(request,stop,
                    [&](const FrameGenerationProgress& progress){
                        auto message=std::make_unique<FrameGenerationProgressMessage>();
                        message->progress=progress;
                        progressMessages->RegisterAndPost(std::move(message),[&](uint64_t token){
                            return PostMessageW(target,WM_FRAMEGEN_PROGRESS,static_cast<WPARAM>(token),0)!=FALSE;});
                    });
                completions->RegisterAndPost(std::move(completion),[&](uint64_t token){
                    return PostMessageW(target,WM_FRAMEGEN_COMPLETE,static_cast<WPARAM>(token),0)!=FALSE;});
            });
        }catch(const std::system_error&){
            MessageBoxW(m_hwnd,T(L"framegen.worker_failed").c_str(),title.c_str(),MB_OK|MB_ICONERROR);
            m_frameGenMultiplier=0;return;
        }
        LOG("Frame generation started: "<<m_decoder.FrameRate()<<" fps x"<<plan.multiplier<<" -> "<<plan.targetFps
            <<" fps, presents/frame="<<plan.presentsPerFrame<<", output="<<WideToUtf8(output.wstring()));
        SetTaskbarProgress(0.0);
        SyncFeatureMenuState();UpdateCachedStatus();InvalidateControls();
    }
    void CancelFrameGeneration(){
        if(!m_frameGenWorker.joinable()){
            m_frameGenProgressMessages.Clear();m_frameGenCompletions.Clear();m_frameGenMultiplier=0;return;
        }
        // Request and return. The join used to run here, on the UI thread, from
        // HandleCommand - so the window stopped painting until the pass noticed
        // the token. The completion message retires the thread instead, and the
        // status line reads "Stopping frame generation" until it arrives.
        m_frameGenWorker.request_stop();
        m_frameGenCancelling=true;
        if(m_hwnd&&IsWindow(m_hwnd)){SyncFeatureMenuState();UpdateCachedStatus();InvalidateControls();}
    }
    void CompleteFrameGenerationProgress(uint64_t token){
        auto message=m_frameGenProgressMessages.Take(token);
        if(!message||!m_frameGenWorker.joinable())return;
        m_frameGenProgress=message->progress;
        if(m_frameGenProgress.sourceFramesTotal>0)
            SetTaskbarProgress(double(m_frameGenProgress.sourceFramesRead)/
                               double(m_frameGenProgress.sourceFramesTotal));
        UpdateCachedStatus();InvalidateControls();
    }
    void CompleteFrameGeneration(uint64_t token){
        auto completion=m_frameGenCompletions.Take(token);
        if(!completion)return;
        if(m_frameGenWorker.joinable()){m_frameGenWorker.join();m_frameGenWorker=std::jthread{};}
        m_frameGenMultiplier=0;m_frameGenCancelling=false;
        SetTaskbarProgress(std::nullopt);
        const std::wstring title=T(L"framegen.title");
        SyncFeatureMenuState();UpdateCachedStatus();InvalidateControls();
        LOG("Frame generation finished: ok="<<completion->result.ok
            <<" frames="<<completion->result.framesWritten<<" generated="<<completion->result.generatedFrames
            <<" outputFps="<<completion->result.outputFps
            <<" audio="<<completion->result.audioCarried<<" audioStreams="<<completion->result.outputAudioStreams
            <<" subtitleStreams="<<completion->result.outputSubtitleStreams
            <<" detail="<<WideToUtf8(completion->result.detail));
        if(completion->result.error==FrameGenerationError::Cancelled)return;
        if(!completion->result.ok){
            // The pass removes its output and its staging file on every failure
            // path, so "nothing was written" is a fact rather than a comfort.
            // The runtime's own sentence - "DLSS-G evaluate 1 of 1 failed after
            // 412 source frames: 0x..." - is in the log above, where an API
            // trace belongs.
            MessageBoxW(m_hwnd,T(L"framegen.failed").c_str(),title.c_str(),MB_OK|MB_ICONERROR);
            return;
        }
        // The file outlives this dialog, so the command that opens it is enabled
        // from here. Before this, the path was named once in a message box and
        // then unreachable from every surface in the player.
        m_frameGenLastOutput=completion->output;
        SyncFeatureMenuState();
        // "Then start playing" - but only when the conversion is still about
        // what is on screen. Nothing stops the user opening another video while
        // this ran, and taking playback away from that one minutes later would
        // be the player deciding what they are watching. Compared against both
        // of the current file's carriers rather than the view-dependent pick,
        // because pressing D while waiting does not mean the user left.
        if(completion->source!=m_path&&completion->source!=m_neuralPath.wstring()){
            LOG("Frame generation result not adopted: the player has moved to another source.");
            MessageBoxW(m_hwnd,T(L"framegen.finished_elsewhere").c_str(),title.c_str(),MB_OK|MB_ICONINFORMATION);
            return;
        }
        AdoptConvertedFile(completion->output,completion->title,completion->neuralInput,
                           completion->result.outputFps);
    }
    // The ONE way a converted file becomes what is playing, used by the
    // completion and by the "play the file you already made" offer. Two call
    // sites meant two rule sets: the offer recorded the cache path in Recent
    // videos and dropped the playhead, while the completion did neither.
    void AdoptConvertedFile(const std::filesystem::path& file,const std::wstring& baseTitle,
                            bool neuralInput,double outputFps){
        // Resume where the user was: the conversion preserves duration exactly,
        // so the same second is the same picture, and a minutes-long job that
        // snaps a film back to 0:00 is the one thing they would notice.
        const double resumeSeconds=m_currentSec;
        m_frameGenLastOutput=file;
        // recordRecent=false: this is a derived carrier in the cache, and a
        // Recent entry for it dangles the moment the cache is cleared.
        if(!LoadOriginal(file.wstring(),ConvertedTitle(baseTitle,neuralInput,outputFps),
                         MediaSourceKind::LocalFile,false,/*recordRecent=*/false)){
            SyncFeatureMenuState();return;
        }
        if(resumeSeconds>1.0)RequestSeek(resumeSeconds,true);
        SyncFeatureMenuState();
    }
    void ShowFrameGenerationOutput(){
        if(m_frameGenLastOutput.empty())return;
        std::error_code error;
        if(!std::filesystem::is_regular_file(m_frameGenLastOutput,error)||error){
            MessageBoxW(m_hwnd,T(L"framegen.reveal_failed").c_str(),T(L"framegen.title").c_str(),
                        MB_OK|MB_ICONINFORMATION);
            m_frameGenLastOutput.clear();SyncFeatureMenuState();return;
        }
        // /select, takes one quoted path; a default cache path contains spaces.
        const std::wstring arguments=L"/select,\""+m_frameGenLastOutput.wstring()+L"\"";
        ShellExecuteW(m_hwnd,L"open",L"explorer.exe",arguments.c_str(),nullptr,SW_SHOWNORMAL);
    }
    // Taskbar progress for the one job that lasts minutes, because in
    // fullscreen with the controls auto-hidden, and whenever the window is
    // minimised, the status strip is the only place progress is shown and none
    // of it is on screen.
    //
    // MEASURED, so the next reader does not chase it: every call here returns
    // S_OK on Windows 11 26200 - CoCreateInstance, HrInit, SetProgressState and
    // SetProgressValue - and no green fill appeared on this machine's taskbar
    // button at any point of a 33 s conversion. The shell's rendering is not
    // something this code can force, the calls are the documented ones, and
    // they cost nothing on a configuration that does draw them; the log line
    // below is what says the shell accepted them.
    void SetTaskbarProgress(std::optional<double> fraction){
        if(!m_hwnd)return;
        if(!m_taskbar&&!m_taskbarUnavailable){
            const HRESULT created=CoCreateInstance(CLSID_TaskbarList,nullptr,CLSCTX_INPROC_SERVER,
                                                   IID_PPV_ARGS(&m_taskbar));
            const HRESULT initialized=(SUCCEEDED(created)&&m_taskbar)?m_taskbar->HrInit():created;
            LOG("Taskbar progress: CoCreateInstance="<<HexText(created)<<" HrInit="<<HexText(initialized));
            if(FAILED(created)||!m_taskbar||FAILED(initialized)){m_taskbar.Reset();m_taskbarUnavailable=true;return;}
        }
        if(!m_taskbar)return;
        if(!fraction){m_taskbar->SetProgressState(m_hwnd,TBPF_NOPROGRESS);return;}
        const HRESULT state=m_taskbar->SetProgressState(m_hwnd,TBPF_NORMAL);
        const HRESULT value=m_taskbar->SetProgressValue(m_hwnd,
            static_cast<ULONGLONG>(std::clamp(*fraction,0.0,1.0)*1000.0),1000ull);
        if(!m_taskbarReported){
            m_taskbarReported=true;
            LOG("Taskbar progress first update: SetProgressState="<<HexText(state)<<" SetProgressValue="
                <<HexText(value)<<" fraction="<<*fraction);
        }
    }

    // Update notice. The check runs at most once a day in the background, keeps
    // its answer in the INI beside the player, and never blocks startup: the
    // badge appears when the reply arrives.
    static int64_t UnixNow(){
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }
    std::string ReadIniNarrow(const wchar_t* section,const wchar_t* key)const{
        wchar_t buffer[128]{};
        const DWORD length=GetPrivateProfileStringW(section,key,L"",buffer,static_cast<DWORD>(std::size(buffer)),SettingsPath().c_str());
        return WideToUtf8(std::wstring_view(buffer,length));
    }
    int64_t ReadIniInt64(const wchar_t* section,const wchar_t* key)const{
        wchar_t buffer[64]{};
        GetPrivateProfileStringW(section,key,L"0",buffer,static_cast<DWORD>(std::size(buffer)),SettingsPath().c_str());
        wchar_t* end=nullptr;
        const long long value=wcstoll(buffer,&end,10);
        return (end&&end!=buffer)?static_cast<int64_t>(value):0;
    }
    void LoadUpdateSettings(){
        m_updateChecksEnabled=GetPrivateProfileIntW(L"Updates",L"Enabled",1,SettingsPath().c_str())!=0;
        m_updateLastChecked=ReadIniInt64(L"Updates",L"LastCheckedUnix");
        m_updateLatestTag=ReadIniNarrow(L"Updates",L"LatestTag");
        m_updateDismissedTag=ReadIniNarrow(L"Updates",L"DismissedTag");
    }
    void SaveUpdateSettings()const{
        const auto path=SettingsPath();
        WritePrivateProfileStringW(L"Updates",L"Enabled",m_updateChecksEnabled?L"1":L"0",path.c_str());
        WritePrivateProfileStringW(L"Updates",L"LastCheckedUnix",std::to_wstring(m_updateLastChecked).c_str(),path.c_str());
        WritePrivateProfileStringW(L"Updates",L"LatestTag",Utf8ToWide(m_updateLatestTag).c_str(),path.c_str());
        WritePrivateProfileStringW(L"Updates",L"DismissedTag",Utf8ToWide(m_updateDismissedTag).c_str(),path.c_str());
    }
    void MaybeStartUpdateCheck(bool userRequested){
        if(!m_hwnd||m_updateWorker.joinable())return;
        const UpdateCheckDecision decision=userRequested
            ?UpdateCheckDecision::Fetch
            :DecideUpdateCheck(m_updateChecksEnabled,m_updateLastChecked,UnixNow(),kUpdateCheckIntervalSeconds);
        if(decision==UpdateCheckDecision::Disabled)return;
        if(decision==UpdateCheckDecision::UseCache){ApplyUpdateNotice(false,true);return;}
        // An explicit check is also a request to stop hiding a dismissed release.
        if(userRequested){m_updateDismissedTag.clear();SaveUpdateSettings();}
        HWND target=m_hwnd;auto* completions=&m_updateCompletions;
        try{
            m_updateWorker=std::jthread([target,completions,userRequested](std::stop_token stop){
                auto completion=std::make_unique<UpdateCheckCompletion>();
                completion->userRequested=userRequested;
                completion->fetch=FetchLatestReleaseTag(stop);
                completions->RegisterAndPost(std::move(completion),[&](uint64_t token){
                    return PostMessageW(target,WM_UPDATE_CHECKED,static_cast<WPARAM>(token),0)!=FALSE;});
            });
        }catch(const std::system_error&){LOG("The update check worker could not start; continuing without it.");}
    }
    void CancelUpdateCheck(){if(m_updateWorker.joinable()){m_updateWorker.request_stop();m_updateWorker.join();m_updateWorker=std::jthread{};}m_updateCompletions.Clear();}
    void CompleteUpdateCheck(uint64_t token){
        auto completion=m_updateCompletions.Take(token);if(!completion)return;
        if(m_updateWorker.joinable()){m_updateWorker.join();m_updateWorker=std::jthread{};}
        if(completion->fetch.ok){
            m_updateLatestTag=completion->fetch.tag;
            m_updateLastChecked=UnixNow();
            SaveUpdateSettings();
            LOG("Update check: latest release "<<m_updateLatestTag<<"; this build is "<<DLSS_VIDEO_PLAYER_VERSION);
        }else if(!completion->fetch.error.empty()){
            LOG("Update check failed: "<<WideToUtf8(completion->fetch.error));
        }
        ApplyUpdateNotice(completion->userRequested,completion->fetch.ok);
    }
    void ApplyUpdateNotice(bool announce,bool reachable){
        m_updateNotice=EvaluateUpdateNotice(DLSS_VIDEO_PLAYER_VERSION,m_updateLatestTag,m_updateDismissedTag);
        if(HMENU bar=GetMenu(m_hwnd)){
            const std::wstring label=m_updateNotice?T(L"update.badge")+FormatSemanticVersion(m_updateNotice->latest):std::wstring();
            if(app_menu::SetUpdateBadge(bar,label))DrawMenuBar(m_hwnd);
        }
        if(!announce)return;
        const std::wstring caption=T(L"app.title");
        if(m_updateNotice){
            const std::wstring message=T(L"update.available")+FormatSemanticVersion(m_updateNotice->latest)+T(L"update.open_question");
            if(MessageBoxW(m_hwnd,message.c_str(),caption.c_str(),MB_YESNO|MB_ICONINFORMATION)==IDYES)OpenReleasesPage();
            return;
        }
        const std::wstring message=reachable
            ?T(L"update.up_to_date")+Utf8ToWide(DLSS_VIDEO_PLAYER_VERSION)
            :T(L"update.unreachable");
        MessageBoxW(m_hwnd,message.c_str(),caption.c_str(),MB_OK|(reachable?MB_ICONINFORMATION:MB_ICONWARNING));
    }
    void OpenReleasesPage(){
        const std::wstring url(kUpdateReleasesPageUrl);
        const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(m_hwnd,L"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL));
        if(result<=32)LOG("Opening the releases page failed: code="<<result);
    }
    // The badge is a one-shot reminder: opening the page retires this release
    // so the bar goes quiet again until the next one ships.
    void ActivateUpdateBadge(){
        OpenReleasesPage();
        if(!m_updateNotice)return;
        m_updateDismissedTag=m_updateNotice->tag;
        SaveUpdateSettings();
        ApplyUpdateNotice(false,true);
    }
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
        if(JobBehindPlayback()){RefreshBufferOverlay();const RECT dirty=StatusRect();InvalidateRect(m_hwnd,&dirty,FALSE);
            if(m_liveSession&&m_activityMotionEnabled){RECT timeline=TimelineRect();InflateRect(&timeline,0,Dip(2));InvalidateRect(m_hwnd,&timeline,FALSE);}return;}
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
    // printf into a std::wstring, measured first. The frame-generation
    // confirmation used a wchar_t[768] and swprintf_s, which a long cache path
    // turns into the secure-CRT invalid-parameter handler instead of a prompt.
    template<class... Args>
    static std::wstring Format(const std::wstring& format,Args... arguments){
        const int length=_scwprintf(format.c_str(),arguments...);
        if(length<=0)return format;
        std::wstring text(static_cast<size_t>(length),L'\0');
        swprintf_s(text.data(),text.size()+1,format.c_str(),arguments...);
        return text;
    }
    // Empty unless this machine's driver is below the neural floor. Built here
    // so the render thread never touches the localizer.
    std::wstring NeuralDriverNoticeText()const{
        if(ClassifyNeuralDriver(m_opt.detectedGpu.driverVersion)!=NeuralDriverSupport::BelowFloor)return {};
        std::wstring detected=m_opt.detectedGpu.driverVersion;
        if(const auto parsed=ParseNvidiaDriverVersion(m_opt.detectedGpu.driverVersion))detected=FormatNvidiaDriverVersion(*parsed);
        return T(L"driver.below_floor")+L"\n\n"+T(L"driver.detected")+detected+L"\n"+
               T(L"driver.minimum")+FormatNvidiaDriverVersion(kNeuralDriverFloor)+L"\n"+
               T(L"driver.verified")+FormatNvidiaDriverVersion(kNeuralDriverRecommended);
    }
    // Built on this thread for the same reason as the driver notice above.
    NeuralCacheFailureText CacheFailureText()const{
        return {T(L"cache.staging_failed"),T(L"cache.not_writable"),T(L"cache.cause.unwritable"),
                T(L"cache.cause.invalid_key"),T(L"cache.cause.create_failed"),
                T(L"cache.cause.exists"),T(L"cache.cause.outside_root")};
    }
    AudioPlayer& Audio(){return m_networkAudio?*m_networkAudio:m_audio;}
    const AudioPlayer& Audio()const{return m_networkAudio?*m_networkAudio:m_audio;}
    // How often playback says how it is doing. A session can currently drop two
    // frames in three for minutes and write NOTHING: m_submitFps and
    // m_droppedFrames reach the status bar and stop there, so a report of
    // "only some frames are showing" arrives with no record of whether frames
    // were late, how late, or what else was using the GPU at the time. That is
    // the same blindness ReadNextCachedFrame's stall bound was added for.
    //
    // Two seconds, and only while a neural pair is on screen. A line per
    // presented frame would be 60 disk writes a second - see the 14,000-line
    // incident behind the memoised source-key lookup - and the question this
    // answers is a trend, not an event.
    static constexpr double kPlaybackHealthSeconds=2.0;
    void ReportPlaybackHealth(){
        if(!m_loaded||!m_playing){m_playbackHealthAt={};return;}
        const auto now=Clock::now();
        if(m_playbackHealthAt==Clock::time_point{}){
            m_playbackHealthAt=now;m_playbackHealthDropped=m_droppedFrames;
            m_playbackHealthPresented=m_cachedPresentedFrames;return;
        }
        const double elapsed=std::chrono::duration<double>(now-m_playbackHealthAt).count();
        if(elapsed<kPlaybackHealthSeconds)return;
        // Loading a source zeroes m_droppedFrames while this still holds the
        // previous session's total, and an unsigned subtraction there would
        // print a number near 2^64 rather than a small one.
        const uint64_t dropped=m_droppedFrames>=m_playbackHealthDropped
            ? m_droppedFrames-m_playbackHealthDropped : m_droppedFrames;
        const uint64_t presented=m_cachedPresentedFrames>=m_playbackHealthPresented
            ? m_cachedPresentedFrames-m_playbackHealthPresented : m_cachedPresentedFrames;
        const double presentedFps=elapsed>0.0?double(presented)/elapsed:0.0;
        const double sourceFps=m_decoder.FrameRate();
        // Presented against what the source asks for: the ratio is the symptom a
        // viewer describes, and the budget beside it is what a frame had to fit
        // into to avoid being dropped.
        LOG("Playback health: presented="<<presentedFps<<" fps of "<<sourceFps
            <<" source fps over the last "<<elapsed<<" s; dropped="<<dropped
            <<" (total "<<m_droppedFrames<<") presented_total="<<m_cachedPresentedFrames
            <<" at "<<Position()<<" s; budget="<<(sourceFps>0.0?1000.0/sourceFps:0.0)
            <<" ms/frame; guide="<<(m_renderMsFrames?m_guideMsTotal/double(m_renderMsFrames):0.0)
            <<" ms present="<<(m_renderMsFrames?m_renderMsTotal/double(m_renderMsFrames):0.0)
            <<" ms over "<<m_renderMsFrames<<" frames; stride=1in"<<m_presentStride<<" live="<<m_liveSession
            <<" rendering="<<NeuralJobActive()<<" upscaling="<<UpscalingActive());
        // An interval with no re-anchor in it is the session recovering, and
        // the limit exists to catch one that never does.
        m_guideMsTotal=0.0;m_renderMsTotal=0.0;m_renderMsFrames=0;
        if(!m_cadenceReanchoredRecently)m_cadenceReanchors=0;
        m_cadenceReanchoredRecently=false;
        m_playbackHealthAt=now;m_playbackHealthDropped=m_droppedFrames;
        m_playbackHealthPresented=m_cachedPresentedFrames;
    }
    bool ReadNextCachedFrame(){
        // Before the read, because the stall window below has to be told that
        // reads stopped happening at all - see kPairStallGapSeconds.
        const auto attempt=Clock::now();
        if(m_pairStallRead==Clock::time_point{}||
           std::chrono::duration<double>(attempt-m_pairStallRead).count()>kPairStallGapSeconds)
            m_pairStall={};
        m_pairStallRead=attempt;
        const auto read=m_synchronizedPlayback.ReadNextAvailable();
        if(read==SynchronizedReadResult::PairReady){
            m_pairStall={};
            m_nextPairFrame=VisiblePairFrame();if(!m_nextPairFrame)return false;
            m_haveNext=true;return true;
        }
        // NotReady is a decoder warming up: a segment source is reopened at
        // every boundary and after every seek, and that takes a few frames. It
        // is also the one result with no owner - Tick just returns - so when it
        // does NOT clear, playback sits on the frame it last presented and says
        // nothing. That is the "video shows a static frame" report: 1440p120
        // pairs, everything rendered, and no line in the log to say what the
        // pair was waiting for. Bound it, name the fault once, and take the same
        // way out that unrendered video takes.
        if(read==SynchronizedReadResult::NotReady){
            if(m_pairStall==Clock::time_point{}){m_pairStall=attempt;return false;}
            const double stalled=std::chrono::duration<double>(attempt-m_pairStall).count();
            if(stalled<kPairStallSeconds)return false;
            const std::string fault=m_synchronizedPlayback.LastFault();
            LOG("Neural playback has had no pair for "<<stalled<<" s at "<<Position()<<" s; fault="
                <<(fault.empty()?std::string("none"):fault)<<" regions="<<LiveCoverage().size()
                <<" head="<<LiveHeadSeconds()<<" s live="<<m_liveSession);
            m_pairStall={};
            if(m_liveSession){
                const bool wasPlaying=m_playing;const double handBackAt=Position();
                DetachLivePlayback();
                AdoptAcquiredSourceCopyForPlayback();
                RequestSeek(handBackAt,wasPlaying);
                return false;
            }
            // A finished render being watched has no session to hand back to, so
            // this is a decode failure in everything but name and stops the same
            // way one does - with the reason on the status bar, not a modal.
            m_haveNext=false;m_playing=false;Audio().Pause(true);
            m_neuralNotice=T(L"neural.sync.warning");
            UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();
            return false;
        }
        m_pairStall={};
        // The playhead is on video nobody has rendered. When a job is filling
        // exactly this hole its frames are seconds away, so waiting shows the
        // picture the user asked for. Anywhere else - a seek back in front of the
        // render, a hole the session has not reached, or a target no job is
        // working on any more because the session gave up filling holes - waiting
        // would freeze playback for as long as a render takes, or for good. The
        // original comes back there and the session re-attaches when its coverage
        // reaches the playhead.
        if(read==SynchronizedReadResult::WaitingForRender){
            const int64_t at=static_cast<int64_t>(std::llround(Position()*1e7));
            const bool beingFilled=NeuralJobActive()&&at>=m_liveTarget.start100ns&&at<m_liveTarget.end100ns;
            if(m_liveSession&&!beingFilled){
                // Both read before the detach and the swap below: the swap stops
                // the audio that Position() reads while playing, after which it
                // falls back to the play-start clock and answers for the frame the
                // last attach began on, not for where the viewer is.
                const bool wasPlaying=m_playing;const double handBackAt=Position();
                LOG("Live playback reached unrendered video at "<<handBackAt<<" s, outside the render target ["
                    <<double(m_liveTarget.start100ns)*1e-7<<","<<double(m_liveTarget.end100ns)*1e-7
                    <<") s; playing the original there.");
                DetachLivePlayback();
                // Before the seek, not after: this is what decides whether the
                // seek is a local one or a re-resolution of the stream.
                AdoptAcquiredSourceCopyForPlayback();
                RequestSeek(handBackAt,wasPlaying);
                return false;
            }
            EnterLiveBuffering();return false;
        }
        if(read==SynchronizedReadResult::OutOfSync||read==SynchronizedReadResult::Error){
            LOG((read==SynchronizedReadResult::OutOfSync?"Neural playback out of sync: ":"Neural playback decode error: ")
                <<m_synchronizedPlayback.LastFault()<<"; position="<<Position()<<" presented="<<m_cachedPresentedFrames
                <<" dropped="<<m_droppedFrames);
            const std::wstring message=T(read==SynchronizedReadResult::OutOfSync?L"neural.sync.warning":L"error.decode");
            // An active session's own segments are what broke, and this runs
            // inside Tick: a modal here pumps messages under a live frame, so
            // the job's completion arrived through it and tore the session
            // down - renderer included - beneath the caller. The session ends
            // and the original takes the same frame back, with the reason in
            // the status bar; the box came back on every Play until it did.
            if(m_liveSession){
                StopLiveNeuralSession(true);
                m_neuralNotice=message;
                UpdateCachedStatus();InvalidateControls();
                return false;
            }
            m_haveNext=false;m_playing=false;Audio().Pause(true);
            const std::wstring caption=T(L"app.title");
            MessageBoxW(m_hwnd,message.c_str(),caption.c_str(),MB_OK|MB_ICONERROR);
            InvalidateControls();InvalidatePlaybackProgress();return false;
        }
        m_haveNext=false;m_playing=false;Audio().Pause(true);if(read==SynchronizedReadResult::EndOfStream)LOG("Cached playback completed: presented="<<m_cachedPresentedFrames<<" dropped="<<m_droppedFrames);
        InvalidateControls();InvalidatePlaybackProgress();return false;
    }
    // One step of the presentation cadence for a neural pair. Returns false
    // when this tick is finished - the pair was skipped to hold the cadence, or
    // the position was re-anchored - and true when the caller should present
    // the frame it is holding.
    //
    // Skipping still DECODES the pair: stride removes the presentation work and
    // nothing else. That is the honest limit of this lever and the reason
    // re-anchoring exists beside it.
    bool CadenceAdvanceCachedFrame(double now,double frameDur){
        const double due=double(NextFrame().timestamp100ns)*1e-7;
        // Not due yet is not late; the ordinary wait below handles it.
        if(now+0.001<due)return true;
        const auto decision=playback_cadence::Decide(now-due,frameDur,m_presentStride,m_presentPhase);
        if(decision.stride!=m_presentStride){
            LOG("Neural playback cadence "<<(decision.stride>m_presentStride?"widened":"narrowed")
                <<" to 1 in "<<decision.stride<<" at "<<now<<" s; behind="<<(now-due)*1000.0
                <<" ms of a "<<frameDur*1000.0<<" ms frame.");
            m_presentStride=decision.stride;
        }
        if(decision.action==playback_cadence::Action::Reanchor){
            // Walking this gap costs a pair decode per frame; a seek costs about
            // half a second whatever the distance. Bounded, because a session
            // that keeps arriving late is one this machine cannot follow, and
            // saying so beats a picture that hitches every second for ever.
            ++m_cadenceReanchors;m_cadenceReanchoredRecently=true;m_presentPhase=0;
            LOG("Neural playback is "<<(now-due)<<" s behind the clock at "<<now
                <<" s; re-anchoring rather than decoding every frame in between (attempt "
                <<m_cadenceReanchors<<" of "<<kCadenceReanchorLimit<<").");
            if(m_cadenceReanchors>=kCadenceReanchorLimit){
                LOG("Neural playback cannot follow this source live; playing the original.");
                const bool wasPlaying=m_playing;const double handBackAt=Position();
                m_cadenceReanchors=0;m_presentStride=1;m_presentPhase=0;
                if(m_liveSession){
                    DetachLivePlayback();
                    AdoptAcquiredSourceCopyForPlayback();
                    RequestSeek(handBackAt,wasPlaying);
                }else{
                    m_haveNext=false;m_playing=false;Audio().Pause(true);
                    m_neuralNotice=T(L"neural.cadence.cannot_follow");
                    UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();
                }
                return false;
            }
            m_presentPhase=0;
            RequestSeek(now,m_playing);
            return false;
        }
        // Advanced for every pair consumed, whichever way the decision went.
        // Resetting it on a present - which is what this did first - made
        // (phase % stride) true on every frame, so the stride widened all the
        // way to its cap and skipped nothing: measured stride=1in8 with
        // dropped=0, presenting every pair it managed to read.
        m_presentPhase=playback_cadence::NextPhase(m_presentPhase,m_presentStride);
        if(decision.action==playback_cadence::Action::Skip){
            // Counted as dropped because that is what it is from the viewer's
            // side, but NOT a history reset: a stride is a regular decimation,
            // the motion between two presented frames is a coherent two frames'
            // worth, and RenderVideoFrame already hands DLSS the real timestamp
            // delta. Resetting here would be the documented over-reset failure.
            ++m_droppedFrames;
            m_currentSec=due;m_haveNext=false;m_nextPairFrame.reset();
            InvalidatePlaybackProgress();
            return false;
        }
        // BUG 2 was here: clearing the re-anchor count on any present meant a
        // session that presented one frame between anchors never reached the
        // limit - the log read "attempt 1 of 3" six times in a row while the
        // picture hitched every 1.7 s. The count is cleared by a QUIET
        // interval instead; see ReportPlaybackHealth.
        return true;
    }
    // The member of the current pair on screen, sharing the pair rather than
    // copying the frame out of it.
    std::shared_ptr<const VideoFrame> VisiblePairFrame()const{
        auto pair=m_synchronizedPlayback.CurrentPairShared();
        const VideoFrame* visible=m_synchronizedPlayback.VisibleFrame();
        if(!pair||!visible)return {};
        return std::shared_ptr<const VideoFrame>(std::move(pair),visible);
    }
    // What Tick presents next: the pair member on a synchronized pair, the
    // decoder's buffer otherwise.
    const VideoFrame& NextFrame()const{return m_nextPairFrame?*m_nextPairFrame:m_next;}
    bool HaveLastPlaybackFrame()const{return m_lastPlaybackFrame&&!m_lastPlaybackFrame->bgra.empty();}
    // Keeps what was just rendered for a re-render (an upscaling toggle, a
    // rebuilt renderer). It was a copy on every presented frame, and on a pair
    // `m_next` had already been copied out of it: two full frames, about 11 MB
    // per pair at 1440p. A pair member is shared with its pair, the decoder's
    // buffer is taken by swap - Tick reads the next frame into `m_next` straight
    // after, handing the decoder the old buffer - and anything else is copied.
    void RememberPlaybackFrame(const VideoFrame& f){
        if(m_lastPlaybackFrame.get()==&f)return;
        if(m_nextPairFrame.get()==&f){m_lastPlaybackFrame=m_nextPairFrame;return;}
        for(const auto& pair:{m_synchronizedPlayback.CurrentPairShared(),m_lastPair}){
            if(pair&&&f==&pair->original){m_lastPlaybackFrame=std::shared_ptr<const VideoFrame>(pair,&pair->original);return;}
            if(pair&&&f==&pair->neural){m_lastPlaybackFrame=std::shared_ptr<const VideoFrame>(pair,&pair->neural);return;}
        }
        m_lastPlaybackFrame.reset();
        if(!m_ownedPlaybackFrame||m_ownedPlaybackFrame.use_count()!=1)m_ownedPlaybackFrame=std::make_shared<VideoFrame>();
        if(&f==&m_next)std::swap(*m_ownedPlaybackFrame,m_next);
        else *m_ownedPlaybackFrame=f;
        m_lastPlaybackFrame=m_ownedPlaybackFrame;
    }
    // Retains the presented pair by reference count. A newly presented pair
    // supersedes a paused settings preview, which is what overwriting
    // m_lastNeuralFrame used to do.
    void RememberRenderedCachedPair(){if(!m_cachedPlayback)return;if(auto pair=m_synchronizedPlayback.CurrentPairShared()){m_lastPair=std::move(pair);m_previewNeuralValid=false;m_havePresentedPair=true;}}
    // Null until a pair has been presented; every caller already gates on
    // m_havePresentedPair or checks for null.
    const VideoFrame* LastOriginalFrame()const{return m_lastPair?&m_lastPair->original:nullptr;}
    const VideoFrame* LastNeuralFrame()const{return m_previewNeuralValid?&m_previewNeuralFrame:(m_lastPair?&m_lastPair->neural:nullptr);}
    void ForgetRenderedCachedPair(){m_lastPair.reset();m_referencePair.reset();m_referenceRenderer=nullptr;m_previewNeuralValid=false;m_previewNeuralFrame=VideoFrame{};}
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
        // current_path() rather than nothing when the module cannot be
        // located: the settings file is not worth refusing to start over.
        const auto directory=platform_paths::ModuleDirectory();
        return (directory?*directory:std::filesystem::current_path())/L"DLSSVideoPlayer.ini";
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
        // Auto and the rung are separate keys on purpose. Every earlier version
        // persisted UpscaleHeight on every save, so a stored 1440 is the old
        // default rather than evidence of a choice, and keying Auto off that
        // value would deny Auto to every existing install. The absence of
        // UpscaleAuto is what identifies those files, and absence means Auto.
        m_upscaleAuto=ReadIniFloat(L"Playback",L"UpscaleAuto",1.0f)!=0.0f;
        // Generated frames per source frame the user asked for: 1 (2x) by
        // default, 0 for "as many as the display allows". Clamped to what this
        // project has phase-verified, so a hand-edited ini cannot ask for a
        // multiple nothing has measured.
        const float preference=ReadIniFloat(L"Playback",L"FrameGenerationGenerated",1.0f);
        m_frameGenPreference=(preference<0.0f||preference>float(frame_rate_policy::kPhaseVerifiedMultiFrameCount))
            ?1u:static_cast<uint32_t>(preference);
        // Absent means off, which is the step-and-spread rule in FrameRatePolicy.h.
        // This setting opts back into the divides-the-refresh-or-nothing
        // behaviour the player shipped with.
        m_evenCadenceOnly=ReadIniFloat(L"Playback",L"EvenCadenceOnly",0.0f)!=0.0f;
        const uint32_t storedTarget=uint32_t(ReadIniFloat(L"Playback",L"UpscaleHeight",1440.0f));
        if(UpscaleRungWidth(storedTarget))m_upscaleTargetHeight=storedTarget;
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
        m_gpuColorConversion=GetPrivateProfileIntW(L"Encoding",L"GpuColorConversion",0,SettingsPath().c_str())!=0;
        m_gpuSourceConversion=GetPrivateProfileIntW(L"Encoding",L"GpuSourceConversion",0,SettingsPath().c_str())!=0;
        m_nvencPreset=std::clamp<uint32_t>(uint32_t(GetPrivateProfileIntW(L"Encoding",L"NvencPreset",5,SettingsPath().c_str())),1,7);
        m_neuralSettings={};LoadNeuralSettings(SettingsPath(),m_neuralSettings);
        const UINT mode=GetPrivateProfileIntW(L"Comparison",L"Mode",0,SettingsPath().c_str());
        m_comparison={};
        for(const auto value:{ComparisonMode::Blend,ComparisonMode::SplitVertical,ComparisonMode::Wipe})
            if(mode==static_cast<UINT>(value))m_comparison.mode=value;
        m_comparison.amount=std::clamp(ReadIniFloat(L"Comparison",L"Amount",0.5f),0.0f,1.0f);
        m_comparison.splitX=std::clamp(ReadIniFloat(L"Comparison",L"SplitX",0.5f),0.0f,1.0f);
        m_comparison.zoomScale=ReadIniFloat(L"Comparison",L"ZoomScale",1.0f)>=kZoomScale?kZoomScale:1.0f;
        // Read back after the comparison defaults above, which would otherwise clear it.
        m_comparison.strength=std::clamp(ReadIniFloat(L"VideoAdjustments",L"NeuralStrength",1.0f),0.0f,2.0f);
        LoadRenderPace();
    }

    // The steady-state neural render paces this machine measured, so the
    // live-session forecast speaks for this GPU rather than the reference.
    // Stored per source geometry as `Samples=WxH:ms,ms,...;WxH:ms` for the
    // named GPU; a different GPU starts from an empty profile, and the
    // single-value `WxH:ms` form earlier versions wrote still loads as a
    // one-sample ring.
    //
    // One sample per geometry used to be the whole record, newest replacing
    // oldest unconditionally. A session measured while another process
    // saturated the CPU wrote 42.333431 ms/frame for 1920x1080 - 3.7x the
    // 11.4222 ms mean of the eight idle sessions around it, with the GPU free -
    // and because this record is persisted, that one number followed the user
    // across restarts: the next sessions forecast under a third of real time
    // and raised the "watching it live would pause to buffer almost
    // continuously" warning on hardware that renders that clip at 2.9x real
    // time. Contention can only ever make a render look slower, so the error is
    // one-sided and the newest sample is not the most trustworthy one; keeping
    // a few and taking the median lets the measurements outvote the outlier.
    //
    // The median rather than the minimum, which would be the fastest way to
    // erase a contended sample: this forecast exists to refuse sessions that
    // cannot keep up, an optimistic estimator hides exactly the warning that
    // was missing when a 4K60 session dropped 848 of 869 frames, and one-sided
    // error means a low quantile drifts towards the best case the machine has
    // ever had rather than the case the user is about to get.
    static constexpr size_t kPaceRingSamples=5;
    struct PaceHistory{uint32_t width{},height{};std::vector<double> msPerFrame;};
    static double MedianMsPerFrame(std::vector<double> samples){
        if(samples.empty())return 0.0;
        std::sort(samples.begin(),samples.end());
        const size_t middle=samples.size()/2;
        return samples.size()%2?samples[middle]:(samples[middle-1]+samples[middle])*0.5;
    }
    void RecordPaceSample(uint32_t width,uint32_t height,double msPerFrame){
        if(!width||!height||!(msPerFrame>0.0))return;
        for(PaceHistory& history:m_paceHistory){
            if(history.width!=width||history.height!=height)continue;
            history.msPerFrame.push_back(msPerFrame);
            if(history.msPerFrame.size()>kPaceRingSamples)history.msPerFrame.erase(history.msPerFrame.begin());
            return;
        }
        // Same bound as the profile the forecast reads, and the same eviction:
        // a geometry nobody has played for six geometries is the one to lose.
        if(m_paceHistory.size()==playback_timing::RenderPaceProfile::kMaxSamples)m_paceHistory.erase(m_paceHistory.begin());
        m_paceHistory.push_back({width,height,{msPerFrame}});
    }
    // The forecast reads one number per geometry; that number is the geometry's
    // median, recomputed whenever the history changes.
    void RebuildRenderPace(){
        m_renderPace={};
        for(const PaceHistory& history:m_paceHistory)
            m_renderPace.Record({history.width,history.height,MedianMsPerFrame(history.msPerFrame)});
    }
    void LoadRenderPace(){
        std::wstring gpu(512,L'\0');
        DWORD length=GetPrivateProfileStringW(L"NeuralPace",L"Gpu",L"",gpu.data(),static_cast<DWORD>(gpu.size()),SettingsPath().c_str());
        gpu.resize(length);
        std::wstring samples(2048,L'\0');
        length=GetPrivateProfileStringW(L"NeuralPace",L"Samples",L"",samples.data(),static_cast<DWORD>(samples.size()),SettingsPath().c_str());
        samples.resize(length);
        m_paceHistory.clear();m_renderPace={};
        if(gpu!=m_opt.detectedGpu.description)return;
        for(size_t start=0;start<samples.size();){
            size_t end=samples.find(L';',start);if(end==std::wstring::npos)end=samples.size();
            const std::wstring entry=samples.substr(start,end-start);
            start=end+1;
            const size_t cross=entry.find(L'x');if(cross==std::wstring::npos)continue;
            const size_t colon=entry.find(L':',cross);if(colon==std::wstring::npos)continue;
            unsigned width=0,height=0;
            if(swscanf_s(entry.c_str(),L"%ux%u",&width,&height)!=2)continue;
            for(size_t sample=colon+1;sample<=entry.size();){
                size_t sampleEnd=entry.find(L',',sample);if(sampleEnd==std::wstring::npos)sampleEnd=entry.size();
                double ms=0.0;
                if(swscanf_s(entry.substr(sample,sampleEnd-sample).c_str(),L"%lf",&ms)==1)
                    RecordPaceSample(width,height,ms);
                sample=sampleEnd+1;
            }
        }
        RebuildRenderPace();
    }
    void SaveRenderPace()const{
        std::wstring samples;
        for(const PaceHistory& history:m_paceHistory){
            if(history.msPerFrame.empty())continue;
            if(!samples.empty())samples+=L';';
            wchar_t geometry[32]{};swprintf_s(geometry,L"%ux%u:",history.width,history.height);
            samples+=geometry;
            for(size_t index=0;index<history.msPerFrame.size();++index){
                wchar_t text[32]{};swprintf_s(text,L"%.6f",history.msPerFrame[index]);
                if(index)samples+=L',';
                samples+=text;
            }
        }
        WritePrivateProfileStringW(L"NeuralPace",L"Gpu",m_opt.detectedGpu.description.c_str(),SettingsPath().c_str());
        WritePrivateProfileStringW(L"NeuralPace",L"Samples",samples.c_str(),SettingsPath().c_str());
        // Keys from the single-sample layout that preceded `Samples`.
        for(const wchar_t* stale:{L"MsPerFrame",L"Width",L"Height"})
            WritePrivateProfileStringW(L"NeuralPace",stale,nullptr,SettingsPath().c_str());
    }
    // Enough segments after the first to average out encoder spawn jitter.
    static constexpr uint64_t kMinPaceFrames=120;
    void RecordLiveRenderPace(){
        if(!m_liveSegments||!m_livePaceWidth||!m_livePaceHeight)return;
        const auto pace=m_liveSegments->Pace();
        if(pace.frames<kMinPaceFrames||!(pace.wallMs>0.0))return;
        RecordPaceSample(m_livePaceWidth,m_livePaceHeight,pace.MsPerFrame());
        RebuildRenderPace();
        SaveRenderPace();
        const size_t kept=[&]{for(const PaceHistory& history:m_paceHistory)if(history.width==m_livePaceWidth&&history.height==m_livePaceHeight)return history.msPerFrame.size();return size_t{0};}();
        LOG("Measured neural render pace: "<<m_livePaceWidth<<"x"<<m_livePaceHeight<<" at "<<pace.MsPerFrame()
            <<" ms/frame over "<<pace.frames<<" frames ("<<playback_timing::RenderPaceScale(pace.MsPerFrame(),m_livePaceWidth,m_livePaceHeight)
            <<"x the reference GPU); "<<m_renderPace.samples.size()<<" geometries known for this GPU. This geometry forecasts from the median of "
            <<kept<<" samples: "<<playback_timing::PredictRenderMs(m_renderPace,m_livePaceWidth,m_livePaceHeight,0.0)<<" ms/frame.");
    }

    void SaveVideoSettings()const{
        SaveCacheSettings();
        WriteIniFloat(L"Playback",L"Volume",m_volume);
        WriteIniFloat(L"Playback",L"Muted",m_muted?1.0f:0.0f);
        WriteIniFloat(L"Playback",L"Fill",m_fill?1.0f:0.0f);
        WriteIniFloat(L"Playback",L"NeuralView",m_neuralRequested?1.0f:0.0f);
        WriteIniFloat(L"Playback",L"SuperResolution",m_upscalingRequested?1.0f:0.0f);
        WriteIniFloat(L"Playback",L"UpscaleAuto",m_upscaleAuto?1.0f:0.0f);
        WriteIniFloat(L"Playback",L"UpscaleHeight",static_cast<float>(m_upscaleTargetHeight));
        WriteIniFloat(L"Playback",L"FrameGenerationGenerated",static_cast<float>(m_frameGenPreference));
        WriteIniFloat(L"Playback",L"EvenCadenceOnly",m_evenCadenceOnly?1.0f:0.0f);
        WriteIniFloat(L"Playback",L"YouTubeQuality",static_cast<float>(m_youtubeSourceQuality));
        WriteIniFloat(L"VideoAdjustments",L"Brightness",m_colorSettings.brightness);
        WriteIniFloat(L"VideoAdjustments",L"Contrast",m_colorSettings.contrast);
        WriteIniFloat(L"VideoAdjustments",L"Saturation",m_colorSettings.saturation);
        WriteIniFloat(L"VideoAdjustments",L"Gamma",m_colorSettings.gamma);
        WriteIniFloat(L"VideoAdjustments",L"Temperature",m_colorSettings.temperature);
        WriteIniFloat(L"VideoAdjustments",L"Tint",m_colorSettings.tint);
        // The dial lives in m_comparison but is grouped with the image adjustments here
        // because the adjustments dialog owns its control and its reset.
        WriteIniFloat(L"VideoAdjustments",L"NeuralStrength",m_comparison.strength);
        WritePrivateProfileStringW(L"NeuralGuides",L"MotionVectors",m_renderGuides.motionVectors?L"1":L"0",SettingsPath().c_str());
        WritePrivateProfileStringW(L"NeuralGuides",L"Depth",m_renderGuides.depth?L"1":L"0",SettingsPath().c_str());
        WritePrivateProfileStringW(L"Encoding",L"GpuColorConversion",m_gpuColorConversion?L"1":L"0",SettingsPath().c_str());
        WritePrivateProfileStringW(L"Encoding",L"GpuSourceConversion",m_gpuSourceConversion?L"1":L"0",SettingsPath().c_str());
        WritePrivateProfileStringW(L"Encoding",L"NvencPreset",std::to_wstring(m_nvencPreset).c_str(),SettingsPath().c_str());
        SaveNeuralSettings(SettingsPath(),m_neuralSettings);
        WritePrivateProfileStringW(L"Comparison",L"Mode",std::to_wstring(static_cast<int>(m_comparison.mode)).c_str(),SettingsPath().c_str());
        WriteIniFloat(L"Comparison",L"Amount",m_comparison.amount);
        WriteIniFloat(L"Comparison",L"SplitX",m_comparison.splitX);
        WriteIniFloat(L"Comparison",L"ZoomScale",m_comparison.zoomScale);
    }

    void ApplyVideoAdjustments(bool refreshPaused=true){
        if(m_renderer){
            m_renderer->SetColorSettings(m_colorSettings);
            if(refreshPaused&&!m_playing&&!m_seeking&&!m_renderer->PresentCurrent())RecoverUnusableRenderer();
        }
    }

    // Blend/Split/Wipe compare the neural member against the original of the
    // same pair; that only exists during cached playback with the neural view.
    bool ComparisonModesAvailable()const{return m_loaded&&m_cachedPlayback&&m_neuralRequested;}
    // Without a resident pair there is no original to composite against, so the mode
    // degrades to Neural and the strength dial degrades to 1: a missing reference shows
    // today's picture instead of compositing the neural frame against a black texture.
    ComparisonSettings EffectiveComparison()const{ComparisonSettings effective=m_comparison;if(!ComparisonModesAvailable()){effective.mode=ComparisonMode::Neural;effective.strength=1.0f;}return effective;}
    static UINT CommandForComparisonMode(ComparisonMode mode){switch(mode){case ComparisonMode::Blend:return IDM_COMPARE_BLEND;case ComparisonMode::SplitVertical:return IDM_COMPARE_SPLIT;case ComparisonMode::Wipe:return IDM_COMPARE_WIPE;default:return IDM_COMPARE_NEURAL;}}
    // Uploads the original member the presentation shader compares against.
    // Only modes that read the reference pay for the source-size copy, plus a strength
    // dial off its default, which composites against that same original.
    // True when the reference was handed to the renderer.
    bool UploadComparisonReference(const VideoFrame& original){
        // Whatever the renderer held is replaced below, or the attempt failed:
        // either way it is no longer known to be the remembered pair's.
        m_referencePair.reset();m_referenceRenderer=nullptr;
        if(!m_renderer||original.bgra.empty())return false;
        const ComparisonSettings effective=EffectiveComparison();
        // The early-out below is also what makes an NV12 source cheap: the pure
        // neural view needs no reference at all, so the conversion under it runs
        // only while someone is actually comparing - a paused inspection, where
        // a CPU pass over one frame costs nothing anyone can perceive.
        if(effective.mode==ComparisonMode::Neural&&effective.strength==1.0f)return false;
        if(original.layout==VideoPixelLayout::Nv12){
            Nv12ToBgraBt709Limited(original.bgra.data(),m_decoder.Width(),m_decoder.Height(),
                                   m_referenceBgra);
            if(m_referenceBgra.empty())return false;
            return m_renderer->UploadReferenceFrame(m_referenceBgra.data(),m_referenceBgra.size());
        }
        return m_renderer->UploadReferenceFrame(original.bgra.data(),original.bgra.size());
    }
    // The paused path's reference, uploaded only when the pair changed. Dragging
    // the split while paused ran ApplyComparison per mouse move, and each one
    // converted the whole original from NV12 and copied it into an upload
    // buffer - a full frame of CPU work for a divider that moved a pixel. The
    // renderer is compared as well as the pair: a rebuilt renderer starts with
    // no reference, whatever the pair was.
    void UploadPausedComparisonReference(){
        if(!m_havePresentedPair||!m_lastPair||!m_renderer)return;
        if(m_referencePair==m_lastPair&&m_referenceRenderer==m_renderer.get()&&m_renderer->HasReference())return;
        if(UploadComparisonReference(m_lastPair->original)){m_referencePair=m_lastPair;m_referenceRenderer=m_renderer.get();}
    }
    void ApplyComparison(bool refreshPaused=true){
        if(m_renderer){
            m_renderer->SetComparison(EffectiveComparison());
            if(refreshPaused&&!m_playing&&!m_seeking){UploadPausedComparisonReference();if(!m_renderer->PresentCurrent())RecoverUnusableRenderer();}
        }
        SyncFeatureMenuState();
    }
    // A refused mode used to be indistinguishable from one that did nothing: the
    // menu item greys out, but a command that arrives while no pair is resident
    // left no trace at all. Say which precondition was missing.
    void SetComparisonMode(ComparisonMode mode){
        if(!ComparisonModesAvailable()){
            LOG("Comparison mode refused: loaded="<<m_loaded<<" cachedPair="<<m_cachedPlayback<<" neuralView="<<m_neuralRequested);
            return;
        }
        if(mode==ComparisonMode::Original)return;
        m_comparison.mode=mode;ApplyComparison();
        LOG("Comparison mode="<<static_cast<int>(mode)<<" splitX="<<m_comparison.splitX<<" zoom="<<m_comparison.zoomScale
            <<" reference="<<m_havePresentedPair);
    }
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
                                                ToolbarActionEnabled(ToolbarAction::ToggleUpscaling),UpscalingActive(),
                                                FrameGenerationAvailable(),m_frameGenWorker.joinable());
            // Both one-shot conversions report themselves by becoming their own
            // cancel command, which is what the toolbar pill has always done.
            // A separate cancel row was a permanently greyed row and a second
            // place for the menu and the toolbar to disagree.
            if(const HMENU owner=app_menu::FindMenuContainingCommand(menu,IDM_FRAME_GENERATION))
                app_menu::SetMenuCommandText(owner,IDM_FRAME_GENERATION,
                    T(m_frameGenWorker.joinable()?L"menu.cancel_frame_generation":L"menu.frame_generation"));
            // The converted file outlives the dialog that announced it, so the
            // command that opens it is live exactly while that file exists.
            EnableMenuItem(menu,IDM_SHOW_FRAMEGEN_OUTPUT,
                           MF_BYCOMMAND|(m_frameGenLastOutput.empty()?MF_GRAYED:MF_ENABLED));
            const UINT checked=m_upscaleAuto?IDM_UPSCALE_AUTO
                :(m_upscaleTargetHeight==2160?IDM_UPSCALE_2160
                 :(m_upscaleTargetHeight==1080?IDM_UPSCALE_1080:IDM_UPSCALE_1440));
            CheckMenuRadioItem(menu,IDM_UPSCALE_AUTO,IDM_UPSCALE_2160,checked,MF_BYCOMMAND);
            // Custom ends the block, so a settings edit that leaves every preset
            // shows as Custom rather than leaving the last one checked.
            const size_t presetIndex=neural_presets::IndexOf(m_neuralSettings);
            CheckMenuRadioItem(menu,IDM_NEURAL_PRESET_FIRST,IDM_NEURAL_PRESET_CUSTOM,
                               presetIndex==neural_presets::kPresetCount
                                   ?IDM_NEURAL_PRESET_CUSTOM
                                   :UINT(IDM_NEURAL_PRESET_FIRST+presetIndex),
                               MF_BYCOMMAND);
            // 2x..5x then "as many as the display allows", in the order the
            // submenu appends them, so the radio always shows what the next
            // conversion will plan against.
            const UINT generatedChecked=m_frameGenPreference==0u?IDM_FRAMEGEN_MAX
                :(m_frameGenPreference>=4u?IDM_FRAMEGEN_5X
                 :(m_frameGenPreference==3u?IDM_FRAMEGEN_4X
                  :(m_frameGenPreference==2u?IDM_FRAMEGEN_3X:IDM_FRAMEGEN_2X)));
            CheckMenuRadioItem(menu,IDM_FRAMEGEN_2X,IDM_FRAMEGEN_MAX,generatedChecked,MF_BYCOMMAND);
            // Outside the radio range above, which CheckMenuRadioItem clears:
            // this is a constraint on the multiple, not one of the choices.
            CheckMenuItem(menu,IDM_FRAMEGEN_EVEN_ONLY,
                          MF_BYCOMMAND|(m_evenCadenceOnly?MF_CHECKED:MF_UNCHECKED));
            const UINT outputState=(m_seeking||m_seekPending||NeuralJobActive()||m_youtubeLifecycle.IsResolving())?MF_GRAYED:MF_ENABLED;
            for(const UINT item:{IDM_UPSCALE_AUTO,IDM_UPSCALE_1080,IDM_UPSCALE_1440,IDM_UPSCALE_2160})
                EnableMenuItem(menu,item,MF_BYCOMMAND|outputState);
            // A conversion holds the GPU and the helper directory, and frame
            // generation already refuses to start while an export runs. The
            // reverse guard was missing, so both could run at once and the
            // export's status line hid the conversion's progress for minutes.
            EnableMenuItem(menu,IDM_EXPORT_CACHED_VIDEO,MF_BYCOMMAND|((m_cachedPlayback&&!m_neuralPath.empty()&&!m_exportWorker.joinable()&&!m_frameGenWorker.joinable()&&!ActivityBusy())?MF_ENABLED:MF_GRAYED));
            // Same swap for the export row: while a job runs it IS the cancel
            // command, so the menu never shows a row that does nothing.
            if(const HMENU convert=app_menu::FindMenuContainingCommand(menu,IDM_EXPORT_STAGES)){
                app_menu::SetMenuCommandText(convert,IDM_EXPORT_STAGES,
                    T(m_exportWorker.joinable()?L"menu.cancel_export_running":L"menu.export_stages"));
                EnableMenuItem(convert,IDM_EXPORT_STAGES,
                    MF_BYCOMMAND|((m_exportWorker.joinable()||(m_loaded&&!ActivityBusy()&&!m_frameGenWorker.joinable()))?MF_ENABLED:MF_GRAYED));
            }
            CheckMenuRadioItem(menu,IDM_ASPECT_FIT,IDM_ASPECT_FILL,m_fill?IDM_ASPECT_FILL:IDM_ASPECT_FIT,MF_BYCOMMAND);
            app_menu::UpdateRenderActionAvailability(menu,m_loaded,RangeRenderAvailable(),NeuralJobActive(),NeuralJobPaused(),!m_cachedReceiptPath.empty());
            app_menu::UpdateComparisonMenu(menu,ComparisonModesAvailable(),m_loaded&&m_renderer!=nullptr,CommandForComparisonMode(m_comparison.mode),m_comparison.zoomScale>1.0f);
            DrawMenuBar(m_hwnd);
        }
    }

    void InvalidateControls(){
        // The one place every state change already funnels through, which makes
        // it the one place the tip rectangles cannot fall behind the layout.
        if(!m_hwnd)return;SyncFeatureMenuState();RefreshToolbarTips();RECT c{};GetClientRect(m_hwnd,&c);
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
        SetAdjustmentValue(h,IDC_ADJ_NEURAL_STRENGTH,PlainValue(m_comparison.strength));
    }

    void SyncAdjustmentControls(HWND h){
        SetTrack(h,IDC_ADJ_BRIGHTNESS,0,400,int(std::lround((m_colorSettings.brightness+2.0f)*100.0f)));
        SetTrack(h,IDC_ADJ_CONTRAST,0,300,int(std::lround(m_colorSettings.contrast*100.0f)));
        SetTrack(h,IDC_ADJ_SATURATION,0,300,int(std::lround(m_colorSettings.saturation*100.0f)));
        SetTrack(h,IDC_ADJ_GAMMA,25,300,int(std::lround(m_colorSettings.gamma*100.0f)));
        SetTrack(h,IDC_ADJ_TEMPERATURE,0,200,int(std::lround((m_colorSettings.temperature+1.0f)*100.0f)));
        SetTrack(h,IDC_ADJ_TINT,0,200,int(std::lround((m_colorSettings.tint+1.0f)*100.0f)));
        SetTrack(h,IDC_ADJ_NEURAL_STRENGTH,0,200,int(std::lround(m_comparison.strength*100.0f)));
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
        m_comparison.strength=float(pos(IDC_ADJ_NEURAL_STRENGTH))/100.0f;
        // Colors first without a present, then the comparison path: it uploads the
        // original the dial composites against and repaints the frame already on screen.
        // Both are presentation state, so a drag costs one present and never a re-render.
        UpdateAdjustmentValueLabels(h);ApplyVideoAdjustments(false);ApplyComparison(true);
    }

    // A tooltip host lives for as long as its dialog, and its tool text lives in
    // m_tipText because TTM_ADDTOOL stores the pointer it is given instead of
    // copying the string. Both are keyed by dialog: two settings dialogs can be
    // open at once, and a single shared store would free one dialog's strings out
    // from under the other dialog's still-subclassed tooltips.
    HWND EnsureTipHost(HWND dialog){
        const auto existing=m_tipHosts.find(dialog);
        if(existing!=m_tipHosts.end()&&IsWindow(existing->second))return existing->second;
        HWND host=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
            CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,dialog,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(host){
            SendMessageW(host,TTM_SETMAXTIPWIDTH,0,420);
            SendMessageW(host,TTM_SETDELAYTIME,TTDT_AUTOPOP,MAKELPARAM(30000,0));
            m_tipHosts[dialog]=host;
        }
        return host;
    }
    // The host is a popup owned by the dialog, so Windows destroys it with its
    // owner; only this dialog's strings are dropped, never another dialog's.
    void ReleaseDialogTips(HWND dialog){m_tipHosts.erase(dialog);m_tipText.erase(dialog);}

    // Which sentence a toolbar control gets on hover. Null means the control
    // says everything it needs to in its own label - "10s" does not need a
    // paragraph - so only the ones with a state worth explaining carry one.
    static const wchar_t* ToolbarTipKey(ToolbarAction action){
        switch(action){
        case ToolbarAction::ToggleNeuralRendering:return L"toolbar.tip.neural";
        case ToolbarAction::ToggleUpscaling:return L"toolbar.tip.upscaling";
        case ToolbarAction::ToggleFrameGeneration:return L"toolbar.tip.framegen";
        case ToolbarAction::Open:return L"toolbar.tip.open";
        case ToolbarAction::PlayPause:return L"toolbar.tip.playpause";
        case ToolbarAction::Mute:return L"toolbar.tip.mute";
        case ToolbarAction::Aspect:return L"toolbar.tip.aspect";
        case ToolbarAction::Adjustments:return L"toolbar.tip.color";
        case ToolbarAction::DebugView:return L"toolbar.tip.debug";
        case ToolbarAction::Fullscreen:return L"toolbar.tip.fullscreen";
        default:return nullptr;
        }
    }

    // Toolbar buttons are painted, not child windows, so their tips are
    // registered by RECTANGLE. The rectangles move on every resize and whenever
    // the bar drops items, so they are re-registered from the same layout the
    // painter uses - a tip pinned to a stale rect is worse than no tip, because
    // it describes whatever control has moved into that space.
    void RefreshToolbarTips(){
        if(!m_hwnd||!IsWindow(m_hwnd))return;
        const auto items=ToolbarItems();
        HWND host=EnsureTipHost(m_hwnd);
        if(!host)return;
        auto& text=m_tipText[m_hwnd];
        // Tool ids are the action value, so a re-registration replaces the tool
        // for that action rather than stacking a second one on top of it.
        for(const auto& item:items){
            const wchar_t* key=ToolbarTipKey(item.action);
            if(!key)continue;
            const UINT_PTR id=static_cast<UINT_PTR>(item.action);
            TTTOOLINFOW info{};info.cbSize=TTTOOLINFOW_V2_SIZE;info.uFlags=TTF_SUBCLASS;
            info.hwnd=m_hwnd;info.uId=id;info.rect=item.bounds;
            // Present already: move it. The control moved, the sentence did not.
            if(SendMessageW(host,TTM_GETTOOLINFOW,0,reinterpret_cast<LPARAM>(&info))){
                info.rect=item.bounds;
                SendMessageW(host,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&info));
                continue;
            }
            text.push_back(std::make_unique<std::wstring>(T(key)));
            info.lpszText=text.back()->data();
            info.rect=item.bounds;
            SendMessageW(host,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
        }
        // Multi-line tips need a width or comctl draws one long line.
        SendMessageW(host,TTM_SETMAXTIPWIDTH,0,LPARAM(Dip(320)));
    }
    void AddTip(HWND dialog,HWND control,const wchar_t* tipKey){
        if(!tipKey||!control)return;
        HWND host=EnsureTipHost(dialog);if(!host)return;
        auto& text=m_tipText[dialog];
        text.push_back(std::make_unique<std::wstring>(T(tipKey)));
        // The app runs on comctl32 v5 (no v6 manifest), which rejects the v6
        // struct size, so ask for the version the classic control understands.
        TTTOOLINFOW info{};info.cbSize=TTTOOLINFOW_V2_SIZE;info.uFlags=TTF_IDISHWND|TTF_SUBCLASS;info.hwnd=dialog;
        info.uId=reinterpret_cast<UINT_PTR>(control);info.lpszText=text.back()->data();
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

    // Snapshots each child's position in client coordinates the moment a
    // resizable settings dialog finishes building its controls, so later
    // WM_SIZE handling has a stable baseline to reposition from instead of
    // compounding drift onto whatever the previous resize left behind.
    void CaptureSettingsDesignLayout(HWND h){
        std::vector<std::pair<HWND,RECT>> items;
        EnumChildWindows(h,[](HWND child,LPARAM lp)->BOOL{
            RECT r{};GetWindowRect(child,&r);
            POINT pts[2]={{r.left,r.top},{r.right,r.bottom}};
            MapWindowPoints(nullptr,GetParent(child),pts,2);
            reinterpret_cast<std::vector<std::pair<HWND,RECT>>*>(lp)->push_back({child,RECT{pts[0].x,pts[0].y,pts[1].x,pts[1].y}});
            return TRUE;
        },reinterpret_cast<LPARAM>(&items));
        m_settingsDesignLayout[h]=std::move(items);
    }

    // Repositions the children of a resizable settings dialog for its current
    // client size, working from the design-time layout captured above:
    // trackbars and the note static stretch with the window, value labels and
    // push/default-push buttons stay anchored to the right and bottom edges.
    void ResizeSettingsChildren(HWND h,int designWidth,int designHeight){
        const auto it=m_settingsDesignLayout.find(h);
        if(it==m_settingsDesignLayout.end())return;
        RECT cr{};GetClientRect(h,&cr);
        const int W=int(cr.right-cr.left),H=int(cr.bottom-cr.top);
        for(const auto&[child,design]:it->second){
            if(!IsWindow(child))continue;
            wchar_t cls[32]{};GetClassNameW(child,cls,32);
            const int dx=int(design.left),dy=int(design.top),dw=int(design.right-design.left),dh=int(design.bottom-design.top);
            if(_wcsicmp(cls,TRACKBAR_CLASSW)==0){
                SetWindowPos(child,nullptr,0,0,std::max(20,W-132-16-70),dh,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE);
            }else if(_wcsicmp(cls,L"static")==0&&dw==64&&dx>=370){
                // Trackbar value label: fixed width, anchored to the right edge.
                SetWindowPos(child,nullptr,W-16-64,dy,0,0,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOSIZE);
            }else if(_wcsicmp(cls,L"static")==0&&dw==418){
                // Note static: stretches with the window, left edge fixed.
                SetWindowPos(child,nullptr,dx,dy,std::max(20,W-32),dh,SWP_NOZORDER|SWP_NOACTIVATE);
            }else if(_wcsicmp(cls,L"button")==0){
                const LONG_PTR style=GetWindowLongPtrW(child,GWL_STYLE);
                const LONG_PTR type=style&BS_TYPEMASK;
                if(type==BS_PUSHBUTTON||type==BS_DEFPUSHBUTTON)
                    SetWindowPos(child,nullptr,dx+(W-designWidth),H-(designHeight-dy),0,0,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOSIZE);
            }
        }
        InvalidateRect(h,nullptr,TRUE);
    }

    // Resizable settings dialogs share this style/rect math: the caller
    // passes the design client size, and gets back a window rect sized so
    // its client area equals that design size under the given styles.
    static RECT SettingsWindowRect(int clientW,int clientH,DWORD style,DWORD exStyle,UINT dpi){
        RECT rc{0,0,clientW,clientH};
        // The process is PER_MONITOR_AWARE_V2, so AdjustWindowRectEx reports 96-dpi
        // frame metrics and leaves the client short of the design size on a scaled
        // monitor - the bottom-anchored buttons then overlap the note text.
        using AdjustForDpiFn=BOOL(WINAPI*)(LPRECT,DWORD,BOOL,DWORD,UINT);
        static const auto adjustForDpi=reinterpret_cast<AdjustForDpiFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"),"AdjustWindowRectExForDpi"));
        if(adjustForDpi&&adjustForDpi(&rc,style,FALSE,exStyle,dpi))return rc;
        rc={0,0,clientW,clientH};AdjustWindowRectEx(&rc,style,FALSE,exStyle);return rc;
    }

    void BuildAdjustmentControls(HWND h){
        CreateAdjustmentRow(h,IDC_ADJ_BRIGHTNESS,L"adjustments.brightness",28);
        CreateAdjustmentRow(h,IDC_ADJ_CONTRAST,L"adjustments.contrast",78);
        CreateAdjustmentRow(h,IDC_ADJ_SATURATION,L"adjustments.saturation",128);
        CreateAdjustmentRow(h,IDC_ADJ_GAMMA,L"adjustments.gamma",178);
        CreateAdjustmentRow(h,IDC_ADJ_TEMPERATURE,L"adjustments.temperature",228);
        CreateAdjustmentRow(h,IDC_ADJ_TINT,L"adjustments.tint",278);
        CreateAdjustmentRow(h,IDC_ADJ_NEURAL_STRENGTH,L"adjustments.neural_strength",328,L"adjustments.neural_strength.tip");
        HFONT f=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND note=CreateWindowExW(0,L"STATIC",T(L"adjustments.note").c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,16,372,418,38,h,nullptr,nullptr,nullptr);SendMessageW(note,WM_SETFONT,(WPARAM)f,TRUE);
        HWND reset=CreateWindowExW(0,L"BUTTON",T(L"adjustments.reset").c_str(),WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,252,418,86,30,h,(HMENU)(INT_PTR)IDC_ADJ_RESET,nullptr,nullptr);
        HWND close=CreateWindowExW(0,L"BUTTON",T(L"adjustments.close").c_str(),WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,348,418,86,30,h,(HMENU)(INT_PTR)IDC_ADJ_CLOSE,nullptr,nullptr);
        SendMessageW(reset,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(close,WM_SETFONT,(WPARAM)f,TRUE);
        SyncAdjustmentControls(h);
        CaptureSettingsDesignLayout(h);
    }

    static constexpr int kAdjustDesignW=466,kAdjustDesignH=502;

    void ShowAdjustments(){
        if(m_adjustWnd&&IsWindow(m_adjustWnd)){ShowWindow(m_adjustWnd,SW_SHOWNORMAL);SetForegroundWindow(m_adjustWnd);return;}
        // Registered here, next to the only CreateWindowExW that names it, exactly like
        // the neural and encoder dialogs: the startup registration this replaced still
        // named the class V11 while this call asked for V12, so the window was never
        // created and the whole dialog was unreachable.
        static constexpr const wchar_t* kClassName=L"DLSSVideoAdjustmentsClassV12";
        WNDCLASSW a{};a.lpfnWndProc=AdjustWndProcStatic;a.hInstance=GetModuleHandleW(nullptr);a.lpszClassName=kClassName;a.hCursor=LoadCursor(nullptr,IDC_ARROW);a.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
        if(!RegisterClassW(&a)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
        constexpr DWORD style=(WS_OVERLAPPEDWINDOW&~WS_MAXIMIZEBOX)|WS_VISIBLE;
        const RECT wr=SettingsWindowRect(kAdjustDesignW,kAdjustDesignH,style,WS_EX_TOOLWINDOW,ActiveWindowDpi(m_hwnd));
        const int w=int(wr.right-wr.left),h=int(wr.bottom-wr.top);
        RECT pr{};GetWindowRect(m_hwnd,&pr);const int pw=int(pr.right-pr.left),ph=int(pr.bottom-pr.top);int x=int(pr.left)+std::max(0,(pw-w)/2),y=int(pr.top)+std::max(0,(ph-h)/2);
        m_adjustWnd=CreateWindowExW(WS_EX_TOOLWINDOW,kClassName,T(L"adjustments.title").c_str(),
            style,x,y,w,h,m_hwnd,nullptr,GetModuleHandleW(nullptr),this);
    }

    LRESULT AdjustWndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        switch(m){
        case WM_CREATE:BuildAdjustmentControls(h);return 0;
        case WM_HSCROLL:ReadAdjustmentControls(h);return 0;
        case WM_GETMINMAXINFO:{
            const RECT wr=SettingsWindowRect(kAdjustDesignW,kAdjustDesignH,DWORD(GetWindowLongPtrW(h,GWL_STYLE)),DWORD(GetWindowLongPtrW(h,GWL_EXSTYLE)),ActiveWindowDpi(h));
            auto* mmi=reinterpret_cast<MINMAXINFO*>(l);mmi->ptMinTrackSize={wr.right-wr.left,wr.bottom-wr.top};return 0;
        }
        case WM_SIZE:ResizeSettingsChildren(h,kAdjustDesignW,kAdjustDesignH);return 0;
        case WM_COMMAND:
            if(LOWORD(w)==IDC_ADJ_RESET){m_colorSettings={};m_comparison.strength=1.0f;SyncAdjustmentControls(h);ApplyVideoAdjustments(false);ApplyComparison(true);SaveVideoSettings();return 0;}
            if(LOWORD(w)==IDC_ADJ_CLOSE){DestroyWindow(h);return 0;}
            break;
        case WM_CLOSE:DestroyWindow(h);return 0;
        case WM_DESTROY:SaveVideoSettings();m_settingsDesignLayout.erase(h);ReleaseDialogTips(h);if(h==m_adjustWnd)m_adjustWnd=nullptr;return 0;
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
        // The combo lists 1..4 and the setting IS the pass count, so the
        // index is one below it.
        select(IDC_NS_PASSES,std::clamp(m_neuralSettings.passes,1,4)-1);
        const auto check=[&](int id,bool on){if(HWND box=GetDlgItem(h,id))SendMessageW(box,BM_SETCHECK,on?BST_CHECKED:BST_UNCHECKED,0);};
        check(IDC_NS_AUTOMASK,m_neuralSettings.autoMask);check(IDC_NS_GUIDE_MV,m_renderGuides.motionVectors);check(IDC_NS_GUIDE_DEPTH,m_renderGuides.depth);
        check(IDC_NS_CHAINED,m_neuralSettings.chainedHistory);
        // Chained history only governs passes 2+, so it is dead UI at one
        // pass rather than a setting that quietly does nothing.
        if(HWND chained=GetDlgItem(h,IDC_NS_CHAINED))EnableWindow(chained,m_neuralSettings.passes>1);
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
        m_neuralSettings.passes=std::clamp(sel(IDC_NS_PASSES,m_neuralSettings.passes-1)+1,1,4);
        m_neuralSettings.chainedHistory=checked(IDC_NS_CHAINED);
        if(HWND chained=GetDlgItem(h,IDC_NS_CHAINED))EnableWindow(chained,m_neuralSettings.passes>1);
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
    // A heading over each block of controls. Nine controls at one visual level
    // is a list; three named groups is a structure, and the Gestalt common
    // region is the whole reason a heading works - it tells you which controls
    // answer the same question before you read any of their labels. "Look" is
    // what the model does to the picture, "Quality and render time" is what it
    // costs, "Guides" is what it is given to work from.
    void CreateSettingsGroupHeading(HWND h,const wchar_t* key,int y){
        HWND heading=CreateWindowExW(0,L"STATIC",T(key).c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,
                                     16,y,436,18,h,nullptr,nullptr,nullptr);
        SendMessageW(heading,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
    }

    void BuildNeuralSettingControls(HWND h){
        CreateSettingsGroupHeading(h,L"neural.settings.group_look",12);
        CreateAdjustmentRow(h,IDC_NS_INTENSITY,L"neural.settings.intensity",38,L"neural.tip.intensity");
        CreateAdjustmentRow(h,IDC_NS_STRUCTURE,L"neural.settings.structure",88,L"neural.tip.structure");
        CreateAdjustmentRow(h,IDC_NS_TONE,L"neural.settings.tone",138,L"neural.tip.tone");
        CreateAdjustmentRow(h,IDC_NS_SKIN,L"neural.settings.skin",188,L"neural.tip.skin");
        CreateNeuralCombo(h,IDC_NS_STYLE,L"neural.settings.style",238,{L"Default",L"Natural",L"Cinematic"},L"neural.tip.style");
        CreateNeuralCheck(h,IDC_NS_AUTOMASK,L"neural.settings.automask",132,276,236,L"neural.tip.automask");
        HFONT f=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
        // Stacking, which arrived with RenoDX 6.x. Its own group because it
        // costs render time rather than changing the model's look: a second
        // pass measured 780,048 -> 932,019 bytes of output over the same
        // 72-frame range and took 9.81 s against 8.01 s.
        CreateSettingsGroupHeading(h,L"neural.settings.group_cost",316);
        CreateNeuralCombo(h,IDC_NS_PASSES,L"neural.settings.passes",346,{L"1 (single pass)",L"2 passes",L"3 passes",L"4 passes"},L"neural.tip.passes");
        CreateNeuralCheck(h,IDC_NS_CHAINED,L"neural.settings.chained",132,384,300,L"neural.tip.chained");
        CreateSettingsGroupHeading(h,L"neural.settings.group_guides",424);
        CreateNeuralCheck(h,IDC_NS_GUIDE_MV,L"neural.settings.guide_mv",132,452,116,L"neural.tip.guide_mv");
        CreateNeuralCheck(h,IDC_NS_GUIDE_DEPTH,L"neural.settings.guide_depth",252,452,80,L"neural.tip.guide_depth");
        HWND note=CreateWindowExW(0,L"STATIC",T(L"neural.settings.note").c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,16,492,418,38,h,nullptr,nullptr,nullptr);SendMessageW(note,WM_SETFONT,(WPARAM)f,TRUE);
        HWND reset=CreateWindowExW(0,L"BUTTON",T(L"neural.settings.reset").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,120,538,86,30,h,(HMENU)(INT_PTR)IDC_NS_RESET,nullptr,nullptr);
        HWND apply=CreateWindowExW(0,L"BUTTON",T(L"neural.settings.apply").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,216,538,122,30,h,(HMENU)(INT_PTR)IDC_NS_APPLY,nullptr,nullptr);
        HWND close=CreateWindowExW(0,L"BUTTON",T(L"neural.settings.close").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,348,538,86,30,h,(HMENU)(INT_PTR)IDC_NS_CLOSE,nullptr,nullptr);
        SendMessageW(reset,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(apply,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(close,WM_SETFONT,(WPARAM)f,TRUE);
        AddTip(h,reset,L"neural.tip.reset");AddTip(h,apply,L"neural.tip.apply");
        SyncNeuralSettingControls(h);
        CaptureSettingsDesignLayout(h);
    }

    static constexpr int kNeuralDesignW=466,kNeuralDesignH=622;

    // A preset is a starting point, not a mode: it writes the same six controls
    // the dialog edits, so the dialog stays the place the values live and an
    // edit afterwards simply lands on Custom. Applied the same way the dialog
    // applies a change, including the paused re-render that lets the picture
    // answer rather than the label.
    void ApplyNeuralPreset(size_t index){
        if(index>=neural_presets::kPresetCount)return;
        const NeuralSettings wanted=neural_presets::kPresets[index].settings;
        if(m_neuralSettings==wanted){SyncFeatureMenuState();return;}
        m_neuralSettings=wanted;
        LOG("Neural preset \""<<std::string(neural_presets::kPresets[index].key)
            <<"\" applied: "<<CanonicalNeuralSettings(m_neuralSettings));
        if(m_neuralWnd&&IsWindow(m_neuralWnd))SyncNeuralSettingControls(m_neuralWnd);
        SaveVideoSettings();
        SchedulePausedSettingsPreview();
        SyncFeatureMenuState();
    }

    void ShowNeuralSettings(){
        if(m_neuralWnd&&IsWindow(m_neuralWnd)){ShowWindow(m_neuralWnd,SW_SHOWNORMAL);SetForegroundWindow(m_neuralWnd);return;}
        static constexpr const wchar_t* kClassName=L"DLSSVideoNeuralSettingsClassV14";
        WNDCLASSW n{};n.lpfnWndProc=NeuralWndProcStatic;n.hInstance=GetModuleHandleW(nullptr);n.lpszClassName=kClassName;n.hCursor=LoadCursor(nullptr,IDC_ARROW);n.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
        if(!RegisterClassW(&n)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
        constexpr DWORD style=(WS_OVERLAPPEDWINDOW&~WS_MAXIMIZEBOX)|WS_VISIBLE;
        const RECT wr=SettingsWindowRect(kNeuralDesignW,kNeuralDesignH,style,WS_EX_TOOLWINDOW,ActiveWindowDpi(m_hwnd));
        const int w=int(wr.right-wr.left),h=int(wr.bottom-wr.top);
        RECT pr{};GetWindowRect(m_hwnd,&pr);const int pw=int(pr.right-pr.left),ph=int(pr.bottom-pr.top);int x=int(pr.left)+std::max(0,(pw-w)/2),y=int(pr.top)+std::max(0,(ph-h)/2);
        m_neuralWnd=CreateWindowExW(WS_EX_TOOLWINDOW,kClassName,T(L"neural.settings.title").c_str(),
            style,x,y,w,h,m_hwnd,nullptr,GetModuleHandleW(nullptr),this);
    }

    // The render the picture came from is not what the dialog now holds. Only the
    // export path noticed this before, so a settings change made during playback
    // left the previous render on screen with nothing saying so. A shown settings
    // preview is the one case where the frame is ahead of the cache entry rather
    // than behind the dialog, and `CompletePausedPreview` deliberately leaves
    // `m_cachedSettings` alone - the entry really is still the old render - so the
    // preview flag, not that comparison, is what says the picture is current.
    bool SettingsAheadOfRender()const{
        return m_cachedPlayback&&!m_liveSession&&!m_previewShown&&
               (m_cachedSettings!=m_neuralSettings||m_cachedGuides!=m_renderGuides);
    }
    // Raised from the paths that cannot preview, cleared when the picture catches
    // up - by a preview or by the settings coming back to what rendered it - and
    // never over a failure notice, which says something more urgent about the same
    // render.
    void NoteSettingsAheadOfRender(){
        const std::wstring text=T(L"neural.settings.ahead");
        if(SettingsAheadOfRender()){
            if(!m_neuralNotice.empty())return;
            m_neuralNotice=text;
        }else if(m_neuralNotice==text)m_neuralNotice.clear();
        else return;
        UpdateCachedStatus();InvalidateControls();
    }
    // Every way of leaving a shown settings preview puts the cache's own render back
    // on screen, so the notice that names it stale comes back with it. Guarded on the
    // transition: the playback path below calls this per rendered frame, and an
    // ordinary frame must not pay a lookup and a settings comparison.
    void LeaveSettingsPreviewFrame(){
        if(!m_previewShown)return;
        m_previewShown=false;
        NoteSettingsAheadOfRender();
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
        case WM_GETMINMAXINFO:{
            const RECT wr=SettingsWindowRect(kNeuralDesignW,kNeuralDesignH,DWORD(GetWindowLongPtrW(h,GWL_STYLE)),DWORD(GetWindowLongPtrW(h,GWL_EXSTYLE)),ActiveWindowDpi(h));
            auto* mmi=reinterpret_cast<MINMAXINFO*>(l);mmi->ptMinTrackSize={wr.right-wr.left,wr.bottom-wr.top};return 0;
        }
        case WM_SIZE:ResizeSettingsChildren(h,kNeuralDesignW,kNeuralDesignH);return 0;
        case WM_COMMAND:{
            const int id=LOWORD(w);const int code=HIWORD(w);
            if(id==IDC_NS_RESET){m_neuralSettings={};m_renderGuides={};ApplyLiveGuideControls();SyncNeuralSettingControls(h);SaveVideoSettings();SchedulePausedSettingsPreview();return 0;}
            if(id==IDC_NS_APPLY){ApplyNeuralSettings();return 0;}
            if(id==IDC_NS_CLOSE){DestroyWindow(h);return 0;}
            // Every combo and box the dialog builds has to be named here or it
            // is drawn, movable and inert: the control changes, nothing reads
            // it back, and the setting the user thinks they picked never
            // reaches the render. Adding a control without adding it to this
            // line is the one mistake this dialog invites, and it is silent.
            if(((id==IDC_NS_STYLE||id==IDC_NS_PASSES)&&code==CBN_SELCHANGE)||
               ((id==IDC_NS_AUTOMASK||id==IDC_NS_GUIDE_MV||id==IDC_NS_GUIDE_DEPTH||id==IDC_NS_CHAINED)&&code==BN_CLICKED)){ReadNeuralSettingControls(h);return 0;}
            break;
        }
        case WM_CLOSE:DestroyWindow(h);return 0;
        case WM_DESTROY:SaveVideoSettings();m_settingsDesignLayout.erase(h);ReleaseDialogTips(h);if(h==m_neuralWnd)m_neuralWnd=nullptr;return 0;
        }
        return DefWindowProcW(h,m,w,l);
    }

    static LRESULT CALLBACK NeuralWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        PlayerApp* a=nullptr;
        if(m==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
        else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        return a?a->NeuralWndProc(h,m,w,l):DefWindowProcW(h,m,w,l);
    }

    void SyncEncoderSettingControls(HWND h){
        const auto check=[&](int id,bool on){if(HWND box=GetDlgItem(h,id))SendMessageW(box,BM_SETCHECK,on?BST_CHECKED:BST_UNCHECKED,0);};
        check(IDC_ES_GPU_CONVERT,m_gpuColorConversion);
        check(IDC_ES_GPU_SOURCE,m_gpuSourceConversion);
        if(HWND combo=GetDlgItem(h,IDC_ES_NVENC_PRESET))SendMessageW(combo,CB_SETCURSEL,static_cast<WPARAM>(int(m_nvencPreset)-1),0);
    }

    void ReadEncoderSettingControls(HWND h){
        auto sel=[&](int id,int fallback)->int{HWND c=GetDlgItem(h,id);const int index=c?int(SendMessageW(c,CB_GETCURSEL,0,0)):CB_ERR;return index==CB_ERR?fallback:index;};
        auto checked=[&](int id)->bool{HWND b=GetDlgItem(h,id);return b&&SendMessageW(b,BM_GETCHECK,0,0)==BST_CHECKED;};
        // Only the next conversion reads these, so there is nothing to re-render for them.
        m_gpuColorConversion=checked(IDC_ES_GPU_CONVERT);
        m_gpuSourceConversion=checked(IDC_ES_GPU_SOURCE);
        m_nvencPreset=uint32_t(sel(IDC_ES_NVENC_PRESET,int(m_nvencPreset)-1))+1;
        SaveVideoSettings();
    }

    void BuildEncoderSettingControls(HWND h){
        CreateNeuralCheck(h,IDC_ES_GPU_CONVERT,L"encoder.settings.gpu_convert",132,28,236,L"encoder.tip.gpu_convert");
        CreateNeuralCheck(h,IDC_ES_GPU_SOURCE,L"encoder.settings.gpu_source",132,52,236,L"encoder.tip.gpu_source");
        CreateNeuralCombo(h,IDC_ES_NVENC_PRESET,L"encoder.settings.nvenc_preset",84,{L"p1 (fastest)",L"p2",L"p3",L"p4",L"p5",L"p6",L"p7 (best quality)"},L"encoder.tip.nvenc_preset");
        HFONT f=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND note=CreateWindowExW(0,L"STATIC",T(L"encoder.settings.note").c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,16,120,418,38,h,nullptr,nullptr,nullptr);SendMessageW(note,WM_SETFONT,(WPARAM)f,TRUE);
        HWND reset=CreateWindowExW(0,L"BUTTON",T(L"encoder.settings.reset").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,252,166,86,30,h,(HMENU)(INT_PTR)IDC_ES_RESET,nullptr,nullptr);
        HWND close=CreateWindowExW(0,L"BUTTON",T(L"encoder.settings.close").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,348,166,86,30,h,(HMENU)(INT_PTR)IDC_ES_CLOSE,nullptr,nullptr);
        SendMessageW(reset,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(close,WM_SETFONT,(WPARAM)f,TRUE);
        SyncEncoderSettingControls(h);
        CaptureSettingsDesignLayout(h);
    }

    static constexpr int kEncoderDesignW=466,kEncoderDesignH=232;

    void ShowEncoderSettings(){
        if(m_encoderWnd&&IsWindow(m_encoderWnd)){ShowWindow(m_encoderWnd,SW_SHOWNORMAL);SetForegroundWindow(m_encoderWnd);return;}
        static constexpr const wchar_t* kClassName=L"DLSSVideoEncoderSettingsClassV1";
        WNDCLASSW n{};n.lpfnWndProc=EncoderWndProcStatic;n.hInstance=GetModuleHandleW(nullptr);n.lpszClassName=kClassName;n.hCursor=LoadCursor(nullptr,IDC_ARROW);n.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
        if(!RegisterClassW(&n)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
        constexpr DWORD style=(WS_OVERLAPPEDWINDOW&~WS_MAXIMIZEBOX)|WS_VISIBLE;
        const RECT wr=SettingsWindowRect(kEncoderDesignW,kEncoderDesignH,style,WS_EX_TOOLWINDOW,ActiveWindowDpi(m_hwnd));
        const int w=int(wr.right-wr.left),h=int(wr.bottom-wr.top);
        RECT pr{};GetWindowRect(m_hwnd,&pr);const int pw=int(pr.right-pr.left),ph=int(pr.bottom-pr.top);int x=int(pr.left)+std::max(0,(pw-w)/2),y=int(pr.top)+std::max(0,(ph-h)/2);
        m_encoderWnd=CreateWindowExW(WS_EX_TOOLWINDOW,kClassName,T(L"encoder.settings.title").c_str(),
            style,x,y,w,h,m_hwnd,nullptr,GetModuleHandleW(nullptr),this);
    }

    LRESULT EncoderWndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        switch(m){
        case WM_CREATE:BuildEncoderSettingControls(h);return 0;
        case WM_GETMINMAXINFO:{
            const RECT wr=SettingsWindowRect(kEncoderDesignW,kEncoderDesignH,DWORD(GetWindowLongPtrW(h,GWL_STYLE)),DWORD(GetWindowLongPtrW(h,GWL_EXSTYLE)),ActiveWindowDpi(h));
            auto* mmi=reinterpret_cast<MINMAXINFO*>(l);mmi->ptMinTrackSize={wr.right-wr.left,wr.bottom-wr.top};return 0;
        }
        case WM_SIZE:ResizeSettingsChildren(h,kEncoderDesignW,kEncoderDesignH);return 0;
        case WM_COMMAND:{
            const int id=LOWORD(w);const int code=HIWORD(w);
            if(id==IDC_ES_RESET){m_gpuColorConversion=false;m_gpuSourceConversion=false;m_nvencPreset=5;SyncEncoderSettingControls(h);SaveVideoSettings();return 0;}
            if(id==IDC_ES_CLOSE){DestroyWindow(h);return 0;}
            if(((id==IDC_ES_GPU_CONVERT||id==IDC_ES_GPU_SOURCE)&&code==BN_CLICKED)||(id==IDC_ES_NVENC_PRESET&&code==CBN_SELCHANGE)){ReadEncoderSettingControls(h);return 0;}
            break;
        }
        case WM_CLOSE:DestroyWindow(h);return 0;
        case WM_DESTROY:SaveVideoSettings();m_settingsDesignLayout.erase(h);ReleaseDialogTips(h);if(h==m_encoderWnd)m_encoderWnd=nullptr;return 0;
        }
        return DefWindowProcW(h,m,w,l);
    }

    // ---- Export with DLSS stages ----------------------------------------
    //
    // One dialog for the three stages, because the interesting thing about them
    // is which COMBINATION you want and the combination is what the file
    // records. The order is fixed and stated on the panel rather than offered:
    // NVIDIA's DLSS 5 runs neural rendering on the upscaled frame and DLSS-G
    // consumes the finished picture, so Super Resolution -> neural -> frame
    // generation is the reference arrangement and there is no reading of the
    // evidence where letting the user reorder it helps them.
    //
    // This writes a file and nothing else. It deliberately does not touch the
    // neural cache: a cache entry is a playback carrier keyed on the source and
    // the settings, while this is a one-off at a size and a rate the viewer
    // picked. "Save converted video" remains the way to keep the render you are
    // already watching.
    static constexpr int kExportDesignW=470,kExportDesignH=368;

    uint32_t ExportMaxMultiplier()const{
        // 1 + the runtime's generated-frames-per-pair. Unmeasured reads as 2, the
        // floor every DLSS-G capable card admits; the plan re-checks against the
        // measured value once the user actually presses Export.
        if(!m_frameGenCapability)return 2;
        return m_frameGenCapability->available?1u+m_frameGenCapability->multiFrameCountMax:0u;
    }

    ExportPlan CurrentExportPlan()const{
        return PlanExport(m_exportSelection,m_decoder.Width(),m_decoder.Height(),
                          m_decoder.FrameRate(),ExportMaxMultiplier(),m_decoder.IsStillImage());
    }

    static const wchar_t* ExportRefusalKey(ExportRefusal refusal){
        switch(refusal){
        case ExportRefusal::NothingSelected:return L"export.stages.refusal.nothing";
        case ExportRefusal::SourceGeometryUnknown:return L"export.stages.refusal.geometry";
        case ExportRefusal::AlreadyAtTarget:return L"export.stages.refusal.target";
        case ExportRefusal::MultiplierUnsupported:return L"export.stages.refusal.multiplier";
        case ExportRefusal::StillImage:return L"export.stages.refusal.still";
        case ExportRefusal::UpscaleNeedsNeural:return L"export.stages.refusal.upscale_needs_neural";
        case ExportRefusal::None:break;
        }
        return L"export.stages.refusal.nothing";
    }

    // The file this export reads. A stream has to have finished copying first:
    // the passes hand a path to ffmpeg and to a decoder of their own, exactly as
    // frame generation does, so they cannot read a URL.
    std::filesystem::path ExportSourceFile()const{
        if(!m_loaded)return {};
        if(m_sourceKind==MediaSourceKind::LocalFile||m_cachedSourceFile)return std::filesystem::path(m_path);
        if(const std::filesystem::path* copy=FrameGenerationAcquiredCopy())return *copy;
        return {};
    }

    void ShowExportStages(){
        if(m_exportStagesWnd&&IsWindow(m_exportStagesWnd)){ShowWindow(m_exportStagesWnd,SW_SHOWNORMAL);SetForegroundWindow(m_exportStagesWnd);return;}
        static constexpr const wchar_t* kClassName=L"DLSSVideoExportStagesClassV1";
        WNDCLASSW n{};n.lpfnWndProc=ExportStagesWndProcStatic;n.hInstance=GetModuleHandleW(nullptr);n.lpszClassName=kClassName;n.hCursor=LoadCursor(nullptr,IDC_ARROW);n.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
        if(!RegisterClassW(&n)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
        constexpr DWORD style=(WS_OVERLAPPEDWINDOW&~WS_MAXIMIZEBOX)|WS_VISIBLE;
        const RECT wr=SettingsWindowRect(kExportDesignW,kExportDesignH,style,WS_EX_TOOLWINDOW,ActiveWindowDpi(m_hwnd));
        const int w=int(wr.right-wr.left),h=int(wr.bottom-wr.top);
        RECT pr{};GetWindowRect(m_hwnd,&pr);const int pw=int(pr.right-pr.left),ph=int(pr.bottom-pr.top);
        m_exportStagesWnd=CreateWindowExW(WS_EX_TOOLWINDOW,kClassName,T(L"export.stages.title").c_str(),
            style,int(pr.left)+std::max(0,(pw-w)/2),int(pr.top)+std::max(0,(ph-h)/2),w,h,
            m_hwnd,nullptr,GetModuleHandleW(nullptr),this);
    }

    void BuildExportStageControls(HWND h){
        HFONT f=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
        CreateSettingsGroupHeading(h,L"export.stages.group_stages",8);
        CreateNeuralCheck(h,IDC_EX_UPSCALE,L"export.stages.upscale",16,32,300,L"export.tip.upscale");
        CreateNeuralCombo(h,IDC_EX_RESOLUTION,L"export.stages.resolution",70,{L"1080p",L"1440p",L"2160p"});
        CreateNeuralCheck(h,IDC_EX_NEURAL,L"export.stages.neural",16,112,300,L"export.tip.neural");
        CreateNeuralCheck(h,IDC_EX_FRAMEGEN,L"export.stages.framegen",16,152,300,L"export.tip.framegen");
        CreateNeuralCombo(h,IDC_EX_MULTIPLIER,L"export.stages.multiplier",190,{L"2×",L"3×",L"4×",L"5×"});
        CreateSettingsGroupHeading(h,L"export.stages.group_result",230);
        HWND summary=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT,16,254,436,34,h,(HMENU)(INT_PTR)IDC_EX_SUMMARY,nullptr,nullptr);
        SendMessageW(summary,WM_SETFONT,(WPARAM)f,TRUE);
        HWND note=CreateWindowExW(0,L"STATIC",T(L"export.stages.note").c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,16,288,436,34,h,nullptr,nullptr,nullptr);
        SendMessageW(note,WM_SETFONT,(WPARAM)f,TRUE);
        HWND run=CreateWindowExW(0,L"BUTTON",T(L"export.stages.run").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,232,330,120,30,h,(HMENU)(INT_PTR)IDC_EX_RUN,nullptr,nullptr);
        HWND close=CreateWindowExW(0,L"BUTTON",T(L"export.stages.close").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,362,330,90,30,h,(HMENU)(INT_PTR)IDC_EX_CLOSE,nullptr,nullptr);
        SendMessageW(run,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(close,WM_SETFONT,(WPARAM)f,TRUE);
        SyncExportStageControls(h);
        CaptureSettingsDesignLayout(h);
    }

    void SyncExportStageControls(HWND h){
        const auto check=[&](int id,bool on){if(HWND b=GetDlgItem(h,id))SendMessageW(b,BM_SETCHECK,on?BST_CHECKED:BST_UNCHECKED,0);};
        const auto select=[&](int id,int index){if(HWND c=GetDlgItem(h,id))SendMessageW(c,CB_SETCURSEL,static_cast<WPARAM>(index),0);};
        check(IDC_EX_UPSCALE,m_exportSelection.upscale);
        check(IDC_EX_NEURAL,m_exportSelection.neural);
        check(IDC_EX_FRAMEGEN,m_exportSelection.frameGeneration);
        int rung=1;for(size_t i=0;i<std::size(kUpscaleRungHeights);++i)if(kUpscaleRungHeights[i]==m_exportSelection.targetHeight)rung=int(i);
        select(IDC_EX_RESOLUTION,rung);
        select(IDC_EX_MULTIPLIER,std::clamp(int(m_exportSelection.multiplier),2,5)-2);
        // A rung you cannot choose and a rate you cannot reach are greyed, not
        // hidden: the control staying visible is what tells the user the stage
        // exists and why it is unavailable here.
        if(HWND c=GetDlgItem(h,IDC_EX_RESOLUTION))EnableWindow(c,m_exportSelection.upscale);
        if(HWND c=GetDlgItem(h,IDC_EX_MULTIPLIER))EnableWindow(c,m_exportSelection.frameGeneration&&ExportMaxMultiplier()>2);
        const ExportPlan plan=CurrentExportPlan();
        std::wstring summary;
        if(!plan.valid)summary=T(ExportRefusalKey(plan.refusal));
        else{
            wchar_t line[256];
            swprintf_s(line,L"%u × %u at %.4g fps · %u pass%s",plan.outputWidth,plan.outputHeight,
                       plan.outputFps,ExportStageCount(plan),ExportStageCount(plan)==1?L"":L"es");
            summary=line;
        }
        SetDlgItemTextW(h,IDC_EX_SUMMARY,summary.c_str());
        if(HWND run=GetDlgItem(h,IDC_EX_RUN))EnableWindow(run,plan.valid&&!ExportStagesBusy());
    }

    void ReadExportStageControls(HWND h){
        const auto checked=[&](int id){HWND b=GetDlgItem(h,id);return b&&SendMessageW(b,BM_GETCHECK,0,0)==BST_CHECKED;};
        const auto sel=[&](int id,int fallback){HWND c=GetDlgItem(h,id);const int i=c?int(SendMessageW(c,CB_GETCURSEL,0,0)):CB_ERR;return i==CB_ERR?fallback:i;};
        m_exportSelection.upscale=checked(IDC_EX_UPSCALE);
        m_exportSelection.neural=checked(IDC_EX_NEURAL);
        m_exportSelection.frameGeneration=checked(IDC_EX_FRAMEGEN);
        m_exportSelection.targetHeight=kUpscaleRungHeights[std::clamp(sel(IDC_EX_RESOLUTION,1),0,int(std::size(kUpscaleRungHeights))-1)];
        m_exportSelection.multiplier=uint32_t(std::clamp(sel(IDC_EX_MULTIPLIER,0),0,3)+2);
        SyncExportStageControls(h);
    }

    bool ExportStagesBusy()const{return m_exportWorker.joinable()||m_frameGenWorker.joinable()||NeuralJobActive();}

    LRESULT ExportStagesWndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        switch(m){
        case WM_CREATE:BuildExportStageControls(h);return 0;
        case WM_GETMINMAXINFO:{
            const RECT wr=SettingsWindowRect(kExportDesignW,kExportDesignH,DWORD(GetWindowLongPtrW(h,GWL_STYLE)),DWORD(GetWindowLongPtrW(h,GWL_EXSTYLE)),ActiveWindowDpi(h));
            auto* mmi=reinterpret_cast<MINMAXINFO*>(l);mmi->ptMinTrackSize={wr.right-wr.left,wr.bottom-wr.top};return 0;
        }
        case WM_SIZE:ResizeSettingsChildren(h,kExportDesignW,kExportDesignH);return 0;
        case WM_COMMAND:{
            const int id=LOWORD(w);const int code=HIWORD(w);
            if(id==IDC_EX_CLOSE){DestroyWindow(h);return 0;}
            if(id==IDC_EX_RUN){StartStageExport();SyncExportStageControls(h);return 0;}
            // Every control the dialog builds is named here. One left out is
            // drawn, movable and inert - the exact failure the neural settings
            // dialog shipped with when stacking was added.
            if(((id==IDC_EX_UPSCALE||id==IDC_EX_NEURAL||id==IDC_EX_FRAMEGEN)&&code==BN_CLICKED)||
               ((id==IDC_EX_RESOLUTION||id==IDC_EX_MULTIPLIER)&&code==CBN_SELCHANGE)){ReadExportStageControls(h);return 0;}
            break;
        }
        case WM_CLOSE:DestroyWindow(h);return 0;
        case WM_DESTROY:m_settingsDesignLayout.erase(h);ReleaseDialogTips(h);if(h==m_exportStagesWnd)m_exportStagesWnd=nullptr;return 0;
        }
        return DefWindowProcW(h,m,w,l);
    }

    static LRESULT CALLBACK ExportStagesWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l){
        if(m==WM_NCCREATE){SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));return DefWindowProcW(h,m,w,l);}
        auto* self=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        return self?self->ExportStagesWndProc(h,m,w,l):DefWindowProcW(h,m,w,l);
    }

    // Runs the plan on the export thread, one stage at a time, and hands the
    // last stage's file to the destination the user picked. The intermediate
    // file lives in the cache root beside the other derived carriers and is
    // removed whichever way this ends.
    void StartStageExport(){
        const std::wstring title=T(L"export.stages.title");
        if(ExportStagesBusy()){MessageBoxW(m_hwnd,T(L"export.stages.busy").c_str(),title.c_str(),MB_OK|MB_ICONINFORMATION);return;}
        const std::filesystem::path source=ExportSourceFile();
        if(source.empty()){MessageBoxW(m_hwnd,T(L"export.stages.no_source").c_str(),title.c_str(),MB_OK|MB_ICONINFORMATION);return;}
        // Measured inside the user's own action, with the cursor that says so,
        // exactly as frame generation does - a background probe beside the
        // renderer froze playback once already.
        if(m_exportSelection.frameGeneration&&!m_frameGenCapability){
            const HCURSOR previous=SetCursor(LoadCursorW(nullptr,IDC_WAIT));
            m_frameGenCapability=QueryFrameGenerationCapability();
            SetCursor(previous);
        }
        const ExportPlan plan=CurrentExportPlan();
        if(!plan.valid){MessageBoxW(m_hwnd,T(ExportRefusalKey(plan.refusal)).c_str(),title.c_str(),MB_OK|MB_ICONINFORMATION);return;}
        NeuralCacheManager cache(m_cacheRoot);
        if(!cache.Valid()){MessageBoxW(m_hwnd,CacheFailureText().DescribeRoot(cache.LastFailure()).c_str(),title.c_str(),MB_OK|MB_ICONERROR);return;}
        const std::filesystem::path scratch=cache.Root()/L"export-stages";
        std::error_code directoryError;std::filesystem::create_directories(scratch,directoryError);
        if(directoryError){MessageBoxW(m_hwnd,T(L"framegen.cache_failed").c_str(),title.c_str(),MB_OK|MB_ICONERROR);return;}
        const std::filesystem::path destination=PickExportFile(m_hwnd,m_displayTitle,m_decoder.IsStillImage(),m_decoder.IsAnimation());
        if(destination.empty())return;

        LOG("Stage export starting: upscale="<<m_exportSelection.upscale
            <<" neural="<<m_exportSelection.neural<<" framegen="<<m_exportSelection.frameGeneration
            <<" output="<<plan.outputWidth<<"x"<<plan.outputHeight<<" fps="<<plan.outputFps
            <<" passes="<<ExportStageCount(plan)<<" source="<<WideToUtf8(source.wstring()));

        const auto helpers=ExecutableDirectory();
        const auto worker=helpers/L"neural-runtime"/L"NeuralWorker.exe";
        const double fps=m_decoder.FrameRate(),duration=m_decoder.DurationSeconds();
        const uint32_t sourceWidth=m_decoder.Width(),sourceHeight=m_decoder.Height();
        const uint32_t nvencPreset=m_nvencPreset;
        HWND target=m_hwnd;auto* completions=&m_exportCompletions;
        try{
            m_exportWorker=std::jthread([=](std::stop_token stop){
                auto completion=std::make_unique<ExportCompletion>();
                completion->output=destination;
                const uint64_t tag=GetTickCount64();
                const auto stageOne=scratch/(L"stage1-"+std::to_wstring(tag)+L".mkv");
                const auto stageTwo=scratch/(L"stage2-"+std::to_wstring(tag)+L".mkv");
                std::filesystem::path produced=source;
                const auto sweep=[&]{std::error_code ec;
                    if(stageOne!=produced)std::filesystem::remove(stageOne,ec);
                    if(stageTwo!=produced)std::filesystem::remove(stageTwo,ec);};
                if(plan.workerStage){
                    NeuralRenderRequest request{};
                    request.sourcePath=produced;request.stagingVideoPath=stageOne;
                    request.width=sourceWidth;request.height=sourceHeight;
                    request.fps=fps;request.durationSeconds=duration;
                    request.nvencPreset=nvencPreset;
                    request.requireNeural=plan.requireNeural;
                    if(plan.outputWidth!=sourceWidth||plan.outputHeight!=sourceHeight){
                        request.outputWidth=plan.outputWidth;request.outputHeight=plan.outputHeight;
                    }
                    // The callback this used to pass empty. The worker has
                    // always reported frames; nothing was listening.
                    const uint32_t passes=(plan.workerStage?1u:0u)+(plan.frameGenStage?1u:0u);
                    const wchar_t* passKey=plan.requireNeural
                        ?(plan.outputWidth!=sourceWidth?L"export.progress.pass_sr_neural":L"export.progress.pass_neural")
                        :L"export.progress.pass_sr";
                    const auto post=[&](uint32_t pass,const wchar_t* key,uint64_t done,uint64_t total){
                        auto* update=new StageExportProgress{true,pass,passes,key,done,total,{}};
                        if(!PostMessageW(target,WM_STAGE_EXPORT_PROGRESS,0,reinterpret_cast<LPARAM>(update)))delete update;
                    };
                    post(1,passKey,0,0);
                    const NeuralRenderResult result=RunNeuralWorker(worker,request,
                        [&](const NeuralRenderProgress& p){post(1,passKey,p.completedFrames,p.totalFrames);},stop);
                    if(!result.ok){
                        completion->result={false,result.cancelled?MaterializeError::Cancelled:MaterializeError::ProcessFailed,result.detail};
                        sweep();
                        completions->RegisterAndPost(std::move(completion),[&](uint64_t token){return PostMessageW(target,WM_EXPORT_COMPLETE,static_cast<WPARAM>(token),0)!=FALSE;});
                        return;
                    }
                    produced=stageOne;
                }
                if(plan.frameGenStage){
                    FrameGenerationRequest request{};
                    request.source=produced;
                    // Audio, subtitles and chapters come from the original: every
                    // carrier this project writes is video-only.
                    request.streamSource=source;
                    request.output=stageTwo;
                    request.multiplier=plan.multiplier;
                    request.nvencPreset=nvencPreset;
                    const uint32_t generatePass=plan.workerStage?2u:1u;
                    const uint32_t generatePasses=(plan.workerStage?1u:0u)+1u;
                    const auto postGenerate=[&](uint64_t done,uint64_t total){
                        auto* update=new StageExportProgress{true,generatePass,generatePasses,
                                                             L"export.progress.pass_framegen",done,total,{}};
                        if(!PostMessageW(target,WM_STAGE_EXPORT_PROGRESS,0,reinterpret_cast<LPARAM>(update)))delete update;
                    };
                    postGenerate(0,0);
                    const FrameGenerationResult result=FrameGenerationPass(helpers).Run(request,stop,
                        [&](const FrameGenerationProgress& p){postGenerate(p.sourceFramesRead,p.sourceFramesTotal);});
                    if(!result.ok){
                        completion->result={false,result.error==FrameGenerationError::Cancelled?MaterializeError::Cancelled:MaterializeError::ProcessFailed,result.detail};
                        sweep();
                        completions->RegisterAndPost(std::move(completion),[&](uint64_t token){return PostMessageW(target,WM_EXPORT_COMPLETE,static_cast<WPARAM>(token),0)!=FALSE;});
                        return;
                    }
                    produced=stageTwo;
                }
                std::error_code moveError;
                std::filesystem::remove(destination,moveError);
                std::filesystem::rename(produced,destination,moveError);
                if(moveError){
                    // A rename across volumes fails; a copy is the fallback the
                    // user's chosen folder may require.
                    moveError.clear();
                    std::filesystem::copy_file(produced,destination,std::filesystem::copy_options::overwrite_existing,moveError);
                }
                {
                    auto* done=new StageExportProgress{};
                    if(!PostMessageW(target,WM_STAGE_EXPORT_PROGRESS,0,reinterpret_cast<LPARAM>(done)))delete done;
                }
                completion->result={!moveError,moveError?MaterializeError::ProcessFailed:MaterializeError::None,
                                    moveError?L"The finished export could not be written to the chosen file.":std::wstring{}};
                sweep();
                completions->RegisterAndPost(std::move(completion),[&](uint64_t token){return PostMessageW(target,WM_EXPORT_COMPLETE,static_cast<WPARAM>(token),0)!=FALSE;});
            });
        }catch(const std::system_error&){
            MessageBoxW(m_hwnd,T(L"export.worker_failed").c_str(),T(L"export.title.failed").c_str(),MB_OK|MB_ICONERROR);return;
        }
        SyncFeatureMenuState();UpdateCachedStatus();
    }

    static LRESULT CALLBACK EncoderWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        PlayerApp* a=nullptr;
        if(m==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
        else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        return a?a->EncoderWndProc(h,m,w,l):DefWindowProcW(h,m,w,l);
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
            if(m==WM_PAINT){PAINTSTRUCT ps{};BeginPaint(h,&ps);EndPaint(h,&ps);a->m_staticPresentPending=true;return 0;}
            if(m==WM_SIZE){a->m_staticPresentPending=true;return 0;}
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
        // Every open by name ends what a previous drop said; a drop sets its
        // note again once this returns.
        m_dropNote.Clear();
        // Preview first: identify the source and replay a validated cache entry
        // when one exists, otherwise open the original and let the user choose
        // what to render. Opening a file never starts a whole-video render.
        if(sourceKind==MediaSourceKind::LocalFile&&NeuralPreRenderEnabled()){
            StartNeuralJob(source,{},displayTitle,{},sourceKind,m_youtubeSourceQuality,{},0.0,{},true);
            return true;
        }
        return LoadOriginal(source,displayTitle,sourceKind);
    }

    // recordRecent=false for a derived carrier the user never opened by name:
    // the frame-generation output lives in the cache, so a Recent entry for it
    // would dangle the moment the cache is cleared and would list a path the
    // user cannot recognise beside the films they actually opened.
    bool LoadOriginal(const std::wstring& source,const std::wstring& displayTitle=L"",MediaSourceKind sourceKind=MediaSourceKind::LocalFile,bool localPayload=false,bool recordRecent=true) {
        if(source.empty())return false;
        CancelNeuralJob(false);
        CancelYouTubeResolution();
        Unload();
        LOG("Opening " << (localPayload ? std::string_view("acquired source copy") : SafeSourceLogLabel(sourceKind)) << ".");
        if(!m_decoder.Open(source,localPayload?MediaSourceKind::LocalFile:sourceKind,{},/*preferNv12=*/true)){std::wstring e=T(sourceKind==MediaSourceKind::YouTube?L"youtube.error.ffmpeg":L"error.decode"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);return false;}
        m_dar=m_decoder.DisplayAspectRatio(); if(!std::isfinite(m_dar)||m_dar<0.2)m_dar=double(m_decoder.Width())/std::max(1u,m_decoder.Height());
        const auto ow=m_decoder.Width(),oh=m_decoder.Height();
        m_activeQuality=DefaultNeuralCarrierQuality();
        const auto [guideW,guideH]=TemporalGuideGenerator::AnalysisGrid(m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate());
        ShowWindow(m_viewport,SW_SHOW); Layout();
        m_renderer=MakeD3D12Renderer();
        ConfigureRendererSource();
        if(!m_renderer->Initialize(m_renderWnd,m_decoder.Width(),m_decoder.Height(),ow,oh,guideW,guideH,m_activeQuality)||!RendererTookSourceLayout()){std::wstring e=T(L"error.renderer"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);m_renderer.reset();m_decoder.Close();ShowWindow(m_viewport,SW_HIDE);return false;}
        m_renderer->SetDLSS(false);m_renderer->SetColorSettings(m_colorSettings);m_renderer->SetComparison(EffectiveComparison());
        VideoFrame first; if(!m_decoder.ReadNext(first)){std::wstring e=T(L"error.frame"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);Unload();return false;}
        m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;RenderVideoFrame(first,true);m_currentSec=double(first.timestamp100ns)*1e-7;
        m_haveNext=m_decoder.ReadNext(m_next);if(!m_decoder.IsStillImage())Audio().Start(source,m_currentSec);Audio().SetVolume(m_muted?0.0f:m_volume);m_playing=!m_decoder.IsStillImage();m_playStartSec=m_currentSec;m_playStart=Clock::now();m_loaded=true;m_path=source;m_sourceKind=sourceKind;m_cachedSourceFile=localPayload;m_displayTitle=DisplayTitleForSource(sourceKind,displayTitle);if(m_displayTitle.empty()&&sourceKind==MediaSourceKind::LocalFile){m_displayTitle=std::filesystem::path(source).stem().wstring();if(m_displayTitle.empty())m_displayTitle=std::filesystem::path(source).filename().wstring();}m_droppedFrames=0;m_seekPending=false;m_seeking=false;m_fpsWindowStart=Clock::now();m_fpsWindowFrames=0;m_submitFps=0.0;
        RestoreUpscaling();UpdateTitle();UpdateCachedStatus();Layout();if(recordRecent)RecordOriginalRecent();SyncFeatureMenuState();UpdateAudioTrackMenu();InvalidateRect(m_hwnd,nullptr,TRUE);return true;
    }

    void Unload() {
        // A new file gets a new digest, even if the old one had the same size
        // and timestamp: the memo is scoped to the loaded media precisely so
        // it cannot outlive it.
        ForgetSourceDigest();
        // The live pair holds the segment files open; it has to be closed
        // before the session's directory is removed, or the removal fails
        // silently and the segments stay on disk until the next session.
        m_synchronizedPlayback.Close();
        if(m_liveSession||m_previewJob)CancelNeuralJob(false);
        if(m_liveSession)ReleaseLiveSession();
        DropRetainedLiveSegments();
        // Nothing that was waiting on this file carries over to the next one:
        // a settings preview still settling, or a neural toggle pressed during
        // a seek, would otherwise fire on the next file's first seek.
        CancelPausedSettingsPreview();m_previewShown=false;m_neuralToggleDeferred=false;m_livePaceConfirmedKey.clear();
        m_lastPlaybackFrame={};m_ownedPlaybackFrame.reset();m_upscalingError.clear();m_neuralNotice.clear();m_sourceNotice.clear();m_decodeNotice.clear();m_neuralPath.clear();m_cachedRange={};m_cachedReceiptPath.clear();m_cachedSettings={};m_cachedGuides={};m_markers={};m_dragSplit=false;m_renderMouseKnown=false;
        m_seekPending=false;m_seeking=false;Audio().Stop();m_networkAudio.reset();m_renderer.reset();m_decoder.Close();m_cachedPlayback=false;m_cachedSourceFile=false;m_cachedPresentedFrames=0;m_havePresentedPair=false;ForgetRenderedCachedPair();m_guides.Reset();m_haveNext=false;m_waitingForNetworkFrame=false;m_networkReadState.Reset();m_next=VideoFrame{};m_nextPairFrame.reset();m_loaded=false;m_playing=false;m_currentSec=0;m_lastRenderedTs=-1;m_path.clear();m_youtubeAudioUrl.clear();m_youtubePageUrl.clear();m_displayTitle.clear();m_sourceKind=MediaSourceKind::LocalFile;m_cachedStatus.clear();InvalidateFrameGenerationCopy();
        m_jobSourcePath.clear();m_jobSourceKey.clear();m_jobSourcePageUrl.clear();m_sourceCache.reset();
        if(m_viewport)ShowWindow(m_viewport,SW_HIDE);Layout();UpdateTitle(); if(m_hwnd)InvalidateRect(m_hwnd,nullptr,TRUE);
    }

    bool UpscalingActive()const{return m_renderer&&m_renderer->DLSSEnabled();}
    // The rung a render should aim at. Auto reads the monitor this window is on
    // rather than the source, because the source decides only whether the rung
    // is reachable (UpscalingTarget's `grows`) while the panel decides whether
    // the pixels survive the present. 0 means no rung applies - a panel below
    // 1080 lines, or a monitor that would not report its mode - and every
    // target call refuses it, so Auto fails closed to no upscaling.
    // EnumDisplaySettingsW is the expensive half of CurrentMonitorMode, and this
    // accessor is reached from UpscalingAvailable through ToolbarState, which
    // every paint and every hover runs. MonitorFromWindow is a cheap handle
    // lookup by comparison, so the mode is cached against that handle and
    // re-read only when the window lands on a different monitor - or when
    // WM_DISPLAYCHANGE/WM_DPICHANGED says the mode under it moved.
    MonitorMode MonitorModeCached()const{
        const HMONITOR monitor=m_hwnd?MonitorFromWindow(m_hwnd,MONITOR_DEFAULTTONEAREST):nullptr;
        if(!m_monitorModeValid||monitor!=m_monitorModeHandle){
            m_monitorMode=CurrentMonitorMode(m_hwnd);
            m_monitorModeHandle=monitor;
            m_monitorModeValid=true;
        }
        return m_monitorMode;
    }
    void InvalidateMonitorMode(){m_monitorModeValid=false;}
    // A mode change under a live SR renderer does not rebuild it: the renderer
    // is swapped only through EnableUpscaling, which recreates a device, a
    // child window and an NGX feature, and a display change is exactly when a
    // device is least worth touching - the same event can accompany a device
    // loss. So the running output can outlive the rung Auto would now pick, and
    // the status line will read the new rung while the picture is still the old
    // one. Saying so in the log is the difference between a known deferral and
    // Auto looking like it ignored the panel; the next load, toggle or rung
    // change applies it.
    void ReportUpscaleRungDrift()const{
        if(!m_upscaleAuto||!UpscalingActive())return;
        const auto wanted=UpscalingTarget(m_decoder.Width(),m_decoder.Height(),EffectiveUpscaleHeight());
        if(wanted.width==m_renderer->OutputW()&&wanted.height==m_renderer->OutputH())return;
        LOG("Display mode changed under an active SR renderer. Auto now wants "
            <<wanted.width<<"x"<<wanted.height<<" (grows="<<wanted.grows<<") but the renderer is still "
            <<m_renderer->OutputW()<<"x"<<m_renderer->OutputH()
            <<"; it is rebuilt on the next load, upscaling toggle or rung change.");
    }
    uint32_t EffectiveUpscaleHeight()const{
        return m_upscaleAuto?AutoUpscaleTargetHeight(MonitorModeCached().height):m_upscaleTargetHeight;
    }
    bool UpscalingAvailable()const{
        return m_loaded&&m_renderer&&m_renderer->DLSSAvailable()&&
            HaveLastPlaybackFrame()&&
            (UpscalingActive()||UpscalingTarget(m_decoder.Width(),m_decoder.Height(),EffectiveUpscaleHeight()).grows);
    }
    // Why upscaling is not on offer, in the two words a toolbar pill has room
    // for. "Unavailable" is a verdict without a reason: it told the reporter of
    // a 436x573 photo nothing, and it reads as a broken toggle on the far more
    // common case where the source already fills the panel. The clauses are
    // UpscalingAvailable's own, in its order, so the pill can never name a
    // reason that is not the one holding it back.
    const wchar_t* UpscalingUnavailableReason()const{
        if(!m_loaded)return L"No video";
        if(!m_renderer)return L"Starting up";
        if(!m_renderer->DLSSAvailable())return L"No DLSS";
        if(!HaveLastPlaybackFrame())return L"No frame yet";
        // The remaining clause is a target that would not grow, which is two
        // opposite facts: a source that already fills the output, or a panel
        // with nowhere to put the pixels.
        if(!EffectiveUpscaleHeight())return L"Panel too small";
        return L"Meets output";
    }
    std::wstring UpscalingStatus()const{
        if(!m_upscalingError.empty())return m_upscalingError;
        if(UpscalingActive())return L"DLSS Upscaling on \u00b7 "+std::to_wstring(m_renderer->OutputW())+L"×"+std::to_wstring(m_renderer->OutputH())+
            (m_upscaleAuto?L" (auto)":L"");
        if(m_loaded&&m_decoder.Width()&&m_decoder.Height()&&!UpscalingTarget(m_decoder.Width(),m_decoder.Height(),EffectiveUpscaleHeight()).grows){
            // Two different answers the old text collapsed into one. A 4K source
            // on a 4K panel has nothing to gain; a panel below 1080 lines has
            // nowhere to put the pixels. Both are "off", for opposite reasons, and a
            // viewer who sees the wrong reason goes looking for a broken toggle.
            if(!EffectiveUpscaleHeight())return L"DLSS Upscaling off (display below 1080 lines)";
            return L"DLSS Upscaling off (source meets output)";
        }
        // The feature is called DLSS Upscaling in the menu and on the pill, so
        // the status line says that too. It used to report the same toggle as
        // "DLSS SR" one line below the control the user had just flipped.
        if(UpscalingAvailable())return L"DLSS Upscaling off";
        // A GPU or driver with no DLSS at all is the one unavailability a viewer
        // cannot fix by resizing anything, and the only one that used to arrive
        // as a bare "unavailable".
        if(m_loaded&&m_renderer&&!m_renderer->DLSSAvailable())
            return L"DLSS Upscaling unavailable (this GPU or driver has no DLSS)";
        // What is left once a DLSS-capable renderer exists and the target would
        // grow: the first frame has not been presented yet, which is the one
        // unavailability that clears itself.
        return L"DLSS Upscaling unavailable (waiting for the first frame)";
    }
    bool EnableUpscaling(uint32_t height){
        if(!m_loaded||!m_renderer||!HaveLastPlaybackFrame())return false;
        const std::shared_ptr<const VideoFrame> lastFrame=m_lastPlaybackFrame;const VideoFrame& last=*lastFrame;
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
            ConfigureRendererSource(candidate->renderer.get());
            if(candidate->renderer->Initialize(candidate->window,m_decoder.Width(),m_decoder.Height(),
                size.width,size.height,gw,gh,NVSDK_NGX_PerfQuality_Value_MaxQuality,true)&&
                RendererTookSourceLayout(candidate->renderer.get())&&
                candidate->renderer->DLSSAvailable()){
                candidate->renderer->SetColorSettings(m_colorSettings);candidate->renderer->SetComparison(EffectiveComparison());
                GuideFrame guide;
                candidate->guides.SetControls(m_guides.Controls());
                // The frame says which layout it is in. Playback opens NV12 for
                // any even-dimension BT.709-limited source, which is most of
                // them, so taking the Bgra default here read past the end of
                // the buffer instead of building guides.
                if(candidate->guides.Generate(last.bgra.data(),last.bgra.size(),m_decoder.Width(),m_decoder.Height(),
                    m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate(),
                    IdentityOf(last,m_historyGeneration,0,HistoryReset::FirstFrame),guide,
                    last.layout)){
                    ready=candidate->renderer->RenderFrame(last.bgra.data(),last.bgra.size(),
                        guide.guideGridRGBA32F.data(),guide.guideGridRGBA32F.size()*sizeof(float),guide.gridW,guide.gridH,
                        true,guide.motionVectors,float(1000.0/std::max(1.0,m_decoder.FrameRate())))&&candidate->renderer->LastFrameUsedDLSS();
                }
            }
        }
        if(ready){
            auto old=std::move(m_renderer);const HWND oldWindow=m_renderWnd;
            m_renderer=std::move(candidate->renderer);m_renderWnd=candidate->window;candidate->window=nullptr;
            m_guides=std::move(candidate->guides);m_guideReset=false;m_dlssReset=false;
            Layout();ShowWindow(m_renderWnd,SW_SHOW);old.reset();if(oldWindow)DestroyWindow(oldWindow);
            // The target itself is not adopted here. Every caller already owns
            // the rung it asked for - Auto derives it per source, a manual pick
            // stores it - and writing it back turned an Auto session into a
            // pinned one the moment SR came on.
            m_upscalingError.clear();m_upscalingRequested=true;
            LOG("Playback SR enabled: "<<m_decoder.Width()<<"x"<<m_decoder.Height()<<" -> "<<m_renderer->OutputW()<<"x"<<m_renderer->OutputH());
        }else{
            // "Unavailable" told the reporter of a 436x573 photo nothing. The
            // runtime refusing every output this source can reach is a different
            // answer from SR failing to start, and it is the one to act on.
            const bool outsideRange=candidate->renderer&&candidate->renderer->DLSSSourceOutsideRange();
            m_upscalingError=outsideRange
                ?L"DLSS Upscaling unavailable (source too small for a "+std::to_wstring(height)+L"p target)"
                :std::wstring(L"DLSS Upscaling could not start; playback is unchanged \u00b7 see the log");
            LOG("Playback SR candidate rejected; existing renderer preserved. sourceOutsideSupportedRange="<<outsideRange);
        }
        candidate.reset();m_currentSec=position;m_playStartSec=position;m_playStart=Clock::now();m_playing=playing;Audio().Pause(!playing);
        UpdateCachedStatus();InvalidateControls();return ready;
    }
    void RestoreUpscaling(){
        const uint32_t height=EffectiveUpscaleHeight();
        if(m_upscalingRequested&&UpscalingTarget(m_decoder.Width(),m_decoder.Height(),height).grows)
            EnableUpscaling(height);
    }
    void ToggleUpscaling(){
        if(!ToolbarActionEnabled(ToolbarAction::ToggleUpscaling))return;
        if(UpscalingActive()){
            m_upscalingRequested=false;m_renderer->SetDLSS(false);m_upscalingError.clear();
            if(const auto last=m_lastPlaybackFrame)RenderVideoFrame(*last,true);UpdateCachedStatus();InvalidateControls();
        }else if(!EnableUpscaling(EffectiveUpscaleHeight())){
            MessageBoxW(m_hwnd,L"DLSS upscaling could not start at this output size. Your current video is unchanged. See DLSSVideoPlayer.log for details.",L"DLSS Upscaling",MB_OK|MB_ICONINFORMATION);
        }
    }
    // `height` 0 selects Auto; anything else must name a rung. A refused
    // re-enable leaves the previous selection standing, the same way a failed
    // rung change always has.
    void SetUpscaleTarget(uint32_t height){
        if((height&&!UpscaleRungWidth(height))||m_seeking||m_seekPending||NeuralJobActive()||m_youtubeLifecycle.IsResolving())return;
        const bool automatic=height==0;
        const uint32_t resolved=automatic?AutoUpscaleTargetHeight(MonitorModeCached().height):height;
        if(UpscalingActive()){
            if(UpscalingTarget(m_decoder.Width(),m_decoder.Height(),resolved).grows){if(!EnableUpscaling(resolved))return;}
            else{m_renderer->SetDLSS(false);if(const auto last=m_lastPlaybackFrame)RenderVideoFrame(*last,true);}
        }
        m_upscaleAuto=automatic;if(!automatic)m_upscaleTargetHeight=height;
        m_upscalingError.clear();UpdateCachedStatus();InvalidateControls();
    }

    // Told to the renderer BEFORE Initialize, which is the only moment its
    // source layout can be set, and read back after: the decoder decides the
    // layout from its probe and the renderer either took it or the two disagree,
    // which is a black screen rather than an error unless somebody checks.
    // Every renderer that will be shown decoded frames has to be told their
    // layout before Initialize, not just the one in m_renderer: a candidate
    // built for a resolution change is shown the same frames and presents the
    // first quarter of a BGRA image as a Y plane if it was left on the default.
    void ConfigureRendererSource(D3D12Renderer* renderer){
        if(!renderer)return;
        renderer->SetSourceLayout(m_decoder.PixelLayout());
        renderer->SetSourceColor(m_decoder.ColorDescription());
        // Every renderer the player shows presents at its window's size.
        renderer->SetPresentFollowsWindow(true);
    }
    void ConfigureRendererSource(){ConfigureRendererSource(m_renderer.get());}
    bool RendererTookSourceLayout(D3D12Renderer* renderer){
        if(!renderer)return false;
        if(renderer->ActiveSourceLayout()==m_decoder.PixelLayout())return true;
        LOG("Renderer refused the decoder's "
            <<(m_decoder.PixelLayout()==VideoPixelLayout::Nv12?"NV12":"BGRA")
            <<" source layout; playback would present garbage, so this open fails instead.");
        return false;
    }
    bool RendererTookSourceLayout(){return RendererTookSourceLayout(m_renderer.get());}
    // Both members of a pair feed one renderer whose layout is fixed, so they
    // take whatever the main decoder achieved rather than asking again.
    bool PairPrefersNv12()const{return m_decoder.PixelLayout()==VideoPixelLayout::Nv12;}
    bool RenderVideoFrame(const VideoFrame& f,bool resetGuide) {
        // A member, not a local: its grid was a fresh 230 KB allocation on every
        // guided frame. Generate rewrites every field it reports.
        if(!m_renderer)return false; GuideFrame& g=m_guideFrame;
        // Translate the legacy reset flags into a named reason: a fresh load is
        // the first frame; a seek/reload reset outranks a decoder discontinuity,
        // which outranks a dropped frame.
        HistoryReset reason=HistoryReset::None;
        if(m_lastRenderedTs<0)reason=HistoryReset::FirstFrame;
        else if(m_guideReset||m_dlssReset)reason=HistoryReset::Seek;
        else if(resetGuide)reason=f.discontinuity?HistoryReset::Seek:HistoryReset::Drop;
        const auto guideStart=Clock::now();
        // Nothing reads motion or depth unless the NGX evaluate runs or a guide
        // debug view is up, and SetDLSS(false) runs on every media load - so on
        // the default path this whole estimator produced a grid with no
        // consumer. It reads every source pixel and allocates about eight
        // vectors per call; measured at 0.84 ms per frame at 2560x1440 against
        // a 41.7 ms budget, and 7.2% of one at 119.88 fps.
        const bool guidesNeeded=m_renderer->GuidesRequired();
        if(guidesNeeded){
            // Resuming after frames with no guides leaves the generator holding
            // a previous frame that is not this frame's predecessor, so the
            // history it would claim is wrong. Declare the discontinuity.
            if(m_guidesSkipped){m_guidesSkipped=false;m_guides.Reset();reason=HistoryReset::FirstFrame;}
            // The frame says which layout it is in; the guide generator has read
            // both since the export path started decoding to NV12, and reading a
            // NV12 buffer as BGRA is the kind of mistake that shows up as motion
            // estimated from noise rather than as a failure.
            if(!m_guides.Generate(f.bgra.data(),f.bgra.size(),m_decoder.Width(),m_decoder.Height(),m_renderer->DLSSInputW(),m_renderer->DLSSInputH(),m_decoder.FrameRate(),IdentityOf(f,m_historyGeneration,0,reason),g,f.layout))return false;
            m_historyGeneration=g.id.historyGeneration;
        } else {
            m_guidesSkipped=true;
            // No guides this frame: what the renderer and the log below read is
            // the default, as the fresh local was. The grid keeps its capacity.
            std::vector<float> grid=std::move(g.guideGridRGBA32F);grid.clear();
            g=GuideFrame{};g.guideGridRGBA32F=std::move(grid);
        }
        m_guideMsTotal+=std::chrono::duration<double,std::milli>(Clock::now()-guideStart).count();
        // This path renders without a frame identity, so the renderer never logs
        // its reset reason and a cut decided from the pixels was invisible here.
        // Both outcomes are worth a line: an accepted cut discards the DLSS
        // history, and a suppressed one is the debounce doing its job.
        if(g.sceneCut!=SceneCutStrength::None)
            LOG("Scene cut "<<(g.sceneCutSuppressed?"suppressed":"accepted")<<" during playback: strength="
                <<(g.sceneCut==SceneCutStrength::Residual?"residual":"histogram")<<" residual="<<g.sceneCutResidual
                <<" histogramOverlap="<<g.sceneCutHistogramOverlap<<" pts="<<f.timestamp100ns<<" history="<<m_historyGeneration);
        float ms=float(1000.0/std::max(1.0,m_decoder.FrameRate()));
        if(m_lastRenderedTs>=0 && f.timestamp100ns>m_lastRenderedTs){double d=double(f.timestamp100ns-m_lastRenderedTs)*1e-4;if(d>0.1&&d<500.0)ms=float(d);}
        bool r=m_dlssReset||!g.hasHistory;
        if(m_cachedPlayback){if(const auto* pair=m_synchronizedPlayback.CurrentPair())UploadComparisonReference(pair->original);}
        // Read before RememberPlaybackFrame, which may take `f`'s storage.
        const int64_t renderedTs=f.timestamp100ns;
        const auto renderStart=Clock::now();
        bool ok=m_renderer->RenderFrame(f.bgra.data(),f.bgra.size(),g.guideGridRGBA32F.data(),g.guideGridRGBA32F.size()*sizeof(float),g.gridW,g.gridH,r,g.motionVectors,ms);
        m_renderMsTotal+=std::chrono::duration<double,std::milli>(Clock::now()-renderStart).count();
        ++m_renderMsFrames;
        if(ok){
            RememberPlaybackFrame(f);
            if(m_renderer->DLSSEnabled()&&!m_renderer->LastFrameUsedDLSS()){
                m_renderer->SetDLSS(false);m_upscalingRequested=false;
                m_upscalingError=L"SR failed; original scaling restored";
                LOG("Runtime SR evaluation failed; disabled without stopping playback.");
                InvalidateControls();
            }
        }
        m_lastRenderedTs=renderedTs;m_lastGlobalX=g.globalMotionX;m_lastGlobalY=g.globalMotionY;return ok;
    }

    // The renderer latches itself unusable when a fence wait fails - the
    // device was removed, or the GPU stopped answering inside its budget - and
    // every frame and present after that fails the same way, so playback on it
    // was a dropped-frame counter over a frozen picture and a paused player
    // re-presented nothing sixty times a second. Playback stops, the reason
    // goes to the status bar, and the renderer is rebuilt once on the same
    // window in the shape LoadOriginal builds it, with the frame that was on
    // screen put back. A rebuild that fails means the device is gone: the media
    // is unloaded and the reason said out loud, because an idle surface with no
    // explanation is the report this exists to prevent. True when it acted, so
    // the caller stops what it was doing with the old renderer.
    bool RecoverUnusableRenderer(){
        if(!m_renderer||!m_renderer->GpuUnusable())return false;
        const auto reason=m_renderer->LastFenceWaitResult();
        const bool removed=reason==d3d12_renderer_detail::FenceWaitResult::DeviceRemoved;
        if(m_playing){m_currentSec=Position();m_playing=false;Audio().Pause(true);if(m_cachedPlayback)m_synchronizedPlayback.SetPaused(true);}
        LOG("Renderer unusable at "<<m_currentSec<<" s: "<<(removed?"device removed":"GPU fence wait failed")<<" (result "<<static_cast<int>(reason)<<"); rebuilding it once.");
        const auto [guideW,guideH]=TemporalGuideGenerator::AnalysisGrid(m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate());
        // Into a FRESH child window, never m_renderWnd. DXGI allows one
        // flip-model swapchain per HWND and the renderer being retired may
        // still own one - D3D12RendererDeleter keeps a renderer alive when its
        // bounded drain does not complete - so rebuilding in place answered
        // DXGI_ERROR_INVALID_CALL and this path reported "the GPU has been
        // lost" for the failure it exists to survive. EnableUpscaling and
        // CreateRendererCandidate always did it this way; only recovery did
        // not. renderer_recovery::Rebuild owns the ordering so PolicyTests can
        // assert it without a device.
        const auto rebuild=renderer_recovery::Rebuild(m_renderWnd,
            [this]{return CreateWindowExW(WS_EX_ACCEPTFILES,L"DLSSVideoRenderClassV11",nullptr,
                WS_CHILD|WS_CLIPSIBLINGS,0,0,100,100,m_viewport,nullptr,GetModuleHandleW(nullptr),this);},
            [](HWND window){DestroyWindow(window);},
            [this]{
                // The counter moves only when the deleter could not destroy it.
                const uint32_t retainedBefore=D3D12Renderer::RetainedRendererCount();
                m_renderer.reset();
                return D3D12Renderer::RetainedRendererCount()==retainedBefore;
            },
            [&](HWND window){
                m_renderer=MakeD3D12Renderer();
                ConfigureRendererSource();
                return m_renderer->Initialize(window,m_decoder.Width(),m_decoder.Height(),m_decoder.Width(),m_decoder.Height(),guideW,guideH,m_activeQuality)&&
                    RendererTookSourceLayout();
            });
        m_renderWnd=rebuild.window;
        if(rebuild.outcome!=renderer_recovery::Outcome::Rebuilt){
            LOG("Renderer rebuild failed after the GPU became unusable ("
                <<(rebuild.outcome==renderer_recovery::Outcome::WindowCreationFailed
                    ?"no render window":"device initialization")
                <<"); unloading. Old window destroyed: "<<rebuild.oldWindowDestroyed<<".");
            Unload();
            MessageBoxW(m_hwnd,T(removed?L"renderer.removed.lost":L"renderer.stalled.lost").c_str(),T(L"app.title").c_str(),MB_OK|MB_ICONERROR);
            return true;
        }
        // Created hidden so a failed rebuild never flashes an empty window,
        // and positioned before it is shown.
        Layout();ShowWindow(m_renderWnd,SW_SHOW);
        m_renderer->SetDLSS(false);m_renderer->SetColorSettings(m_colorSettings);m_renderer->SetComparison(EffectiveComparison());
        m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;
        if(HaveLastPlaybackFrame()){const auto last=m_lastPlaybackFrame;RenderVideoFrame(*last,true);}
        m_guideReset=false;m_dlssReset=false;
        RestoreUpscaling();
        m_neuralNotice=T(removed?L"renderer.removed.rebuilt":L"renderer.stalled.rebuilt");
        LOG("Renderer rebuilt after the GPU became unusable; playback paused at "<<m_currentSec<<" s.");
        UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();
        return true;
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

    static const wchar_t* QualityNameW(NVSDK_NGX_PerfQuality_Value q){switch(q){case NVSDK_NGX_PerfQuality_Value_MaxPerf:return L"Performance";case NVSDK_NGX_PerfQuality_Value_Balanced:return L"Balanced";case NVSDK_NGX_PerfQuality_Value_UltraPerformance:return L"UltraPerf";case NVSDK_NGX_PerfQuality_Value_DLAA:return L"DLAA";default:return L"Quality";}}

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
        // A cached entry can only serve its own range. An active session is
        // different now: its coverage is a set of regions with holes between
        // them, and the original plays in the holes, so every seek target in the
        // source is legal. Clamping to the newest rendered frame is what made a
        // seek back to an earlier rendered region impossible to even express -
        // the target was pulled forward to the clamp before anything could
        // answer whether it was rendered.
        if(m_cachedPlayback&&!m_liveSession&&!m_cachedRange.Whole()){low=double(m_cachedRange.start100ns)*1e-7;high=std::max(low,double(m_cachedRange.end100ns)*1e-7-1.0/std::max(1.0,m_decoder.FrameRate()));}
        if(high>0)return std::clamp(sec,low,high);return std::max(low,sec);
    }

    void RequestSeek(double sec) {
        const bool resume=m_seekPending?m_seekResumePlaying:m_playing; RequestSeek(sec,resume);
    }

    void RequestSeek(double sec,bool resumeAfter) {
        if(m_decoder.IsStillImage()){sec=0.0;resumeAfter=false;}
        // A stream's seek is a re-resolution, so take the acquired copy first when
        // one exists - the same frames, locally, and no session torn down around
        // it. Without a copy this returns false in a few microseconds and the
        // network path runs as before.
        if(!m_loaded)return;sec=ClampSeek(sec);m_lastSeekTick=GetTickCount64();
        // Playback moves again, so "playback stopped" is no longer true; a
        // decode that fails again says so again.
        m_decodeNotice.clear();
        if(NetworkPlayback()&&!AdoptAcquiredSourceCopyForPlayback()){StartYouTubeSeek(sec,resumeAfter);return;}
        if(!m_seekPending) m_currentSec=Position();
        m_pendingSeekSec=sec;m_seekResumePlaying=resumeAfter;m_seekPending=true;m_playing=false;Audio().Pause(true);m_seekPreview=sec;InvalidateControls();InvalidatePlaybackProgress();UpdateCachedStatus();
    }

    bool PerformSeek(double sec,bool resumeAfter) {
        if(!m_loaded||m_seeking)return false;SetSeeking(true);LeaveSettingsPreviewFrame();CancelPausedSettingsPreview();sec=ClampSeek(sec);LOG("Seek begin target="<<sec<<" resume="<<resumeAfter);
        // Seek is deliberately transactional and performed from Tick(), never from a mouse message.
        // Shut down the audio producer first, wait for GPU work, then restart the video decoder.
        Audio().Stop();
        if(m_renderer&&m_renderer->WaitGPU()!=d3d12_renderer_detail::FenceWaitResult::Completed){
            // The failed wait latched the renderer unusable, so this is the
            // recovery a failed frame takes: the frame that was on screen comes
            // back on a rebuilt renderer, or the media is unloaded and the
            // reason said out loud. The seek itself is abandoned either way;
            // it used to unload silently.
            LOG("Seek abandoned after GPU synchronization failure.");
            RecoverUnusableRenderer();SetSeeking(false);UpdateCachedStatus();
            return false;
        }
        if(m_cachedPlayback){
            m_haveNext=false;m_next=VideoFrame{};m_nextPairFrame.reset();m_synchronizedPlayback.SetPaused(false);
            if(!m_synchronizedPlayback.SeekSeconds(sec)||!m_synchronizedPlayback.VisibleFrame()){
                // A live pair only holds what is rendered. Outside it the
                // original takes the frame back and the session rebases there.
                // `SynchronizedPlayback` records why pairing gave up but never logs
                // it, so this used to read as "not rendered" whatever the reason -
                // behind the render start, a hole, a frame mismatch or a segment the
                // render has not reached. Name it, with the coverage it was judged
                // against, or the next report of this is unanswerable again.
                if(m_liveSession){
                    const std::string fault=m_synchronizedPlayback.LastFault();
                    // The fault text carries the coverage set, which is the only
                    // thing that can say whether this target sat in a hole or
                    // past everything rendered. The session keeps its coverage
                    // and keeps rendering; only playback moves back.
                    LOG("Live seek to "<<sec<<" s is not rendered; playing the original there."
                        <<" range=["<<double(m_liveRange.start100ns)*1e-7<<","<<double(m_liveRange.end100ns)*1e-7<<") s"
                        <<" regions="<<LiveCoverage().size()<<" fault="<<(fault.empty()?std::string("none"):fault));
                    // The playhead moves to the target before anything else can
                    // read it: the attach test and the render-target choice both
                    // ask where the user is, and leaving the abandoned position
                    // in place made them answer for the frame being left behind.
                    m_currentSec=sec;
                    DetachLivePlayback();
                    AdoptAcquiredSourceCopyForPlayback();
                    SetSeeking(false);RequestSeek(sec,resumeAfter);return false;
                }
                LOG("Cached seek failed transactionally; invalidating synchronized playback.");Unload();return false;
            }
            m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;const auto shown=VisiblePairFrame();if(!shown){LOG("Cached seek left no visible frame; invalidating synchronized playback.");Unload();return false;}const VideoFrame& frame=*shown;
            if(!RenderVideoFrame(frame,true)){
                // Silent until now, and it leaves the frame that was on screen
                // exactly where it was, with the position it already had: a seek
                // that reports nothing and changes nothing reads as a frozen
                // picture, which is what it was reported as.
                LOG("Cached seek frame render failed at "<<sec<<" s; the frame on screen is unchanged.");
                m_playing=false;m_synchronizedPlayback.SetPaused(true);SetSeeking(false);return false;
            }
            RememberRenderedCachedPair();
            // A drag preview would respawn the audio helper on every step; the
            // release restarts it once.
            m_currentSec=double(frame.timestamp100ns)*1e-7;
            if(!m_dragSeek){const bool audioOk=Audio().Start(m_path,m_currentSec);if(audioOk){Audio().SetVolume(m_muted?0.0f:m_volume);Audio().Pause(!resumeAfter);}}
            m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=resumeAfter;m_synchronizedPlayback.SetPaused(!resumeAfter);m_guideReset=false;m_dlssReset=false;SetSeeking(false);UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();return true;
        }
        m_haveNext=false;m_next=VideoFrame{};m_nextPairFrame.reset();
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
            const bool nv12=PairPrefersNv12();
            m_decoder.Close(); if(m_decoder.Open(m_path,DecodeKind(),{},nv12))got=readAt(sec,f);
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

    void SetPaused(bool pause){if(!m_loaded||m_seeking)return;if(pause==!m_playing)return;if(pause){m_currentSec=playback_timing::PausePosition(m_currentSec);m_playing=false;Audio().Pause(true);if(m_cachedPlayback)m_synchronizedPlayback.SetPaused(true);}else{if(!m_cachedPlayback&&!NetworkPlayback()&&!m_haveNext&&m_decoder.DurationSeconds()>0){RequestSeek(status_note::PlayRestartSeconds(!m_decodeNotice.empty(),m_currentSec),true);return;}m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=true;ResumeAudio();if(m_cachedPlayback)m_synchronizedPlayback.SetPaused(false);}InvalidateControls();InvalidatePlaybackProgress();}
    // Frame steps stop audio rather than respawning it per step. Anything that
    // started it since (a seek, a scrub) leaves it active, and then a resume
    // is only a resume.
    void ResumeAudio(){const bool restart=m_audioStaleAfterStep&&!Audio().Active();m_audioStaleAfterStep=false;if(!restart){Audio().Pause(false);return;}if(Audio().Start(m_path,m_currentSec))Audio().SetVolume(m_muted?0.0f:m_volume);}
    // Space pauses playback during an active session; the render behind it keeps
    // filling the buffer, and only an offline job takes the pause event.
    void TogglePause(){if(NeuralJobActive()&&!JobBehindPlayback()){SetNeuralJobPaused(!NeuralJobPaused());return;}if(m_liveBuffering){m_liveResumePlaying=!m_liveResumePlaying;InvalidateControls();return;}CancelPausedSettingsPreview();SetPaused(m_playing);}
    // A play press made while the buffer fills is remembered, not obeyed yet.
    bool LiveResumePending()const{return m_liveBuffering&&m_liveResumePlaying;}
    void StepCachedFrame(){
        if(!m_loaded||!m_cachedPlayback||m_playing||m_seeking)return;
        Audio().Pause(true);
        std::shared_ptr<const VideoFrame> held;
        if(m_haveNext){held=std::move(m_nextPairFrame);m_next={};m_haveNext=false;}
        else{
            if(!m_synchronizedPlayback.Step())return;
            const auto read=m_synchronizedPlayback.ReadNextAvailable();
            if(read!=SynchronizedReadResult::PairReady)return;
            held=VisiblePairFrame();
        }
        if(!held)return;
        const VideoFrame& frame=*held;
        if(!RenderVideoFrame(frame,frame.discontinuity))return;
        RememberRenderedCachedPair();++m_cachedPresentedFrames;
        m_currentSec=double(frame.timestamp100ns)*1e-7;
        // Restarting here spawned ffmpeg and reopened the endpoint on every
        // step; audio is restarted once, at the frame playback resumes from.
        Audio().Stop();m_audioStaleAfterStep=true;
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

    // 18 dip, up from 14, so the coverage lane along its bottom can be 7 dip
    // instead of 3: that lane is the render map, the one thing on this bar no
    // other player has, and at 3 dip it read as a hairline.
    RECT TimelineRect()const{if(!ControlsVisible())return {};RECT c{};GetClientRect(m_hwnd,&c);return RECT{Dip(18),c.bottom-Dip(30),c.right-Dip(18),c.bottom-Dip(12)};}
    // ---- The keyboard cheat sheet (? / F1) -------------------------------
    //
    // Every row comes from the live menu bar (app_menu::CollectShortcuts),
    // so a shortcut added to a menu appears here and a relabelled one cannot
    // disagree; only the keys no menu names come from a table of their own.
    struct ShortcutGroup{std::wstring name;std::vector<app_menu::ShortcutRow> rows;};
    std::vector<ShortcutGroup> ShortcutGroups()const{
        // Fullscreen detaches the menu bar while the controls are hidden; the
        // sheet still reads the one that comes back.
        HMENU menu=m_hwnd?GetMenu(m_hwnd):nullptr;if(!menu)menu=m_fullscreenMenu;
        std::vector<ShortcutGroup> groups;
        for(auto& row:app_menu::CollectShortcuts(menu,m_loc)){
            if(groups.empty()||groups.back().name!=row.group)groups.push_back(ShortcutGroup{row.group,{}});
            groups.back().rows.push_back(std::move(row));
        }
        return groups;
    }
    void ToggleShortcutSheet(){if(m_shortcutSheetOpen)HideShortcutSheet();else ShowShortcutSheet();}
    void HideShortcutSheet(){m_shortcutSheetOpen=false;if(m_shortcutWnd&&IsWindowVisible(m_shortcutWnd))ShowWindow(m_shortcutWnd,SW_HIDE);}
    void ShowShortcutSheet(){
        m_shortcutSheetOpen=true;m_shortcutGroups=ShortcutGroups();
        // Laid out and shown only for a window someone can see; the
        // regression suite drives the state against hidden windows.
        if(!m_hwnd||!IsWindowVisible(m_hwnd))return;
        if(!m_shortcutWnd){
            static constexpr const wchar_t* kClassName=L"DLSSVideoShortcutSheetV1";
            WNDCLASSW sheet{};sheet.lpfnWndProc=ShortcutSheetWndProcStatic;sheet.hInstance=GetModuleHandleW(nullptr);sheet.lpszClassName=kClassName;sheet.hCursor=LoadCursor(nullptr,IDC_ARROW);
            if(!RegisterClassW(&sheet)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
            m_shortcutWnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,kClassName,nullptr,WS_POPUP,0,0,1,1,m_hwnd,nullptr,GetModuleHandleW(nullptr),this);
            if(!m_shortcutWnd)return;
        }
        // Columns are as wide as the widest action and the widest keys, as
        // measured in the font they are drawn in.
        int actionWidth=0,keysWidth=0;
        if(HDC dc=GetDC(m_shortcutWnd)){
            const HGDIOBJ old=SelectObject(dc,m_fontSmall?m_fontSmall:m_font);
            for(const auto& group:m_shortcutGroups)for(const auto& row:group.rows){
                SIZE size{};GetTextExtentPoint32W(dc,row.action.c_str(),int(row.action.size()),&size);actionWidth=std::max<int>(actionWidth,size.cx);
                GetTextExtentPoint32W(dc,row.keys.c_str(),int(row.keys.size()),&size);keysWidth=std::max<int>(keysWidth,size.cx);
            }
            SelectObject(dc,old);ReleaseDC(m_shortcutWnd,dc);
        }
        m_shortcutMetrics=shortcut_sheet::Metrics{Dip(22),Dip(30),Dip(12),actionWidth+Dip(20)+keysWidth,Dip(28),Dip(20),Dip(36),Dip(26)};
        RECT client{};GetClientRect(m_hwnd,&client);
        std::vector<size_t> rows;for(const auto& group:m_shortcutGroups)rows.push_back(group.rows.size());
        m_shortcutLayout=shortcut_sheet::LayoutSheet(rows,m_shortcutMetrics,int(client.right-client.left),int(client.bottom-client.top));
        POINT origin{(client.right-client.left-m_shortcutLayout.width)/2,std::max<LONG>(0,(client.bottom-client.top-m_shortcutLayout.height)/2)};
        ClientToScreen(m_hwnd,&origin);
        SetWindowPos(m_shortcutWnd,HWND_TOP,origin.x,origin.y,m_shortcutLayout.width,m_shortcutLayout.height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        InvalidateRect(m_shortcutWnd,nullptr,FALSE);
    }
    void PaintShortcutSheet(HWND window){
        PAINTSTRUCT paint{};HDC dc=BeginPaint(window,&paint);if(!dc)return;
        RECT client{};GetClientRect(window,&client);
        HBRUSH background=CreateSolidBrush(ui_palette::Window);FillRect(dc,&client,background);DeleteObject(background);
        HBRUSH border=CreateSolidBrush(RGB(62,65,70));FrameRect(dc,&client,border);DeleteObject(border);
        SetBkMode(dc,TRANSPARENT);
        const auto& metrics=m_shortcutMetrics;
        const HGDIOBJ old=SelectObject(dc,m_font);SetTextColor(dc,ui_palette::PrimaryText);
        RECT title{metrics.padding,metrics.padding,client.right-metrics.padding,metrics.padding+metrics.titleHeight};
        const std::wstring titleText=T(L"shortcuts.title");DrawTextW(dc,titleText.c_str(),-1,&title,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOPREFIX);
        HBRUSH rule=CreateSolidBrush(RGB(54,56,61));
        for(size_t index=0;index<m_shortcutGroups.size()&&index<m_shortcutLayout.groups.size();++index){
            const auto& placement=m_shortcutLayout.groups[index];if(!placement.visible)continue;
            const auto& group=m_shortcutGroups[index];
            const int left=metrics.padding+placement.column*(metrics.columnWidth+metrics.columnGap);
            int y=m_shortcutLayout.columnTop+placement.top;
            SelectObject(dc,m_font);SetTextColor(dc,ui_palette::PrimaryText);
            RECT header{left,y,left+metrics.columnWidth,y+metrics.headerHeight-Dip(6)};DrawTextW(dc,group.name.c_str(),-1,&header,DT_LEFT|DT_BOTTOM|DT_SINGLELINE|DT_NOPREFIX);
            RECT line{left,y+metrics.headerHeight-Dip(3),left+metrics.columnWidth,y+metrics.headerHeight-Dip(2)};FillRect(dc,&line,rule);
            y+=metrics.headerHeight;SelectObject(dc,m_fontSmall?m_fontSmall:m_font);
            for(const auto& row:group.rows){
                RECT cell{left,y,left+metrics.columnWidth,y+metrics.rowHeight};
                SetTextColor(dc,ui_palette::SecondaryText);DrawTextW(dc,row.action.c_str(),-1,&cell,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
                SetTextColor(dc,ui_palette::PrimaryText);DrawTextW(dc,row.keys.c_str(),-1,&cell,DT_RIGHT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
                y+=metrics.rowHeight;
            }
        }
        DeleteObject(rule);
        SelectObject(dc,m_fontSmall?m_fontSmall:m_font);SetTextColor(dc,ui_palette::SecondaryText);
        RECT footer{metrics.padding,client.bottom-metrics.padding-metrics.footerHeight,client.right-metrics.padding,client.bottom-metrics.padding};
        const std::wstring hint=T(L"shortcuts.close_hint");DrawTextW(dc,hint.c_str(),-1,&footer,DT_LEFT|DT_BOTTOM|DT_SINGLELINE|DT_NOPREFIX);
        SelectObject(dc,old);EndPaint(window,&paint);
    }
    static LRESULT CALLBACK ShortcutSheetWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l){
        if(m==WM_NCCREATE){SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));return DefWindowProcW(h,m,w,l);}
        auto* self=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        switch(m){
        // Never takes focus - the keys it lists keep working while it is up -
        // and a click anywhere on it puts it away.
        case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:case WM_RBUTTONDOWN:if(self)self->HideShortcutSheet();return 0;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:if(self){self->PaintShortcutSheet(h);return 0;}break;
        case WM_NCDESTROY:if(self&&self->m_shortcutWnd==h)self->m_shortcutWnd=nullptr;break;
        }
        return DefWindowProcW(h,m,w,l);
    }
    // ---- The timeline's render map: chapters, the hover preview ----------
    //
    // The file the timeline asks about: the loaded local file, or the local
    // copy of a stream once there is one. A stream still on the network has
    // neither chapters nor a cheap frame to show, and asking would be a second
    // network open behind playback's back.
    std::filesystem::path TimelineMediaFile()const{return m_loaded?ExportSourceFile():std::filesystem::path{};}
    // Runs with the status line, which every load, unload and state change
    // reaches: a different file is a new generation, which drops the chapters
    // and thumbnails of the old one and asks for the new one's chapter list.
    void SyncTimelineMedia(){
        const std::filesystem::path file=TimelineMediaFile();
        if(file==m_timelineFile)return;
        m_timelineFile=file;++m_timelineGeneration;
        m_chapters.clear();m_thumbnails.Clear();ClearTimelineHover();
        if(!file.empty()&&m_hwnd)m_timelineWorker.RequestChapters(m_hwnd,WM_TIMELINE_MEDIA,m_timelineGeneration,file);
    }
    void CompleteTimelineMedia(){
        if(auto chapters=m_timelineWorker.TakeChapters();chapters&&chapters->generation==m_timelineGeneration){
            if(!chapters->chapters.empty())LOG("Timeline: "<<chapters->chapters.size()<<" chapter(s) in the loaded file.");
            m_chapters=std::move(chapters->chapters);InvalidatePlaybackProgress();
        }
        bool shown=false;
        for(auto& thumbnail:m_timelineWorker.TakeThumbnails()){
            if(thumbnail.generation!=m_timelineGeneration)continue;
            shown=shown||(m_previewKey&&*m_previewKey==thumbnail.key);
            const int64_t key=thumbnail.key;m_thumbnails.Insert(key,std::move(thumbnail));
        }
        if(shown&&m_timelineHoverX)ShowTimelinePreview();
    }
    // What the hover says about the moment under the cursor, from the same
    // coverage the lanes are painted from.
    timeline::HoverFacts TimelineHoverFacts(double seconds)const{
        timeline::HoverFacts facts{};facts.seconds=seconds;facts.durationKnown=m_decoder.DurationSeconds()>0.0;
        const int64_t at=static_cast<int64_t>(std::llround(seconds*1e7));
        if(m_liveSession){
            facts.rendered=SpanContaining(LiveCoverage(),at).has_value();
            const auto progress=RenderChipProgress();
            if(progress.active&&progress.fraction<1.0)facts.secondsToFullCoverage=progress.etaSeconds;
        }else if(m_cachedPlayback)facts.rendered=m_cachedRange.Whole()||(at>=m_cachedRange.start100ns&&at<m_cachedRange.end100ns);
        if(const timeline::Chapter* chapter=timeline::ChapterAt(m_chapters,seconds)){
            facts.chapter=chapter->title.empty()?L"Chapter "+std::to_wstring((chapter-m_chapters.data())+1):chapter->title;
        }
        return facts;
    }
    // Where a hover thumbnail comes from: the finished neural render when the
    // moment is inside one - that is the picture this player exists to show -
    // otherwise the original. A live session's segments are not used: they
    // are still being written and are deleted with the session.
    struct ThumbnailSource{std::filesystem::path file;double seconds{};int64_t key{};};
    std::optional<ThumbnailSource> TimelineThumbnailSource(double seconds)const{
        const double duration=m_decoder.DurationSeconds();
        if(!(duration>0.0)||m_timelineFile.empty())return std::nullopt;
        const int64_t bucket=timeline::ThumbnailBucket(seconds,duration);
        const double at=timeline::ThumbnailBucketSeconds(bucket,duration);
        const int64_t at100ns=static_cast<int64_t>(std::llround(at*1e7));
        if(m_cachedPlayback&&!m_liveSession&&!m_neuralPath.empty()&&
           (m_cachedRange.Whole()||(at100ns>=m_cachedRange.start100ns&&at100ns<m_cachedRange.end100ns)))
            return ThumbnailSource{m_neuralPath,std::max(0.0,at-double(m_cachedRange.start100ns)*1e-7),bucket*2+1};
        return ThumbnailSource{m_timelineFile,at,bucket*2};
    }
    SIZE TimelineThumbnailSize()const{return timeline::ThumbnailSize(m_dar,Dip(176));}
    void UpdateTimelineHover(int x,int y){
        if(!m_loaded||!ControlsVisible()){ClearTimelineHover();return;}
        RECT zone=TimelineRect();InflateRect(&zone,0,Dip(6));
        if(!m_dragSeek&&!PtIn(zone,x,y)){ClearTimelineHover();return;}
        const RECT track=TimelineRect();
        const int clamped=std::clamp(x,int(track.left),int(std::max(track.left,track.right-1)));
        if(m_timelineHoverX&&*m_timelineHoverX==clamped)return;
        m_timelineHoverX=clamped;ShowTimelinePreview();
    }
    void ClearTimelineHover(){
        m_timelineHoverX.reset();m_previewKey.reset();m_previewText.clear();
        if(m_previewWnd&&IsWindowVisible(m_previewWnd))ShowWindow(m_previewWnd,SW_HIDE);
    }
    // The label is immediate; the thumbnail is asked for and appears when it
    // arrives, so a hover never waits on a decode.
    void ShowTimelinePreview(){
        if(!m_timelineHoverX)return;
        const double duration=m_decoder.DurationSeconds();
        const double seconds=duration>0.0?SecondsFromX(*m_timelineHoverX):0.0;
        m_previewText=timeline::HoverText(TimelineHoverFacts(seconds));
        m_previewKey.reset();
        const SIZE thumbSize=TimelineThumbnailSize();
        if(const auto source=TimelineThumbnailSource(seconds)){
            m_previewKey=source->key;
            if(!m_thumbnails.Find(source->key))
                m_timelineWorker.RequestThumbnail(m_hwnd,WM_TIMELINE_MEDIA,m_timelineGeneration,source->key,source->file,source->seconds,thumbSize);
        }
        // Painted only for a window someone can see; the regression suite
        // drives all of the above against hidden windows.
        if(!m_hwnd||!IsWindowVisible(m_hwnd))return;
        if(!m_previewWnd){
            static constexpr const wchar_t* kClassName=L"DLSSVideoTimelinePreviewV1";
            WNDCLASSW preview{};preview.lpfnWndProc=PreviewWndProcStatic;preview.hInstance=GetModuleHandleW(nullptr);preview.lpszClassName=kClassName;preview.hCursor=LoadCursor(nullptr,IDC_ARROW);
            if(!RegisterClassW(&preview)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
            m_previewWnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,kClassName,nullptr,WS_POPUP,0,0,1,1,m_hwnd,nullptr,GetModuleHandleW(nullptr),this);
            if(!m_previewWnd)return;
        }
        const auto* thumbnail=m_previewKey?m_thumbnails.Find(*m_previewKey):nullptr;
        SIZE textSize{};
        if(HDC dc=GetDC(m_previewWnd)){const HGDIOBJ old=SelectObject(dc,m_fontSmall?m_fontSmall:m_font);GetTextExtentPoint32W(dc,m_previewText.c_str(),int(m_previewText.size()),&textSize);SelectObject(dc,old);ReleaseDC(m_previewWnd,dc);}
        textSize.cy=std::max<LONG>(textSize.cy,Dip(18));
        POINT anchor{*m_timelineHoverX,TimelineRect().top};ClientToScreen(m_hwnd,&anchor);
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(m_hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
        const auto layout=timeline::LayoutPreview(anchor,thumbnail?thumbnail->size:SIZE{},textSize,Dip(5),Dip(10),monitor.rcWork);
        m_previewLayout=layout;
        SetWindowPos(m_previewWnd,HWND_TOP,layout.window.left,layout.window.top,layout.window.right-layout.window.left,
                     layout.window.bottom-layout.window.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        InvalidateRect(m_previewWnd,nullptr,FALSE);
    }
    void PaintTimelinePreview(HWND window){
        PAINTSTRUCT paint{};HDC dc=BeginPaint(window,&paint);if(!dc)return;
        RECT client{};GetClientRect(window,&client);
        HBRUSH background=CreateSolidBrush(ui_palette::Window);FillRect(dc,&client,background);DeleteObject(background);
        HBRUSH border=CreateSolidBrush(RGB(62,65,70));FrameRect(dc,&client,border);DeleteObject(border);
        if(const auto* thumbnail=m_previewKey?m_thumbnails.Find(*m_previewKey):nullptr;thumbnail&&m_previewLayout.thumbnail.right>m_previewLayout.thumbnail.left){
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(info.bmiHeader);info.bmiHeader.biWidth=thumbnail->size.cx;
            info.bmiHeader.biHeight=-thumbnail->size.cy;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            const RECT& target=m_previewLayout.thumbnail;
            StretchDIBits(dc,target.left,target.top,target.right-target.left,target.bottom-target.top,0,0,thumbnail->size.cx,thumbnail->size.cy,
                          thumbnail->bgra.data(),&info,DIB_RGB_COLORS,SRCCOPY);
        }
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,ui_palette::PrimaryText);
        const HGDIOBJ old=SelectObject(dc,m_fontSmall?m_fontSmall:m_font);RECT text=m_previewLayout.text;
        DrawTextW(dc,m_previewText.c_str(),-1,&text,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        SelectObject(dc,old);EndPaint(window,&paint);
    }
    static LRESULT CALLBACK PreviewWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l){
        if(m==WM_NCCREATE){SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));return DefWindowProcW(h,m,w,l);}
        auto* self=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        switch(m){
        // Looked at, never touched: the cursor under it belongs to the bar.
        case WM_NCHITTEST:return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:if(self){self->PaintTimelinePreview(h);return 0;}break;
        case WM_NCDESTROY:if(self&&self->m_previewWnd==h)self->m_previewWnd=nullptr;break;
        }
        return DefWindowProcW(h,m,w,l);
    }
    // The hatched stretch the render is filling right now, animated with the
    // activity timer that is already running while a job is.
    void DrawRenderingNow(HDC dc,const RECT& lane,LONG left,LONG right){
        const int saved=SaveDC(dc);if(!saved)return;
        IntersectClipRect(dc,left,lane.top,right,lane.bottom);
        const RECT area{left,lane.top,right,lane.bottom};
        HBRUSH base=CreateSolidBrush(RGB(30,74,68));FillRect(dc,&area,base);DeleteObject(base);
        HPEN pen=CreatePen(PS_SOLID,std::max(1,Dip(2)),ui_palette::NeuralCoverage);SelectObject(dc,pen);
        const LONG height=lane.bottom-lane.top,period=std::max<LONG>(4,Dip(6));
        const LONG phase=m_activityMotionEnabled?LONG((ActivityElapsedMs()/60)%uint64_t(period)):0;
        for(LONG x=left-height-period+phase;x<right+period;x+=period){MoveToEx(dc,x,lane.bottom,nullptr);LineTo(dc,x+height,lane.top);}
        RestoreDC(dc,saved);DeleteObject(pen);
    }
    IdleSurfaceLayout IdleLayout()const{RECT c{};GetClientRect(m_hwnd,&c);return LayoutIdleSurface(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd));}
    std::vector<ToolbarItem> ToolbarItems()const{if(!ControlsVisible())return {};RECT c{};GetClientRect(m_hwnd,&c);return LayoutToolbar(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd));}
    std::vector<ToolbarItem> FocusableItems()const{if(m_loaded)return ToolbarItems();const auto idle=IdleLayout();return{idle.actions.begin(),idle.actions.end()};}
    ToolbarAvailability ToolbarState()const{return{m_loaded,m_seeking||m_seekPending,m_renderer!=nullptr,YouTubePlaybackAvailable(),m_youtubeLifecycle.IsResolving()||(NeuralJobActive()&&!JobBehindPlayback()),m_cachedPlayback&&m_havePresentedPair&&m_renderer!=nullptr,m_liveSession||LiveSessionAvailable()||StillImageRenderAvailable(),UpscalingAvailable(),FrameGenerationAvailable(),m_frameGenWorker.joinable()&&!m_frameGenCancelling};}
    std::optional<RECT> VolumeRect()const{if(!ControlsVisible())return std::nullopt;RECT c{};GetClientRect(m_hwnd,&c);const auto items=ToolbarItems();return LayoutVolumeSlider(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd),items);}
    bool PtIn(const RECT&r,int x,int y)const{return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}

    RECT TimeTextRect()const{RECT c{};GetClientRect(m_hwnd,&c);return RECT{Dip(16),c.bottom-Dip(55),Dip(142),c.bottom-Dip(32)};}
    // The status line and its chips. The line used to stop 203 dip short of
    // the right edge whenever the volume slider was shown, but the slider and
    // its label sit in the toolbar row above (69 and 75 dip up), so that was
    // room nothing used; the chips take it now.
    RECT StatusRowRect()const{RECT c{};GetClientRect(m_hwnd,&c);return RECT{Dip(145),c.bottom-Dip(55),c.right-Dip(16),c.bottom-Dip(32)};}
    RECT StatusRect()const{RECT row=StatusRowRect();row.left-=Dip(2);return row;}
    status_chips::RowLayout StatusRowLayout()const{
        std::array<bool,status_chips::kChipCount> visible{};
        for(size_t index=0;index<status_chips::kChipCount;++index)visible[index]=m_cachedChips[index].visible;
        return status_chips::LayoutRow(StatusRowRect(),ActiveWindowDpi(m_hwnd),visible);
    }
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
        UpdateStatusChips();SyncTimelineMedia();
        const std::wstring status=BuildStatusText();if(status==m_cachedStatus)return;
        m_cachedStatus=status;if(m_hwnd){if(m_loaded){const RECT dirty=StatusRect();InvalidateRect(m_hwnd,&dirty,FALSE);}else InvalidateRect(m_hwnd,nullptr,FALSE);}
    }
    // How far the render behind playback has got, for the render chip. A live
    // session reports its whole range, holes and all, at the pace it is
    // actually managing; a settings preview reports its own job.
    status_chips::RenderProgress RenderChipProgress()const{
        if(m_liveSession&&m_liveRange.end100ns>m_liveRange.start100ns){
            const CoverageSpan range{m_liveRange.start100ns,m_liveRange.end100ns};
            const double fraction=LiveSessionFinished()?1.0:std::min(CoveredFraction(LiveCoverage(),range),0.999);
            const double remaining=(1.0-fraction)*double(range.Width())*1e-7;
            return {true,fraction,status_chips::SecondsToFullCoverage(remaining,LivePaceRatio())};
        }
        if(m_previewJob&&NeuralJobActive()&&m_neuralProgress.totalFrames>0){
            const double fraction=double(std::min(m_neuralProgress.completedFrames,m_neuralProgress.totalFrames))/double(m_neuralProgress.totalFrames);
            const double eta=std::chrono::duration<double>(m_neuralProgress.estimatedRemaining).count();
            return {true,fraction,eta>0.0?std::optional<double>(eta):std::nullopt};
        }
        return {};
    }
    status_chips::Snapshot BuildStatusChips()const{
        return status_chips::Build(m_loaded&&m_renderer!=nullptr,RenderChipProgress(),m_submitFps,
                                   m_decoder.FrameRate(),m_droppedFrames);
    }
    // Called with the status line, which every state change and every
    // presented frame already reaches. Only a changed chip repaints, and only
    // a changed FACT - see status_chips::Flash - starts a flash.
    void UpdateStatusChips(){
        const status_chips::Snapshot chips=BuildStatusChips();
        if(chips==m_cachedChips)return;
        bool layoutChanged=false;
        for(size_t index=0;index<status_chips::kChipCount;++index)layoutChanged=layoutChanged||chips[index].visible!=m_cachedChips[index].visible;
        m_cachedChips=chips;
        const bool flashed=m_chipFlash.Observe(chips,Clock::now());
        if(!m_hwnd||!m_loaded)return;
        // A chip appearing or going moves the line's right edge, so the whole
        // row repaints; otherwise only the chips do.
        RECT dirty=StatusRect();
        if(!layoutChanged){LONG left=dirty.right;for(const RECT& chip:StatusRowLayout().chips)if(chip.right>chip.left)left=std::min(left,chip.left);dirty.left=left;}
        InvalidateRect(m_hwnd,&dirty,FALSE);
        if(flashed&&!m_chipFlashTimer)m_chipFlashTimer=SetTimer(m_hwnd,kChipFlashTimerId,m_activityMotionEnabled?40u:UINT(status_chips::kFlashDuration.count()),nullptr);
    }
    void AnimateStatusChips(){
        if(m_hwnd){const RECT dirty=StatusRect();InvalidateRect(m_hwnd,&dirty,FALSE);}
        if(!m_chipFlash.Animating(Clock::now())&&m_chipFlashTimer){if(m_hwnd)KillTimer(m_hwnd,m_chipFlashTimer);m_chipFlashTimer=0;}
    }
    // One chip: a quiet surface normally, lit toward its own colour for a
    // moment after the fact it reports changes. Teal for the render, as the
    // coverage lane and a working pill are; blue for the rate, as progress is;
    // amber for a dropped frame, the only one of the three that is never good
    // news.
    void DrawStatusChip(HDC dc,const RECT& bounds,const status_chips::Content& chip,COLORREF accent,double level){
        if(bounds.right<=bounds.left||!chip.visible)return;
        const auto mix=[](COLORREF from,COLORREF to,double amount){
            const auto channel=[&](int a,int b){return BYTE(std::clamp(int(std::lround(a+(b-a)*amount)),0,255));};
            return RGB(channel(GetRValue(from),GetRValue(to)),channel(GetGValue(from),GetGValue(to)),channel(GetBValue(from),GetBValue(to)));
        };
        HBRUSH brush=CreateSolidBrush(mix(ui_palette::Inactive,accent,0.35*level));HPEN pen=CreatePen(PS_SOLID,1,mix(RGB(62,65,70),accent,level));
        const HGDIOBJ oldBrush=SelectObject(dc,brush),oldPen=SelectObject(dc,pen);
        const int radius=std::max(1,Dip(4));RoundRect(dc,bounds.left,bounds.top,bounds.right,bounds.bottom,radius*2,radius*2);
        SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(brush);DeleteObject(pen);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,chip.quiet&&level<=0.0?ui_palette::SecondaryText:ui_palette::PrimaryText);
        RECT text=bounds;InflateRect(&text,-Dip(status_chips::kChipPaddingDip),0);
        DrawTextW(dc,chip.text.c_str(),-1,&text,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
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
        if(!seeking)RunDeferredNeuralToggle();
    }
    // The toggle press that arrived mid-seek, honoured once the seek lands. The
    // press is dropped rather than queued twice if the seek left the player
    // somewhere the toggle cannot act.
    void RunDeferredNeuralToggle(){
        if(!m_neuralToggleDeferred||m_seeking||m_seekPending)return;
        m_neuralToggleDeferred=false;
        if(!ToolbarActionEnabled(ToolbarAction::ToggleNeuralRendering)){
            LOG("Deferred neural rendering toggle dropped: the seek did not leave it available.");
            UpdateCachedStatus();InvalidateControls();return;
        }
        LOG("Running the neural rendering toggle that arrived during the seek.");
        ToggleNeuralRendering();
    }

    // `working` is the fourth state: a conversion or a render is running. It
    // defaults false, so every control that is simply on or off says so by
    // saying nothing.
    struct ToolbarButtonContent{UiIcon icon;std::wstring label;bool enabled;bool active;bool working{false};};

    ToolbarButtonContent ButtonContent(ToolbarAction action,bool idleSurface=false)const{
        const bool rendererReady=m_renderer!=nullptr;
        const bool enabled=IsToolbarActionEnabled(action,ToolbarState());
        switch(action){
        case ToolbarAction::Open:return{UiIcon::Open,T(OpenActionLabelKey(idleSurface).data()),enabled,false};
        case ToolbarAction::OpenYouTube:return{UiIcon::YouTube,T(L"idle.youtube"),enabled,false};
        case ToolbarAction::Back10:return{UiIcon::Rewind,L"10s",enabled,false};
        // A press on play while a session buffers can only be remembered; the
        // control used to keep saying "Play", so the obvious second press
        // cancelled the first and the picture never started.
        case ToolbarAction::PlayPause:{const bool playing=m_playing||LiveResumePending();return{playing?UiIcon::Pause:UiIcon::Play,playing?L"Pause":L"Play",enabled,playing};}
        // A toggle pressed during a seek is queued rather than dropped, and the
        // label says so: a dead-looking key is how it read before.
        case ToolbarAction::ToggleNeuralRendering:{const bool active=(m_cachedPlayback&&m_comparisonView==ComparisonView::Neural)||m_previewShown;const bool cachedPair=m_cachedPlayback&&m_havePresentedPair&&rendererReady;const std::wstring label=m_neuralToggleDeferred?L"Neural Rendering · Queued for the seek":m_previewJob?L"Neural Rendering · Previewing settings":m_previewShown?L"Neural Rendering · Settings preview":enabled?(active?L"Neural Rendering · On":L"Neural Rendering · Off"):(cachedPair?std::wstring(L"Neural Rendering · Seeking · ")+(active?L"On":L"Off"):(NeuralJobActive()?L"Neural Rendering · Preparing cache":L"Neural Rendering · No cache"));return{UiIcon::Sparkles,label,enabled,active,m_previewJob!=0||NeuralJobActive()};}
        case ToolbarAction::Stop:return{UiIcon::Stop,L"Stop",enabled,false};
        case ToolbarAction::Forward10:return{UiIcon::FastForward,L"10s",enabled,false};
        case ToolbarAction::Mute:return{m_muted?UiIcon::VolumeOff:UiIcon::Volume,m_muted?L"Sound":L"Mute",enabled,m_muted};
        // The unavailable arm carries the reason, not the verdict: a pill that
        // says only "Unavailable" sends a viewer looking for a broken toggle
        // when the answer is usually that their source already fills the panel.
        case ToolbarAction::ToggleUpscaling:return{UiIcon::Upscaling,
            UpscalingAvailable()?(UpscalingActive()?std::wstring(L"DLSS Upscaling \u00b7 On")
                                                   :std::wstring(L"DLSS Upscaling \u00b7 Off"))
                                :std::wstring(L"DLSS Upscaling \u00b7 ")+UpscalingUnavailableReason(),
            enabled,UpscalingActive()};
        // An action, not a toggle: it starts a conversion, and while one runs
        // the pill is the way to stop it rather than an inert label. Every arm
        // comes from the one state machine, so the pill, the menu item and the
        // status line cannot disagree, and the icon is its own rather than a
        // third Sparkles beside the two real toggles.
        case ToolbarAction::ToggleFrameGeneration:{
            const auto ui=FrameGenerationUiNow();
            using S=FrameGenerationUiState;
            switch(ui.state){
                case S::Converting:return{UiIcon::FrameGeneration,T(L"framegen.pill.cancel"),true,true,true};
                case S::Stopping:return{UiIcon::FrameGeneration,T(L"framegen.pill.cancel"),false,true,true};
                case S::Busy:return{UiIcon::FrameGeneration,T(L"framegen.pill.busy"),false,false};
                // The stream cases: one offers the copy, the other says it is
                // being fetched. Neither is "Unavailable" - the first is the
                // only unavailable-looking state the user can act on.
                case S::NeedsSourceCopy:return{UiIcon::FrameGeneration,T(L"framegen.pill.get_copy"),enabled,false};
                case S::CopyingSource:return{UiIcon::FrameGeneration,T(L"framegen.pill.copying"),false,true,true};
                case S::NoLocalCopy:
                case S::Refused:return{UiIcon::FrameGeneration,T(L"framegen.pill.unavailable"),false,false};
                // Unchecked reads "Generate" like Ready: the click is what
                // measures the runtime, so offering it is the honest label.
                case S::Unchecked:
                case S::Ready:break;
            }
            return{UiIcon::FrameGeneration,T(L"framegen.pill.generate"),enabled,false};
        }
        case ToolbarAction::Aspect:return{UiIcon::Crop,m_fill?L"Fit":L"Fill",enabled,m_fill};
        case ToolbarAction::Adjustments:return{UiIcon::Adjustments,L"Color",enabled,m_adjustWnd!=nullptr};
        case ToolbarAction::DebugView:{const bool active=rendererReady&&m_renderer->GetDebugView()!=D3D12Renderer::DebugView::Final;return{UiIcon::Debug,L"Debug",enabled,active};}
        case ToolbarAction::Fullscreen:return{UiIcon::Maximize,L"Full",enabled,m_fullscreen};
        case ToolbarAction::None:break;
        }
        return{UiIcon::Warning,L"Unavailable",false,false};
    }

    bool ToolbarActionEnabled(ToolbarAction action)const{return IsToolbarActionEnabled(action,ToolbarState());}

    void DrawButton(HDC dc,ToolbarAction action,UiIcon icon,const std::wstring&label,const RECT&r,bool enabled,bool active,bool hover,bool pressed,bool focus,bool compact,bool working=false){
        const ButtonVisual visual=ResolveButtonVisual(ButtonState{enabled,active,working,hover,pressed,focus});
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
        GetTextExtentPoint32W(dc,label.c_str(),static_cast<int>(label.size()),&textSize);
        const bool featureLabel=action==ToolbarAction::ToggleNeuralRendering||action==ToolbarAction::ToggleUpscaling||action==ToolbarAction::ToggleFrameGeneration;
        int neededWidth=iconSize.cx+textSize.cx+Dip(2*kButtonHorizontalInsetDip+kButtonIconLabelGapDip);
        // A feature pill the layout has narrowed keeps its state and drops its
        // name, rather than ellipsising the state - the one part of the label
        // that changes - off the end. The icon and the tooltip carry the name.
        std::wstring shown=label;
        if(featureLabel&&neededWidth>r.right-r.left){
            shown=std::wstring(FeaturePillStateLabel(label));
            GetTextExtentPoint32W(dc,shown.c_str(),static_cast<int>(shown.size()),&textSize);
            neededWidth=iconSize.cx+textSize.cx+Dip(2*kButtonHorizontalInsetDip+kButtonIconLabelGapDip);
        }
        SelectObject(dc,measuredText);
        const bool showText=!showIcon||featureLabel||neededWidth<=r.right-r.left;
        const ButtonContentLayout content=LayoutButtonContent(r,iconSize,showText?textSize:SIZE{},stacked&&showText,ActiveWindowDpi(m_hwnd));
        if(showIcon){const HGDIOBJ oldFont=SelectObject(dc,m_iconFont);RECT iconRect=content.icon;DrawTextW(dc,&glyph,1,&iconRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);SelectObject(dc,oldFont);}
        if(showText){const HGDIOBJ oldFont=SelectObject(dc,textFont);RECT textRect=content.text;
            DrawTextW(dc,shown.c_str(),-1,&textRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
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

    // The source has to be copied locally before one frame can be rendered, and
    // for a long video that is minutes. Reporting nothing there is what let a
    // working download read as a hang.
    std::wstring AcquisitionDetailText()const{
        std::wstring text=L"Downloading the source · "+std::to_wstring(m_neuralProgress.bytes/(1024u*1024u))+L" MiB";
        if(m_neuralProgress.expectedSeconds>0.0&&m_neuralProgress.acquiredSeconds>0.0){
            const double fraction=std::clamp(m_neuralProgress.acquiredSeconds/m_neuralProgress.expectedSeconds,0.0,1.0);
            text+=L" · "+std::to_wstring(static_cast<int>(std::lround(fraction*100.0)))+L"%";
        }
        return text;
    }

    void RenderUi(HDC dc,const RECT& c){
        m_neuralCancelBounds={};
        HBRUSH windowBg=CreateSolidBrush(ui_palette::Window);FillRect(dc,&c,windowBg);DeleteObject(windowBg);
        // An active session renders behind live playback, so it never takes the
        // window: its feedback is the buffering panel and the coverage lane.
        // The export gets the same panel. It is a minutes-long job that writes a
        // file, which is exactly what this surface was built to report, and a
        // second progress widget for the same shape of work would be a second
        // thing to keep consistent for no benefit.
        if(m_stageExport.running){
            const PreRenderSurfaceLayout surface=LayoutPreRenderSurface(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd));
            const uint64_t total=m_stageExport.totalFrames,done=std::min(m_stageExport.completedFrames,total?total:m_stageExport.completedFrames);
            const auto visual=ResolveActivityVisual(surface.progressTrack,ActivityElapsedMs(),done,total,total>0,m_activityMotionEnabled);
            DrawActivitySpinner(dc,surface.spinner,visual.spinnerStep);
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(242,243,245));HGDIOBJ oldFont=SelectObject(dc,m_font);
            RECT row=surface.title;const std::wstring title=T(L"export.progress.title");
            DrawTextW(dc,title.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            SelectObject(dc,m_fontSmall);SetTextColor(dc,ui_palette::SecondaryText);
            // "Pass 1 of 2 - Super Resolution and neural rendering": which of
            // the two long things is happening, and how many are left.
            wchar_t phase[256];
            swprintf_s(phase,L"Pass %u of %u · %s",m_stageExport.pass,std::max<uint32_t>(1,m_stageExport.passes),
                       T(m_stageExport.passKey?m_stageExport.passKey:L"export.progress.writing").c_str());
            row=surface.phase;DrawTextW(dc,phase,-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            std::wstring frames;
            if(total>0){
                wchar_t line[128];swprintf_s(line,L"%u%% · %llu / %llu frames",visual.percent,
                                             static_cast<unsigned long long>(done),static_cast<unsigned long long>(total));
                frames=line;
            }
            row=surface.frameCount;DrawTextW(dc,frames.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            // Elapsed from this export's own start, and an estimate only once
            // there are enough frames for one to mean anything.
            const double elapsedSeconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-m_stageExport.started).count();
            std::wstring timing=L"Elapsed "+TimeText(elapsedSeconds);
            if(total>0&&done>4&&elapsedSeconds>2.0){
                const double remaining=elapsedSeconds/double(done)*double(total-done);
                if(remaining>0.0)timing+=L" · ETA "+TimeText(remaining);
            }
            row=surface.elapsedEta;DrawTextW(dc,timing.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            row=surface.size;const std::wstring hint=T(L"menu.cancel_export_running");
            DrawTextW(dc,hint.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            HBRUSH track=CreateSolidBrush(RGB(68,71,77));FillRect(dc,&surface.progressTrack,track);DeleteObject(track);
            HBRUSH fill=CreateSolidBrush(ui_palette::NeuralCoverage);FillRect(dc,&visual.fill,fill);DeleteObject(fill);
            SelectObject(dc,oldFont);
            return;
        }
        if((NeuralJobActive()&&!JobBehindPlayback())||(!m_loaded&&m_youtubeLifecycle.IsResolving())){
            const bool neural=NeuralJobActive();
            const PreRenderSurfaceLayout surface=LayoutPreRenderSurface(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd));m_neuralCancelBounds=surface.cancelButton;
            const bool paused=neural&&m_neuralLifecycle.state==NeuralPlaybackState::Paused;
            const bool acquiring=neural&&m_neuralProgress.phase==NeuralRenderPhase::Acquiring;
            uint64_t completed=neural?m_neuralProgress.completedFrames:0,total=neural?m_neuralProgress.totalFrames:0;
            // No frame exists until the source is local, but the copy's position
            // in the source is a real fraction, so the bar can move with it
            // instead of sitting still for the length of a download.
            if(acquiring&&m_neuralProgress.expectedSeconds>0.0){
                total=static_cast<uint64_t>(std::llround(m_neuralProgress.expectedSeconds*1000.0));
                completed=std::min(total,static_cast<uint64_t>(std::llround(std::max(0.0,m_neuralProgress.acquiredSeconds)*1000.0)));
            }
            const auto visual=ResolveActivityVisual(surface.progressTrack,ActivityElapsedMs(),completed,total,
                neural&&(m_neuralProgress.phase==NeuralRenderPhase::NeuralRendering||(acquiring&&total>0)),
                m_activityMotionEnabled&&!paused);
            DrawActivitySpinner(dc,surface.spinner,visual.spinnerStep);
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(242,243,245));HGDIOBJ oldFont=SelectObject(dc,m_font);
            std::wstring title=neural?(m_pendingNeuralTitle.empty()?L"Preparing playback":m_pendingNeuralTitle):(m_pendingYouTubeTitle.empty()?L"YouTube video":m_pendingYouTubeTitle);RECT row=surface.title;DrawTextW(dc,title.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            SelectObject(dc,m_fontSmall);SetTextColor(dc,ui_palette::SecondaryText);
            const std::wstring phase=!neural?L"Loading YouTube video":paused?T(L"neural.phase.paused"):NeuralPhaseText(m_neuralProgress);row=surface.phase;DrawTextW(dc,phase.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            const std::wstring resolution=neural?(m_neuralSourceWidth?std::to_wstring(m_neuralSourceWidth)+L" × "+std::to_wstring(m_neuralSourceHeight):L"Reading source metadata…"):L"Finding a playable source…";row=surface.resolution;DrawTextW(dc,resolution.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            const std::wstring frames=acquiring?(total?std::to_wstring(visual.percent)+L"% of the source copied":std::wstring{})
                :total?(!visual.indeterminate?std::to_wstring(visual.percent)+L"% · ":L"")+std::to_wstring(completed)+L" / "+std::to_wstring(total)+L" frames":L"";row=surface.frameCount;DrawTextW(dc,frames.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            const auto elapsed=std::max<uint64_t>(ActivityElapsedMs()/1000,neural?static_cast<uint64_t>(std::max<int64_t>(0,m_neuralProgress.elapsed.count()/1000)):0);
            const auto eta=neural?std::chrono::duration_cast<std::chrono::seconds>(m_neuralProgress.estimatedRemaining).count():0;
            const std::wstring timing=L"Elapsed "+TimeText(double(elapsed))+(eta>0?L" · ETA "+TimeText(double(eta)):L"");row=surface.elapsedEta;DrawTextW(dc,timing.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            const std::wstring bytes=!neural?L"Playback starts when the video is ready":m_neuralProgress.phase==NeuralRenderPhase::CheckingCache?T(L"neural.cache.checking"):acquiring?AcquisitionDetailText():(m_neuralProgress.bytes?std::to_wstring(m_neuralProgress.bytes/(1024*1024))+L" MiB written":L"Preparing encoder…");row=surface.size;DrawTextW(dc,bytes.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            HBRUSH track=CreateSolidBrush(RGB(68,71,77));FillRect(dc,&surface.progressTrack,track);DeleteObject(track);HBRUSH progressBrush=CreateSolidBrush(ui_palette::PrimaryBlue);FillRect(dc,&visual.fill,progressBrush);DeleteObject(progressBrush);
            SelectObject(dc,oldFont);DrawButton(dc,ToolbarAction::None,UiIcon::Stop,T(L"neural.cancel"),surface.cancelButton,true,false,PtIn(surface.cancelButton,m_mouseX,m_mouseY),false,false,false);return;
        }
        if(!m_loaded){
            const IdleSurfaceLayout idle=IdleLayout();
            SetBkMode(dc,TRANSPARENT);
            SetTextColor(dc,RGB(242,243,245));auto of=SelectObject(dc,m_font);std::wstring tt=T(L"idle.title");RECT title=idle.title;DrawTextW(dc,tt.c_str(),-1,&title,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            SetTextColor(dc,ui_palette::SecondaryText);SelectObject(dc,m_fontSmall);std::wstring ss=m_youtubeLifecycle.IsResolving()?m_cachedStatus:(m_cacheNotice.empty()?T(L"idle.subtitle"):m_cacheNotice);RECT subtitle=idle.subtitle;DrawTextW(dc,ss.c_str(),-1,&subtitle,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);SelectObject(dc,of);
            for(const auto& item:idle.actions){const auto content=ButtonContent(item.action,true);DrawButton(dc,item.action,content.icon,content.label,item.bounds,content.enabled,false,content.enabled&&m_hoverAction==item.action,m_pressedToolbarAction==item.action,GetFocus()==m_hwnd&&m_focusedToolbarAction==item.action,false);}
            if(!YouTubePlaybackAvailable()){SetBkMode(dc,TRANSPARENT);SetTextColor(dc,ui_palette::SecondaryText);of=SelectObject(dc,m_fontSmall);const bool compactReason=idle.subtitle.top==idle.subtitle.bottom;std::wstring reason=T(compactReason?L"idle.youtube_unavailable_compact":L"idle.youtube_unavailable");RECT reasonRect=idle.youtubeReason;DrawTextW(dc,reason.c_str(),-1,&reasonRect,DT_CENTER|DT_TOP|DT_WORDBREAK|DT_END_ELLIPSIS|DT_NOPREFIX);SelectObject(dc,of);}return;
        }
        if(!ControlsVisible())return;
        RECT bar{0,c.bottom-ControlHeight(),c.right,c.bottom};HBRUSH bg=CreateSolidBrush(ui_palette::ControlSurface);FillRect(dc,&bar,bg);DeleteObject(bg);HPEN line=CreatePen(PS_SOLID,1,RGB(54,56,61));auto op=SelectObject(dc,line);MoveToEx(dc,0,bar.top,nullptr);LineTo(dc,c.right,bar.top);SelectObject(dc,op);DeleteObject(line);
        const auto toolbarItems=ToolbarItems();
        for(const auto& item:toolbarItems){const auto content=ButtonContent(item.action);const bool hover=content.enabled&&m_hoverAction==item.action;DrawButton(dc,item.action,content.icon,content.label,item.bounds,content.enabled,content.active,hover,m_pressedToolbarAction==item.action,GetFocus()==m_hwnd&&m_focusedToolbarAction==item.action,item.compact,content.working);}
        const auto volumeRect=LayoutVolumeSlider(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd),toolbarItems);if(volumeRect){const RECT& vr=*volumeRect;HPEN vp=CreatePen(PS_SOLID,std::max(1,Dip(4)),RGB(94,98,105));op=SelectObject(dc,vp);MoveToEx(dc,vr.left,(vr.top+vr.bottom)/2,nullptr);LineTo(dc,vr.right,(vr.top+vr.bottom)/2);SelectObject(dc,op);DeleteObject(vp);int vx=vr.left+int((vr.right-vr.left)*(m_muted?0.0f:m_volume));const int knob=std::max(3,Dip(5));DrawSolidEllipse(dc,RECT{vx-knob,(vr.top+vr.bottom)/2-knob,vx+knob,(vr.top+vr.bottom)/2+knob},RGB(230,232,235),"Volume knob");}
        double shown=playback_timing::TimelinePosition(m_dragSeek,m_seekPreview,m_seekPending,m_pendingSeekSec,m_currentSec);RECT tr=TimelineRect();double d=m_decoder.DurationSeconds(),f=d>0?std::clamp(shown/d,0.0,1.0):0;
        // A source that reports no length (a browser-recorded WebM) greys the
        // bar: there is nothing to scale a position against, so it shows no
        // progress, takes no click, and its hover says how to seek instead.
        const bool lengthKnown=d>0;
        HBRUSH tb=CreateSolidBrush(lengthKnown?RGB(68,71,77):ui_palette::Inactive);FillRect(dc,&tr,tb);DeleteObject(tb);
        const auto markerX=[&](int64_t pts){return tr.left+int(std::lround((tr.right-tr.left)*(d>0?std::clamp(double(pts)*1e-7/d,0.0,1.0):0.0)));};
        // Three lanes in one track, so no state hides another: the In/Out
        // selection on top (violet, the loudest because it is what the render
        // acts on), played progress in the middle, and the part of the source
        // that already has cached neural frames along the bottom (teal).
        const LONG height=tr.bottom-tr.top,coverageLane=std::max<LONG>(Dip(3),height*2/5);
        // A session's coverage is a set of rendered regions, so each one is
        // painted on its own. One band from the session's start to its newest
        // frame would claim the holes between them as rendered, and those holes
        // are precisely the part the user cannot see rendered yet. A finished
        // cache entry still paints its single range.
        const std::vector<CoverageSpan> coverage=m_liveSession?LiveCoverage():std::vector<CoverageSpan>{};
        RECT rendered=tr;const bool renderedSpan=m_cachedPlayback||!coverage.empty();
        if(renderedSpan){
            if(m_liveSession){
                if(!coverage.empty()){
                    rendered.left=markerX(coverage.front().start100ns);
                    rendered.right=std::max<LONG>(rendered.left+1,markerX(coverage.back().end100ns));
                }
            }
            else if(!m_cachedRange.Whole()){rendered.left=markerX(m_cachedRange.start100ns);rendered.right=std::max<LONG>(rendered.left+1,markerX(m_cachedRange.end100ns));}
            HBRUSH nb=CreateSolidBrush(ui_palette::NeuralCoverage);
            if(m_liveSession)
                for(const CoverageSpan& span:coverage){
                    RECT band{markerX(span.start100ns),tr.bottom-coverageLane,0,tr.bottom};
                    band.right=std::max<LONG>(band.left+1,markerX(span.end100ns));
                    FillRect(dc,&band,nb);
                }
            else{RECT band{rendered.left,tr.bottom-coverageLane,rendered.right,tr.bottom};FillRect(dc,&band,nb);}
            DeleteObject(nb);
        }
        // Where the render is working right now, hatched, from the head of
        // the hole the running job is filling: the band says what is done,
        // this says where it is growing.
        if(m_liveSession&&NeuralJobActive()&&m_liveTarget.end100ns>m_liveTarget.start100ns&&lengthKnown){
            const LONG head=markerX(static_cast<int64_t>(std::llround(LiveJobReachSeconds()*1e7)));
            if(const auto span=timeline::RenderingNowSpan(tr.left,tr.right,head,markerX(m_liveTarget.end100ns),Dip(4),Dip(28)))
                DrawRenderingNow(dc,RECT{tr.left,tr.bottom-coverageLane,tr.right,tr.bottom},span->first,span->second);
        }
        // A live session plays the original inside its holes, so progress is not
        // confined to the rendered regions; a cache entry's playback is.
        const bool clampProgress=m_cachedPlayback&&!m_liveSession;
        RECT done{clampProgress?rendered.left:tr.left,tr.top,0,renderedSpan?tr.bottom-coverageLane:tr.bottom};
        done.right=std::clamp<LONG>(static_cast<LONG>(tr.left+std::lround((tr.right-tr.left)*f)),done.left,clampProgress?rendered.right:tr.right);
        if(lengthKnown){HBRUSH db=CreateSolidBrush(ui_palette::PrimaryBlue);FillRect(dc,&done,db);DeleteObject(db);}
        // One pair of edge ticks per rendered region, so a hole reads as a gap
        // between two regions instead of being hidden inside one long band.
        if(renderedSpan&&m_liveSession){
            HBRUSH eb=CreateSolidBrush(ui_palette::NeuralCoverage);
            for(const CoverageSpan& span:coverage){
                const LONG left=markerX(span.start100ns),right=std::max<LONG>(left+1,markerX(span.end100ns));
                RECT startEdge{left,tr.top,left+std::max(1,Dip(1)),tr.bottom},endEdge{right-std::max(1,Dip(1)),tr.top,right,tr.bottom};
                FillRect(dc,&startEdge,eb);FillRect(dc,&endEdge,eb);
            }
            DeleteObject(eb);
        }
        else if(renderedSpan&&!m_cachedRange.Whole()){HBRUSH eb=CreateSolidBrush(ui_palette::NeuralCoverage);RECT startEdge{rendered.left,tr.top,rendered.left+std::max(1,Dip(1)),tr.bottom},endEdge{rendered.right-std::max(1,Dip(1)),tr.top,rendered.right,tr.bottom};FillRect(dc,&startEdge,eb);FillRect(dc,&endEdge,eb);DeleteObject(eb);}
        // The selection is drawn last and fills the track, so it reads at a
        // glance; the coverage lane stays visible beneath it.
        if(m_markers.in100ns&&m_markers.out100ns&&*m_markers.out100ns>*m_markers.in100ns){
            RECT span{markerX(*m_markers.in100ns),tr.top,markerX(*m_markers.out100ns),renderedSpan?tr.bottom-coverageLane:tr.bottom};
            if(span.right<=span.left)span.right=span.left+std::max(1,Dip(1));
            HBRUSH sb=CreateSolidBrush(ui_palette::MarkedRange);FillRect(dc,&span,sb);DeleteObject(sb);
            RECT rail{span.left,tr.top,span.right,tr.top+std::max<LONG>(1,Dip(2))};HBRUSH rb=CreateSolidBrush(ui_palette::MarkedRangeEdge);FillRect(dc,&rail,rb);DeleteObject(rb);
        }
        // Chapter boundaries as gaps cut through the bar, the way a video site
        // draws them, so they read as divisions of the video rather than as
        // one more coloured tick among the In/Out markers. The hover names the
        // chapter.
        if(lengthKnown&&!m_chapters.empty()){
            HBRUSH gap=CreateSolidBrush(ui_palette::ControlSurface);
            for(const timeline::Chapter& chapter:m_chapters){
                if(chapter.startSeconds<=0.0||chapter.startSeconds>=d)continue;
                const LONG x=markerX(static_cast<int64_t>(std::llround(chapter.startSeconds*1e7)));
                RECT cut{x-std::max(1,Dip(1)),tr.top,x+std::max(1,Dip(1)),tr.bottom};FillRect(dc,&cut,gap);
            }
            DeleteObject(gap);
        }
        const int tickWidth=std::max(2,Dip(3)),tickRise=Dip(8);
        const auto markerTick=[&](int64_t pts,COLORREF color){const int x=markerX(pts);RECT tick{x-tickWidth/2,tr.top-tickRise,x-tickWidth/2+tickWidth,tr.bottom+std::max(1,Dip(2))};HBRUSH mb=CreateSolidBrush(color);FillRect(dc,&tick,mb);DeleteObject(mb);};
        if(m_markers.in100ns)markerTick(*m_markers.in100ns,RGB(96,220,130));if(m_markers.out100ns)markerTick(*m_markers.out100ns,RGB(255,168,64));
        const int knobR=std::max(4,Dip(6));int kx=done.right;if(lengthKnown)DrawSolidEllipse(dc,RECT{kx-knobR,tr.top-Dip(2),kx+knobR,tr.bottom+Dip(2)},RGB(246,246,248),"Timeline knob");
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(206,208,212));auto of=SelectObject(dc,m_fontSmall);std::wstring time=TimeText(shown)+L" / "+(lengthKnown?TimeText(d):std::wstring(L"--:--"));TextOutW(dc,Dip(18),c.bottom-Dip(50),time.c_str(),int(time.size()));
        const status_chips::RowLayout statusRow=StatusRowLayout();
        {
            const auto now=Clock::now();
            constexpr std::array<COLORREF,status_chips::kChipCount> accents{ui_palette::NeuralCoverage,ui_palette::PrimaryBlue,ui_palette::Attention};
            const HGDIOBJ chipFont=SelectObject(dc,m_fontSmall?m_fontSmall:m_font);
            for(size_t index=0;index<status_chips::kChipCount;++index)
                DrawStatusChip(dc,statusRow.chips[index],m_cachedChips[index],accents[index],
                               m_chipFlash.Level(static_cast<status_chips::Chip>(index),now,m_activityMotionEnabled));
            SelectObject(dc,chipFont);SetTextColor(dc,RGB(206,208,212));
        }
        RECT sr{statusRow.text.left,statusRow.text.top+Dip(2),std::max(statusRow.text.left,statusRow.text.right-Dip(2)),statusRow.text.bottom-Dip(2)};
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
        // The running job's own key, when it is for the video that is loaded. It
        // is the same key the recent history would name after the job finished,
        // and having it now is what lets a first watch reuse the copy the job
        // acquired - for a render, and for playback's own seeks.
        // Tied to the video it was reported for, not just to "some job ran": the
        // initial-open commit does not go through Unload, so without this a
        // second video would be handed the first one's key - and two 1440p
        // trailers share a geometry, so the guard downstream would not catch it.
        if(!m_jobSourceKey.empty()&&!m_youtubePageUrl.empty()&&m_jobSourcePageUrl==m_youtubePageUrl)
            return m_jobSourceKey;
        if(!m_recent||m_youtubePageUrl.empty())return std::nullopt;
        const auto id=CanonicalYouTubeVideoId(m_youtubePageUrl);
        // The recent history outlives the cache folder: a user who clears the
        // cache, or an acquisition that never finished, leaves an entry naming a
        // copy that is not there. Handing that key to a job made it fail on a
        // missing source instead of acquiring one, and a live session ended on it.
        for(const auto& entry:m_recent->Entries()){
            if(!entry.youtube||entry.id!=id||entry.sourceQuality!=static_cast<int>(m_youtubeSourceQuality)||entry.sourceKey.empty())continue;
            const NeuralCacheManager& cache=SourceCache();
            if(!cache.Valid())return std::nullopt;
            // `LookupSource` authenticates the copy by hashing the whole payload -
            // 60.5 MiB for a 1440p trailer, measured at 63-86 ms - and this question
            // is asked several times per Tick by the toolbar, the status text and the
            // session gates. Asking it per paint stalled the UI thread to two Ticks a
            // second: measured 510 ms per Tick and 29 dropped frames a second on a
            // source whose acquired copy was already in the cache, while the decoder
            // kept handing 29.7 fps to a queue nobody drained. The verdict is memoised
            // against the payload's identity - its size and write time - so a copy
            // that a later acquisition replaced is authenticated again rather than
            // trusted. Whether the payload can
            // be stat'ed AT ALL is part of that identity: treating a missing one
            // as "nothing memoised" re-ran this whole lookup, and re-logged it,
            // on every toolbar paint and status refresh - 14,000 identical log
            // lines and a disk write per frame, measured over one minute on one
            // stream whose recent entry outlived its cache folder.
            std::error_code sizeError,timeError;
            const auto payload=cache.SourcePayloadPath(entry.sourceKey);
            const std::filesystem::path payloadPath=payload?*payload:std::filesystem::path{};
            const auto size=payloadPath.empty()?uintmax_t{}
                                               :std::filesystem::file_size(payloadPath,sizeError);
            const auto written=payloadPath.empty()?std::filesystem::file_time_type{}
                                                  :std::filesystem::last_write_time(payloadPath,timeError);
            const bool absent=payloadPath.empty()||sizeError||timeError;
            if(m_sourceKeyMemo.valid&&m_sourceKeyMemo.key==entry.sourceKey&&
               m_sourceKeyMemo.absent==absent&&
               (absent||(m_sourceKeyMemo.size==size&&m_sourceKeyMemo.written==written)))
                return m_sourceKeyMemo.verdict;
            const auto cached=cache.LookupSource(entry.sourceKey);
            const bool complete=cached&&cached->manifest.encoder==kCompleteSourcePolicy;
            // A copy caught MID-PROMOTION - the payload moved into place, its
            // manifest not written yet - authenticates as missing while its size
            // and write time are already final, so memoising that verdict pins
            // it forever. That is what made a finished download read as no copy
            // at all for the rest of the session: the toolbar happened to ask
            // inside the promote. Nothing is remembered while an acquisition for
            // this source is still running.
            if(!complete&&!absent&&SourcePrefetchActive()){m_sourceKeyMemo={};return std::nullopt;}
            m_sourceKeyMemo={true,absent,entry.sourceKey,payloadPath,absent?uintmax_t{}:size,
                             absent?std::filesystem::file_time_type{}:written,
                             complete?std::optional<std::string>{entry.sourceKey}:std::nullopt};
            // Logged where the verdict is decided rather than where it is read,
            // so it names a state change instead of counting paints.
            if(!complete)
                LOG("A recent entry names a source copy that is no longer in the cache; ignoring it: key="
                    <<entry.sourceKey);
            return m_sourceKeyMemo.verdict;
        }
        return std::nullopt;
    }
    bool SourcePrefetchActive()const{return m_prefetchState&&!m_prefetchState->finished.load(std::memory_order_acquire);}
    // The cache manager the two lookups above read through. Constructing one is
    // not free: PrepareWritableRoot creates six directories and writes a probe
    // file, and SweepStaging walks - and may delete from - the staging folder.
    // Both lookups built a fresh one per call, BEFORE the memo check, and the
    // toolbar asks once per button on every paint while the status text asks on
    // every presented frame. Built on the first question about a loaded source,
    // released with it in Unload, and rebuilt only when the cache root moves.
    const NeuralCacheManager& SourceCache()const{
        if(!m_sourceCache||m_sourceCacheRequestedRoot!=m_cacheRoot){
            m_sourceCache=std::make_unique<NeuralCacheManager>(m_cacheRoot);
            m_sourceCacheRequestedRoot=m_cacheRoot;
            ++m_sourceCacheBuilds;
        }
        return *m_sourceCache;
    }
    // Where the acquired copy of this stream is, when the cache still holds a
    // complete one. The hash this pays for is the memoised one above.
    std::optional<std::filesystem::path> AcquiredSourceCopyPath()const{
        if(m_sourceKind!=MediaSourceKind::YouTube)return std::nullopt;
        // What the job reported is the copy it is rendering from right now, known
        // from the moment its acquisition finished.
        if(!m_jobSourcePath.empty()){
            std::error_code ec;
            if(std::filesystem::is_regular_file(m_jobSourcePath,ec)&&!ec)return m_jobSourcePath;
        }
        const auto key=CachedYouTubeSourceKey();
        if(!key)return std::nullopt;
        const NeuralCacheManager& cache=SourceCache();
        if(!cache.Valid())return std::nullopt;
        // `SourcePayloadPath`, not `LookupSource`: the latter authenticates the
        // copy by hashing all 60-odd MiB of it, which is not a price a seek
        // should pay. What playback needs of this file is checked directly by
        // opening it below - it decodes, and its geometry matches what the
        // renderer and the rendered segments were built for.
        const auto payload=cache.SourcePayloadPath(*key);
        if(!payload)return std::nullopt;
        std::error_code ec;
        if(!std::filesystem::is_regular_file(*payload,ec)||ec)return std::nullopt;
        return *payload;
    }
    // Moves the loaded original from the live stream onto the copy the render is
    // already reading from.
    //
    // A seek on a stream is a re-resolution: the signed URL is re-issued, the
    // decoder and the renderer are swapped, and an active session is released and
    // restarted around it. That is what a seek out of rendered coverage cost on
    // YouTube - `DetachLivePlayback` clears `m_cachedPlayback`, which is what
    // `NetworkPlayback` keys on, so the seek that follows takes the network path
    // even though a local copy of the very same frames is sitting in the cache.
    // A resolution that fails there ends the session with a dialog. The copy is
    // what the segments were rendered from, so from here the same seek is local.
    bool AdoptAcquiredSourceCopyForPlayback(){
        if(m_sourceKind!=MediaSourceKind::YouTube||m_cachedSourceFile||!m_loaded)return false;
        if(m_youtubeLifecycle.IsResolving())return false;
        const auto copy=AcquiredSourceCopyPath();
        if(!copy)return false;
        VideoDecoder local;
        // Ask for the layout the renderer was built for. Open's preferNv12
        // defaults to false while the streaming open passes true, so without
        // this the swap silently moved the decoder to BGRA under a renderer
        // still configured for NV12.
        const bool preferNv12=m_decoder.PixelLayout()==VideoPixelLayout::Nv12;
        if(!local.Open(copy->wstring(),MediaSourceKind::LocalFile,{},preferNv12)){
            LOG("The acquired source copy could not be opened for playback; staying on the stream.");return false;
        }
        // The renderer and every rendered segment were built for the geometry
        // loaded now; a copy acquired at another rung cannot stand in for it.
        if(local.NativeWidth()!=m_decoder.NativeWidth()||local.NativeHeight()!=m_decoder.NativeHeight()){
            LOG("The acquired source copy is "<<local.NativeWidth()<<"x"<<local.NativeHeight()<<" against the stream's "
                <<m_decoder.NativeWidth()<<"x"<<m_decoder.NativeHeight()<<"; staying on the stream.");
            local.Close();return false;
        }
        // preferNv12 is a request, not a guarantee: odd geometry and colour
        // descriptions the GPU conversion does not implement still decode to
        // BGRA. Swap() takes the whole source description including the
        // layout, and nothing downstream reconfigures the renderer, so a
        // disagreement here would upload the first quarter of a BGRA image as
        // a Y plane - a corrupt picture the size check cannot catch, because
        // w*h*4 is larger than the NV12 frame it is compared against.
        if(local.PixelLayout()!=m_decoder.PixelLayout()){
            LOG("The acquired source copy decodes as "<<(local.PixelLayout()==VideoPixelLayout::Nv12?"NV12":"BGRA")
                <<" against the stream's "<<(m_decoder.PixelLayout()==VideoPixelLayout::Nv12?"NV12":"BGRA")
                <<"; staying on the stream rather than presenting garbage.");
            local.Close();return false;
        }
        Audio().Stop();m_networkAudio.reset();
        m_decoder.Swap(local);local.Close();
        m_networkReadState.Reset();m_waitingForNetworkFrame=false;m_haveNext=false;m_next=VideoFrame{};m_nextPairFrame.reset();
        m_cachedSourceFile=true;m_path=copy->wstring();
        LOG("Playback moved onto the acquired local copy of this stream; seeks are local from here.");
        return true;
    }
    // Downloads the playing stream into the source cache while playback
    // continues, so a render starts on a local file instead of waiting for the
    // whole video. Only a live stream needs it, and only once per source.
    // `forFrameGeneration` ignores the neural pre-render preference: the copy is
    // wanted for a conversion, which does not involve the neural path at all,
    // and it is the user's explicit request rather than a background nicety.
    void EnsureSourcePrefetch(bool forFrameGeneration=false){
        if(!m_loaded||(!forFrameGeneration&&!NeuralPreRenderEnabled())||!NetworkPlayback())return;
        if(m_prefetchWorker.joinable()||CachedYouTubeSourceKey())return;
        if(m_path.empty()||m_youtubePageUrl.empty())return;
        const double duration=m_decoder.DurationSeconds();
        if(!std::isfinite(duration)||duration<=0.0)return;
        auto state=std::make_shared<SourcePrefetchState>();
        const std::wstring media=m_path,audio=m_youtubeAudioUrl,page=m_youtubePageUrl;
        const auto quality=m_youtubeSourceQuality;const auto cacheRoot=m_cacheRoot,moduleDirectory=ExecutableDirectory();
        const NeuralCacheFailureText cacheFailureText=CacheFailureText();
        try{
            m_prefetchWorker=std::jthread([state,cacheRoot,moduleDirectory,media,audio,page,quality,duration,cacheFailureText](std::stop_token stop){
                NeuralCacheManager cache(cacheRoot);
                if(cache.Valid()){
                    const SourceAcquisition acquired=AcquireYouTubeSource(cache,cacheFailureText,moduleDirectory,media,audio,page,quality,duration,{},stop);
                    if(!acquired.path.empty())state->key=acquired.key;
                    else{state->detail=acquired.detail;state->cancelled=acquired.cancelled;}
                }else state->detail=L"The neural cache directory is unavailable.";
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
            const std::wstring failureDetail=m_prefetchState?m_prefetchState->detail:std::wstring{};
            const bool acquisitionCancelled=m_prefetchState&&m_prefetchState->cancelled;
            const std::wstring page=m_prefetchPageUrl,title=m_prefetchTitle;const auto quality=m_prefetchQuality;
            m_prefetchState.reset();m_prefetchPageUrl.clear();m_prefetchTitle.clear();
            // A settled acquisition is the ONE moment the answer to "is there a
            // local copy of this stream" can change while one source stays
            // loaded, so it is the one place the memo is dropped. Dropping it
            // every tick instead cost a cache-root validation and two stats per
            // toolbar paint, which froze the window measurably.
            InvalidateFrameGenerationCopy();
            if(key.empty()){
                LOG("Background source acquisition finished without a reusable copy"
                    <<(acquisitionCancelled?" (cancelled)":"")<<": "
                    <<WideToUtf8(failureDetail.empty()?std::wstring(L"no reason was reported"):failureDetail));
                SyncFeatureMenuState();UpdateCachedStatus();InvalidateControls();return;
            }
            LOG("Background source acquisition complete; a render will reuse it.");
            NeuralJobCompletion owned{};owned.sourceKind=MediaSourceKind::YouTube;owned.pageUrl=page;owned.displayTitle=title;owned.sourceQuality=quality;owned.sourceKey=key;
            RecordRecent(owned,true);
            // The settled acquisition is what turns the pill from "Get a copy"
            // into "Generate", and nothing else on this tick invalidates it:
            // UpdateCachedStatus repaints the status rect alone, so the toolbar
            // kept its old label until a mouse move happened to redraw it.
            SyncFeatureMenuState();UpdateCachedStatus();InvalidateControls();return;
        }
        // A different video is playing now: the download is worthless. An offline
        // job owns the prefetch it will consume, but a live session renders what is
        // already on screen, so a download for some other page is still worthless.
        if(m_loaded&&(!NeuralJobActive()||m_liveSession)&&m_youtubePageUrl!=m_prefetchPageUrl){
            LOG("Loaded source changed; stopping the background acquisition.");
            CancelSourcePrefetch();SyncFeatureMenuState();UpdateCachedStatus();InvalidateControls();
        }
    }
    void CancelSourcePrefetch(){if(m_prefetchWorker.joinable()){m_prefetchWorker.request_stop();m_prefetchWorker.join();m_prefetchWorker=std::jthread{};}m_prefetchState.reset();m_prefetchPageUrl.clear();m_prefetchTitle.clear();InvalidateFrameGenerationCopy();}
    bool RangeRenderAvailable()const{
        // m_frameGenWorker for the same reason the export gate carries it: two
        // GPU jobs in one process contend, and starting a range render mid
        // conversion greys the frame-generation item while its cancel item
        // stays live - a half state with no explanation.
        if(!m_loaded||NeuralJobActive()||NeuralWorkerRetiring()||m_youtubeLifecycle.IsResolving()||!NeuralPreRenderEnabled()||m_path.empty()||m_frameGenWorker.joinable())return false;
        if(m_sourceKind!=MediaSourceKind::YouTube)return true;
        // A stream is rendered from its own acquired copy: either the cached one
        // or a fresh acquisition, which needs the page URL and a real duration.
        //
        // Once playback itself is ON that copy, the key is the only route left:
        // the stream URL the acquisition would need is no longer what is loaded,
        // and handing it the local path earns an instant refusal - "the source
        // format or duration is unavailable" - which two of in a row stops a
        // session. Say unavailable here instead of starting a job that cannot run.
        if(m_cachedSourceFile)return CachedYouTubeSourceKey().has_value();
        return CachedYouTubeSourceKey().has_value()||(!m_youtubePageUrl.empty()&&m_decoder.DurationSeconds()>0.0);
    }
    // Renders [start,end) of the source that is loaded now. A YouTube source
    // reuses its owned source-cache entry when the recent history still names
    // one; otherwise the job acquires the source before rendering the range.
    bool RenderRangeOfCurrentSource(NeuralRenderRange range,NeuralJobKind kind=NeuralJobKind::Offline){
        if(!RangeRenderAvailable()){LOG("Render request ignored: loaded="<<m_loaded<<" job="<<NeuralJobActive()<<" resolving="<<m_youtubeLifecycle.IsResolving()<<" prerender="<<NeuralPreRenderEnabled()<<" haveSource="<<!m_path.empty()<<" cachedSourceKey="<<CachedYouTubeSourceKey().has_value()<<" duration="<<m_decoder.DurationSeconds());return false;}
        // Asked here rather than at each of the three callers, and only for a
        // render that is actually about to start, because the answer costs a
        // cache-root validation. See LoadedSourceIsGeneratedFrames for why the
        // order is worth a sentence to the viewer.
        if(LoadedSourceIsGeneratedFrames()){
            LOG("Neural render starting on this player's own frame-generation output ("
                <<WideToUtf8(m_path)<<"); the documented order is to render first and convert the result.");
            m_neuralNotice=T(L"neural.order.generated_source");
            UpdateCachedStatus();InvalidateControls();
        }
        if(m_sourceKind==MediaSourceKind::YouTube){
            const std::wstring page=m_youtubePageUrl,title=m_displayTitle;
            if(const auto sourceKey=CachedYouTubeSourceKey()){
                return StartNeuralJob(page,{},title,page,MediaSourceKind::YouTube,m_youtubeSourceQuality,*sourceKey,0.0,range,false,kind);
            }
            // No branch here for "the payload is already a local copy": the key
            // above is what the job reported, so a first watch takes the reuse
            // path and the render keeps ONE source identity. Starting the copy as
            // a local file instead would key its entries on a path, write a
            // local-file entry into the recent history for a file inside the
            // cache, and leave the copy unreferenced for the next eviction pass -
            // the copy playback may by then be reading from.
            const std::wstring media=m_path,audio=m_youtubeAudioUrl;const double duration=m_decoder.DurationSeconds();
            return StartNeuralJob(media,audio,title,page,MediaSourceKind::YouTube,m_youtubeSourceQuality,{},duration,range,false,kind);
        }
        const std::wstring source=m_path,title=m_displayTitle;
        return StartNeuralJob(source,{},title,{},MediaSourceKind::LocalFile,m_youtubeSourceQuality,{},0.0,range,false,kind);
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
    // The first file of a session is what the user is waiting for, so it is
    // short; the ones behind it stay at kLiveSegmentSeconds.
    static constexpr double kLiveFirstSegmentSeconds=0.5;
    static constexpr double kLiveStartLead=live_session::kStartLead;
    static constexpr double kLiveResumeLead=live_session::kResumeLead;
    // A session now renders the whole video by default, so its range chip named
    // the entire clip on every session - which reads as a user selection when
    // there was none. Only a range narrower than the source is worth saying.
    bool CachedRangeCoversSource()const{
        if(m_cachedRange.Whole())return true;
        const int64_t duration=SourceDuration100ns();
        return duration>0&&m_cachedRange.start100ns<=0&&m_cachedRange.end100ns>=duration;
    }
    bool LiveSessionAvailable()const{return RangeRenderAvailable()&&!m_cachedPlayback&&!m_decoder.IsStillImage();}
    // A photo has no timeline for a session to follow, but it can still be
    // rendered: the single-frame job is the one the frame preview already runs.
    // Without this the toolbar's Neural Rendering button was permanently dead on
    // a photo, which reads as "images are not supported".
    bool StillImageRenderAvailable()const{
        return m_loaded&&m_decoder.IsStillImage()&&!m_cachedPlayback&&!m_previewJob&&RangeRenderAvailable();
    }
    // One restart at the playhead is a recovery; a second means the coverage is
    // never going to reach it, and looping is worse than falling back.
    static constexpr int kLiveStalledRebaseLimit=1;
    // Coverage is a set of rendered regions, not a head. Everything the session
    // decides is asked of that set.
    std::vector<CoverageSpan> LiveCoverage()const{
        return m_liveSegments?m_liveSegments->CoveredRanges():std::vector<CoverageSpan>{};
    }
    int64_t LiveFrame100ns()const{return static_cast<int64_t>(std::llround(1e7/std::max(1.0,m_decoder.FrameRate())));}
    // What is left to render inside the session's range. A residual narrower than
    // one frame is coverage rather than work: a job handed it refuses the range.
    //
    // A hole this returns that no job will start on is a hole the session
    // selects again on the very next tick, forever: StartLiveRenderTarget
    // refuses it, reports success and changes nothing, so nothing renders,
    // nothing fails, and `finished` - the flag that ends a rebuffer - is never
    // reached. One 59.94 fps session logged that refusal 40850 times in six
    // minutes with playback paused behind it and every frame of the video
    // already rendered.
    //
    // So every hole is asked the question StartLiveRenderTarget will ask it,
    // against the start it will ask it about. Snapping DOWN is what settles it
    // today - a boundary at or below the hole start always leaves a frame
    // inside a non-empty hole - which makes this filter drop nothing as the two
    // rules currently stand. It is here because those are two rules in two
    // places: either one changing alone is enough to strand a hole again, and
    // the failure mode is not a wrong picture but a player that never resumes.
    std::vector<CoverageSpan> LiveHoles()const{
        if(!m_liveSegments)return {};
        const double fps=m_decoder.FrameRate();
        std::vector<CoverageSpan> holes=UncoveredSpans(LiveCoverage(),CoverageSpan{m_liveRange.start100ns,m_liveRange.end100ns},LiveFrame100ns());
        holes.erase(std::remove_if(holes.begin(),holes.end(),[&](const CoverageSpan& hole){
                        return RenderRangeIsCovered(SnapRenderStart(hole.start100ns),hole.end100ns,fps);
                    }),
                    holes.end());
        return holes;
    }
    // How far the running job has rendered inside its own target: the end of the
    // coverage that starts where the target does, or the target's start when it
    // has published nothing yet. This is what the retarget budget is measured
    // against, because LivePlayableSpan() below is about the PLAYHEAD and is
    // empty whenever the playhead sits in a hole.
    double LiveJobReachSeconds()const{
        const double start=double(m_liveTarget.start100ns)*1e-7;
        if(!m_liveSegments)return start;
        const int64_t slack=LiveFrame100ns();
        for(const CoverageSpan& span:LiveCoverage())
            if(span.start100ns<=m_liveTarget.start100ns+slack&&span.end100ns>m_liveTarget.start100ns)
                return double(span.end100ns)*1e-7;
        return start;
    }
    // The rendered region the playhead is inside, which is the only buffer
    // playback can drain - a region on the far side of a hole is not lead, and
    // measuring against the newest rendered timestamp attached sessions with
    // nothing to show. Coverage beginning a frame or two after the playhead
    // counts: a job starts on the next whole frame, which is what
    // AttachPosition100ns exists for.
    std::optional<CoverageSpan> LivePlayableSpan()const{
        if(!m_liveSegments)return std::nullopt;
        const auto covered=LiveCoverage();
        const int64_t at=static_cast<int64_t>(std::llround(Position()*1e7));
        if(const auto span=SpanContaining(covered,at))return span;
        const int64_t slack=2*LiveFrame100ns();
        for(const CoverageSpan& span:covered)
            if(span.start100ns>at&&span.start100ns-at<=slack)return span;
        return std::nullopt;
    }
    double LiveHeadSeconds()const{
        const auto span=LivePlayableSpan();
        return span?double(span->end100ns)*1e-7:0.0;
    }
    double LiveLeadSeconds()const{return live_session::Lead(LiveSessionView());}
    // Finished means the session's whole range is rendered, not that one job
    // ended: a job fills one hole and the next one starts on the next hole.
    bool LiveSessionFinished()const{return m_liveSegments&&LiveHoles().empty();}
    // Rendering is linear in pixel count, so a source too large for this GPU can
    // be recognised before a single frame is rendered. Saying so beats letting the
    // user watch a loader that will never clear.
    //
    // Asked once per source and geometry, not once per session start: a YouTube
    // seek re-resolves the stream and restarts the session on its retained
    // coverage, and every one of those restarts put the same question up again.
    // A yes stands until the file is unloaded or the geometry changes, which is
    // when the forecast changes. A no ends the request and says so in the status
    // bar, where a refused toggle used to leave nothing at all.
    bool ConfirmLiveSessionPace(double fps){
        const auto forecast=playback_timing::ForecastLiveRender(m_decoder.Width(),m_decoder.Height(),fps,m_renderPace,RenderPacePrior(m_opt.detectedGpu.generation));
        if(!forecast.measured){
            // No prior exists for every RTX generation, and inventing one would
            // be a guess dressed as a measurement. Say so instead of implying
            // the card was checked and passed.
            LOG("Active neural session pace is unmeasured on this GPU ("<<GpuPathName(m_opt.detectedGpu.generation)
                <<"); this session measures it.");
            return true;
        }
        if(forecast.keepsUp)return true;
        const std::string key=LiveSourceGeometryKey();
        if(key==m_livePaceConfirmedKey)return true;
        LOG("Active neural session forecast: "<<m_decoder.Width()<<"x"<<m_decoder.Height()<<" at "<<fps
            <<" fps renders at about "<<forecast.renderFps<<" fps ("<<forecast.realtimeRatio<<"x realtime).");
        wchar_t text[512];
        swprintf_s(text,T(L"neural.live.slow").c_str(),m_decoder.Width(),m_decoder.Height(),fps,
                   forecast.renderFps,forecast.realtimeRatio);
        if(MessageBoxW(m_hwnd,text,T(L"neural.live.title").c_str(),MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES){
            LOG("Active neural session declined by the user for the forecast pace.");
            m_neuralNotice=T(L"neural.live.declined");
            UpdateCachedStatus();InvalidateControls();
            return false;
        }
        m_livePaceConfirmedKey=key;
        return true;
    }
    // The video and the geometry it is loaded at, which is what a render pace
    // forecast and a set of rendered frames are both tied to.
    //
    // A YouTube m_path is a signed media URL that is re-issued by every resolution,
    // including the one a seek or a quality reload performs on the video already
    // playing. Keying on it would make the retained coverage unadoptable after any
    // of those, which is the same as not retaining at all. The page URL is the
    // stable identity of the video, and it is what Recent and the source cache
    // already key on.
    //
    // Geometry belongs in the key: a YouTube quality reload commits the same
    // video at a different resolution, and segments rendered at the old one
    // are refused by the pair on a width/height mismatch - every frame an
    // Error rather than a picture. Retained frames must be frames this
    // session could actually use.
    std::string LiveSourceGeometryKey()const{
        const std::wstring& identity=
            (m_sourceKind==MediaSourceKind::YouTube&&!m_youtubePageUrl.empty())?m_youtubePageUrl:m_path;
        return WideToUtf8(identity)+"|"+std::to_string(m_decoder.Width())+"x"+std::to_string(m_decoder.Height());
    }
    // What makes retained coverage reusable: same source at the same geometry,
    // same neural settings, same guides. Anything else and the frames on disk
    // are not the frames the user would get now.
    std::string LiveRetentionKey()const{
        return LiveSourceGeometryKey()+"|"+CanonicalNeuralSettings(m_neuralSettings)+"|"+CanonicalGuideControls(m_renderGuides);
    }
    // Deletes the segment files a published run retired (ReplaceRun), except
    // one a live decoder still has open or is opening: that one goes on a later
    // pass, once playback has crossed into the joined entry. A file something
    // else holds - a scanner, say - is simply tried again.
    void SweepRetiredLiveSegments(const std::shared_ptr<NeuralSegmentIndex>& index){
        if(!index)return;
        for(const std::filesystem::path& path:index->RetiredFiles()){
            if(m_synchronizedPlayback.HoldsFile(path))continue;
            std::error_code ec;std::filesystem::remove(path,ec);
            if(!ec)index->ForgetRetired(path);
        }
    }
    void DropRetainedLiveSegments(){
        m_retainedSegments.reset();m_retainedRange={};m_retainedKey.clear();
        if(!m_retainedDirectory.empty()){std::error_code ec;std::filesystem::remove_all(m_retainedDirectory,ec);m_retainedDirectory.clear();}
    }
    // What the session sets out to render: the marked range when the playhead is
    // inside one, otherwise the whole video. Holes inside it are filled one job
    // at a time in the order the user needs them, so a session toggled on at 20 s
    // renders the opening as well - which is what makes the whole video seekable
    // with the picture the user asked for.
    NeuralRenderRange LiveSessionRange(int64_t at,double fps,int64_t duration)const{
        if(const auto marked=RangeFromMarkers(m_markers,fps,duration);marked&&at>=marked->start100ns&&at<marked->end100ns)
            return NeuralRenderRange{marked->start100ns,marked->end100ns};
        return NeuralRenderRange{0,duration};
    }
    // The hole this session should be rendering, given where the user is
    // watching: the one under the playhead, else the nearest ahead, else the
    // earliest behind.
    std::optional<CoverageSpan> WantedLiveTarget()const{
        return NextRenderTarget(LiveHoles(),static_cast<int64_t>(std::llround(Position()*1e7)));
    }
    // Points the render at one hole. Coverage already on disk is never touched:
    // that is the whole difference between this and the old rebase.
    bool StartLiveRenderTarget(CoverageSpan hole){
        const NeuralRenderRange target{SnapRenderStart(hole.start100ns),hole.end100ns};
        const size_t holes=LiveHoles().size();
        if(RenderRangeIsCovered(target.start100ns,target.end100ns,m_decoder.FrameRate())){
            // A sub-frame residual is coverage, not work. Unreachable for any
            // non-empty hole while the start above snaps down: a boundary at or
            // below the hole start leaves a frame inside the range by
            // construction. It stays as a guard for a caller that hands over a
            // span this never measured - and it must never again be the only
            // thing between the session and a target it keeps reselecting,
            // because reporting success while changing no state is a spin, and
            // it spun: 40850 refusals of one frame, playback paused throughout.
            LOG("Active neural session target ["<<double(target.start100ns)*1e-7<<","
                <<double(target.end100ns)*1e-7<<") s is shorter than one frame; nothing to render.");
            m_liveTarget=target;return true;
        }
        if(!RenderRangeOfCurrentSource(target,NeuralJobKind::Live)){
            LOG("Active neural session could not start a render for ["<<double(target.start100ns)*1e-7
                <<","<<double(target.end100ns)*1e-7<<") s.");
            return false;
        }
        // The pace clock starts with this job's first segment: the wait between
        // jobs, and each job's startup, are not render time.
        m_liveSegments->ResetPace();
        // The coverage this job started from: if it ends with the index
        // unchanged, it rendered nothing and must not be started again.
        m_liveTarget=target;m_liveTargetRevision=m_liveSegments->Revision();
        // The pace is what the status line's chip reads, logged where it can be
        // checked without a screenshot. Zero until the settle window passes.
        LOG("Active neural session rendering ["<<double(target.start100ns)*1e-7<<","<<double(target.end100ns)*1e-7
            <<") s; "<<m_liveSegments->Count()<<" segments already on disk, "<<holes<<" hole(s) left in the range"
            <<"; pace="<<LiveRealtimeRatio()<<"x real time.");
        return true;
    }
    void StartLiveNeuralSession(){
        if(!LiveSessionAvailable()){LOG("Active neural session refused: loaded="<<m_loaded<<" cached="<<m_cachedPlayback<<" renderable="<<RangeRenderAvailable());return;}
        const double fps=m_decoder.FrameRate();const int64_t duration=SourceDuration100ns();
        if(!(fps>0.0)||duration<=0){
            LOG("Active neural session refused: fps="<<fps<<" duration100ns="<<duration);
            return;
        }
        const int64_t at=SnapToFrame(Position100ns());
        const NeuralRenderRange range=LiveSessionRange(at,fps,duration);
        // Every exit below this point used to be silent, which is how a session
        // that started and then did nothing left no way to tell which of them
        // it had taken.
        LOG("Active neural session starting at "<<double(at)*1e-7<<" s over ["
            <<double(range.start100ns)*1e-7<<","<<double(range.end100ns)*1e-7<<") s.");
        if(range.end100ns<=range.start100ns){
            LOG("Active neural session refused: the range holds no frame to render.");
            return;
        }
        if(!ConfirmLiveSessionPace(fps))return;
        // Frames rendered before the last toggle-off are still on disk, and they
        // are adopted whenever they belong to this video with these settings -
        // wherever on the timeline they sit. Requiring the playhead to be inside
        // them is what deleted a rendered tail the moment the user seeked back in
        // front of it, and then re-rendered ground that was already there.
        const std::string key=LiveRetentionKey();
        bool adopt=m_retainedSegments&&m_retainedKey==key&&!m_retainedSegments->Empty();
        // A published run is served from its cache entry, which lives outside
        // the session directory: another instance's eviction can take it while
        // the coverage sits retained. A region that cannot be opened is a hole
        // that looks rendered, so the whole retained set goes instead.
        if(adopt)
            for(const std::filesystem::path& file:m_retainedSegments->Files()){
                std::error_code ec;
                if(!std::filesystem::is_regular_file(file,ec)){
                    LOG("Retained neural coverage refers to a file that is gone ("<<WideToUtf8(file.wstring())<<"); rendering again.");
                    adopt=false;break;
                }
            }
        if(!adopt)DropRetainedLiveSegments();
        if(adopt){
            m_liveSegments=m_retainedSegments;m_liveDirectory=m_retainedDirectory;
            m_retainedSegments.reset();m_retainedDirectory.clear();m_retainedKey.clear();m_retainedRange={};
            SweepRetiredLiveSegments(m_liveSegments);
        }else{
            // Per process, and empty when there is no writable cache root. The
            // shared path let a second instance delete this one's segments, and
            // an empty root made it the relative "live" - so the remove_all
            // below ran against the process working directory.
            m_liveDirectory=live_session::SessionDirectory(m_cacheRoot,GetCurrentProcessId());
            if(m_liveDirectory.empty()){LOG("Active neural session has no writable cache root for its segments.");return;}
            std::error_code ec;std::filesystem::remove_all(m_liveDirectory,ec);std::filesystem::create_directories(m_liveDirectory,ec);
            if(ec){LOG("Active neural session could not create its segment directory.");m_liveDirectory.clear();return;}
            m_liveSegments=std::make_shared<NeuralSegmentIndex>();
        }
        m_liveRange=range;m_liveTarget={};m_liveSession=true;m_liveAttached=false;m_liveRenderFailures=0;
        // The pace clock starts at the first rendered segment, not here: a first
        // watch spends a minute acquiring the source before a frame is rendered,
        // and counting that made a GPU measured at 2x report 0.48x real time.
        m_livePaintedRevision=m_liveSegments->Revision();m_liveStartTick=0;m_neuralRequested=true;
        m_liveCoveredAtStart=CoveredDuration100ns(LiveCoverage(),CoverageSpan{range.start100ns,range.end100ns});
        m_livePaceWidth=m_decoder.Width();m_livePaceHeight=m_decoder.Height();
        // A GPU that renders far faster than real time refills the buffer faster
        // than playback drains it, so the four-second cushion is only a wait.
        m_liveForecastRatio=
            playback_timing::ForecastLiveRender(m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate(),
                                                m_renderPace,RenderPacePrior(m_opt.detectedGpu.generation)).realtimeRatio;
        m_liveStartLead=live_session::StartLead(m_liveForecastRatio,kLiveStartLead);
        EnterLiveBuffering();
        const auto target=WantedLiveTarget();
        if(!target)
            LOG("Active neural session replaying "<<m_liveSegments->Count()<<" retained segments; ["
                <<double(range.start100ns)*1e-7<<","<<double(range.end100ns)*1e-7<<") s is already rendered.");
        else if(!StartLiveRenderTarget(*target)){StopLiveNeuralSession(true);return;}
        SyncFeatureMenuState();UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();
    }
    // Drops the session state. Rendered segments are kept for the next toggle
    // unless the caller says the frames can no longer be trusted.
    void ReleaseLiveSession(bool retainSegments=false){
        // A retired job may still be writing into this session's directory.
        JoinRetiringNeuralWorker();
        const bool retain=retainSegments&&m_liveSegments&&!m_liveSegments->Empty()&&!m_liveDirectory.empty();
        if(retain){
            m_retainedSegments=m_liveSegments;m_retainedDirectory=m_liveDirectory;m_retainedRange=m_liveRange;m_retainedKey=LiveRetentionKey();
            const auto covered=m_retainedSegments->CoveredRanges();
            std::string spans;
            for(const CoverageSpan& span:covered){
                if(!spans.empty())spans+=", ";
                spans+="["+std::to_string(double(span.start100ns)*1e-7)+","+std::to_string(double(span.end100ns)*1e-7)+")";
            }
            SweepRetiredLiveSegments(m_retainedSegments);
            LOG("Retained "<<m_retainedSegments->Count()<<" rendered segments in "<<covered.size()
                <<" region(s) for the next toggle: "<<(spans.empty()?std::string("none"):spans)<<".");
        }
        m_liveSession=false;m_liveAttached=false;m_liveBuffering=false;m_liveResumePlaying=false;
        m_livePaintedRevision=0;m_liveStartTick=0;m_liveRange={};m_liveTarget={};m_liveCoveredAtStart=0;
        HideBufferOverlay();
        RecordLiveRenderPace();
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
            m_haveNext=false;m_next=VideoFrame{};m_nextPairFrame.reset();
            m_synchronizedPlayback.Close();m_cachedPlayback=false;m_havePresentedPair=false;ForgetRenderedCachedPair();m_cachedRange={};m_cachedPresentedFrames=0;m_comparisonView=ComparisonView::Original;
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
    // Playback follows a live session's own segments, so a job that published
    // none of them leaves nothing to follow. That happens for real: a session
    // whose render key is already in the cache is answered with the published
    // entry in about 50 ms, renders no frame, and appends nothing to the index.
    // The published entry covers exactly this session's range, which makes it
    // strictly better than what the session was going to produce, so playback
    // moves onto it at its coverage start.
    bool PlayPublishedEntryForLiveSession(const NeuralJobCompletion& completion){
        std::error_code entryError;
        if(completion.neuralPath.empty()||!std::filesystem::is_regular_file(completion.neuralPath,entryError)||entryError)return false;
        if(completion.sourcePath.empty()||completion.range.end100ns<=completion.range.start100ns)return false;
        const bool wasPlaying=m_playing||m_liveResumePlaying;
        LOG("Active neural session rendered nothing because its render key was already published; moving playback to the cache entry at "
            <<double(completion.range.start100ns)*1e-7<<" s. cacheHit="<<completion.cacheHit<<" frames="<<completion.result.frameCount
            <<" entry="<<WideToUtf8(completion.neuralPath.wstring()));
        // This job is finished and already joined, so the session has nothing
        // left to do. Release it here rather than letting LoadCachedPlayback's
        // Unload do it: that path cancels the job, and the cancel throws away
        // the cold-start record whose total the picture below is about to end.
        ReleaseLiveSession();
        if(!LoadCachedPlayback(completion)){
            LOG("Active neural session could not open the published entry it was handed; falling back to the original.");
            return false;
        }
        m_neuralLifecycle.Transition(NeuralPlaybackState::Ready);RecordRecent(completion);m_neuralNotice.clear();
        if(!wasPlaying)SetPaused(true);
        SyncFeatureMenuState();SyncSourceActionAvailability();UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();
        // The same sentence the segment attach logs, because it answers the
        // same question - the neural picture is on screen at this timestamp
        // with this much rendered video ahead of it - and every reader of this
        // log, the field matrix and the session instrument included, asks it
        // that way. Here the whole published range is already on disk, so the
        // lead is everything between the playhead and the end of the entry.
        LOG("Active neural playback attached at "<<m_currentSec<<" s with "
            <<std::max(0.0,double(completion.range.end100ns)*1e-7-m_currentSec)<<" s buffered.");
        return true;
    }
    // The job ended while its output is still playing: publish or report, but
    // never reload playback from the cache entry it just wrote - unless there is
    // no output, in which case that entry is the only thing there is to play.
    void CompleteLiveNeuralJob(const NeuralJobCompletion& completion){
        // Everything the published run retired that playback is not reading.
        SweepRetiredLiveSegments(m_liveSegments);
        const bool covered=m_liveSegments&&!m_liveSegments->Empty();
        // A job that ended without adding coverage must not be started again on
        // the same hole forever: a cache hit that publishes an entry but no
        // segment, and a range the worker refuses, both look like success. Two
        // fruitless jobs stop the session filling holes; what it rendered stays.
        const bool grew=m_liveSegments&&m_liveSegments->Revision()!=m_liveTargetRevision;
        if(completion.result.ok&&grew)m_liveRenderFailures=0;
        else if(++m_liveRenderFailures>=kLiveRenderFailureLimit)
            LOG("Active neural session stopped filling holes after "<<m_liveRenderFailures
                <<" job(s) that added no coverage; keeping "<<(m_liveSegments?m_liveSegments->Count():size_t{0})<<" segments.");
        std::error_code entryError;
        const live_session::CompletedSession finished{covered,completion.result.ok,
            !completion.neuralPath.empty()&&std::filesystem::is_regular_file(completion.neuralPath,entryError)&&!entryError};
        const live_session::CompletedSessionPlan plan=live_session::PlanForCompletedSession(finished);
        if(completion.result.ok){
            m_neuralLifecycle.Transition(NeuralPlaybackState::Ready);RecordRecent(completion);m_neuralNotice.clear();
            // Playback stays on the segments, but the published entry is what
            // "Save converted video" and the receipt need. The entry is this
            // job's hole, and the export labels it with the session's range,
            // so it is only on offer when the hole WAS the range.
            m_neuralPath=live_session::ExportableEntry(LiveCoverage(),CoverageSpan{m_liveRange.start100ns,m_liveRange.end100ns},
                                                       CoverageSpan{completion.range.start100ns,completion.range.end100ns},LiveFrame100ns())
                ?completion.neuralPath:std::filesystem::path{};
            m_cachedReceiptPath=completion.receiptPath;m_cachedSettings=completion.settings;m_cachedGuides=completion.guides;
            LOG("Active neural session rendered "<<completion.result.frameCount<<" frames and published its cache entry; save="<<(m_cachedPlayback&&!m_neuralPath.empty()&&!m_exportWorker.joinable()&&!ActivityBusy())<<" wholeRange="<<!m_neuralPath.empty()<<" entry="<<(completion.neuralPath.empty()?std::string("(none)"):WideToUtf8(completion.neuralPath.wstring())));
        }else{
            TransitionToFailure(completion.result.failure);NoteNeuralFailure(completion);
            LOG("Active neural session ended early: kind="<<NeuralRenderFailureName(completion.result.failure)<<" covered="<<covered<<" detail="<<WideToUtf8(completion.result.detail));
        }
        // An empty index is the one state every other decision reads as "keep
        // waiting": the session is finished with zero lead, so ShouldAttach is
        // false, there is no hole to retarget to, and the buffering panel never
        // comes down.
        // It is never right to leave the session standing there.
        if(plan==live_session::CompletedSessionPlan::PublishedEntry&&PlayPublishedEntryForLiveSession(completion))return;
        if(plan!=live_session::CompletedSessionPlan::Segments){
            LOG("Active neural session published no playable coverage; handing the original back. ok="<<completion.result.ok
                <<" cacheHit="<<completion.cacheHit<<" entry="<<(completion.neuralPath.empty()?std::string("(none)"):WideToUtf8(completion.neuralPath.wstring())));
            StopLiveNeuralSession(true);
            if(!completion.result.ok)return;
            m_neuralNotice=T(L"neural.live.stalled");
            UpdateCachedStatus();InvalidateControls();
            return;
        }
        SyncFeatureMenuState();SyncSourceActionAvailability();UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();
    }
    // Moves playback from the original stream onto the growing render, at the
    // frame that is on screen.
    bool AttachLiveNeural(){
        if(!m_liveSession||m_liveAttached||!m_loaded||!m_liveSegments)return false;
        // Where to join is a policy decision, tested without a window: coverage
        // that starts after the playhead is the frame playback continues from.
        // The region asked about is the one AROUND the playhead - the index's
        // first segment may belong to a region elsewhere in the video entirely.
        const auto span=LivePlayableSpan();
        if(!span)return false;
        const int64_t at100=live_session::AttachPosition100ns(
            static_cast<int64_t>(std::llround(Position()*1e7)),m_liveRange.start100ns,span->start100ns);
        const double at=double(at100)*1e-7;
        if(!m_liveSegments->Covered(at100))return false;
        const bool wasPlaying=m_playing||m_liveResumePlaying;
        Audio().Stop();m_haveNext=false;m_next=VideoFrame{};m_nextPairFrame.reset();
        if(!m_synchronizedPlayback.OpenLive(m_path,m_liveSegments,SynchronizedRange{m_liveRange.start100ns,m_liveRange.end100ns},{},m_decoder.Media(),PairPrefersNv12())){LOG("Active neural playback could not open the live pair.");return false;}
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
        NoteNeuralFramePresented();
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
        if(m_liveResumePlaying){m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=true;ResumeAudio();if(m_cachedPlayback)m_synchronizedPlayback.SetPaused(false);}
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
        if(m_playing||m_liveSession){NoteSettingsAheadOfRender();return;}
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
        if(!PausedPreviewAvailable()){NoteSettingsAheadOfRender();return;}
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
                if(RenderVideoFrame(frame,true)){m_previewNeuralFrame=frame;m_previewNeuralValid=true;m_previewShown=true;LOG("Neural settings preview presented for the paused frame.");}
                m_guideReset=false;m_dlssReset=false;
            }else LOG("Neural settings preview could not decode its rendered frame.");
            preview.Close();
        }else LOG("Neural settings preview discarded: the paused frame moved.");
        NoteSettingsAheadOfRender();SyncSourceActionAvailability();UpdateCachedStatus();InvalidateControls();
        if(m_previewQueued){m_previewQueued=false;LOG("Neural settings changed during the preview; previewing the newest settings.");StartPausedSettingsPreview();}
    }
    // Playback leaves the live pair and continues on the original decoder; the
    // session keeps rendering and re-attaches when it covers the playhead again.
    void DetachLivePlayback(){
        if(!m_liveAttached)return;
        m_liveAttached=false;m_haveNext=false;m_next=VideoFrame{};m_nextPairFrame.reset();
        m_synchronizedPlayback.Close();m_cachedPlayback=false;m_havePresentedPair=false;ForgetRenderedCachedPair();m_cachedRange={};m_cachedPresentedFrames=0;m_comparisonView=ComparisonView::Original;
        if(m_renderer)m_renderer->SetComparison(EffectiveComparison());
    }
    // Wall clock since the user last moved the playhead. Never having seeked
    // reads as forever ago, so a session nobody has touched is never held back.
    double SecondsSinceSeek()const{return m_lastSeekTick?double(GetTickCount64()-m_lastSeekTick)/1000.0:1e9;}
    // Keeps the render aimed at the hole the user needs: it starts the next hole
    // when a job ends, and moves the job when the playhead goes somewhere that
    // job will not reach. It never deletes coverage, which is the whole
    // difference from the rebase this replaces - that one stopped the session,
    // dropped every rendered segment the new playhead was not inside, and
    // rendered the same seconds again.
    void MaintainLiveRenderTarget(){
        if(!m_liveSession||!m_liveSegments)return;
        // A YouTube seek re-resolves the stream, and rendering is unavailable for
        // that whole window. Retargeting into it started nothing, twice in 40 ms,
        // which tripped the give-up limit and turned the session off behind the
        // user's seek - the reported "error on seeking back on a YouTube stream".
        // The commit that ends the resolve releases and restarts the session
        // anyway, on its retained coverage, so there is nothing to do until then.
        if(m_previewJob||m_seeking||m_seekPending||m_dragSeek||m_youtubeLifecycle.IsResolving())return;
        if(m_liveRenderFailures>=kLiveRenderFailureLimit){
            // Nothing is going to fill the hole the viewer is in, and every other
            // decision here says "wait": ShouldAttach sees no lead, the stalled
            // rebase needs one, so the buffering panel would stay up for good.
            // Hand the original back and say so, which is what the completed-job
            // Stop plan does for a session that published nothing.
            if(!m_liveAttached&&!LivePlayableSpan()){
                LOG("Active neural session gave up filling holes at "<<Position()<<" s after "
                    <<m_liveRenderFailures<<" fruitless jobs; playing the original.");
                StopLiveNeuralSession(true);
                m_neuralNotice=T(L"neural.live.stalled");
                UpdateCachedStatus();InvalidateControls();
            }
            return;
        }
        // Every decision below either starts a job or throws a running one away,
        // and both are priced in cold starts. While the playhead is still moving
        // the hole under it is not the one the viewer will be in, so acting on it
        // buys a helper launch the next press discards. The hand-back above still
        // runs: a viewer who is seeking must never be left in a buffering panel.
        if(SecondsSinceSeek() < live_session::kSeekSettleSeconds)return;
        const auto wanted=WantedLiveTarget();
        if(NeuralJobActive()){
            if(!wanted)return;
            if(!live_session::ShouldRetarget(LiveSessionView(),
                                             CoverageSpan{m_liveTarget.start100ns,m_liveTarget.end100ns},*wanted,
                                             LiveFrame100ns(),LiveJobReachSeconds()))
                return;
            LOG("Active neural session retargeting from ["<<double(m_liveTarget.start100ns)*1e-7<<","
                <<double(m_liveTarget.end100ns)*1e-7<<") to ["<<double(wanted->start100ns)*1e-7<<","
                <<double(wanted->end100ns)*1e-7<<") s for the playhead at "<<Position()<<" s; keeping "
                <<m_liveSegments->Count()<<" rendered segments.");
            // Not joined: the new target starts once the retired job's
            // completion arrives, a Tick or two later.
            CancelNeuralJob(false,true);
        }
        if(!wanted){
            // Every frame of the range is rendered: nothing more to start, and
            // the whole of it is now seekable on the render.
            if(m_liveTarget.end100ns>m_liveTarget.start100ns){
                LOG("Active neural session rendered all of ["<<double(m_liveRange.start100ns)*1e-7<<","
                    <<double(m_liveRange.end100ns)*1e-7<<") s over "<<m_liveSegments->Count()<<" segments.");
                m_liveTarget={};UpdateCachedStatus();InvalidateControls();
            }
            return;
        }
        // A render that cannot be started right now is not a job that rendered
        // nothing: only a job that actually ran and added no coverage counts
        // towards giving up, and the completion path is what records that. This
        // one comes back next tick.
        if(!RangeRenderAvailable()){
            if(m_liveTarget.end100ns<=m_liveTarget.start100ns)
                LOG("Active neural session is waiting to render ["<<double(wanted->start100ns)*1e-7<<","
                    <<double(wanted->end100ns)*1e-7<<") s: rendering is unavailable right now.");
            return;
        }
        if(!StartLiveRenderTarget(*wanted)){
            ++m_liveRenderFailures;
            LOG("Active neural session could not start a render for ["<<double(wanted->start100ns)*1e-7<<","
                <<double(wanted->end100ns)*1e-7<<") s; keeping the coverage it has.");
        }
    }
    live_session::SessionView LiveSessionView()const{
        // What the policy measures against is the hole being rendered, not the
        // session's whole range: attaching, resuming and moving the render all
        // turn on where THIS job will reach.
        return {Position(),double(m_liveTarget.start100ns)*1e-7,LiveHeadSeconds(),m_liveAttached,LiveSessionFinished(),
                m_dragSeek||m_seeking||m_seekPending,!m_playing&&!m_liveResumePlaying};
    }
    // UI-thread side of the session: keep the render aimed where the user is,
    // repaint as coverage arrives, start playing once the lead-in is buffered,
    // and resume after a rebuffer.
    void UpdateLiveSession(){
        if(!m_liveSession)return;
        MaintainLiveRenderTarget();
        if(!m_liveSession)return;
        // The file playback was reading when its run was published is freed
        // at the next boundary; a second's latency on that costs nothing.
        if(const ULONGLONG now=GetTickCount64();now-m_liveSweepTick>=1000){m_liveSweepTick=now;SweepRetiredLiveSegments(m_liveSegments);}
        // Coverage can change without the newest rendered timestamp moving - a
        // run filling an earlier hole does exactly that - so the repaint follows
        // the index's revision instead of a head.
        const uint64_t revision=m_liveSegments?m_liveSegments->Revision():0;
        if(revision!=m_livePaintedRevision){
            if(!m_liveStartTick){
                m_liveStartTick=GetTickCount64();
                // Only the clock starts here. The baseline stays the coverage the
                // session began with: this bump IS the first segment, so rebasing
                // on it would subtract that segment from every later reading.
            }
            m_livePaintedRevision=revision;InvalidatePlaybackProgress();RefreshBufferOverlay();UpdateCachedStatus();
        }
        const live_session::SessionView view=LiveSessionView();
        if(!m_liveAttached){
            if(live_session::ShouldAttach(view,m_liveStartLead)){
                if(AttachLiveNeural()){m_liveAttachFailures=0;m_liveStalledRebases=0;ExitLiveBuffering();return;}
                ++m_liveAttachFailures;
            }
            // A lead that never becomes a picture is the coverage being in the
            // wrong place, not a slow render. One restart at the playhead is a
            // real recovery; repeating it is a loop, and this one did loop, so
            // the second failure ends the session and hands the original stream
            // back instead of re-rendering the same seconds forever.
            if(live_session::ShouldRebaseStalledAttach(view,m_liveAttachFailures)){
                LOG("Active neural playback never attached at "<<Position()<<" s with head "<<LiveHeadSeconds()
                    <<" s over "<<(m_liveSegments?m_liveSegments->Count():size_t{0})<<" segments after "
                    <<m_liveAttachFailures<<" attempts; rebase "<<(m_liveStalledRebases+1)<<" of "<<kLiveStalledRebaseLimit<<".");
                m_liveAttachFailures=0;
                if(++m_liveStalledRebases>kLiveStalledRebaseLimit){
                    LOG("Active neural session gave up: its coverage never reached the playhead. Playing the original.");
                    m_liveStalledRebases=0;
                    StopLiveNeuralSession(true);
                    m_neuralNotice=T(L"neural.live.stalled");
                    UpdateCachedStatus();InvalidateControls();
                    return;
                }
                // Retargeted at the playhead rather than restarting the session:
                // the coverage on disk is still playable everywhere else in the
                // video, and re-rendering it is what made this loop expensive.
                if(const auto hole=WantedLiveTarget()){
                    CancelNeuralJob(false,true);
                    // Same rule as above: a refusal that only means "not right
                    // now" is not a job that rendered nothing.
                    if(RangeRenderAvailable()&&!StartLiveRenderTarget(*hole))++m_liveRenderFailures;
                }
            }
            return;
        }
        m_liveAttachFailures=0;m_liveStalledRebases=0;
        // Re-sized every tick rather than latched at session start: the
        // forecast is a constant for the GPU, the measurement is what this
        // render is managing against everything else on the machine, and a
        // frame-generated source is exactly where the two differ.
        m_liveStartLead=live_session::StartLead(LivePaceRatio(),kLiveStartLead);
        if(m_liveBuffering&&live_session::ShouldResume(view,LiveResumeLead()))ExitLiveBuffering();
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
        const double lead=LiveLeadSeconds(),target=m_liveAttached?LiveResumeLead():m_liveStartLead;
        wchar_t detail[128]={};
        if(m_previewJob)swprintf_s(detail,L"%s",T(L"neural.preview.detail").c_str());
        // A session that is still acquiring its source has no frames to report,
        // and "0 frames rendered" is exactly what a stall looks like.
        else if(m_neuralProgress.phase==NeuralRenderPhase::Acquiring)swprintf_s(detail,L"%s",AcquisitionDetailText().c_str());
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
    // Where a render may BEGIN, which is not the same question. SnapToFrame
    // rounds to the nearest boundary, and nearest rounds forward from anything
    // past the half-frame - over the frame the caller is pointing at. A hole
    // begins where a retained region's last segment ran out of container
    // timestamps, a few ticks below the grid boundary of the frame that follows
    // it; nearest snapped the job's start past that frame, the worker's seek
    // landed on the frame after it, and the job started to fill the hole left
    // the hole exactly as it found it. Down is the rule RangeFromMarkers
    // already gives an in marker, and the overlap it leaves against the region
    // behind is what merges the two back into one instead of parting them with
    // a frame nobody renders.
    int64_t SnapRenderStart(int64_t pts)const{const double fps=m_decoder.FrameRate();return fps>0?FramePts(FrameIndexAtOrBefore(pts,fps),fps):pts;}
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
    static std::string GpuPathName(GpuGeneration generation){return GpuGenerationPathName(generation);}
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
    // A failed render used to leave nothing on screen. The detail was written to the
    // log and then dropped, so a user whose driver is too old pressed the neural
    // toggle, watched the original keep playing, and had no way to learn why without
    // opening a file. This keeps a one-line reason in the status bar, and for the
    // driver case - the one thing the user can actually act on - shows the full
    // notice once per session. Once, because the refusal repeats on every play,
    // seek and settings change, and a dialog on each of those is worse than silence.
    // Returns true when it put the notice on screen, so a caller that would show its
    // own dialog does not stack a second one on top.
    bool NoteNeuralFailure(const NeuralJobCompletion& completion){
        const bool belowFloor=completion.result.failure==NeuralRenderFailure::Preflight&&
                              ClassifyNeuralDriver(m_opt.detectedGpu.driverVersion)==NeuralDriverSupport::BelowFloor;
        if(belowFloor)m_neuralNotice=T(L"driver.below_floor");
        else if(const wchar_t* kind=NeuralFailureTextKey(completion.result.failure))m_neuralNotice=T(kind);
        else m_neuralNotice=completion.result.detail;
        if(!belowFloor||m_driverNoticeShown)return false;
        m_driverNoticeShown=true;
        const std::wstring notice=NeuralDriverNoticeText();
        if(notice.empty())return false;
        MessageBoxW(m_hwnd,notice.c_str(),T(L"app.title").c_str(),MB_OK|MB_ICONWARNING);
        return true;
    }
    // The progress updates own heap memory, so the queued ones are taken and
    // deleted rather than left for USER32 to discard with the window. Safe to
    // call only after the worker is joined, which WM_DESTROY does first.
    void DrainStageExportMessages(){
        if(!m_hwnd)return;MSG message{};
        while(PeekMessageW(&message,m_hwnd,WM_STAGE_EXPORT_PROGRESS,WM_STAGE_EXPORT_PROGRESS,PM_REMOVE))
            delete reinterpret_cast<StageExportProgress*>(message.lParam);
    }
    void DrainNeuralMessages(){m_neuralProgressMessages.Clear();m_neuralCompletions.Clear();if(!m_hwnd)return;MSG message{};while(PeekMessageW(&message,m_hwnd,WM_NEURAL_PROGRESS,WM_NEURAL_COMPLETE,PM_REMOVE)){};}
    // One line per render, in the same terse register as the receipt summary,
    // so a user's log carries the whole breakdown without the receipt file.
    void ReportNeuralColdStart(){
        if(!m_coldStart||!m_coldStart->ClaimReport())return;
        // The helper's phases are absent whenever no helper ran, and absent
        // again for the two a reused resident helper did not pay. A reader
        // cannot tell either from a lost measurement, so the note names which
        // helper the line describes: none(cache-hit), reuse, launch, relaunch
        // or single-shot. The field order the matrix tool parses is unchanged.
        const std::string helper=m_coldStart->HelperNote();
        LOG("Neural cold start: "<<SummarizeNeuralColdStartForLog(m_coldStart->Snapshot())
            <<(helper.empty()?std::string{}:" helper="+helper));
    }
    // The first neural frame of this render is on screen. Reported here rather
    // than at the completion, because an active session that is toggled off
    // never delivers a completion to report at.
    void NoteNeuralFramePresented(){
        if(!m_coldStart)return;
        m_coldStart->Presented();
        ReportNeuralColdStart();
    }
    // `retire` requests the stop and returns without joining: the worker is
    // parked in m_retiringNeuralWorker and joined when its own completion
    // message arrives, as frame generation does. A live retarget used to join
    // here on the UI thread, so playback froze for as long as the helper took
    // to notice the token. Every other caller still waits, and joins a
    // retiring worker first, because the resident helper and the preflight
    // latch belong to one job thread at a time.
    void CancelNeuralJob(bool updateUi=true,bool retire=false){
        if(!retire)JoinRetiringNeuralWorker();
        if(!NeuralJobActive())return;
        // A preview renders behind the paused frame; an offline job replaced
        // the media and has an original to hand back. Read before the flags
        // below are dropped.
        const bool behindPlayback=JobBehindPlayback();
        const uint64_t cancelledGeneration=m_neuralLifecycle.generation;
        m_neuralLifecycle.Transition(NeuralPlaybackState::Cancelling);if(m_neuralPauseEvent)ResetEvent(m_neuralPauseEvent);
        if(m_neuralWorker.joinable()){
            m_neuralWorker.request_stop();
            if(retire){
                JoinRetiringNeuralWorker();
                m_retiringNeuralWorker=std::move(m_neuralWorker);m_retiringNeuralGeneration=cancelledGeneration;
                LOG("Neural job "<<cancelledGeneration<<" asked to stop; it is retired by its completion message.");
            }else m_neuralWorker.join();
            m_neuralWorker=std::jthread{};
        }
        std::unique_ptr<NeuralJobCompletion> cancelledCompletion;
        if(m_hwnd){MSG message{};while(PeekMessageW(&message,m_hwnd,WM_NEURAL_PROGRESS,WM_NEURAL_COMPLETE,PM_REMOVE)){
            if(message.message==WM_NEURAL_PROGRESS)m_neuralProgressMessages.Remove(static_cast<uint64_t>(message.wParam));
            else if(message.message==WM_NEURAL_COMPLETE)cancelledCompletion=m_neuralCompletions.Take(static_cast<uint64_t>(message.wParam));
        }}
        m_neuralProgressMessages.Clear();m_neuralCompletions.Clear();m_neuralLifecycle.Invalidate();
        // A dropped preview has nothing to present: the paused frame stays as
        // it is, the panel comes down, and the next settings change starts a
        // fresh one. Left set, these kept the panel up and refused every
        // later preview for the rest of the file.
        if(m_previewJob){m_previewJob=false;m_previewQueued=false;HideBufferOverlay();}
        if(updateUi){m_neuralProgress={};m_neuralCancelBounds={};InvalidateRect(m_hwnd,nullptr,FALSE);SyncSourceActionAvailability();
            std::error_code sourceError;
            if(cancelledCompletion&&!behindPlayback&&!cancelledCompletion->sourcePath.empty()&&std::filesystem::is_regular_file(cancelledCompletion->sourcePath,sourceError)){
                m_neuralLifecycle.Transition(NeuralPlaybackState::OriginalOnly);
                LoadOriginalFallback(*cancelledCompletion);
            }
        }
        LOG("Neural pre-render cancelled and worker stopped.");
        // The cancelled job's measurements describe a picture that never came
        // and must not be mistaken for the next render's.
        m_coldStart.reset();
    }
    // `prepareOnly` acquires and identifies the source, replays a validated
    // cache entry when one exists, and otherwise stops before the feature-18
    // probe so the player can open the original for range selection.
    // A Live or Preview job renders behind the loaded media instead of
    // replacing it: Live streams segments, Preview renders one paused frame.
    // False when no worker was started: the lifecycle is back to idle, and the
    // caller must not count the job as running.
    bool StartNeuralJob(const std::wstring& mediaUrl,const std::wstring& audioUrl,const std::wstring& displayTitle,const std::wstring& pageUrl,MediaSourceKind sourceKind,YouTubeSourceQuality sourceQuality,const std::string& reuseSourceKey={},double expectedDurationSeconds=0.0,NeuralRenderRange range={},bool prepareOnly=false,NeuralJobKind kind=NeuralJobKind::Offline){
        if(mediaUrl.empty()){LOG("Neural job refused: the source has no path to render.");return false;}
        CancelNeuralJob(false);
        if(kind==NeuralJobKind::Offline)Unload();
        const uint64_t generation=m_neuralLifecycle.Begin();m_neuralProgress={};m_neuralProgress.phase=NeuralRenderPhase::CheckingCache;m_pendingNeuralTitle=DisplayTitleForSource(sourceKind,displayTitle);m_neuralSourceWidth=0;m_neuralSourceHeight=0;if(m_neuralPauseEvent)ResetEvent(m_neuralPauseEvent);
        SyncSourceActionAvailability();InvalidateRect(m_hwnd,nullptr,FALSE);
        try{
            HWND target=m_hwnd;const auto gpu=m_opt.detectedGpu.generation;const std::wstring driverVersion=m_opt.detectedGpu.driverVersion;const auto moduleDirectory=ExecutableDirectory();const auto cacheRoot=m_cacheRoot;const GuideControls guides=m_renderGuides;const NeuralSettings settings=m_neuralSettings;const HANDLE pauseEvent=m_neuralPauseEvent;const bool gpuColorConversion=m_gpuColorConversion;const bool gpuSourceConversion=m_gpuSourceConversion;const uint32_t nvencPreset=m_nvencPreset;
            // The background acquisition of this very source, when one is in
            // flight: the job waits for it rather than downloading again.
            const std::shared_ptr<SourcePrefetchState> prefetch=(sourceKind==MediaSourceKind::YouTube&&!pageUrl.empty()&&pageUrl==m_prefetchPageUrl)?m_prefetchState:nullptr;
            CompletionRegistry<NeuralProgressMessage>* progressMessages=&m_neuralProgressMessages;CompletionRegistry<NeuralJobCompletion>* completions=&m_neuralCompletions;
            // An active session renders into its own directory of segment files;
            // the cache entry is the concatenation published when the job ends.
            // A resumed or retargeted session keeps every earlier job's files, so
            // each job gets its own subdirectory and its own run id. The id is
            // what makes a relaunch discard its own segments and nobody else's:
            // segments are sorted by timestamp now, so this job's are not
            // necessarily the tail of the index.
            const std::shared_ptr<NeuralSegmentIndex> liveIndex=kind==NeuralJobKind::Live?m_liveSegments:nullptr;
            std::filesystem::path liveDirectory;
            const uint64_t liveRunId=liveIndex?uint64_t(++m_liveJobSerial):0u;
            if(liveIndex){
                liveDirectory=m_liveDirectory/(L"job"+std::to_wstring(liveRunId));
                std::error_code ec;std::filesystem::create_directories(liveDirectory,ec);
                if(ec){
                    // Begin() above already marked a job as running; leaving it
                    // there kept the spinner on and every render action greyed
                    // out for the rest of the file, with nothing to say why.
                    LOG("Active neural session could not create the segment directory "<<WideToUtf8(liveDirectory.wstring())<<": "<<ec.message());
                    m_neuralLifecycle.Invalidate();m_neuralProgress={};m_pendingNeuralTitle.clear();
                    m_neuralNotice=T(L"neural.live.directory_failed");
                    SyncSourceActionAvailability();UpdateCachedStatus();InvalidateRect(m_hwnd,nullptr,FALSE);
                    return false;
                }
            }
            const uint32_t segmentFrames=kind==NeuralJobKind::Live?static_cast<uint32_t>(std::max<long>(1,std::lround(m_decoder.FrameRate()*kLiveSegmentSeconds))):0u;
            // Nothing can be shown until the first file is muxed, so the first
            // one is short. Later files stay long: a boundary costs an encoder
            // start and a mux, and only the first one is on the user's clock.
            const uint32_t firstSegmentFrames=kind==NeuralJobKind::Live?static_cast<uint32_t>(std::max<long>(1,std::lround(m_decoder.FrameRate()*kLiveFirstSegmentSeconds))):0u;
            // The driver verdict and the latched preflight failure are read on
            // this thread; the job only needs the answers.
            const std::wstring driverNotice=NeuralDriverNoticeText();
            const NeuralCacheFailureText cacheFailureText=CacheFailureText();
            const NeuralPreflightKey preflightKey{m_opt.detectedGpu.description,m_opt.detectedGpu.driverVersion,std::string{}};
            NeuralPreflightLatch* preflightLatch=&m_preflightLatch;
            // The helper this player keeps between jobs. Reached by pointer, like
            // the latch above: one job runs at a time and the UI thread joins it
            // before touching either, so the job thread owns both while it runs.
            ResidentNeuralHelper* residentHelper=&m_residentHelper;
            // The request instant: every phase below is measured from here, and
            // the total the acceptance criterion names ends when the first
            // neural frame reaches the screen.
            m_coldStart=std::make_shared<NeuralColdStartRecord>();
            const std::shared_ptr<NeuralColdStartRecord> coldStart=m_coldStart;
            m_neuralWorker=std::jthread([target,generation,mediaUrl,audioUrl,displayTitle,pageUrl,sourceKind,sourceQuality,gpu,driverVersion,moduleDirectory,progressMessages,completions,reuseSourceKey,cacheRoot,expectedDurationSeconds,range,guides,settings,pauseEvent,prepareOnly,prefetch,liveIndex,liveDirectory,liveRunId,segmentFrames,firstSegmentFrames,driverNotice,cacheFailureText,preflightKey,preflightLatch,residentHelper,coldStart,gpuColorConversion,gpuSourceConversion,nvencPreset,sourceDigestMemo=m_sourceDigestMemo](std::stop_token stop){
                auto completion=std::make_unique<NeuralJobCompletion>();completion->generation=generation;completion->displayTitle=displayTitle;completion->pageUrl=pageUrl;completion->sourceKind=sourceKind;completion->sourceQuality=sourceQuality;
                uint32_t progressWidth=0,progressHeight=0;
                // Set the moment the job knows its local source; every progress
                // post after that carries it, so playback can leave the stream.
                std::filesystem::path progressSourcePath;
                std::string progressSourceKey;
                auto postProgress=[&](const NeuralRenderProgress& progress){auto message=std::make_unique<NeuralProgressMessage>();message->generation=generation;message->progress=progress;message->width=progressWidth;message->height=progressHeight;message->sourcePath=progressSourcePath;message->sourceKey=progressSourceKey;message->pageUrl=pageUrl;progressMessages->RegisterAndPost(std::move(message),[&](uint64_t token){return PostMessageW(target,WM_NEURAL_PROGRESS,static_cast<WPARAM>(token),0)!=FALSE;});};
                NeuralCacheManager cache(cacheRoot);if(!cache.Valid()){completion->result.detail=cacheFailureText.DescribeRoot(cache.LastFailure());goto finish;}
                {
                    std::filesystem::path sourcePath;
                    if(sourceKind==MediaSourceKind::YouTube){
                        std::string reuseKey=reuseSourceKey;
                        // Wait for an acquisition of this same source whenever one is
                        // in flight, not only when no key is known: the recents entry
                        // can already name the key that acquisition is still writing,
                        // and looking it up mid-download finds an incomplete entry. A
                        // live session that hit that ended with no message at all.
                        if(prefetch&&!prefetch->finished.load(std::memory_order_acquire)){
                            LOG("Waiting for the background source acquisition.");
                            NeuralRenderProgress acquiring{};acquiring.phase=NeuralRenderPhase::Acquiring;postProgress(acquiring);
                            while(!prefetch->finished.load(std::memory_order_acquire)&&!stop.stop_requested())
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                        if(stop.stop_requested()){completion->result.cancelled=true;completion->result.detail=L"Neural render was cancelled.";goto finish;}
                        if(reuseKey.empty()&&prefetch)reuseKey=prefetch->key;
                        if(!reuseKey.empty()){
                            const auto cached=cache.LookupSource(reuseKey,stop);
                            // A cancelled lookup reads as a missing copy; it must not be reported as one.
                            if(stop.stop_requested()){completion->result.cancelled=true;completion->result.detail=L"Neural render was cancelled.";goto finish;}
                            if(cached&&cached->manifest.encoder==kCompleteSourcePolicy){
                                sourcePath=cached->payloadPath;completion->sourceKey=reuseKey;
                                LOG("Owned source cache verified; network resolution skipped.");
                            }else if(audioUrl.empty()){
                                // No stream pair in hand, so there is nothing to acquire
                                // from: the UI answers this by resolving the page again.
                                completion->cachedSourceUnavailable=true;
                                completion->result.detail=L"The downloaded copy of this video is no longer available.";goto finish;
                            }else{
                                // A recorded key whose copy is gone or half-written, with
                                // the stream this session is already playing still in hand.
                                // Acquiring again beats ending the session.
                                LOG("The recorded source copy is missing or incomplete; acquiring this stream again.");
                                reuseKey.clear();
                            }
                        }
                        if(sourcePath.empty()){
                            const SourceAcquisition acquired=AcquireYouTubeSource(cache,cacheFailureText,moduleDirectory,mediaUrl,audioUrl,pageUrl,sourceQuality,expectedDurationSeconds,
                                [&](const MediaDownloadProgress& download){
                                    NeuralRenderProgress acquiring{};acquiring.phase=NeuralRenderPhase::Acquiring;
                                    acquiring.bytes=download.bytes;acquiring.acquiredSeconds=download.seconds;
                                    acquiring.expectedSeconds=expectedDurationSeconds;postProgress(acquiring);
                                },stop);
                            if(!acquired.key.empty())completion->sourceKey=acquired.key;
                            if(acquired.path.empty()){completion->result.cancelled=acquired.cancelled;completion->result.detail=acquired.detail;goto finish;}
                            sourcePath=acquired.path;
                        }
                    }else{
                        std::error_code pathError;
                        sourcePath=std::filesystem::absolute(std::filesystem::path(mediaUrl),pathError);
                        if(pathError){completion->result.detail=L"The source path could not be resolved.";goto finish;}
                    }
                    completion->sourcePath=sourcePath;
                    // Published to the UI thread from here, which is what lets
                    // playback move off a stream and onto this copy.
                    progressSourcePath=sourcePath;progressSourceKey=completion->sourceKey;
                    // Memoised per loaded file. Every job used to full-hash
                    // the source first, including the prepare-only cache check
                    // and every live retarget, which is 3-5 s of dead air on a
                    // 5 GB file each time it is asked for.
                    const auto sourceDigest=MemoisedSourceDigest(*sourceDigestMemo,sourcePath,stop);if(!sourceDigest){completion->result.cancelled=stop.stop_requested();completion->result.detail=L"The source digest could not be computed.";goto finish;}
                    // Metadata only: this decoder was opened and closed two lines
                    // later, and a full open paid for an ffmpeg child for nothing.
                    VideoDecoder metadata;if(!metadata.OpenMetadata(sourcePath.wstring(),MediaSourceKind::LocalFile,stop)){completion->result.detail=L"The source could not be decoded for neural rendering.";goto finish;}
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
                    // The pass evaluates models out of the driver store, which no
                    // staged file covers: without these two terms a render made on
                    // one driver is served and validated on a later one.
                    // The pipeline term carries `bt709-export-v1` because the
                    // capture-side encoder arguments are deliberately not part of
                    // this key and the export colorimetry changed: renders written
                    // before that fix carry BT.601 pixels with no colour tags, and
                    // without moving the term they would stay valid hits under an
                    // unchanged VERSION. `GpuSourceConversion` is the one conversion
                    // switch that does belong in the key, because it changes what the
                    // model is shown rather than how the result is encoded.
                    const auto modelStore=ResolveNeuralModelStore(driverVersion,stop);
                    LOG("Neural model store "<<NeuralModelStoreSourceName(modelStore.source)<<" files="<<modelStore.files<<" hashed="<<modelStore.contentHashedFiles<<" digest="<<modelStore.digest);
                    NeuralCacheIdentity identity{*sourceDigest,width,height,DLSS_VIDEO_PLAYER_VERSION,GpuPathName(gpu),*runtimeDigest,NeuralRenderPipelineIdentity(gpuSourceConversion,nvencPreset,gpuColorConversion),false,*settingsDigest,range,guides.IsDefault()?std::string{}:CanonicalGuideControls(guides),WideToUtf8(driverVersion),modelStore.digest};const std::string renderKey=BuildNeuralCacheKey(identity);completion->renderKey=renderKey;completion->range=range;completion->settings=settings;completion->guides=guides;
                    LOG("Checking neural cache key="<<renderKey<<" range=["<<range.start100ns<<","<<range.end100ns<<") guides="<<CanonicalGuideControls(guides)<<" settings="<<CanonicalNeuralSettings(settings));
                    if(const auto cached=cache.LookupRender(renderKey,stop)){
                        // LookupRender already verifies the full payload hash and
                        // strict feature-18 manifest. Do not decode every frame again.
                        const ProbeResult cachedProbe=ProbeMedia(moduleDirectory,cached->payloadPath,stop,MediaProbeMode::CachedMetadata);
                        if(stop.stop_requested()){completion->result.cancelled=true;completion->result.detail=L"Neural render was cancelled.";goto finish;}
                        const int64_t durationTolerance=std::max<int64_t>(1,cached->manifest.duration100ns/static_cast<int64_t>(cached->manifest.frameCount)+1);
                        // Split by what each piece of evidence needs. The
                        // manifest is compared in process; everything else
                        // needs ffprobe to have run. Reading a probe that
                        // could not run as a probe that disagreed quarantined
                        // - and then deleted - entries whose payload had just
                        // been hash-verified as intact.
                        const cached_render::Evidence evidence{
                            cachedProbe.ok,
                            cached->manifest.sourceDigest==*sourceDigest&&cached->manifest.runtimeDigest==*runtimeDigest&&cached->manifest.settingsDigest==*settingsDigest&&cached->manifest.rangeStart100ns==range.start100ns&&cached->manifest.rangeEnd100ns==range.end100ns&&cached->manifest.guides==identity.guides,
                            cachedProbe.width==width&&cachedProbe.height==height&&cachedProbe.width==cached->manifest.width&&cachedProbe.height==cached->manifest.height,
                            std::llabs(cachedProbe.duration100ns-cached->manifest.duration100ns)<=durationTolerance&&std::llabs(cachedProbe.duration100ns-expectedDuration100ns)<=frameDurationTolerance};
                        const auto verdict=cached_render::Judge(evidence);
                        const bool valid=verdict==cached_render::Verdict::Serve;
                        // A validated hit is answered without a helper, so the
                        // five phases a helper measures are absent by nature.
                        if(valid){coldStart->NoteNoHelper("cache-hit");completion->result.ok=true;completion->result.frameCount=cached->manifest.frameCount;completion->result.duration100ns=cached->manifest.duration100ns;completion->result.jobId=cached->manifest.jobId;completion->result.historyResets=cached->manifest.historyResets;completion->result.firstTimestamp100ns=cached->manifest.rangeStart100ns;completion->sourcePath=sourcePath;completion->neuralPath=cached->payloadPath;completion->cacheHit=true;completion->range={cached->manifest.rangeStart100ns,cached->manifest.rangeEnd100ns};if(!cached->manifest.receiptDigest.empty()&&std::filesystem::is_regular_file(cached->directory/L"receipt.json"))completion->receiptPath=cached->directory/L"receipt.json";goto finish;}
                        if(verdict==cached_render::Verdict::Unverified){
                            LOG("Neural cache entry "<<renderKey<<" could not be verified because the probe did not run ("
                                <<WideToUtf8(cachedProbe.detail)<<"); it is kept and this render proceeds without it.");
                        }else if(!cache.Quarantine(*cached)){completion->result.detail=L"The invalid neural cache entry could not be quarantined.";goto finish;}
                    }
                    // A cancelled lookup reads as a miss; it must not start a render.
                    if(stop.stop_requested()){completion->result.cancelled=true;completion->result.detail=L"Neural render was cancelled.";goto finish;}
                    // The cache check every open runs: no helper, and the line
                    // it logs is not a render that failed to measure.
                    if(prepareOnly){coldStart->NoteNoHelper("range-selection");completion->preparedOnly=true;LOG("Neural cache miss; opening the original for range selection.");goto finish;}
                    LOG("Neural cache miss or invalid entry; starting a new render.");
                    // Everything above is the player's own preparation; from
                    // here the cost belongs to the probe and the helper.
                    coldStart->Mark(NeuralColdStartPhase::Request);
                    // A driver below the floor cannot create feature 18 at all,
                    // so do not pay five seconds for a probe to learn that.
                    if(!driverNotice.empty()){completion->result.failure=NeuralRenderFailure::Preflight;completion->result.detail=driverNotice;LOG("Neural render refused before the probe: "<<WideToUtf8(driverNotice));goto finish;}
                    // A stray module beside the helper is refused by the helper at
                    // startup - after a process start and, on a first run, a
                    // five-second preflight. It is named here instead, in the
                    // helper's own words, and never latched: the directory is
                    // listed again on every attempt, so removing the file is all
                    // it takes. A directory that cannot be listed is left to the
                    // helper, which refuses it with its own reason.
                    if(const auto unlocked=FindUnlockedRuntimeModules(runtimeDirectory,EmbeddedRuntimeLock());unlocked&&!unlocked->empty()){
                        completion->result.failure=NeuralRenderFailure::Preflight;completion->result.detail=runtime_modules::UnlockedModulesRefusal(*unlocked);
                        LOG("Neural render refused before the helper: "<<WideToUtf8(completion->result.detail));
                        goto finish;
                    }
                    // The same runtime on the same driver fails the same way:
                    // probe once per configuration, not once per play and seek.
                    // A failure is remembered against the module listing too: the
                    // digest hashes only the locked files, so a refusal caused by
                    // anything else the loader picks up from that directory stood
                    // until restart even after the user removed the cause.
                    const NeuralPreflightKey runtimeKey{preflightKey.gpu,preflightKey.driver,*runtimeDigest};
                    const NeuralPreflightKey failureKey{preflightKey.gpu,preflightKey.driver,
                        *runtimeDigest+"|"+WideToUtf8(runtime_modules::ModuleListingIdentity(runtimeDirectory))};
                    if(const std::wstring latched=preflightLatch->LatchedFailureDetail(failureKey);!latched.empty()){
                        completion->result.failure=NeuralRenderFailure::Preflight;completion->result.detail=latched;
                        LOG("Neural preflight skipped; this runtime and driver already failed: "<<WideToUtf8(latched));
                        goto finish;
                    }
                    const auto workerExecutable=runtimeDirectory/L"NeuralWorker.exe";
                    // A pass is as reusable as a failure: the probe answers for a
                    // GPU, a driver and a runtime, not for a playback session, and
                    // paying five seconds per toggle for the same answer is what
                    // made the picture take sixteen seconds to appear.
                    std::string preflightJson=preflightLatch->LatchedSuccessJson(runtimeKey);
                    if(preflightJson.empty())preflightJson=LoadNeuralPreflightReceipt(cacheRoot,runtimeKey);
                    if(!preflightJson.empty()){
                        preflightLatch->RecordSuccess(runtimeKey,preflightJson);
                        preflightJson=MarkReusedNeuralPreflight(preflightJson);
                        LOG("Neural preflight skipped; this runtime and driver already armed feature 18.");
                    }else{
                        // The feature-18 probe needs the GPU; only a cache miss pays for it.
                        // It also needs the runtime to itself: it loads its own proxy and
                        // creates its own feature 18, and an idle resident helper from an
                        // earlier job is still holding the device and the session log. That
                        // helper is on its way out regardless - a probe only runs when the
                        // runtime identity in its key changed.
                        residentHelper->Release();
                        NeuralRenderProgress preflighting{};preflighting.phase=NeuralRenderPhase::Preflight;postProgress(preflighting);
                        const NeuralPreflightResult preflight=RunNeuralPreflight(workerExecutable,stop);
                        // Marked before the verdict is read: a probe that failed
                        // or was cancelled still cost what it cost.
                        coldStart->Mark(NeuralColdStartPhase::Preflight);
                        if(preflight.cancelled||stop.stop_requested()){completion->result.cancelled=true;completion->result.detail=L"Neural render was cancelled.";goto finish;}
                        if(!preflight.ok){completion->result.failure=NeuralRenderFailure::Preflight;completion->result.detail=preflight.detail.empty()?L"The neural runtime preflight failed.":preflight.detail;preflightLatch->RecordFailure(failureKey,completion->result.detail);LOG("Neural preflight failed: cause="<<NeuralPreflightCauseName(preflight.cause)<<" "<<WideToUtf8(completion->result.detail)<<(preflight.json.empty()?"":" receipt=")<<preflight.json);goto finish;}
                        preflightLatch->RecordSuccess(runtimeKey,preflight.json);
                        StoreNeuralPreflightReceipt(cacheRoot,runtimeKey,preflight.json);
                        preflightJson=preflight.json;
                    }
                    const auto staging=cache.BeginRenderStaging(renderKey);if(!staging){completion->result.detail=cacheFailureText.Describe(cache);goto finish;}
                    {std::ofstream settingsFile(*staging/L"neural-settings.ini",std::ios::binary|std::ios::trunc);settingsFile.write(settingsSnapshot->data(),static_cast<std::streamsize>(settingsSnapshot->size()));if(!settingsFile){cache.MarkInvalid(*staging);completion->result.detail=L"The neural settings snapshot could not be staged.";goto finish;}}
                    NeuralRenderRequest request{nullptr,sourcePath,liveIndex?liveDirectory/L"neural.mkv":*staging/L"neural.mkv",width,height,fps,duration};request.jobId=generation;request.range=range;request.prerollFrames=PrerollFramesFor(range,fps);request.guides=guides;request.pauseEvent=pauseEvent;request.segmentFrames=liveIndex?segmentFrames:0u;request.firstSegmentFrames=liveIndex?firstSegmentFrames:0u;request.gpuColorConversion=gpuColorConversion;request.nvencPreset=nvencPreset;request.gpuSourceConversion=gpuSourceConversion;
                    NeuralRenderReceiptInputs receipt{preflightJson,lockChecks,request,{},renderKey,*settingsDigest,*runtimeDigest,std::chrono::system_clock::now(),{}};
                    NeuralSegmentSink sink{};
                    if(liveIndex){
                        // Every finalized segment is playable on arrival; the
                        // player reads them behind the render head.
                        sink.onSegment=[&](const NeuralRenderSegment& segment){
                            NeuralSegment entry{};entry.path=liveDirectory/segment.fileName;entry.runId=liveRunId;entry.index=segment.index;entry.firstFrameNumber=segment.firstFrameNumber;entry.firstTimestamp100ns=segment.firstTimestamp100ns;entry.end100ns=segment.end100ns;entry.frameCount=segment.frameCount;
                            LOG("Neural segment run="<<liveRunId<<" index="<<entry.index<<" frames="<<segment.frameCount<<" firstFrame="<<segment.firstFrameNumber<<" span=["<<double(segment.firstTimestamp100ns)*1e-7<<","<<double(segment.end100ns)*1e-7<<") file="<<WideToUtf8(segment.fileName));
                            liveIndex->Append(std::move(entry));
                            // The first file the player can show: everything
                            // after it is the player's own attach cost.
                            coldStart->Ready();
                        };
                        // A relaunched worker republishes from its own index 0,
                        // so only this run's segments are discarded; coverage any
                        // other run left behind stays valid wherever it sits.
                        sink.onRestart=[&]{LOG("Neural render restarted from zero; discarding run "<<liveRunId<<"'s published segments.");liveIndex->DropRun(liveRunId);};
                    }
                    NeuralJobHooks hooks{};
                    hooks.progress=postProgress;
                    hooks.segments=sink;
                    // The helper holds this job from here. For a launched one
                    // that is its created process; for a reused one it is the
                    // command frame, which is also the whole of what a warm
                    // toggle now pays before the helper's own clock starts. The
                    // plan is noted here rather than after the job, because an
                    // active session reports its cold start when the first frame
                    // reaches the screen - seconds before this call returns.
                    hooks.accepted=[&](resident_helper::HelperPlan plan){
                        coldStart->Mark(NeuralColdStartPhase::Launch);
                        coldStart->NoteHelper(std::string(resident_helper::HelperPlanName(plan)));
                    };
                    // Merged the moment the helper reports it, which is when
                    // its first output file exists. Merging only after the
                    // call returned put the helper's five phases seconds
                    // behind the attach: an active session presents its
                    // first frame while the render runs on, and the log line
                    // written there carried five dashes in ten of ten
                    // sessions while receipt.json for the same render
                    // carried all five numbers. A reused helper reports only
                    // firstOutput, and the two phases it did not pay stay
                    // absent rather than becoming zeroes.
                    hooks.helperTimeline=[&](const NeuralColdStartTimeline& helper){coldStart->Merge(helper);};
                    // The residency key: the runtime this job locked, the files
                    // it verified, and the settings INI written above. ReShade
                    // and RenoDX read that INI at process start, so a change to
                    // it is a different helper and not a different job.
                    const auto helperKey=resident_helper::MakeHelperKey(runtimeDirectory.wstring(),*runtimeDigest,*settingsDigest);
                    resident_helper::HelperPlan helperPlan=resident_helper::HelperPlan::Launch;
                    completion->result=residentHelper->RunJob(workerExecutable,helperKey,request,hooks,stop,&helperPlan);
                    LOG("Neural helper plan="<<resident_helper::HelperPlanName(helperPlan)<<" resident="<<residentHelper->Resident()<<".");
                    // Nothing is marked finished here any more: one job fills one
                    // hole, and whether the session has more to do is a question
                    // about coverage, answered on the UI thread.
                    // Again for a timeline that arrived too late to be reported
                    // over the pipe - a crash or a cancel the launcher
                    // synthesized a result for. Merging twice is idempotent.
                    coldStart->Merge(completion->result.coldStart);
                    completion->result.coldStart=coldStart->Snapshot();
                    receipt.result=completion->result;receipt.finished=std::chrono::system_clock::now();
                    LOG("Neural render receipt: "<<SummarizeNeuralReceiptForLog(receipt));
                    if(!completion->result.ok){cache.MarkInvalid(*staging);goto finish;}
                    // The entry is keyed, labelled and proven by THIS run: its range, its
                    // frame count, its evidence counters. So it must contain exactly the
                    // segments this run published. A resumed session hands earlier coverage
                    // to the next job for playback to keep reading, and joining that in too
                    // produced a file longer than the label - one session joined 46 files of
                    // 2622 frames and 87.4 s against a result of 1647 frames and 54.9 s, and
                    // the gate correctly refused the render it had just finished.
                    //
                    // Selected by run id rather than by position: segments are held sorted
                    // by timestamp, so a run that filled a hole behind an earlier region is
                    // not the tail of the index, and a positional slice would take the wrong
                    // files. The scan stays in timeline order, which is what a join needs.
                    // The next hole cannot start until this returns, so the phases
                    // are timed: one driven session sat 18 s between a finished
                    // render and the next one with the GPU idle and a hole left.
                    const auto publishStart=std::chrono::steady_clock::now();
                    const auto msSince=[](std::chrono::steady_clock::time_point from){
                        return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-from).count();};
                    std::vector<std::filesystem::path> parts;
                    if(liveIndex)
                        for(size_t position=0;position<liveIndex->Count();++position)
                            if(const auto segment=liveIndex->At(position);segment&&segment->runId==liveRunId)
                                parts.push_back(segment->path);
                    const size_t joinedParts=liveIndex?parts.size():size_t{1};
                    if(liveIndex&&(parts.empty()||ConcatenateMedia(moduleDirectory,parts,*staging/L"neural.mkv",stop)!=EncodeError::None)){
                        cache.MarkInvalid(*staging);completion->result.ok=false;
                        completion->result.detail=L"The rendered segments could not be joined into a cache entry.";goto finish;
                    }
                    const double concatMs=msSince(publishStart);
                    const auto finalSettings=ReadNeuralAddonSettingsSnapshot(runtimeDirectory/L"ReShade.ini");if(!finalSettings||*finalSettings!=*settingsSnapshot){cache.MarkInvalid(*staging);completion->result.ok=false;completion->result.detail=L"Neural settings changed during rendering. Try the render again.";goto finish;}
                    const std::string receiptJson=BuildNeuralRenderReceiptJson(receipt);const auto receiptDigest=Sha256Bytes(receiptJson);
                    {std::ofstream receiptFile(*staging/L"receipt.json",std::ios::binary|std::ios::trunc);receiptFile.write(receiptJson.data(),static_cast<std::streamsize>(receiptJson.size()));if(!receiptFile||!receiptDigest){cache.MarkInvalid(*staging);completion->result.ok=false;completion->result.detail=L"The neural render receipt could not be staged.";goto finish;}}
                    const auto probeStart=std::chrono::steady_clock::now();
                    const ProbeResult probe=ProbeMedia(moduleDirectory,*staging/L"neural.mkv",stop);
                    const double probeMs=msSince(probeStart);
                    if(stop.stop_requested()){cache.MarkInvalid(*staging);completion->result.cancelled=true;completion->result.ok=false;completion->result.detail=L"Neural render was cancelled.";goto finish;}
                    NeuralCacheManifest manifest{};manifest.sourceDigest=*sourceDigest;manifest.runtimeDigest=*runtimeDigest;manifest.encoder=completion->result.encoder==EncoderKind::HevcNvenc?"hevc_nvenc":"h264_software";manifest.width=width;manifest.height=height;manifest.frameCount=completion->result.frameCount;manifest.duration100ns=completion->result.duration100ns;manifest.nativeEvaluations=completion->result.nativeEvaluations;manifest.verifiedNeuralFrames=completion->result.verifiedNeuralFrames;manifest.observedFeature18Evaluations=completion->result.evidence.highestObservedEvaluation;manifest.feature18Created=completion->result.evidence.feature18Created;manifest.feature18ArmedBeforeCapture=completion->result.feature18ArmedBeforeCapture;manifest.upscaling=false;
                    manifest.settingsDigest=*settingsDigest;manifest.rangeStart100ns=range.start100ns;manifest.rangeEnd100ns=range.end100ns;manifest.guides=identity.guides;manifest.jobId=generation;manifest.historyResets=completion->result.historyResets;manifest.receiptDigest=*receiptDigest;
                    // The key's environment terms, so eviction can tell this entry from one a driver
                    // update, a model refresh or an upgrade of this installation has orphaned.
                    manifest.environment=NeuralCacheEnvironmentFor(identity);
                    const int64_t joinedDurationTolerance=JoinedMediaDurationTolerance100ns(fps,joinedParts);
                    const bool probeMatches=probe.ok&&probe.width==width&&probe.height==height&&probe.frameCount==completion->result.frameCount&&NeuralPublishDurationsMatch(probe.duration100ns,completion->result.duration100ns,expectedDuration100ns,joinedDurationTolerance);
                    NeuralCacheManifest publishCandidate=manifest;publishCandidate.kind=NeuralCacheEntryKind::Render;publishCandidate.state=NeuralCacheState::Complete;publishCandidate.neuralDigest=std::string(64,'0');
                    const bool manifestReusable=IsReusableNeuralCacheManifest(publishCandidate);
                    const bool gate=CanPublishNeuralCompletion(completion->result.ok,probeMatches,manifestReusable);
                    NeuralCachePromotion promotion{};
                    const auto promoteStart=std::chrono::steady_clock::now();
                    const bool published=gate&&cache.PromoteRender(renderKey,*staging,manifest,&promotion);
                    LOG("Neural publish timing: parts="<<joinedParts<<" concatMs="<<concatMs<<" probeMs="<<probeMs
                        <<" promoteMs="<<msSince(promoteStart)<<" totalMs="<<msSince(publishStart)<<".");
                    if(!published){
                        // This gate discarded a finished render once and left nothing to diagnose it
                        // with; then it did it again for a rename an antivirus scan was holding, and
                        // the numbers below all agreed. Both halves of the verdict are named now.
                        LOG("Neural publish refused: gate="<<gate<<" promoteStage="<<NeuralCachePromotionStageName(promotion.stage)
                            <<" promoteError="<<promotion.win32Error<<" renameAttempts="<<promotion.attempts
                            <<" renderOk="<<completion->result.ok<<" manifestReusable="<<manifestReusable<<" probeOk="<<probe.ok<<" probe="<<probe.width<<"x"<<probe.height<<" expected="<<width<<"x"<<height<<" probeFrames="<<probe.frameCount<<" resultFrames="<<completion->result.frameCount<<" probeDuration="<<probe.duration100ns<<" resultDuration="<<completion->result.duration100ns<<" expectedDuration="<<expectedDuration100ns<<" tolerance="<<joinedDurationTolerance<<" parts="<<joinedParts<<".");
                        if(!cache.MarkInvalid(*staging))LOG("The refused staging directory could not be set aside either: "<<WideToUtf8(staging->wstring()));
                        completion->result.ok=false;completion->result.detail=L"The neural video failed final cache validation.";goto finish;
                    }
                    if(promotion.attempts>1)LOG("Neural cache entry published after "<<promotion.attempts<<" rename attempts; the entry was held by another process.");
                    if(promotion.entry){completion->neuralPath=promotion.entry->payloadPath;completion->receiptPath=promotion.entry->directory/L"receipt.json";}else{completion->result.ok=false;completion->result.detail=L"The neural cache entry could not be reopened.";}
                    // The entry IS this run's segments, joined: same frames, same
                    // timestamps. Serving the run from it retires the segments, so
                    // a session stops holding every rendered byte twice until it is
                    // released - and a retained one across toggles. The UI thread
                    // deletes them once no decoder has them open.
                    if(liveIndex&&promotion.entry){
                        std::error_code sizeError;const auto entryBytes=std::filesystem::file_size(promotion.entry->payloadPath,sizeError);
                        const bool replaced=liveIndex->ReplaceRun(liveRunId,promotion.entry->payloadPath);
                        LOG("Live run "<<liveRunId<<(replaced?" now plays from its published entry; ":" keeps playing its segments; the entry could not replace ")
                            <<joinedParts<<" segment files"<<(replaced?" retired":"")<<". entryBytes="<<(sizeError?uintmax_t{0}:entryBytes)<<".");
                    }
                }
            finish:
                // A cache hit, a refusal or a prepared open never reached a
                // helper, so this is where the player's own work ends.
                coldStart->MarkIfAbsent(NeuralColdStartPhase::Request);
                completions->RegisterAndPost(std::move(completion),[&](uint64_t token){return PostMessageW(target,WM_NEURAL_COMPLETE,static_cast<WPARAM>(token),0)!=FALSE;});
            });
        }catch(const std::system_error&){m_neuralLifecycle.Invalidate();SyncSourceActionAvailability();const std::wstring message=L"The neural pre-render worker could not start.";MessageBoxW(m_hwnd,message.c_str(),T(L"app.title").c_str(),MB_OK|MB_ICONERROR);return false;}
        return true;
    }
    bool LoadCachedPlayback(const NeuralJobCompletion& completion){
        Unload();
        // m_decoder only DESCRIBES the original here (geometry, rate, layout,
        // still or not); the pair decodes it. A full Open kept an ffmpeg child
        // and an NVDEC session running that nothing read, and the pair then
        // probed the same file again. A still or a GIF needs options a known
        // open does not carry, and an answer without an ffprobe profile came
        // from Media Foundation: those two still let the pair probe.
        if(!m_decoder.OpenMetadata(completion.sourcePath.wstring(),MediaSourceKind::LocalFile,{},/*preferNv12=*/true)){Unload();return false;}
        const VideoDecoder::KnownMedia originalMedia=(m_decoder.IsStillImage()||m_decoder.IsAnimation()||m_decoder.Media().hardwareProfile.empty())?VideoDecoder::KnownMedia{}:m_decoder.Media();
        if(!m_synchronizedPlayback.Open(completion.sourcePath,completion.neuralPath,{},SynchronizedRange{completion.range.start100ns,completion.range.end100ns},PairPrefersNv12(),originalMedia)){Unload();return false;}
        m_dar=m_decoder.DisplayAspectRatio();if(!std::isfinite(m_dar)||m_dar<0.2)m_dar=double(m_decoder.Width())/std::max(1u,m_decoder.Height());
        const auto [guideW,guideH]=TemporalGuideGenerator::AnalysisGrid(m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate());
        ShowWindow(m_viewport,SW_SHOW);Layout();m_renderer=MakeD3D12Renderer();
        ConfigureRendererSource();
        if(!m_renderer||!m_renderer->Initialize(m_renderWnd,m_decoder.Width(),m_decoder.Height(),m_decoder.Width(),m_decoder.Height(),guideW,guideH,DefaultNeuralCarrierQuality())||!RendererTookSourceLayout()){Unload();return false;}
        m_renderer->SetDLSS(false);m_renderer->SetColorSettings(m_colorSettings);m_renderer->SetComparison(EffectiveComparison());m_activeQuality=DefaultNeuralCarrierQuality();
        const ComparisonView desiredView=m_neuralRequested?ComparisonView::Neural:ComparisonView::Original;
        // Decoder start-up (and any hardware-decode fallback) is asynchronous:
        // wait for the first pair within the same bound the seek path uses.
        SynchronizedReadResult firstRead=SynchronizedReadResult::NotReady;
        for(const auto deadline=Clock::now()+std::chrono::seconds(10);firstRead==SynchronizedReadResult::NotReady&&Clock::now()<deadline;){firstRead=m_synchronizedPlayback.ReadNextAvailable();if(firstRead==SynchronizedReadResult::NotReady)std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        if(firstRead!=SynchronizedReadResult::PairReady||!m_synchronizedPlayback.SetView(desiredView)||!m_synchronizedPlayback.VisibleFrame()){LOG("Cached playback could not produce its first synchronized pair (result="<<static_cast<int>(firstRead)<<").");Unload();return false;}
        m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;
        const VideoFrame first=*m_synchronizedPlayback.VisibleFrame();if(!RenderVideoFrame(first,true)){Unload();return false;}
        NoteNeuralFramePresented();
        m_neuralPath=completion.neuralPath;m_cachedRange=completion.range;m_cachedReceiptPath=completion.receiptPath;m_cachedSettings=completion.settings;m_cachedGuides=completion.guides;m_currentSec=double(first.timestamp100ns)*1e-7;m_haveNext=false;m_cachedPlayback=true;m_comparisonView=desiredView;m_cachedPresentedFrames=1;RememberRenderedCachedPair();
        if(!m_decoder.IsStillImage())m_audio.Start(completion.sourcePath.wstring(),m_currentSec);m_audio.SetVolume(m_muted?0.0f:m_volume);m_playing=!m_decoder.IsStillImage();m_synchronizedPlayback.SetPaused(m_decoder.IsStillImage());m_playStartSec=m_currentSec;m_playStart=Clock::now();
        m_loaded=true;m_path=completion.sourcePath.wstring();m_sourceKind=completion.sourceKind;m_youtubePageUrl=completion.pageUrl;m_youtubeSourceQuality=completion.sourceQuality;m_displayTitle=DisplayTitleForSource(completion.sourceKind,completion.displayTitle);if(m_displayTitle.empty())m_displayTitle=completion.sourcePath.stem().wstring();
        m_droppedFrames=0;m_presentStride=1;m_presentPhase=0;m_cadenceReanchors=0;m_seekPending=false;m_seeking=false;m_fpsWindowStart=Clock::now();m_fpsWindowFrames=0;m_submitFps=0.0;m_guideReset=false;m_dlssReset=false;
        RestoreUpscaling();UpdateTitle();NoteLoadedSourceQuality();UpdateCachedStatus();Layout();SyncFeatureMenuState();InvalidateRect(m_hwnd,nullptr,TRUE);return true;
    }
    void CompleteNeuralProgress(uint64_t token){
        auto message=m_neuralProgressMessages.Take(token);if(!message||!m_neuralLifecycle.Accept(message->generation))return;
        // A pause report that arrives after the user already resumed is stale.
        if(message->progress.phase==NeuralRenderPhase::Paused&&!NeuralJobPaused())message->progress.phase=NeuralRenderPhase::NeuralRendering;
        m_neuralProgress=message->progress;m_neuralSourceWidth=message->width;m_neuralSourceHeight=message->height;
        // The local copy this job renders from, its cache key, and the video it
        // belongs to - all known long before the job ends.
        if(!message->sourcePath.empty()&&message->pageUrl==m_youtubePageUrl){
            m_jobSourcePath=message->sourcePath;m_jobSourceKey=message->sourceKey;m_jobSourcePageUrl=message->pageUrl;
        }
        const NeuralPlaybackState next=StateForProgressPhase(message->progress.phase,m_neuralLifecycle.state);
        // The worker may still report a frame that was in flight when the user
        // paused; the pause event, not that report, decides when rendering resumes.
        const bool holdPause=m_neuralLifecycle.state==NeuralPlaybackState::Paused&&next==NeuralPlaybackState::Rendering&&NeuralJobPaused();
        if(!holdPause)m_neuralLifecycle.Transition(next);
        InvalidateRect(m_hwnd,nullptr,FALSE);
    }
    // Silence budget per phase, in seconds; 0 means the phase is not watched.
    // Only phases that must keep reporting are: acquisition now reports
    // downloaded bytes at least every 250 ms, decoding and rendering report per
    // frame, while encoding and validation legitimately go quiet while ffmpeg
    // flushes and the output is hashed. The sibling project that drives the same
    // runtime polices its helper with 30 s to create a feature, 60 s to evaluate
    // one and a 5 s per-frame deadline, and the GPU fence waits inside this job
    // already give up after 20 s. A phase quiet for this long is not slow, it is
    // gone, and a progress bar that never moves is the worst way to say so.
    static double NeuralPhaseSilenceBudgetSeconds(NeuralRenderPhase phase){
        switch(phase){
        case NeuralRenderPhase::Acquiring:return 60.0;
        case NeuralRenderPhase::CheckingCache:
        case NeuralRenderPhase::Preflight:
        case NeuralRenderPhase::Decoding:
        case NeuralRenderPhase::NeuralRendering:return 120.0;
        default:return 0.0;
        }
    }
    void NoteNeuralJobProgress(){
        m_watchedPhase=m_neuralProgress.phase;m_watchedFrames=m_neuralProgress.completedFrames;
        m_watchedBytes=m_neuralProgress.bytes;m_neuralProgressAt=Clock::now();
    }
    void WatchNeuralJobProgress(){
        if(!NeuralJobActive()||NeuralJobPaused()){m_neuralProgressAt={};return;}
        if(m_neuralProgressAt==Clock::time_point{}){NoteNeuralJobProgress();return;}
        if(m_neuralProgress.phase!=m_watchedPhase||m_neuralProgress.completedFrames!=m_watchedFrames||
           m_neuralProgress.bytes!=m_watchedBytes){NoteNeuralJobProgress();return;}
        const double budget=NeuralPhaseSilenceBudgetSeconds(m_neuralProgress.phase);
        if(!(budget>0.0))return;
        const double silent=std::chrono::duration<double>(Clock::now()-m_neuralProgressAt).count();
        if(silent<budget)return;
        LOG("Neural job reported no progress for "<<silent<<" s in phase "<<WideToUtf8(NeuralPhaseText(m_neuralProgress))
            <<"; stopping it instead of leaving the loader running.");
        m_neuralProgressAt={};
        if(m_liveSession){
            // The session ended for a reason the user did not cause; a bare
            // hand-back read as the toggle having been dropped.
            StopLiveNeuralSession(true);
            m_neuralNotice=T(NeuralFailureTextKey(NeuralRenderFailure::Protocol));
            UpdateCachedStatus();InvalidateControls();
        }else{
            CancelNeuralJob(false);
            m_neuralProgress={};m_neuralCancelBounds={};
            TransitionToFailure(NeuralRenderFailure::Protocol);
            SyncSourceActionAvailability();InvalidateRect(m_hwnd,nullptr,FALSE);
        }
    }
    // The worker a live retarget stopped without waiting for. It must be gone
    // before another job starts - RangeRenderAvailable says so - or before the
    // session's directory is removed.
    bool NeuralWorkerRetiring()const{return m_retiringNeuralWorker.joinable();}
    void JoinRetiringNeuralWorker(){
        if(!m_retiringNeuralWorker.joinable())return;
        m_retiringNeuralWorker.join();m_retiringNeuralWorker=std::jthread{};m_retiringNeuralGeneration=0;
    }
    // The completion message is not a guarantee: a worker that posted it before the
    // cancel had its message drained by that cancel. A thread that has already
    // exited is joined here without waiting, so the next render is never held back
    // by a retirement nobody will announce.
    void ReapRetiringNeuralWorker(){
        if(!m_retiringNeuralWorker.joinable())return;
        if(WaitForSingleObject(static_cast<HANDLE>(m_retiringNeuralWorker.native_handle()),0)!=WAIT_OBJECT_0)return;
        JoinRetiringNeuralWorker();
        LOG("Retired neural worker reaped after it exited.");
    }
    void CompleteNeuralJob(uint64_t token){
        auto completion=m_neuralCompletions.Take(token);
        // The retired worker's last act is posting this, so the join is immediate.
        if(completion&&m_retiringNeuralWorker.joinable()&&completion->generation==m_retiringNeuralGeneration){
            JoinRetiringNeuralWorker();
            LOG("Retired neural job "<<completion->generation<<" finished.");
            // The next render was held back for it (RangeRenderAvailable).
            SyncSourceActionAvailability();InvalidateControls();
        }
        if(!completion||!m_neuralLifecycle.Accept(completion->generation))return;
        // A render that publishes no segments has its first playable output
        // here, and this is the last moment a picture could appear for it.
        if(m_coldStart)m_coldStart->Ready();
        HandleNeuralCompletion(completion);
        ReportNeuralColdStart();
    }
    void HandleNeuralCompletion(const std::unique_ptr<NeuralJobCompletion>& completion){
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
        if(completion->result.ok&&!completion->neuralPath.empty()&&LoadCachedPlayback(*completion)){m_neuralLifecycle.Transition(NeuralPlaybackState::Ready);RecordRecent(*completion);m_neuralNotice.clear();SyncSourceActionAvailability();if(completion->cacheHit)LOG("Verified neural cache hit opened without re-rendering.");else LOG("Verified neural cache playback opened.");return;}
        TransitionToFailure(completion->result.failure);const bool noticeShown=NoteNeuralFailure(*completion);LOG("Neural pre-render failed: kind="<<NeuralRenderFailureName(completion->result.failure)<<" state="<<WideToUtf8(NeuralPlaybackStateName(m_neuralLifecycle.state))<<" detail="<<WideToUtf8(completion->result.detail));
        SyncSourceActionAvailability();
        if(!completion->sourcePath.empty()&&std::filesystem::is_regular_file(completion->sourcePath)){m_neuralLifecycle.Transition(NeuralPlaybackState::OriginalOnly);LoadOriginalFallback(*completion);}
        else if(!noticeShown){std::wstring detail=completion->result.detail.empty()?L"Neural pre-render failed before playback could start.":completion->result.detail;if(const wchar_t* kind=NeuralFailureTextKey(completion->result.failure))detail=T(kind)+L"\n\n"+detail;MessageBoxW(m_hwnd,detail.c_str(),T(L"app.title").c_str(),MB_OK|MB_ICONERROR);InvalidateRect(m_hwnd,nullptr,FALSE);}
        else InvalidateRect(m_hwnd,nullptr,FALSE);
    }
    // PrepareYouTubeMedia starts the network audio on the resolution worker,
    // and WASAPI is COM: CoCreateInstance on a thread that never joined an
    // apartment works only while some other thread happens to hold the
    // process's MTA open. Joining it explicitly stops that depending on luck.
    // Declared first in each worker, so it outlives the completion - and the
    // audio inside it - when that is destroyed on the worker undelivered.
    struct WorkerComApartment{HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);WorkerComApartment()=default;WorkerComApartment(const WorkerComApartment&)=delete;WorkerComApartment& operator=(const WorkerComApartment&)=delete;~WorkerComApartment(){if(SUCCEEDED(result))CoUninitialize();}};
    static void PrepareYouTubeMedia(YouTubeCompletion& completion,std::stop_token stop,[[maybe_unused]] uint32_t maxW,[[maybe_unused]] uint32_t maxH,[[maybe_unused]] bool qualityExplicit,[[maybe_unused]] NVSDK_NGX_PerfQuality_Value explicitQuality){
        if(!completion.result.ok||stop.stop_requested())return;
        completion.decoder=std::make_unique<VideoDecoder>();
        // preferNv12 for the same reason LoadOriginal asks for it: this decoder
        // becomes m_decoder, and PairPrefersNv12() reads its layout back for both
        // members of a neural pair. Omitting it took the `false` default, so a
        // network source shipped 14.7 MB BGRA frames down both pipes - 29.5 MB
        // per pair against a 16.7 ms budget at 59.94 fps - and no YouTube source
        // could ever hold live neural playback. The request is still only a
        // request: OpenFFmpeg refuses any colour description the GPU pass does
        // not implement, so a non-BT.709-limited stream decodes to BGRA exactly
        // as before.
        if(!completion.decoder->Open(completion.result.mediaUrl,MediaSourceKind::YouTube,stop,/*preferNv12=*/true)){completion.mediaErrorKey=stop.stop_requested()?L"youtube.error.cancelled":L"youtube.error.ffmpeg";return;}
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
                const WorkerComApartment com;
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
                const WorkerComApartment com;
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
        // From completion.decoder, NOT m_decoder: the swap into m_decoder happens
        // in InstallPreparedYouTube, after this candidate has been built and
        // validated, so m_decoder here still describes the media being replaced.
        // ConfigureRendererSource()/RendererTookSourceLayout() read m_decoder and
        // are therefore the wrong helpers for this one path.
        //
        // Omitting this left the candidate on its Bgra default while the decoder
        // delivered NV12, and ValidatePreparedFrame rejected every frame - the
        // transaction rolled back and the open reported that no video frame could
        // be decoded. Setting it before Initialize is what fixes the layout; the
        // check after is because SetSourceLayout is a request (odd geometry falls
        // back), and a silent disagreement uploads a quarter of a BGRA image as a
        // Y plane rather than failing.
        candidate->renderer->SetSourceLayout(completion.decoder->PixelLayout());
        candidate->renderer->SetSourceColor(completion.decoder->ColorDescription());
        candidate->renderer->SetPresentFollowsWindow(true);
        if(!candidate->renderer->Initialize(candidate->window,completion.configuration.decodeWidth,completion.configuration.decodeHeight,completion.configuration.outputWidth,completion.configuration.outputHeight,completion.configuration.guideWidth,completion.configuration.guideHeight,quality))return{};
        if(candidate->renderer->ActiveSourceLayout()!=completion.decoder->PixelLayout()){
            LOG("Prepared renderer refused the decoder's "
                <<(completion.decoder->PixelLayout()==VideoPixelLayout::Nv12?"NV12":"BGRA")
                <<" source layout; this open fails rather than presenting garbage.");
            return{};
        }
        candidate->renderer->SetDLSS(false);candidate->renderer->SetColorSettings(m_colorSettings);candidate->renderer->SetComparison(EffectiveComparison());
        if(m_renderer)candidate->renderer->SetDebugView(m_renderer->GetDebugView());
        candidate->configuration.inputWidth=candidate->renderer->DLSSInputW();candidate->configuration.inputHeight=candidate->renderer->DLSSInputH();
        return candidate;
    }
    bool ValidatePreparedFrame(const YouTubeCompletion& completion,D3D12Renderer& renderer,TemporalGuideGenerator& guides){
        // The frame's own layout on both, for the same reason RenderVideoFrame
        // passes f.layout: the byte count the geometry check expects and the
        // planes the guide generator reads are both layout-dependent, and both
        // defaulted to BGRA here while the decoder could only ever produce BGRA.
        if(!NetworkPreparedGeometryIsValid(completion.configuration,completion.decoder->Width(),completion.decoder->Height(),completion.firstFrame.bgra.size(),completion.firstFrame.layout))return false;
        const FrameIdentity identity=IdentityOf(completion.firstFrame,0,0,HistoryReset::FirstFrame);
        GuideFrame guide;if(!guides.Generate(completion.firstFrame.bgra.data(),completion.firstFrame.bgra.size(),completion.configuration.decodeWidth,completion.configuration.decodeHeight,renderer.DLSSInputW(),renderer.DLSSInputH(),completion.decoder->FrameRate(),identity,guide,completion.firstFrame.layout))return false;
        const float frameMs=float(1000.0/std::max(1.0,completion.decoder->FrameRate()));
        return renderer.RenderFrame(completion.firstFrame.bgra.data(),completion.firstFrame.bgra.size(),identity,guide,frameMs);
    }
    bool InstallPreparedYouTube(YouTubeCompletion& completion,std::unique_ptr<PreparedRendererCandidate> candidate){
        // A new source cannot inherit the previous one's live session or cached pair.
        // Tick() reads frames from m_synchronizedPlayback for as long as m_cachedPlayback
        // is set, so leaving them attached starves the decoder swapped in below - the
        // picture stops on the first frame while the swapped audio keeps running - and the
        // timeline keeps painting the old session's rendered span. Unload() performs this
        // same teardown; every other source-swap path reaches it and this one did not.
        // Done before anything is swapped, so the release sees a consistent old state:
        // ExecuteNetworkCandidateTransaction only calls this after the candidate renderer
        // has been created and validated, so the rollback that promises to preserve the
        // active state can no longer fire.
        //
        // Retained segments are deliberately NOT dropped here. This path is also how a
        // seek or a quality reload re-commits the SAME video, and dropping them there
        // deletes rendered coverage the next toggle would have resumed on - the whole
        // point of ReleaseLiveSession(true). LiveRetentionKey() already guards adoption
        // by source, settings and guides, so a genuinely different video cannot adopt
        // them and StartLiveNeuralSession drops them itself.
        // A seek or a quality reload re-commits the SAME video, so the session
        // that was running belongs to it. Releasing it retains the coverage;
        // restarting it below resumes on those regions. Without that restart the
        // user's own backward seek read as "neural stopped and became
        // unavailable", which is the report this session set out to fix.
        const bool resumeLive=m_liveSession&&completion.commitKind!=NetworkCommitKind::InitialOpen;
        // A different video keeps none of the last one's state: this path does not
        // go through Unload, and a copy of the previous video would otherwise
        // still be on offer to playback - same geometry, wrong film.
        if(completion.commitKind==NetworkCommitKind::InitialOpen){m_jobSourcePath.clear();m_jobSourceKey.clear();m_jobSourcePageUrl.clear();}
        if(m_liveSession){CancelNeuralJob(false);ReleaseLiveSession(true);}
        m_synchronizedPlayback.Close();
        m_cachedPlayback=false;m_cachedSourceFile=false;m_cachedPresentedFrames=0;m_havePresentedPair=false;
        ForgetRenderedCachedPair();m_cachedRange={};m_cachedReceiptPath.clear();
        m_cachedSettings={};m_cachedGuides={};m_neuralPath.clear();m_markers={};
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
                m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=completion.firstFrame.timestamp100ns;RememberPlaybackFrame(completion.firstFrame);m_lastGlobalX=0;m_lastGlobalY=0;
                m_currentSec=double(completion.firstFrame.timestamp100ns)*1e-7;m_haveNext=false;m_waitingForNetworkFrame=true;m_networkReadState.Reset();m_playing=shouldPlay;m_playStartSec=m_currentSec;m_playStart=Clock::now();m_loaded=true;m_path=source;m_youtubeAudioUrl=audioSource;m_youtubePageUrl=pageUrl;m_youtubeSourceQuality=completion.sourceQuality;m_sourceKind=MediaSourceKind::YouTube;m_displayTitle=DisplayTitleForSource(MediaSourceKind::YouTube,title);m_droppedFrames=completion.commitKind==NetworkCommitKind::InitialOpen?0:m_droppedFrames;m_seekPending=false;m_seeking=false;m_fpsWindowStart=Clock::now();m_fpsWindowFrames=0;m_submitFps=0.0;
                Audio().SetVolume(m_muted?0.0f:m_volume);Audio().Pause(!shouldPlay);
            });
        if(!committed)return false;
        RecordOriginalRecent();
        RestoreUpscaling();
        UpdateYouTubeQualitySelection(GetMenu(m_hwnd),m_youtubeSourceQuality);DrawMenuBar(m_hwnd);
        UpdateTitle();UpdateCachedStatus();Layout();InvalidateRect(m_hwnd,nullptr,TRUE);
        // After the swap, not before: the session opens decoders against the
        // stream that is now loaded.
        if(resumeLive)StartLiveNeuralSession();
        return true;
    }
    // The resolver reports what it actually selected. A 360p fallback on an
    // age-restricted video used to reach the screen as a bad picture with no
    // explanation, and the neural render then ran at that resolution too.
    static constexpr int kUsableSourceHeight=720;
    void NoteResolvedSourceQuality(const ResolveResult& result){
        // A seek or a quality reload commits without resolving again, so its
        // completion carries no format metadata. Fall back to geometry instead of
        // reporting a 0p resolve that never happened.
        if(result.selectedHeight<=0){NoteLoadedSourceQuality();return;}
        m_sourceNotice.clear();
        LOG("YouTube source selected: "<<result.selectedHeight<<"p at "<<result.videoKbps<<" kbps, age limit "<<result.ageLimit<<".");
        if(result.selectedHeight>=kUsableSourceHeight)return;
        wchar_t text[512]{};
        swprintf_s(text,T(result.ageLimit>0?L"youtube.source.low.signin":L"youtube.source.low").c_str(),
                   result.selectedHeight,result.videoKbps/1000.0);
        m_sourceNotice=text;
        LOG("YouTube served a degraded source: "<<WideToUtf8(m_sourceNotice));
    }
    // Every other way a source reaches the screen: a cached copy an earlier
    // session acquired at a degraded height is reused as-is, and the resolver
    // never runs, so geometry is the only signal those paths have. The rate is
    // unknown here, so the text offers the way out instead of a number.
    void NoteLoadedSourceQuality(){
        if(!m_sourceNotice.empty()||m_sourceKind!=MediaSourceKind::YouTube)return;
        const uint32_t height=m_decoder.Height();
        if(!height||height>=uint32_t(kUsableSourceHeight))return;
        wchar_t text[512]{};
        swprintf_s(text,T(L"youtube.source.low_height").c_str(),int(height));
        m_sourceNotice=text;
        LOG("YouTube source is degraded: "<<height<<"p from a path that did not resolve a format.");
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
        // After the install, not before: committing a prepared renderer runs
        // through Unload, which clears the notice.
        if(committed)NoteResolvedSourceQuality(completion->result);
    }
    PlayerRuntimeStatus RuntimeStatus()const{
        return ResolvePlayerRuntimeStatus(m_opt.safeMode,m_opt.neuralAddonConfigured,m_renderer&&m_renderer->DLSSEnabled(),m_renderer&&m_renderer->DLSSFeatureCreated());
    }
    // Lead-in state of an active session, in front of everything else because it
    // is why playback is waiting.
    std::wstring LiveSessionStatusText()const{
        wchar_t lead[64]={};swprintf_s(lead,L"%.1f s",LiveLeadSeconds());
        std::wstring text=(m_liveBuffering?T(L"neural.live.buffering"):T(L"neural.live.title"))+L" \u00b7 "+lead+L" "+T(L"neural.live.lead");
        // How much of the video is rendered is the render chip's to say
        // (RenderChipProgress), as a share of the whole range rather than of
        // where the newest frame is: it was a segment of a line that lost its
        // tail to the ellipsis first.
        // What the next buffer fill will do with the press the user already made.
        if(m_liveBuffering)text+=L" \u00b7 "+T(LiveResumePending()?L"neural.live.will_play":L"neural.live.will_stay_paused");
        // The forecast is a constant for one GPU; this is what the render is
        // actually managing here, including whatever else the machine is doing.
        if(const double ratio=LiveRealtimeRatio();ratio>0.0&&ratio<0.98){
            wchar_t pace[48]={};swprintf_s(pace,L" \u00b7 %.2fx real time",ratio);text+=pace;
        }
        // Turing, Ampere and workstation cards have no measured prior, so the
        // forecast that let this session start was a default, not a check. Say
        // that once the session is running rather than implying the card passed.
        else if(!playback_timing::ForecastLiveRender(m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate(),
                    m_renderPace,RenderPacePrior(m_opt.detectedGpu.generation)).measured)
            text+=L" \u00b7 pace unmeasured on this GPU";
        return text;
    }
    // Video seconds covered per second of wall clock. Measured against the
    // coverage this session added, so adopted regions and the holes between
    // rendered ones cannot inflate it.
    // What the cushions are sized against: what this session is ACTUALLY
    // managing once there is enough of it to measure, and the forecast until
    // then. RealtimeRatio reports nothing for the first eight seconds, which is
    // exactly when the first attach is decided - so without the fallback the
    // sub-real-time arm would never see a ratio at the one moment it matters.
    double LivePaceRatio()const{
        const double measured=LiveRealtimeRatio();
        return measured>0.0?measured:m_liveForecastRatio;
    }
    double LiveResumeLead()const{
        return live_session::ResumeLead(LivePaceRatio(),kLiveResumeLead);
    }
    double LiveRealtimeRatio()const{
        if(!m_liveSession||!m_liveSegments||!m_liveStartTick)return 0.0;
        const int64_t covered=CoveredDuration100ns(LiveCoverage(),CoverageSpan{m_liveRange.start100ns,m_liveRange.end100ns});
        return live_session::RealtimeRatio(double(std::max<int64_t>(0,covered-m_liveCoveredAtStart))*1e-7,
                                           double(GetTickCount64()-m_liveStartTick)/1000.0);
    }
    std::wstring BuildStatusText()const{
        // The menu path the hint names has to be the one the menu has: the
        // cancel item lives under DLSS > Convert & save, not under File.
        if(m_exportWorker.joinable())return L"Exporting processed media \u00b7 DLSS > Convert & export > Cancel export to stop";
        // The conversion is the one activity that owns the whole status line:
        // it is minutes long, it is the reason the picture is not changing, and
        // a percentage is the only thing that distinguishes progress from a
        // hang. A source with no readable duration reports frames instead of a
        // percentage rather than inventing one.
        //
        // Order matters here, because this line is drawn with DT_END_ELLIPSIS:
        // state, then how far along, then how to stop, and only then the
        // counters. The cancel hint used to be last, so it was the first thing
        // a narrow window threw away.
        if(m_frameGenWorker.joinable()){
            std::wstring text=T(m_frameGenCancelling?L"framegen.status.stopping":L"framegen.status.generating");
            if(m_frameGenProgress.sourceFramesTotal>0){
                const double fraction=std::clamp(double(m_frameGenProgress.sourceFramesRead)/
                                                 double(m_frameGenProgress.sourceFramesTotal),0.0,1.0);
                text+=L" \u00b7 "+std::to_wstring(static_cast<int>(std::lround(fraction*100.0)))+L"%";
                // Remaining time from this run's own rate, not a guess: a user
                // cannot tell a ten-minute job from an hour-long one out of a
                // percentage alone, and this is the number they are waiting on.
                const double elapsed=std::chrono::duration<double>(Clock::now()-m_frameGenStarted).count();
                if(fraction>0.02&&elapsed>2.0){
                    const double remaining=elapsed*(1.0-fraction)/fraction;
                    text+=L" \u00b7 "+(remaining<90.0
                        ?std::to_wstring(static_cast<int>(std::lround(remaining)))+L" s left"
                        :std::to_wstring(static_cast<int>(std::lround(remaining/60.0)))+L" min left");
                }
            }else{
                text+=L" \u00b7 "+std::to_wstring(m_frameGenProgress.sourceFramesRead)+L" frames read";
            }
            if(!m_frameGenCancelling)text+=L" \u00b7 "+T(L"framegen.status.cancel_hint");
            text+=L" \u00b7 "+std::to_wstring(m_frameGenMultiplier)+L"\u00d7 \u2192 "+
                  FormatFrameRate(m_frameGenTargetFps)+L" fps \u00b7 "+
                  std::to_wstring(m_frameGenProgress.framesWritten)+L" written";
            return text;
        }
        PlayerStatusSnapshot status{};if(m_youtubeLifecycle.IsResolving()){status.activity=PlayerStatusActivity::ResolvingYouTube;return BuildPlayerStatusText(status);}if(!m_loaded||!m_renderer)return{};
        // Cached playback fills the SAME snapshot as ordinary playback and adds
        // only what is true of a cache entry: which view is on screen, the
        // range when it is not the whole video, and the settings it was
        // rendered with. Hand-building a second line here is how the two
        // disagreed - pressing D silently swapped one fact set for another,
        // and this branch kept printing the DLSS input size and "Dropped 0"
        // after the shared builder stopped.
        if(m_cachedPlayback){
            const PlayerRuntimeStatus cachedRuntime=RuntimeStatus();
            status.mediaLoaded=true;status.runtimeConfiguration=cachedRuntime.configuration;
            status.dlssState=cachedRuntime.dlssState;
            status.sourceWidth=m_decoder.NativeWidth();status.sourceHeight=m_decoder.NativeHeight();
            status.inputWidth=m_renderer->DLSSInputW();status.inputHeight=m_renderer->DLSSInputH();
            status.outputWidth=m_renderer->OutputW();status.outputHeight=m_renderer->OutputH();
            status.quality=QualityNameW(m_activeQuality);
            status.upscalingStatus=UpscalingStatus();status.frameGenerationStatus=FrameGenerationStatus();
            std::wstring text=(m_liveSession?std::wstring{}:std::wstring(L"Neural video \u00b7 "))+
                T(m_comparisonView==ComparisonView::Neural?L"neural.view.rendered":L"neural.view.original")+
                L" \u00b7 "+BuildPlayerStatusText(status);
            if(!CachedRangeCoversSource())text+=L" \u00b7 Range "+FormatTimecode(m_cachedRange.start100ns,m_decoder.FrameRate(),true)+L"\u2013"+FormatTimecode(m_cachedRange.end100ns,m_decoder.FrameRate(),true);
            if(const std::wstring markers=MarkerStatusText();!markers.empty())text+=L" \u00b7 "+markers;
            text+=L" \u00b7 "+NeuralSettingsSummary(m_cachedSettings,m_cachedGuides);
            if(m_liveSession)text=LiveSessionStatusText()+L" \u00b7 "+text;
            if(m_seeking||m_seekPending)text=T(L"status.seeking")+L" \u00b7 "+text;
            if(const std::wstring dropped=m_dropNote.Visible(m_loaded,m_path);!dropped.empty())text=dropped+L" \u00b7 "+text;
            return text;
        }
        const PlayerRuntimeStatus runtime=RuntimeStatus();status.mediaLoaded=true;status.runtimeConfiguration=runtime.configuration;status.dlssState=runtime.dlssState;status.sourceWidth=m_decoder.NativeWidth();status.sourceHeight=m_decoder.NativeHeight();status.inputWidth=m_renderer->DLSSInputW();status.inputHeight=m_renderer->DLSSInputH();status.outputWidth=m_renderer->OutputW();status.outputHeight=m_renderer->OutputH();status.quality=QualityNameW(m_activeQuality);
        status.upscalingStatus=UpscalingStatus();status.frameGenerationStatus=FrameGenerationStatus();std::wstring text=BuildPlayerStatusText(status);
        // Lead with what was marked, or with how to mark, because the runtime
        // detail behind it is what a narrow window truncates.
        if(const std::wstring markers=MarkerStatusText();!markers.empty())text=markers+L" \u00b7 "+text;
        else if(!m_liveSession&&RangeRenderAvailable())text=T(L"status.render_hint")+L" \u00b7 "+text;
        if(m_liveSession)text=LiveSessionStatusText()+L" \u00b7 "+text;
        if(SourcePrefetchActive())text=T(L"status.preparing_source")+L" \u00b7 "+text;
        if(m_seeking||m_seekPending)text=T(L"status.seeking")+L" \u00b7 "+text;
        // Ahead of everything else: it is the reason the picture on screen is the
        // original rather than a rendered one.
        if(!m_neuralNotice.empty())text=m_neuralNotice+L" \u00b7 "+text;
        // A source the resolver had to settle for outranks everything except the
        // neural notice: no render can put back what the stream never carried.
        if(!m_sourceNotice.empty())text=m_sourceNotice+L" \u00b7 "+text;
        // Why the picture stopped, for as long as it is stopped (RequestSeek
        // clears it), and which file of a drop is playing, for as long as it is.
        if(!m_decodeNotice.empty())text=m_decodeNotice+L" \u00b7 "+text;
        if(const std::wstring dropped=m_dropNote.Visible(m_loaded,m_path);!dropped.empty())text=dropped+L" \u00b7 "+text;
        // Ahead of even that: nothing this player can do about the picture is
        // worth reading while its cache cannot be written to.
        if(!m_cacheNotice.empty())text=m_cacheNotice+L" \u00b7 "+text;
        return text;
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
        // A conversion writes into this cache, so clearing it mid conversion
        // deletes the staging file and the output from under the running pass.
        if(ActivityBusy()||m_exportWorker.joinable()||m_frameGenWorker.joinable()){MessageBoxW(m_hwnd,L"Finish or cancel rendering, saving and frame generation before clearing the cache.",T(L"menu.clear_neural_cache").c_str(),MB_OK|MB_ICONINFORMATION);return;}
        NeuralCacheManager cache(m_cacheRoot);if(!cache.Valid()){MessageBoxW(m_hwnd,CacheFailureText().DescribeRoot(cache.LastFailure()).c_str(),T(L"menu.clear_neural_cache").c_str(),MB_OK|MB_ICONERROR);return;}
        const uintmax_t bytes=cache.SizeBytes();const std::wstring prompt=L"Close playback and delete "+std::to_wstring(bytes/(1024*1024))+L" MiB of neural cache data? Local original files will be kept.";
        if(MessageBoxW(m_hwnd,prompt.c_str(),T(L"menu.clear_neural_cache").c_str(),MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
        Unload();
        if(!cache.Clear())MessageBoxW(m_hwnd,L"The neural cache could not be fully cleared.",T(L"menu.clear_neural_cache").c_str(),MB_OK|MB_ICONERROR);
        if(m_recent){auto entries=m_recent->Entries();for(auto it=entries.rbegin();it!=entries.rend();++it){it->sourceKey.clear();it->renderKey.clear();m_recent->Remember(*it);}m_recent->Save();}
        SyncSourceActionAvailability();
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
        // While a conversion runs this pill reads "Cancel", so it has to cancel.
        case ToolbarAction::ToggleFrameGeneration:
            if(m_frameGenWorker.joinable())CancelFrameGeneration();else StartFrameGeneration();
            break;
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
        // A source with no length has no position to map a click to - every
        // click would seek to the start - so its bar is greyed and inert;
        // Left and Right still seek, which is what the hover says.
        if(!m_seeking){RECT tr=TimelineRect();if(PtIn(tr,x,y)&&m_decoder.DurationSeconds()>0.0){m_dragSeek=true;m_dragWasPlaying=m_playing;m_lastScrubSeek={};m_seekPreview=SecondsFromX(x);SetCapture(m_hwnd);InvalidateControls();return;}const auto vr=VolumeRect();if(vr&&PtIn(*vr,x,y)){m_dragVolume=true;SetCapture(m_hwnd);SetVolumeFromX(x);return;}}
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
            // A seek in flight is the one refusal that is purely about timing:
            // everything the toggle needs exists, it just cannot act mid-seek.
            // Dropping the press made the key look dead - press D right after
            // scrubbing and nothing happened - so remember it and run it when
            // the seek lands.
            if((m_seeking||m_seekPending)&&(LiveSessionAvailable()||m_liveSession||
                                            (m_cachedPlayback&&m_havePresentedPair&&m_renderer))){
                m_neuralToggleDeferred=true;
                LOG("Neural rendering toggle deferred until the seek lands.");
                UpdateCachedStatus();InvalidateControls();
                return;
            }
            LOG("Neural rendering toggle ignored: loaded="<<m_loaded<<" renderer="<<(m_renderer!=nullptr)<<" seeking="<<(m_seeking||m_seekPending)
                <<" cachedPair="<<(m_cachedPlayback&&m_havePresentedPair)<<" sessionAvailable="<<LiveSessionAvailable()
                <<" prerender="<<NeuralPreRenderEnabled()<<" stillImage="<<m_decoder.IsStillImage());
            return;
        }
        m_neuralToggleDeferred=false;
        if(m_liveSession){StopLiveNeuralSession(true);return;}
        // A photo cannot run a session, so the toggle renders the one frame it
        // has. That is the same job the frame preview submits, and the cache
        // entry it writes is what the toggle then switches between.
        if(!m_cachedPlayback&&m_decoder.IsStillImage()){PreviewCurrentFrame();return;}
        if(!m_cachedPlayback){StartLiveNeuralSession();return;}
        m_neuralRequested=!m_neuralRequested;const ComparisonView next=m_neuralRequested?ComparisonView::Neural:ComparisonView::Original;if(!m_synchronizedPlayback.SetView(next)){m_neuralRequested=!m_neuralRequested;return;}m_comparisonView=next;if(m_renderer)m_renderer->SetComparison(EffectiveComparison());const VideoFrame* presented=next==ComparisonView::Neural?LastNeuralFrame():LastOriginalFrame();m_guides.Reset();m_guideReset=true;m_dlssReset=true;
        // Null only before any pair has been presented. The value members this
        // replaced were never null, so this used to render a default-constructed
        // frame - an empty buffer the renderer rejected. Skip the render it would
        // have failed anyway and keep every state update that followed it.
        if(presented)RenderVideoFrame(*presented,true);m_guideReset=false;m_dlssReset=false;if(m_haveNext){if(auto pair=m_synchronizedPlayback.CurrentPairShared()){const VideoFrame* member=next==ComparisonView::Neural?&pair->neural:&pair->original;m_nextPairFrame=std::shared_ptr<const VideoFrame>(std::move(pair),member);}}UpdateCachedStatus();InvalidateControls();
    }
    void Rehook(){if(!m_renderer)return;const std::wstring message=T(L"rehook.confirm"),title=T(L"rehook.title");const int answer=MessageBoxW(m_hwnd,message.c_str(),title.c_str(),MB_YESNOCANCEL|MB_ICONWARNING|MB_DEFBUTTON2);ExecuteGuardedRehook(answer,[&]{m_renderer->RequestDLSSRecreate();m_dlssReset=true;});}
    void SetYouTubeSourceQuality(YouTubeSourceQuality quality){if(quality==m_youtubeSourceQuality)return;if(m_loaded&&m_sourceKind==MediaSourceKind::YouTube&&!m_youtubePageUrl.empty()){StartYouTubeResolution(m_youtubePageUrl,m_displayTitle,quality,Position(),m_playing,NetworkCommitKind::QualityReload);return;}m_youtubeSourceQuality=quality;UpdateYouTubeQualitySelection(GetMenu(m_hwnd),quality);DrawMenuBar(m_hwnd);}
    void SetDebug(D3D12Renderer::DebugView v){if(m_renderer){m_renderer->SetDebugView(v);if(!m_playing&&!m_renderer->PresentCurrent())RecoverUnusableRenderer();InvalidateControls();}}
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
            (m_adjustWnd&&IsWindowVisible(m_adjustWnd))||(m_neuralWnd&&IsWindowVisible(m_neuralWnd))||(m_encoderWnd&&IsWindowVisible(m_encoderWnd));
        if(interacting){m_fullscreenLastInput=Clock::now();return;}
        if(Clock::now()-m_fullscreenLastInput<kFullscreenIdleDelay)return;
        m_fullscreenControlsHidden=true;m_focusedToolbarAction=ToolbarAction::None;
        m_hoverAction=ToolbarAction::None;m_pressedToolbarAction=ToolbarAction::None;
        StopFullscreenTimer();SetMenu(m_hwnd,nullptr);DrawMenuBar(m_hwnd);ClearTimelineHover();
        Layout();InvalidateRect(m_hwnd,nullptr,FALSE);
    }
    void ToggleFullscreen(){
        ClearTimelineHover();
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
        case WM_STAGE_EXPORT_PROGRESS:{
            std::unique_ptr<StageExportProgress> update(reinterpret_cast<StageExportProgress*>(l));
            if(update){
                // The clock starts with the first report of a run, so the
                // elapsed figure is the export's, not the process's.
                const bool starting=update->running&&!m_stageExport.running;
                const auto started=starting?std::chrono::steady_clock::now():m_stageExport.started;
                m_stageExport=*update;m_stageExport.started=started;
                SyncActivityFeedback();UpdateCachedStatus();InvalidateControls();
                InvalidateRect(h,nullptr,FALSE);
            }
            return 0;
        }
        case WM_EXPORT_COMPLETE:CompleteExport(static_cast<uint64_t>(w));return 0;
        case WM_FRAMEGEN_PROGRESS:CompleteFrameGenerationProgress(static_cast<uint64_t>(w));return 0;
        case WM_FRAMEGEN_COMPLETE:CompleteFrameGeneration(static_cast<uint64_t>(w));return 0;
        case WM_UPDATE_CHECKED:CompleteUpdateCheck(static_cast<uint64_t>(w));return 0;
        case WM_TIMELINE_MEDIA:CompleteTimelineMedia();return 0;
        case WM_TIMER:if(w==kActivityTimerId){AnimateActivity();return 0;}if(w==kFullscreenTimerId){AutoHideFullscreenControls();return 0;}if(w==kPreviewTimerId){StartPausedSettingsPreview();return 0;}if(w==kModalTickTimerId){if(m_modalTickTimer)RunTick();return 0;}if(w==kChipFlashTimerId){AnimateStatusChips();return 0;}break;
        case WM_ENTERMENULOOP:m_fullscreenMenuLoop=true;RevealFullscreenControls();StartModalTick();break;
        case WM_EXITMENULOOP:m_fullscreenMenuLoop=false;m_fullscreenLastInput=Clock::now();StopModalTick();break;
        case WM_ENTERSIZEMOVE:StartModalTick();break;
        case WM_EXITSIZEMOVE:StopModalTick();break;
        case WM_SYSKEYDOWN:if(w==VK_MENU)RevealFullscreenControls();break;
        case WM_SYSCOMMAND:if((w&0xfff0)==SC_KEYMENU)RevealFullscreenControls();break;
        case WM_NCDESTROY:
            StopFullscreenTimer();
            if(m_fullscreenMenu){if(IsMenu(m_fullscreenMenu)&&GetMenu(h)!=m_fullscreenMenu)DestroyMenu(m_fullscreenMenu);m_fullscreenMenu=nullptr;}
            break;
        case WM_SETTINGCHANGE:ReadAnimationPreference();InvalidateRect(h,nullptr,FALSE);break;
        case WM_SHOWWINDOW:SyncActivityFeedback();break;
        case WM_DESTROY:CancelExport();DrainStageExportMessages();if(m_activityTimer){KillTimer(h,m_activityTimer);m_activityTimer=0;}StopModalTick();CancelNeuralJob(false);DrainNeuralMessages();CancelYouTubeResolution(false);DrainYouTubeCompletions();CancelSourcePrefetch();CancelUpdateCheck();m_running=false;PostQuitMessage(0);return 0;
        case WM_CLOSE:CancelExport();CancelNeuralJob(false);CancelYouTubeResolution(false);CancelSourcePrefetch();CancelUpdateCheck();DestroyWindow(h);return 0;
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
            InvalidateMonitorMode();
            Layout();InvalidateRect(h,nullptr,FALSE);return 0;
        }
        // A mode change on the monitor the window is already on keeps the same
        // HMONITOR, so the handle comparison cannot see it and the cached mode
        // has to be dropped here. Switching a 4K panel to 1080p is exactly this
        // case, and it moves the Auto rung.
        case WM_DISPLAYCHANGE:InvalidateMonitorMode();ReportUpscaleRungDrift();Layout();InvalidateRect(h,nullptr,FALSE);return 0;
        case WM_SIZE:Layout();SyncActivityFeedback();RefreshToolbarTips();if(m_shortcutSheetOpen)ShowShortcutSheet();ClearTimelineHover();return 0;
        case WM_MOVE:if(m_shortcutSheetOpen)ShowShortcutSheet();ClearTimelineHover();break;
        case WM_PAINT:Paint();return 0;
        case WM_NCMOUSEMOVE:{
            POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
            if(ScreenToClient(h,&point))FullscreenPointerMoved(h,MAKELPARAM(point.x,point.y));
            break;
        }
        case WM_MOUSEMOVE:{FullscreenPointerMoved(h,l);m_mouseX=GET_X_LPARAM(l);m_mouseY=GET_Y_LPARAM(l);if(!m_trackingMouse){TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,h,0};m_trackingMouse=TrackMouseEvent(&tracking)!=FALSE;}if(m_dragSeek&&GetCapture()==h){const double preview=SecondsFromX(m_mouseX);if(preview!=m_seekPreview){m_seekPreview=preview;InvalidatePlaybackProgress();ScrubToPreview();}}if(m_dragVolume&&GetCapture()==h)SetVolumeFromX(m_mouseX);SetHoverAction(ToolbarActionAt(m_mouseX,m_mouseY));UpdateTimelineHover(m_mouseX,m_mouseY);return 0;}
        case WM_MOUSELEAVE:m_trackingMouse=false;m_mouseX=-999;m_mouseY=-999;SetHoverAction(ToolbarAction::None);if(!m_dragSeek)ClearTimelineHover();return 0;
        case WM_LBUTTONDOWN:MouseDown(GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
        case WM_LBUTTONUP:MouseUp(GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
        case WM_CAPTURECHANGED:if(m_dragSeek){m_dragSeek=false;EndScrub();InvalidateControls();UpdateTimelineHover(m_mouseX,m_mouseY);}if(m_dragVolume)m_dragVolume=false;if(m_pressedToolbarAction!=ToolbarAction::None){m_pressedToolbarAction=ToolbarAction::None;InvalidateControls();}return 0;
        case WM_SETFOCUS:InvalidateControls();return 0;
        case WM_KILLFOCUS:InvalidateControls();return 0;
        case WM_DROPFILES:{
            // Each name sized by its own query, on the heap: this used to be a 64 KB
            // array in WndProc's frame, and every file after the first was dropped
            // without a word.
            HDROP d=reinterpret_cast<HDROP>(w);std::vector<std::wstring> dropped;
            const UINT count=DragQueryFileW(d,0xFFFFFFFF,nullptr,0);
            for(UINT i=0;i<count;++i){
                const UINT length=DragQueryFileW(d,i,nullptr,0);std::wstring path(length,L'\0');
                if(!length||DragQueryFileW(d,i,path.data(),length+1)!=length)path.clear();
                dropped.push_back(std::move(path));
            }
            DragFinish(d);
            const auto choice=dropped_files::Choose(dropped,kVideoPatterns,[](const std::wstring& path){std::error_code ec;return std::filesystem::is_directory(path,ec);});
            if(!choice.open){LOG("Dropped "<<count<<" item(s), none of them a file; nothing opened.");return 0;}
            if(choice.ignored)LOG("Dropped "<<count<<" items; opening item "<<(*choice.open+1)<<" and ignoring "<<choice.ignored<<".");
            if(Load(dropped[*choice.open])&&choice.ignored){
                m_dropNote.Set(L"Opened one dropped file; "+std::to_wstring(choice.ignored)+(choice.ignored==1?L" other was":L" others were")+L" ignored",dropped[*choice.open]);
                UpdateCachedStatus();
            }
            return 0;}
        case WM_MOUSEWHEEL:{if(m_loaded){const float step=(GET_WHEEL_DELTA_WPARAM(w)>0)?0.05f:-0.05f;const float volume=std::clamp(m_volume+step,0.0f,1.0f);const bool changed=m_muted||volume!=m_volume;if(changed){m_muted=false;m_volume=volume;Audio().SetVolume(m_volume);InvalidateToolbarAction(ToolbarAction::Mute);InvalidateVolumeControls();}}return 0;}
        case WM_COMMAND:HandleCommand(LOWORD(w));return 0;
        case WM_HOTKEY:HandleHotkey(int(w));return 0;
        // ? is a character, not a key: it is Shift+/ on a US layout and
        // somewhere else on most others, so it is matched after translation.
        case WM_CHAR:if(w==L'?'){ToggleShortcutSheet();return 0;}break;
        case WM_KEYDOWN:
            // F1 is Windows' help key; nothing else in the player used it.
            if(w==VK_F1){ToggleShortcutSheet();return 0;}
            if(w==VK_ESCAPE&&m_shortcutSheetOpen){HideShortcutSheet();return 0;}
            if(w==VK_F10)RevealFullscreenControls();
            // Esc stops every other long-running activity in this player - a
            // live session, a neural job, a YouTube resolve - and a conversion
            // is the longest of them. It had no key at all.
            if(w==VK_ESCAPE&&m_frameGenWorker.joinable()&&!m_frameGenCancelling){CancelFrameGeneration();return 0;}
            if(w==VK_TAB){FocusNextToolbarAction((GetKeyState(VK_SHIFT)&0x8000)!=0);return 0;}if(w==VK_RETURN&&m_focusedToolbarAction!=ToolbarAction::None){ActivateFocusedToolbarAction();return 0;}if(app_menu::RoutesToOpenYouTube(app_menu::PlayerCommandRoute::KeyDown,static_cast<UINT>(w),(GetKeyState(VK_CONTROL)&0x8000)!=0)){ActivateYouTube();return 0;}if((GetKeyState(VK_CONTROL)&0x8000)&&w=='O'){OpenFromDialog();return 0;}if((GetKeyState(VK_CONTROL)&0x8000)&&w=='E'){ShowAdjustments();return 0;}if(const auto command=app_menu::CommandForPlayerKey(static_cast<UINT>(w),(GetKeyState(VK_CONTROL)&0x8000)!=0,(GetKeyState(VK_SHIFT)&0x8000)!=0)){HandleCommand(*command);return 0;}if(w==VK_SPACE){TogglePause();return 0;}if(w==VK_OEM_PERIOD){StepCachedFrame();return 0;}if(w==VK_LEFT){RequestSeek(Position()-10);return 0;}if(w==VK_RIGHT){RequestSeek(Position()+10);return 0;}if(w==VK_F11){ToggleFullscreen();return 0;}if(app_menu::RoutesToRehook(app_menu::PlayerCommandRoute::KeyDown,static_cast<UINT>(w))){Rehook();return 0;}if(w=='S'){StopPlayback();return 0;}if(w=='A'){m_fill=!m_fill;Layout();return 0;}if(w=='D'){ToggleNeuralRendering();return 0;}if(w=='M'){ToggleMute();return 0;}if(w=='1'){SetDebug(D3D12Renderer::DebugView::Final);return 0;}if(w=='2'){SetDebug(D3D12Renderer::DebugView::Input);return 0;}if(w=='3'){SetDebug(D3D12Renderer::DebugView::MotionVectors);return 0;}if(w=='4'){SetDebug(D3D12Renderer::DebugView::Depth);return 0;}if(w==VK_ESCAPE&&m_liveSession){StopLiveNeuralSession(true);return 0;}if(w==VK_ESCAPE&&NeuralJobActive()){CancelNeuralJob();return 0;}if(w==VK_ESCAPE&&m_youtubeLifecycle.IsResolving()){CancelYouTubeResolution();return 0;}if(w==VK_ESCAPE&&m_fullscreen){ToggleFullscreen();return 0;}break;
        }
        return DefWindowProcW(h,m,w,l);
    }

    void HandleCommand(UINT id){
        if(id>=IDM_RECENT_VIDEO_FIRST&&id<IDM_RECENT_VIDEO_FIRST+5){OpenRecent(id-IDM_RECENT_VIDEO_FIRST);return;}
        if(id>=IDM_NEURAL_PRESET_FIRST&&id<IDM_NEURAL_PRESET_FIRST+neural_presets::kPresetCount){
            ApplyNeuralPreset(size_t(id-IDM_NEURAL_PRESET_FIRST));return;}
        if(app_menu::RoutesToRehook(app_menu::PlayerCommandRoute::NativeMenu,id)){Rehook();return;}
        if(app_menu::RoutesToOpenYouTube(app_menu::PlayerCommandRoute::NativeMenu,id,false)){ActivateYouTube();return;}
        if(const ExampleVideo* example=app_menu::ExampleVideoForCommand(id)){ActivateExampleVideo(*example);return;}
        if(const auto quality=app_menu::YouTubeQualityForCommand(id)){SetYouTubeSourceQuality(*quality);return;}
        if(id>=IDM_AUDIO_TRACK_FIRST&&id<IDM_AUDIO_TRACK_FIRST+IDM_AUDIO_TRACK_COUNT){
            ChooseAudioTrack(int(id-IDM_AUDIO_TRACK_FIRST));return;}
        switch(id){
        case IDM_OPEN:OpenFromDialog();break;case IDM_EXIT:DestroyWindow(m_hwnd);break;case IDM_PLAY:TogglePause();break;case IDM_STOP:StopPlayback();break;case IDM_BACK10:RequestSeek(Position()-10);break;case IDM_FWD10:RequestSeek(Position()+10);break;case IDM_MUTE:ToggleMute();break;case IDM_NEURAL_RENDERING:ToggleNeuralRendering();break;
        case IDM_DLSS_UPSCALING:ToggleUpscaling();break;
        case IDM_UPSCALE_AUTO:SetUpscaleTarget(0);break;
        case IDM_UPSCALE_1080:SetUpscaleTarget(1080);break;
        case IDM_UPSCALE_1440:SetUpscaleTarget(1440);break;
        case IDM_UPSCALE_2160:SetUpscaleTarget(2160);break;
        case IDM_FRAME_GENERATION:if(m_frameGenWorker.joinable())CancelFrameGeneration();else StartFrameGeneration();break;
        case IDM_SHOW_FRAMEGEN_OUTPUT:ShowFrameGenerationOutput();break;
        case IDM_FRAMEGEN_2X:SetFrameGenerationPreference(1);break;
        case IDM_FRAMEGEN_3X:SetFrameGenerationPreference(2);break;
        case IDM_FRAMEGEN_4X:SetFrameGenerationPreference(3);break;
        case IDM_FRAMEGEN_5X:SetFrameGenerationPreference(4);break;
        case IDM_FRAMEGEN_MAX:SetFrameGenerationPreference(0);break;
        case IDM_FRAMEGEN_EVEN_ONLY:SetEvenCadenceOnly(!m_evenCadenceOnly);break;
        case IDM_EXPORT_CACHED_VIDEO:ExportCachedVideo();break;
        case IDM_VIEW_FINAL:SetDebug(D3D12Renderer::DebugView::Final);break;case IDM_VIEW_INPUT:SetDebug(D3D12Renderer::DebugView::Input);break;case IDM_VIEW_MV:SetDebug(D3D12Renderer::DebugView::MotionVectors);break;case IDM_VIEW_DEPTH:SetDebug(D3D12Renderer::DebugView::Depth);break;case IDM_VIDEO_ADJUSTMENTS:ShowAdjustments();break;case IDM_ASPECT_FIT:m_fill=false;Layout();break;case IDM_ASPECT_FILL:m_fill=true;Layout();break;case IDM_FULLSCREEN:ToggleFullscreen();break;case IDM_ADVANCED_SAFE_MODE:RestartInSafeMode();break;case IDM_CLEAR_NEURAL_CACHE:ClearNeuralCache();break;
        case IDM_MARK_IN:SetMarker(true,Position100ns());break;case IDM_MARK_OUT:SetMarker(false,Position100ns());break;case IDM_CLEAR_MARKS:ClearMarkers();break;case IDM_GOTO_TIMECODE:ShowTimecodeDialog();break;
        case IDM_PAUSE_NEURAL_RENDER:if(NeuralJobActive())SetNeuralJobPaused(!NeuralJobPaused());break;
        case IDM_PREVIEW_FRAME:PreviewCurrentFrame();break;case IDM_PREVIEW_CLIP:PreviewClip();break;case IDM_RENDER_RANGE:RenderMarkedRange();break;case IDM_RENDER_WHOLE:RenderWholeSource();break;
        case IDM_NEURAL_SETTINGS:ShowNeuralSettings();break;
        // Handled before the switch would reach an unknown id, because the
        // presets are a contiguous range rather than named commands.
        case IDM_NEURAL_PRESET_CUSTOM:break;// reports state; not selectable
case IDM_EXPORT_STAGES:if(m_exportWorker.joinable())CancelExport();else ShowExportStages();break;case IDM_ENCODER_SETTINGS:ShowEncoderSettings();break;case IDM_OPEN_RENDER_RECEIPT:OpenRenderReceipt();break;
        case IDM_CHECK_FOR_UPDATES:MaybeStartUpdateCheck(true);break;
        case IDM_KEYBOARD_SHORTCUTS:ToggleShortcutSheet();break;
        case IDM_UPDATE_AVAILABLE:ActivateUpdateBadge();break;
        case IDM_COMPARE_NEURAL:SetComparisonMode(ComparisonMode::Neural);break;case IDM_COMPARE_BLEND:SetComparisonMode(ComparisonMode::Blend);break;case IDM_COMPARE_SPLIT:SetComparisonMode(ComparisonMode::SplitVertical);break;case IDM_COMPARE_WIPE:SetComparisonMode(ComparisonMode::Wipe);break;
        case IDM_COMPARE_BLEND_LESS:AdjustBlendAmount(-0.1f);break;case IDM_COMPARE_BLEND_MORE:AdjustBlendAmount(0.1f);break;case IDM_COMPARE_ZOOM:ToggleZoom();break;
        }
    }

    std::unique_ptr<RecentMediaHistory> m_recent;
    std::filesystem::path m_cacheRoot;
    std::filesystem::path m_neuralPath;
    // What the export is doing right now, posted from the worker thread and
    // read by the activity panel. Two passes at most, so "pass N of M" is the
    // honest shape rather than one bar that jumps backwards when stage two
    // starts counting its own frames from zero.
    struct StageExportProgress{
        bool running{};
        uint32_t pass{},passes{};
        const wchar_t* passKey{};
        uint64_t completedFrames{},totalFrames{};
        std::chrono::steady_clock::time_point started{};
    };
    StageExportProgress m_stageExport{};
    HWND m_exportStagesWnd{};
    ExportSelection m_exportSelection{};
    CompletionRegistry<ExportCompletion> m_exportCompletions;
    std::jthread m_exportWorker;
    CompletionRegistry<FrameGenerationCompletion> m_frameGenCompletions;
    CompletionRegistry<FrameGenerationProgressMessage> m_frameGenProgressMessages;
    std::jthread m_frameGenWorker;
    FrameGenerationProgress m_frameGenProgress{};
    // Non-zero only while a conversion runs; the status line reads it to say
    // what the progress is progress towards.
    uint32_t m_frameGenMultiplier=0;
    double m_frameGenTargetFps=0.0;
    // Set while a cancel is in flight: the request is posted and the worker is
    // retired by its own completion message, so nothing joins on the UI thread.
    bool m_frameGenCancelling=false;
    // Generated frames per source frame the user asked for: 1 (2x) on a fresh
    // install, 0 for "as many as the display allows". Kept between launches.
    uint32_t m_frameGenPreference=1;
    // Generate only when the rate divides the display's refresh. Off: see
    // SetEvenCadenceOnly and FrameRatePolicy.h for why that is the default.
    bool m_evenCadenceOnly=false;
    Clock::time_point m_frameGenStarted{};
    // When the current pair first came back NotReady, or the epoch when one is
    // assembling normally, beside when this was last asked for a pair at all.
    // The second is what separates a wait from a pause across one.
    // See ReadNextCachedFrame.
    Clock::time_point m_pairStall{};
    Clock::time_point m_pairStallRead{};
    // Trend reporting for ReportPlaybackHealth: when it last spoke, and the
    // dropped count it spoke with.
    Clock::time_point m_playbackHealthAt{};
    uint64_t m_playbackHealthDropped=0,m_playbackHealthPresented=0;
    // Presentation cadence for neural pairs: show one pair in m_presentStride,
    // m_presentPhase counting pairs since the last presented one. See
    // PlaybackCadence.h.
    uint32_t m_presentStride=1,m_presentPhase=0;
    int m_cadenceReanchors=0;bool m_cadenceReanchoredRecently=false;
    // The two halves of a presented frame, summed over a health interval.
    double m_guideMsTotal=0.0,m_renderMsTotal=0.0;uint64_t m_renderMsFrames=0;
    // Reused by the comparison reference's NV12->BGRA pass so comparing does not
    // allocate a 14 MB buffer per frame.
    std::vector<uint8_t> m_referenceBgra;
    // The last converted file, kept so "Show converted file" can reach it after
    // the completion dialog is gone. Cleared when the file stops existing.
    std::filesystem::path m_frameGenLastOutput;
    // Taskbar progress for the one job that lasts minutes. Created on first use
    // and never retried once the shell refuses it.
    Microsoft::WRL::ComPtr<ITaskbarList3> m_taskbar;
    bool m_taskbarUnavailable=false;
    bool m_taskbarReported=false;
    // Measured once per process on the first conversion, never at startup: it
    // creates and releases one NGX FrameGeneration feature on a throwaway
    // device, and an unmeasured cap would let the policy plan a rate the
    // runtime refuses.
    std::optional<FrameGenerationCapability> m_frameGenCapability;
    CompletionRegistry<UpdateCheckCompletion> m_updateCompletions;
    std::jthread m_updateWorker;
    std::optional<UpdateNotice> m_updateNotice;
    std::string m_updateLatestTag,m_updateDismissedTag;
    int64_t m_updateLastChecked=0;
    bool m_updateChecksEnabled=true;
    // One negative verdict per GPU, driver and runtime digest; a failed probe
    // is not repeated on every play and seek.
    NeuralPreflightLatch m_preflightLatch;
    // The neural helper kept between jobs. Declared ahead of m_neuralWorker,
    // like the latch above and for the same reason: members are destroyed in
    // reverse, so the job thread that uses this is joined before this goes.
    // Its destructor asks the process to exit and then closes the job object
    // over whatever is left; a player that dies without running it is covered
    // by the kill-on-close job object instead.
    ResidentNeuralHelper m_residentHelper;
    AppOptions m_opt;Localizer m_loc;UiResources m_uiResources;D3D12Renderer::ColorSettings m_colorSettings{};NVSDK_NGX_PerfQuality_Value m_activeQuality=DefaultNeuralCarrierQuality();HWND m_hwnd=nullptr,m_viewport=nullptr,m_renderWnd=nullptr,m_adjustWnd=nullptr;HFONT m_font=nullptr,m_fontSmall=nullptr,m_iconFont=nullptr;
    bool m_running=true,m_loaded=false,m_playing=false,m_haveNext=false,m_waitingForNetworkFrame=false,m_fill=false,m_fullscreen=false,m_dragSeek=false,m_dragVolume=false,m_muted=false,m_seekPending=false,m_seekResumePlaying=false,m_seeking=false,m_trackingMouse=false,m_iconFallbackLogged=false,m_neuralRequested=true;
    // Timeline scrubbing: what playback was doing before the drag, and when the
    // last preview seek went out so a drag cannot queue one per mouse move.
    bool m_dragWasPlaying=false;
    // Set by a frame step, which stops audio; the next resume restarts it.
    bool m_audioStaleAfterStep=false;
    Clock::time_point m_lastScrubSeek{};
    ToolbarAction m_pressedToolbarAction=ToolbarAction::None,m_focusedToolbarAction=ToolbarAction::None,m_hoverAction=ToolbarAction::None;
    HMENU m_fullscreenMenu=nullptr;
    UINT_PTR m_fullscreenTimer=0;
    bool m_fullscreenControlsHidden=false,m_fullscreenMenuLoop=false,m_fullscreenKeyboardFocus=false,m_fullscreenPointerKnown=false;
    POINT m_fullscreenPointer{};
    Clock::time_point m_fullscreenLastInput=Clock::now();
    // Why the last neural render did not produce a picture, in one line, for the
    // status bar. Cleared by the next successful render and by Unload.
    std::wstring m_neuralNotice;
    // What the resolver had to settle for on this source. Not a render failure,
    // so it survives a successful render and is cleared only by Unload.
    std::wstring m_sourceNotice;
    // Kept apart from m_sourceNotice because each ends on its own condition:
    // the decode note when playback moves again, the drop note when another
    // file is opened - not on the Unload an asynchronous load does on the way.
    std::wstring m_decodeNotice;
    status_note::DropNote m_dropNote;
    // Why the cache root could not be created, read once at startup. An
    // installation condition rather than a playback one, so Unload leaves it.
    std::wstring m_cacheNotice;
    // The driver notice is a modal, so it is shown once for the whole session.
    bool m_driverNoticeShown=false;
    LONG m_savedStyle=0;RECT m_savedRect{};double m_dar=16.0/9.0,m_currentSec=0,m_playStartSec=0,m_seekPreview=0,m_pendingSeekSec=0;float m_volume=1.0f,m_lastGlobalX=0,m_lastGlobalY=0;int m_mouseX=-999,m_mouseY=-999;
    // Memoised verdict of `CachedYouTubeSourceKey`, which otherwise re-hashes the
    // whole acquired copy every time the toolbar, the status text or a session gate
    // asks whether one exists. Keyed on the payload's size and write time so a
    // replaced copy is authenticated again.
    struct SourceKeyMemo {
        bool valid{};
        // The payload could not be stat'ed at all, which is a stable state and
        // memoised as one.
        bool absent{};
        std::string key;
        std::filesystem::path payload;
        uintmax_t size{};
        std::filesystem::file_time_type written{};
        std::optional<std::string> verdict;
    };
    mutable SourceKeyMemo m_sourceKeyMemo;
    // The loaded source's cache manager; see SourceCache(). The build count is
    // what the regression suite reads to prove a paint builds none.
    mutable std::unique_ptr<NeuralCacheManager> m_sourceCache;mutable std::filesystem::path m_sourceCacheRequestedRoot;mutable uint64_t m_sourceCacheBuilds=0;
    Clock::time_point m_playStart=Clock::now(),m_fpsWindowStart=Clock::now();double m_submitFps=0.0;uint64_t m_fpsWindowFrames=0;std::wstring m_path,m_youtubeAudioUrl,m_youtubePageUrl,m_displayTitle,m_cachedStatus,m_cachedWindowTitle,m_pendingYouTubeTitle,m_pendingNeuralTitle;YouTubeSourceQuality m_youtubeSourceQuality=YouTubeSourceQuality::P1080;MediaSourceKind m_sourceKind=MediaSourceKind::LocalFile;VideoDecoder m_decoder;VideoFrame m_next;D3D12RendererOwner m_renderer;TemporalGuideGenerator m_guides;AudioPlayer m_audio;std::unique_ptr<AudioPlayer>m_networkAudio;NetworkReadState m_networkReadState;YouTubeResolutionLifecycle m_youtubeLifecycle;std::unique_ptr<YouTubeResolver>m_youtubeResolver;CompletionRegistry<YouTubeCompletion>m_youtubeCompletions;std::jthread m_youtubeWorker;
    // The loaded source's digest, so a second job against the same file does
    // not pay for a second full hash. Read from job threads.
    // Shared with the job thread by value, never through `this`: the neural
    // worker deliberately captures nothing owned by the window.
    std::shared_ptr<SharedSourceDigest> m_sourceDigestMemo=std::make_shared<SharedSourceDigest>();
    NeuralPlaybackLifecycle m_neuralLifecycle;NeuralRenderProgress m_neuralProgress;CompletionRegistry<NeuralProgressMessage>m_neuralProgressMessages;CompletionRegistry<NeuralJobCompletion>m_neuralCompletions;std::jthread m_neuralWorker;SynchronizedPlayback m_synchronizedPlayback;ComparisonView m_comparisonView=ComparisonView::Original;bool m_cachedPlayback=false,m_havePresentedPair=false;uint64_t m_cachedPresentedFrames=0;// The presented pair is ALIASED, not copied: RememberRenderedCachedPair ran on
    // every presented frame and took a deep copy of both members - 11 MB per pair
    // at 1440p NV12 against a 16.7 ms budget - for two consumers that only ever
    // read on a paused redraw or a view toggle. m_previewNeuralFrame is the one
    // neural frame that does NOT come from a pair (a paused settings preview), so
    // it keeps its own storage and supersedes the pair's neural member while set.
    std::shared_ptr<const SynchronizedFramePair> m_lastPair;VideoFrame m_previewNeuralFrame;bool m_previewNeuralValid=false;RECT m_neuralCancelBounds{};uint32_t m_neuralSourceWidth=0,m_neuralSourceHeight=0;
    // A worker a live retarget asked to stop, until its completion arrives.
    // Declared after m_residentHelper for the reason m_neuralWorker is.
    std::jthread m_retiringNeuralWorker;uint64_t m_retiringNeuralGeneration=0;
    // Progress-watchdog state: the last progress the job reported and when it
    // moved, so a phase that stops reporting can be told from a slow one.
    Clock::time_point m_neuralProgressAt{};NeuralRenderPhase m_watchedPhase{};uint64_t m_watchedFrames=0,m_watchedBytes=0;
    // The loaded original is the acquired local copy of a network source: its
    // identity stays YouTube, but decode, seek and audio are local-file work.
    bool m_cachedSourceFile=false;
    // Background acquisition of a streamed source, started when a range is
    // marked so a render does not wait for the whole download.
    std::shared_ptr<SourcePrefetchState> m_prefetchState;
    std::jthread m_prefetchWorker;
    std::wstring m_prefetchPageUrl,m_prefetchTitle;
    YouTubeSourceQuality m_prefetchQuality{YouTubeSourceQuality::Auto};
    // The message loop blocks on this instead of yielding. Owned by the app so
    // the handle is created once rather than once per tick.
    PrecisionSleeper m_tickSleeper;
    bool m_guideReset=true,m_dlssReset=true;
    // Set while guides are being skipped because nothing reads them, so the
    // first frame after they resume declares its discontinuity instead of
    // claiming history against a frame that is not its predecessor.
    bool m_guidesSkipped=false;int64_t m_lastRenderedTs=-1;uint64_t m_droppedFrames=0;uint32_t m_historyGeneration=0;
    bool m_upscalingRequested=false;
    UINT_PTR m_activityTimer=0;
    // The status chips as last painted, what each last flashed on, and the
    // repaint timer that runs only while one is still fading.
    status_chips::Snapshot m_cachedChips{};status_chips::Flash m_chipFlash;UINT_PTR m_chipFlashTimer=0;
    // The timeline's render map: the file it describes and a generation that
    // retires answers about a previous one, that file's chapters, the hover
    // thumbnails already decoded, and the hover preview popup.
    TimelineMediaWorker m_timelineWorker;uint64_t m_timelineGeneration=0;std::filesystem::path m_timelineFile;
    std::vector<timeline::Chapter> m_chapters;timeline::LruCache<TimelineMediaWorker::Thumbnail> m_thumbnails{24};
    std::optional<int> m_timelineHoverX;std::optional<int64_t> m_previewKey;std::wstring m_previewText;
    timeline::PreviewLayout m_previewLayout{};HWND m_previewWnd=nullptr;
    // The keyboard cheat sheet: whether it is up, its popup, and the rows and
    // layout it was last shown with.
    bool m_shortcutSheetOpen=false;HWND m_shortcutWnd=nullptr;std::vector<ShortcutGroup> m_shortcutGroups;
    shortcut_sheet::Metrics m_shortcutMetrics{};shortcut_sheet::Layout m_shortcutLayout{};
    // Drives Tick while a modal loop owns the thread; see StartModalTick.
    UINT_PTR m_modalTickTimer=0;bool m_inTick=false;
    // The paused frame needs presenting again: the window under it was resized
    // or repainted. Renderer-side changes answer PresentationStale instead. The
    // count is what the regression suite reads.
    bool m_staticPresentPending=false;uint64_t m_staticPresents=0;
    // Which pair's original the renderer's comparison reference holds, so a
    // paused split drag re-uploads only when the pair changes.
    std::shared_ptr<const SynchronizedFramePair> m_referencePair;const D3D12Renderer* m_referenceRenderer=nullptr;
    bool m_activityBusy=false,m_activityMotionEnabled=true;
    Clock::time_point m_activityStarted=Clock::now();
    // The manual rung, only consulted when m_upscaleAuto is false. Auto is the
    // fresh-install default; 1440 is what a manual pick starts from because it
    // is the rung the fixed target used to be.
    bool m_upscaleAuto=true;
    uint32_t m_upscaleTargetHeight=1440;
    // Cached monitor mode and the handle it was read for; see MonitorModeCached.
    mutable MonitorMode m_monitorMode{};
    // Memoised answer to "does the cache hold a complete copy of this stream",
    // which frame generation asks on every status refresh and toolbar paint.
    mutable std::optional<std::filesystem::path> m_frameGenCopy;
    mutable bool m_frameGenCopyChecked=false;
    mutable HMONITOR m_monitorModeHandle=nullptr;
    mutable bool m_monitorModeValid=false;
    std::wstring m_upscalingError;
    // The frame last rendered; see RememberPlaybackFrame. Shared with its pair
    // on a synchronized pair, or held in m_ownedPlaybackFrame otherwise.
    std::shared_ptr<const VideoFrame> m_lastPlaybackFrame;std::shared_ptr<VideoFrame> m_ownedPlaybackFrame;
    // The pair member Tick presents next on a synchronized pair; see NextFrame.
    std::shared_ptr<const VideoFrame> m_nextPairFrame;
    // RenderVideoFrame's guides, kept for their grid's capacity.
    GuideFrame m_guideFrame;
    // Read when a job starts; changing them only affects the next render.
    GuideControls m_renderGuides;
    // Capture conversion: NV12 converted on the GPU, or BGRA converted by ffmpeg on
    // the CPU. Not part of NeuralSettings, because it changes how a frame is encoded
    // and not what the model is asked for, so it stays out of the cache key.
    bool m_gpuColorConversion=false;
    // Source conversion: decode to NV12 and convert on the GPU instead of letting
    // ffmpeg deliver BGRA. Off by default on a measured throughput trade, and still
    // deliberately not the same kind of switch as the one above: it changes the
    // model's INPUT, so it must join the cache key before it is ever defaulted on.
    // Its old blocker is gone - the decoder probes the source's colour tags and
    // hands over NV12 only for a matrix and range the GPU conversion implements, so
    // an undeclared or BT.601 source now falls back to the CPU conversion instead of
    // reaching the model under the wrong matrix.
    bool m_gpuSourceConversion=false;
    // p5, on the measurement recorded beside EncoderSpec::nvencPreset.
    uint32_t m_nvencPreset=5;
    NeuralSettings m_neuralSettings;
    // Frame-accurate in/out markers on the loaded source's timeline.
    RangeMarkers m_markers;
    // Presentation comparison of a synchronized pair (persisted in [Comparison]).
    static constexpr float kZoomScale=2.0f;
    ComparisonSettings m_comparison;
    HWND m_neuralWnd=nullptr;
    HWND m_encoderWnd=nullptr;
    // Tooltip host per settings dialog, and the strings each host points at:
    // TTM_ADDTOOL keeps the pointer rather than copying the text, and two of
    // these dialogs can be open at the same time.
    std::map<HWND,HWND> m_tipHosts;
    std::map<HWND,std::vector<std::unique_ptr<std::wstring>>> m_tipText;
    // Design-time (unresized) child positions for each resizable settings
    // dialog, captured once when its controls are built so WM_SIZE can
    // recompute layout without drifting across repeated resizes.
    std::map<HWND,std::vector<std::pair<HWND,RECT>>> m_settingsDesignLayout;
    POINT m_renderMouse{};
    bool m_renderMouseKnown=false,m_dragSplit=false;
    // Settings the playing cache entry was rendered with (its receipt has the full record).
    NeuralSettings m_cachedSettings;
    GuideControls m_cachedGuides;
    // Range of the playing cache entry (Whole() for full renders) and its
    // receipt.json beside the payload (empty when the entry has none).
    NeuralRenderRange m_cachedRange;
    std::filesystem::path m_cachedReceiptPath;
    // A neural toggle that arrived while a seek was in flight, waiting for it.
    bool m_neuralToggleDeferred=false;
    // Manual-reset event shared with the render helper: signalled means pause.
    HANDLE m_neuralPauseEvent=nullptr;
    // Active neural rendering: the render job runs while playback continues and
    // the player consumes finalized segments as they land. m_liveDirectory holds
    // those segments until their run is published: from then on the run plays
    // from its cache entry and the segments are deleted (ReplaceRun).
    // Consecutive attaches that had a lead to work with and still produced no
    // picture. Non-zero means the coverage does not contain the playhead.
    int m_liveAttachFailures=0;
    // Restarts already spent trying to put coverage under the playhead.
    int m_liveStalledRebases=0;
    // Two jobs that add no coverage stop the session filling holes: a refused
    // range and a cache hit that publishes an entry without a segment both look
    // like success, and retrying either forever is a render loop.
    static constexpr int kLiveRenderFailureLimit=2;
    int m_liveRenderFailures=0;
    uint64_t m_liveTargetRevision=0;
    // Turning the toggle off keeps them in the retained slot, so turning it back
    // on resumes on the frames already rendered instead of rendering them again.
    // Each job writes into its own subdirectory of m_liveDirectory.
    std::shared_ptr<NeuralSegmentIndex> m_liveSegments;
    bool m_liveSession=false,m_liveAttached=false,m_liveBuffering=false,m_liveResumePlaying=false;
    // m_liveRange is what the session set out to render - the marked range, or
    // the whole video - and it does not move. m_liveTarget is the one hole the
    // running job is filling inside it, which moves as the user watches and
    // seeks. Coverage itself lives in the index, as a set of rendered regions;
    // these two are only the intent.
    NeuralRenderRange m_liveRange{},m_liveTarget{};
    // The source and geometry the user already said yes to for a session the
    // forecast calls too slow; the question is not asked again for it.
    std::string m_livePaceConfirmedKey;
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
    // What the running or last job reported as its local source. A stream's
    // acquired copy lands here before any recent-history entry names it.
    std::filesystem::path m_jobSourcePath;
    std::string m_jobSourceKey;
    std::wstring m_jobSourcePageUrl;
    // Tick of the last playhead move the user asked for, local or network.
    uint64_t m_lastSeekTick=0;
    // Coverage can change without the newest rendered timestamp moving - a run
    // filling an earlier hole does exactly that - so the repaint trigger is the
    // index's revision. The covered duration at session start is the baseline the
    // render pace is measured against, since adopted coverage was not rendered now.
    uint64_t m_livePaintedRevision=0;int64_t m_liveCoveredAtStart=0;
    ULONGLONG m_liveStartTick=0;double m_liveStartLead=kLiveStartLead;
    // Last pass of SweepRetiredLiveSegments from the tick.
    ULONGLONG m_liveSweepTick=0;
    // The forecast this session started on, kept so the cushion has a pace to
    // size against before RealtimeRatio has enough samples to report one.
    double m_liveForecastRatio=0.0;
    uint32_t m_livePaceWidth=0,m_livePaceHeight=0;
    playback_timing::RenderPaceProfile m_renderPace;
    // Every pace this GPU measured per geometry, newest last; m_renderPace
    // carries the median of each and is what the forecast reads.
    std::vector<PaceHistory> m_paceHistory;
    // The cold start of the newest neural job, or null when no job has run.
    std::shared_ptr<NeuralColdStartRecord> m_coldStart;
    HWND m_bufferWnd=nullptr;
    RECT m_bufferAnchor{};
};

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int)
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    // Before anything that can fault, so an early crash still leaves a dump
    // beside the log rather than nothing at all.
    crash_dump::Install();
    EnablePerMonitorDpiAwareness();
    AppOptions options=ParseArgs();
    if(!options.argumentsOk){LOG("Invalid command line: "<<WideToUtf8(options.argumentError));MessageBoxW(nullptr,options.argumentError.c_str(),L"DLSS 5 Video Player",MB_OK|MB_ICONERROR);return 1;}
    const StartupResult startup=RunNeuralAddonBootstrap(options);
    if(startup==StartupResult::ExitSuccess)return 0;
    if(startup==StartupResult::ExitFailure)return 1;
    const NeuralRenderDefaults renderDefaults=ResolveNeuralRenderDefaults(
        options.neuralAddonConfigured,options.outputExplicit,options.maxW,options.maxH);
    options.maxW=renderDefaults.width;options.maxH=renderDefaults.height;
    if(options.neuralAddonConfigured&&!options.outputExplicit)
        LOG("Neural pre-render defaults active: YouTube Auto takes the tallest rung up to 1440p, or up to 2160p when nothing lower exists; local files retain native source resolution and spatial upscaling stays off.");
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
                if(app.NeedsRealtimeTick())app.WaitForNextTick();
                else WaitMessage();
            }
            return 0;
        },
        [] { MFShutdown(); },
        [] { CoUninitialize(); });
}
