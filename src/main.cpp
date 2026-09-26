#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <windows.media.h>
#include <SystemMediaTransportControlsInterop.h>
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
#include "TemporalSettings.h"
#include "StrictJson.h"
#include "AudioPlayer.h"
#include "SubtitleOverlay.h"
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
#include "SeekPolicy.h"
#include "RenderPacePolicy.h"
#include "PlayerCommandLine.h"
#include "PlaybackTickPolicy.h"
#include "NeuralJobPolicy.h"
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
#include "NeuralMotionPolicy.h"
#include "ExportPipeline.h"
#include "RenderCommandLine.h"
#include "UpdateCheck.h"
#include "TrailerThumbnail.h"
#include "SynchronizedPlayback.h"
#include "BackgroundFileReaper.h"
#include "StatusChipPolicy.h"
#include "ToolbarTipPolicy.h"
#include "ChromeMotionPolicy.h"
#include "EscapeKeyPolicy.h"
#include "InitialWindowPolicy.h"
#include "WindowPlacementPolicy.h"
#include "SliderPolicy.h"
#include "WarmUpPolicy.h"
#include "TimelinePolicy.h"
#include "ShortcutSheetPolicy.h"
#include "DarkModePolicy.h"
#include "MediaTransportPolicy.h"
#include "StartScreenPolicy.h"
#include "CompareBarPolicy.h"
#include "CompareViewPolicy.h"
#include "CompareMaskPolicy.h"
#include "CompareImageIO.h"
#include "resources.h"
#include "HdrPolicy.h"

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

// Popup menus - File under the dark menu bar, the compare bar's mode menu, an edit
// box's context menu - were drawn light, because only the bar itself is owner-drawn
// (WM_UAHDRAWMENU, DarkModePolicy.h). uxtheme's SetPreferredAppMode(ForceDark),
// ordinal 135 since Windows 10 1903 with FlushMenuThemes at 136, is the switch
// Explorer and Notepad++ use for them. Undocumented like the bar messages, and
// guarded the same way: only on a build that has it, found by ordinal, and a
// failure anywhere leaves light menus and nothing else.
static void EnableDarkPopupMenus()
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (!rtlGetVersion || rtlGetVersion(&version) != 0 || version.dwMajorVersion < 10 || version.dwBuildNumber < 18362) return;
    const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    const auto setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    const auto flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    if (!setPreferredAppMode) return;
    constexpr int kForceDark = 2;
    setPreferredAppMode(kForceDark);
    if (flushMenuThemes) flushMenuThemes();
}

// The shipped exe declares per-monitor v2 in its manifest (DLSSVideoPlayer.manifest),
// and then this call is refused as already set; it matters for a binary without one.
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
// Colour strength came back on RenoDX 6.5.3, which honours it (68-89 % of bytes
// move at 1.00 -> 0.20; docs/measurements/knobs-653-20260924). 7306 was the
// render-preset combo, removed because the runtime still ignores the preset.
static constexpr int IDC_NS_COLOR = 7305;
static constexpr int IDC_NS_STYLE = 7307;
static constexpr int IDC_NS_AUTOMASK = 7308;
static constexpr int IDC_NS_PASSES = 7309;
static constexpr int IDC_NS_CHAINED = 7310;
static constexpr int IDC_NS_GUIDE_MV = 7311;
static constexpr int IDC_NS_GUIDE_DEPTH = 7312;
// 7313-7315 were the encoder controls, moved to their own dialog below.
// The Temporal group (TemporalSettings.h).
static constexpr int IDC_NS_SCENE_CUTS = 7340;
static constexpr int IDC_NS_STABILITY = 7341;
static constexpr int IDC_NS_RESET = 7320;
static constexpr int IDC_NS_APPLY = 7321;
static constexpr int IDC_NS_CLOSE = 7322;

static constexpr int IDC_ES_GPU_CONVERT = 7401;
static constexpr int IDC_ES_GPU_SOURCE = 7402;
static constexpr int IDC_ES_NVENC_PRESET = 7403;
static constexpr int IDC_ES_RESET = 7404;
static constexpr int IDC_ES_CLOSE = 7405;
static constexpr int IDC_ES_CAPTURE_DITHER = 7406;
static constexpr int IDC_ES_CACHE_QUALITY = 7407;
static constexpr int IDC_ES_DEBAND = 7408;
static constexpr int IDC_ES_EXPOSURE = 7409;
static constexpr int IDC_EX_UPSCALE = 7501;
static constexpr int IDC_EX_NEURAL = 7502;
static constexpr int IDC_EX_FRAMEGEN = 7503;
static constexpr int IDC_EX_RESOLUTION = 7504;
static constexpr int IDC_EX_MULTIPLIER = 7505;
static constexpr int IDC_EX_SUMMARY = 7506;
static constexpr int IDC_EX_RUN = 7507;
static constexpr int IDC_EX_CLOSE = 7508;
static constexpr int IDC_EX_HISTORY = 7509;

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
// A press on Windows' media controls (wParam: the SMTC button) and a seek from
// their timeline (lParam: milliseconds), both posted from WinRT's thread pool.
static constexpr UINT WM_MEDIA_BUTTON = WM_APP + 50;
static constexpr UINT WM_MEDIA_SEEK = WM_APP + 51;
// A start-screen fact arrived from GatherStartScreen.
static constexpr UINT WM_START_SCREEN = WM_APP + 52;

struct YouTubeUrlDialogState {
    const Localizer* localizer{};
    HFONT font{};
    // The dpi the controls were laid out at, and a font of our own once a
    // dpi change has replaced the one the owner lent.
    UINT dpi{};
    HFONT ownedFont{};
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

// ---- Dialog chrome: DPI and dark ----------------------------------------
//
// Shared by the two modal prompts below and the four settings dialogs in
// PlayerApp, so every dialog the player opens is sized for the monitor it is
// on and drawn in the player's own colours.

// AdjustWindowRectEx reports 96-dpi frame metrics to a per-monitor-aware
// process, which leaves the client short on a scaled monitor; the ForDpi form
// is resolved at run time because it needs Windows 10 1607.
static BOOL AdjustWindowRectForDpi(RECT& rect, DWORD style, BOOL menu, DWORD exStyle, UINT dpi)
{
    using AdjustForDpiFn=BOOL(WINAPI*)(LPRECT,DWORD,BOOL,DWORD,UINT);
    static const auto adjustForDpi=reinterpret_cast<AdjustForDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"),"AdjustWindowRectExForDpi"));
    const RECT original=rect;
    if(adjustForDpi&&adjustForDpi(&rect,style,menu,exStyle,dpi))return TRUE;
    rect=original;
    return AdjustWindowRectEx(&rect,style,menu,exStyle);
}

// Segoe UI at `heightDip` device-independent pixels. The player's own text
// and the dialogs' come from here, so both scale the same way.
static HFONT CreateUiFont(int heightDip, UINT dpi)
{
    return CreateFontW(-MulDiv(heightDip,static_cast<int>(dpi==0?USER_DEFAULT_SCREEN_DPI:dpi),USER_DEFAULT_SCREEN_DPI),
                       0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
}

// The same face as `font`, scaled from one dpi to another; null when `font`
// cannot be read, and the caller keeps the one it has.
static HFONT ScaleFontForDpi(HFONT font, UINT from, UINT to)
{
    LOGFONTW logical{};
    if(!font||!GetObjectW(font,sizeof(logical),&logical))return nullptr;
    logical.lfHeight=MulDiv(logical.lfHeight,static_cast<int>(to?to:USER_DEFAULT_SCREEN_DPI),static_cast<int>(from?from:USER_DEFAULT_SCREEN_DPI));
    logical.lfWidth=0;
    return CreateFontIndirectW(&logical);
}

// Every child of `parent` carried from one dpi to the next and given `font`.
// Windows resizes the dialog itself (WM_DPICHANGED's suggested rectangle);
// the controls inside are the dialog's job.
static void ScaleChildWindows(HWND parent, UINT from, UINT to, HFONT font)
{
    struct Context{HWND parent;UINT from,to;HFONT font;};
    Context context{parent,from,to,font};
    EnumChildWindows(parent,[](HWND child,LPARAM parameter)->BOOL{
        const auto& c=*reinterpret_cast<const Context*>(parameter);
        if(GetParent(child)!=c.parent)return TRUE;
        RECT r{};GetWindowRect(child,&r);MapWindowPoints(nullptr,c.parent,reinterpret_cast<POINT*>(&r),2);
        const RECT scaled=dark_mode::ScaleRectForDpi(r,c.from,c.to);
        SetWindowPos(child,nullptr,scaled.left,scaled.top,scaled.right-scaled.left,scaled.bottom-scaled.top,SWP_NOZORDER|SWP_NOACTIVATE);
        if(c.font)SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(c.font),TRUE);
        return TRUE;
    },reinterpret_cast<LPARAM>(&context));
}

// The player's title bar is dark (DWMWA_USE_IMMERSIVE_DARK_MODE, attribute
// 20); a dialog it opens should not be the one light frame on the screen.
static void ApplyDarkTitleBar(HWND window)
{
    BOOL dark=TRUE;DwmSetWindowAttribute(window,20,&dark,sizeof(dark));
}

static HBRUSH DarkDialogBrush(){static const HBRUSH brush=CreateSolidBrush(dark_mode::DialogBackground);return brush;}
static HBRUSH DarkFieldBrush(){static const HBRUSH brush=CreateSolidBrush(dark_mode::FieldBackground);return brush;}

// WM_CTLCOLOR* for a dark dialog: the background every control sits on, and
// the text colour of the ones that honour it (statics, edits, lists; a themed
// check box draws its own). Statics, check boxes and trackbars sit on the
// dialog; edits and lists on the field surface. Returns null for a message it
// does not own.
static HBRUSH DarkControlColor(UINT message, WPARAM wParam, COLORREF text)
{
    const HDC dc=reinterpret_cast<HDC>(wParam);
    switch(message){
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor(dc,text);SetBkColor(dc,dark_mode::DialogBackground);return DarkDialogBrush();
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(dc,dark_mode::Text);SetBkColor(dc,dark_mode::FieldBackground);return DarkFieldBrush();
    default:return nullptr;
    }
}

// The dark visual style for one dialog control, now that the manifest brings
// Common Controls 6 (DLSSVideoPlayer.manifest): combo boxes and edits take
// Windows' dark "CFD" style, the common file dialog's, with light text on a
// dark field; check boxes, radios, trackbars, scroll bars, lists and tooltips
// take DarkMode_Explorer, whose check box draws a light caption beside a dark
// box. uxtheme is loaded by name, so a Windows without the styles keeps the
// themed light look and the WM_CTLCOLOR* backgrounds below.
static void ApplyDarkControlTheme(HWND control)
{
    if(!control)return;
    using SetWindowThemeFn=HRESULT(WINAPI*)(HWND,LPCWSTR,LPCWSTR);
    static const auto setTheme=[]{
        const HMODULE uxtheme=LoadLibraryExW(L"uxtheme.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        return uxtheme?reinterpret_cast<SetWindowThemeFn>(GetProcAddress(uxtheme,"SetWindowTheme")):nullptr;
    }();
    if(!setTheme)return;
    wchar_t name[64]{};GetClassNameW(control,name,int(std::size(name)));
    const std::wstring_view kind(name);
    if(kind==L"ComboBox"||kind==L"Edit"){setTheme(control,L"DarkMode_CFD",nullptr);return;}
    if(kind==L"Button"){
        const LONG_PTR type=GetWindowLongPtrW(control,GWL_STYLE)&BS_TYPEMASK;
        const bool check=type==BS_CHECKBOX||type==BS_AUTOCHECKBOX||type==BS_3STATE||type==BS_AUTO3STATE||
                         type==BS_RADIOBUTTON||type==BS_AUTORADIOBUTTON;
        if(check)setTheme(control,L"DarkMode_Explorer",nullptr);
        return;
    }
    if(kind==L"msctls_trackbar32"||kind==L"ScrollBar"||kind==L"tooltips_class32"||kind==L"ListBox")
        setTheme(control,L"DarkMode_Explorer",nullptr);
}

// Push buttons are owner-drawn, because a classic push button is painted in
// system grey whatever its parent says. The dialog's default button is marked
// with this property and drawn in the accent, as the toolbar draws a control
// that is on: BS_DEFPUSHBUTTON cannot be combined with BS_OWNERDRAW.
static constexpr const wchar_t* kDefaultButtonProperty=L"DLSSVideo.DefaultButton";
static void MarkDefaultButton(HWND button){if(button)SetPropW(button,kDefaultButtonProperty,reinterpret_cast<HANDLE>(1));}
// Window properties are not freed with the window, so a dialog takes its
// children's off before they go (WM_DESTROY reaches the parent first).
static void RemoveChildProperties(HWND parent, std::initializer_list<const wchar_t*> names)
{
    struct Context{HWND parent;std::initializer_list<const wchar_t*> names;};
    Context context{parent,names};
    EnumChildWindows(parent,[](HWND child,LPARAM parameter)->BOOL{
        for(const wchar_t* name:reinterpret_cast<const Context*>(parameter)->names)RemovePropW(child,name);
        return TRUE;
    },reinterpret_cast<LPARAM>(&context));
}
// Keyboard focus on a rounded button: a ring that follows the button's corners,
// in the button's own text colour so it reads on every fill, blue and teal
// included. DrawFocusRect drew a dotted square inside the rounded shape.
static void DrawRoundedFocusRing(HDC dc,RECT bounds,int radius,COLORREF color,UINT dpi)
{
    const int inset=MulDiv(3,static_cast<int>(dpi?dpi:USER_DEFAULT_SCREEN_DPI),USER_DEFAULT_SCREEN_DPI);
    const int width=std::max(1,MulDiv(2,static_cast<int>(dpi?dpi:USER_DEFAULT_SCREEN_DPI),USER_DEFAULT_SCREEN_DPI));
    InflateRect(&bounds,-inset,-inset);
    const int corner=std::max(1,radius-inset);
    HPEN pen=CreatePen(PS_INSIDEFRAME,width,color);if(!pen)return;
    const HGDIOBJ oldPen=SelectObject(dc,pen),oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
    RoundRect(dc,bounds.left,bounds.top,bounds.right,bounds.bottom,corner*2,corner*2);
    SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(pen);
}
// The player's one slider look (slider::Layout gives the geometry): a rounded
// rail, the stretch from the origin to the value filled in the progress blue,
// and a round knob with a dark rim so it reads on the fill. The rail and the
// knob are separate calls so a mark (the Mix's 100% tick) can sit between them.
static void DrawSliderRail(HDC dc,const slider::Geometry& g,bool enabled)
{
    const int radius=std::max<int>(1,(g.track.bottom-g.track.top)/2);
    const auto pill=[&](const RECT& r,COLORREF color){
        if(r.right<=r.left)return;
        HBRUSH brush=CreateSolidBrush(color);HPEN pen=CreatePen(PS_SOLID,1,color);
        const HGDIOBJ oldBrush=SelectObject(dc,brush),oldPen=SelectObject(dc,pen);
        RoundRect(dc,r.left,r.top,r.right,r.bottom,radius*2,radius*2);
        SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(brush);DeleteObject(pen);
    };
    pill(g.track,enabled?RGB(68,71,77):ui_palette::Inactive);
    pill(g.fill,enabled?ui_palette::PrimaryBlue:RGB(78,82,90));
}
static void DrawSliderKnob(HDC dc,const slider::Geometry& g,bool enabled,bool focus,UINT dpi)
{
    const int r=g.knobRadius;
    HBRUSH brush=CreateSolidBrush(enabled?RGB(246,246,248):RGB(98,101,108));HPEN pen=CreatePen(PS_SOLID,1,ui_palette::Window);
    const HGDIOBJ oldBrush=SelectObject(dc,brush),oldPen=SelectObject(dc,pen);
    Ellipse(dc,g.knob.x-r,g.knob.y-r,g.knob.x+r+1,g.knob.y+r+1);
    SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(brush);DeleteObject(pen);
    if(!focus)return;
    // Keyboard focus: a ring round the knob in the accent, clear of it by 2 dip.
    const int gap=slider::Dip(2,dpi),width=std::max(1,slider::Dip(2,dpi)),ring=r+gap+width;
    HPEN ringPen=CreatePen(PS_INSIDEFRAME,width,ui_palette::PrimaryBlue);
    const HGDIOBJ oldRingPen=SelectObject(dc,ringPen),oldHollow=SelectObject(dc,GetStockObject(NULL_BRUSH));
    Ellipse(dc,g.knob.x-ring,g.knob.y-ring,g.knob.x+ring+1,g.knob.y+ring+1);
    SelectObject(dc,oldHollow);SelectObject(dc,oldRingPen);DeleteObject(ringPen);
}

// A settings dialog's trackbar, drawn as the player's slider. The native
// control stays - keyboard, accessibility, TBM_* and WM_HSCROLL all keep
// working - and only its paint is replaced (NM_CUSTOMDRAW, CDRF_SKIPDEFAULT).
// The knob sits where the control's own thumb is, so the pointer grabs what
// it sees; the fill runs from the value where the setting does nothing
// (kSliderOriginProperty, a position + 1) to the value, so a row at its
// default reads as untouched.
static constexpr const wchar_t* kSliderOriginProperty=L"DLSSVideo.SliderOrigin";
static void DrawDialogSlider(const NMCUSTOMDRAW& draw)
{
    const HWND track=draw.hdr.hwndFrom;
    RECT client{};GetClientRect(track,&client);
    const int width=client.right-client.left,height=client.bottom-client.top;
    if(width<=0||height<=0)return;
    const HDC target=draw.hdc;
    const HDC dc=CreateCompatibleDC(target);
    const HBITMAP bitmap=dc?CreateCompatibleBitmap(target,width,height):nullptr;
    if(!dc||!bitmap){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);return;}
    const HGDIOBJ oldBitmap=SelectObject(dc,bitmap);
    FillRect(dc,&client,DarkDialogBrush());
    const UINT dpi=ActiveWindowDpi(track);
    const int lo=int(SendMessageW(track,TBM_GETRANGEMIN,0,0)),hi=int(SendMessageW(track,TBM_GETRANGEMAX,0,0));
    const int pos=int(SendMessageW(track,TBM_GETPOS,0,0));
    RECT channel{};SendMessageW(track,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&channel));
    RECT thumb{};SendMessageW(track,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
    // The native thumb travels between these two centres; the rail is laid on
    // exactly that span (slider::Layout insets by the knob's rest radius).
    const int half=std::max<int>(1,(thumb.right-thumb.left)/2),inset=slider::Dip(slider::kKnobRestDip,dpi);
    const RECT area{channel.left+half-inset,0,channel.right-half+inset,height};
    const double span=double(std::max(1,hi-lo));
    const INT_PTR origin=reinterpret_cast<INT_PTR>(GetPropW(track,kSliderOriginProperty));
    const double from=origin>0?double(origin-1-lo)/span:0.0;
    const bool enabled=IsWindowEnabled(track)!=FALSE,dragging=GetCapture()==track;
    slider::Geometry g=slider::Layout(area,double(pos-lo)/span,from,dragging?1.0:0.0,dpi);
    if(thumb.right>thumb.left)g.knob.x=(thumb.left+thumb.right)/2;
    DrawSliderRail(dc,g,enabled);
    DrawSliderKnob(dc,g,enabled,GetFocus()==track&&(SendMessageW(track,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)==0,dpi);
    BitBlt(target,0,0,width,height,dc,0,0,SRCCOPY);
    SelectObject(dc,oldBitmap);DeleteObject(bitmap);DeleteDC(dc);
}

static void DrawDarkPushButton(const DRAWITEMSTRUCT& item)
{
    const bool enabled=(item.itemState&ODS_DISABLED)==0,pressed=(item.itemState&ODS_SELECTED)!=0;
    const bool isDefault=GetPropW(item.hwndItem,kDefaultButtonProperty)!=nullptr;
    const ButtonVisual visual=ResolveButtonVisual(ButtonState{enabled,isDefault,false,false,pressed,(item.itemState&ODS_FOCUS)!=0});
    const UINT dpi=ActiveWindowDpi(item.hwndItem);
    const int radius=MulDiv(4,static_cast<int>(dpi),USER_DEFAULT_SCREEN_DPI);
    const HBRUSH background=DarkDialogBrush();FillRect(item.hDC,&item.rcItem,background);
    HBRUSH brush=CreateSolidBrush(visual.fill);HPEN pen=CreatePen(PS_SOLID,1,visual.border);
    const HGDIOBJ oldBrush=SelectObject(item.hDC,brush),oldPen=SelectObject(item.hDC,pen);
    RoundRect(item.hDC,item.rcItem.left,item.rcItem.top,item.rcItem.right,item.rcItem.bottom,radius*2,radius*2);
    SelectObject(item.hDC,oldBrush);SelectObject(item.hDC,oldPen);DeleteObject(brush);DeleteObject(pen);
    wchar_t text[128]{};GetWindowTextW(item.hwndItem,text,static_cast<int>(std::size(text)));
    const HFONT font=reinterpret_cast<HFONT>(SendMessageW(item.hwndItem,WM_GETFONT,0,0));
    const HGDIOBJ oldFont=font?SelectObject(item.hDC,font):nullptr;
    SetBkMode(item.hDC,TRANSPARENT);SetTextColor(item.hDC,visual.text);
    RECT label=item.rcItem;DrawTextW(item.hDC,text,-1,&label,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|
                                     ((item.itemState&ODS_NOACCEL)?DT_HIDEPREFIX:0u));
    if(oldFont)SelectObject(item.hDC,oldFont);
    if(visual.drawFocus)DrawRoundedFocusRing(item.hDC,item.rcItem,radius,visual.text,dpi);
}

// The two modal prompts' share of the dark chrome and the dpi handling: the
// colours, the owner-drawn buttons, and a move to a monitor at another dpi,
// which scales the controls and replaces the font the owner lent. `error` is
// the one static drawn in the error colour. Returns whether it answered.
template<class State>
static bool ModalDialogChrome(HWND window, UINT message, WPARAM wParam, LPARAM lParam, State& state, LRESULT& result)
{
    switch(message){
    case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORBTN:case WM_CTLCOLORLISTBOX:case WM_CTLCOLORDLG:
        result=reinterpret_cast<LRESULT>(DarkControlColor(message,wParam,
            reinterpret_cast<HWND>(lParam)==state.error?dark_mode::ErrorText:dark_mode::Text));
        return true;
    case WM_DRAWITEM:{
        const auto* item=reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if(!item||item->CtlType!=ODT_BUTTON)return false;
        DrawDarkPushButton(*item);result=TRUE;return true;
    }
    case WM_DPICHANGED:{
        const UINT dpi=HIWORD(wParam);
        HFONT font=ScaleFontForDpi(state.ownedFont?state.ownedFont:state.font,state.dpi,dpi);
        ScaleChildWindows(window,state.dpi,dpi,font);
        if(font){if(state.ownedFont)DeleteObject(state.ownedFont);state.ownedFont=font;}
        state.dpi=dpi;
        if(const auto* suggested=reinterpret_cast<const RECT*>(lParam))
            SetWindowPos(window,nullptr,suggested->left,suggested->top,suggested->right-suggested->left,
                         suggested->bottom-suggested->top,SWP_NOZORDER|SWP_NOACTIVATE);
        result=0;return true;
    }
    case WM_DESTROY:
        RemoveChildProperties(window,{kDefaultButtonProperty});
        return false;
    case WM_NCDESTROY:
        if(state.ownedFont){DeleteObject(state.ownedFont);state.ownedFont=nullptr;}
        return false;
    default:return false;
    }
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
    if (LRESULT result = 0; ModalDialogChrome(window, message, wParam, lParam, *state, result)) return result;

    switch (message) {
    case WM_CREATE: {
        state->dpi = ActiveWindowDpi(window);
        ApplyDarkTitleBar(window);
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
        ApplyDarkControlTheme(state->edit);
        HWND paste = CreateWindowExW(0, L"BUTTON", state->localizer->Get(L"youtube.dialog.paste").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
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
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            client.right - pad - buttonWidth * 2 - DialogDip(window, 10), client.bottom - pad - buttonHeight,
            buttonWidth, buttonHeight, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), nullptr, nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", state->localizer->Get(L"youtube.dialog.cancel").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            client.right - pad - buttonWidth, client.bottom - pad - buttonHeight,
            buttonWidth, buttonHeight, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), nullptr, nullptr);
        for (HWND control : {label, state->edit, paste, note, state->error, play, cancel}) {
            SetControlFont(control, state->font);
        }
        MarkDefaultButton(play);
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
    dialogClass.hbrBackground = DarkDialogBrush();
    if (!RegisterClassW(&dialogClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    RECT bounds{0, 0, DialogDip(owner, clientWidth), DialogDip(owner, clientHeight)};
    AdjustWindowRectForDpi(bounds, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                           WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, ActiveWindowDpi(owner));
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
    // The dpi the controls were laid out at, and a font of our own once a
    // dpi change has replaced the one the owner lent.
    UINT dpi{};
    HFONT ownedFont{};
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
    if (LRESULT result = 0; ModalDialogChrome(window, message, wParam, lParam, *state, result)) return result;

    switch (message) {
    case WM_CREATE: {
        state->dpi = ActiveWindowDpi(window);
        ApplyDarkTitleBar(window);
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
        ApplyDarkControlTheme(state->edit);
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
        HWND cancel = button(L"timecode.cancel", IDCANCEL, BS_OWNERDRAW);
        HWND setOut = button(L"timecode.set_out", IDC_TIMECODE_SET_OUT, BS_OWNERDRAW);
        HWND setIn = button(L"timecode.set_in", IDC_TIMECODE_SET_IN, BS_OWNERDRAW);
        HWND go = button(L"timecode.go", IDOK, BS_OWNERDRAW);
        MarkDefaultButton(go);
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
    TemporalSettings temporal;
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
    const player_command_line::Parsed parsed=player_command_line::Parse(o.userArguments);
    o.maxW=parsed.maxWidth;o.maxH=parsed.maxHeight;o.outputExplicit=parsed.outputExplicit;o.file=parsed.file;
    if(!parsed.error.empty()){o.argumentsOk=false;o.argumentError=parsed.error;}
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

// Where "Export with DLSS stages" writes. Only the containers the export can
// actually write for this source are offered (ExportContainerChoices), the
// returned name always ends in one of them, and replacing a file is asked
// about here because the export replaces it.
static std::filesystem::path PickStageExportFile(HWND owner, std::wstring_view title, bool photo, bool animation) {
    wchar_t path[32768]{};
    std::wstring suggested(title.empty()?L"neural-video":std::wstring(title));
    for(wchar_t& c:suggested)if(c==L'<'||c==L'>'||c==L':'||c==L'"'||c==L'/'||c==L'\\'||c==L'|'||c==L'?'||c==L'*')c=L'_';
    suggested+=L"-dlss";wcsncpy_s(path,suggested.c_str(),_TRUNCATE);
    const auto choices=ExportContainerChoices(photo,animation);
    std::wstring filter;
    for(const ExportContainer container:choices){
        const auto [name,pattern]=ExportContainerFilter(container);
        filter+=name;filter.push_back(L'\0');filter+=pattern;filter.push_back(L'\0');
    }
    filter.push_back(L'\0');
    const std::wstring defaultExtension=ExportContainerExtension(choices.front())+1;
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=owner;dialog.lpstrFile=path;dialog.nMaxFile=static_cast<DWORD>(std::size(path));
    dialog.lpstrFilter=filter.c_str();dialog.nFilterIndex=1;dialog.lpstrDefExt=defaultExtension.c_str();dialog.lpstrTitle=L"Export with DLSS stages to a file";
    dialog.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|OFN_OVERWRITEPROMPT;
    if(!GetSaveFileNameW(&dialog))return {};
    const std::filesystem::path chosen(ExportFileName(path,dialog.nFilterIndex?dialog.nFilterIndex-1u:0u,photo,animation));
    // The dialog asked about the name it returned; an extension appended here
    // names a different file, which has to be asked about again.
    std::error_code existsError;
    if(chosen!=std::filesystem::path(path)&&std::filesystem::exists(chosen,existsError)){
        const std::wstring question=chosen.filename().wstring()+L" already exists.\nDo you want to replace it?";
        if(MessageBoxW(owner,question.c_str(),L"Export with DLSS stages",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return {};
    }
    return chosen;
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

// Where a saved comparison goes (P2.21).
static std::filesystem::path PickComparisonImage(HWND owner, const Localizer& loc, const std::wstring& suggested) {
    wchar_t path[32768]{};wcsncpy_s(path,suggested.c_str(),_TRUNCATE);
    const wchar_t filter[]=L"PNG image (*.png)\0*.png\0\0";
    const std::wstring title=loc.Get(L"compare.save.dialog");
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=owner;dialog.lpstrFile=path;dialog.nMaxFile=static_cast<DWORD>(std::size(path));
    dialog.lpstrFilter=filter;dialog.nFilterIndex=1;dialog.lpstrDefExt=L"png";dialog.lpstrTitle=title.c_str();
    dialog.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|OFN_OVERWRITEPROMPT;
    return GetSaveFileNameW(&dialog)?std::filesystem::path(path):std::filesystem::path{};
}

// A mask for the Mix (P2.6): any still image WIC reads, used as grey.
static std::filesystem::path PickMaskImage(HWND owner, const Localizer& loc) {
    wchar_t path[32768]{};
    std::wstring filter=loc.Get(L"compare.mask.filter");filter.push_back(L'\0');
    filter+=L"*.png;*.bmp;*.jpg;*.jpeg;*.tif;*.tiff;*.gif";filter.push_back(L'\0');filter.push_back(L'\0');
    const std::wstring title=loc.Get(L"compare.mask.dialog");
    OPENFILENAMEW o{};o.lStructSize=sizeof(o);o.hwndOwner=owner;o.lpstrFile=path;o.nMaxFile=static_cast<DWORD>(std::size(path));
    o.lpstrFilter=filter.c_str();o.nFilterIndex=1;o.lpstrTitle=title.c_str();
    o.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_EXPLORER|OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&o)?std::filesystem::path(path):std::filesystem::path{};
}

// A subtitle file for the loaded source (P3.2): anything FFmpeg reads subtitles from.
static std::filesystem::path PickSubtitleFile(HWND owner, const Localizer& loc) {
    wchar_t path[32768]{};
    std::wstring filter=loc.Get(L"subtitles.filter");filter.push_back(L'\0');
    filter+=L"*.srt;*.ass;*.ssa;*.vtt;*.sup;*.idx;*.mks;*.mkv";filter.push_back(L'\0');
    filter+=loc.Get(L"dialog.all");filter.push_back(L'\0');filter+=L"*.*";filter.push_back(L'\0');filter.push_back(L'\0');
    const std::wstring title=loc.Get(L"subtitles.dialog");
    OPENFILENAMEW o{};o.lStructSize=sizeof(o);o.hwndOwner=owner;o.lpstrFile=path;o.nMaxFile=static_cast<DWORD>(std::size(path));
    o.lpstrFilter=filter.c_str();o.nFilterIndex=1;o.lpstrTitle=title.c_str();
    o.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_EXPLORER|OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&o)?std::filesystem::path(path):std::filesystem::path{};
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

static const wchar_t* ExportRefusalKey(ExportRefusal refusal){
    switch(refusal){
    case ExportRefusal::NothingSelected:return L"export.stages.refusal.nothing";
    case ExportRefusal::SourceGeometryUnknown:return L"export.stages.refusal.geometry";
    case ExportRefusal::AlreadyAtTarget:return L"export.stages.refusal.target";
    case ExportRefusal::MultiplierUnsupported:return L"export.stages.refusal.multiplier";
    case ExportRefusal::StillImage:return L"export.stages.refusal.still";
    case ExportRefusal::None:break;
    }
    return L"export.stages.refusal.nothing";
}

// The saved processing scale, or the default for a value that is not a rung -
// a hand-edited 60 is not a choice this player offers, so it is not honoured.
static uint32_t ReadProcessingScale(const std::filesystem::path& settings){
    const UINT saved=GetPrivateProfileIntW(L"NeuralRender",L"ProcessingScale",kDefaultProcessingScale,settings.c_str());
    return IsProcessingScaleRung(saved)?saved:kDefaultProcessingScale;
}

// The saved Super Resolution history, by name; anything else is the default.
static UpscalingHistory ReadUpscalingHistory(const std::filesystem::path& settings){
    wchar_t saved[32]{};
    GetPrivateProfileStringW(L"Playback",L"UpscalingHistory",L"",saved,static_cast<DWORD>(std::size(saved)),settings.c_str());
    return ParseUpscalingHistory(WideToUtf8(saved)).value_or(kRecommendedUpscalingHistory);
}

// The saved [Encoding] CacheQuality rung. A name this build does not know - a newer
// player's rung - reads as Standard.
static EncoderQuality ReadCacheQuality(const std::filesystem::path& settings){
    wchar_t rung[32]{};
    GetPrivateProfileStringW(L"Encoding",L"CacheQuality",L"standard",rung,static_cast<DWORD>(std::size(rung)),settings.c_str());
    std::string narrow;for(const wchar_t* c=rung;*c;++c)narrow.push_back(*c<0x80?static_cast<char>(*c):'?');
    EncoderQuality quality=EncoderQuality::Standard;
    ParseEncoderQuality(narrow,quality);
    return quality;
}

// Everything a neural render writes into the add-on's [RenoDX.DLSS5]: the
// Neural settings, and the model's place against the carrier's upscale that
// the processing scale needs (PreUpscaleOverride says when that is written).
static std::vector<NeuralAddonOverride> RenderAddonOverrides(const std::filesystem::path& ini,
                                                             const NeuralSettings& settings,
                                                             uint32_t processingScale){
    std::vector<NeuralAddonOverride> overrides=NeuralAddonOverridesFor(settings);
    std::string current;
    if(const auto snapshot=ReadNeuralAddonSettingsSnapshot(ini)){
        // The snapshot is canonical: one exact-case key per line.
        constexpr std::string_view kKey="\nNRPreUpscale=";
        if(const size_t at=snapshot->find(kKey);at!=std::string::npos){
            const size_t begin=at+kKey.size();
            current=snapshot->substr(begin,snapshot->find('\n',begin)-begin);
        }
    }
    if(const auto order=PreUpscaleOverride(processingScale,current))
        overrides.emplace_back("NRPreUpscale",std::string(*order));
    return overrides;
}

// ---- What refuses a neural pass before a helper is asked for one --------
//
// Shared by the live render (NeuralJobRun) and the stage export, in the words
// the live path has always used, so a runtime one of them refuses is never
// rendered with by the other. The export used to check neither: it wrote the
// settings and ran the helper against whatever the directory held, and a
// drifted runtime the live path refused still produced a file called neural.

// Empty when every locked file matches.
static std::wstring RuntimeLockRefusal(std::span<const RuntimeLockCheck> checks){
    if(RuntimeLockSatisfied(checks))return {};
    return L"The neural runtime does not match the locked stack: "+DescribeRuntimeLockDrift(checks);
}

// Empty when the directory holds no module the lock does not name, and when it
// cannot be listed: the helper refuses that itself, with its own reason.
static std::wstring UnlockedRuntimeModulesRefusal(const std::filesystem::path& runtimeDirectory,const RuntimeLock& lock){
    const auto unlocked=FindUnlockedRuntimeModules(runtimeDirectory,lock);
    return unlocked?runtime_modules::UnlockedModulesRefusal(*unlocked):std::wstring{};
}

// Both, the lock first, which is the order a live render meets them in. Empty
// when neither refuses. A cancelled check leaves hashes unverified and so
// reads as drift; the caller asks its stop token before believing it.
static std::wstring StageExportRuntimeRefusal(const std::filesystem::path& runtimeDirectory,const RuntimeLock& lock,std::stop_token stop){
    const auto checks=VerifyRuntimeLock(runtimeDirectory,lock,stop);
    if(std::wstring refusal=RuntimeLockRefusal(checks);!refusal.empty())return refusal;
    return UnlockedRuntimeModulesRefusal(runtimeDirectory,lock);
}

// ---- Export with DLSS stages, the passes themselves --------------------
//
// Shared by the dialog and by `--render`, so a script gets the file the dialog
// would have written rather than a second implementation of it that drifts.
// Everything the two callers differ in - where progress goes, how the outcome
// is shown, whether a range was asked for - is a parameter.
struct StageExportJob {
    ExportPlan plan;
    std::filesystem::path source;
    std::filesystem::path destination;
    // Where the intermediate passes are written: the cache root's
    // export-stages directory, beside the other derived carriers.
    std::filesystem::path scratch;
    // The player's directory: ffmpeg beside it, the helper in neural-runtime.
    std::filesystem::path helpers;
    uint32_t sourceWidth{},sourceHeight{};
    double fps{},duration{};
    // FrameGenerationRequest::holdDuplicates for the frame-generation pass.
    bool holdDuplicates{};
    // Whole for the dialog. A range reaches the worker pass only; frame
    // generation then reads that pass's carrier, which covers just the range.
    NeuralRenderRange range{};
    uint32_t nvencPreset{5};
    // The model's resolution for a neural pass at the source size. An export
    // that upscales runs the model on the upscaled frame, as it always has,
    // whatever this says: a reduced model input and a Super Resolution output
    // are one carrier's two jobs, and it can only do one of them.
    uint32_t processingScale{kDefaultProcessingScale};
    // Super Resolution's history for an upscaling pass without the model; a pass
    // that runs the model keeps Temporal (CarrierUpscalingHistory).
    UpscalingHistory upscalingHistory{kRecommendedUpscalingHistory};
    // Written to the add-on before a neural pass. The dialog's tooltip has
    // always said the neural stage "runs the neural model with the settings
    // from Neural settings", but nothing wrote them: the export used whatever
    // the last live render had left in ReShade.ini.
    NeuralSettings neuralSettings{};
    // The capture-quality switches of Encoder settings, so the export's neural pass
    // writes the same way the cache does.
    bool captureDither{true};
    EncoderQuality quality{EncoderQuality::Standard};
    bool sourceDeband{false};
    bool suppliedExposure{false};
    // Ends the player's idle resident helper before this export's helper
    // starts in the same runtime directory. Empty for `--render`, which runs in
    // a process of its own and has none.
    std::function<void()> releaseResidentHelper;
};

// `passKey` null is the end of the passes, when the finished file is moved
// into place.
struct StageExportUpdate {
    uint32_t pass{},passes{};
    const wchar_t* passKey{};
    uint64_t completedFrames{},totalFrames{};
};

enum class StageExportStatus { Done, Refused, Failed, Cancelled };

struct StageExportOutcome {
    StageExportStatus status{StageExportStatus::Failed};
    std::wstring detail;
};

static StageExportOutcome RunStageExport(const StageExportJob& job,std::stop_token stop,
                                         const std::function<void(const StageExportUpdate&)>& progress){
    const ExportPlan& plan=job.plan;
    const uint64_t tag=GetTickCount64();
    const auto stageOne=job.scratch/(L"stage1-"+std::to_wstring(tag)+L".mkv");
    const auto stageTwo=job.scratch/(L"stage2-"+std::to_wstring(tag)+L".mkv");
    std::filesystem::path produced=job.source;
    // Both intermediates, always: the last step writes the destination from
    // them rather than renaming one into place, so the one it read is as
    // spent as the other. The guard that used to keep `produced` also kept
    // the first pass's carrier when frame generation failed after it.
    const auto sweep=[&]{std::error_code ec;
        std::filesystem::remove(stageOne,ec);
        std::filesystem::remove(stageTwo,ec);};
    const auto report=[&](StageExportUpdate update){if(progress)progress(update);};
    // Asked before the passes rather than by the last step after them: the
    // file is replaced, and a source replaced by its own export is gone.
    {std::error_code sameError;
        if(std::filesystem::equivalent(job.source,job.destination,sameError)&&!sameError)
            return {StageExportStatus::Refused,L"The export cannot replace its own source. Choose a new filename."};}
    const uint32_t passes=ExportStageCount(plan);
    if(plan.workerStage){
        NeuralRenderRequest request{};
        request.sourcePath=produced;request.stagingVideoPath=stageOne;
        request.width=job.sourceWidth;request.height=job.sourceHeight;
        request.fps=job.fps;request.durationSeconds=job.duration;
        request.range=job.range;
        request.nvencPreset=job.nvencPreset;
        request.captureDither=job.captureDither;
        // The stage export writes at the same rung the cache does.
        request.quality=job.quality;
        request.sourceDeband=job.sourceDeband;
        request.suppliedExposure=job.suppliedExposure;
        request.requireNeural=plan.requireNeural;
        const bool upscales=plan.outputWidth!=job.sourceWidth||plan.outputHeight!=job.sourceHeight;
        if(upscales){
            request.outputWidth=plan.outputWidth;request.outputHeight=plan.outputHeight;
        }
        if(plan.requireNeural&&!upscales)request.processingScale=job.processingScale;
        if(upscales)request.upscalingHistory=CarrierUpscalingHistory(job.upscalingHistory,plan.requireNeural);
        const wchar_t* passKey=plan.requireNeural
            ?(plan.outputWidth!=job.sourceWidth?L"export.progress.pass_sr_neural":L"export.progress.pass_neural")
            :L"export.progress.pass_sr";
        const auto runtimeDirectory=job.helpers/L"neural-runtime";
        // A neural pass is refused on what refuses a live render, before
        // anything is written: a file that drifted from the lock, or a module
        // the lock does not name beside feature 18. A Super Resolution-only
        // pass is not presented as neural, and the helper still refuses a
        // stray module at its own startup.
        if(plan.requireNeural){
            const std::wstring refusal=StageExportRuntimeRefusal(runtimeDirectory,EmbeddedRuntimeLock(),stop);
            if(stop.stop_requested())return {StageExportStatus::Cancelled,{}};
            if(!refusal.empty()){
                LOG("Stage export refused before the helper: "<<WideToUtf8(refusal));
                return {StageExportStatus::Refused,refusal};
            }
        }
        // One writer at a time, exactly as a live render: the settings written
        // below and the helper's proxy log are shared per runtime directory, and
        // `--render` can run beside a player that is rendering.
        NeuralRuntimeLease runtimeLease(runtimeDirectory);
        if(!runtimeLease.Held())
            return {StageExportStatus::Refused,L"Another neural render is using the experimental runtime. Wait for it to finish, then try again."};
        // An idle resident helper from an earlier live job still holds the
        // device, its feature-18 workset and the runtime's ReShade.log. A
        // second helper beside it risks the VRAM a small card does not have,
        // and moves the proxy's log to ReShade.log1 where the evidence reader
        // may not look. It goes first, as it does before a preflight probe -
        // under the lease, which every job thread that uses it also holds.
        if(job.releaseResidentHelper)job.releaseResidentHelper();
        // The add-on state the job needs, with the neural settings when it runs
        // the model. The helper checks the same state itself and relaunches when
        // it had to change it; writing it here first saves that relaunch.
        const auto overrides=plan.requireNeural
            ?RenderAddonOverrides(runtimeDirectory/L"ReShade.ini",job.neuralSettings,request.processingScale)
            :std::vector<NeuralAddonOverride>{};
        const auto configured=ConfigureNeuralAddon(runtimeDirectory/L"ReShade.ini",plan.requireNeural,overrides);
        if(!configured.ok){
            LOG("Stage export could not prepare the neural add-on: "<<WideToUtf8(configured.error));
            return {StageExportStatus::Failed,L"The neural settings could not be prepared."};
        }
        report({1,passes,passKey,0,0});
        const NeuralRenderResult result=RunNeuralWorker(job.helpers/L"neural-runtime"/L"NeuralWorker.exe",request,
            [&](const NeuralRenderProgress& p){report({1,passes,passKey,p.completedFrames,p.totalFrames});},stop);
        if(!result.ok){
            sweep();
            return {result.cancelled?StageExportStatus::Cancelled:StageExportStatus::Failed,result.detail};
        }
        produced=stageOne;
    }
    if(plan.frameGenStage){
        FrameGenerationRequest request{};
        request.source=produced;
        // Video only: the last step below attaches the original's streams to
        // whatever the passes produced, trimmed to the range.
        request.carryStreams=false;
        request.output=stageTwo;
        request.multiplier=plan.multiplier;
        request.nvencPreset=job.nvencPreset;
        request.holdDuplicates=job.holdDuplicates;
        const uint32_t generatePass=plan.workerStage?2u:1u;
        report({generatePass,passes,L"export.progress.pass_framegen",0,0});
        const FrameGenerationResult result=FrameGenerationPass(job.helpers).Run(request,stop,
            [&](const FrameGenerationProgress& p){report({generatePass,passes,L"export.progress.pass_framegen",p.sourceFramesRead,p.sourceFramesTotal});});
        if(!result.ok){
            sweep();
            return {result.error==FrameGenerationError::Cancelled?StageExportStatus::Cancelled:StageExportStatus::Failed,result.detail};
        }
        produced=stageTwo;
    }
    // Every pass writes Matroska. This used to be renamed onto the chosen name
    // whatever it was, so "clip.mp4" was a Matroska file under an MP4 name;
    // the last step now writes the container the name asks for, keeping the
    // video bitstream as the passes encoded it.
    //
    // It is also where the original's audio, subtitles and chapters come in,
    // for every combination of stages. The neural worker writes its carrier
    // video-only, so an export without frame generation used to be silent,
    // and so was one with a range, because frame generation copied streams
    // from that carrier. Every pass keeps the length of what it read, so the
    // streams need no retime - only the trim a ranged carrier needs, which
    // is the cached-range export's.
    StageExportMuxRequest finish{produced,job.source,job.destination};
    if(!job.range.Whole()){
        finish.rangeStartSeconds=double(job.range.start100ns)*1e-7;
        if(job.range.end100ns>job.range.start100ns)finish.rangeDurationSeconds=double(job.range.end100ns-job.range.start100ns)*1e-7;
    }
    report({});
    const MaterializeResult finished=MuxStageExport(job.helpers,finish,stop);
    sweep();
    if(!finished.ok){
        if(finished.error==MaterializeError::Cancelled)return {StageExportStatus::Cancelled,{}};
        LOG("Stage export could not write "<<WideToUtf8(job.destination.wstring())<<": "<<WideToUtf8(finished.detail));
        return {StageExportStatus::Failed,finished.detail};
    }
    LOG("Stage export wrote "<<WideToUtf8(job.destination.wstring())<<(finished.detail.empty()?"":" - ")<<WideToUtf8(finished.detail));
    return {StageExportStatus::Done,{}};
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
// Windows' media controls - the volume flyout's player card, the lock
// screen, a Bluetooth headset's buttons - through the WinRT ABI and WRL, both
// in the Windows SDK. combase is loaded at run time rather than linked, so a
// Windows without the WinRT runtime costs these controls and nothing else.
//
// ButtonPressed and PlaybackPositionChangeRequested arrive on a thread pool
// thread, so their handlers only post a message: the player acts on them on
// the UI thread, where every other command runs.
class MediaTransportControls {
public:
    MediaTransportControls()=default;
    ~MediaTransportControls(){Detach();}
    MediaTransportControls(const MediaTransportControls&)=delete;
    MediaTransportControls& operator=(const MediaTransportControls&)=delete;

    bool Attach(HWND window,UINT buttonMessage,UINT seekMessage){
        namespace media=ABI::Windows::Media;
        namespace foundation=ABI::Windows::Foundation;
        const Api& api=Functions();if(!api.ok)return false;
        const OwnedString className(L"Windows.Media.SystemMediaTransportControls");if(!className.value)return false;
        ComPtr<ISystemMediaTransportControlsInterop> interop;
        if(FAILED(api.getFactory(className.value,__uuidof(ISystemMediaTransportControlsInterop),reinterpret_cast<void**>(interop.GetAddressOf()))))return false;
        ComPtr<media::ISystemMediaTransportControls> controls;
        if(FAILED(interop->GetForWindow(window,__uuidof(media::ISystemMediaTransportControls),reinterpret_cast<void**>(controls.GetAddressOf()))))return false;
        controls->put_IsPlayEnabled(true);controls->put_IsPauseEnabled(true);controls->put_IsStopEnabled(true);
        controls->put_IsEnabled(false);
        const auto buttons=Microsoft::WRL::Callback<foundation::ITypedEventHandler<media::SystemMediaTransportControls*,media::SystemMediaTransportControlsButtonPressedEventArgs*>>(
            [window,buttonMessage](media::ISystemMediaTransportControls*,media::ISystemMediaTransportControlsButtonPressedEventArgs* args)->HRESULT{
                media::SystemMediaTransportControlsButton button{};
                if(args&&SUCCEEDED(args->get_Button(&button)))PostMessageW(window,buttonMessage,static_cast<WPARAM>(button),0);
                return S_OK;
            });
        if(!buttons||FAILED(controls->add_ButtonPressed(buttons.Get(),&m_buttonToken)))return false;
        m_controls=controls;
        // The seek bar in the flyout is the "timeline properties if cheap"
        // half: ISystemMediaTransportControls2 is Windows 10 1607, and without
        // it the card simply has no bar.
        if(SUCCEEDED(m_controls.As(&m_controls2))){
            const auto seeks=Microsoft::WRL::Callback<foundation::ITypedEventHandler<media::SystemMediaTransportControls*,media::PlaybackPositionChangeRequestedEventArgs*>>(
                [window,seekMessage](media::ISystemMediaTransportControls*,media::IPlaybackPositionChangeRequestedEventArgs* args)->HRESULT{
                    foundation::TimeSpan position{};
                    if(args&&SUCCEEDED(args->get_RequestedPlaybackPosition(&position))&&position.Duration>=0)
                        PostMessageW(window,seekMessage,0,static_cast<LPARAM>(position.Duration/10000));
                    return S_OK;
                });
            if(!seeks||FAILED(m_controls2->add_PlaybackPositionChangeRequested(seeks.Get(),&m_seekToken)))m_controls2.Reset();
        }
        return true;
    }
    void Detach(){
        if(m_controls2){m_controls2->remove_PlaybackPositionChangeRequested(m_seekToken);m_controls2.Reset();}
        if(m_controls){m_controls->remove_ButtonPressed(m_buttonToken);m_controls.Reset();}
    }
    bool Attached()const{return m_controls!=nullptr;}
    void SetStatus(media_transport::Status status){
        if(!m_controls)return;
        m_controls->put_IsEnabled(status!=media_transport::Status::Closed);
        m_controls->put_PlaybackStatus(static_cast<ABI::Windows::Media::MediaPlaybackStatus>(static_cast<int>(status)));
    }
    void SetTitle(const std::wstring& title){
        namespace media=ABI::Windows::Media;
        if(!m_controls)return;
        ComPtr<media::ISystemMediaTransportControlsDisplayUpdater> updater;
        if(FAILED(m_controls->get_DisplayUpdater(&updater)))return;
        if(title.empty()){updater->ClearAll();updater->Update();return;}
        updater->put_Type(media::MediaPlaybackType_Video);
        ComPtr<media::IVideoDisplayProperties> video;
        const OwnedString text(title);
        if(text.value&&SUCCEEDED(updater->get_VideoProperties(&video)))video->put_Title(text.value);
        updater->Update();
    }
    void SetTimeline(double durationSeconds,double positionSeconds){
        namespace media=ABI::Windows::Media;
        if(!m_controls2)return;
        const Api& api=Functions();
        const OwnedString className(L"Windows.Media.SystemMediaTransportControlsTimelineProperties");
        ComPtr<IInspectable> instance;
        if(!className.value||FAILED(api.activate(className.value,instance.GetAddressOf())))return;
        ComPtr<media::ISystemMediaTransportControlsTimelineProperties> timeline;
        if(FAILED(instance.As(&timeline)))return;
        const auto span=[](double seconds){return ABI::Windows::Foundation::TimeSpan{static_cast<INT64>(std::llround(std::max(0.0,seconds)*1e7))};};
        timeline->put_StartTime(span(0.0));timeline->put_EndTime(span(durationSeconds));
        timeline->put_MinSeekTime(span(0.0));timeline->put_MaxSeekTime(span(durationSeconds));
        timeline->put_Position(span(std::min(positionSeconds,durationSeconds)));
        m_controls2->UpdateTimelineProperties(timeline.Get());
    }

private:
    struct Api{
        using GetFactoryFn=HRESULT(WINAPI*)(HSTRING,REFIID,void**);
        using ActivateFn=HRESULT(WINAPI*)(HSTRING,IInspectable**);
        using CreateStringFn=HRESULT(WINAPI*)(PCNZWCH,UINT32,HSTRING*);
        using DeleteStringFn=HRESULT(WINAPI*)(HSTRING);
        GetFactoryFn getFactory{};ActivateFn activate{};CreateStringFn createString{};DeleteStringFn deleteString{};bool ok{};
    };
    static const Api& Functions(){
        static const Api api=[]{
            Api loaded{};
            if(const HMODULE combase=LoadLibraryExW(L"combase.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)){
                loaded.getFactory=reinterpret_cast<Api::GetFactoryFn>(GetProcAddress(combase,"RoGetActivationFactory"));
                loaded.activate=reinterpret_cast<Api::ActivateFn>(GetProcAddress(combase,"RoActivateInstance"));
                loaded.createString=reinterpret_cast<Api::CreateStringFn>(GetProcAddress(combase,"WindowsCreateString"));
                loaded.deleteString=reinterpret_cast<Api::DeleteStringFn>(GetProcAddress(combase,"WindowsDeleteString"));
            }
            loaded.ok=loaded.getFactory&&loaded.activate&&loaded.createString&&loaded.deleteString;
            return loaded;
        }();
        return api;
    }
    struct OwnedString{
        HSTRING value{};
        explicit OwnedString(std::wstring_view text){
            if(Functions().ok&&FAILED(Functions().createString(text.data(),static_cast<UINT32>(text.size()),&value)))value=nullptr;
        }
        ~OwnedString(){if(value)Functions().deleteString(value);}
        OwnedString(const OwnedString&)=delete;
        OwnedString& operator=(const OwnedString&)=delete;
    };
    ComPtr<ABI::Windows::Media::ISystemMediaTransportControls> m_controls;
    ComPtr<ABI::Windows::Media::ISystemMediaTransportControls2> m_controls2;
    EventRegistrationToken m_buttonToken{},m_seekToken{};
};

// A Tabler glyph as a small icon, for the taskbar thumbnail's buttons, which
// take nothing but HICONs. GDI draws no alpha, so the glyph is drawn white on
// black and its coverage becomes the alpha of a premultiplied white icon -
// the same glyph, antialiased, as the toolbar button beside it.
static HICON RenderGlyphIcon(wchar_t glyph, int size)
{
    if(size<=0||glyph==L'\0')return nullptr;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(info.bmiHeader);info.bmiHeader.biWidth=size;info.bmiHeader.biHeight=-size;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;
    HBITMAP color=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!color||!bits){if(color)DeleteObject(color);return nullptr;}
    HDC dc=CreateCompatibleDC(nullptr);
    HFONT font=CreateFontW(-size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"tabler-icons");
    HICON icon=nullptr;
    if(dc&&font){
        const HGDIOBJ oldBitmap=SelectObject(dc,color),oldFont=SelectObject(dc,font);
        RECT box{0,0,size,size};FillRect(dc,&box,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
        DrawTextW(dc,&glyph,1,&box,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        GdiFlush();
        auto* pixel=static_cast<uint32_t*>(bits);
        for(int index=0;index<size*size;++index){
            const uint32_t value=pixel[index];
            const uint32_t coverage=std::max({value&0xffu,(value>>8)&0xffu,(value>>16)&0xffu});
            pixel[index]=(coverage<<24)|(coverage<<16)|(coverage<<8)|coverage;
        }
        SelectObject(dc,oldFont);SelectObject(dc,oldBitmap);
        HBITMAP mask=CreateBitmap(size,size,1,1,nullptr);
        ICONINFO iconInfo{TRUE,0,0,mask,color};
        if(mask){icon=CreateIconIndirect(&iconInfo);DeleteObject(mask);}
    }
    if(font)DeleteObject(font);
    if(dc)DeleteDC(dc);
    DeleteObject(color);
    return icon;
}

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

// What the start screen needs from disk, gathered off the UI thread: whether
// the neural runtime is installed and matches its lock (hashing ~226 MB of
// runtime, about half a second), and for each recent video with a finished
// render, a frame of that render and how much of the video it covers. Answers
// are published one at a time into a shared slot and announced with a bare
// message, so the screen fills in as they come instead of waiting for all.
struct StartScreenRequest {
    std::filesystem::path moduleDirectory;
    std::filesystem::path cacheRoot;
    struct Recent{std::string renderKey,sourceKey;bool youtube{};};
    std::vector<Recent> recent;
    int thumbnailWidth{};
};
struct StartScreenAnswers {
    std::mutex mutex;
    uint64_t generation{};
    start_screen::RuntimeState runtime{start_screen::RuntimeState::Checking};
    std::wstring runtimeVersion;
    struct Render{std::wstring badge;std::optional<TimelineMediaWorker::Thumbnail> thumbnail;};
    std::map<std::string,Render> renders;   // by render key
};

static start_screen::RuntimeState CheckNeuralRuntime(const std::filesystem::path& moduleDirectory, std::stop_token stop)
{
    const std::filesystem::path runtime=moduleDirectory/L"neural-runtime";
    const auto file=[&](const wchar_t* name){std::error_code error;return std::filesystem::is_regular_file(runtime/name,error);};
    const NeuralRuntimeLayout layout=ClassifyNeuralRuntimeLayout(file(L"ReShade.ini"),file(L"dxgi.dll"),file(L"renodx-dlss5.addon64"),file(L"nvngx_dlssnr.dll"));
    if(layout==NeuralRuntimeLayout::Absent)return start_screen::RuntimeState::Absent;
    if(layout==NeuralRuntimeLayout::Incomplete||!file(L"NeuralWorker.exe"))return start_screen::RuntimeState::Incomplete;
    const auto checks=VerifyRuntimeLock(runtime,EmbeddedRuntimeLock(),stop);
    return RuntimeLockSatisfied(checks)?start_screen::RuntimeState::Verified:start_screen::RuntimeState::Drifted;
}

static void GatherStartScreen(StartScreenRequest request,std::shared_ptr<StartScreenAnswers> answers,uint64_t generation,
                              HWND window,UINT message,std::stop_token stop)
{
    const auto publish=[&](auto&& apply){
        {std::scoped_lock lock(answers->mutex);if(answers->generation!=generation)return false;apply(*answers);}
        PostMessageW(window,message,0,0);return true;
    };
    const auto runtime=CheckNeuralRuntime(request.moduleDirectory,stop);
    if(stop.stop_requested())return;
    const std::wstring version=Utf8ToWide(EmbeddedRuntimeLock().runtimeVersion);
    if(!publish([&](StartScreenAnswers& a){a.runtime=runtime;a.runtimeVersion=version;}))return;
    const std::filesystem::path ffmpeg=request.moduleDirectory/L"ffmpeg.exe";
    std::error_code toolError;const bool haveFfmpeg=std::filesystem::is_regular_file(ffmpeg,toolError);
    for(const auto& recent:request.recent){
        if(stop.stop_requested())return;
        // Peeked, not looked up: this is a picture on a tile, never what plays, and
        // LookupRender would hash every recent render on every start and mark each
        // one used, which reorders eviction for videos nobody opened. Opening the
        // tile runs the real lookup.
        const auto entry=NeuralCacheManager::Peek(request.cacheRoot,NeuralCacheEntryKind::Render,recent.renderKey);
        if(!entry||!IsReusableNeuralCacheManifest(entry->manifest))continue;
        const NeuralCacheManifest* manifest=&entry->manifest;
        StartScreenAnswers::Render render{};
        std::optional<int64_t> sourceDuration;
        if(recent.youtube)if(const auto source=NeuralCacheManager::Peek(request.cacheRoot,NeuralCacheEntryKind::Source,recent.sourceKey))sourceDuration=source->manifest.duration100ns;
        render.badge=start_screen::CoverageBadge(manifest->rangeStart100ns,manifest->rangeEnd100ns,manifest->duration100ns,sourceDuration);
        const auto& payload=entry->payloadPath;
        if(haveFfmpeg&&manifest->width&&manifest->height){
            const SIZE size=timeline::ThumbnailSize(double(manifest->width)/double(manifest->height),request.thumbnailWidth);
            const size_t bytes=size_t(size.cx)*size_t(size.cy)*4u;
            // A third of the way in: past a fade from black, inside the render.
            const auto output=RunToolCapture(ffmpeg,timeline::ThumbnailArguments(payload,double(manifest->duration100ns)*1e-7/3.0,size),
                                             stop,std::chrono::seconds(5),bytes);
            if(output&&output->size()==bytes)render.thumbnail=TimelineMediaWorker::Thumbnail{generation,0,size,std::vector<uint8_t>(output->begin(),output->end())};
        }
        if(!publish([&](StartScreenAnswers& a){a.renders[recent.renderKey]=std::move(render);}))return;
    }
}

// Everything a neural job reads, captured on the UI thread when it starts:
// the job thread never touches PlayerApp. The two pointers are owned by the
// player and outlive the job, which the UI thread joins before touching them.
struct NeuralJobInputs {
    HWND target{};
    uint64_t generation{};
    std::wstring mediaUrl,audioUrl,displayTitle,pageUrl;
    MediaSourceKind sourceKind{MediaSourceKind::LocalFile};
    YouTubeSourceQuality sourceQuality{YouTubeSourceQuality::Auto};
    GpuGeneration gpu{GpuGeneration::Unsupported};
    std::wstring driverVersion;
    std::filesystem::path moduleDirectory;
    CompletionRegistry<NeuralProgressMessage>* progressMessages{};
    CompletionRegistry<NeuralJobCompletion>* completions{};
    std::string reuseSourceKey;
    std::filesystem::path cacheRoot;
    double expectedDurationSeconds{};
    NeuralRenderRange range;
    GuideControls guides;
    TemporalSettings temporal;
    NeuralSettings settings;
    HANDLE pauseEvent{};
    bool prepareOnly{};
    // The background acquisition of this very source, when one is in flight.
    std::shared_ptr<SourcePrefetchState> prefetch;
    // An active session's segment index, its directory for this job and the
    // job's run id; empty for an offline render.
    std::shared_ptr<NeuralSegmentIndex> liveIndex;
    std::filesystem::path liveDirectory;
    uint64_t liveRunId{};
    uint32_t segmentFrames{},firstSegmentFrames{};
    std::wstring driverNotice;
    NeuralCacheFailureText cacheFailureText;
    NeuralPreflightKey preflightKey;
    NeuralPreflightLatch* preflightLatch{};
    ResidentNeuralHelper* residentHelper{};
    std::shared_ptr<NeuralColdStartRecord> coldStart;
    bool gpuColorConversion{},gpuSourceConversion{};
    uint32_t nvencPreset{},processingScale{};
    bool captureDither{};
    EncoderQuality cacheQuality{};
    bool sourceDeband{},suppliedExposure{};
    std::shared_ptr<SharedSourceDigest> sourceDigestMemo;
};

// One neural job on its own thread, as named steps: find the local source,
// identify it and the runtime, answer from the cache when it can, prove the
// runtime, render, and publish. Each step returns false when the job is over,
// with `completion_` saying why; Run posts the completion either way. The
// steps share what the earlier ones established through the members below,
// in the order they are set.
class NeuralJobRun {
public:
    NeuralJobRun(const NeuralJobInputs& in,std::stop_token stop):in_(in),stop_(std::move(stop)),cache_(in.cacheRoot){}
    NeuralJobRun(const NeuralJobRun&)=delete;
    NeuralJobRun& operator=(const NeuralJobRun&)=delete;

    void Run(){
        completion_=std::make_unique<NeuralJobCompletion>();completion_->generation=in_.generation;completion_->displayTitle=in_.displayTitle;completion_->pageUrl=in_.pageUrl;completion_->sourceKind=in_.sourceKind;completion_->sourceQuality=in_.sourceQuality;
        if(!cache_.Valid())completion_->result.detail=in_.cacheFailureText.DescribeRoot(cache_.LastFailure());
        else{
            if(ResolveSource()&&IdentifySource()&&PrepareRuntime()&&AnswerFromCache()&&ClearPreflight()&&Render())Publish();
            // What the steps held ends here, as it did at the end of the
            // block they were written in: the runtime lease is released
            // before the completion can start the next job.
            lease_.reset();
        }
        // A cache hit, a refusal or a prepared open never reached a
        // helper, so this is where the player's own work ends.
        in_.coldStart->MarkIfAbsent(NeuralColdStartPhase::Request);
        in_.completions->RegisterAndPost(std::move(completion_),[&](uint64_t token){return PostMessageW(in_.target,WM_NEURAL_COMPLETE,static_cast<WPARAM>(token),0)!=FALSE;});
    }

private:
    void PostProgress(const NeuralRenderProgress& progress){auto message=std::make_unique<NeuralProgressMessage>();message->generation=in_.generation;message->progress=progress;message->width=progressWidth_;message->height=progressHeight_;message->sourcePath=progressSourcePath_;message->sourceKey=progressSourceKey_;message->pageUrl=in_.pageUrl;in_.progressMessages->RegisterAndPost(std::move(message),[&](uint64_t token){return PostMessageW(in_.target,WM_NEURAL_PROGRESS,static_cast<WPARAM>(token),0)!=FALSE;});}
    bool Cancelled(){completion_->result.cancelled=true;completion_->result.detail=L"Neural render was cancelled.";return false;}

    // The local file to render: the path itself, or a stream's verified copy
    // in the cache - the one a background acquisition is writing, a recorded
    // one, or a fresh download.
    bool ResolveSource(){
        if(in_.sourceKind==MediaSourceKind::YouTube){
            std::string reuseKey=in_.reuseSourceKey;
            const auto& prefetch=in_.prefetch;
            // Wait for an acquisition of this same source whenever one is
            // in flight, not only when no key is known: the recents entry
            // can already name the key that acquisition is still writing,
            // and looking it up mid-download finds an incomplete entry. A
            // live session that hit that ended with no message at all.
            if(prefetch&&!prefetch->finished.load(std::memory_order_acquire)){
                LOG("Waiting for the background source acquisition.");
                NeuralRenderProgress acquiring{};acquiring.phase=NeuralRenderPhase::Acquiring;PostProgress(acquiring);
                while(!prefetch->finished.load(std::memory_order_acquire)&&!stop_.stop_requested())
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if(stop_.stop_requested())return Cancelled();
            if(reuseKey.empty()&&prefetch)reuseKey=prefetch->key;
            if(!reuseKey.empty()){
                const auto cached=cache_.LookupSource(reuseKey,stop_);
                // A cancelled lookup reads as a missing copy; it must not be reported as one.
                if(stop_.stop_requested())return Cancelled();
                if(cached&&cached->manifest.encoder==kCompleteSourcePolicy){
                    sourcePath_=cached->payloadPath;completion_->sourceKey=reuseKey;
                    LOG("Owned source cache verified; network resolution skipped.");
                }else if(in_.audioUrl.empty()){
                    // No stream pair in hand, so there is nothing to acquire
                    // from: the UI answers this by resolving the page again.
                    completion_->cachedSourceUnavailable=true;
                    completion_->result.detail=L"The downloaded copy of this video is no longer available.";return false;
                }else{
                    // A recorded key whose copy is gone or half-written, with
                    // the stream this session is already playing still in hand.
                    // Acquiring again beats ending the session.
                    LOG("The recorded source copy is missing or incomplete; acquiring this stream again.");
                    reuseKey.clear();
                }
            }
            if(sourcePath_.empty()){
                const SourceAcquisition acquired=AcquireYouTubeSource(cache_,in_.cacheFailureText,in_.moduleDirectory,in_.mediaUrl,in_.audioUrl,in_.pageUrl,in_.sourceQuality,in_.expectedDurationSeconds,
                    [&](const MediaDownloadProgress& download){
                        NeuralRenderProgress acquiring{};acquiring.phase=NeuralRenderPhase::Acquiring;
                        acquiring.bytes=download.bytes;acquiring.acquiredSeconds=download.seconds;
                        acquiring.expectedSeconds=in_.expectedDurationSeconds;PostProgress(acquiring);
                    },stop_);
                if(!acquired.key.empty())completion_->sourceKey=acquired.key;
                if(acquired.path.empty()){completion_->result.cancelled=acquired.cancelled;completion_->result.detail=acquired.detail;return false;}
                sourcePath_=acquired.path;
            }
        }else{
            std::error_code pathError;
            sourcePath_=std::filesystem::absolute(std::filesystem::path(in_.mediaUrl),pathError);
            if(pathError){completion_->result.detail=L"The source path could not be resolved.";return false;}
        }
        completion_->sourcePath=sourcePath_;
        // Published to the UI thread from here, which is what lets
        // playback move off a stream and onto this copy.
        progressSourcePath_=sourcePath_;progressSourceKey_=completion_->sourceKey;
        return true;
    }

    // The source's digest and what the render key needs from its metadata,
    // and the range checked against it.
    bool IdentifySource(){
        // Memoised per loaded file. Every job used to full-hash
        // the source first, including the prepare-only cache check
        // and every live retarget, which is 3-5 s of dead air on a
        // 5 GB file each time it is asked for.
        sourceDigest_=MemoisedSourceDigest(*in_.sourceDigestMemo,sourcePath_,stop_);if(!sourceDigest_){completion_->result.cancelled=stop_.stop_requested();completion_->result.detail=L"The source digest could not be computed.";return false;}
        // Metadata only: this decoder was opened and closed two lines
        // later, and a full open paid for an ffmpeg child for nothing.
        VideoDecoder metadata;if(!metadata.OpenMetadata(sourcePath_.wstring(),MediaSourceKind::LocalFile,stop_)){completion_->result.detail=L"The source could not be decoded for neural rendering.";return false;}
        width_=metadata.NativeWidth();height_=metadata.NativeHeight();fps_=metadata.FrameRate();duration_=metadata.DurationSeconds();
        // An undeclared HD source now reaches the model as BT.709
        // rather than ffmpeg's BT.601 (UntaggedColorPolicy.h), so its
        // renders carry a term that retires the ones made before.
        untaggedBt709_=metadata.DecodesUntaggedAsBt709();
        // An HDR source reaches the model tone mapped to SDR now
        // (HdrPolicy.h), for a peak read from its metadata; its
        // renders carry both, and every SDR key stays as it was.
        toneMapTerm_=metadata.ToneMapIdentityTerm();
        // A source with a display matrix reaches the model stood up now
        // (DisplayOrientationPolicy.h). A quarter turn moves the key through
        // its width and height; a half turn or a mirror keeps both, so every
        // turned source names its turn, and every upright key stays as it was.
        orientationTerm_=metadata.OrientationIdentityTerm();metadata.Close();
        if(!width_||!height_||!std::isfinite(fps_)||fps_<=0.0||!std::isfinite(duration_)||duration_<=0.0){completion_->result.detail=L"The source metadata is incomplete.";return false;}progressWidth_=width_;progressHeight_=height_;
        NeuralRenderProgress checking{};checking.phase=NeuralRenderPhase::CheckingCache;PostProgress(checking);
        const int64_t sourceDuration100ns=static_cast<int64_t>(std::llround(duration_*10000000.0));
        frameDurationTolerance_=neural_job::FrameDurationTolerance100ns(fps_);
        if(neural_job::RangeOutsideSource(in_.range,sourceDuration100ns)){completion_->result.failure=NeuralRenderFailure::Source;completion_->result.detail=L"The requested render range lies outside the source.";return false;}
        // A range render is exactly [start,end) long; a whole render matches the source.
        expectedDuration100ns_=neural_job::ExpectedDuration100ns(in_.range,sourceDuration100ns);
        return true;
    }

    // The runtime this job renders with: its digest, its lock, its lease, and
    // the add-on settings written for this job and read back as its identity.
    bool PrepareRuntime(){
        runtimeDirectory_=in_.moduleDirectory/L"neural-runtime";
        const auto runtimeStarted=std::chrono::steady_clock::now();
        runtimeDigest_=BuildRuntimeDigest(runtimeDirectory_,LockedRuntimeFileNames(),stop_);if(!runtimeDigest_){completion_->result.cancelled=stop_.stop_requested();completion_->result.detail=L"The configured neural runtime is incomplete.";return false;}
        // The lock names every drifted file; a mismatch is refused, never repaired by swapping runtimes.
        const auto lockStarted=std::chrono::steady_clock::now();
        lockChecks_=VerifyRuntimeLock(runtimeDirectory_,EmbeddedRuntimeLock(),stop_);
        if(stop_.stop_requested())return Cancelled();
        // What the cold start's `request` phase is made of, so a slow first D
        // press can be pinned on the step that took the time.
        LOG("Neural runtime identified: digest "<<std::chrono::duration_cast<std::chrono::milliseconds>(lockStarted-runtimeStarted).count()
            <<" ms, lock check "<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-lockStarted).count()<<" ms.");
        if(std::wstring refusal=RuntimeLockRefusal(lockChecks_);!refusal.empty()){LOG("Neural runtime lock drift; render refused: "<<WideToUtf8(DescribeRuntimeLockDrift(lockChecks_)));completion_->result.failure=NeuralRenderFailure::Preflight;completion_->result.detail=std::move(refusal);return false;}
        // One writer at a time: the settings written below and the
        // helper's proxy log are shared per runtime directory, so a
        // second player instance must not interleave with this job.
        lease_.emplace(runtimeDirectory_);
        if(!lease_->Held()){completion_->result.failure=NeuralRenderFailure::Preflight;completion_->result.detail=L"Another neural render is using the experimental runtime. Wait for it to finish, then try again.";LOG("Neural runtime is in use by another render; refusing to share it.");return false;}
        const auto overrides=RenderAddonOverrides(runtimeDirectory_/L"ReShade.ini",in_.settings,in_.processingScale);const auto configured=ConfigureNeuralAddon(runtimeDirectory_/L"ReShade.ini",true,overrides);
        if(!configured.ok){completion_->result.detail=L"The neural settings could not be prepared.";return false;}
        std::wstring settingsError;settingsSnapshot_=ReadNeuralAddonSettingsSnapshot(runtimeDirectory_/L"ReShade.ini",&settingsError);
        if(!settingsSnapshot_){completion_->result.detail=settingsError.empty()?L"The neural settings could not be read.":settingsError;return false;}
        settingsDigest_=Sha256Bytes(*settingsSnapshot_);if(!settingsDigest_){completion_->result.detail=L"The neural settings digest could not be computed.";return false;}
        return true;
    }

    // The render key, and a validated cache entry under it when there is one.
    // Returns false on a hit (the job is answered) and on a prepared open,
    // which stops at the lookup.
    bool AnswerFromCache(){
        const auto& range=in_.range;const auto& guides=in_.guides;const auto& temporal=in_.temporal;const auto& settings=in_.settings;
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
        const auto modelStoreStarted=std::chrono::steady_clock::now();
        const auto modelStore=ResolveNeuralModelStore(in_.driverVersion,stop_);
        LOG("Neural model store "<<NeuralModelStoreSourceName(modelStore.source)<<" in "
            <<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-modelStoreStarted).count()<<" ms files="<<modelStore.files<<" hashed="<<modelStore.contentHashedFiles<<" digest="<<modelStore.digest
            <<(NeuralModelStoreSettled(modelStore)?"":" (unsettled: the key may not match a later read)")
            <<(modelStore.recentlyWrittenFiles?" recentlyWritten="+std::to_string(modelStore.recentlyWrittenFiles):std::string{})
            <<(modelStore.trustedRewrites?" trustedRewrites="+std::to_string(modelStore.trustedRewrites):std::string{})
            <<(modelStore.reads>1?" reads="+std::to_string(modelStore.reads)+" waited="+std::to_string(modelStore.waited.count())+"ms on "+WideToUtf8(modelStore.youngestFile):std::string{}));
        identity_=NeuralCacheIdentity{*sourceDigest_,width_,height_,DLSS_VIDEO_PLAYER_VERSION,GpuGenerationPathName(in_.gpu),*runtimeDigest_,NeuralRenderPipelineIdentity(in_.gpuSourceConversion,KeyedNvencPreset(in_.nvencPreset,in_.cacheQuality),KeyedGpuColorConversion(in_.gpuColorConversion,in_.cacheQuality))+ProcessingScaleIdentityTerm(in_.processingScale)+UntaggedColorIdentityTerm(untaggedBt709_,in_.gpuSourceConversion)+toneMapTerm_+orientationTerm_+TemporalPipelineTerm(temporal)+NeuralMotionIdentityTerm(kNeuralZeroMotionTest)+CaptureQualityIdentityTerm({in_.captureDither,in_.cacheQuality,in_.sourceDeband,in_.suppliedExposure}),false,*settingsDigest_,range,guides.IsDefault()?std::string{}:CanonicalGuideControls(guides),WideToUtf8(in_.driverVersion),modelStore.digest};renderKey_=BuildNeuralCacheKey(identity_);completion_->renderKey=renderKey_;completion_->range=range;completion_->settings=settings;completion_->guides=guides;completion_->temporal=temporal;
        LOG("Checking neural cache key="<<renderKey_<<" range=["<<range.start100ns<<","<<range.end100ns<<") guides="<<CanonicalGuideControls(guides)<<" settings="<<CanonicalNeuralSettings(settings));
        if(const auto cached=cache_.LookupRender(renderKey_,stop_)){
            // LookupRender already verifies the full payload hash and
            // strict feature-18 manifest. Do not decode every frame again.
            const ProbeResult cachedProbe=ProbeMedia(in_.moduleDirectory,cached->payloadPath,stop_,MediaProbeMode::CachedMetadata);
            if(stop_.stop_requested())return Cancelled();
            // Split by what each piece of evidence needs. The
            // manifest is compared in process; everything else
            // needs ffprobe to have run. Reading a probe that
            // could not run as a probe that disagreed quarantined
            // - and then deleted - entries whose payload had just
            // been hash-verified as intact.
            const cached_render::Evidence evidence=neural_job::CachedRenderEvidence(cached->manifest,cachedProbe,
                {*sourceDigest_,*runtimeDigest_,*settingsDigest_,range,identity_.guides,width_,height_,expectedDuration100ns_,frameDurationTolerance_});
            const auto verdict=cached_render::Judge(evidence);
            const bool valid=verdict==cached_render::Verdict::Serve;
            // A validated hit is answered without a helper, so the
            // five phases a helper measures are absent by nature.
            if(valid){in_.coldStart->NoteNoHelper("cache-hit");completion_->result.ok=true;completion_->result.frameCount=cached->manifest.frameCount;completion_->result.duration100ns=cached->manifest.duration100ns;completion_->result.jobId=cached->manifest.jobId;completion_->result.historyResets=cached->manifest.historyResets;completion_->result.firstTimestamp100ns=cached->manifest.rangeStart100ns;completion_->sourcePath=sourcePath_;completion_->neuralPath=cached->payloadPath;completion_->cacheHit=true;completion_->range={cached->manifest.rangeStart100ns,cached->manifest.rangeEnd100ns};if(!cached->manifest.receiptDigest.empty()&&std::filesystem::is_regular_file(cached->directory/L"receipt.json"))completion_->receiptPath=cached->directory/L"receipt.json";return false;}
            if(verdict==cached_render::Verdict::Unverified){
                LOG("Neural cache entry "<<renderKey_<<" could not be verified because the probe did not run ("
                    <<WideToUtf8(cachedProbe.detail)<<"); it is kept and this render proceeds without it.");
            }else if(!cache_.Quarantine(*cached)){completion_->result.detail=L"The invalid neural cache entry could not be quarantined.";return false;}
        }
        // A cancelled lookup reads as a miss; it must not start a render.
        if(stop_.stop_requested())return Cancelled();
        // The cache check every open runs: no helper, and the line
        // it logs is not a render that failed to measure.
        if(in_.prepareOnly){in_.coldStart->NoteNoHelper("range-selection");completion_->preparedOnly=true;LOG("Neural cache miss; opening the original for range selection.");return false;}
        LOG("Neural cache miss or invalid entry; starting a new render.");
        // Everything above is the player's own preparation; from
        // here the cost belongs to the probe and the helper.
        in_.coldStart->Mark(NeuralColdStartPhase::Request);
        return true;
    }

    // Everything that refuses a render before a helper is asked for one: the
    // driver floor, a stray module, a latched failure, and the feature-18
    // probe itself, paid once per runtime and driver.
    bool ClearPreflight(){
        const auto& driverNotice=in_.driverNotice;const auto& preflightKey=in_.preflightKey;auto* preflightLatch=in_.preflightLatch;
        // A driver below the floor cannot create feature 18 at all,
        // so do not pay five seconds for a probe to learn that.
        if(!driverNotice.empty()){completion_->result.failure=NeuralRenderFailure::Preflight;completion_->result.detail=driverNotice;LOG("Neural render refused before the probe: "<<WideToUtf8(driverNotice));return false;}
        // A stray module beside the helper is refused by the helper at
        // startup - after a process start and, on a first run, a
        // five-second preflight. It is named here instead, in the
        // helper's own words, and never latched: the directory is
        // listed again on every attempt, so removing the file is all
        // it takes. A directory that cannot be listed is left to the
        // helper, which refuses it with its own reason.
        if(std::wstring refusal=UnlockedRuntimeModulesRefusal(runtimeDirectory_,EmbeddedRuntimeLock());!refusal.empty()){
            completion_->result.failure=NeuralRenderFailure::Preflight;completion_->result.detail=std::move(refusal);
            LOG("Neural render refused before the helper: "<<WideToUtf8(completion_->result.detail));
            return false;
        }
        // The same runtime on the same driver fails the same way:
        // probe once per configuration, not once per play and seek.
        // A failure is remembered against the module listing too: the
        // digest hashes only the locked files, so a refusal caused by
        // anything else the loader picks up from that directory stood
        // until restart even after the user removed the cause.
        const NeuralPreflightKey runtimeKey{preflightKey.gpu,preflightKey.driver,*runtimeDigest_};
        const NeuralPreflightKey failureKey{preflightKey.gpu,preflightKey.driver,
            *runtimeDigest_+"|"+WideToUtf8(runtime_modules::ModuleListingIdentity(runtimeDirectory_))};
        if(const std::wstring latched=preflightLatch->LatchedFailureDetail(failureKey);!latched.empty()){
            completion_->result.failure=NeuralRenderFailure::Preflight;completion_->result.detail=latched;
            LOG("Neural preflight skipped; this runtime and driver already failed: "<<WideToUtf8(latched));
            return false;
        }
        workerExecutable_=runtimeDirectory_/L"NeuralWorker.exe";
        // A pass is as reusable as a failure: the probe answers for a
        // GPU, a driver and a runtime, not for a playback session, and
        // paying five seconds per toggle for the same answer is what
        // made the picture take sixteen seconds to appear.
        preflightJson_=preflightLatch->LatchedSuccessJson(runtimeKey);
        if(preflightJson_.empty())preflightJson_=LoadNeuralPreflightReceipt(in_.cacheRoot,runtimeKey);
        if(!preflightJson_.empty()){
            preflightLatch->RecordSuccess(runtimeKey,preflightJson_);
            preflightJson_=MarkReusedNeuralPreflight(preflightJson_);
            LOG("Neural preflight skipped; this runtime and driver already armed feature 18.");
        }else{
            // The feature-18 probe needs the GPU; only a cache miss pays for it.
            // It also needs the runtime to itself: it loads its own proxy and
            // creates its own feature 18, and an idle resident helper from an
            // earlier job is still holding the device and the session log. That
            // helper is on its way out regardless - a probe only runs when the
            // runtime identity in its key changed.
            in_.residentHelper->Release();
            NeuralRenderProgress preflighting{};preflighting.phase=NeuralRenderPhase::Preflight;PostProgress(preflighting);
            const NeuralPreflightResult preflight=RunNeuralPreflight(workerExecutable_,stop_);
            // Marked before the verdict is read: a probe that failed
            // or was cancelled still cost what it cost.
            in_.coldStart->Mark(NeuralColdStartPhase::Preflight);
            if(preflight.cancelled||stop_.stop_requested())return Cancelled();
            if(!preflight.ok){completion_->result.failure=NeuralRenderFailure::Preflight;completion_->result.detail=preflight.detail.empty()?L"The neural runtime preflight failed.":preflight.detail;preflightLatch->RecordFailure(failureKey,completion_->result.detail);LOG("Neural preflight failed: cause="<<NeuralPreflightCauseName(preflight.cause)<<" "<<WideToUtf8(completion_->result.detail)<<(preflight.json.empty()?"":" receipt=")<<preflight.json);return false;}
            preflightLatch->RecordSuccess(runtimeKey,preflight.json);
            StoreNeuralPreflightReceipt(in_.cacheRoot,runtimeKey,preflight.json);
            preflightJson_=preflight.json;
        }
        return true;
    }

    // The render itself, in the staging directory under the key, by the
    // resident helper (launched or reused). An active session's segments are
    // published into its index as the helper finalizes them.
    bool Render(){
        const auto& liveIndex=in_.liveIndex;const auto& liveDirectory=in_.liveDirectory;const uint64_t liveRunId=in_.liveRunId;const auto& coldStart=in_.coldStart;
        // The key is settled and the helper is next: from here to the first
        // frame is model preparation, which is what the warm-up line says.
        // Only while there is no rendered frame yet - the one time that line
        // is shown. A later job of the session (a hole behind the playhead)
        // posting it cost 2-3 dropped frames in 2 of 3 runs at the moment the
        // first job finished; without it, none in 3.
        if(in_.liveIndex&&in_.liveIndex->Count()==0){NeuralRenderProgress preparing{};preparing.phase=NeuralRenderPhase::NeuralRendering;PostProgress(preparing);}
        staging_=cache_.BeginRenderStaging(renderKey_);if(!staging_){completion_->result.detail=in_.cacheFailureText.Describe(cache_);return false;}
        {std::ofstream settingsFile(*staging_/L"neural-settings.ini",std::ios::binary|std::ios::trunc);settingsFile.write(settingsSnapshot_->data(),static_cast<std::streamsize>(settingsSnapshot_->size()));if(!settingsFile){cache_.MarkInvalid(*staging_);completion_->result.detail=L"The neural settings snapshot could not be staged.";return false;}}
        const auto& range=in_.range;
        NeuralRenderRequest request{nullptr,sourcePath_,liveIndex?liveDirectory/L"neural.mkv":*staging_/L"neural.mkv",width_,height_,fps_,duration_};request.jobId=in_.generation;request.range=range;request.prerollFrames=PrerollFramesFor(range,fps_);request.guides=in_.guides;request.temporal=in_.temporal;request.pauseEvent=in_.pauseEvent;request.segmentFrames=liveIndex?in_.segmentFrames:0u;request.firstSegmentFrames=liveIndex?in_.firstSegmentFrames:0u;request.gpuColorConversion=in_.gpuColorConversion;request.nvencPreset=in_.nvencPreset;request.gpuSourceConversion=in_.gpuSourceConversion;request.processingScale=in_.processingScale;request.captureDither=in_.captureDither;request.quality=in_.cacheQuality;request.sourceDeband=in_.sourceDeband;request.suppliedExposure=in_.suppliedExposure;
        receipt_.emplace(NeuralRenderReceiptInputs{preflightJson_,lockChecks_,request,{},renderKey_,*settingsDigest_,*runtimeDigest_,std::chrono::system_clock::now(),{}});
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
        hooks.progress=[this](const NeuralRenderProgress& progress){PostProgress(progress);};
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
        const auto helperKey=resident_helper::MakeHelperKey(runtimeDirectory_.wstring(),*runtimeDigest_,*settingsDigest_);
        resident_helper::HelperPlan helperPlan=resident_helper::HelperPlan::Launch;
        completion_->result=in_.residentHelper->RunJob(workerExecutable_,helperKey,request,hooks,stop_,&helperPlan);
        LOG("Neural helper plan="<<resident_helper::HelperPlanName(helperPlan)<<" resident="<<in_.residentHelper->Resident()<<".");
        // Nothing is marked finished here any more: one job fills one
        // hole, and whether the session has more to do is a question
        // about coverage, answered on the UI thread.
        // Again for a timeline that arrived too late to be reported
        // over the pipe - a crash or a cancel the launcher
        // synthesized a result for. Merging twice is idempotent.
        coldStart->Merge(completion_->result.coldStart);
        completion_->result.coldStart=coldStart->Snapshot();
        receipt_->result=completion_->result;receipt_->finished=std::chrono::system_clock::now();
        LOG("Neural render receipt: "<<SummarizeNeuralReceiptForLog(*receipt_));
        if(!completion_->result.ok){cache_.MarkInvalid(*staging_);return false;}
        return true;
    }

    // The finished render into the cache: an active session's segments
    // joined, its receipt staged beside it, the payload probed, and the entry
    // promoted only when the probe agrees with what the helper reported.
    bool Publish(){
        const auto& liveIndex=in_.liveIndex;const uint64_t liveRunId=in_.liveRunId;const auto& range=in_.range;
        const auto& moduleDirectory=in_.moduleDirectory;
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
        const std::vector<std::filesystem::path> parts=liveIndex?neural_job::SegmentsOfRun(*liveIndex,liveRunId):std::vector<std::filesystem::path>{};
        const size_t joinedParts=liveIndex?parts.size():size_t{1};
        if(liveIndex&&(parts.empty()||ConcatenateMedia(moduleDirectory,parts,*staging_/L"neural.mkv",stop_)!=EncodeError::None)){
            cache_.MarkInvalid(*staging_);completion_->result.ok=false;
            completion_->result.detail=L"The rendered segments could not be joined into a cache entry.";return false;
        }
        const double concatMs=msSince(publishStart);
        const auto finalSettings=ReadNeuralAddonSettingsSnapshot(runtimeDirectory_/L"ReShade.ini");if(!finalSettings||*finalSettings!=*settingsSnapshot_){cache_.MarkInvalid(*staging_);completion_->result.ok=false;completion_->result.detail=L"Neural settings changed during rendering. Try the render again.";return false;}
        const std::string receiptJson=BuildNeuralRenderReceiptJson(*receipt_);const auto receiptDigest=Sha256Bytes(receiptJson);
        {std::ofstream receiptFile(*staging_/L"receipt.json",std::ios::binary|std::ios::trunc);receiptFile.write(receiptJson.data(),static_cast<std::streamsize>(receiptJson.size()));if(!receiptFile||!receiptDigest){cache_.MarkInvalid(*staging_);completion_->result.ok=false;completion_->result.detail=L"The neural render receipt could not be staged.";return false;}}
        const auto probeStart=std::chrono::steady_clock::now();
        const ProbeResult probe=ProbeMedia(moduleDirectory,*staging_/L"neural.mkv",stop_);
        const double probeMs=msSince(probeStart);
        if(stop_.stop_requested()){cache_.MarkInvalid(*staging_);completion_->result.cancelled=true;completion_->result.ok=false;completion_->result.detail=L"Neural render was cancelled.";return false;}
        NeuralCacheManifest manifest{};manifest.sourceDigest=*sourceDigest_;manifest.runtimeDigest=*runtimeDigest_;manifest.encoder=neural_job::CacheEncoderName(completion_->result.encoder);manifest.width=width_;manifest.height=height_;manifest.frameCount=completion_->result.frameCount;manifest.duration100ns=completion_->result.duration100ns;manifest.nativeEvaluations=completion_->result.nativeEvaluations;manifest.verifiedNeuralFrames=completion_->result.verifiedNeuralFrames;manifest.observedFeature18Evaluations=completion_->result.evidence.highestObservedEvaluation;manifest.feature18Created=completion_->result.evidence.feature18Created;manifest.feature18ArmedBeforeCapture=completion_->result.feature18ArmedBeforeCapture;manifest.upscaling=false;
        manifest.settingsDigest=*settingsDigest_;manifest.rangeStart100ns=range.start100ns;manifest.rangeEnd100ns=range.end100ns;manifest.guides=identity_.guides;manifest.jobId=in_.generation;manifest.historyResets=completion_->result.historyResets;manifest.receiptDigest=*receiptDigest;
        // The key's environment terms, so eviction can tell this entry from one a driver
        // update, a model refresh or an upgrade of this installation has orphaned.
        manifest.environment=NeuralCacheEnvironmentFor(identity_);
        const int64_t joinedDurationTolerance=JoinedMediaDurationTolerance100ns(fps_,joinedParts);
        const bool probeMatches=neural_job::PublishedProbeMatches(probe,width_,height_,completion_->result.frameCount,completion_->result.duration100ns,expectedDuration100ns_,joinedDurationTolerance);
        NeuralCacheManifest publishCandidate=manifest;publishCandidate.kind=NeuralCacheEntryKind::Render;publishCandidate.state=NeuralCacheState::Complete;publishCandidate.neuralDigest=std::string(64,'0');
        const bool manifestReusable=IsReusableNeuralCacheManifest(publishCandidate);
        const bool gate=CanPublishNeuralCompletion(completion_->result.ok,probeMatches,manifestReusable);
        NeuralCachePromotion promotion{};
        const auto promoteStart=std::chrono::steady_clock::now();
        const bool published=gate&&cache_.PromoteRender(renderKey_,*staging_,manifest,&promotion);
        LOG("Neural publish timing: parts="<<joinedParts<<" concatMs="<<concatMs<<" probeMs="<<probeMs
            <<" promoteMs="<<msSince(promoteStart)<<" totalMs="<<msSince(publishStart)<<".");
        if(!published){
            // This gate discarded a finished render once and left nothing to diagnose it
            // with; then it did it again for a rename an antivirus scan was holding, and
            // the numbers below all agreed. Both halves of the verdict are named now.
            LOG("Neural publish refused: gate="<<gate<<" promoteStage="<<NeuralCachePromotionStageName(promotion.stage)
                <<" promoteError="<<promotion.win32Error<<" renameAttempts="<<promotion.attempts
                <<" renderOk="<<completion_->result.ok<<" manifestReusable="<<manifestReusable<<" probeOk="<<probe.ok<<" probe="<<probe.width<<"x"<<probe.height<<" expected="<<width_<<"x"<<height_<<" probeFrames="<<probe.frameCount<<" resultFrames="<<completion_->result.frameCount<<" probeDuration="<<probe.duration100ns<<" resultDuration="<<completion_->result.duration100ns<<" expectedDuration="<<expectedDuration100ns_<<" tolerance="<<joinedDurationTolerance<<" parts="<<joinedParts<<".");
            if(!cache_.MarkInvalid(*staging_))LOG("The refused staging directory could not be set aside either: "<<WideToUtf8(staging_->wstring()));
            completion_->result.ok=false;completion_->result.detail=L"The neural video failed final cache validation.";return false;
        }
        if(promotion.attempts>1)LOG("Neural cache entry published after "<<promotion.attempts<<" rename attempts; the entry was held by another process.");
        if(promotion.entry){completion_->neuralPath=promotion.entry->payloadPath;completion_->receiptPath=promotion.entry->directory/L"receipt.json";}else{completion_->result.ok=false;completion_->result.detail=L"The neural cache entry could not be reopened.";}
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
        return true;
    }

    const NeuralJobInputs& in_;
    std::stop_token stop_;
    NeuralCacheManager cache_;
    std::unique_ptr<NeuralJobCompletion> completion_;
    // What every progress post carries: the geometry once the source is
    // read, and the local source the moment the job knows it, so playback
    // can leave the stream.
    uint32_t progressWidth_=0,progressHeight_=0;
    std::filesystem::path progressSourcePath_;
    std::string progressSourceKey_;
    // ResolveSource
    std::filesystem::path sourcePath_;
    // IdentifySource
    std::optional<std::string> sourceDigest_;
    uint32_t width_{},height_{};
    double fps_{},duration_{};
    bool untaggedBt709_{};
    std::string toneMapTerm_;
    std::string orientationTerm_;
    int64_t frameDurationTolerance_{},expectedDuration100ns_{};
    // PrepareRuntime
    std::filesystem::path runtimeDirectory_;
    std::optional<std::string> runtimeDigest_;
    std::vector<RuntimeLockCheck> lockChecks_;
    std::optional<NeuralRuntimeLease> lease_;
    std::optional<std::string> settingsSnapshot_,settingsDigest_;
    // AnswerFromCache
    NeuralCacheIdentity identity_{};
    std::string renderKey_;
    // ClearPreflight
    std::filesystem::path workerExecutable_;
    std::string preflightJson_;
    // Render
    std::optional<std::filesystem::path> staging_;
    std::optional<NeuralRenderReceiptInputs> receipt_;
};

class PlayerApp {
#ifdef PLAYER_APP_TESTING
    friend struct PlayerAppTestAccess;
#endif
public:
    explicit PlayerApp(AppOptions o):m_opt(std::move(o)),m_youtubeSourceQuality(YouTubeSourceQuality::Auto),m_neuralPauseEvent(CreateEventW(nullptr,TRUE,FALSE,nullptr)){}
    ~PlayerApp(){if(m_activityTimer&&m_hwnd)KillTimer(m_hwnd,m_activityTimer);CancelExport();CancelFrameGeneration();CancelNeuralJob(false);CancelYouTubeResolution(false);SaveVideoSettings();if(m_adjustWnd)DestroyWindow(m_adjustWnd);if(m_neuralWnd)DestroyWindow(m_neuralWnd);if(m_encoderWnd)DestroyWindow(m_encoderWnd);UnregisterOverlayHotkeys();Unload(); if(m_font)DeleteObject(m_font); if(m_fontSmall)DeleteObject(m_fontSmall); if(m_fontTitle)DeleteObject(m_fontTitle); if(m_iconFont)DeleteObject(m_iconFont); if(m_neuralPauseEvent)CloseHandle(m_neuralPauseEvent);for(const auto& icon:m_thumbIcons)DestroyIcon(icon.second);}

    bool Create(HINSTANCE hi) {
        if(!m_uiResources.Load(hi))LOG("Embedded Tabler icon font unavailable; continuing with label-only controls.");
        LoadVideoSettings();
        NeuralCacheManager historyCache(m_cacheRoot);
        if(historyCache.Valid()){
            m_cacheRoot=historyCache.Root();
            SaveCacheSettings();
            // Beside the cache, so a start's first D press already knows which
            // model-store bytes it may trust through NGX's rewrites.
            UseNeuralModelStoreMemo(m_cacheRoot/L"model-store-memo.txt");
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
        // The first window is sized from the work area (initial_window): 80%
        // of it, shaped for a 16:9 picture above the chrome, at the monitor's
        // dpi. A fixed 1440x880 of client was 823x503 dip at 175%, where every
        // pill dropped to an icon. The work area is also the ceiling: a frame
        // taller than it hangs the bottom 50 dip - the status line and the
        // whole seek bar - under the taskbar, and the user loses scrubbing.
        const UINT startDpi=ActiveWindowDpi(nullptr);
        const auto dipAtStart=[&](int value){return MulDiv(value,int(startDpi),USER_DEFAULT_SCREEN_DPI);};
        RECT nonClient{0,0,0,0};
        {
            using AdjustForDpiFn=BOOL(WINAPI*)(LPRECT,DWORD,BOOL,DWORD,UINT);
            static const auto adjustForDpi=reinterpret_cast<AdjustForDpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"AdjustWindowRectExForDpi"));
            if(!adjustForDpi||!adjustForDpi(&nonClient,WS_OVERLAPPEDWINDOW,TRUE,WS_EX_ACCEPTFILES,startDpi))AdjustWindowRectEx(&nonClient,WS_OVERLAPPEDWINDOW,TRUE,WS_EX_ACCEPTFILES);
        }
        const SIZE frameExtra{nonClient.right-nonClient.left,nonClient.bottom-nonClient.top};
        RECT rc{0,0,1440+frameExtra.cx,880+frameExtra.cy};
        RECT work{};
        int windowX=CW_USEDEFAULT,windowY=CW_USEDEFAULT;
        if(SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0)){
            const LONG workWidth=work.right-work.left,workHeight=work.bottom-work.top;
            if(workWidth>0&&workHeight>0){
                const SIZE client=initial_window::ClientSize({SIZE{workWidth,workHeight},frameExtra,
                    dipAtStart(CONTROL_H_DIP+compare_bar::kBarHeightDip),
                    SIZE{MinimumToolbarClientWidth(startDpi),MinimumIdleClientHeight(startDpi)},
                    FullPillToolbarClientWidth(startDpi)});
                rc=RECT{0,0,client.cx+frameExtra.cx,client.cy+frameExtra.cy};
                LOG("First window: "<<client.cx<<"x"<<client.cy<<" client at "<<startDpi<<" dpi in a "<<workWidth<<"x"<<workHeight<<" work area.");
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
        RestoreWindowPlacement();
        ReadAnimationPreference();
        app_menu::UpdateYouTubeQualitySelection(GetMenu(m_hwnd),m_youtubeSourceQuality);
        UpdateRecentMenu();
        LoadUpdateSettings();
        MaybeStartUpdateCheck(false);
        RegisterOverlayHotkeys();
        LOG("System media transport controls "<<(m_mediaTransport.Attach(m_hwnd,WM_MEDIA_BUTTON,WM_MEDIA_SEEK)?"attached.":"unavailable; the media keys still reach the player through its hotkey."));
        BOOL dark=TRUE; DwmSetWindowAttribute(m_hwnd,20,&dark,sizeof(dark)); DWORD corner=2; DwmSetWindowAttribute(m_hwnd,33,&corner,sizeof(corner));
        m_viewport=CreateWindowExW(0,v.lpszClassName,nullptr,WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,100,100,m_hwnd,nullptr,hi,nullptr);
        m_renderWnd=CreateWindowExW(WS_EX_ACCEPTFILES,L"DLSSVideoRenderClassV11",nullptr,WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,0,0,100,100,m_viewport,nullptr,hi,this);
        UpdateFontsForDpi(ActiveWindowDpi(m_hwnd));
        DragAcceptFiles(m_hwnd,TRUE); DragAcceptFiles(m_renderWnd,TRUE); ShowWindow(m_viewport,SW_HIDE); Layout(); UpdateTitle();
        StartCacheEviction();
        if(!m_opt.file.empty()){if(IsSupportedYouTubeUrl(m_opt.file))StartYouTubeResolution(m_opt.file,L"",m_youtubeSourceQuality);else Load(m_opt.file);} // No startup file picker: the player opens idle by default.
        SyncStartScreen();
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
                // The model-store term is judged by two settled reads taken
                // kEvictionModelStoreGap apart that agree (NeuralModelStoresAgree).
                // One read is one sample of a directory NGX rewrites on every
                // initialisation, the player's own included: a read that caught
                // nvngx_server_config.txt truncated keyed a render under a digest
                // no later read reproduced, and this pass then deleted renders as
                // "retired by a changed model store" when nothing had changed. A
                // wrong current deletes renders that still work; a missed one
                // leaves orphans for the next start or the free-space floor.
                const NeuralModelStore models=ResolveNeuralModelStore(driverVersion);
                NeuralModelStore confirm;
                if(runtime&&NeuralModelStoreSettled(models)){
                    std::this_thread::sleep_for(kEvictionModelStoreGap);
                    confirm=ResolveNeuralModelStore(driverVersion);
                }
                // The agreed pair is also the only thing the model-store memo
                // learns from (ModelStoreMemo): one read can be torn, two agreeing
                // 5 s apart are the store.
                RememberAgreedNeuralModelStores(models,confirm);
                if(runtime&&NeuralModelStoresAgree(models,confirm))
                    current=cache_eviction::Identity{DLSS_VIDEO_PLAYER_VERSION,NeuralCacheInstallation(),*runtime,WideToUtf8(driverVersion),models.digest};
                else LOG("Cache eviction is not judging entries by identity: runtime="<<(runtime?"resolved":"unavailable")
                         <<" modelStore="<<(!NeuralModelStoreSettled(models)?"unsettled":
                                            !NeuralModelStoreSettled(confirm)?"unsettled on the second read":"changed between reads")
                         <<" first="<<models.digest<<" second="<<confirm.digest<<".");
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
        if(auto passthrough=Audio().PassthroughStatus();!(passthrough==m_shownPassthrough)){
            m_shownPassthrough=std::move(passthrough);UpdateCachedStatus();}
        UpdateLiveSession();
        WatchNeuralJobProgress();
        SyncHdrPresentation();
        if(playback_tick::PerformsPendingSeek(TickNow())) {
            const double target=m_pendingSeekSec; const bool resume=m_seekResumePlaying;
            m_seekPending=false; PerformSeek(target,resume); return;
        }
        // Ahead of both presents below, so a subtitle that changed is in the
        // frame or the paused re-present this tick makes.
        ServiceSubtitles();
        // A paused frame is presented when something invalidated it - a window
        // resize or repaint, or a renderer setting the present pass reads - and
        // not otherwise. It used to be re-presented at 60 Hz whether or not
        // anything had changed: a full-screen draw and a Present per 16 ms of
        // every pause, for an image the compositor already holds.
        if(playback_tick::PresentsPausedFrame(TickNow(),static_cast<bool>(m_renderer),m_staticPresentPending,m_renderer&&m_renderer->PresentationStale())){
            m_staticPresentPending=false;++m_staticPresents;
            if(!m_renderer->PresentCurrent()&&RecoverUnusableRenderer())return;
        }
        // Before every early return below, because the state worth reporting is
        // the one where nothing is reaching the screen. Reporting from the
        // present path instead meant the worst case - no frames presented at
        // all - was the quietest: one measured session logged a single line
        // covering 132 s and two presented frames.
        ReportPlaybackHealth();
        if(playback_tick::ReadsCachedAhead(TickNow())){
            if(!ReadNextCachedFrame())return;
        }
        if(playback_tick::ReadsNetworkAhead(TickNow())){
            if(ApplyNetworkRead(m_decoder.ReadNextAvailable(m_next),NetworkReadPosition::BeforeRender)!=NetworkReadAction::UseFrame)return;
        }
        if(!playback_tick::AdvancesFrame(TickNow())) return;
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
            if(!playback_tick::IsLate(now,due,frameDur)) break;
            VideoFrame skip=std::move(m_next); (void)skip; ++m_droppedFrames; dropped=true;
            if(NetworkPlayback()){if(ApplyNetworkRead(m_decoder.ReadNextAvailable(m_next),NetworkReadPosition::BeforeRender)!=NetworkReadAction::UseFrame)break;}
            else if(!readLocal()){m_haveNext=false;break;}
        }
        }
        if(dropped){m_guides.Reset();m_guideReset=true;m_dlssReset=true;}
        if(!m_haveNext){if(playback_tick::EndsPlaybackWhenQueueEmpty(NetworkPlayback())){m_playing=false;Audio().Pause(true);}InvalidateControls();InvalidatePlaybackProgress();UpdateCachedStatus();return;}
        const VideoFrame& next=NextFrame();
        double due=double(next.timestamp100ns)*1e-7;
        if(playback_tick::IsEarly(now,due)) return;
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
    DWORD TickSleepMs()const{return playback_tick::SleepMs(TickNow());}
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
    // The press-and-hold A/B: fires once, compare_gesture::kHoldMs after a press on the
    // picture that has not become a drag.
    static constexpr UINT_PTR kPeekTimerId=0xD15A;
    // The toolbar's hover fades; runs only while a fade is moving.
    static constexpr UINT_PTR kHoverTimerId=0xD15B;
    // The render-complete glow along the timeline; runs for its 900 ms only.
    static constexpr UINT_PTR kGlowTimerId=0xD15C;
    // A toast's next change: every frame while it moves, the hold's end while it sits.
    static constexpr UINT_PTR kToastTimerId=0xD15D;
    // The render band's reveal of a landed segment; 180 ms at a time.
    static constexpr UINT_PTR kBandTimerId=0xD15E;
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
    // Playback > Audio > Passthrough to receiver. Local playback only: the
    // network player is built on the resolver's worker with passthrough off,
    // and a stream's Opus or AAC would never qualify anyway. A change restarts
    // audio where the viewer is, like a track change, so it applies at once.
    void SetAudioPassthrough(bool enabled){
        if(m_audioPassthrough==enabled)return;
        m_audioPassthrough=enabled;m_audio.SetPassthrough(enabled);
        LOG("Audio passthrough "<<(enabled?"on":"off")<<".");
        SaveVideoSettings();SyncFeatureMenuState();
        if(m_loaded&&!NetworkPlayback()&&m_audio.Active())m_audio.Seek(Position());
        UpdateCachedStatus();
    }
    // What the status line says about passthrough: nothing while it is off,
    // and otherwise whether the bitstream is going out or why it is not.
    std::wstring AudioPassthroughNote()const{
        const audio_passthrough::Status status=Audio().PassthroughStatus();
        const wchar_t* key=audio_passthrough::StatusKey(status.state);
        if(!key)return{};
        std::wstring subject=audio_passthrough::CodecLabel(status.codec);
        // One of ours at a rate nothing carries, rather than another codec.
        if(status.state==audio_passthrough::State::NotApplicable&&status.codec!=audio_passthrough::Codec::None)
            key=L"audio.passthrough.unsupported_rate";
        else if(status.state==audio_passthrough::State::NotApplicable){
            const std::string codec=audio_track::detail::CodecName(status.trackCodec);
            subject=codec.empty()?T(L"audio.passthrough.unknown_track"):std::wstring(codec.begin(),codec.end());
        }
        return Format(T(key),subject.c_str());
    }

    // ---- Subtitles (P3.2) ----------------------------------------------------
    //
    // Drawn by an ffmpeg child (SubtitleOverlay) at the size the picture has on
    // screen, and composited by the renderer's window compositor after everything
    // else, so no subtitle can reach the cache, the model's input or an export.
    // What is chosen lives here and is served every tick from ServiceSubtitles;
    // the decisions are SubtitlePolicy.h's, and nothing below waits on a child.
    std::wstring SubtitleSourceKey()const{
        const std::wstring identity=MaskIdentity();
        return identity.empty()?std::wstring{}:compare_mask::SourceKey(identity);
    }
    std::optional<subtitle::Choice> RememberedSubtitleChoice()const{
        const std::wstring key=SubtitleSourceKey();
        if(key.empty())return std::nullopt;
        wchar_t saved[4096]{};
        GetPrivateProfileStringW(L"Subtitles",key.c_str(),L"",saved,static_cast<DWORD>(std::size(saved)),SettingsPath().c_str());
        return subtitle::ParseChoice(saved);
    }
    // Only a choice the viewer made is kept - a track, a file, off, a delay - and
    // it comes back with the source, like its mask.
    void RememberSubtitleChoice(){
        const std::wstring key=SubtitleSourceKey();
        if(key.empty())return;
        const auto path=SettingsPath();
        std::vector<wchar_t> section(65536,L'\0');
        const DWORD length=GetPrivateProfileSectionW(L"Subtitles",section.data(),static_cast<DWORD>(section.size()),path.c_str());
        std::map<std::wstring,subtitle::Choice> choices;uint64_t newest=0;
        for(size_t at=0;at<length&&section[at];){
            const std::wstring line(section.data()+at);at+=line.size()+1;
            const size_t equals=line.find(L'=');if(equals==std::wstring::npos)continue;
            if(const auto choice=subtitle::ParseChoice(std::wstring_view(line).substr(equals+1))){
                choices[line.substr(0,equals)]=*choice;newest=std::max(newest,choice->sequence);
            }
        }
        subtitle::Choice choice=m_subtitleChoice;choice.delayMs=m_subtitleDelayMs;choice.sequence=newest+1;
        if(choice.mode==subtitle::Choice::Mode::File)choice.file=m_subtitleFile;
        choices[key]=choice;
        WritePrivateProfileStringW(L"Subtitles",key.c_str(),subtitle::FormatChoice(choice).c_str(),path.c_str());
        for(const std::wstring& stale:subtitle::Evict(choices))WritePrivateProfileStringW(L"Subtitles",stale.c_str(),nullptr,path.c_str());
    }
    // A new source starts with nothing: its streams are listed on the worker and
    // the choice is made when the list arrives (TakeSubtitleDiscoveries).
    void SyncSubtitlesToSource(){
        const std::wstring source=m_loaded?m_path:std::wstring{};
        if(source==m_subtitlesForPath)return;
        m_subtitlesForPath=source;
        m_subtitleTracks.clear();m_subtitleSidecar.clear();m_subtitleOrigin=0.0;
        m_subtitleFile.clear();m_subtitleFileCodec.clear();m_subtitleFileCharenc.clear();m_subtitleProbeFile.clear();
        m_subtitleChoice={};m_subtitleDelayMs=0;m_subtitleWanted.reset();m_subtitleClock=-1.0;m_subtitleDiscovering=false;
        m_subtitles.Hide();
        if(m_renderer)m_renderer->SetSubtitleOverlay(nullptr,0,0);
        m_subtitleFrame.reset();
        if(!source.empty()){
            // A stream has no folder to look in and no container this can read
            // cheaply; only a file the viewer loaded for it before comes back.
            const bool local=_wcsnicmp(source.c_str(),L"http://",7)!=0&&_wcsnicmp(source.c_str(),L"https://",8)!=0;
            if(local){m_subtitles.Discover(source,true);m_subtitleDiscovering=true;}
            else ChooseInitialSubtitles();
        }
        UpdateSubtitleMenu();
    }
    // What a source opens with: what the viewer chose for it before, else the file
    // beside it, else the stream its container marks (subtitle::SelectDefault).
    void ChooseInitialSubtitles(){
        using Mode=subtitle::Choice::Mode;
        const auto remembered=RememberedSubtitleChoice();
        m_subtitleDelayMs=remembered?remembered->delayMs:0;
        const Mode mode=remembered?remembered->mode:Mode::Auto;
        if(mode==Mode::Off){SelectSubtitlesOff(false);return;}
        if(mode==Mode::Track&&remembered->track<int(m_subtitleTracks.size())){SelectSubtitleTrack(remembered->track,false);return;}
        std::error_code error;
        if(mode==Mode::File&&std::filesystem::is_regular_file(remembered->file,error)){LoadSubtitleFile(remembered->file,false);return;}
        if(!m_subtitleSidecar.empty()){LoadSubtitleFile(m_subtitleSidecar,false);return;}
        ChooseContainerSubtitles();
    }
    void ChooseContainerSubtitles(){
        const size_t chosen=subtitle::SelectDefault(m_subtitleTracks);
        if(chosen!=subtitle::kNoTrack)SelectSubtitleTrack(int(chosen),false);else SelectSubtitlesOff(false);
    }
    void TakeSubtitleDiscoveries(){
        while(auto found=m_subtitles.TakeDiscovery()){
            if(m_subtitleDiscovering&&found->media==m_subtitlesForPath){
                m_subtitleDiscovering=false;
                m_subtitleTracks=std::move(found->tracks);m_subtitleSidecar=found->sidecar;m_subtitleOrigin=found->origin;
                ChooseInitialSubtitles();
                UpdateSubtitleMenu();
            }else if(!m_subtitleProbeFile.empty()&&found->media==m_subtitleProbeFile){
                const bool chosenNow=m_subtitleProbeRemember;
                m_subtitleProbeFile.clear();
                if(!found->probed||found->tracks.empty()||found->tracks.front().Drawn()==subtitle::Kind::Unsupported){
                    LOG("Subtitles: the file has no subtitles this player can draw.");
                    // Picked just now: say so. Loaded by itself: the container's
                    // own choice stands instead.
                    if(chosenNow){m_sourceNotice=T(L"subtitles.failed");UpdateCachedStatus();}
                    else ChooseContainerSubtitles();
                    UpdateSubtitleMenu();
                    continue;
                }
                m_subtitleFile=found->media;m_subtitleFileCodec=found->tracks.front().codec;m_subtitleFileCharenc=found->charenc;
                SelectSubtitleFile(chosenNow);
            }
        }
    }
    void SelectSubtitlesOff(bool remember){
        if(!m_loaded)return;
        m_subtitleChoice={};m_subtitleChoice.mode=subtitle::Choice::Mode::Off;m_subtitleWanted.reset();
        if(remember)RememberSubtitleChoice();
        UpdateSubtitleMenu();
    }
    void SelectSubtitleTrack(int index,bool remember){
        if(!m_loaded||index<0||size_t(index)>=m_subtitleTracks.size())return;
        const subtitle::Track& track=m_subtitleTracks[size_t(index)];
        if(track.Drawn()==subtitle::Kind::Unsupported){LOG("Subtitles: "<<subtitle::Describe(track)<<" cannot be drawn.");return;}
        m_subtitleChoice={};m_subtitleChoice.mode=subtitle::Choice::Mode::Track;m_subtitleChoice.track=index;
        SubtitleOverlay::Source source;
        source.path=m_path;source.stream=index;source.codec=track.codec;source.origin=m_subtitleOrigin;
        m_subtitleWanted=source;
        LOG("Subtitles: showing "<<subtitle::Describe(track)<<'.');
        if(remember)RememberSubtitleChoice();
        UpdateSubtitleMenu();
    }
    // A file is probed on the worker first - for what it holds and how its text
    // is encoded - and shown when the answer arrives.
    void LoadSubtitleFile(const std::wstring& path,bool chosenNow){
        if(!m_loaded||path.empty())return;
        m_subtitleProbeFile=path;m_subtitleProbeRemember=chosenNow;
        m_subtitles.Discover(path,false);
    }
    void SelectSubtitleFile(bool remember){
        if(!m_loaded||m_subtitleFile.empty())return;
        m_subtitleChoice={};m_subtitleChoice.mode=subtitle::Choice::Mode::File;m_subtitleChoice.file=m_subtitleFile;
        SubtitleOverlay::Source source;
        source.path=m_subtitleFile;source.external=true;source.codec=m_subtitleFileCodec;source.charenc=m_subtitleFileCharenc;
        m_subtitleWanted=source;
        LOG("Subtitles: showing a subtitle file ("<<m_subtitleFileCodec<<").");
        if(remember)RememberSubtitleChoice();
        UpdateSubtitleMenu();
    }
    void LoadSubtitleFromDialog(){
        if(!m_loaded)return;
        const auto path=PickSubtitleFile(m_hwnd,m_loc);
        if(!path.empty())LoadSubtitleFile(path.wstring(),true);
    }
    // V: off, each stream the player can draw, the loaded file, and round again.
    void NextSubtitles(){
        if(!m_loaded)return;
        using Mode=subtitle::Choice::Mode;
        std::vector<int> order{-1};
        for(size_t index=0;index<m_subtitleTracks.size();++index)
            if(m_subtitleTracks[index].Drawn()!=subtitle::Kind::Unsupported)order.push_back(int(index));
        if(!m_subtitleFile.empty())order.push_back(-2);
        const int current=m_subtitleChoice.mode==Mode::Track?m_subtitleChoice.track:m_subtitleChoice.mode==Mode::File?-2:-1;
        const auto at=std::find(order.begin(),order.end(),current);
        const int next=order[at==order.end()?0:size_t(std::distance(order.begin(),at)+1)%order.size()];
        std::wstring label;
        if(next==-1){SelectSubtitlesOff(true);label=T(L"menu.subtitles_off");}
        else if(next==-2){SelectSubtitleFile(true);label=std::filesystem::path(m_subtitleFile).filename().wstring();}
        else{SelectSubtitleTrack(next,true);label=Utf8ToWide(subtitle::Describe(m_subtitleTracks[size_t(next)]));}
        m_sourceNotice=T(L"menu.subtitles")+L": "+label;UpdateCachedStatus();
    }
    std::wstring SubtitleDelayText()const{
        wchar_t text[32]{};swprintf_s(text,L"%+.1f s",double(m_subtitleDelayMs)/1000.0);return text;
    }
    // H and J: 0.1 s at a time, direction 0 back to none. The clock the overlay
    // is asked about moves with it, which is all a delay is (SubtitleClock).
    void StepSubtitleDelay(int direction){
        if(!m_loaded)return;
        m_subtitleDelayMs=direction?subtitle::StepDelay(m_subtitleDelayMs,direction):0;
        RememberSubtitleChoice();
        m_sourceNotice=T(L"subtitles.delay")+SubtitleDelayText();UpdateCachedStatus();
        ShowToast(m_sourceNotice);
        UpdateSubtitleMenu();
    }
    void UpdateSubtitleMenu(){
        if(!m_hwnd)return;
        const HMENU menu=GetMenu(m_hwnd);
        if(!menu)return;
        std::vector<std::wstring> labels;
        for(const auto& track:m_subtitleTracks)labels.push_back(Utf8ToWide(subtitle::Describe(track)));
        const std::wstring file=m_subtitleFile.empty()?std::wstring{}:
            T(L"menu.subtitles_file")+std::filesystem::path(m_subtitleFile).filename().wstring();
        UINT chosen=IDM_SUBTITLE_OFF;
        if(m_subtitleChoice.mode==subtitle::Choice::Mode::Track)chosen=IDM_SUBTITLE_TRACK_FIRST+UINT(m_subtitleChoice.track);
        else if(m_subtitleChoice.mode==subtitle::Choice::Mode::File)chosen=IDM_SUBTITLE_FILE;
        app_menu::UpdateSubtitles(menu,T(L"menu.subtitles_off"),labels,file,chosen,m_loaded);
        std::wstring reset=T(L"menu.subtitles_delay_reset");
        if(m_subtitleDelayMs)reset+=L" ("+SubtitleDelayText()+L")";
        if(const HMENU owner=app_menu::FindMenuContainingCommand(menu,IDM_SUBTITLE_DELAY_RESET))
            app_menu::SetMenuCommandText(owner,IDM_SUBTITLE_DELAY_RESET,reset);
        DrawMenuBar(m_hwnd);
    }
    // Every tick: keeps the child drawing what is chosen at the size the picture
    // has, tells it when the clock jumped, and hands the renderer a new picture
    // when there is one. Paused, a new picture marks the present stale, and the
    // tick's paused present shows it.
    void ServiceSubtitles(){
        SyncSubtitlesToSource();
        TakeSubtitleDiscoveries();
        if(!m_loaded||!m_renderer)return;
        // A rebuilt renderer holds no picture of its own.
        if(m_subtitleRenderer!=m_renderer.get()){m_subtitleRenderer=m_renderer.get();m_subtitleFrame.reset();}
        if(!m_subtitleWanted){
            if(m_subtitles.Showing())m_subtitles.Hide();
            if(m_subtitleFrame||m_renderer->SubtitleOverlayShown()){m_renderer->SetSubtitleOverlay(nullptr,0,0);m_subtitleFrame.reset();}
            return;
        }
        const double clock=subtitle::SubtitleClock(Position(),m_subtitleDelayMs);
        SubtitleOverlay::Canvas canvas;
        canvas.width=m_renderer->BackbufferW();canvas.height=m_renderer->BackbufferH();
        canvas.videoWidth=m_decoder.Width();canvas.videoHeight=m_decoder.Height();
        canvas.rate=subtitle::CanvasRate(m_decoder.FrameRate());canvas.duration=m_decoder.DurationSeconds();
        const SubtitleOverlay::Source* shown=m_subtitles.CurrentSource();
        bool start=!shown||!(*shown==*m_subtitleWanted);
        if(!start&&!(m_subtitles.CurrentCanvas()==canvas)){
            // A window being dragged to a new size passes through dozens; the
            // picture already up is stretched until one of them holds.
            const ULONGLONG now=GetTickCount64();
            if(canvas.width!=m_subtitleResizeW||canvas.height!=m_subtitleResizeH){
                m_subtitleResizeW=canvas.width;m_subtitleResizeH=canvas.height;m_subtitleResizeAt=now;
            }else if(now-m_subtitleResizeAt>=250)start=true;
        }
        if(start&&subtitle::CanvasUsable(canvas.width,canvas.height))m_subtitles.Show(*m_subtitleWanted,canvas,clock);
        else if(m_subtitleClock>=0.0&&subtitle::ClockJumped(m_subtitleClock,clock))m_subtitles.Seek(clock);
        m_subtitleClock=clock;
        const auto frame=m_subtitles.FrameAt(clock);
        if(frame==m_subtitleFrame)return;
        m_subtitleFrame=frame;
        if(!frame||frame->Empty()){m_renderer->SetSubtitleOverlay(nullptr,0,0);return;}
        if(!m_renderer->SetSubtitleOverlay(frame->bgra.data(),frame->width,frame->height,&frame->drawn))
            LOG("Subtitles: the renderer did not take a "<<frame->width<<"x"<<frame->height<<" picture.");
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
        if(m_cachedSettings!=m_neuralSettings||m_cachedGuides!=m_renderGuides||m_cachedTemporal!=m_temporalSettings){
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
           CachedRangeCoversSource()&&m_cachedSettings==m_neuralSettings&&m_cachedGuides==m_renderGuides&&
           m_cachedTemporal==m_temporalSettings)
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
    // Off by default: measured on an RTX 4080 SUPER, the frames it replaces were
    // already the held frame to within encoder noise (FrameGenerationRequest).
    void SetHoldDuplicateFrames(bool enabled){
        if(m_frameGenHoldDuplicates==enabled)return;
        m_frameGenHoldDuplicates=enabled;
        LOG("Hold repeated frames "<<(enabled?"on":"off")<<"; the next conversion uses it.");
        SaveVideoSettings();SyncFeatureMenuState();
    }
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
        request.holdDuplicates=m_frameGenHoldDuplicates;
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
        if(!EnsureTaskbar())return;
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
    int ControlHeight()const{return ControlsVisible()?Dip(CONTROL_H_DIP)+(CompareBarVisible()?Dip(compare_bar::kBarHeightDip):0):0;}
    // The compare bar is a row of its own on top of the strip whenever a source is
    // loaded on a build that can render neurally, and its controls grey out while
    // nothing can be compared. Shown only when comparing it would be one more thing to
    // jump in and out of the layout at every seek and view switch; hidden in safe mode,
    // where there is never a neural member to compare.
    bool CompareBarVisible()const{return m_loaded&&ControlsVisible()&&NeuralPreRenderEnabled();}
    void UpdateFontsForDpi(UINT dpi){
        const UINT activeDpi=dpi==0?USER_DEFAULT_SCREEN_DPI:dpi;
        HFONT regular=CreateUiFont(16,activeDpi);
        HFONT smallFont=CreateUiFont(14,activeDpi);
        // The start screen's "DLSS 5": a heading, not a sentence in the body size.
        HFONT titleFont=CreateFontW(-MulDiv(24,static_cast<int>(activeDpi),USER_DEFAULT_SCREEN_DPI),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        if(titleFont){if(m_fontTitle)DeleteObject(m_fontTitle);m_fontTitle=titleFont;}
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
        m_frameGenHoldDuplicates=GetPrivateProfileIntW(L"FrameGeneration",L"HoldDuplicates",0,SettingsPath().c_str())!=0;
        m_audioPassthrough=GetPrivateProfileIntW(L"Audio",L"Passthrough",0,SettingsPath().c_str())!=0;
        m_audio.SetPassthrough(m_audioPassthrough);
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
        {
            // Stored by name, so an unknown or absent value is the default rung rather
            // than whatever an index happens to point at in a later build.
            wchar_t cuts[32]{};
            GetPrivateProfileStringW(L"Temporal",L"SceneCuts",L"default",cuts,static_cast<DWORD>(std::size(cuts)),SettingsPath().c_str());
            m_temporalSettings.sceneCuts=scene_cut::ParseSensitivity(WideToUtf8(cuts)).value_or(scene_cut::Sensitivity::Default);
            m_guides.SetSceneCutSensitivity(m_temporalSettings.sceneCuts);
            wchar_t stability[32]{};
            GetPrivateProfileStringW(L"Temporal",L"Stability",L"off",stability,static_cast<DWORD>(std::size(stability)),SettingsPath().c_str());
            m_temporalSettings.stability=ParseTemporalStability(WideToUtf8(stability)).value_or(TemporalStability::Off);
        }
        m_gpuColorConversion=GetPrivateProfileIntW(L"Encoding",L"GpuColorConversion",0,SettingsPath().c_str())!=0;
        m_gpuSourceConversion=GetPrivateProfileIntW(L"Encoding",L"GpuSourceConversion",0,SettingsPath().c_str())!=0;
        m_nvencPreset=std::clamp<uint32_t>(uint32_t(GetPrivateProfileIntW(L"Encoding",L"NvencPreset",5,SettingsPath().c_str())),1,7);
        m_processingScale=ReadProcessingScale(SettingsPath());
        m_upscalingHistory=ReadUpscalingHistory(SettingsPath());
        m_captureDither=GetPrivateProfileIntW(L"Encoding",L"CaptureDither",1,SettingsPath().c_str())!=0;
        m_cacheQuality=ReadCacheQuality(SettingsPath());
        m_sourceDeband=GetPrivateProfileIntW(L"Encoding",L"SourceDeband",0,SettingsPath().c_str())!=0;
        m_suppliedExposure=GetPrivateProfileIntW(L"Encoding",L"SuppliedExposure",0,SettingsPath().c_str())!=0;
        m_neuralSettings={};LoadNeuralSettings(SettingsPath(),m_neuralSettings);
        const int mode=static_cast<int>(GetPrivateProfileIntW(L"Comparison",L"Mode",0,SettingsPath().c_str()));
        m_comparison={};
        // The Mix replaced the strength dial and Blend; compare_settings::Migrate reads
        // a file from before it. -1 is outside every value either key could hold.
        const float savedMix=ReadIniFloat(L"Comparison",L"Mix",-1.0f);
        std::vector<int> knownModes;for(const ComparisonMode known:CompareBarModes())knownModes.push_back(static_cast<int>(known));
        const auto loaded=compare_settings::Migrate(savedMix>=0.0f?std::optional<float>(savedMix):std::nullopt,mode,
            ReadIniFloat(L"Comparison",L"Amount",0.5f),ReadIniFloat(L"VideoAdjustments",L"NeuralStrength",1.0f),knownModes);
        m_comparison.mode=static_cast<ComparisonMode>(loaded.mode);
        m_comparison.strength=loaded.mix;
        m_comparison.splitX=std::clamp(ReadIniFloat(L"Comparison",L"SplitX",0.5f),0.0f,1.0f);
        // The zoom is a step on compare_zoom's ladder now; the old 2x-of-the-window
        // ZoomScale has no step that means the same, so it is left behind.
        m_zoomStep=std::clamp(int(GetPrivateProfileIntW(L"Comparison",L"ZoomStep",0,SettingsPath().c_str())),0,compare_zoom::kSteps-1);
        m_comparison.swap=GetPrivateProfileIntW(L"Comparison",L"Swap",0,SettingsPath().c_str())!=0;
        m_comparison.differenceGain=compare_settings::LoadDifferenceGain(ReadIniFloat(L"Comparison",L"DifferenceGain",compare_settings::kDefaultDifferenceGain));
        m_comparison.differenceLuma=GetPrivateProfileIntW(L"Comparison",L"DifferenceLuma",1,SettingsPath().c_str())!=0;
        m_comparison.secondMix=compare_settings::LoadSecondMix(ReadIniFloat(L"Comparison",L"SecondMix",compare_settings::kDefaultSecondMix));
        m_comparison.againstVsr=GetPrivateProfileIntW(L"Comparison",L"AgainstVsr",0,SettingsPath().c_str())!=0;
        m_comparison.vsrQuality=vsr_policy::LoadQuality(int(GetPrivateProfileIntW(L"Comparison",L"VsrQuality",static_cast<int>(vsr_policy::kDefaultQuality),SettingsPath().c_str())));
        m_compareHdrAtSdr=GetPrivateProfileIntW(L"Comparison",L"HdrAtSdr",0,SettingsPath().c_str())!=0;
        ++m_labelTextRevision;
        LoadRenderPace();
    }

    // The steady-state neural render paces this machine measured, per GPU,
    // source geometry and processing-scale rung; the record, its INI text
    // and the profiles the forecast reads are RenderPacePolicy.h's.
    void RecordPaceSample(uint32_t width,uint32_t height,double msPerFrame,uint32_t scale=kDefaultProcessingScale){
        render_pace::Record(m_paceHistory,width,height,msPerFrame,scale);
    }
    void RebuildRenderPace(){render_pace::Rebuild(m_paceHistory,m_renderPace,m_reducedRenderPace);}
    // m_renderPace is the source-scale profile; the reduced rungs follow in
    // kProcessingScaleRungs order.
    const playback_timing::RenderPaceProfile& RenderPaceAt(uint32_t scale)const{
        return render_pace::ProfileAt(scale,m_renderPace,m_reducedRenderPace);
    }
    // The keep-up forecast for a live session on this source at the current
    // processing scale, which is the one the session would render at.
    playback_timing::LiveRenderForecast LiveForecast(uint32_t width,uint32_t height,double fps)const{
        return live_session::ForecastAtProcessingScale(width,height,fps,m_processingScale,RenderPaceAt(m_processingScale),
                                                       m_renderPace,RenderPacePrior(m_opt.detectedGpu.generation));
    }
    void LoadRenderPace(){
        std::wstring gpu(512,L'\0');
        DWORD length=GetPrivateProfileStringW(L"NeuralPace",L"Gpu",L"",gpu.data(),static_cast<DWORD>(gpu.size()),SettingsPath().c_str());
        gpu.resize(length);
        m_paceHistory.clear();m_renderPace={};
        for(auto& profile:m_reducedRenderPace)profile={};
        if(gpu!=m_opt.detectedGpu.description)return;
        for(const uint32_t scale:kProcessingScaleRungs){
            std::wstring samples(2048,L'\0');
            length=GetPrivateProfileStringW(L"NeuralPace",render_pace::SamplesKey(scale).c_str(),L"",samples.data(),static_cast<DWORD>(samples.size()),SettingsPath().c_str());
            samples.resize(length);
            render_pace::Parse(samples,scale,m_paceHistory);
        }
        RebuildRenderPace();
    }
    void SaveRenderPace()const{
        WritePrivateProfileStringW(L"NeuralPace",L"Gpu",m_opt.detectedGpu.description.c_str(),SettingsPath().c_str());
        for(const uint32_t scale:kProcessingScaleRungs){
            const std::wstring samples=render_pace::Format(m_paceHistory,scale);
            // A reduced rung nobody has measured writes no key at all, so an
            // ini that never used the ladder looks exactly as it did.
            const bool source=scale==kDefaultProcessingScale;
            WritePrivateProfileStringW(L"NeuralPace",render_pace::SamplesKey(scale).c_str(),source||!samples.empty()?samples.c_str():nullptr,SettingsPath().c_str());
        }
        // Keys from the single-sample layout that preceded `Samples`.
        for(const wchar_t* stale:{L"MsPerFrame",L"Width",L"Height"})
            WritePrivateProfileStringW(L"NeuralPace",stale,nullptr,SettingsPath().c_str());
    }
    // Enough segments after the first to average out encoder spawn jitter.
    static constexpr uint64_t kMinPaceFrames=120;
    void RecordLiveRenderPace(){
        if(!m_liveSegments||!m_livePaceWidth||!m_livePaceHeight)return;
        // A rung changed mid-session renders the rest of it at the new one, so
        // the session's pace belongs to neither rung.
        if(m_processingScale!=m_livePaceScale){
            LOG("Neural render pace not recorded: the processing scale changed from "<<m_livePaceScale<<"% to "<<m_processingScale<<"% during the session.");
            return;
        }
        const auto pace=m_liveSegments->MeasuredPace();
        if(pace.frames<kMinPaceFrames||!(pace.wallMs>0.0))return;
        RecordPaceSample(m_livePaceWidth,m_livePaceHeight,pace.MsPerFrame(),m_livePaceScale);
        RebuildRenderPace();
        SaveRenderPace();
        const size_t kept=render_pace::SamplesKept(m_paceHistory,m_livePaceScale,m_livePaceWidth,m_livePaceHeight);
        const auto& profile=RenderPaceAt(m_livePaceScale);
        LOG("Measured neural render pace: "<<m_livePaceWidth<<"x"<<m_livePaceHeight<<" at "<<m_livePaceScale<<"% processing scale, "<<pace.MsPerFrame()
            <<" ms/frame over "<<pace.frames<<" frames ("<<playback_timing::RenderPaceScale(pace.MsPerFrame(),m_livePaceWidth,m_livePaceHeight)
            <<"x the reference GPU); "<<profile.samples.size()<<" geometries known for this GPU at this scale. This geometry forecasts from the median of "
            <<kept<<" samples: "<<playback_timing::PredictRenderMs(profile,m_livePaceWidth,m_livePaceHeight,0.0)<<" ms/frame.");
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
        WritePrivateProfileStringW(L"FrameGeneration",L"HoldDuplicates",m_frameGenHoldDuplicates?L"1":L"0",SettingsPath().c_str());
        WritePrivateProfileStringW(L"Audio",L"Passthrough",m_audioPassthrough?L"1":L"0",SettingsPath().c_str());
        WriteIniFloat(L"Playback",L"YouTubeQuality",static_cast<float>(m_youtubeSourceQuality));
        WriteIniFloat(L"VideoAdjustments",L"Brightness",m_colorSettings.brightness);
        WriteIniFloat(L"VideoAdjustments",L"Contrast",m_colorSettings.contrast);
        WriteIniFloat(L"VideoAdjustments",L"Saturation",m_colorSettings.saturation);
        WriteIniFloat(L"VideoAdjustments",L"Gamma",m_colorSettings.gamma);
        WriteIniFloat(L"VideoAdjustments",L"Temperature",m_colorSettings.temperature);
        WriteIniFloat(L"VideoAdjustments",L"Tint",m_colorSettings.tint);
        // The Mix lives in [Comparison] Mix. NeuralStrength is still written with the same
        // value so that an older build reading this file shows the picture this one did.
        WriteIniFloat(L"VideoAdjustments",L"NeuralStrength",m_comparison.strength);
        WritePrivateProfileStringW(L"NeuralGuides",L"MotionVectors",m_renderGuides.motionVectors?L"1":L"0",SettingsPath().c_str());
        WritePrivateProfileStringW(L"NeuralGuides",L"Depth",m_renderGuides.depth?L"1":L"0",SettingsPath().c_str());
        WritePrivateProfileStringW(L"Temporal",L"SceneCuts",Utf8ToWide(std::string(scene_cut::SensitivityName(m_temporalSettings.sceneCuts))).c_str(),SettingsPath().c_str());
        WritePrivateProfileStringW(L"Temporal",L"Stability",Utf8ToWide(std::string(TemporalStabilityName(m_temporalSettings.stability))).c_str(),SettingsPath().c_str());
        WritePrivateProfileStringW(L"Encoding",L"GpuColorConversion",m_gpuColorConversion?L"1":L"0",SettingsPath().c_str());
        WritePrivateProfileStringW(L"Encoding",L"GpuSourceConversion",m_gpuSourceConversion?L"1":L"0",SettingsPath().c_str());
        WritePrivateProfileStringW(L"Encoding",L"NvencPreset",std::to_wstring(m_nvencPreset).c_str(),SettingsPath().c_str());
        WritePrivateProfileStringW(L"NeuralRender",L"ProcessingScale",std::to_wstring(m_processingScale).c_str(),SettingsPath().c_str());
        WritePrivateProfileStringW(L"Playback",L"UpscalingHistory",Utf8ToWide(std::string(UpscalingHistoryName(m_upscalingHistory))).c_str(),SettingsPath().c_str());
        WritePrivateProfileStringW(L"Encoding",L"CaptureDither",m_captureDither?L"1":L"0",SettingsPath().c_str());
        WritePrivateProfileStringW(L"Encoding",L"SourceDeband",m_sourceDeband?L"1":L"0",SettingsPath().c_str());
        WritePrivateProfileStringW(L"Encoding",L"SuppliedExposure",m_suppliedExposure?L"1":L"0",SettingsPath().c_str());
        {
            const std::string_view rung=EncoderQualityName(m_cacheQuality);
            WritePrivateProfileStringW(L"Encoding",L"CacheQuality",std::wstring(rung.begin(),rung.end()).c_str(),SettingsPath().c_str());
        }
        SaveNeuralSettings(SettingsPath(),m_neuralSettings);
        WritePrivateProfileStringW(L"Comparison",L"Mode",std::to_wstring(static_cast<int>(m_comparison.mode)).c_str(),SettingsPath().c_str());
        WriteIniFloat(L"Comparison",L"Mix",m_comparison.strength);
        WritePrivateProfileStringW(L"Comparison",L"Swap",m_comparison.swap?L"1":L"0",SettingsPath().c_str());
        WriteIniFloat(L"Comparison",L"DifferenceGain",m_comparison.differenceGain);
        WritePrivateProfileStringW(L"Comparison",L"DifferenceLuma",m_comparison.differenceLuma?L"1":L"0",SettingsPath().c_str());
        WriteIniFloat(L"Comparison",L"SecondMix",m_comparison.secondMix);
        WritePrivateProfileStringW(L"Comparison",L"AgainstVsr",m_comparison.againstVsr?L"1":L"0",SettingsPath().c_str());
        WritePrivateProfileStringW(L"Comparison",L"VsrQuality",std::to_wstring(static_cast<int>(m_comparison.vsrQuality)).c_str(),SettingsPath().c_str());
        WriteIniFloat(L"Comparison",L"SplitX",m_comparison.splitX);
        WritePrivateProfileStringW(L"Comparison",L"ZoomStep",std::to_wstring(m_zoomStep).c_str(),SettingsPath().c_str());
        WritePrivateProfileStringW(L"Comparison",L"HdrAtSdr",m_compareHdrAtSdr?L"1":L"0",SettingsPath().c_str());
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
    // A held press on the picture shows the original in place of whatever the mode is,
    // without changing the mode; see compare_gesture.
    ComparisonSettings EffectiveComparison()const{
        ComparisonSettings effective=m_comparison;
        const int viewW=ZoomViewWidth();const uint32_t outputW=ZoomOutputWidth();
        effective.zoomScale=compare_zoom::ScaleForStep(m_zoomStep,outputW,viewW);
        if(effective.zoomScale<=1.0f){effective.zoomCenterX=0.5f;effective.zoomCenterY=0.5f;}
        effective.loupe=false;
        if(!ComparisonModesAvailable()){effective.mode=ComparisonMode::Neural;effective.strength=1.0f;effective.againstVsr=false;return effective;}
        // RTX VSR remembered from a machine or build that had it reads as DLSS 5 here,
        // without forgetting the choice.
        effective.mode=SelectedComparisonMode();
        if(!VsrUsable())effective.againstVsr=false;
        if(m_peekOriginal)effective.mode=ComparisonMode::Original;
        effective.labelFade=float(m_tagFade.Level(Clock::now()));
        effective.mask=!m_maskFeathered.pixels.empty();effective.maskInvert=m_maskInvert;
        // The loupe exists while the pointer is over the picture. Backbuffer pixels are
        // the render window's client pixels, because the backbuffers follow it.
        RECT client{};
        if(m_loupe&&m_renderMouseKnown&&m_renderWnd&&GetClientRect(m_renderWnd,&client)&&client.right>0&&client.bottom>0&&
           PtIn(client,m_renderMouse.x,m_renderMouse.y)){
            const int radius=Dip(kLoupeRadiusDip);
            const auto placement=compare_loupe::Place(m_renderMouse,client.right,client.bottom,radius,Dip(8));
            // In the panes the point is the one under the pointer in its own pane; on
            // side by side's letterbox bars there is none.
            const auto at=compare_view::Locate(PaneLayout(effective.mode),(float(m_renderMouse.x)+0.5f)/float(client.right),(float(m_renderMouse.y)+0.5f)/float(client.bottom));
            effective.loupe=at.inside;
            effective.loupeU=compare_zoom::ImageAt(effective.zoomCenterX,effective.zoomScale,at.x);
            effective.loupeV=compare_zoom::ImageAt(effective.zoomCenterY,effective.zoomScale,at.y);
            effective.loupeLeftX=float(placement.left.x);effective.loupeLeftY=float(placement.left.y);
            effective.loupeRightX=float(placement.right.x);effective.loupeRightY=float(placement.right.y);
            effective.loupeRadius=float(radius);
            const float viewPerTexel=outputW?effective.zoomScale*float(viewW)/float(outputW):effective.zoomScale;
            effective.loupeMagnification=compare_loupe::Magnification(viewPerTexel);
        }
        return effective;
    }
    static constexpr int kLoupeRadiusDip=90;
    // What the zoom ladder measures against: the width the picture is drawn at, and the
    // renderer's output width (0 until there is one, when the ladder falls back to
    // multiples of Fit).
    int ZoomViewWidth()const{
        RECT client{};if(!m_renderWnd||!GetClientRect(m_renderWnd,&client))return 0;
        return int(std::lround(float(client.right)*compare_view::PaneWidthShare(CurrentPaneLayout())));
    }
    uint32_t ZoomOutputWidth()const{return m_renderer?m_renderer->OutputW():0u;}
    std::optional<POINT> PointerOverPicture()const{
        RECT client{};
        if(m_renderMouseKnown&&m_renderWnd&&GetClientRect(m_renderWnd,&client)&&PtIn(client,m_renderMouse.x,m_renderMouse.y))return m_renderMouse;
        return std::nullopt;
    }
    static std::wstring ZoomStepText(int step){
        switch(step){case 1:return L"1:1";case 2:return L"2\u00d7";case 3:return L"4\u00d7";case 4:return L"8\u00d7";default:return L"";}
    }
    // The modes the compare bar offers, in its order, which is also the order C steps
    // through. Blend is not one of them: it was the Mix under another name. RTX VSR
    // sits with the two single pictures, before the modes that combine them.
    static std::span<const ComparisonMode> CompareBarModes(){
        static constexpr std::array modes{ComparisonMode::Neural,ComparisonMode::Original,ComparisonMode::Vsr,ComparisonMode::SplitVertical,
                                          ComparisonMode::Wipe,ComparisonMode::Difference,ComparisonMode::SideBySide,ComparisonMode::Quad};
        return modes;
    }
    // RTX VSR (P2.8): the renderer says whether it can run (VsrPolicy.h). Without a
    // renderer there is nothing to ask, and nothing to compare either.
    bool VsrUsable()const{return m_renderer&&m_renderer->VsrReason()==vsr_policy::Reason::Ready;}
    vsr_policy::Reason CurrentVsrReason()const{return m_renderer?m_renderer->VsrReason():vsr_policy::Reason::NoSession;}
    // What a greyed RTX VSR says: the reason, with the numbers it names.
    std::wstring VsrReasonText()const{
        const vsr_policy::Reason reason=CurrentVsrReason();
        const std::wstring format=T(vsr_policy::ReasonKey(reason));
        wchar_t text[256]{};
        if(reason==vsr_policy::Reason::NeedsDriver&&m_renderer){
            const auto& caps=m_renderer->VsrCapabilities();
            swprintf_s(text,format.c_str(),caps.minDriverMajor,caps.minDriverMinor);return text;
        }
        if(reason==vsr_policy::Reason::CreateFailed&&m_renderer){
            const std::string hex=HexText(m_renderer->VsrLastResult());
            swprintf_s(text,format.c_str(),std::wstring(hex.begin(),hex.end()).c_str());return text;
        }
        return format;
    }
    // The mode the bar and the menu mark: RTX VSR reads as DLSS 5 where it cannot run.
    ComparisonMode SelectedComparisonMode()const{
        return m_comparison.mode==ComparisonMode::Vsr&&!VsrUsable()?ComparisonMode::Neural:m_comparison.mode;
    }
    bool CompareModeEnabled(ComparisonMode mode)const{return ComparisonModesAvailable()&&(mode!=ComparisonMode::Vsr||VsrUsable());}
    static const wchar_t* CompareModeLabelKey(ComparisonMode mode){
        switch(mode){case ComparisonMode::Vsr:return L"compare.mode.vsr";case ComparisonMode::Original:return L"compare.mode.original";case ComparisonMode::SplitVertical:return L"compare.mode.split";case ComparisonMode::Wipe:return L"compare.mode.wipe";case ComparisonMode::Difference:return L"compare.mode.difference";case ComparisonMode::SideBySide:return L"compare.mode.side_by_side";case ComparisonMode::Quad:return L"compare.mode.quad";default:return L"compare.mode.neural";}
    }
    std::wstring CompareModeLabel(ComparisonMode mode,bool brief)const{return T((std::wstring(CompareModeLabelKey(mode))+(brief?L".short":L"")).c_str());}
    static UINT CommandForComparisonMode(ComparisonMode mode){switch(mode){case ComparisonMode::Vsr:return IDM_COMPARE_VSR;case ComparisonMode::Original:return IDM_COMPARE_ORIGINAL;case ComparisonMode::SplitVertical:return IDM_COMPARE_SPLIT;case ComparisonMode::Wipe:return IDM_COMPARE_WIPE;case ComparisonMode::Difference:return IDM_COMPARE_DIFFERENCE;case ComparisonMode::SideBySide:return IDM_COMPARE_SIDE_BY_SIDE;case ComparisonMode::Quad:return IDM_COMPARE_QUAD;default:return IDM_COMPARE_NEURAL;}}
    // The pane layout a mode draws; zoom, pan and the loupe work in pane coordinates.
    static compare_view::Layout PaneLayout(ComparisonMode mode){
        return mode==ComparisonMode::SideBySide?compare_view::Layout::SideBySide:mode==ComparisonMode::Quad?compare_view::Layout::Quad:compare_view::Layout::Single;
    }
    compare_view::Layout CurrentPaneLayout()const{return ComparisonModesAvailable()&&!m_peekOriginal?PaneLayout(SelectedComparisonMode()):compare_view::Layout::Single;}
    // A point in the render window as the pane under it and where in that pane's picture.
    compare_view::PanePoint PanePointAt(POINT point)const{
        RECT client{};
        if(!m_renderWnd||!GetClientRect(m_renderWnd,&client)||client.right<=0||client.bottom<=0)return{};
        return compare_view::Locate(CurrentPaneLayout(),(float(point.x)+0.5f)/float(client.right),(float(point.y)+0.5f)/float(client.bottom));
    }
    // Uploads the original member the presentation shader compares against.
    // Only modes that read the reference pay for the source-size copy, plus a strength
    // dial off its default, which composites against that same original.
    // True when the reference was handed to the renderer.
    bool UploadComparisonReference(const VideoFrame& original){
        // Whatever the renderer held is replaced below, or the attempt failed:
        // either way it is no longer known to be the remembered pair's.
        m_referencePair.reset();m_referenceRenderer=nullptr;
        if(!m_renderer||original.bgra.empty())return false;
        // First, because a mask remembered for this source is itself a reason to read
        // the original.
        SyncMaskToSource();
        const ComparisonSettings effective=EffectiveComparison();
        // The early-out below is also what makes an NV12 source cheap: the pure
        // neural view needs no reference at all, so the conversion under it runs
        // only while someone is actually comparing - a paused inspection, where
        // a CPU pass over one frame costs nothing anyone can perceive.
        if(!ComparisonReadsReference(effective))return false;
        EnsureLabelAtlas();EnsureMask();
        if(original.layout==VideoPixelLayout::Nv12){
            Nv12ToBgraBt709Limited(original.bgra.data(),m_decoder.Width(),m_decoder.Height(),
                                   m_referenceBgra);
            if(m_referenceBgra.empty())return false;
            return m_renderer->UploadReferenceFrame(m_referenceBgra.data(),m_referenceBgra.size());
        }
        return m_renderer->UploadReferenceFrame(original.bgra.data(),original.bgra.size(),original.pq);
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
            SyncMaskToSource();
            m_renderer->SetComparison(EffectiveComparison());
            if(ComparisonModesAvailable()){EnsureLabelAtlas();EnsureMask();}
            if(refreshPaused&&!m_playing&&!m_seeking){UploadPausedComparisonReference();if(!m_renderer->PresentCurrent())RecoverUnusableRenderer();}
            // The RTX VSR feature is created the first time a view reads it; a refusal
            // there greys the view, and the tags that name it are drawn again.
            const vsr_policy::Reason vsr=m_renderer->VsrReason();
            if(vsr!=m_lastVsrReason){
                if(m_lastVsrReason==vsr_policy::Reason::Ready)LOG("RTX VSR became unavailable: "<<utf8_text::FromWide(VsrReasonText()));
                m_lastVsrReason=vsr;++m_labelTextRevision;
            }
        }
        SyncFeatureMenuState();InvalidateCompareBar();
    }
    // The view moved - a pan, the loupe following the pointer - and nothing the menu or
    // the compare bar shows changed, so their refresh (a DrawMenuBar per mouse move)
    // is skipped. A paused frame is presented again, as ApplyComparison does.
    void ApplyComparisonView(){
        if(!m_renderer)return;
        m_renderer->SetComparison(EffectiveComparison());
        if(!m_playing&&!m_seeking){UploadPausedComparisonReference();if(!m_renderer->PresentCurrent())RecoverUnusableRenderer();}
    }
    // A refused mode used to be indistinguishable from one that did nothing: the
    // menu item greys out, but a command that arrives while no pair is resident
    // left no trace at all. Say which precondition was missing.
    void SetComparisonMode(ComparisonMode mode){
        if(!ComparisonModesAvailable()){
            LOG("Comparison mode refused: loaded="<<m_loaded<<" cachedPair="<<m_cachedPlayback<<" neuralView="<<m_neuralRequested);
            return;
        }
        // RTX VSR that cannot run says why, where the key was pressed.
        if(mode==ComparisonMode::Vsr&&!VsrUsable()){
            const std::wstring reason=VsrReasonText();
            LOG("RTX VSR view refused: "<<utf8_text::FromWide(reason));
            ShowToast(reason);return;
        }
        StartCompareMarkSlide(SelectedComparisonMode(),mode);
        // The tags name the new arrangement; they fade in with it rather than
        // printing at once over a picture that has just changed shape.
        if(mode!=m_comparison.mode){m_tagFade.Reset(false);m_tagFade.Set(true,Clock::now(),m_activityMotionEnabled);if(m_activityMotionEnabled)EnsureHoverTimer();}
        m_comparison.mode=mode;ApplyComparison();
        LOG("Comparison mode="<<static_cast<int>(mode)<<" splitX="<<m_comparison.splitX<<" zoomStep="<<m_zoomStep
            <<" reference="<<m_havePresentedPair);
    }
    // The Mix is also the adjustments dialog's DLSS 5 mix slider; both drive this value.
    void SetMix(float mix){
        if(!ComparisonModesAvailable())return;
        const float clamped=std::clamp(mix,0.0f,2.0f);if(clamped==m_comparison.strength)return;
        m_comparison.strength=clamped;ApplyComparison();
        if(m_adjustWnd)SyncAdjustmentControls(m_adjustWnd);
    }
    void AdjustMix(float delta){SetMix(compare_settings::StepMix(m_comparison.strength,delta));}
    void ToggleSwap(){if(!ComparisonModesAvailable())return;m_comparison.swap=!m_comparison.swap;ApplyComparison();}
    // The Difference view's gain and channel choice are both named in its tag, so a
    // change redraws the tag atlas.
    void StepDifferenceGain(int direction){
        if(!ComparisonModesAvailable())return;
        const float gain=compare_settings::StepDifferenceGain(m_comparison.differenceGain,direction);
        if(gain==m_comparison.differenceGain)return;
        m_comparison.differenceGain=gain;++m_labelTextRevision;ApplyComparison();
    }
    // The fourth pane's Mix is named in its tag, so a change redraws the atlas.
    void SetSecondMix(float mix){
        if(!ComparisonModesAvailable())return;
        const float snapped=compare_settings::LoadSecondMix(mix);if(snapped==m_comparison.secondMix)return;
        m_comparison.secondMix=snapped;++m_labelTextRevision;ApplyComparison();
    }
    UINT SecondMixIndex()const{
        for(size_t index=0;index<compare_settings::kSecondMixes.size();++index)if(compare_settings::kSecondMixes[index]==m_comparison.secondMix)return UINT(index);
        return 1;
    }
    void ToggleDifferenceLuma(){if(!ComparisonModesAvailable())return;m_comparison.differenceLuma=!m_comparison.differenceLuma;++m_labelTextRevision;ApplyComparison();}
    // Shift+R: Split, Wipe, Difference and Side by side compare the original against
    // RTX VSR instead of DLSS 5, and back. Named in the Difference tag, so it redraws.
    void ToggleAgainstVsr(){
        if(!ComparisonModesAvailable())return;
        if(!VsrUsable()){const std::wstring reason=VsrReasonText();LOG("Compare against RTX VSR refused: "<<utf8_text::FromWide(reason));ShowToast(reason);return;}
        m_comparison.againstVsr=!m_comparison.againstVsr;++m_labelTextRevision;ApplyComparison();
        LOG("Comparison against="<<(m_comparison.againstVsr?"RTX VSR":"DLSS 5"));
    }
    // The ladder is named in the RTX VSR tag, so a change redraws the atlas.
    void SetVsrQuality(vsr_policy::Quality quality){
        if(quality==m_comparison.vsrQuality)return;
        m_comparison.vsrQuality=quality;++m_labelTextRevision;ApplyComparison();
        LOG("RTX VSR quality="<<static_cast<int>(quality));
    }
    // C steps over a mode that cannot run rather than stopping on it.
    void CycleComparisonMode(bool reverse){
        if(!ComparisonModesAvailable())return;
        const auto modes=CompareBarModes();
        const auto current=std::find(modes.begin(),modes.end(),SelectedComparisonMode());
        size_t index=current==modes.end()?0:size_t(current-modes.begin());
        for(size_t step=0;step<modes.size();++step){
            index=(index+(reverse?modes.size()-1:1))%modes.size();
            if(CompareModeEnabled(modes[index]))break;
        }
        SetComparisonMode(modes[index]);
    }
    // The divider is an image-UV position; while zoomed the shader shows
    // uv=(screen-center)/zoom+center, so invert that to keep it under the pointer.
    void SetSplitFromRenderX(int x){
        RECT client{};if(!m_renderWnd||!GetClientRect(m_renderWnd,&client)||client.right<=0)return;
        const float screen=std::clamp(float(x)/float(client.right),0.0f,1.0f);
        const ComparisonSettings view=EffectiveComparison();
        m_comparison.splitX=std::clamp(compare_zoom::ImageAt(view.zoomCenterX,view.zoomScale,screen),0.0f,1.0f);
        ApplyComparison();
    }
    bool SplitDragActive()const{const ComparisonMode mode=EffectiveComparison().mode;return mode==ComparisonMode::SplitVertical||mode==ComparisonMode::Wipe;}
    // One step along compare_zoom's ladder, keeping the image point under `anchor` (a
    // point in the render window) where it is; the picture's centre without one.
    void ZoomBy(int direction,bool wrap,std::optional<POINT> anchor){
        if(!m_loaded||!m_renderer)return;
        const int viewW=ZoomViewWidth();const uint32_t outputW=ZoomOutputWidth();
        const int next=compare_zoom::Step(m_zoomStep,direction,outputW,viewW,wrap);
        if(next==m_zoomStep)return;
        const float from=compare_zoom::ScaleForStep(m_zoomStep,outputW,viewW),to=compare_zoom::ScaleForStep(next,outputW,viewW);
        float anchorX=0.5f,anchorY=0.5f;
        if(anchor){
            const auto at=PanePointAt(*anchor);
            if(at.inside){anchorX=std::clamp(at.x,0.0f,1.0f);anchorY=std::clamp(at.y,0.0f,1.0f);}
        }
        const auto centre=compare_zoom::ZoomAt({m_comparison.zoomCenterX,m_comparison.zoomCenterY},from,to,anchorX,anchorY);
        m_zoomStep=next;m_comparison.zoomCenterX=centre.x;m_comparison.zoomCenterY=centre.y;
        ApplyComparison();
    }
    // Z: in one step at the pointer, and back to Fit after 8x.
    void ToggleZoom(){ZoomBy(+1,true,PointerOverPicture());}
    void ZoomToFit(){if(!m_zoomStep)return;m_zoomStep=0;m_comparison.zoomCenterX=0.5f;m_comparison.zoomCenterY=0.5f;ApplyComparison();}
    bool Zoomed()const{return EffectiveComparison().zoomScale>1.0f;}
    // Drags the zoomed picture with the pointer: (dx, dy) in render-window pixels.
    void PanBy(int dx,int dy){
        RECT client{};if(!m_renderWnd||!GetClientRect(m_renderWnd,&client)||client.right<=0||client.bottom<=0)return;
        const float scale=EffectiveComparison().zoomScale;if(scale<=1.0f||(!dx&&!dy))return;
        // A pane's picture is a share of the window, so the same drag is a larger
        // share of the picture there.
        const float share=compare_view::PaneWidthShare(CurrentPaneLayout());
        const auto centre=compare_zoom::Pan({m_comparison.zoomCenterX,m_comparison.zoomCenterY},scale,float(dx)/(float(client.right)*share),float(dy)/(float(client.bottom)*share));
        m_comparison.zoomCenterX=centre.x;m_comparison.zoomCenterY=centre.y;
        ApplyComparisonView();
    }
    void ToggleLoupe(){if(!ComparisonModesAvailable())return;m_loupe=!m_loupe;ApplyComparison();}

    // --- Save comparison image (P2.21) ---------------------------------------------
    // What the saved image says about itself; see compare_provenance::FooterLines.
    compare_provenance::Facts ComparisonProvenance()const{
        compare_provenance::Facts facts;
        const std::string version=DLSS_VIDEO_PLAYER_VERSION;
        facts.application=L"DLSS 5 Video Player "+std::wstring(version.begin(),version.end());
        facts.source=m_displayTitle.empty()?std::filesystem::path(m_path).filename().wstring():m_displayTitle;
        const double fps=std::max(1.0,m_decoder.FrameRate());
        facts.timecode=FormatTimecode(Position100ns(),fps,true);
        facts.frame=uint64_t(std::llround(std::max(0.0,Position())*fps));
        const ComparisonSettings shown=EffectiveComparison();
        std::wstring view=T(CompareModeLabelKey(shown.mode));
        if(shown.mode==ComparisonMode::SplitVertical||shown.mode==ComparisonMode::Wipe)view+=L" "+PercentText(shown.splitX)+(shown.swap?L" swapped":L"");
        const std::wstring vsrName=T(L"compare.mode.vsr")+L" "+T((L"menu.compare_vsr_quality_"+std::to_wstring(vsr_policy::QualityIndex(shown.vsrQuality))).c_str());
        const bool vsrShown=m_renderer&&m_renderer->VsrShown();
        if(shown.mode==ComparisonMode::Quad)view+=vsrShown?L" (fourth pane "+vsrName+L")":L" (fourth pane Mix "+PercentText(shown.secondMix)+L")";
        if(shown.mode==ComparisonMode::Vsr&&vsrShown)view+=L" \u00b7 "+T((L"menu.compare_vsr_quality_"+std::to_wstring(vsr_policy::QualityIndex(shown.vsrQuality))).c_str());
        if(vsrShown&&ComparisonComparesAgainstVsr(shown))view+=L" against "+vsrName;
        if(shown.mode==ComparisonMode::Difference){wchar_t gain[16]{};swprintf_s(gain,L"%g",double(shown.differenceGain));view+=std::wstring(L" \u00d7")+gain+(shown.differenceLuma?L" luma":L" color");}
        view+=L" \u00b7 Mix "+PercentText(shown.strength)+L" \u00b7 Zoom "+(m_zoomStep>0?ZoomStepText(m_zoomStep):T(L"compare.zoom.fit"));
        if(shown.mask)view+=L" \u00b7 Mask "+m_maskPath.filename().wstring()+(shown.maskInvert?L" inverted":L"");
        facts.view=view;
        // The render's settings as the player recorded them for the cache entry being
        // played: the canonical neural settings and guide switches, hashed.
        if(m_cachedPlayback){
            const auto digest=Sha256Bytes(CanonicalNeuralSettings(m_cachedSettings)+"|"+CanonicalGuideControls(m_cachedGuides));
            facts.settings=digest?L"sha256:"+std::wstring(digest->begin(),digest->begin()+std::min<size_t>(16,digest->size())):std::wstring(L"unavailable");
        }else facts.settings=T(L"compare.save.no_render");
        const std::string& runtime=EmbeddedRuntimeLock().runtimeVersion;
        facts.runtime=runtime.empty()?std::wstring(L"unknown"):std::wstring(runtime.begin(),runtime.end());
        SYSTEMTIME now{};GetLocalTime(&now);wchar_t stamp[32]{};
        swprintf_s(stamp,L"%04u-%02u-%02u %02u:%02u:%02u",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);
        facts.saved=stamp;
        return facts;
    }
    // The composed picture above a footer, as 24-bit BGR rows. The footer is set in a
    // monospaced face - the facts in it are measured values - on the near-black ground
    // with the flag rule down its left edge, like the tags on the picture.
    std::vector<uint8_t> ComposeComparisonImage(const std::vector<uint8_t>& rgba,uint32_t width,uint32_t height,
                                                const std::vector<std::wstring>& lines,uint32_t& outHeight,uint32_t& stride)const{
        std::vector<uint8_t> bgr;outHeight=0;stride=0;
        if(!width||!height||rgba.size()<size_t(width)*height*4u)return bgr;
        const int fontPx=std::clamp(int(width)/100,12,28),lineHeight=fontPx*3/2,pad=fontPx;
        const int footer=pad*2+lineHeight*int(lines.size());
        const int total=int(height)+footer;
        HDC screen=GetDC(nullptr);HDC dc=CreateCompatibleDC(screen);ReleaseDC(nullptr,screen);
        if(!dc)return bgr;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(info.bmiHeader);info.bmiHeader.biWidth=int(width);info.bmiHeader.biHeight=-total;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* bits=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!bitmap||!bits){if(bitmap)DeleteObject(bitmap);DeleteDC(dc);return bgr;}
        const HGDIOBJ oldBitmap=SelectObject(dc,bitmap);
        auto* pixels=static_cast<uint8_t*>(bits);
        for(size_t at=0;at<size_t(width)*height;++at){pixels[at*4]=rgba[at*4+2];pixels[at*4+1]=rgba[at*4+1];pixels[at*4+2]=rgba[at*4];pixels[at*4+3]=255;}
        RECT band{0,int(height),int(width),total};HBRUSH ground=CreateSolidBrush(RGB(5,5,6));FillRect(dc,&band,ground);DeleteObject(ground);
        RECT rule{0,int(height),std::max(2,fontPx/6),total};HBRUSH flag=CreateSolidBrush(kCompareMark);FillRect(dc,&rule,flag);DeleteObject(flag);
        HFONT font=CreateFontW(-fontPx,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH|FF_MODERN,L"Cascadia Mono");
        const HGDIOBJ oldFont=SelectObject(dc,font?font:GetStockObject(ANSI_FIXED_FONT));
        SetBkMode(dc,TRANSPARENT);
        for(size_t index=0;index<lines.size();++index){
            // The first line names the picture, so it takes the brighter step.
            SetTextColor(dc,index==0?RGB(236,235,232):RGB(164,160,153));
            RECT line{pad*2,int(height)+pad+int(index)*lineHeight,int(width)-pad,int(height)+pad+int(index+1)*lineHeight};
            DrawTextW(dc,lines[index].c_str(),-1,&line,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        }
        GdiFlush();
        stride=(width*3u+3u)&~3u;outHeight=uint32_t(total);
        bgr.assign(size_t(stride)*outHeight,0);
        for(int y=0;y<total;++y)for(uint32_t x=0;x<width;++x){
            const uint8_t* from=pixels+(size_t(y)*width+x)*4;uint8_t* to=bgr.data()+size_t(y)*stride+size_t(x)*3;
            to[0]=from[0];to[1]=from[1];to[2]=from[2];
        }
        SelectObject(dc,oldFont);if(font)DeleteObject(font);SelectObject(dc,oldBitmap);DeleteObject(bitmap);DeleteDC(dc);
        return bgr;
    }
    bool SaveComparisonImageTo(const std::filesystem::path& path){
        if(!m_loaded||!m_renderer)return false;
        // The present first, so an original uploaded since the last one is in the capture.
        if(!m_renderer->PresentCurrent()){RecoverUnusableRenderer();return false;}
        std::vector<uint8_t> rgba;uint32_t width=0,height=0;
        if(!m_renderer->CaptureComposedView(rgba,width,height)){LOG("Save comparison image: the composed view could not be read back.");return false;}
        const auto lines=compare_provenance::FooterLines(ComparisonProvenance());
        uint32_t total=0,stride=0;
        const auto bgr=ComposeComparisonImage(rgba,width,height,lines,total,stride);
        if(bgr.empty())return false;
        const HRESULT hr=compare_image::SavePngBgr(path,bgr.data(),width,total,stride);
        if(FAILED(hr)){LOG("Save comparison image failed hr="<<HexText(hr)<<" path="<<WideToUtf8(path.wstring()));return false;}
        LOG("Comparison image saved: "<<width<<"x"<<height<<" plus a "<<(total-height)<<" px footer -> "<<WideToUtf8(path.wstring()));
        return true;
    }
    void SaveComparisonImage(){
        if(!m_loaded||!m_renderer)return;
        const auto path=PickComparisonImage(m_hwnd,m_loc,compare_provenance::SuggestedName(m_displayTitle,FormatTimecode(Position100ns(),std::max(1.0,m_decoder.FrameRate()),true)));
        if(path.empty())return;
        const bool saved=SaveComparisonImageTo(path);
        m_sourceNotice=saved?T(L"compare.save.saved")+path.filename().wstring():T(L"compare.save.failed");
        if(saved)ShowToast(T(L"compare.save.saved")+path.filename().wstring());
        UpdateCachedStatus();
    }

    // --- The spatial mask on the Mix (P2.6) -------------------------------------
    // Which source the mask state belongs to. Compared by the two strings that
    // identify it before anything is built, because this runs per presented pair.
    std::wstring MaskIdentity()const{
        if(!m_youtubePageUrl.empty()){
            const std::string id=CanonicalYouTubeVideoId(m_youtubePageUrl);
            if(!id.empty())return L"youtube:"+std::wstring(id.begin(),id.end());
        }
        return m_path.empty()?std::wstring{}:L"file:"+m_path;
    }
    void SyncMaskToSource(){
        if(m_path==m_maskForPath&&m_youtubePageUrl==m_maskForPage)return;
        m_maskForPath=m_path;m_maskForPage=m_youtubePageUrl;
        const std::wstring identity=MaskIdentity();
        const std::wstring key=identity.empty()?std::wstring{}:compare_mask::SourceKey(identity);
        if(key==m_maskSourceKey)return;
        m_maskSourceKey=key;
        DropMask();
        if(key.empty())return;
        wchar_t saved[4096]{};
        GetPrivateProfileStringW(L"ComparisonMasks",key.c_str(),L"",saved,static_cast<DWORD>(std::size(saved)),SettingsPath().c_str());
        const auto record=compare_mask::Parse(saved);
        if(!record)return;
        if(!LoadMask(record->path,record->feather,record->invert,false))
            LOG("The mask remembered for this source was not restored.");
    }
    void DropMask(){
        if(m_maskSource.pixels.empty()&&m_maskFeathered.pixels.empty())return;
        m_maskSource={};m_maskFeathered={};m_maskPath.clear();++m_maskRevision;
    }
    // Reads, shrinks and feathers a mask; `remember` records it for this source.
    bool LoadMask(const std::filesystem::path& path,int feather,bool invert,bool remember){
        compare_mask::Gray gray;
        const HRESULT hr=compare_image::LoadGray(path,gray);
        if(FAILED(hr)){LOG("Mask image could not be read: hr="<<HexText(hr)<<" path="<<WideToUtf8(path.wstring()));return false;}
        m_maskSource=compare_mask::Shrink(gray);m_maskPath=path;m_maskFeather=feather;m_maskInvert=invert;
        RefeatherMask();
        if(remember)RememberMask();
        LOG("Comparison mask loaded: "<<gray.width<<"x"<<gray.height<<" kept at "<<m_maskSource.width<<"x"<<m_maskSource.height
            <<" feather="<<m_maskFeather<<" invert="<<m_maskInvert);
        return true;
    }
    void RefeatherMask(){
        m_maskFeathered=m_maskSource;
        const uint32_t frameW=m_renderer&&m_renderer->OutputW()?m_renderer->OutputW():m_decoder.Width();
        compare_mask::Feather(m_maskFeathered,compare_mask::MaskRadius(m_maskFeather,m_maskFeathered.width,frameW));
        ++m_maskRevision;
    }
    void RememberMask(){
        if(m_maskSourceKey.empty()||m_maskPath.empty())return;
        const auto path=SettingsPath();
        // The section as it stands, to number this entry after the newest and to find
        // the oldest ones past the bound.
        std::vector<wchar_t> section(65536,L'\0');
        const DWORD length=GetPrivateProfileSectionW(L"ComparisonMasks",section.data(),static_cast<DWORD>(section.size()),path.c_str());
        std::map<std::wstring,compare_mask::Record> records;uint64_t newest=0;
        for(size_t at=0;at<length&&section[at];){
            const std::wstring line(section.data()+at);at+=line.size()+1;
            const size_t equals=line.find(L'=');if(equals==std::wstring::npos)continue;
            if(const auto record=compare_mask::Parse(std::wstring_view(line).substr(equals+1))){
                records[line.substr(0,equals)]=*record;newest=std::max(newest,record->sequence);
            }
        }
        const compare_mask::Record record{newest+1,m_maskFeather,m_maskInvert,m_maskPath.wstring()};
        records[m_maskSourceKey]=record;
        WritePrivateProfileStringW(L"ComparisonMasks",m_maskSourceKey.c_str(),compare_mask::Format(record).c_str(),path.c_str());
        for(const std::wstring& stale:compare_mask::Evict(records))WritePrivateProfileStringW(L"ComparisonMasks",stale.c_str(),nullptr,path.c_str());
    }
    void LoadMaskFromDialog(){
        if(!m_loaded)return;
        const auto path=PickMaskImage(m_hwnd,m_loc);if(path.empty())return;
        SyncMaskToSource();
        if(!LoadMask(path,m_maskFeather,false,true)){
            m_sourceNotice=T(L"compare.mask.failed");UpdateCachedStatus();return;
        }
        ApplyComparison();
    }
    void ClearMaskForSource(){
        if(m_maskSource.pixels.empty())return;
        DropMask();
        if(!m_maskSourceKey.empty())WritePrivateProfileStringW(L"ComparisonMasks",m_maskSourceKey.c_str(),nullptr,SettingsPath().c_str());
        ApplyComparison();
    }
    void SetMaskFeather(size_t index){
        if(m_maskSource.pixels.empty()||index>=compare_mask::kFeathers.size())return;
        m_maskFeather=compare_mask::kFeathers[index];RefeatherMask();RememberMask();ApplyComparison();
    }
    void ToggleMaskInvert(){if(m_maskSource.pixels.empty())return;m_maskInvert=!m_maskInvert;RememberMask();ApplyComparison();}
    // Uploads the feathered mask when the renderer lacks this revision of it, and takes
    // it away when there is none. Synchronous (it drains), so only on a change.
    void EnsureMask(){
        if(!m_renderer)return;
        if(m_maskFeathered.pixels.empty()){if(m_renderer->HasMask())m_renderer->ClearMask();return;}
        if(m_renderer->HasMask()&&m_maskUploadedRevision==m_maskRevision)return;
        if(m_maskRefusedBy==m_renderer.get()&&m_maskRefusedRevision==m_maskRevision)return;
        if(m_renderer->SetMask(m_maskFeathered.pixels.data(),m_maskFeathered.width,m_maskFeathered.height)){
            m_maskUploadedRevision=m_maskRevision;m_maskRefusedBy=nullptr;
        }else{
            m_maskRefusedBy=m_renderer.get();m_maskRefusedRevision=m_maskRevision;
            LOG("Comparison mask not uploaded ("<<m_maskFeathered.width<<"x"<<m_maskFeathered.height<<").");
        }
    }
    UINT MaskFeatherIndex()const{
        for(size_t index=0;index<compare_mask::kFeathers.size();++index)if(compare_mask::kFeathers[index]==m_maskFeather)return UINT(index);
        return 0;
    }
    // View > Fit, Fill and 1:1 pixels; A and the toolbar pill flip between the first two.
    void SetAspect(bool fill,bool onePixel){m_fill=fill;m_onePixel=onePixel;Layout();SyncFeatureMenuState();InvalidateControls();}
    // A press on the picture: the divider, a drag, or the press-and-hold A/B; see
    // compare_gesture for how the three are told apart.
    void RenderMouseDown(HWND source,LPARAM position){
        m_fullscreenKeyboardFocus=false;SetFocus(m_hwnd);
        if(m_keyboardCues){m_keyboardCues=false;InvalidateControls();}
        const bool compare=ComparisonModesAvailable(),zoomed=m_loaded&&Zoomed();
        if(!compare&&!zoomed)return;
        const POINT point{GET_X_LPARAM(position),GET_Y_LPARAM(position)};
        m_panLast=point;
        ApplyGestureStep(compare_gesture::Press(m_gesture,point,compare&&SplitDragActive(),zoomed),point);
        m_dragSplit=compare&&SplitDragActive();SetCapture(source);
        if(compare)SetTimer(m_hwnd,kPeekTimerId,compare_gesture::kHoldMs,nullptr);
    }
    // The middle button pans a zoomed picture in every mode, split and wipe included,
    // where the left button's drag belongs to the divider.
    void RenderMiddleDown(HWND source,LPARAM position){
        m_fullscreenKeyboardFocus=false;SetFocus(m_hwnd);
        if(!m_loaded||!Zoomed())return;
        m_middlePan=true;m_panLast={GET_X_LPARAM(position),GET_Y_LPARAM(position)};SetCapture(source);
    }
    void RenderMiddleUp(HWND source){if(!m_middlePan)return;m_middlePan=false;if(GetCapture()==source)ReleaseCapture();}
    void RenderMouseLeft(){
        m_renderTracking=false;m_renderMouseKnown=false;
        if(m_loupe&&ComparisonModesAvailable())ApplyComparisonView();
    }
    void RenderMouseMove(HWND source,LPARAM position){
        FullscreenPointerMoved(source,position);
        m_renderMouse={GET_X_LPARAM(position),GET_Y_LPARAM(position)};m_renderMouseKnown=true;
        // The loupe has to know when the pointer leaves the picture.
        if(!m_renderTracking){TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,source,0};m_renderTracking=TrackMouseEvent(&tracking)!=FALSE;}
        if(m_middlePan){PanBy(m_renderMouse.x-m_panLast.x,m_renderMouse.y-m_panLast.y);m_panLast=m_renderMouse;}
        else if(m_loupe&&m_gesture.phase==compare_gesture::Phase::Idle&&ComparisonModesAvailable())ApplyComparisonView();
        // Not gated on holding the capture: losing it ends the press (RenderCaptureLost),
        // so a press still in progress is the only thing this needs to know.
        if(m_gesture.phase!=compare_gesture::Phase::Idle)
            ApplyGestureStep(compare_gesture::Move(m_gesture,m_renderMouse,Dip(compare_gesture::kSlopDip)),m_renderMouse);
    }
    void RenderMouseUp(HWND source){
        KillTimer(m_hwnd,kPeekTimerId);
        const bool pressed=m_gesture.phase!=compare_gesture::Phase::Idle;
        ApplyGestureStep(compare_gesture::Release(m_gesture),m_renderMouse);
        m_dragSplit=false;if(pressed&&GetCapture()==source)ReleaseCapture();
    }
    // Capture taken away mid-press (a menu, a dialog, alt-tab) ends the press the way a
    // release would, so a peek can never outlive the button that started it.
    void RenderCaptureLost(){KillTimer(m_hwnd,kPeekTimerId);m_dragSplit=false;m_middlePan=false;ApplyGestureStep(compare_gesture::Release(m_gesture),m_renderMouse);}
    void PeekHoldElapsed(){KillTimer(m_hwnd,kPeekTimerId);ApplyGestureStep(compare_gesture::HoldElapsed(m_gesture),m_renderMouse);}
    void ApplyGestureStep(const compare_gesture::Step& step,POINT point){
        // The peek ends before the divider moves, so the move that ends a hold already
        // drags the divider it lands on.
        if(step.endPeek&&m_peekOriginal){m_peekOriginal=false;ApplyComparison();}
        if(step.setDivider&&SplitDragActive())SetSplitFromRenderX(point.x);
        if(step.pan){PanBy(point.x-m_panLast.x,point.y-m_panLast.y);m_panLast=point;}
        else if(m_loupe&&ComparisonModesAvailable()&&m_gesture.phase!=compare_gesture::Phase::Idle)ApplyComparisonView();
        if(step.startPeek&&!m_peekOriginal&&ComparisonModesAvailable()){m_peekOriginal=true;ApplyComparison();LOG("Press-and-hold: showing the original.");}
    }

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
            CheckMenuRadioItem(menu,IDM_PROCESSING_SCALE_FIRST,IDM_PROCESSING_SCALE_LAST,
                               app_menu::CommandForProcessingScale(m_processingScale),MF_BYCOMMAND);
            CheckMenuRadioItem(menu,IDM_UPSCALE_HISTORY_TEMPORAL,IDM_UPSCALE_HISTORY_PER_FRAME,
                               m_upscalingHistory==UpscalingHistory::PerFrame?IDM_UPSCALE_HISTORY_PER_FRAME:IDM_UPSCALE_HISTORY_TEMPORAL,
                               MF_BYCOMMAND);
            // Live only while an HDR source is on an HDR display: anywhere else there
            // is no HDR original to compare at SDR.
            const bool hdrCompare=m_loaded&&m_renderer&&m_renderer->HdrOutputActive()&&
                                  m_decoder.SourceHdrSignal()!=hdr_policy::HdrSignal::Sdr;
            EnableMenuItem(menu,IDM_COMPARE_HDR_AT_SDR,MF_BYCOMMAND|(hdrCompare?MF_ENABLED:MF_GRAYED));
            CheckMenuItem(menu,IDM_COMPARE_HDR_AT_SDR,MF_BYCOMMAND|(m_compareHdrAtSdr?MF_CHECKED:MF_UNCHECKED));
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
            CheckMenuItem(menu,IDM_AUDIO_PASSTHROUGH,
                          MF_BYCOMMAND|(m_audioPassthrough?MF_CHECKED:MF_UNCHECKED));
            CheckMenuItem(menu,IDM_FRAMEGEN_HOLD_DUPLICATES,
                          MF_BYCOMMAND|(m_frameGenHoldDuplicates?MF_CHECKED:MF_UNCHECKED));
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
            app_menu::CheckRadioCommand(menu,IDM_ASPECT_FIT,IDM_ASPECT_ONE_TO_ONE,m_onePixel?IDM_ASPECT_ONE_TO_ONE:(m_fill?IDM_ASPECT_FILL:IDM_ASPECT_FIT));
            app_menu::UpdateRenderActionAvailability(menu,m_loaded,RangeRenderAvailable(),NeuralJobActive(),NeuralJobPaused(),!m_cachedReceiptPath.empty());
            app_menu::UpdateComparisonMenu(menu,ComparisonModesAvailable(),m_loaded&&m_renderer!=nullptr,CommandForComparisonMode(SelectedComparisonMode()),m_zoomStep>0,m_comparison.swap,m_loupe,m_comparison.differenceLuma,
                                           VsrUsable(),m_comparison.againstVsr);
            app_menu::UpdateVsrQualityMenu(menu,ComparisonModesAvailable()&&VsrUsable(),UINT(vsr_policy::QualityIndex(m_comparison.vsrQuality)));
            app_menu::UpdateMaskMenu(menu,m_loaded,!m_maskSource.pixels.empty(),m_maskInvert,MaskFeatherIndex());
            app_menu::UpdateSecondMixMenu(menu,ComparisonModesAvailable(),SecondMixIndex());
            EnableMenuItem(menu,IDM_SAVE_COMPARISON_IMAGE,MF_BYCOMMAND|(m_loaded&&m_renderer?MF_ENABLED:MF_GRAYED));
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
            ApplyDarkControlTheme(host);
            SendMessageW(host,TTM_SETMAXTIPWIDTH,0,Sd(dialog,420));
            SendMessageW(host,TTM_SETDELAYTIME,TTDT_AUTOPOP,MAKELPARAM(toolbar_tips::kAutoPopMs,0));
            SendMessageW(host,TTM_SETDELAYTIME,TTDT_INITIAL,MAKELPARAM(toolbar_tips::kInitialDelayMs,0));
            SendMessageW(host,TTM_SETDELAYTIME,TTDT_RESHOW,MAKELPARAM(toolbar_tips::kReshowDelayMs,0));
            m_tipHosts[dialog]=host;
        }
        return host;
    }
    // The host is a popup owned by the dialog, so Windows destroys it with its
    // owner; only this dialog's strings are dropped, never another dialog's.
    void ReleaseDialogTips(HWND dialog){m_tipHosts.erase(dialog);m_tipText.erase(dialog);}

    // Toolbar buttons are painted, not child windows, so their tips are
    // registered by RECTANGLE. The rectangles move on every resize and whenever
    // the bar drops items, so they are re-registered from the same layout the
    // painter uses - a tip pinned to a stale rect is worse than no tip, because
    // it describes whatever control has moved into that space. That includes a
    // control that is no longer laid out at all: the start screen's two buttons
    // sit where the picture is once a file loads, and a narrowed bar drops
    // items, so every action this surface does not show gets an empty rect.
    // Tool ids past every ToolbarAction value, so the compare bar's tips never replace a toolbar one.
    static constexpr UINT_PTR kCompareTipIdBase=0x1000;
    static constexpr UINT_PTR kCompareModeTipIdBase=0x1100;static constexpr size_t kCompareModeTipCount=8;
    static constexpr UINT_PTR kChipTipIdBase=0x1200;
    // Text for a tool registered with LPSTR_TEXTCALLBACK, built when the tip
    // is about to show; empty means the tool has nothing to say (no tip).
    std::wstring CallbackTipText(UINT_PTR id){
        if(id>=kCompareModeTipIdBase&&id<kCompareModeTipIdBase+kCompareModeTipCount){
            const auto modes=CompareBarModes();const size_t index=size_t(id-kCompareModeTipIdBase);
            if(index>=modes.size())return {};
            if(!ComparisonModesAvailable())return T(L"compare.hint.unavailable");
            // A greyed RTX VSR says what stops it (DESIGN.md, Tooltips).
            if(modes[index]==ComparisonMode::Vsr)return VsrUsable()?T(L"compare.tip.vsr"):VsrReasonText();
            return CompareModeLabel(modes[index],false)+T(L"compare.tip.mode_cycle");
        }
        if(id>=kChipTipIdBase&&id<kChipTipIdBase+status_chips::kChipCount){
            const size_t index=size_t(id-kChipTipIdBase);
            if(!m_cachedChips[index].visible)return {};
            status_chips::TipFacts facts{RenderChipProgress(),0.0,0.0,m_submitFps,m_decoder.FrameRate(),m_droppedFrames};
            if(m_liveSession&&m_liveRange.end100ns>m_liveRange.start100ns){
                facts.rangeSeconds=double(m_liveRange.end100ns-m_liveRange.start100ns)*1e-7;
                facts.paceRatio=LiveRealtimeRatio();
            }
            return status_chips::TipText(static_cast<status_chips::Chip>(index),facts);
        }
        return {};
    }
    std::wstring m_callbackTip;
    LRESULT TipNotify(const NMHDR& header){
        if(header.code!=TTN_GETDISPINFOW)return 0;
        auto& info=const_cast<NMTTDISPINFOW&>(reinterpret_cast<const NMTTDISPINFOW&>(header));
        m_callbackTip=CallbackTipText(info.hdr.idFrom);
        info.lpszText=m_callbackTip.data();info.szText[0]=L'\0';info.hinst=nullptr;
        return 0;
    }
    void RefreshToolbarTips(){
        if(!m_hwnd||!IsWindow(m_hwnd))return;
        const auto items=FocusableItems();
        HWND host=EnsureTipHost(m_hwnd);
        if(!host)return;
        auto& text=m_tipText[m_hwnd];
        // Tool ids are the action value, so a re-registration replaces the tool
        // for that action rather than stacking a second one on top of it.
        for(size_t index=0;index<static_cast<size_t>(ToolbarAction::None);++index){
            const auto action=static_cast<ToolbarAction>(index);
            const wchar_t* key=toolbar_tips::TipKey(action);
            if(!key)continue;
            RECT bounds{};
            for(const auto& item:items)if(item.action==action)bounds=item.bounds;
            const UINT_PTR id=static_cast<UINT_PTR>(action);
            TTTOOLINFOW info{};info.cbSize=TTTOOLINFOW_V2_SIZE;info.uFlags=TTF_SUBCLASS;
            info.hwnd=m_hwnd;info.uId=id;
            // Present already: move it. The control moved, the sentence did not.
            if(SendMessageW(host,TTM_GETTOOLINFOW,0,reinterpret_cast<LPARAM>(&info))){
                info.rect=bounds;
                SendMessageW(host,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&info));
                continue;
            }
            text.push_back(std::make_unique<std::wstring>(T(key)));
            info.lpszText=text.back()->data();
            info.rect=bounds;
            SendMessageW(host,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
        }
        // The compare bar's parts that can lose their words, named on hover. A part the
        // bar does not show keeps its tool with an empty rectangle, which never fires.
        const auto bar=CompareBarVisible()?CompareBarLayout():compare_bar::Layout{};
        static constexpr std::array<std::pair<compare_bar::Part,const wchar_t*>,5> kCompareTips{{
            {compare_bar::Part::ModeMenu,L"compare.tip.mode"},{compare_bar::Part::ZoomOut,L"compare.tip.zoom_out"},{compare_bar::Part::ZoomIn,L"compare.tip.zoom_in"},
            {compare_bar::Part::Swap,L"compare.tip.swap"},{compare_bar::Part::Loupe,L"compare.tip.loupe"}}};
        for(const auto& [part,key]:kCompareTips){
            RECT bounds{};
            for(const auto& item:bar.items)if(item.part==part)bounds=item.bounds;
            TTTOOLINFOW info{};info.cbSize=TTTOOLINFOW_V2_SIZE;info.uFlags=TTF_SUBCLASS;
            info.hwnd=m_hwnd;info.uId=kCompareTipIdBase+static_cast<UINT_PTR>(part);
            if(SendMessageW(host,TTM_GETTOOLINFOW,0,reinterpret_cast<LPARAM>(&info))){
                info.rect=bounds;SendMessageW(host,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&info));continue;
            }
            text.push_back(std::make_unique<std::wstring>(T(key)));
            info.lpszText=text.back()->data();info.rect=bounds;
            SendMessageW(host,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
        }
        // The mode segments' tips are asked for when shown (TTN_GETDISPINFO):
        // a greyed mode says what would make it work, a live one names itself
        // and the key that cycles.
        for(int index=0;index<static_cast<int>(kCompareModeTipCount);++index){
            RECT bounds{};
            for(const auto& item:bar.items)if(item.part==compare_bar::Part::Mode&&item.index==index)bounds=item.bounds;
            TTTOOLINFOW info{};info.cbSize=TTTOOLINFOW_V2_SIZE;info.uFlags=TTF_SUBCLASS;
            info.hwnd=m_hwnd;info.uId=kCompareModeTipIdBase+static_cast<UINT_PTR>(index);
            if(SendMessageW(host,TTM_GETTOOLINFOW,0,reinterpret_cast<LPARAM>(&info))){
                info.rect=bounds;SendMessageW(host,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&info));continue;
            }
            info.lpszText=LPSTR_TEXTCALLBACKW;info.rect=bounds;
            SendMessageW(host,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
        }
        // The chips, asked for when shown for the same reason: the numbers move.
        const auto chips=m_loaded?StatusRowLayout():status_chips::RowLayout{};
        for(size_t index=0;index<status_chips::kChipCount;++index){
            TTTOOLINFOW info{};info.cbSize=TTTOOLINFOW_V2_SIZE;info.uFlags=TTF_SUBCLASS;
            info.hwnd=m_hwnd;info.uId=kChipTipIdBase+index;
            const RECT bounds=ControlsVisible()?chips.chips[index]:RECT{};
            if(SendMessageW(host,TTM_GETTOOLINFOW,0,reinterpret_cast<LPARAM>(&info))){
                info.rect=bounds;SendMessageW(host,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&info));continue;
            }
            info.lpszText=LPSTR_TEXTCALLBACKW;info.rect=bounds;
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
        // V2 is the size both comctl32 v5 and v6 accept; the v6 size would be
        // refused by a binary that runs without the manifest.
        TTTOOLINFOW info{};info.cbSize=TTTOOLINFOW_V2_SIZE;info.uFlags=TTF_IDISHWND|TTF_SUBCLASS;info.hwnd=dialog;
        info.uId=reinterpret_cast<UINT_PTR>(control);info.lpszText=text.back()->data();
        SendMessageW(host,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
    }
    // ---- Settings dialogs: design units, fonts, anchoring ----------------
    //
    // Every settings dialog is laid out in design units - 96-dpi pixels - and
    // placed through Sd at the dpi of the monitor it opens on. They used to be
    // raw pixels in DEFAULT_GUI_FONT with an unscaled client, so at 200% a
    // 466x502 dialog was a quarter of the size it was designed to be.
    static int Sd(HWND h,int value){return MulDiv(value,static_cast<int>(ActiveWindowDpi(h)),USER_DEFAULT_SCREEN_DPI);}
    // How a control follows its dialog when the dialog is resized, recorded
    // on the control when it is made. The old rule guessed the role from the
    // control's pixel width, which stops working the moment widths scale.
    enum class DialogAnchor:int{Fixed=0,StretchTrack,RightValue,StretchNote,BottomRight};
    static constexpr const wchar_t* kDialogAnchorProperty=L"DLSSVideo.DialogAnchor";
    HFONT DialogFont(HWND h){
        const auto found=m_dialogFonts.find(h);if(found!=m_dialogFonts.end())return found->second;
        HFONT font=CreateUiFont(12,ActiveWindowDpi(h));
        if(font)m_dialogFonts[h]=font;
        return font?font:static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    HWND DialogControl(HWND h,const wchar_t* className,const wchar_t* text,DWORD style,int x,int y,int width,int height,int id=0,DialogAnchor anchor=DialogAnchor::Fixed){
        HWND control=CreateWindowExW(0,className,text,WS_CHILD|WS_VISIBLE|style,Sd(h,x),Sd(h,y),Sd(h,width),Sd(h,height),h,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
        if(!control)return nullptr;
        ApplyDarkControlTheme(control);
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(DialogFont(h)),TRUE);
        if(anchor!=DialogAnchor::Fixed)SetPropW(control,kDialogAnchorProperty,reinterpret_cast<HANDLE>(static_cast<INT_PTR>(anchor)));
        return control;
    }
    // Owner-drawn so it can be dark (DrawDarkPushButton); `isDefault` is what
    // BS_DEFPUSHBUTTON said before and is drawn in the accent.
    HWND DialogButton(HWND h,const wchar_t* textKey,int id,int x,int y,int width,int height,bool isDefault=false){
        HWND button=DialogControl(h,L"BUTTON",T(textKey).c_str(),WS_TABSTOP|BS_OWNERDRAW,x,y,width,height,id,DialogAnchor::BottomRight);
        if(isDefault)MarkDefaultButton(button);
        return button;
    }
    // `origin` is the track position where the setting does nothing - the fill
    // runs from it (DrawDialogSlider) - and `x` shifts the row into a second
    // column. A row in a column layout (`columned`, or any shifted row) keeps
    // its size when the dialog is resized, so it never grows into its neighbour.
    void CreateAdjustmentRow(HWND h,int id,const wchar_t* labelKey,int y,const wchar_t* tipKey=nullptr,int origin=-1,int x=0,bool columned=false){
        const bool column=columned||x!=0;
        HWND label=DialogControl(h,L"STATIC",T(labelKey).c_str(),SS_LEFT,16+x,y,116,20);
        HWND track=DialogControl(h,TRACKBAR_CLASSW,L"",TBS_HORZ|TBS_NOTICKS,132+x,y-6,column?218:236,30,id,column?DialogAnchor::Fixed:DialogAnchor::StretchTrack);
        HWND value=DialogControl(h,L"STATIC",L"",SS_RIGHT,(column?352:370)+x,y,64,20,id+100,column?DialogAnchor::Fixed:DialogAnchor::RightValue);
        if(track&&origin>=0)SetPropW(track,kSliderOriginProperty,reinterpret_cast<HANDLE>(static_cast<INT_PTR>(origin+1)));
        AddTip(h,label,tipKey);AddTip(h,track,tipKey);AddTip(h,value,tipKey);
    }

    // Snapshots each child's position in client coordinates the moment a
    // resizable settings dialog finishes building its controls, so later
    // WM_SIZE handling has a stable baseline to reposition from instead of
    // compounding drift onto whatever the previous resize left behind.
    void CaptureSettingsDesignLayout(HWND h){
        SettingsDesignLayout items;items.dpi=ActiveWindowDpi(h);
        EnumChildWindows(h,[](HWND child,LPARAM lp)->BOOL{
            RECT r{};GetWindowRect(child,&r);
            POINT pts[2]={{r.left,r.top},{r.right,r.bottom}};
            MapWindowPoints(nullptr,GetParent(child),pts,2);
            reinterpret_cast<SettingsDesignLayout*>(lp)->items.push_back({child,RECT{pts[0].x,pts[0].y,pts[1].x,pts[1].y}});
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
        // Margins are design units at the dpi the layout was captured at.
        const auto S=[&](int value){return MulDiv(value,static_cast<int>(it->second.dpi),USER_DEFAULT_SCREEN_DPI);};
        for(const auto&[child,design]:it->second.items){
            if(!IsWindow(child))continue;
            const int dx=int(design.left),dy=int(design.top),dw=int(design.right-design.left),dh=int(design.bottom-design.top);
            switch(static_cast<DialogAnchor>(reinterpret_cast<INT_PTR>(GetPropW(child,kDialogAnchorProperty)))){
            // A trackbar grows with the window, leaving room for its value label.
            case DialogAnchor::StretchTrack:SetWindowPos(child,nullptr,0,0,std::max(S(20),W-dx-S(86)),dh,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE);break;
            // Its value label keeps its width and follows the right edge.
            case DialogAnchor::RightValue:SetWindowPos(child,nullptr,W-S(16)-dw,dy,0,0,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOSIZE);break;
            // A note stretches with the window, left edge fixed.
            case DialogAnchor::StretchNote:SetWindowPos(child,nullptr,dx,dy,std::max(S(20),W-S(32)),dh,SWP_NOZORDER|SWP_NOACTIVATE);break;
            case DialogAnchor::BottomRight:SetWindowPos(child,nullptr,dx+(W-S(designWidth)),H-(S(designHeight)-dy),0,0,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOSIZE);break;
            case DialogAnchor::Fixed:break;
            }
        }
        InvalidateRect(h,nullptr,TRUE);
    }
    // A settings dialog dragged to a monitor at another dpi: the controls,
    // the captured design layout and the font follow it, and then the dialog
    // takes the rectangle Windows suggests, which lays the anchored controls
    // out again through WM_SIZE.
    void RescaleSettingsDialog(HWND h,UINT dpi,const RECT* suggested){
        auto layout=m_settingsDesignLayout.find(h);
        const UINT from=layout!=m_settingsDesignLayout.end()?layout->second.dpi:ActiveWindowDpi(h);
        if(from==dpi&&!suggested)return;
        HFONT font=CreateUiFont(12,dpi);
        ScaleChildWindows(h,from,dpi,font);
        if(font){const auto old=m_dialogFonts.find(h);if(old!=m_dialogFonts.end())DeleteObject(old->second);m_dialogFonts[h]=font;}
        if(layout!=m_settingsDesignLayout.end()){
            for(auto& item:layout->second.items)item.second=dark_mode::ScaleRectForDpi(item.second,from,dpi);
            layout->second.dpi=dpi;
        }
        if(const auto host=m_tipHosts.find(h);host!=m_tipHosts.end())SendMessageW(host->second,TTM_SETMAXTIPWIDTH,0,MulDiv(420,int(dpi),USER_DEFAULT_SCREEN_DPI));
        if(suggested)SetWindowPos(h,nullptr,suggested->left,suggested->top,suggested->right-suggested->left,suggested->bottom-suggested->top,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    // What the four settings dialogs share before their own messages: the dark
    // colours, the owner-drawn buttons, a change of dpi, and the font they own.
    bool SettingsDialogChrome(HWND h,UINT m,WPARAM w,LPARAM l,LRESULT& result){
        switch(m){
        case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORBTN:case WM_CTLCOLORLISTBOX:case WM_CTLCOLORDLG:
            result=reinterpret_cast<LRESULT>(DarkControlColor(m,w,dark_mode::Text));return true;
        case WM_DRAWITEM:{
            const auto* item=reinterpret_cast<const DRAWITEMSTRUCT*>(l);
            if(item&&item->CtlType==ODT_STATIC){DrawDialogHeading(*item);result=TRUE;return true;}
            if(!item||item->CtlType!=ODT_BUTTON)return false;
            DrawDarkPushButton(*item);result=TRUE;return true;
        }
        case WM_NOTIFY:{
            const auto* header=reinterpret_cast<const NMHDR*>(l);
            if(!header||header->code!=NM_CUSTOMDRAW)return false;
            wchar_t kind[32]{};GetClassNameW(header->hwndFrom,kind,int(std::size(kind)));
            if(std::wstring_view(kind)!=TRACKBAR_CLASSW)return false;
            const auto* draw=reinterpret_cast<const NMCUSTOMDRAW*>(l);
            if(draw->dwDrawStage!=CDDS_PREPAINT){result=CDRF_SKIPDEFAULT;return true;}
            DrawDialogSlider(*draw);result=CDRF_SKIPDEFAULT;return true;
        }
        case WM_DPICHANGED:
            if(const auto font=m_dialogHeadingFonts.find(h);font!=m_dialogHeadingFonts.end()){DeleteObject(font->second);m_dialogHeadingFonts.erase(font);}
            RescaleSettingsDialog(h,HIWORD(w),reinterpret_cast<const RECT*>(l));result=0;return true;
        case WM_DESTROY:RemoveChildProperties(h,{kDefaultButtonProperty,kDialogAnchorProperty,kSliderOriginProperty});return false;
        case WM_NCDESTROY:
            if(const auto font=m_dialogFonts.find(h);font!=m_dialogFonts.end()){DeleteObject(font->second);m_dialogFonts.erase(font);}
            if(const auto font=m_dialogHeadingFonts.find(h);font!=m_dialogHeadingFonts.end()){DeleteObject(font->second);m_dialogHeadingFonts.erase(font);}
            return false;
        default:return false;
        }
    }

    // Resizable settings dialogs share this style/rect math: the caller
    // passes the design client size, and gets back a window rect sized so
    // its client area equals that design size under the given styles.
    static RECT SettingsWindowRect(int clientW,int clientH,DWORD style,DWORD exStyle,UINT dpi){
        // The design client is in 96-dpi units, so it is scaled first; then the
        // frame is added at the same dpi. The process is PER_MONITOR_AWARE_V2,
        // so AdjustWindowRectEx alone reports 96-dpi frame metrics and leaves
        // the client short on a scaled monitor - the bottom-anchored buttons
        // then overlap the note text.
        const int d=static_cast<int>(dpi==0?USER_DEFAULT_SCREEN_DPI:dpi);
        RECT rc{0,0,MulDiv(clientW,d,USER_DEFAULT_SCREEN_DPI),MulDiv(clientH,d,USER_DEFAULT_SCREEN_DPI)};
        AdjustWindowRectForDpi(rc,style,FALSE,exStyle,dpi);return rc;
    }

    // Each row's origin is its neutral position (SyncAdjustmentControls'
    // mapping of the default ColorSettings), so a row at its default shows no
    // fill and a changed one shows how far it moved and which way.
    void BuildAdjustmentControls(HWND h){
        CreateSettingsGroupHeading(h,L"adjustments.group_picture",12);
        CreateAdjustmentRow(h,IDC_ADJ_BRIGHTNESS,L"adjustments.brightness",40,nullptr,200);
        CreateAdjustmentRow(h,IDC_ADJ_CONTRAST,L"adjustments.contrast",82,nullptr,100);
        CreateAdjustmentRow(h,IDC_ADJ_SATURATION,L"adjustments.saturation",124,nullptr,100);
        CreateAdjustmentRow(h,IDC_ADJ_GAMMA,L"adjustments.gamma",166,nullptr,100);
        CreateAdjustmentRow(h,IDC_ADJ_TEMPERATURE,L"adjustments.temperature",208,nullptr,100);
        CreateAdjustmentRow(h,IDC_ADJ_TINT,L"adjustments.tint",250,nullptr,100);
        CreateSettingsGroupHeading(h,L"adjustments.group_compare",292);
        CreateAdjustmentRow(h,IDC_ADJ_NEURAL_STRENGTH,L"adjustments.neural_strength",320,L"adjustments.neural_strength.tip",100);
        DialogControl(h,L"STATIC",T(L"adjustments.note").c_str(),SS_LEFT,16,360,418,38,0,DialogAnchor::StretchNote);
        DialogButton(h,L"adjustments.reset",IDC_ADJ_RESET,252,404,86,30);
        DialogButton(h,L"adjustments.close",IDC_ADJ_CLOSE,348,404,86,30,true);
        SyncAdjustmentControls(h);
        CaptureSettingsDesignLayout(h);
    }

    static constexpr int kAdjustDesignW=466,kAdjustDesignH=448;

    void ShowAdjustments(){
        if(m_adjustWnd&&IsWindow(m_adjustWnd)){ShowWindow(m_adjustWnd,SW_SHOWNORMAL);SetForegroundWindow(m_adjustWnd);return;}
        // Registered here, next to the only CreateWindowExW that names it, exactly like
        // the neural and encoder dialogs: the startup registration this replaced still
        // named the class V11 while this call asked for V12, so the window was never
        // created and the whole dialog was unreachable.
        static constexpr const wchar_t* kClassName=L"DLSSVideoAdjustmentsClassV12";
        WNDCLASSW a{};a.lpfnWndProc=AdjustWndProcStatic;a.hInstance=GetModuleHandleW(nullptr);a.lpszClassName=kClassName;a.hCursor=LoadCursor(nullptr,IDC_ARROW);a.hbrBackground=DarkDialogBrush();
        if(!RegisterClassW(&a)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
        constexpr DWORD style=(WS_OVERLAPPEDWINDOW&~WS_MAXIMIZEBOX)|WS_VISIBLE;
        const RECT wr=SettingsWindowRect(kAdjustDesignW,kAdjustDesignH,style,WS_EX_TOOLWINDOW,ActiveWindowDpi(m_hwnd));
        const int w=int(wr.right-wr.left),h=int(wr.bottom-wr.top);
        RECT pr{};GetWindowRect(m_hwnd,&pr);const int pw=int(pr.right-pr.left),ph=int(pr.bottom-pr.top);int x=int(pr.left)+std::max(0,(pw-w)/2),y=int(pr.top)+std::max(0,(ph-h)/2);
        m_adjustWnd=CreateWindowExW(WS_EX_TOOLWINDOW,kClassName,T(L"adjustments.title").c_str(),
            style,x,y,w,h,m_hwnd,nullptr,GetModuleHandleW(nullptr),this);
    }

    LRESULT AdjustWndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        if(LRESULT result=0;SettingsDialogChrome(h,m,w,l,result))return result;
        switch(m){
        case WM_CREATE:ApplyDarkTitleBar(h);BuildAdjustmentControls(h);return 0;
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
        // Skin structure reads Off rather than a number the runtime ignores, and
        // says why it is greyed out when Automatic mask is off - the only state
        // in which no value of it changes the picture.
        SetAdjustmentValue(h,IDC_NS_SKIN,!m_neuralSettings.autoMask?T(L"neural.settings.skin_needs_mask")
            :skin_structure::IsOff(m_neuralSettings.skinStructure)?T(L"neural.settings.skin_off")
            :PlainValue(m_neuralSettings.skinStructure));
        SetAdjustmentValue(h,IDC_NS_COLOR,PlainValue(m_neuralSettings.colorStrength));
    }

    void SyncNeuralSettingControls(HWND h){
        SetTrack(h,IDC_NS_INTENSITY,0,200,int(std::lround(m_neuralSettings.intensity*100.0f)));
        SetTrack(h,IDC_NS_STRUCTURE,0,200,int(std::lround(m_neuralSettings.localStructure*100.0f)));
        SetTrack(h,IDC_NS_TONE,0,200,int(std::lround(m_neuralSettings.localTone*100.0f)));
        SetTrack(h,IDC_NS_SKIN,0,skin_structure::kSliderMax,skin_structure::SliderPosition(m_neuralSettings.skinStructure));
        SetTrack(h,IDC_NS_COLOR,0,100,int(std::lround(m_neuralSettings.colorStrength*100.0f)));
        const auto select=[&](int id,int index){if(HWND combo=GetDlgItem(h,id))SendMessageW(combo,CB_SETCURSEL,static_cast<WPARAM>(index),0);};
        select(IDC_NS_STYLE,std::clamp(m_neuralSettings.style,0,2));
        // The combo lists 1..4 and the setting IS the pass count, so the
        // index is one below it.
        select(IDC_NS_PASSES,std::clamp(m_neuralSettings.passes,1,4)-1);
        // The combo lists the rungs in the enum's own order, so the index IS the rung.
        select(IDC_NS_SCENE_CUTS,static_cast<int>(m_temporalSettings.sceneCuts));
        select(IDC_NS_STABILITY,static_cast<int>(m_temporalSettings.stability));
        const auto check=[&](int id,bool on){if(HWND box=GetDlgItem(h,id))SendMessageW(box,BM_SETCHECK,on?BST_CHECKED:BST_UNCHECKED,0);};
        check(IDC_NS_AUTOMASK,m_neuralSettings.autoMask);check(IDC_NS_GUIDE_MV,m_renderGuides.motionVectors);check(IDC_NS_GUIDE_DEPTH,m_renderGuides.depth);
        check(IDC_NS_CHAINED,m_neuralSettings.chainedHistory);
        // Chained history only governs passes 2+, so it is dead UI at one
        // pass rather than a setting that quietly does nothing.
        if(HWND chained=GetDlgItem(h,IDC_NS_CHAINED))EnableWindow(chained,m_neuralSettings.passes>1);
        EnableDependentNeuralControls(h);
        UpdateNeuralSettingValueLabels(h);
    }

    void ReadNeuralSettingControls(HWND h){
        auto pos=[&](int id)->int{HWND t=GetDlgItem(h,id);return t?int(SendMessageW(t,TBM_GETPOS,0,0)):0;};
        auto sel=[&](int id,int fallback)->int{HWND c=GetDlgItem(h,id);const int index=c?int(SendMessageW(c,CB_GETCURSEL,0,0)):CB_ERR;return index==CB_ERR?fallback:index;};
        auto checked=[&](int id)->bool{HWND b=GetDlgItem(h,id);return b&&SendMessageW(b,BM_GETCHECK,0,0)==BST_CHECKED;};
        m_neuralSettings.intensity=float(pos(IDC_NS_INTENSITY))/100.0f;
        m_neuralSettings.localStructure=float(pos(IDC_NS_STRUCTURE))/100.0f;
        m_neuralSettings.localTone=float(pos(IDC_NS_TONE))/100.0f;
        m_neuralSettings.skinStructure=skin_structure::FromSliderPosition(pos(IDC_NS_SKIN));
        m_neuralSettings.colorStrength=float(pos(IDC_NS_COLOR))/100.0f;
        m_neuralSettings.style=sel(IDC_NS_STYLE,m_neuralSettings.style);
        m_neuralSettings.autoMask=checked(IDC_NS_AUTOMASK);
        m_neuralSettings.passes=std::clamp(sel(IDC_NS_PASSES,m_neuralSettings.passes-1)+1,1,4);
        m_neuralSettings.chainedHistory=checked(IDC_NS_CHAINED);
        if(HWND chained=GetDlgItem(h,IDC_NS_CHAINED))EnableWindow(chained,m_neuralSettings.passes>1);
        EnableDependentNeuralControls(h);
        const GuideControls guides{checked(IDC_NS_GUIDE_MV),checked(IDC_NS_GUIDE_DEPTH)};
        TemporalSettings temporal=m_temporalSettings;
        temporal.sceneCuts=static_cast<scene_cut::Sensitivity>(std::clamp(sel(IDC_NS_SCENE_CUTS,static_cast<int>(temporal.sceneCuts)),0,3));
        temporal.stability=static_cast<TemporalStability>(std::clamp(sel(IDC_NS_STABILITY,static_cast<int>(temporal.stability)),0,3));
        if(guides!=m_renderGuides||temporal!=m_temporalSettings){m_renderGuides=guides;m_temporalSettings=temporal;ApplyLiveGuideControls();}
        UpdateNeuralSettingValueLabels(h);
        // Sliders fire continuously; the preview waits for them to settle.
        SchedulePausedSettingsPreview();
    }

    // Skin structure acts only on what Automatic mask finds: measured on 6.5.3,
    // with the mask off every skin value renders the mask-off picture byte for
    // byte. So its slider is greyed out then, keeping the chosen value for when
    // the mask comes back, and its value label says what it is waiting for.
    void EnableDependentNeuralControls(HWND h){
        if(HWND skin=GetDlgItem(h,IDC_NS_SKIN))EnableWindow(skin,m_neuralSettings.autoMask);
    }

    // The persisted guide switches also drive the live (non-cached) guide
    // generator so the debug views reflect them without a re-render.
    void ApplyLiveGuideControls(){m_guides.SetControls(m_renderGuides);m_guides.SetSceneCutSensitivity(m_temporalSettings.sceneCuts);m_guideReset=true;m_dlssReset=true;UpdateTitle();}

    void CreateNeuralCombo(HWND h,int id,const wchar_t* labelKey,int y,std::initializer_list<const wchar_t*> items,const wchar_t* tipKey=nullptr,int x=0){
        HWND label=DialogControl(h,L"STATIC",T(labelKey).c_str(),SS_LEFT,16+x,y,116,20);
        HWND combo=DialogControl(h,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST,132+x,y-3,160,200,id);
        for(const wchar_t* item:items)SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(item));
        AddTip(h,label,tipKey);AddTip(h,combo,tipKey);
    }

    HWND CreateNeuralCheck(HWND h,int id,const wchar_t* labelKey,int x,int y,int width,const wchar_t* tipKey=nullptr){
        HWND box=DialogControl(h,L"BUTTON",T(labelKey).c_str(),WS_TABSTOP|BS_AUTOCHECKBOX,x,y,width,22,id);
        AddTip(h,box,tipKey);return box;
    }

    // The render preset is deliberately absent: re-measured on RenoDX 6.5.3 it
    // still changes nothing (0 differing bytes for 0->1 and 0->3 on two clips),
    // while every change still costs a full re-render. It remains in
    // NeuralSettings and in DLSSVideoPlayer.ini so runtime-comparison work can
    // still drive it. So do the add-on's NRGlobalTone and NRUICorrection, which
    // the player does not write at all: both measured inert on the same clips.
    // See docs/measurements/knobs-653-20260924/REPORT.md.
    // A heading over each block of controls. Nine controls at one visual level
    // is a list; three named groups is a structure, and the Gestalt common
    // region is the whole reason a heading works - it tells you which controls
    // answer the same question before you read any of their labels. "Look" is
    // what the model does to the picture, "Quality and render time" is what it
    // costs, "Guides" is what it is given to work from.
    //
    // Drawn as a label, not as one more line of text (DrawDialogHeading): small,
    // semibold, upper case and tracked, in the quiet text colour, with a
    // hairline running on to the column's edge - DESIGN.md's label voice, so
    // the three or four groups read as structure before any control does.
    void CreateSettingsGroupHeading(HWND h,const wchar_t* key,int y,int x=0,int width=436){
        DialogControl(h,L"STATIC",T(key).c_str(),SS_OWNERDRAW,16+x,y,width,18);
    }
    HFONT DialogHeadingFont(HWND h){
        if(const auto found=m_dialogHeadingFonts.find(h);found!=m_dialogHeadingFonts.end())return found->second;
        HFONT font=CreateFontW(-Sd(h,11),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        if(font)m_dialogHeadingFonts[h]=font;
        return font?font:DialogFont(h);
    }
    void DrawDialogHeading(const DRAWITEMSTRUCT& item){
        const HWND dialog=GetParent(item.hwndItem);
        FillRect(item.hDC,&item.rcItem,DarkDialogBrush());
        wchar_t text[128]{};GetWindowTextW(item.hwndItem,text,int(std::size(text)));
        for(wchar_t& character:text)character=static_cast<wchar_t>(towupper(character));
        const HGDIOBJ oldFont=SelectObject(item.hDC,DialogHeadingFont(dialog));
        const int oldExtra=SetTextCharacterExtra(item.hDC,std::max(1,Sd(dialog,1)));
        SetBkMode(item.hDC,TRANSPARENT);SetTextColor(item.hDC,dark_mode::QuietText);
        RECT r=item.rcItem;DrawTextW(item.hDC,text,-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        // Measured with the tracking in effect: DT_CALCRECT leaves the extra
        // spacing out, and the rule then started inside the last word.
        SIZE extent{};GetTextExtentPoint32W(item.hDC,text,int(wcslen(text)),&extent);
        SetTextCharacterExtra(item.hDC,oldExtra);SelectObject(item.hDC,oldFont);
        const LONG ruleLeft=item.rcItem.left+extent.cx+Sd(dialog,10),mid=(item.rcItem.top+item.rcItem.bottom)/2;
        if(ruleLeft<item.rcItem.right){RECT rule{ruleLeft,mid,item.rcItem.right,mid+1};HBRUSH line=CreateSolidBrush(dark_mode::Rule);FillRect(item.hDC,&rule,line);DeleteObject(line);}
    }

    // Two columns: what the model does to the picture on the left, what it
    // costs and what it works from on the right. One column was 756 dip tall,
    // 1,323 px at 175%, taller than a 1080p screen's work area; this is 418.
    static constexpr int kNeuralColumn=448;
    void BuildNeuralSettingControls(HWND h){
        constexpr int right=kNeuralColumn;
        CreateSettingsGroupHeading(h,L"neural.settings.group_look",12,0,400);
        CreateAdjustmentRow(h,IDC_NS_INTENSITY,L"neural.settings.intensity",40,L"neural.tip.intensity",100,0,true);
        CreateAdjustmentRow(h,IDC_NS_STRUCTURE,L"neural.settings.structure",82,L"neural.tip.structure",100,0,true);
        CreateAdjustmentRow(h,IDC_NS_TONE,L"neural.settings.tone",124,L"neural.tip.tone",100,0,true);
        CreateAdjustmentRow(h,IDC_NS_SKIN,L"neural.settings.skin",166,L"neural.tip.skin",0,0,true);
        CreateAdjustmentRow(h,IDC_NS_COLOR,L"neural.settings.color",208,L"neural.tip.color",100,0,true);
        CreateNeuralCombo(h,IDC_NS_STYLE,L"neural.settings.style",250,{L"Default",L"Natural",L"Cinematic"},L"neural.tip.style");
        CreateNeuralCheck(h,IDC_NS_AUTOMASK,L"neural.settings.automask",132,286,236,L"neural.tip.automask");
        // Stacking, which arrived with RenoDX 6.x. Its own group because it
        // costs render time rather than changing the model's look: a second
        // pass measured 780,048 -> 932,019 bytes of output over the same
        // 72-frame range and took 9.81 s against 8.01 s.
        CreateSettingsGroupHeading(h,L"neural.settings.group_cost",12,right,400);
        CreateNeuralCombo(h,IDC_NS_PASSES,L"neural.settings.passes",40,{L"1 (single pass)",L"2 passes",L"3 passes",L"4 passes"},L"neural.tip.passes",right);
        CreateNeuralCheck(h,IDC_NS_CHAINED,L"neural.settings.chained",132+right,74,284,L"neural.tip.chained");
        CreateSettingsGroupHeading(h,L"neural.settings.group_guides",112,right,400);
        CreateNeuralCheck(h,IDC_NS_GUIDE_MV,L"neural.settings.guide_mv",132+right,138,116,L"neural.tip.guide_mv");
        CreateNeuralCheck(h,IDC_NS_GUIDE_DEPTH,L"neural.settings.guide_depth",252+right,138,80,L"neural.tip.guide_depth");
        // What the render does across time rather than to one frame: when a cut
        // resets the history. A ladder, not a slider - every rung is a measured
        // point (SceneCut.h), and Default is the one labelled recommended.
        CreateSettingsGroupHeading(h,L"neural.settings.group_temporal",176,right,400);
        {
            const std::wstring cuts[]={T(L"neural.scene_cuts.default"),T(L"neural.scene_cuts.more"),
                                       T(L"neural.scene_cuts.less"),T(L"neural.scene_cuts.off")};
            CreateNeuralCombo(h,IDC_NS_SCENE_CUTS,L"neural.settings.scene_cuts",204,
                              {cuts[0].c_str(),cuts[1].c_str(),cuts[2].c_str(),cuts[3].c_str()},L"neural.tip.scene_cuts",right);
            // A ladder with Off first and the default, for the reason the policy
            // header gives: it trades detail in motion for steadiness, and a
            // default never moves down a ladder to buy something else.
            const std::wstring stability[]={T(L"neural.stability.off"),T(L"neural.stability.low"),
                                            T(L"neural.stability.medium"),T(L"neural.stability.high")};
            CreateNeuralCombo(h,IDC_NS_STABILITY,L"neural.settings.stability",238,
                              {stability[0].c_str(),stability[1].c_str(),stability[2].c_str(),stability[3].c_str()},L"neural.tip.stability",right);
        }
        DialogControl(h,L"STATIC",T(L"neural.settings.note").c_str(),SS_LEFT,16,326,848,38,0,DialogAnchor::StretchNote);
        HWND reset=DialogButton(h,L"neural.settings.reset",IDC_NS_RESET,550,374,86,30);
        HWND apply=DialogButton(h,L"neural.settings.apply",IDC_NS_APPLY,646,374,122,30,true);
        DialogButton(h,L"neural.settings.close",IDC_NS_CLOSE,778,374,86,30);
        AddTip(h,reset,L"neural.tip.reset");AddTip(h,apply,L"neural.tip.apply");
        SyncNeuralSettingControls(h);
        CaptureSettingsDesignLayout(h);
    }

    static constexpr int kNeuralDesignW=880,kNeuralDesignH=418;

    // Playback takes it on the next frame (the renderer reads it per frame, and a
    // paused frame is drawn again so the picture shows it); an export takes it when
    // it starts. Nothing cached depends on it.
    void SetUpscalingHistory(UpscalingHistory history){
        if(history==m_upscalingHistory){SyncFeatureMenuState();return;}
        m_upscalingHistory=history;
        LOG("Super Resolution history set to "<<UpscalingHistoryName(history)<<".");
        SaveVideoSettings();
        if(m_renderer){
            m_renderer->SetUpscalingHistory(history);
            if(!m_playing&&UpscalingActive())if(const auto last=m_lastPlaybackFrame)RenderVideoFrame(*last,false);
        }
        if(m_exportStagesWnd&&IsWindow(m_exportStagesWnd))SyncExportStageControls(m_exportStagesWnd);
        SyncFeatureMenuState();
    }

    // Like a preset: the next render takes it, a paused frame re-previews with
    // it, and a render already running finishes at the scale it started with.
    void SetProcessingScale(uint32_t percent){
        if(!IsProcessingScaleRung(percent))return;
        if(percent==m_processingScale){SyncFeatureMenuState();return;}
        m_processingScale=percent;
        LOG("Processing scale set to "<<percent<<"%.");
        SaveVideoSettings();
        SchedulePausedSettingsPreview();
        SyncFeatureMenuState();
    }

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
        WNDCLASSW n{};n.lpfnWndProc=NeuralWndProcStatic;n.hInstance=GetModuleHandleW(nullptr);n.lpszClassName=kClassName;n.hCursor=LoadCursor(nullptr,IDC_ARROW);n.hbrBackground=DarkDialogBrush();
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
               (m_cachedSettings!=m_neuralSettings||m_cachedGuides!=m_renderGuides||m_cachedTemporal!=m_temporalSettings);
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
        if(LRESULT result=0;SettingsDialogChrome(h,m,w,l,result))return result;
        switch(m){
        case WM_CREATE:ApplyDarkTitleBar(h);BuildNeuralSettingControls(h);return 0;
        case WM_HSCROLL:ReadNeuralSettingControls(h);return 0;
        case WM_GETMINMAXINFO:{
            const RECT wr=SettingsWindowRect(kNeuralDesignW,kNeuralDesignH,DWORD(GetWindowLongPtrW(h,GWL_STYLE)),DWORD(GetWindowLongPtrW(h,GWL_EXSTYLE)),ActiveWindowDpi(h));
            auto* mmi=reinterpret_cast<MINMAXINFO*>(l);mmi->ptMinTrackSize={wr.right-wr.left,wr.bottom-wr.top};return 0;
        }
        case WM_SIZE:ResizeSettingsChildren(h,kNeuralDesignW,kNeuralDesignH);return 0;
        case WM_COMMAND:{
            const int id=LOWORD(w);const int code=HIWORD(w);
            if(id==IDC_NS_RESET){m_neuralSettings={};m_renderGuides={};m_temporalSettings={};ApplyLiveGuideControls();SyncNeuralSettingControls(h);SaveVideoSettings();SchedulePausedSettingsPreview();return 0;}
            if(id==IDC_NS_APPLY){ApplyNeuralSettings();return 0;}
            if(id==IDC_NS_CLOSE){DestroyWindow(h);return 0;}
            // Every combo and box the dialog builds has to be named here or it
            // is drawn, movable and inert: the control changes, nothing reads
            // it back, and the setting the user thinks they picked never
            // reaches the render. Adding a control without adding it to this
            // line is the one mistake this dialog invites, and it is silent.
            if(((id==IDC_NS_STYLE||id==IDC_NS_PASSES||id==IDC_NS_SCENE_CUTS||id==IDC_NS_STABILITY)&&code==CBN_SELCHANGE)||
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
        check(IDC_ES_CAPTURE_DITHER,m_captureDither);
        if(HWND combo=GetDlgItem(h,IDC_ES_CACHE_QUALITY))SendMessageW(combo,CB_SETCURSEL,static_cast<WPARAM>(m_cacheQuality),0);
        // The dither has an 8-bit store to act on only on the Standard rung; greyed,
        // not hidden, so the control still says the switch exists.
        if(HWND box=GetDlgItem(h,IDC_ES_CAPTURE_DITHER))EnableWindow(box,!EncoderQualityIsTenBit(m_cacheQuality));
        check(IDC_ES_DEBAND,m_sourceDeband);
        check(IDC_ES_EXPOSURE,m_suppliedExposure);
    }

    void ReadEncoderSettingControls(HWND h){
        auto sel=[&](int id,int fallback)->int{HWND c=GetDlgItem(h,id);const int index=c?int(SendMessageW(c,CB_GETCURSEL,0,0)):CB_ERR;return index==CB_ERR?fallback:index;};
        auto checked=[&](int id)->bool{HWND b=GetDlgItem(h,id);return b&&SendMessageW(b,BM_GETCHECK,0,0)==BST_CHECKED;};
        // Only the next conversion reads these, so there is nothing to re-render for them.
        m_gpuColorConversion=checked(IDC_ES_GPU_CONVERT);
        m_gpuSourceConversion=checked(IDC_ES_GPU_SOURCE);
        m_nvencPreset=uint32_t(sel(IDC_ES_NVENC_PRESET,int(m_nvencPreset)-1))+1;
        m_captureDither=checked(IDC_ES_CAPTURE_DITHER);
        m_cacheQuality=static_cast<EncoderQuality>(std::clamp(sel(IDC_ES_CACHE_QUALITY,int(m_cacheQuality)),0,2));
        if(HWND box=GetDlgItem(h,IDC_ES_CAPTURE_DITHER))EnableWindow(box,!EncoderQualityIsTenBit(m_cacheQuality));
        m_sourceDeband=checked(IDC_ES_DEBAND);
        m_suppliedExposure=checked(IDC_ES_EXPOSURE);
        SaveVideoSettings();
    }

    // Grouped by what each switch acts on, in the order a frame meets them:
    // the colour conversion, the encoder writing the cache, and what the
    // model is given.
    void BuildEncoderSettingControls(HWND h){
        CreateSettingsGroupHeading(h,L"encoder.group_conversion",12);
        CreateNeuralCheck(h,IDC_ES_GPU_CONVERT,L"encoder.settings.gpu_convert",16,38,418,L"encoder.tip.gpu_convert");
        CreateNeuralCheck(h,IDC_ES_GPU_SOURCE,L"encoder.settings.gpu_source",16,62,418,L"encoder.tip.gpu_source");
        CreateSettingsGroupHeading(h,L"encoder.group_cache",98);
        CreateNeuralCombo(h,IDC_ES_NVENC_PRESET,L"encoder.settings.nvenc_preset",126,{L"p1 (fastest)",L"p2",L"p3",L"p4",L"p5",L"p6",L"p7 (best quality)"},L"encoder.tip.nvenc_preset");
        // The rung names say what each one writes; the tooltip carries the measured
        // size and time beside each, the way the preset's does.
        CreateNeuralCombo(h,IDC_ES_CACHE_QUALITY,L"encoder.settings.cache_quality",160,{T(L"encoder.quality.standard").c_str(),T(L"encoder.quality.high").c_str(),T(L"encoder.quality.lossless").c_str()},L"encoder.tip.cache_quality");
        // In design units like every other control: the raw 302 px this was
        // set to was 173 dip at 175%, which cut "CQ 16" off the rung's name.
        if(HWND combo=GetDlgItem(h,IDC_ES_CACHE_QUALITY))SetWindowPos(combo,nullptr,0,0,Sd(h,302),Sd(h,200),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        CreateNeuralCheck(h,IDC_ES_CAPTURE_DITHER,L"encoder.settings.capture_dither",132,192,300,L"encoder.tip.capture_dither");
        CreateSettingsGroupHeading(h,L"encoder.group_model",228);
        CreateNeuralCheck(h,IDC_ES_DEBAND,L"encoder.settings.deband",16,254,418,L"encoder.tip.deband");
        CreateNeuralCheck(h,IDC_ES_EXPOSURE,L"encoder.settings.exposure",16,278,418,L"encoder.tip.exposure");
        // Three lines: at 38 dip the note's third line was cut off.
        DialogControl(h,L"STATIC",T(L"encoder.settings.note").c_str(),SS_LEFT,16,314,418,58,0,DialogAnchor::StretchNote);
        DialogButton(h,L"encoder.settings.reset",IDC_ES_RESET,252,380,86,30);
        DialogButton(h,L"encoder.settings.close",IDC_ES_CLOSE,348,380,86,30,true);
        SyncEncoderSettingControls(h);
        CaptureSettingsDesignLayout(h);
    }

    static constexpr int kEncoderDesignW=466,kEncoderDesignH=424;

    void ShowEncoderSettings(){
        if(m_encoderWnd&&IsWindow(m_encoderWnd)){ShowWindow(m_encoderWnd,SW_SHOWNORMAL);SetForegroundWindow(m_encoderWnd);return;}
        static constexpr const wchar_t* kClassName=L"DLSSVideoEncoderSettingsClassV1";
        WNDCLASSW n{};n.lpfnWndProc=EncoderWndProcStatic;n.hInstance=GetModuleHandleW(nullptr);n.lpszClassName=kClassName;n.hCursor=LoadCursor(nullptr,IDC_ARROW);n.hbrBackground=DarkDialogBrush();
        if(!RegisterClassW(&n)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
        constexpr DWORD style=(WS_OVERLAPPEDWINDOW&~WS_MAXIMIZEBOX)|WS_VISIBLE;
        const RECT wr=SettingsWindowRect(kEncoderDesignW,kEncoderDesignH,style,WS_EX_TOOLWINDOW,ActiveWindowDpi(m_hwnd));
        const int w=int(wr.right-wr.left),h=int(wr.bottom-wr.top);
        RECT pr{};GetWindowRect(m_hwnd,&pr);const int pw=int(pr.right-pr.left),ph=int(pr.bottom-pr.top);int x=int(pr.left)+std::max(0,(pw-w)/2),y=int(pr.top)+std::max(0,(ph-h)/2);
        m_encoderWnd=CreateWindowExW(WS_EX_TOOLWINDOW,kClassName,T(L"encoder.settings.title").c_str(),
            style,x,y,w,h,m_hwnd,nullptr,GetModuleHandleW(nullptr),this);
    }

    LRESULT EncoderWndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        if(LRESULT result=0;SettingsDialogChrome(h,m,w,l,result))return result;
        switch(m){
        case WM_CREATE:ApplyDarkTitleBar(h);BuildEncoderSettingControls(h);return 0;
        case WM_GETMINMAXINFO:{
            const RECT wr=SettingsWindowRect(kEncoderDesignW,kEncoderDesignH,DWORD(GetWindowLongPtrW(h,GWL_STYLE)),DWORD(GetWindowLongPtrW(h,GWL_EXSTYLE)),ActiveWindowDpi(h));
            auto* mmi=reinterpret_cast<MINMAXINFO*>(l);mmi->ptMinTrackSize={wr.right-wr.left,wr.bottom-wr.top};return 0;
        }
        case WM_SIZE:ResizeSettingsChildren(h,kEncoderDesignW,kEncoderDesignH);return 0;
        case WM_COMMAND:{
            const int id=LOWORD(w);const int code=HIWORD(w);
            if(id==IDC_ES_RESET){m_gpuColorConversion=false;m_gpuSourceConversion=false;m_nvencPreset=5;m_captureDither=true;m_cacheQuality=EncoderQuality::Standard;m_sourceDeband=false;m_suppliedExposure=false;SyncEncoderSettingControls(h);SaveVideoSettings();return 0;}
            if(id==IDC_ES_CLOSE){DestroyWindow(h);return 0;}
            // Every control the dialog builds is named here; one left out is drawn and inert.
            if(((id==IDC_ES_GPU_CONVERT||id==IDC_ES_GPU_SOURCE||id==IDC_ES_CAPTURE_DITHER||id==IDC_ES_DEBAND||id==IDC_ES_EXPOSURE)&&code==BN_CLICKED)||((id==IDC_ES_NVENC_PRESET||id==IDC_ES_CACHE_QUALITY)&&code==CBN_SELCHANGE)){ReadEncoderSettingControls(h);return 0;}
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
    static constexpr int kExportDesignW=470,kExportDesignH=402;

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
        WNDCLASSW n{};n.lpfnWndProc=ExportStagesWndProcStatic;n.hInstance=GetModuleHandleW(nullptr);n.lpszClassName=kClassName;n.hCursor=LoadCursor(nullptr,IDC_ARROW);n.hbrBackground=DarkDialogBrush();
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
        CreateSettingsGroupHeading(h,L"export.stages.group_stages",8);
        CreateNeuralCheck(h,IDC_EX_UPSCALE,L"export.stages.upscale",16,32,300,L"export.tip.upscale");
        CreateNeuralCombo(h,IDC_EX_RESOLUTION,L"export.stages.resolution",70,{L"1080p",L"1440p",L"2160p"});
        {
            const std::wstring temporal=T(L"export.stages.history_temporal"),perFrame=T(L"export.stages.history_per_frame");
            CreateNeuralCombo(h,IDC_EX_HISTORY,L"export.stages.history",104,{temporal.c_str(),perFrame.c_str()},L"export.tip.history");
        }
        CreateNeuralCheck(h,IDC_EX_NEURAL,L"export.stages.neural",16,146,300,L"export.tip.neural");
        CreateNeuralCheck(h,IDC_EX_FRAMEGEN,L"export.stages.framegen",16,186,300,L"export.tip.framegen");
        CreateNeuralCombo(h,IDC_EX_MULTIPLIER,L"export.stages.multiplier",224,{L"2×",L"3×",L"4×",L"5×"});
        CreateSettingsGroupHeading(h,L"export.stages.group_result",264);
        DialogControl(h,L"STATIC",L"",SS_LEFT,16,288,436,34,IDC_EX_SUMMARY);
        DialogControl(h,L"STATIC",T(L"export.stages.note").c_str(),SS_LEFT,16,322,436,34);
        DialogButton(h,L"export.stages.run",IDC_EX_RUN,232,364,120,30,true);
        DialogButton(h,L"export.stages.close",IDC_EX_CLOSE,362,364,90,30);
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
        select(IDC_EX_HISTORY,m_upscalingHistory==UpscalingHistory::PerFrame?1:0);
        // A rung you cannot choose and a rate you cannot reach are greyed, not
        // hidden: the control staying visible is what tells the user the stage
        // exists and why it is unavailable here.
        if(HWND c=GetDlgItem(h,IDC_EX_RESOLUTION))EnableWindow(c,m_exportSelection.upscale);
        // Only Super Resolution on its own has a history to choose: with the model
        // on the same carrier the pass keeps Temporal, and the tooltip says why.
        if(HWND c=GetDlgItem(h,IDC_EX_HISTORY))EnableWindow(c,m_exportSelection.upscale&&!m_exportSelection.neural);
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
        // One choice for playback and the export, so the menu and this row agree.
        const UpscalingHistory history=sel(IDC_EX_HISTORY,0)==1?UpscalingHistory::PerFrame:UpscalingHistory::Temporal;
        if(history!=m_upscalingHistory){SetUpscalingHistory(history);return;}
        SyncExportStageControls(h);
    }

    bool ExportStagesBusy()const{return m_exportWorker.joinable()||m_frameGenWorker.joinable()||NeuralJobActive();}

    LRESULT ExportStagesWndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        if(LRESULT result=0;SettingsDialogChrome(h,m,w,l,result))return result;
        switch(m){
        case WM_CREATE:ApplyDarkTitleBar(h);BuildExportStageControls(h);return 0;
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
               ((id==IDC_EX_RESOLUTION||id==IDC_EX_MULTIPLIER||id==IDC_EX_HISTORY)&&code==CBN_SELCHANGE)){ReadExportStageControls(h);return 0;}
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
        const std::filesystem::path destination=PickStageExportFile(m_hwnd,m_displayTitle,m_decoder.IsStillImage(),m_decoder.IsAnimation());
        if(destination.empty())return;

        LOG("Stage export starting: upscale="<<m_exportSelection.upscale
            <<" history="<<UpscalingHistoryName(CarrierUpscalingHistory(m_upscalingHistory,m_exportSelection.neural))
            <<" neural="<<m_exportSelection.neural<<" framegen="<<m_exportSelection.frameGeneration
            <<" output="<<plan.outputWidth<<"x"<<plan.outputHeight<<" fps="<<plan.outputFps
            <<" passes="<<ExportStageCount(plan)<<" source="<<WideToUtf8(source.wstring()));

        StageExportJob job;
        job.plan=plan;job.source=source;job.destination=destination;job.scratch=scratch;
        job.helpers=ExecutableDirectory();
        job.sourceWidth=m_decoder.Width();job.sourceHeight=m_decoder.Height();
        job.fps=m_decoder.FrameRate();job.duration=m_decoder.DurationSeconds();
        job.nvencPreset=m_nvencPreset;job.neuralSettings=m_neuralSettings;job.processingScale=m_processingScale;
        job.upscalingHistory=m_upscalingHistory;
        job.captureDither=m_captureDither;job.quality=m_cacheQuality;job.sourceDeband=m_sourceDeband;job.suppliedExposure=m_suppliedExposure;
        job.holdDuplicates=m_frameGenHoldDuplicates;
        // Called on the export thread, which is safe for a helper that is not
        // thread-safe: RunStageExport calls it only while holding the runtime
        // lease, and a neural job only touches the helper while holding it too.
        // The export thread is joined before this player is destroyed.
        job.releaseResidentHelper=[this]{m_residentHelper.Release();};
        HWND target=m_hwnd;auto* completions=&m_exportCompletions;
        try{
            m_exportWorker=std::jthread([=](std::stop_token stop){
                auto completion=std::make_unique<ExportCompletion>();
                completion->output=job.destination;
                // The worker has always reported frames; this is what listens.
                const StageExportOutcome outcome=RunStageExport(job,stop,[&](const StageExportUpdate& u){
                    auto* update=u.passKey
                        ?new StageExportProgress{true,u.pass,u.passes,u.passKey,u.completedFrames,u.totalFrames,{}}
                        :new StageExportProgress{};
                    if(!PostMessageW(target,WM_STAGE_EXPORT_PROGRESS,0,reinterpret_cast<LPARAM>(update)))delete update;
                });
                const bool done=outcome.status==StageExportStatus::Done;
                completion->result={done,done?MaterializeError::None
                                        :outcome.status==StageExportStatus::Cancelled?MaterializeError::Cancelled
                                        :MaterializeError::ProcessFailed,outcome.detail};
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
        if(app&&m==WM_SETFOCUS){SetFocus(app->m_hwnd);return 0;}
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
            if(m==WM_MBUTTONDOWN){a->RenderMiddleDown(h,l);return 0;}
            if(m==WM_MBUTTONUP){a->RenderMiddleUp(h);return 0;}
            if(m==WM_MOUSELEAVE){a->RenderMouseLeft();return 0;}
            if(m==WM_CAPTURECHANGED){a->RenderCaptureLost();return 0;}
            if(m==WM_LBUTTONDBLCLK){a->ToggleFullscreen();return 0;}
            // The picture never keeps the keyboard. Keys that reached it anyway went to the
            // main window as WM_KEYDOWN, but the WM_CHAR TranslateMessage makes from them
            // stayed here, so ? (a character, not a key) did nothing while F1 worked.
            if(m==WM_SETFOCUS){SetFocus(a->m_hwnd);return 0;}
            if(m==WM_MOUSEWHEEL||m==WM_KEYDOWN||m==WM_SYSKEYDOWN||m==WM_CHAR)return SendMessageW(a->m_hwnd,m,w,l);
            if(m==WM_DROPFILES)return SendMessageW(a->m_hwnd,m,w,l); // main window owns DragFinish().
        }
        return DefWindowProcW(h,m,w,l);
    }

    bool Load(const std::wstring& source,const std::wstring& displayTitle=L"",MediaSourceKind sourceKind=MediaSourceKind::LocalFile) {
        // Every open by name ends what a previous drop said; a drop sets its
        // note again once this returns.
        m_dropNote.Clear();
        // A path that names no file used to reach the render job, whose first
        // step - hashing it - failed as "The source digest could not be
        // computed", which tells nobody the file is missing.
        if(sourceKind==MediaSourceKind::LocalFile){
            std::error_code fileError;
            if(!std::filesystem::is_regular_file(source,fileError)){
                LOG("Open refused: no file at "<<WideToUtf8(source));
                MessageBoxW(m_hwnd,FormatLocalizedText(T(L"open.missing"),source).c_str(),T(L"app.title").c_str(),MB_OK|MB_ICONINFORMATION);
                return false;
            }
        }
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
        m_lastPlaybackFrame={};m_ownedPlaybackFrame.reset();m_upscalingError.clear();m_neuralNotice.clear();m_sourceNotice.clear();m_decodeNotice.clear();m_neuralPath.clear();m_cachedRange={};m_cachedReceiptPath.clear();m_cachedSettings={};m_cachedGuides={};m_cachedTemporal={};m_markers={};m_dragSplit=false;m_renderMouseKnown=false;m_gesture={};m_peekOriginal=false;m_dragMix=false;m_middlePan=false;m_renderTracking=false;
        m_seekPending=false;m_seeking=false;m_layoutMismatchLogged=false;Audio().Stop();m_networkAudio.reset();m_renderer.reset();m_decoder.Close();m_cachedPlayback=false;m_cachedSourceFile=false;m_cachedPresentedFrames=0;m_havePresentedPair=false;ForgetRenderedCachedPair();m_guides.Reset();m_haveNext=false;m_waitingForNetworkFrame=false;m_networkReadState.Reset();m_next=VideoFrame{};m_nextPairFrame.reset();m_loaded=false;m_playing=false;m_currentSec=0;m_lastRenderedTs=-1;m_path.clear();m_youtubeAudioUrl.clear();m_youtubePageUrl.clear();m_displayTitle.clear();m_sourceKind=MediaSourceKind::LocalFile;m_cachedStatus.clear();InvalidateFrameGenerationCopy();
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
                candidate->guides.SetSceneCutSensitivity(m_guides.SceneCutSensitivity());
                // The frame says which layout it is in. Playback opens NV12 for
                // any even-dimension BT.709-limited source, which is most of
                // them, so taking the Bgra default here read past the end of
                // the buffer instead of building guides.
                if(candidate->guides.Generate(last.bgra.data(),last.bgra.size(),m_decoder.Width(),m_decoder.Height(),
                    m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate(),
                    IdentityOf(last,m_historyGeneration,0,HistoryReset::FirstFrame),guide,
                    last.layout)){
                    candidate->renderer->SetNextSourcePq(last.pq);
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
        renderer->SetSourceColor(m_decoder.DecodedColor());
        // Every renderer the player shows presents at its window's size, and may
        // present HDR when the display under it is in HDR mode (SyncHdrPresentation).
        renderer->SetPresentFollowsWindow(true);
        renderer->SetHdrOutputAllowed(true);
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
        // The renderer reads whatever bytes it is handed in the layout it was
        // configured for, and a BGRA frame is larger than the NV12 one it checks
        // for, so a mismatch reaches the screen as stripes rather than an error.
        if(f.layout!=m_renderer->ActiveSourceLayout()){
            if(!m_layoutMismatchLogged)LOG("A "<<(f.layout==VideoPixelLayout::Nv12?"NV12":"BGRA")<<" frame reached a renderer configured for the other layout; refusing it rather than presenting garbage.");
            m_layoutMismatchLogged=true;return false;
        }
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
        // An HDR original decoded for an HDR display is PQ, and says so per frame.
        m_renderer->SetNextSourcePq(f.pq);
        m_renderer->SetUpscalingHistory(m_upscalingHistory);
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

    // The playback clock (PlaybackTickPolicy.h): the presented frame while
    // paused, the audio clock while playing, and the steady clock from the
    // last anchor when there is no audio clock at all.
    double Position() const {
        return playback_tick::Position({m_loaded,m_playing,m_currentSec,m_playStartSec,m_decoder.DurationSeconds()},
                                       [&]{return Audio().PositionSeconds();},
                                       [&]{return std::chrono::duration<double>(Clock::now()-m_playStart).count();});
    }
    // What the tick's decisions read (PlaybackTickPolicy.h), as it is now.
    playback_tick::TickState TickNow()const{
        return {m_loaded,m_playing,m_seeking,m_seekPending,m_cachedPlayback,NetworkPlayback(),m_haveNext};
    }

    // Where a seek may land (SeekPolicy.h): inside the source's last frame,
    // and inside the range a finished cached entry was rendered for.
    double ClampSeek(double sec)const{
        return seek_policy::Clamp(sec,{m_decoder.DurationSeconds(),m_decoder.FrameRate(),LastFramePts(m_decoder.FrameRate(),SourceDuration100ns()),
                                       m_cachedPlayback,m_liveSession,m_cachedRange});
    }

    void RequestSeek(double sec) {
        const bool resume=seek_policy::ResumeAfterRequest(m_seekPending,m_seekResumePlaying,m_playing); RequestSeek(sec,resume);
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
            if(seek_policy::RestartsAudioAfterSeek(m_dragSeek)){const bool audioOk=Audio().Start(m_path,m_currentSec);if(audioOk){Audio().SetVolume(m_muted?0.0f:m_volume);Audio().Pause(!resumeAfter);}}
            m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=seek_policy::PlaysAfterSeek(resumeAfter,true,m_haveNext);m_synchronizedPlayback.SetPaused(!resumeAfter);m_guideReset=false;m_dlssReset=false;SetSeeking(false);UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();return true;
        }
        m_haveNext=false;m_next=VideoFrame{};m_nextPairFrame.reset();
        auto readAt=[&](double target,VideoFrame& frame)->bool{
            if(!m_decoder.SeekSeconds(target))return false;
            if(m_decoder.ReadNext(frame))return true;
            if(const auto safe=seek_policy::RetryTarget(target,m_decoder.DurationSeconds(),m_decoder.FrameRate());safe&&m_decoder.SeekSeconds(*safe)&&m_decoder.ReadNext(frame))return true;
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
        if(seek_policy::RestartsAudioAfterSeek(m_dragSeek)){const bool audioOk=Audio().Start(m_path,m_currentSec);if(audioOk){Audio().SetVolume(m_muted?0.0f:m_volume);Audio().Pause(!resumeAfter);}else LOG("Seek: no audio stream/output; using steady-clock video pacing.");}
        m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=seek_policy::PlaysAfterSeek(resumeAfter,false,m_haveNext);m_guideReset=false;m_dlssReset=false;SetSeeking(false);UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();LOG("Seek complete actual="<<m_currentSec);return true;
    }

    void SetPaused(bool pause){if(!m_loaded||m_seeking)return;if(pause==!m_playing)return;if(pause){m_currentSec=playback_timing::PausePosition(m_currentSec);m_playing=false;Audio().Pause(true);if(m_cachedPlayback)m_synchronizedPlayback.SetPaused(true);}else{if(playback_tick::ResumeRestartsWithSeek(TickNow(),m_decoder.DurationSeconds())){RequestSeek(status_note::PlayRestartSeconds(!m_decodeNotice.empty(),m_currentSec),true);return;}m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=true;ResumeAudio();if(m_cachedPlayback)m_synchronizedPlayback.SetPaused(false);}InvalidateControls();InvalidatePlaybackProgress();}
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
        int areaH=std::max(1,H-ControlHeight());MoveWindow(m_viewport,0,0,W,areaH,TRUE);
        const compare_view::Fit fit=m_onePixel?compare_view::Fit::Pixels:(m_fill?compare_view::Fit::Fill:compare_view::Fit::Fit);
        const RECT picture=compare_view::RenderRect(W,areaH,m_dar,fit,m_renderer?m_renderer->OutputW():0u,m_renderer?m_renderer->OutputH():0u);
        SetWindowPos(m_renderWnd,nullptr,picture.left,picture.top,picture.right-picture.left,picture.bottom-picture.top,SWP_NOZORDER|SWP_NOACTIVATE);
        // A zoom step is pixels per output pixel, so its scale follows the window.
        if(m_renderer)m_renderer->SetComparison(EffectiveComparison());
        ReconcileFocusForCurrentLayout();RefreshHoverForCurrentLayout();InvalidateRect(m_viewport,nullptr,FALSE);InvalidateControls();
    }

    // 18 dip, up from 14, so the coverage lane along its bottom can be 7 dip
    // instead of 3: that lane is the render map, the one thing on this bar no
    // other player has, and at 3 dip it read as a hairline.
    RECT TimelineRect()const{if(!ControlsVisible())return {};RECT c{};GetClientRect(m_hwnd,&c);return RECT{Dip(18),c.bottom-Dip(30),c.right-Dip(18),c.bottom-Dip(12)};}
    // ---- Windows' media controls and the taskbar thumbnail -----------------
    //
    // Both mirror state the player already has; nothing here decides anything
    // (MediaTransportPolicy.h does). Synced with the status line, which every
    // play, pause, seek, load and unload already reaches.
    void SyncMediaTransport(){
        if(!m_hwnd)return;
        const bool playing=m_playing||LiveResumePending();
        if(m_mediaTransport.Attached()){
            const auto status=media_transport::StatusFor(m_loaded,playing);
            if(status!=m_smtcStatus){m_smtcStatus=status;m_mediaTransport.SetStatus(status);}
            const std::wstring title=m_loaded?m_displayTitle:std::wstring{};
            if(title!=m_smtcTitle){m_smtcTitle=title;m_mediaTransport.SetTitle(title);}
            const double duration=m_loaded?m_decoder.DurationSeconds():0.0;
            if(duration>0.0){
                const auto now=std::chrono::steady_clock::now();const double position=Position();
                if(media_transport::ShouldPushTimeline(m_smtcTimeline,position,playing,now)){
                    m_mediaTransport.SetTimeline(duration,position);m_smtcTimeline={true,playing,position,now};
                }
            }else m_smtcTimeline={};
        }
        SyncThumbBar();
    }
    void HandleMediaButton(int button){
        switch(media_transport::ActionForButton(button,m_loaded,m_playing||LiveResumePending())){
        case media_transport::Action::TogglePause:TogglePause();break;
        case media_transport::Action::Stop:StopPlayback();break;
        case media_transport::Action::None:break;
        }
    }
    media_transport::ThumbBar CurrentThumbBar()const{
        const auto neural=ButtonContent(ToolbarAction::ToggleNeuralRendering);
        media_transport::ThumbState state{};
        state.loaded=m_loaded;state.playing=m_playing||LiveResumePending();
        state.neuralAvailable=neural.enabled;state.neuralOn=neural.active;
        state.compareAvailable=ComparisonModesAvailable();state.comparing=SelectedComparisonMode()!=ComparisonMode::Neural;
        return media_transport::ThumbButtonsFor(state,IDM_PLAY,IDM_NEURAL_RENDERING,IDM_COMPARE_TOGGLE);
    }
    // One side-by-side switch for the thumbnail, over the Compare menu's own
    // modes: its tooltip says "Compare side by side", which is now a mode of its
    // own (two panes of one frame), and the neural picture again from any
    // comparison.
    void ToggleSideBySide(){
        if(!ComparisonModesAvailable())return;
        SetComparisonMode(SelectedComparisonMode()==ComparisonMode::Neural?ComparisonMode::SideBySide:ComparisonMode::Neural);
        SyncFeatureMenuState();UpdateCachedStatus();
    }
    HICON ThumbIcon(UiIcon icon){
        const auto found=m_thumbIcons.find(icon);if(found!=m_thumbIcons.end())return found->second;
        const HICON rendered=RenderGlyphIcon(GlyphForIcon(icon),GetSystemMetrics(SM_CXSMICON));
        if(rendered)m_thumbIcons[icon]=rendered;
        return rendered;
    }
    bool EnsureTaskbar(){
        if(!m_taskbar&&!m_taskbarUnavailable){
            const HRESULT created=CoCreateInstance(CLSID_TaskbarList,nullptr,CLSCTX_INPROC_SERVER,
                                                   IID_PPV_ARGS(&m_taskbar));
            const HRESULT initialized=(SUCCEEDED(created)&&m_taskbar)?m_taskbar->HrInit():created;
            LOG("Taskbar: CoCreateInstance="<<HexText(created)<<" HrInit="<<HexText(initialized));
            if(FAILED(created)||!m_taskbar||FAILED(initialized)){m_taskbar.Reset();m_taskbarUnavailable=true;}
        }
        return m_taskbar!=nullptr;
    }
    std::array<THUMBBUTTON,3> ThumbButtonsForShell(const media_transport::ThumbBar& bar){
        std::array<THUMBBUTTON,3> buttons{};
        for(size_t index=0;index<bar.size();++index){
            THUMBBUTTON& button=buttons[index];
            button.dwMask=THB_ICON|THB_TOOLTIP|THB_FLAGS;button.iId=bar[index].command;
            button.hIcon=ThumbIcon(bar[index].icon);
            button.dwFlags=bar[index].enabled?THBF_ENABLED:THBF_DISABLED;
            const std::wstring tip=T(bar[index].tipKey);wcsncpy_s(button.szTip,tip.c_str(),_TRUNCATE);
        }
        return buttons;
    }
    // Explorer announces each taskbar button it makes, including after it
    // restarts, and the thumbnail buttons can only be added after that.
    void CreateThumbBar(){
        if(!m_uiResources.IsLoaded()||!EnsureTaskbar())return;
        m_thumbBar=CurrentThumbBar();
        auto buttons=ThumbButtonsForShell(m_thumbBar);
        const HRESULT added=m_taskbar->ThumbBarAddButtons(m_hwnd,UINT(buttons.size()),buttons.data());
        m_thumbBarCreated=SUCCEEDED(added);
        LOG("Taskbar thumbnail buttons: ThumbBarAddButtons="<<HexText(added));
    }
    void SyncThumbBar(){
        if(!m_thumbBarCreated||!m_taskbar)return;
        const auto bar=CurrentThumbBar();if(bar==m_thumbBar)return;
        m_thumbBar=bar;auto buttons=ThumbButtonsForShell(bar);
        m_taskbar->ThumbBarUpdateButtons(m_hwnd,UINT(buttons.size()),buttons.data());
    }
    static UINT TaskbarButtonCreatedMessage(){static const UINT message=RegisterWindowMessageW(L"TaskbarButtonCreated");return message;}

    // ---- The dark menu bar ------------------------------------------------
    //
    // Drawn through user32's undocumented UAH menu messages (DarkModePolicy.h
    // names them and explains the guard). Anything that does not check out
    // latches this off for the window and hands the bar back to Windows, so
    // the worst case is the light bar it always was.
    bool FailDarkMenuBar(HWND h,const char* why){
        if(!m_darkMenuFailed){m_darkMenuFailed=true;LOG("Dark menu bar disabled for this window: "<<why<<"; drawing the system menu bar.");DrawMenuBar(h);}
        return false;
    }
    bool DrawDarkMenuBar(HWND h,const dark_mode::UAHMENU* menu){
        if(m_darkMenuFailed)return false;
        if(!menu||!menu->hdc||!menu->hmenu||menu->hmenu!=GetMenu(h))return FailDarkMenuBar(h,"the bar message named another menu");
        MENUBARINFO info{sizeof(info)};
        if(!GetMenuBarInfo(h,OBJID_MENU,0,&info))return FailDarkMenuBar(h,"GetMenuBarInfo failed");
        RECT window{};GetWindowRect(h,&window);
        RECT bar=info.rcBar;OffsetRect(&bar,-window.left,-window.top);
        FillRect(menu->hdc,&bar,DarkDialogBrush());
        return true;
    }
    bool DrawDarkMenuBarItem(HWND h,const dark_mode::UAHDRAWMENUITEM* item){
        if(m_darkMenuFailed)return false;
        if(!item||!item->um.hdc||!item->um.hmenu||item->um.hmenu!=GetMenu(h))return FailDarkMenuBar(h,"the item message named another menu");
        wchar_t text[256]{};
        MENUITEMINFOW info{sizeof(info)};info.fMask=MIIM_STRING;info.dwTypeData=text;info.cch=static_cast<UINT>(std::size(text));
        if(item->umi.iPosition<0||!GetMenuItemInfoW(item->um.hmenu,static_cast<UINT>(item->umi.iPosition),TRUE,&info))
            return FailDarkMenuBar(h,"the item message named no item of the bar");
        const auto colors=dark_mode::MenuBarItemColors(item->dis.itemState);
        HBRUSH fill=CreateSolidBrush(colors.fill);FillRect(item->um.hdc,&item->dis.rcItem,fill);DeleteObject(fill);
        SetBkMode(item->um.hdc,TRANSPARENT);SetTextColor(item->um.hdc,colors.text);
        RECT label=item->dis.rcItem;DrawTextW(item->um.hdc,text,-1,&label,dark_mode::MenuBarTextFormat(item->dis.itemState));
        return true;
    }
    // Windows draws a one-pixel light rule between the menu bar and the client
    // area outside both messages above; it is painted over after the frame.
    void PaintMenuBarSeparator(HWND h){
        if(m_darkMenuFailed||!GetMenu(h))return;
        RECT client{};GetClientRect(h,&client);MapWindowPoints(h,nullptr,reinterpret_cast<POINT*>(&client),2);
        RECT window{};GetWindowRect(h,&window);OffsetRect(&client,-window.left,-window.top);
        const RECT line{client.left,client.top-1,client.right,client.top};
        if(HDC dc=GetWindowDC(h)){FillRect(dc,&line,DarkDialogBrush());ReleaseDC(h,dc);}
    }

    // ---- The start screen -------------------------------------------------
    //
    // The idle window's capability check and tiles (StartScreenPolicy.h). The
    // facts that need the disk come from GatherStartScreen on its own thread,
    // asked for whenever the idle screen is up with a recent list it has not
    // asked about; everything else is already in memory.
    start_screen::Facts StartScreenFacts()const{
        start_screen::Facts facts{};
        facts.gpu=m_opt.detectedGpu.description;facts.generation=m_opt.detectedGpu.generation;
        facts.driverVersion=m_opt.detectedGpu.driverVersion;facts.safeMode=m_opt.safeMode;
        {std::scoped_lock lock(m_startAnswers->mutex);facts.runtime=m_startAnswers->runtime;facts.runtimeVersion=m_startAnswers->runtimeVersion;}
        // The live-session forecast, which is measured on this machine once a
        // session has run and otherwise a measured prior for the generation;
        // with neither it says nothing and neither does this.
        const double prior=RenderPacePrior(m_opt.detectedGpu.generation);
        const auto at=[&](uint32_t width,uint32_t height)->std::optional<double>{
            const auto forecast=playback_timing::ForecastLiveRender(width,height,30.0,m_renderPace,prior);
            return forecast.measured?std::optional<double>(forecast.renderFps):std::nullopt;
        };
        if(facts.runtime!=start_screen::RuntimeState::Absent&&facts.runtime!=start_screen::RuntimeState::Incomplete){
            facts.fps1080=at(1920,1080);facts.fps1440=at(2560,1440);
        }
        return facts;
    }
    struct StartTile{bool trailer{};size_t index{};std::wstring title,detail,badge;std::string renderKey;std::wstring thumbId;};
    // Recent videos first, as the File menu lists them; then every trailer
    // that is not already one of them. A trailer that was opened before is a
    // recent video, with its render and its badge.
    std::vector<StartTile> StartTiles(bool trailers)const{
        std::vector<StartTile> tiles;
        const auto recent=m_recent?m_recent->Entries():std::vector<RecentMediaEntry>{};
        if(!trailers){
            for(size_t index=0;index<recent.size()&&index<5;++index){
                const auto& entry=recent[index];
                // An untitled local file is named by its file name, as its window title is:
                // the full path is unreadable in a tile's width and put a user's folders on
                // the screen anyone might be sharing.
                std::wstring name=entry.title;
                if(name.empty())name=entry.youtube?entry.source:std::filesystem::path(entry.source).filename().wstring();
                StartTile tile{false,index,name,T(entry.youtube?L"start.tile.youtube":L"start.tile.local"),{},entry.renderKey};
                if(!entry.renderKey.empty()){std::scoped_lock lock(m_startAnswers->mutex);const auto found=m_startAnswers->renders.find(entry.renderKey);if(found!=m_startAnswers->renders.end())tile.badge=found->second.badge;}
                // A curated trailer opened before keeps its thumbnail, under its render's frame.
                if(entry.youtube)for(const auto& example:kExampleVideos)if(entry.source==example.url)tile.thumbId=trailer_thumbnail::VideoIdFromWatchUrl(example.url).value_or(std::wstring());
                tiles.push_back(std::move(tile));
            }
            return tiles;
        }
        // A trailer is a YouTube link: with no way to play one, none is offered.
        if(!YouTubePlaybackAvailable())return tiles;
        for(size_t index=0;index<kExampleVideos.size();++index){
            const auto& example=kExampleVideos[index];
            if(std::any_of(recent.begin(),recent.end(),[&](const RecentMediaEntry& entry){return entry.youtube&&entry.source==example.url;}))continue;
            tiles.push_back(StartTile{true,index,std::wstring(example.title),std::wstring(example.channel),{},{},trailer_thumbnail::VideoIdFromWatchUrl(example.url).value_or(std::wstring())});
        }
        return tiles;
    }
    static const wchar_t* StartMarkText(start_screen::Mark mark){return mark==start_screen::Mark::Pass?L"✓ ":mark==start_screen::Mark::Fail?L"✕ ":L"• ";}
    // The panel's two columns measured in the font they are painted in, so the
    // block is centred as a whole instead of hanging off a fixed-width box.
    start_screen::PanelText StartPanelText(const std::vector<start_screen::Line>& lines,bool safeModeLink)const{
        start_screen::PanelText text{};
        const HFONT font=m_fontSmall?m_fontSmall:m_font;
        HDC dc=m_hwnd?GetDC(m_hwnd):nullptr;if(!dc||!font){if(dc)ReleaseDC(m_hwnd,dc);return text;}
        const HGDIOBJ old=SelectObject(dc,font);
        const auto measure=[&](const std::wstring& value){SIZE size{};GetTextExtentPoint32W(dc,value.c_str(),int(value.size()),&size);return int(size.cx);};
        for(const auto& line:lines){
            text.labelColumn=std::max(text.labelColumn,measure(StartMarkText(line.mark)+line.label));
            text.valueColumn=std::max(text.valueColumn,measure(line.value));
        }
        if(safeModeLink)text.valueColumn=std::max(text.valueColumn,measure(T(L"start.safe_mode")));
        SelectObject(dc,old);ReleaseDC(m_hwnd,dc);
        if(text.labelColumn>0)text.labelColumn+=Dip(20);   // the gutter between the columns
        return text;
    }
    start_screen::Layout StartLayout()const{
        RECT c{};if(m_hwnd)GetClientRect(m_hwnd,&c);
        const auto lines=start_screen::CapabilityLines(StartScreenFacts());
        const bool safeModeLink=start_screen::OfferSafeMode(lines,m_opt.safeMode);
        return start_screen::LayoutStartScreen(int(c.right-c.left),int(c.bottom-c.top),ActiveWindowDpi(m_hwnd),lines.size(),
                                               safeModeLink,StartTiles(false).size(),StartTiles(true).size(),
                                               StartPanelText(lines,safeModeLink),!YouTubePlaybackAvailable());
    }
    // Once, the first time the start screen is really on screen with trailers to
    // show: not while a file is open, and never without a cache to keep the
    // pictures in, so a month of launches costs about seven requests.
    // [Start] ThumbnailFetch=0 keeps it off the network; a picture already
    // cached still shows.
    void StartTrailerThumbnails(){
        if(m_trailerThumbnails->Started()||!StartScreenShown()||!YouTubePlaybackAvailable()||m_cacheRoot.empty())return;
        std::vector<std::wstring> ids;
        for(const auto& example:kExampleVideos)if(auto id=trailer_thumbnail::VideoIdFromWatchUrl(example.url))ids.push_back(std::move(*id));
        const bool fetch=GetPrivateProfileIntW(L"Start",L"ThumbnailFetch",1,SettingsPath().c_str())!=0;
        LOG("Trailer thumbnails: "<<ids.size()<<" trailers, network fetch "<<(fetch?"on":"off ([Start] ThumbnailFetch=0)")<<".");
        m_trailerThumbnails->Start(std::move(ids),{m_cacheRoot,fetch,Dip(start_screen::kTileWidthDip),{}},m_hwnd,WM_START_SCREEN);
    }
    void SyncStartScreen(){
        if(m_loaded||!m_hwnd)return;
        StartTrailerThumbnails();
        std::vector<std::string> keys;
        if(m_recent)for(const auto& entry:m_recent->Entries())keys.push_back(entry.renderKey);
        if(m_startRequested&&keys==m_startRequestedKeys)return;
        m_startRequested=true;m_startRequestedKeys=keys;
        if(m_startWorker.joinable()){m_startWorker.request_stop();m_startWorker.join();}
        StartScreenRequest request{ExecutableDirectory(),m_cacheRoot,{},Dip(start_screen::kTileWidthDip)};
        if(m_recent)for(const auto& entry:m_recent->Entries())if(!entry.renderKey.empty())request.recent.push_back({entry.renderKey,entry.sourceKey,entry.youtube});
        uint64_t generation=0;
        {std::scoped_lock lock(m_startAnswers->mutex);generation=++m_startAnswers->generation;m_startAnswers->renders.clear();}
        try{m_startWorker=std::jthread([request=std::move(request),answers=m_startAnswers,generation,window=m_hwnd](std::stop_token stop){
                GatherStartScreen(request,answers,generation,window,WM_START_SCREEN,stop);});}
        catch(const std::system_error&){LOG("Start screen check could not start a thread; the screen shows what it knows.");}
    }
    void PaintStartScreenExtras(HDC dc,const start_screen::Layout& layout){
        if(!layout.full)return;
        const auto lines=start_screen::CapabilityLines(StartScreenFacts());
        SetBkMode(dc,TRANSPARENT);
        const HGDIOBJ oldFont=SelectObject(dc,m_fontSmall?m_fontSmall:m_font);
        for(size_t index=0;index<lines.size()&&index<layout.lines.size();++index){
            const RECT line=layout.lines[index];
            // A drawn mark, not a coloured row: the state is a glyph in the
            // label column, as the toolbar's working state is its own colour.
            const wchar_t* mark=StartMarkText(lines[index].mark);
            const COLORREF markColor=lines[index].mark==start_screen::Mark::Pass?ui_palette::NeuralCoverage:lines[index].mark==start_screen::Mark::Fail?ui_palette::Attention:ui_palette::SecondaryText;
            RECT label{line.left,line.top,line.left+layout.labelWidth,line.bottom};
            SetTextColor(dc,markColor);DrawTextW(dc,mark,-1,&label,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            SIZE markSize{};GetTextExtentPoint32W(dc,mark,int(wcslen(mark)),&markSize);label.left+=markSize.cx;
            SetTextColor(dc,ui_palette::SecondaryText);DrawTextW(dc,lines[index].label.c_str(),-1,&label,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            RECT value{line.left+layout.labelWidth,line.top,line.right,line.bottom};
            SetTextColor(dc,ui_palette::PrimaryText);DrawTextW(dc,lines[index].value.c_str(),-1,&value,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        }
        if(layout.safeMode.right>layout.safeMode.left){
            const std::wstring link=T(L"start.safe_mode");
            SetTextColor(dc,m_startHover==StartHover::SafeMode?RGB(103,179,245):ui_palette::PrimaryBlue);
            RECT text=layout.safeMode;DrawTextW(dc,link.c_str(),-1,&text,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        }
        const auto headings=[&](const RECT& rect,const wchar_t* key){
            if(rect.right<=rect.left)return;
            RECT text=rect;SetTextColor(dc,ui_palette::SecondaryText);const std::wstring heading=T(key);
            DrawTextW(dc,heading.c_str(),-1,&text,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOPREFIX);
        };
        headings(layout.recentHeading,L"start.recent");headings(layout.trailersHeading,L"start.trailers");
        const auto recent=StartTiles(false),trailers=StartTiles(true);
        for(size_t index=0;index<layout.recentTiles.size()&&index<recent.size();++index)DrawStartTile(dc,layout.recentTiles[index],recent[index],StartTileLevel(StartHover::Recent,index));
        for(size_t index=0;index<layout.trailerTiles.size()&&index<trailers.size();++index)DrawStartTile(dc,layout.trailerTiles[index],trailers[index],StartTileLevel(StartHover::Trailer,index));
        SetTextColor(dc,ui_palette::SecondaryText);RECT hint=layout.hint;const std::wstring hintText=T(L"start.hint");
        DrawTextW(dc,hintText.c_str(),-1,&hint,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        SelectObject(dc,oldFont);
    }
    // A tile answers the pointer by lifting: it rises 2 dip, its picture
    // brightens by 6% and a blue edge fades in, over the toolbar's hover fade.
    // `hover` is that fade's level, so without animations it is 0 or 1.
    static constexpr double kTileBrighten=0.06;
    void DrawStartTile(HDC dc,RECT tile,const StartTile& content,double hover){
        const UINT dpi=ActiveWindowDpi(m_hwnd);
        OffsetRect(&tile,0,-LONG(std::lround(hover*Dip(2))));
        const RECT thumb=start_screen::TileThumbnail(tile,dpi);
        HBRUSH surface=CreateSolidBrush(ui_palette::Inactive);FillRect(dc,&thumb,surface);DeleteObject(surface);
        std::optional<TimelineMediaWorker::Thumbnail> picture;
        if(!content.renderKey.empty()){std::scoped_lock lock(m_startAnswers->mutex);const auto found=m_startAnswers->renders.find(content.renderKey);if(found!=m_startAnswers->renders.end())picture=found->second.thumbnail;}
        // The user's own neural frame first; without one, a curated trailer's
        // YouTube thumbnail (16:9 already, so it fills the tile); without that,
        // the placeholder glyph.
        std::shared_ptr<const TrailerPicture> poster;
        if(!picture&&!content.thumbId.empty())poster=m_trailerThumbnails->Picture(content.thumbId);
        const SIZE pictureSize=picture?picture->size:poster?poster->size:SIZE{};
        const uint8_t* pictureBits=picture?picture->bgra.data():poster?poster->bgra.data():nullptr;
        if(pictureBits&&pictureSize.cx>0&&pictureSize.cy>0){
            // Fitted, not stretched: a render keeps its own aspect on the tile.
            const double scale=std::min(double(thumb.right-thumb.left)/pictureSize.cx,double(thumb.bottom-thumb.top)/pictureSize.cy);
            const int w=int(pictureSize.cx*scale),h=int(pictureSize.cy*scale);
            const int x=thumb.left+(int(thumb.right-thumb.left)-w)/2,y=thumb.top+(int(thumb.bottom-thumb.top)-h)/2;
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(info.bmiHeader);info.bmiHeader.biWidth=pictureSize.cx;info.bmiHeader.biHeight=-pictureSize.cy;
            info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            SetStretchBltMode(dc,HALFTONE);
            // The hover lift brightens whichever picture the tile shows.
            const uint8_t* pixels=pictureBits;std::vector<uint8_t> lifted;
            if(hover>0.0){
                lifted.assign(pictureBits,pictureBits+size_t(pictureSize.cx)*size_t(pictureSize.cy)*4u);const double amount=hover*kTileBrighten;
                for(size_t at=0;at<lifted.size();++at)if(at%4!=3)lifted[at]=uint8_t(lifted[at]+std::lround((255-lifted[at])*amount));
                pixels=lifted.data();
            }
            StretchDIBits(dc,x,y,w,h,0,0,pictureSize.cx,pictureSize.cy,pixels,&info,DIB_RGB_COLORS,SRCCOPY);
        }else if(m_iconFont){
            const wchar_t glyph=GlyphForIcon(content.trailer?UiIcon::YouTube:UiIcon::Open);
            const HGDIOBJ old=SelectObject(dc,m_iconFont);SetTextColor(dc,ui_palette::SecondaryText);RECT box=thumb;
            DrawTextW(dc,&glyph,1,&box,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);SelectObject(dc,old);
        }
        if(!content.badge.empty()){
            SIZE size{};GetTextExtentPoint32W(dc,content.badge.c_str(),int(content.badge.size()),&size);
            const int pad=Dip(5);RECT badge{thumb.left+pad,thumb.bottom-pad-size.cy-Dip(4),thumb.left+pad+size.cx+2*Dip(6),thumb.bottom-pad};
            HBRUSH teal=CreateSolidBrush(ui_palette::NeuralCoverage);FillRect(dc,&badge,teal);DeleteObject(teal);
            SetTextColor(dc,ui_palette::Window);DrawTextW(dc,content.badge.c_str(),-1,&badge,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        }
        if(hover>0.0){HBRUSH edge=CreateSolidBrush(chrome_motion::Mix(ui_palette::Window,ui_palette::PrimaryBlue,hover));FrameRect(dc,&thumb,edge);DeleteObject(edge);}
        const int line=Dip(start_screen::kTileTextDip)/2;
        RECT title{tile.left,thumb.bottom+Dip(3),tile.right,thumb.bottom+Dip(3)+line};
        SetTextColor(dc,ui_palette::PrimaryText);DrawTextW(dc,content.title.c_str(),-1,&title,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        RECT detail{tile.left,title.bottom,tile.right,title.bottom+line};
        SetTextColor(dc,ui_palette::SecondaryText);DrawTextW(dc,content.detail.c_str(),-1,&detail,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    }
    enum class StartHover{None,SafeMode,Recent,Trailer};
    // The start screen is what the idle window paints unless a job or a
    // resolve has put the progress panel over it; a click on a tile that is
    // not on screen must not open anything.
    bool StartScreenShown()const{
        return !m_loaded&&!m_stageExport.running&&!(NeuralJobActive()&&!JobBehindPlayback())&&!m_youtubeLifecycle.IsResolving();
    }
    std::pair<StartHover,size_t> StartScreenHit(int x,int y)const{
        if(!StartScreenShown())return {StartHover::None,0};
        const auto layout=StartLayout();if(!layout.full)return {StartHover::None,0};
        if(PtIn(layout.safeMode,x,y))return {StartHover::SafeMode,0};
        for(size_t index=0;index<layout.recentTiles.size();++index)if(PtIn(layout.recentTiles[index],x,y))return {StartHover::Recent,index};
        for(size_t index=0;index<layout.trailerTiles.size();++index)if(PtIn(layout.trailerTiles[index],x,y))return {StartHover::Trailer,index};
        return {StartHover::None,0};
    }
    void UpdateStartHover(int x,int y){
        const auto [hover,index]=StartScreenHit(x,y);
        if(hover==m_startHover&&index==m_startHoverIndex)return;
        const auto now=Clock::now();
        if(m_startHover==StartHover::Recent||m_startHover==StartHover::Trailer)m_tileFades[StartTileKey(m_startHover,m_startHoverIndex)].Set(false,now,m_activityMotionEnabled);
        if(hover==StartHover::Recent||hover==StartHover::Trailer)m_tileFades[StartTileKey(hover,index)].Set(true,now,m_activityMotionEnabled);
        m_startHover=hover;m_startHoverIndex=index;if(!m_loaded&&m_hwnd)InvalidateRect(m_hwnd,nullptr,FALSE);
        if(m_activityMotionEnabled)EnsureHoverTimer();
    }
    static int StartTileKey(StartHover kind,size_t index){return static_cast<int>(kind)*64+static_cast<int>(index);}
    double StartTileLevel(StartHover kind,size_t index)const{
        const auto found=m_tileFades.find(StartTileKey(kind,index));
        return found==m_tileFades.end()?0.0:found->second.Level(Clock::now());
    }
    // One frame of the tiles' lift; the start screen is repainted whole, as
    // its hover always was, and only while nothing is playing.
    bool AnimateStartTiles(Clock::time_point now){
        bool moving=false;for(const auto& [key,fade]:m_tileFades)moving=moving||fade.Animating(now);
        if((moving||m_tilesWereMoving)&&!m_loaded&&m_hwnd)InvalidateRect(m_hwnd,nullptr,FALSE);
        m_tilesWereMoving=moving;return moving;
    }
    // A tile opens what it shows, by the same path as the File menu.
    bool ActivateStartScreen(int x,int y){
        const auto [hit,index]=StartScreenHit(x,y);
        switch(hit){
        case StartHover::SafeMode:RestartInSafeMode();return true;
        case StartHover::Recent:{const auto tiles=StartTiles(false);if(index<tiles.size())OpenRecent(tiles[index].index);return true;}
        case StartHover::Trailer:{const auto tiles=StartTiles(true);if(index<tiles.size()&&IsToolbarActionEnabled(ToolbarAction::OpenYouTube,ToolbarState()))ActivateExampleVideo(kExampleVideos[tiles[index].index]);return true;}
        case StartHover::None:break;
        }
        return false;
    }

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
        // Columns are as wide as the widest row - its action, a gap and its keys -
        // as measured in the font they are drawn in. The widest action and the
        // widest keys are different rows, and adding them made every column
        // wider than any row needed.
        int rowWidth=0;
        if(HDC dc=GetDC(m_shortcutWnd)){
            const HGDIOBJ old=SelectObject(dc,m_fontSmall?m_fontSmall:m_font);
            for(const auto& group:m_shortcutGroups)for(const auto& row:group.rows){
                SIZE action{},keys{};
                GetTextExtentPoint32W(dc,row.action.c_str(),int(row.action.size()),&action);
                GetTextExtentPoint32W(dc,row.keys.c_str(),int(row.keys.size()),&keys);
                rowWidth=std::max<int>(rowWidth,int(action.cx+keys.cx));
            }
            SelectObject(dc,old);ReleaseDC(m_shortcutWnd,dc);
        }
        m_shortcutMetrics=shortcut_sheet::Metrics{Dip(22),Dip(30),Dip(12),rowWidth+Dip(20),Dip(28),Dip(20),Dip(36),Dip(26)};
        // Laid out against the monitor's work area, centred on the player and kept
        // on that monitor. Sized to the player's client, the default window at
        // 175% held one column - the "Space (Overlay: Ctrl+Alt+Space)" row makes
        // a column wide - and the sheet showed File and Playback and silently
        // dropped the other five groups.
        RECT client{};GetClientRect(m_hwnd,&client);
        POINT centre{(client.right-client.left)/2,(client.bottom-client.top)/2};ClientToScreen(m_hwnd,&centre);
        MONITORINFO monitor{sizeof(monitor)};
        RECT area{centre.x-(client.right-client.left)/2,centre.y-(client.bottom-client.top)/2,0,0};
        area.right=area.left+(client.right-client.left);area.bottom=area.top+(client.bottom-client.top);
        if(GetMonitorInfoW(MonitorFromWindow(m_hwnd,MONITOR_DEFAULTTONEAREST),&monitor)){area=monitor.rcWork;InflateRect(&area,-Dip(24),-Dip(24));}
        std::vector<size_t> rows;for(const auto& group:m_shortcutGroups)rows.push_back(group.rows.size());
        // As tall as the player first, wider if it needs to be; the monitor's
        // height only when the groups still do not all fit.
        const auto everyGroup=[](const shortcut_sheet::Layout& layout){
            return std::all_of(layout.groups.begin(),layout.groups.end(),[](const shortcut_sheet::GroupPlacement& group){return group.visible;});};
        m_shortcutLayout=shortcut_sheet::LayoutSheet(rows,m_shortcutMetrics,int(area.right-area.left),
                                                     std::min<int>(int(client.bottom-client.top),int(area.bottom-area.top)));
        if(!everyGroup(m_shortcutLayout))
            m_shortcutLayout=shortcut_sheet::LayoutSheet(rows,m_shortcutMetrics,int(area.right-area.left),int(area.bottom-area.top));
        const LONG x=std::clamp<LONG>(centre.x-m_shortcutLayout.width/2,area.left,std::max<LONG>(area.left,area.right-m_shortcutLayout.width));
        const LONG y=std::clamp<LONG>(centre.y-m_shortcutLayout.height/2,area.top,std::max<LONG>(area.top,area.bottom-m_shortcutLayout.height));
        SetWindowPos(m_shortcutWnd,HWND_TOP,x,y,m_shortcutLayout.width,m_shortcutLayout.height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
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
    // The "render complete" moment: the coverage lane lights, a highlight
    // crosses it left to right - the whole range, rendered - and it settles
    // back to its own teal (chrome_motion::Glow, 900 ms). No sound, no
    // flash over the picture: the timeline is where coverage has always been
    // reported, so that is where the end of it is announced. Without motion
    // the lane holds lit for the same time and goes out, with no sweep.
    static constexpr COLORREF kCoverageLit=RGB(176,246,234);
    void DrawCompletionGlow(HDC dc,const RECT& lane){
        const auto now=Clock::now();
        if(!m_completeGlow.Animating(now)||lane.right<=lane.left||lane.bottom<=lane.top)return;
        const double level=m_completeGlow.Level(now,m_activityMotionEnabled);
        HBRUSH lit=CreateSolidBrush(chrome_motion::Mix(ui_palette::NeuralCoverage,kCoverageLit,level));FillRect(dc,&lane,lit);DeleteObject(lit);
        const auto sweep=m_completeGlow.Sweep(now,m_activityMotionEnabled);
        if(!sweep)return;
        const int saved=SaveDC(dc);if(!saved)return;
        IntersectClipRect(dc,lane.left,lane.top,lane.right,lane.bottom);
        // A soft-edged highlight from bands of rising brightness, centred on the
        // sweep's position; it starts before the lane and leaves past its end.
        const LONG half=Dip(28);
        const LONG centre=lane.left-half+LONG(std::lround(*sweep*double(lane.right-lane.left+2*half)));
        constexpr int kSteps=4;
        for(int step=0;step<kSteps;++step){
            const LONG reach=half*(kSteps-step)/kSteps;
            const RECT band{centre-reach,lane.top,centre+reach,lane.bottom};
            HBRUSH b=CreateSolidBrush(chrome_motion::Mix(kCoverageLit,RGB(236,255,251),double(step+1)/kSteps));FillRect(dc,&band,b);DeleteObject(b);
        }
        RestoreDC(dc,saved);
    }
    void StartCompletionGlow(){
        m_completeGlow.Start(Clock::now());
        // The same moment, in words: how much video, and how long it took.
        if(m_liveRange.end100ns>m_liveRange.start100ns){
            const double video=double(m_liveRange.end100ns-m_liveRange.start100ns)*1e-7;
            const double took=std::chrono::duration<double>(Clock::now()-m_liveSessionStartedAt).count();
            ShowToast(T(L"toast.rendered")+TimeText(video)+T(L"toast.rendered_in")+TimeText(took),ui_palette::NeuralCoverage);
        }
        LOG("Live session coverage complete; the timeline marks it.");
        if(m_glowTimer&&m_hwnd)KillTimer(m_hwnd,m_glowTimer);
        m_glowTimer=m_hwnd?SetTimer(m_hwnd,kGlowTimerId,m_activityMotionEnabled?chrome_motion::kPlaybackFrameMs:UINT(chrome_motion::kCompleteGlow.count()),nullptr):0;
        InvalidatePlaybackProgress();
    }
    // New coverage from a landed segment eases in along the lane; the timer
    // runs at the playback rate for the 180 ms of each reveal only.
    void ObserveBandGrowth(){
        if(!m_liveSession){m_bandGrowth.Reset();return;}
        std::vector<chrome_motion::BandGrowth::Interval> spans;
        for(const CoverageSpan& span:LiveCoverage())spans.push_back({span.start100ns,span.end100ns});
        if(m_bandGrowth.Observe(spans,Clock::now(),m_activityMotionEnabled)&&!m_bandTimer&&m_hwnd)
            m_bandTimer=SetTimer(m_hwnd,kBandTimerId,chrome_motion::kPlaybackFrameMs,nullptr);
    }
    void AnimateBandGrowth(){
        InvalidatePlaybackProgress();
        if(!m_bandGrowth.Animating(Clock::now())&&m_bandTimer){KillTimer(m_hwnd,m_bandTimer);m_bandTimer=0;}
    }
    void AnimateCompletionGlow(){
        InvalidatePlaybackProgress();
        if(!m_completeGlow.Animating(Clock::now())&&m_glowTimer){KillTimer(m_hwnd,m_glowTimer);m_glowTimer=0;}
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
    // The start screen's core: where the title and the two buttons are once
    // the capability panel and the tiles have been placed around them.
    IdleSurfaceLayout IdleLayout()const{return StartLayout().core;}
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
        // Up by the bubble's reach, which sits over the knob inside the strip.
        RECT dirty=*volume;InflateRect(&dirty,Dip(24),Dip(9));dirty.top-=Dip(16);RECT c{};GetClientRect(m_hwnd,&c);dirty.right=c.right-Dip(16);InvalidateRect(m_hwnd,&dirty,FALSE);
    }
    void InvalidateToolbarAction(ToolbarAction action){
        if(!m_hwnd||action==ToolbarAction::None)return;const auto items=FocusableItems();
        for(const auto& item:items)if(item.action==action){InvalidateRect(m_hwnd,&item.bounds,FALSE);return;}
    }
    void UpdateCachedStatus(){
        UpdateStatusChips();SyncTimelineMedia();SyncMediaTransport();SyncStartScreen();
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
            if(WarmUpStep()!=warm_up::Step::None)return {true,fraction,std::nullopt,true};
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
        // Before the unchanged-chips return below: the render chip reads
        // "Render 100%" a moment before the last hole is published, so the
        // chips can be unchanged on the call that sees the session finish.
        if(m_completionLatch.Observe(m_liveSession&&m_liveSegments!=nullptr,LiveSessionFinished()))StartCompletionGlow();
        ObserveBandGrowth();
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
        else RefreshToolbarTips();
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
        const auto dirty=HoverDirtyRectangles(items,m_hoverAction,action);
        // The tint eases in on the control the cursor reached and out of the one
        // it left (chrome_motion::kHoverIn / kHoverOut). Without motion the
        // fades land at once, which is exactly the hover this had before.
        const auto now=Clock::now();
        if(m_hoverAction!=ToolbarAction::None)m_hoverFades[static_cast<size_t>(m_hoverAction)].Set(false,now,m_activityMotionEnabled);
        if(action!=ToolbarAction::None)m_hoverFades[static_cast<size_t>(action)].Set(true,now,m_activityMotionEnabled);
        m_hoverAction=action;
        for(const RECT& rect:dirty)InvalidateRect(m_hwnd,&rect,FALSE);
        if(m_activityMotionEnabled&&!m_hoverTimer&&m_hwnd)m_hoverTimer=SetTimer(m_hwnd,kHoverTimerId,chrome_motion::kFrameMs,nullptr);
    }
    // A layout change or a hidden bar drops the hover without a fade: the
    // control it was on is gone or has moved, so there is nothing to ease out of.
    void ResetHoverFades(){
        for(auto& fade:m_hoverFades)fade.Reset();
        m_hoverAnimating=0;
        m_volumeHot.Reset();m_mixHot.Reset();m_volumeBubble.Reset();m_volumeBubbleOffAt.reset();m_slidersWereMoving=false;
        m_compareFades.clear();m_compareWasMoving=false;
        m_tileFades.clear();m_tilesWereMoving=false;
        if(m_hoverTimer&&m_hwnd){KillTimer(m_hwnd,m_hoverTimer);m_hoverTimer=0;}
    }
    // One timer frame of the hover fades: repaints only the buttons whose tint
    // is moving, plus one last paint for each that just landed, and stops
    // itself once nothing is moving. Playback never pays for a hover.
    void AnimateHover(){
        const auto now=Clock::now();const auto items=FocusableItems();
        uint32_t animating=0;
        for(const auto& item:items){
            const size_t index=static_cast<size_t>(item.action);
            if(index>=m_hoverFades.size())continue;
            const bool moving=m_hoverFades[index].Animating(now);
            if(moving||(m_hoverAnimating>>index)&1u)InvalidateRect(m_hwnd,&item.bounds,FALSE);
            if(moving)animating|=1u<<index;
        }
        m_hoverAnimating=animating;
        const bool sliders=AnimateSliders(now);
        const bool compare=AnimateCompareBar(now);
        const bool tiles=AnimateStartTiles(now);
        const bool tags=AnimateTagFade(now);
        if(!animating&&!sliders&&!compare&&!tiles&&!tags&&m_hoverTimer){KillTimer(m_hwnd,m_hoverTimer);m_hoverTimer=0;}
    }
    // The volume slider's geometry at this moment: the knob grows with the
    // hover fade, and the bubble keeps inside the strip, above the button row.
    slider::Geometry VolumeSliderGeometry(const RECT& area,Clock::time_point now)const{
        RECT c{};GetClientRect(m_hwnd,&c);
        const RECT bounds{area.left-Dip(24),c.bottom-ControlHeight(),c.right-Dip(16),c.bottom};
        return slider::Layout(area,m_muted?0.0:double(m_volume),0.0,m_volumeHot.Level(now),ActiveWindowDpi(m_hwnd),bounds);
    }
    // The value over the knob while it is dragged. It fades by mixing toward
    // the surface under it: GDI has no alpha, and the strip is one flat colour.
    void DrawSliderBubble(HDC dc,const RECT& r,const std::wstring& text,double level,COLORREF under){
        const int radius=std::max(1,Dip(4));
        HBRUSH brush=CreateSolidBrush(chrome_motion::Mix(under,ui_palette::Hover,level));HPEN pen=CreatePen(PS_SOLID,1,chrome_motion::Mix(under,RGB(93,97,104),level));
        const HGDIOBJ oldBrush=SelectObject(dc,brush),oldPen=SelectObject(dc,pen);
        RoundRect(dc,r.left,r.top,r.right,r.bottom,radius*2,radius*2);
        SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(brush);DeleteObject(pen);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,chrome_motion::Mix(under,ui_palette::PrimaryText,level));
        const HGDIOBJ oldFont=SelectObject(dc,m_fontSmall?m_fontSmall:m_font);RECT t=r;
        DrawTextW(dc,text.c_str(),-1,&t,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);SelectObject(dc,oldFont);
    }
    // Where the pointer and the drags leave the two sliders. Called from every
    // mouse message that can change it; it only starts fades, AnimateHover runs
    // them.
    void SyncSliderHover(){
        const auto now=Clock::now();
        const auto volume=VolumeRect();
        const bool volumeHot=m_dragVolume||(volume&&PtIn(*volume,m_mouseX,m_mouseY));
        const bool mixHot=m_dragMix||m_compareHover.part==compare_bar::Part::MixTrack;
        bool changed=false;
        if(volumeHot!=m_volumeHot.On()){m_volumeHot.Set(volumeHot,now,m_activityMotionEnabled);changed=true;}
        if(mixHot!=m_mixHot.On()){m_mixHot.Set(mixHot,now,m_activityMotionEnabled);changed=true;}
        if(m_dragVolume){
            m_volumeBubbleOffAt.reset();
            if(!m_volumeBubble.On()){m_volumeBubble.Set(true,now,m_activityMotionEnabled);changed=true;}
        }else if(m_volumeBubble.On()&&!m_volumeBubbleOffAt){
            m_volumeBubbleOffAt=now+std::chrono::milliseconds(slider::kBubbleLingerMs);changed=true;
        }
        if(!changed)return;
        InvalidateVolumeControls();InvalidateMixTrack();
        EnsureHoverTimer();
    }
    void EnsureHoverTimer(){if(!m_hoverTimer&&m_hwnd)m_hoverTimer=SetTimer(m_hwnd,kHoverTimerId,chrome_motion::kFrameMs,nullptr);}
    void InvalidateMixTrack(){
        if(!m_hwnd||!CompareBarVisible())return;
        RECT track=CompareBarLayout().mixTrack;if(track.right<=track.left)return;
        InflateRect(&track,Dip(slider::kKnobHotDip+2),0);InvalidateRect(m_hwnd,&track,FALSE);
    }
    // One frame of the sliders' fades; true while any is still moving or a
    // bubble is waiting out its linger.
    bool AnimateSliders(Clock::time_point now){
        if(m_volumeBubbleOffAt&&now>=*m_volumeBubbleOffAt){m_volumeBubbleOffAt.reset();m_volumeBubble.Set(false,now,m_activityMotionEnabled);InvalidateVolumeControls();}
        const bool volumeMoving=m_volumeHot.Animating(now)||m_volumeBubble.Animating(now);
        const bool mixMoving=m_mixHot.Animating(now);
        if(volumeMoving||m_slidersWereMoving)InvalidateVolumeControls();
        if(mixMoving||m_slidersWereMoving)InvalidateMixTrack();
        m_slidersWereMoving=volumeMoving||mixMoving;
        return m_slidersWereMoving||m_volumeBubbleOffAt.has_value();
    }
    double HoverLevel(ToolbarAction action,bool hover)const{
        const size_t index=static_cast<size_t>(action);
        if(action==ToolbarAction::None||index>=m_hoverFades.size())return hover?1.0:0.0;
        return m_hoverFades[index].Level(Clock::now());
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
        case ToolbarAction::ToggleNeuralRendering:{const bool active=(m_cachedPlayback&&m_comparisonView==ComparisonView::Neural)||m_previewShown;const bool cachedPair=m_cachedPlayback&&m_havePresentedPair&&rendererReady;const std::wstring label=m_neuralToggleDeferred?L"Neural Rendering · Queued for the seek":WarmUpStep()!=warm_up::Step::None?L"Neural Rendering · Starting":m_previewJob?L"Neural Rendering · Previewing settings":m_previewShown?L"Neural Rendering · Settings preview":enabled?(active?L"Neural Rendering · On":L"Neural Rendering · Off"):(cachedPair?std::wstring(L"Neural Rendering · Seeking · ")+(active?L"On":L"Off"):(NeuralJobActive()?L"Neural Rendering · Preparing cache":L"Neural Rendering · No cache"));return{UiIcon::Sparkles,label,enabled,active,m_previewJob!=0||NeuralJobActive()};}
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
        case ToolbarAction::Aspect:return{UiIcon::Crop,m_fill||m_onePixel?L"Fit":L"Fill",enabled,m_fill||m_onePixel};
        case ToolbarAction::Adjustments:return{UiIcon::Adjustments,L"Color",enabled,m_adjustWnd!=nullptr};
        case ToolbarAction::DebugView:{const bool active=rendererReady&&m_renderer->GetDebugView()!=D3D12Renderer::DebugView::Final;return{UiIcon::Debug,L"Debug",enabled,active};}
        case ToolbarAction::Fullscreen:return{UiIcon::Maximize,L"Full",enabled,m_fullscreen};
        case ToolbarAction::None:break;
        }
        return{UiIcon::Warning,L"Unavailable",false,false};
    }

    bool ToolbarActionEnabled(ToolbarAction action)const{return IsToolbarActionEnabled(action,ToolbarState());}

    void DrawButton(HDC dc,ToolbarAction action,UiIcon icon,const std::wstring&label,const RECT&r,bool enabled,bool active,bool hover,bool pressed,bool focus,bool compact,bool working=false){
        // The resting and hovered looks mixed by how far the hover fade has got,
        // so the tint eases rather than snapping; at 0 or 1 it is either look.
        const ButtonVisual rest=ResolveButtonVisual(ButtonState{enabled,active,working,false,pressed,focus});
        const ButtonVisual hot=ResolveButtonVisual(ButtonState{enabled,active,working,true,pressed,focus});
        const double hoverLevel=HoverLevel(action,hover);
        ButtonVisual visual=hot;
        visual.fill=chrome_motion::Mix(rest.fill,hot.fill,hoverLevel);
        visual.border=chrome_motion::Mix(rest.border,hot.border,hoverLevel);
        visual.text=chrome_motion::Mix(rest.text,hot.text,hoverLevel);
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
        ButtonContentLayout content=LayoutButtonContent(r,iconSize,showText?textSize:SIZE{},stacked&&showText,ActiveWindowDpi(m_hwnd));
        // Pressed sinks the glyph and label by one pixel into the darker fill,
        // the inset a physical key gives under a finger. No timer: a press is
        // as long as the button is held.
        if(pressed&&enabled){const int sink=std::max(1,Dip(1));OffsetRect(&content.icon,0,sink);OffsetRect(&content.text,0,sink);}
        if(showIcon){const HGDIOBJ oldFont=SelectObject(dc,m_iconFont);RECT iconRect=content.icon;DrawTextW(dc,&glyph,1,&iconRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);SelectObject(dc,oldFont);}
        if(showText){const HGDIOBJ oldFont=SelectObject(dc,textFont);RECT textRect=content.text;
            DrawTextW(dc,shown.c_str(),-1,&textRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            SelectObject(dc,oldFont);}
        if(visual.drawFocus&&action!=ToolbarAction::None)DrawRoundedFocusRing(dc,r,radius,visual.text,ActiveWindowDpi(m_hwnd));
    }

    bool FocusCueFor(ToolbarAction action)const{return m_keyboardCues&&GetFocus()==m_hwnd&&m_focusedToolbarAction==action;}
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

    // --- The compare bar (compare_bar lays it out) ----------------------------------
    // Square segments and one mark, after DESIGN.md: the selected mode is not a filled
    // pill but a 2 px rule in the flag orange under its label, the one ink the site
    // uses for the comparison seam. Everything else is the strip's own greys.
    static constexpr COLORREF kCompareMark=RGB(255,106,26);
    // Tabler glyphs for the parts that drop their words when the bar narrows.
    static constexpr wchar_t kCompareSwapGlyph=L'\xeb31';    // switch-horizontal
    static constexpr wchar_t kCompareLoupeGlyph=L'\xfcb0';   // zoom-scan
    static constexpr wchar_t kCompareChevronGlyph=L'\xea5f'; // chevron-down
    struct CompareHover{compare_bar::Part part=compare_bar::Part::None;int index=0;
        friend bool operator==(const CompareHover&,const CompareHover&)=default;};
    // The bar's text measured in the fonts it is painted with, so the layout steps down
    // on what the labels really take rather than on a guess about them.
    compare_bar::Metrics CompareBarMetrics()const{
        const UINT dpi=ActiveWindowDpi(m_hwnd);
        const HFONT font=m_fontSmall?m_fontSmall:m_font;
        HDC dc=m_hwnd?GetDC(m_hwnd):nullptr;
        if(!dc||!font){if(dc)ReleaseDC(m_hwnd,dc);return compare_bar::EstimatedMetrics(CompareBarModes().size(),dpi);}
        const HGDIOBJ old=SelectObject(dc,font);
        const auto measure=[&](const std::wstring& text){SIZE size{};GetTextExtentPoint32W(dc,text.c_str(),int(text.size()),&size);return int(size.cx);};
        compare_bar::Metrics metrics;
        for(const ComparisonMode mode:CompareBarModes()){metrics.modeLabels.push_back(measure(CompareModeLabel(mode,false)));metrics.modeShortLabels.push_back(measure(CompareModeLabel(mode,true)));}
        metrics.swapLabel=measure(T(L"compare.swap"));metrics.loupeLabel=measure(T(L"compare.loupe"));metrics.mixLabel=measure(T(L"compare.mix"));
        if(m_iconFont){SelectObject(dc,m_iconFont);const wchar_t glyph=kCompareSwapGlyph;SIZE size{};GetTextExtentPoint32W(dc,&glyph,1,&size);metrics.icon=std::max(1,int(size.cx));}
        SelectObject(dc,old);ReleaseDC(m_hwnd,dc);
        return metrics;
    }
    compare_bar::Layout CompareBarLayout()const{
        RECT c{};GetClientRect(m_hwnd,&c);
        return compare_bar::LayoutBar(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom)-ControlHeight(),
                                      ActiveWindowDpi(m_hwnd),CompareBarMetrics(),true);
    }
    void InvalidateCompareBar(){
        if(!m_hwnd||!CompareBarVisible())return;
        const RECT bar=CompareBarLayout().bar;InvalidateRect(m_hwnd,&bar,FALSE);
    }
    void SetCompareHover(CompareHover hover){
        if(hover==m_compareHover)return;
        // The same tint fade as the toolbar: the segment the pointer reached
        // eases in, the one it left eases out.
        const auto now=Clock::now();
        if(m_compareHover.part!=compare_bar::Part::None)m_compareFades[CompareFadeKey(m_compareHover)].Set(false,now,m_activityMotionEnabled);
        if(hover.part!=compare_bar::Part::None)m_compareFades[CompareFadeKey(hover)].Set(true,now,m_activityMotionEnabled);
        m_compareHover=hover;InvalidateCompareBar();
        if(m_activityMotionEnabled)EnsureHoverTimer();
    }
    static int CompareFadeKey(CompareHover hover){return static_cast<int>(hover.part)*64+hover.index;}
    double CompareHoverLevel(const compare_bar::Item& item,Clock::time_point now)const{
        const auto found=m_compareFades.find(CompareFadeKey({item.part,item.index}));
        return found==m_compareFades.end()?0.0:found->second.Level(now);
    }
    // The mark leaves the old mode's segment and slides to the new one's.
    // Only between two segments on show: a folded bar (Menu tier) has one
    // button, whose mark does not move.
    void StartCompareMarkSlide(ComparisonMode from,ComparisonMode to){
        if(from==to||!CompareBarVisible())return;
        const auto layout=CompareBarLayout();const auto modes=CompareBarModes();
        const RECT* a=nullptr;const RECT* b=nullptr;
        for(const auto& item:layout.items){
            if(item.part!=compare_bar::Part::Mode||size_t(item.index)>=modes.size())continue;
            if(modes[size_t(item.index)]==from)a=&item.bounds;
            if(modes[size_t(item.index)]==to)b=&item.bounds;
        }
        if(!a||!b)return;
        m_compareMark.Start(a->left,a->right-1,b->left,b->right-1,Clock::now(),m_activityMotionEnabled);
        if(m_activityMotionEnabled)EnsureHoverTimer();
    }
    // One frame of the tags' fade-in: the compositor reads the level, and a
    // paused frame is presented again (the reference is already resident, so
    // this is one present, not an upload). Playing, the next frame carries it.
    bool AnimateTagFade(Clock::time_point now){
        const bool moving=m_tagFade.Animating(now);
        if((moving||m_tagsWereMoving)&&m_renderer&&ComparisonModesAvailable()){
            m_renderer->SetComparison(EffectiveComparison());
            if(!m_playing&&!m_seeking&&!m_renderer->PresentCurrent())RecoverUnusableRenderer();
        }
        m_tagsWereMoving=moving;return moving;
    }
    // One frame of the compare bar's motion; true while any of it moves.
    bool AnimateCompareBar(Clock::time_point now){
        bool moving=m_compareMark.Animating(now);
        for(const auto& [key,fade]:m_compareFades)moving=moving||fade.Animating(now);
        if(moving||m_compareWasMoving)InvalidateCompareBar();
        m_compareWasMoving=moving;
        return moving;
    }
    bool CompareBarPartEnabled(compare_bar::Part part)const{
        switch(part){
        case compare_bar::Part::ZoomOut:case compare_bar::Part::ZoomIn:return m_loaded&&m_renderer!=nullptr;
        case compare_bar::Part::None:return false;
        default:return ComparisonModesAvailable();
        }
    }
    // A mode segment is live when its mode can be shown; RTX VSR may not be.
    bool CompareBarItemEnabled(const compare_bar::Item& item)const{
        if(item.part==compare_bar::Part::Mode){
            const auto modes=CompareBarModes();
            return size_t(item.index)<modes.size()&&CompareModeEnabled(modes[size_t(item.index)]);
        }
        return CompareBarPartEnabled(item.part);
    }
    void CompareBarMouseMove(int x,int y){
        if(m_dragMix&&GetCapture()==m_hwnd){SetMix(compare_bar::MixFromX(CompareBarLayout().mixTrack,x));return;}
        if(!CompareBarVisible()){SetCompareHover({});return;}
        const auto layout=CompareBarLayout();const auto* item=compare_bar::HitTest(layout,POINT{x,y});
        SetCompareHover(item&&CompareBarItemEnabled(*item)?CompareHover{item->part,item->index}:CompareHover{});
    }
    bool CompareBarMouseDown(int x,int y){
        if(!CompareBarVisible())return false;
        const auto layout=CompareBarLayout();
        if(!PtIn(layout.bar,x,y))return false;
        const auto* item=compare_bar::HitTest(layout,POINT{x,y});
        // A press anywhere on the row is the row's, enabled or not, so it never falls
        // through to a toolbar button that happens to sit under it.
        if(!item||!CompareBarItemEnabled(*item))return true;
        switch(item->part){
        case compare_bar::Part::Mode:SetComparisonMode(CompareBarModes()[size_t(item->index)]);break;
        case compare_bar::Part::ModeMenu:ShowCompareModeMenu(item->bounds);break;
        case compare_bar::Part::MixTrack:m_dragMix=true;SetCapture(m_hwnd);SetMix(compare_bar::MixFromX(layout.mixTrack,x));SyncSliderHover();break;
        case compare_bar::Part::ZoomOut:ZoomBy(-1,false,std::nullopt);break;
        case compare_bar::Part::ZoomIn:ZoomBy(+1,false,std::nullopt);break;
        case compare_bar::Part::Swap:ToggleSwap();break;
        case compare_bar::Part::Loupe:ToggleLoupe();break;
        default:break;
        }
        return true;
    }
    // The narrow bar's mode button: a menu of every mode, the current one checked. It
    // opens upward from the button, since the bar sits at the bottom of the window.
    void ShowCompareModeMenu(RECT button){
        HMENU menu=CreatePopupMenu();if(!menu)return;
        const auto modes=CompareBarModes();
        for(size_t index=0;index<modes.size();++index)
            AppendMenuW(menu,MF_STRING|(modes[index]==SelectedComparisonMode()?MF_CHECKED:0u)|(CompareModeEnabled(modes[index])?0u:MF_GRAYED),UINT_PTR(index+1),CompareModeLabel(modes[index],false).c_str());
        POINT anchor{button.left,button.top};ClientToScreen(m_hwnd,&anchor);
        const UINT chosen=UINT(TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_LEFTALIGN|TPM_BOTTOMALIGN|TPM_RIGHTBUTTON,anchor.x,anchor.y,m_hwnd,nullptr));
        DestroyMenu(menu);
        if(chosen>=1&&chosen<=modes.size())SetComparisonMode(modes[chosen-1]);
    }
    // `glyph` is drawn in the icon font ahead of the label, or alone when the label is
    // empty; a chevron after the label marks a button that opens a menu.
    void DrawCompareSegment(HDC dc,RECT r,const std::wstring& label,bool enabled,bool selected,double hover,wchar_t glyph=0,bool chevron=false,bool drawMark=true){
        // One pixel of the strip between neighbours, so a run of segments reads as a
        // segmented control without a border around each.
        r.right-=1;
        HBRUSH fill=CreateSolidBrush(enabled?chrome_motion::Mix(ui_palette::Inactive,ui_palette::Hover,hover):ui_palette::Inactive);FillRect(dc,&r,fill);DeleteObject(fill);
        SetTextColor(dc,!enabled?RGB(98,101,108):(selected?ui_palette::PrimaryText:ui_palette::SecondaryText));
        if(!m_iconFont){glyph=0;chevron=false;}
        if(!glyph&&!chevron){RECT text=r;DrawTextW(dc,label.c_str(),-1,&text,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);}
        else{
            // Icon, label and chevron centred as one run.
            const HGDIOBJ textFont=GetCurrentObject(dc,OBJ_FONT);
            const wchar_t mark=kCompareChevronGlyph;SIZE glyphSize{},labelSize{},chevronSize{};
            SelectObject(dc,m_iconFont);if(glyph)GetTextExtentPoint32W(dc,&glyph,1,&glyphSize);if(chevron)GetTextExtentPoint32W(dc,&mark,1,&chevronSize);SelectObject(dc,textFont);
            if(!label.empty())GetTextExtentPoint32W(dc,label.c_str(),int(label.size()),&labelSize);
            const int gap=Dip(compare_bar::kIconLabelGapDip);
            const int content=glyphSize.cx+(glyph&&!label.empty()?gap:0)+labelSize.cx+(chevron?gap+chevronSize.cx:0);
            int x=r.left+std::max(0,int(r.right-r.left-content)/2);
            if(glyph){SelectObject(dc,m_iconFont);RECT box{x,r.top,x+glyphSize.cx,r.bottom};DrawTextW(dc,&glyph,1,&box,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);SelectObject(dc,textFont);x+=glyphSize.cx+(label.empty()?0:gap);}
            if(!label.empty()){RECT text{x,r.top,std::min<LONG>(r.right,x+labelSize.cx),r.bottom};DrawTextW(dc,label.c_str(),-1,&text,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);x+=labelSize.cx;}
            if(chevron){SelectObject(dc,m_iconFont);RECT box{x+gap,r.top,x+gap+chevronSize.cx,r.bottom};DrawTextW(dc,&mark,1,&box,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);SelectObject(dc,textFont);}
        }
        if(selected&&drawMark){RECT line{r.left,r.bottom-std::max(1,Dip(2)),r.right,r.bottom};HBRUSH ink=CreateSolidBrush(enabled?kCompareMark:RGB(98,101,108));FillRect(dc,&line,ink);DeleteObject(ink);}
    }
    static std::wstring PercentText(float value){return std::to_wstring(int(std::lround(value*100.0f)))+L"%";}
    void DrawCompareBar(HDC dc){
        const auto layout=CompareBarLayout();
        const bool available=ComparisonModesAvailable();
        HPEN rule=CreatePen(PS_SOLID,1,RGB(40,42,46));const HGDIOBJ oldPen=SelectObject(dc,rule);
        MoveToEx(dc,layout.bar.left,layout.bar.bottom-1,nullptr);LineTo(dc,layout.bar.right,layout.bar.bottom-1);
        SelectObject(dc,oldPen);DeleteObject(rule);
        SetBkMode(dc,TRANSPARENT);const HGDIOBJ oldFont=SelectObject(dc,m_fontSmall?m_fontSmall:m_font);
        const auto hovered=[&](const compare_bar::Item& item){return m_compareHover.part==item.part&&m_compareHover.index==item.index;};
        const auto now=Clock::now();const bool sliding=m_compareMark.Animating(now);
        const auto level=[&](const compare_bar::Item& item){return CompareHoverLevel(item,now);};
        for(const auto& item:layout.items){
            const bool enabled=CompareBarItemEnabled(item);
            switch(item.part){
            case compare_bar::Part::Mode:{
                const ComparisonMode mode=CompareBarModes()[size_t(item.index)];
                DrawCompareSegment(dc,item.bounds,CompareModeLabel(mode,item.face==compare_bar::Face::ShortLabel),enabled,mode==SelectedComparisonMode(),level(item),0,false,!sliding);break;}
            // Folded into one button, the mode is always the selected one: the mark says so.
            case compare_bar::Part::ModeMenu:DrawCompareSegment(dc,item.bounds,CompareModeLabel(SelectedComparisonMode(),item.face==compare_bar::Face::ShortLabel),enabled,true,level(item),0,true);break;
            case compare_bar::Part::MixTrack:{
                // The player's slider (slider::Layout), filled from 100% - the
                // render untouched - to the Mix, so more and less read as two
                // directions. The area is widened by the knob's inset, which
                // lays the rail exactly on the track MixFromX measures. No
                // bubble: over this bar it would sit on the picture, and the
                // value is already read out beside the track.
                const RECT& t=item.bounds;const int inset=Dip(slider::kKnobRestDip);
                const slider::Geometry g=slider::Layout(RECT{t.left-inset,t.top,t.right+inset,t.bottom},
                    double(m_comparison.strength)/2.0,0.5,enabled?m_mixHot.Level(Clock::now()):0.0,ActiveWindowDpi(m_hwnd));
                DrawSliderRail(dc,g,enabled);
                const int mid=(t.top+t.bottom)/2,centre=compare_bar::XFromMix(t,1.0f);RECT tick{centre,mid-Dip(5),centre+std::max(1,Dip(1)),mid+Dip(5)};
                HBRUSH tb=CreateSolidBrush(ui_palette::SecondaryText);FillRect(dc,&tick,tb);DeleteObject(tb);
                DrawSliderKnob(dc,g,enabled,false,ActiveWindowDpi(m_hwnd));
                break;}
            case compare_bar::Part::ZoomOut:DrawCompareSegment(dc,item.bounds,L"\u2212",enabled&&m_zoomStep>0,false,level(item));break;
            case compare_bar::Part::ZoomIn:DrawCompareSegment(dc,item.bounds,L"+",enabled&&compare_zoom::Step(m_zoomStep,1,ZoomOutputWidth(),ZoomViewWidth(),false)!=m_zoomStep,false,level(item));break;
            case compare_bar::Part::Swap:case compare_bar::Part::Loupe:{
                const bool swap=item.part==compare_bar::Part::Swap;
                const bool icon=item.face==compare_bar::Face::Icon||item.face==compare_bar::Face::IconLabel;
                DrawCompareSegment(dc,item.bounds,item.face==compare_bar::Face::Icon?std::wstring{}:T(swap?L"compare.swap":L"compare.loupe"),enabled,swap?m_comparison.swap:m_loupe,level(item),
                                   icon?(swap?kCompareSwapGlyph:kCompareLoupeGlyph):wchar_t(0));break;}
            default:break;
            }
        }
        if(sliding){
            const auto [left,right]=m_compareMark.At(now);
            LONG bottom=layout.bar.bottom;
            for(const auto& item:layout.items)if(item.part==compare_bar::Part::Mode){bottom=item.bounds.bottom;break;}
            RECT line{left,bottom-std::max(1,Dip(2)),right,bottom};HBRUSH ink=CreateSolidBrush(kCompareMark);FillRect(dc,&line,ink);DeleteObject(ink);
        }
        SetTextColor(dc,available?ui_palette::SecondaryText:RGB(98,101,108));
        if(layout.mixLabel.right>layout.mixLabel.left){RECT mixLabel=layout.mixLabel;DrawTextW(dc,T(L"compare.mix").c_str(),-1,&mixLabel,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);}
        SetTextColor(dc,available?ui_palette::PrimaryText:RGB(98,101,108));
        RECT mixValue=layout.mixValue;DrawTextW(dc,PercentText(m_comparison.strength).c_str(),-1,&mixValue,DT_RIGHT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        SetTextColor(dc,m_loaded&&m_renderer?ui_palette::PrimaryText:RGB(98,101,108));
        RECT zoomValue=layout.zoomValue;DrawTextW(dc,(m_zoomStep>0?ZoomStepText(m_zoomStep):T(L"compare.zoom.fit")).c_str(),-1,&zoomValue,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        if(layout.hint.right>layout.hint.left){
            SetTextColor(dc,ui_palette::SecondaryText);RECT hint=layout.hint;
            // A mask changes what DLSS 5 shows, so while one is on the bar says which.
            const std::wstring text=!available?T(L"compare.hint.unavailable")
                :(!m_maskPath.empty()?T(L"compare.hint.mask")+m_maskPath.filename().wstring():T(L"compare.hint.hold"));
            DrawTextW(dc,text.c_str(),-1,&hint,DT_RIGHT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        }
        SelectObject(dc,oldFont);
    }
    // The tags the compositor draws on the picture, rendered here because GDI is where
    // the player's text already comes from: uppercase, on a near-black plate with the
    // flag rule down its left edge, DESIGN.md's provenance tag. Premultiplied, one row
    // per tag, at the window's DPI; see compare_labels::Premultiply for the alpha.
    struct LabelAtlasPixels{std::vector<uint8_t> pixels;uint32_t width{},height{},rowHeight{};std::array<uint32_t,D3D12Renderer::LabelRows> widths{};};
    std::array<std::wstring,D3D12Renderer::LabelRows> LabelAtlasTexts()const{
        // "DIFFERENCE x4 - LUMA": what the view is, how far it is amplified and which
        // channels, because a difference image without its gain cannot be read. Against
        // RTX VSR it names that too, since the same view then shows another engine.
        wchar_t gain[16]{};swprintf_s(gain,L"%g",double(m_comparison.differenceGain));
        const std::wstring against=m_comparison.againstVsr&&VsrUsable()?L" \u00b7 "+T(L"compare.tag.vsr"):std::wstring{};
        const std::wstring difference=T(L"compare.tag.difference")+against+L" \u00d7"+gain+L" \u00b7 "+T(m_comparison.differenceLuma?L"compare.tag.luma":L"compare.tag.color");
        // "RTX VSR - HIGH": the engine and the rung it ran at.
        const std::wstring vsr=T(L"compare.tag.vsr")+L" \u00b7 "+T((L"compare.tag.vsr_quality_"+std::to_wstring(vsr_policy::QualityIndex(m_comparison.vsrQuality))).c_str());
        return{T(L"compare.tag.original"),T(L"compare.tag.dlss"),difference,
               T(L"compare.tag.dlss")+L" \u00b7 "+T(L"compare.tag.mix")+L" "+PercentText(m_comparison.secondMix),vsr};
    }
    LabelAtlasPixels BuildLabelAtlas(UINT dpi)const{
        LabelAtlasPixels atlas;
        const auto texts=LabelAtlasTexts();
        const auto scale=[&](int dip){return MulDiv(dip,static_cast<int>(dpi?dpi:USER_DEFAULT_SCREEN_DPI),USER_DEFAULT_SCREEN_DPI);};
        const int rowHeight=scale(22),rule=std::max(2,scale(2)),padLeft=rule+scale(8),padRight=scale(8);
        HDC screen=GetDC(nullptr);HDC dc=CreateCompatibleDC(screen);ReleaseDC(nullptr,screen);
        if(!dc)return atlas;
        HFONT font=CreateFontW(-scale(12),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        const HGDIOBJ oldFont=SelectObject(dc,font?font:GetStockObject(DEFAULT_GUI_FONT));
        SetTextCharacterExtra(dc,std::max(1,scale(1)));
        std::array<SIZE,D3D12Renderer::LabelRows> extents{};int width=1;
        for(size_t row=0;row<texts.size();++row){
            if(texts[row].empty())continue;
            GetTextExtentPoint32W(dc,texts[row].c_str(),int(texts[row].size()),&extents[row]);
            atlas.widths[row]=uint32_t(padLeft+extents[row].cx+padRight);width=std::max(width,int(atlas.widths[row]));
        }
        const int height=rowHeight*int(texts.size());
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(info.bmiHeader);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* bits=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!bitmap||!bits){SelectObject(dc,oldFont);if(font)DeleteObject(font);if(bitmap)DeleteObject(bitmap);DeleteDC(dc);return atlas;}
        const HGDIOBJ oldBitmap=SelectObject(dc,bitmap);
        const compare_labels::Bgra plate{6,5,5,255},ink{232,235,236,255},mark{26,106,255,255};
        RECT all{0,0,width,height};HBRUSH plateBrush=CreateSolidBrush(RGB(plate.r,plate.g,plate.b));FillRect(dc,&all,plateBrush);DeleteObject(plateBrush);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(ink.r,ink.g,ink.b));
        for(size_t row=0;row<texts.size();++row)
            if(!texts[row].empty())TextOutW(dc,padLeft,int(row)*rowHeight+(rowHeight-extents[row].cy)/2,texts[row].c_str(),int(texts[row].size()));
        GdiFlush();
        atlas.pixels.assign(size_t(width)*size_t(height)*4u,0);
        const auto* source=static_cast<const compare_labels::Bgra*>(bits);
        auto* target=reinterpret_cast<compare_labels::Bgra*>(atlas.pixels.data());
        for(int y=0;y<height;++y){
            const size_t row=size_t(y/rowHeight);
            for(int x=0;x<int(atlas.widths[row]);++x){
                const size_t at=size_t(y)*size_t(width)+size_t(x);
                target[at]=x<rule?mark:compare_labels::Premultiply(source[at],plate,ink,0.78f);
            }
        }
        SelectObject(dc,oldBitmap);DeleteObject(bitmap);SelectObject(dc,oldFont);if(font)DeleteObject(font);DeleteDC(dc);
        atlas.width=uint32_t(width);atlas.height=uint32_t(height);atlas.rowHeight=uint32_t(rowHeight);
        return atlas;
    }
    // Uploads the tags when the renderer has none or the DPI or the text changed. A
    // synchronous upload (it drains the queue), so it runs when a comparison starts
    // using the reference, not per frame; a renderer that refused one is not asked
    // again until something changes.
    // Called per presented pair while comparing, so the common case is two compares and
    // no allocation: the texts are only rebuilt when m_labelTextRevision says they moved.
    void EnsureLabelAtlas(){
        if(!m_renderer||!m_hwnd)return;
        const UINT dpi=ActiveWindowDpi(m_hwnd);
        const bool current=m_labelAtlasDpi==dpi&&m_labelAtlasRevision==m_labelTextRevision;
        if(current&&(m_renderer->HasLabelAtlas()||m_labelAtlasRefusedBy==m_renderer.get()))return;
        const auto atlas=BuildLabelAtlas(dpi);
        const bool uploaded=!atlas.pixels.empty()&&m_renderer->SetLabelAtlas(atlas.pixels.data(),atlas.width,atlas.height,atlas.rowHeight,atlas.widths);
        m_labelAtlasDpi=dpi;m_labelAtlasRevision=m_labelTextRevision;m_labelAtlasRefusedBy=uploaded?nullptr:m_renderer.get();
        if(!uploaded)LOG("Comparison tags unavailable: the label atlas was not uploaded ("<<atlas.width<<"x"<<atlas.height<<").");
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
            const start_screen::Layout start=StartLayout();
            const IdleSurfaceLayout& idle=start.core;
            PaintStartScreenExtras(dc,start);
            SetBkMode(dc,TRANSPARENT);
            SetTextColor(dc,RGB(242,243,245));auto of=SelectObject(dc,m_fontTitle?m_fontTitle:m_font);std::wstring tt=T(L"idle.title");RECT title=idle.title;DrawTextW(dc,tt.c_str(),-1,&title,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            SetTextColor(dc,ui_palette::SecondaryText);SelectObject(dc,m_fontSmall);std::wstring ss=m_youtubeLifecycle.IsResolving()?m_cachedStatus:(m_cacheNotice.empty()?T(L"idle.subtitle"):m_cacheNotice);RECT subtitle=idle.subtitle;DrawTextW(dc,ss.c_str(),-1,&subtitle,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);SelectObject(dc,of);
            for(const auto& item:idle.actions){const auto content=ButtonContent(item.action,true);DrawButton(dc,item.action,content.icon,content.label,item.bounds,content.enabled,false,content.enabled&&m_hoverAction==item.action,m_pressedToolbarAction==item.action,FocusCueFor(item.action),false);}
            if(!YouTubePlaybackAvailable()){SetBkMode(dc,TRANSPARENT);SetTextColor(dc,ui_palette::SecondaryText);of=SelectObject(dc,m_fontSmall);const bool compactReason=idle.subtitle.top==idle.subtitle.bottom;std::wstring reason=T(compactReason?L"idle.youtube_unavailable_compact":L"idle.youtube_unavailable");RECT reasonRect=idle.youtubeReason;DrawTextW(dc,reason.c_str(),-1,&reasonRect,DT_CENTER|DT_TOP|DT_WORDBREAK|DT_END_ELLIPSIS|DT_NOPREFIX);SelectObject(dc,of);}return;
        }
        if(!ControlsVisible())return;
        RECT bar{0,c.bottom-ControlHeight(),c.right,c.bottom};HBRUSH bg=CreateSolidBrush(ui_palette::ControlSurface);FillRect(dc,&bar,bg);DeleteObject(bg);HPEN line=CreatePen(PS_SOLID,1,RGB(54,56,61));auto op=SelectObject(dc,line);MoveToEx(dc,0,bar.top,nullptr);LineTo(dc,c.right,bar.top);SelectObject(dc,op);DeleteObject(line);
        // Only what the dirty rectangle reaches is drawn. The chrome's
        // animations repaint one row at a time - the status row for a toast,
        // the timeline for the glow - up to 60 times a second on the thread
        // that presents the video, and every button measured its label and the
        // compare bar re-measured all of its own on each of those paints.
        const auto reaches=[&](RECT r){InflateRect(&r,Dip(2),Dip(2));return RectVisible(dc,&r)!=FALSE;};
        if(CompareBarVisible()&&reaches(RECT{0,c.bottom-ControlHeight(),c.right,c.bottom-ControlHeight()+Dip(compare_bar::kBarHeightDip)}))DrawCompareBar(dc);
        const auto toolbarItems=ToolbarItems();
        for(const auto& item:toolbarItems){if(!reaches(item.bounds))continue;const auto content=ButtonContent(item.action);const bool hover=content.enabled&&m_hoverAction==item.action;DrawButton(dc,item.action,content.icon,content.label,item.bounds,content.enabled,content.active,hover,m_pressedToolbarAction==item.action,FocusCueFor(item.action),item.compact,content.working);}
        const auto volumeRect=LayoutVolumeSlider(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd),toolbarItems);
        if(volumeRect&&reaches(RECT{volumeRect->left-Dip(24),volumeRect->top-Dip(16),c.right,volumeRect->bottom})){
            const auto now=Clock::now();
            const slider::Geometry g=VolumeSliderGeometry(*volumeRect,now);
            DrawSliderRail(dc,g,true);DrawSliderKnob(dc,g,true,false,ActiveWindowDpi(m_hwnd));
            const double bubble=m_volumeBubble.Level(now);
            if(bubble>0.0)DrawSliderBubble(dc,g.bubble,std::to_wstring(int(std::lround(m_volume*100)))+L"%",bubble,ui_palette::ControlSurface);
        }
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
            // What a segment just added is revealed left to right (BandGrowth):
            // the part not revealed yet is still the empty lane.
            if(m_liveSession){
                HBRUSH lane=CreateSolidBrush(RGB(68,71,77));
                for(const auto& hidden:m_bandGrowth.Hidden(Clock::now())){
                    RECT cover{markerX(hidden.start),tr.bottom-coverageLane,markerX(hidden.end),tr.bottom};
                    if(cover.right>cover.left)FillRect(dc,&cover,lane);
                }
                DeleteObject(lane);
            }
            DrawCompletionGlow(dc,RECT{rendered.left,tr.bottom-coverageLane,rendered.right,tr.bottom});
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
        // The lane is kept clear for the whole of a live session, not only once
        // it has coverage: before the first segment lands the lane holds the
        // hatched rendering-now stretch, and a seek past it used to paint the
        // played progress straight over it - the one sign of where the render
        // was working vanished during the wait it explains.
        const bool laneKept=renderedSpan||m_liveSession;
        RECT done{clampProgress?rendered.left:tr.left,tr.top,0,laneKept?tr.bottom-coverageLane:tr.bottom};
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
            RECT span{markerX(*m_markers.in100ns),tr.top,markerX(*m_markers.out100ns),laneKept?tr.bottom-coverageLane:tr.bottom};
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
        // The status-line slot crossfades to a toast and back: the line fades
        // toward the strip as the toast fades in, so no part of it shows behind
        // or after the toast, and returns as the toast goes. Without animations
        // the two simply swap.
        const auto toastFrame=m_toast.At(Clock::now(),m_activityMotionEnabled);
        const double lineLevel=toastFrame.visible?1.0-toastFrame.alpha:1.0;
        if(lineLevel>0.0){
            SetTextColor(dc,chrome_motion::Mix(ui_palette::ControlSurface,RGB(206,208,212),lineLevel));
            DrawTextW(dc,m_cachedStatus.c_str(),-1,&sr,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        }
        PaintToast(dc,RECT{statusRow.text.left-Dip(2),statusRow.text.top,statusRow.text.right,statusRow.text.bottom});
        if(volumeRect){const RECT& vr=*volumeRect;std::wstring vol=m_muted?T(L"status.muted"):(T(L"status.volume")+L" "+std::to_wstring(int(m_volume*100))+L"%");RECT label{vr.right+Dip(8),vr.top,std::max<LONG>(vr.right+Dip(8),c.right-Dip(16)),vr.bottom};DrawTextW(dc,vol.c_str(),int(vol.size()),&label,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);}SelectObject(dc,of);
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

    // ---- Window placement --------------------------------------------------
    // Saved on close (the placement from before fullscreen when it closes in
    // fullscreen) and restored at start when window_placement::Usable says the
    // screens it was on are still there; otherwise the first-run size stands.
    // GetWindowPlacement's rectangles are in workspace coordinates; they are
    // shifted by the primary monitor's work-area offset for the check only, and
    // handed back to SetWindowPlacement as they were saved.
    void SaveWindowPlacement(){
        if(!m_hwnd)return;
        WINDOWPLACEMENT placement{sizeof(placement)};
        if(m_fullscreen&&m_placementBeforeFullscreen)placement=*m_placementBeforeFullscreen;
        else if(!GetWindowPlacement(m_hwnd,&placement))return;
        const bool maximized=placement.showCmd==SW_SHOWMAXIMIZED||(placement.showCmd==SW_SHOWMINIMIZED&&(placement.flags&WPF_RESTORETOMAXIMIZED));
        const std::wstring text=window_placement::Format({placement.rcNormalPosition,maximized});
        WritePrivateProfileStringW(L"Window",L"Placement",text.c_str(),SettingsPath().c_str());
    }
    static std::vector<RECT> MonitorWorkAreas(){
        std::vector<RECT> areas;
        EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM parameter)->BOOL{
            MONITORINFO info{sizeof(info)};
            if(GetMonitorInfoW(monitor,&info))reinterpret_cast<std::vector<RECT>*>(parameter)->push_back(info.rcWork);
            return TRUE;
        },reinterpret_cast<LPARAM>(&areas));
        return areas;
    }
    void RestoreWindowPlacement(){
        wchar_t text[128]{};
        GetPrivateProfileStringW(L"Window",L"Placement",L"",text,static_cast<DWORD>(std::size(text)),SettingsPath().c_str());
        const auto saved=window_placement::Parse(text);
        if(!saved)return;
        // Workspace to screen: the primary monitor's work area may not start at
        // its corner (a taskbar on the top or the left).
        MONITORINFO primary{sizeof(primary)};
        GetMonitorInfoW(MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY),&primary);
        window_placement::Saved screen=*saved;
        OffsetRect(&screen.normal,primary.rcWork.left-primary.rcMonitor.left,primary.rcWork.top-primary.rcMonitor.top);
        const auto areas=MonitorWorkAreas();
        const POINT minimum=MinimumPlayerWindowTrackSize(m_hwnd,ActiveWindowDpi(m_hwnd));
        if(!window_placement::Usable(screen,areas,SIZE{minimum.x,minimum.y})){
            LOG("Saved window placement "<<WideToUtf8(text)<<" is off the current screens; using the first-run size.");
            return;
        }
        WINDOWPLACEMENT placement{sizeof(placement)};
        placement.rcNormalPosition=saved->normal;
        placement.showCmd=saved->maximized?SW_SHOWMAXIMIZED:SW_SHOWNORMAL;
        SetWindowPlacement(m_hwnd,&placement);
        LOG("Window placement restored: "<<WideToUtf8(text)<<".");
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
        const auto forecast=LiveForecast(m_decoder.Width(),m_decoder.Height(),fps);
        if(!forecast.measured){
            // No prior exists for every RTX generation, and inventing one would
            // be a guess dressed as a measurement. Say so instead of implying
            // the card was checked and passed.
            LOG("Active neural session pace is unmeasured on this GPU ("<<GpuPathName(m_opt.detectedGpu.generation)
                <<"); this session measures it.");
            return true;
        }
        if(forecast.keepsUp)return true;
        // The rung is part of what was agreed to: a yes at 50% says nothing
        // about the slower 100%.
        const std::string key=LiveSourceGeometryKey()+"@"+std::to_string(m_processingScale);
        if(key==m_livePaceConfirmedKey)return true;
        LOG("Active neural session forecast: "<<m_decoder.Width()<<"x"<<m_decoder.Height()<<" at "<<fps
            <<" fps and "<<m_processingScale<<"% processing scale renders at about "<<forecast.renderFps<<" fps ("<<forecast.realtimeRatio<<"x realtime).");
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
        return LiveSourceGeometryKey()+"|"+CanonicalNeuralSettings(m_neuralSettings)+"|"+CanonicalGuideControls(m_renderGuides)+
               "|"+CanonicalTemporalSettings(m_temporalSettings);
    }
    // Deletes the segment files a published run retired (ReplaceRun), except
    // one a live decoder still has open or is opening: that one goes on a later
    // pass, once playback has crossed into the joined entry. A file something
    // else holds - a scanner, say - is simply tried again.
    // Hands what a published run retired, and playback is not reading, to the
    // reaper. Deleted here, on the thread that presents, the files cost 88-102
    // ms at the moment every render finished - playback fell 103-113 ms behind
    // and dropped 3 to 6 frames (BackgroundFileReaper.h).
    void SweepRetiredLiveSegments(const std::shared_ptr<NeuralSegmentIndex>& index){
        if(!index)return;
        std::vector<std::filesystem::path> removable;
        for(const std::filesystem::path& path:index->RetiredFiles()){
            if(m_synchronizedPlayback.HoldsFile(path))continue;
            index->ForgetRetired(path);removable.push_back(path);
        }
        m_segmentReaper.Remove(std::move(removable));
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
        m_liveRange=range;m_liveTarget={};m_liveSession=true;m_liveAttached=false;m_liveRenderFailures=0;m_liveSessionStartedAt=Clock::now();
        // The pace clock starts at the first rendered segment, not here: a first
        // watch spends a minute acquiring the source before a frame is rendered,
        // and counting that made a GPU measured at 2x report 0.48x real time.
        m_livePaintedRevision=m_liveSegments->Revision();m_liveStartTick=0;m_neuralRequested=true;
        m_liveCoveredAtStart=CoveredDuration100ns(LiveCoverage(),CoverageSpan{range.start100ns,range.end100ns});
        m_livePaceWidth=m_decoder.Width();m_livePaceHeight=m_decoder.Height();m_livePaceScale=m_processingScale;
        // A GPU that renders far faster than real time refills the buffer faster
        // than playback drains it, so the four-second cushion is only a wait.
        m_liveForecastRatio=LiveForecast(m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate()).realtimeRatio;
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
            m_cachedReceiptPath=completion.receiptPath;m_cachedSettings=completion.settings;m_cachedGuides=completion.guides;m_cachedTemporal=completion.temporal;
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
        m_cachedPlayback=true;m_cachedRange=m_liveRange;m_cachedSettings=m_neuralSettings;m_cachedGuides=m_renderGuides;m_cachedTemporal=m_temporalSettings;m_cachedReceiptPath.clear();m_neuralPath.clear();
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
    // ---- Toasts --------------------------------------------------------------
    // A short confirmation - a file saved, a subtitle shift, a whole video
    // rendered - that takes the status row's slot for its hold, crossfading
    // with the line (chrome_motion::Toast), which it would otherwise be buried in.
    // Drawn by the strip's own GDI paint, not as a popup over the picture: a
    // layered popup over the D3D12 swap chain cost frames. Measured on the
    // fixture's render completion, three runs each: 6, 6 and 2 dropped with
    // the popup, 2, 2 and 0 with it fading in place, 0, 0 and 1 with no toast.
    // Here it repaints the status row only, as the chips' flash does.
    void ShowToast(std::wstring text,COLORREF mark=ui_palette::PrimaryBlue){
        if(!m_hwnd||text.empty())return;
        m_toastText=std::move(text);m_toastMark=mark;
        LOG("Toast: "<<WideToUtf8(m_toastText));
        m_toast.Show(Clock::now());
        AnimateToast();
    }
    void AnimateToast(){
        if(!m_hwnd)return;
        const auto now=Clock::now();
        if(m_loaded&&ControlsVisible()){const RECT dirty=StatusRect();InvalidateRect(m_hwnd,&dirty,FALSE);}
        if(!m_toast.At(now,m_activityMotionEnabled).visible){if(m_toastTimer){KillTimer(m_hwnd,m_toastTimer);m_toastTimer=0;}m_toast.Hide();return;}
        if(const auto next=m_toast.NextChange(now,m_activityMotionEnabled))
            m_toastTimer=SetTimer(m_hwnd,kToastTimerId,UINT(std::max<long long>(USER_TIMER_MINIMUM,next->count())),nullptr);
    }
    // The toast over the start of the status row: an Inactive-grey panel with
    // a mark on its left edge (teal for a render, the accent otherwise). It
    // fades by mixing toward the strip, which is one flat colour, and rises
    // from below the row, clipped to it.
    void PaintToast(HDC dc,const RECT& row){
        const auto frame=m_toast.At(Clock::now(),m_activityMotionEnabled);
        if(!frame.visible||row.right<=row.left)return;
        const int saved=SaveDC(dc);if(!saved)return;
        IntersectClipRect(dc,row.left,row.top,row.right,row.bottom);
        const HGDIOBJ oldFont=SelectObject(dc,m_fontSmall?m_fontSmall:m_font);
        SIZE text{};GetTextExtentPoint32W(dc,m_toastText.c_str(),int(m_toastText.size()),&text);
        const LONG width=std::min<LONG>(text.cx+Dip(3+10+12),row.right-row.left);
        RECT panel{row.left,row.top,row.left+width,row.bottom};
        OffsetRect(&panel,0,LONG(std::lround(frame.rise*Dip(chrome_motion::kToastRiseDip))));
        const COLORREF under=ui_palette::ControlSurface;
        const int radius=std::max(1,Dip(4));
        HBRUSH fill=CreateSolidBrush(chrome_motion::Mix(under,ui_palette::Inactive,frame.alpha));HPEN pen=CreatePen(PS_SOLID,1,chrome_motion::Mix(under,ui_palette::Inactive,frame.alpha));
        const HGDIOBJ oldBrush=SelectObject(dc,fill),oldPen=SelectObject(dc,pen);
        RoundRect(dc,panel.left,panel.top,panel.right,panel.bottom,radius*2,radius*2);
        SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(fill);DeleteObject(pen);
        RECT markRect{panel.left,panel.top+Dip(3),panel.left+Dip(3),panel.bottom-Dip(3)};
        HBRUSH ink=CreateSolidBrush(chrome_motion::Mix(under,m_toastMark,frame.alpha));FillRect(dc,&markRect,ink);DeleteObject(ink);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,chrome_motion::Mix(under,ui_palette::PrimaryText,frame.alpha));
        RECT label{panel.left+Dip(3+10),panel.top,panel.right-Dip(12),panel.bottom};
        DrawTextW(dc,m_toastText.c_str(),-1,&label,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        SelectObject(dc,oldFont);RestoreDC(dc,saved);
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
    // The render report (P2.11): the receipt's temporal metrics in words. Static and
    // fed the receipt's text, so what it says about a receipt is testable without a
    // render; the caller reads the file and shows the result.
    static std::wstring RenderReportText(const Localizer& localizer,std::string_view receipt,const std::wstring& title){
        strict_json::JsonValue document;
        if(!strict_json::JsonParser(receipt).ParseDocument(document))return localizer.Get(L"report.unavailable");
        const strict_json::JsonValue* result=document.Member("result");
        const strict_json::JsonValue* metrics=result?result->Member("metrics"):nullptr;
        if(!metrics||metrics->kind!=strict_json::JsonValue::Kind::Object)return localizer.Get(L"report.unmeasured");
        const auto number=[&](std::string_view key)->std::optional<double>{
            const strict_json::JsonValue* value=metrics->Member(key);
            if(!value||value->kind!=strict_json::JsonValue::Kind::Number)return std::nullopt;
            char* end=nullptr;const double parsed=std::strtod(value->text.c_str(),&end);
            if(end==value->text.c_str()||!std::isfinite(parsed))return std::nullopt;
            return parsed;
        };
        const auto codes=[](double value,bool sign){
            wchar_t text[32]{};swprintf_s(text,sign?L"%+.2f":L"%.2f",value);return std::wstring(text);
        };
        const auto count=[&](std::string_view key){const auto value=number(key);return value?std::to_wstring(uint64_t(*value)):std::wstring(L"-");};
        const auto frames=number("frames"),sourceWarp=number("sourceWarpError"),outputWarp=number("outputWarpError"),
                   sourceSigma=number("sourceSigma"),outputSigma=number("outputSigma"),luma=number("lumaShift"),
                   colour=number("colorDelta");
        if(!frames||!sourceWarp||!outputWarp||!sourceSigma||!outputSigma||!luma||!colour)
            return localizer.Get(L"report.unavailable");
        // The settings the render was made with, as its receipt recorded them.
        TemporalSettings temporal;
        if(const strict_json::JsonValue* request=document.Member("request"))
            if(const strict_json::JsonValue* recorded=request->Member("temporal");recorded&&recorded->kind==strict_json::JsonValue::Kind::String)
                temporal=ParseTemporalSettings(recorded->text).value_or(TemporalSettings{});
        const std::wstring cuts=Utf8ToWide(std::string(scene_cut::SensitivityName(temporal.sceneCuts)));
        const std::wstring stability=Utf8ToWide(std::string(TemporalStabilityName(temporal.stability)));
        wchar_t body[2048]{};
        swprintf_s(body,localizer.Get(L"report.body").c_str(),title.c_str(),count("frames").c_str(),count("pairs").c_str(),
                   count("shots").c_str(),codes(*outputWarp-*sourceWarp,true).c_str(),codes(*sourceWarp,false).c_str(),
                   codes(*outputWarp,false).c_str(),codes(*outputSigma-*sourceSigma,true).c_str(),
                   codes(*sourceSigma,false).c_str(),codes(*outputSigma,false).c_str(),codes(*colour,false).c_str(),
                   codes(*luma,true).c_str(),cuts.c_str(),stability.c_str());
        return body;
    }
    void ShowRenderReport(){
        if(m_cachedReceiptPath.empty())return;
        std::string receipt;
        {
            std::ifstream file(m_cachedReceiptPath,std::ios::binary);
            receipt.assign(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
        }
        const std::wstring title=m_displayTitle.empty()?m_cachedReceiptPath.parent_path().filename().wstring():m_displayTitle;
        const std::wstring text=RenderReportText(Localizer{},receipt,title);
        MessageBoxW(m_hwnd,text.c_str(),T(L"report.title").c_str(),MB_OK|MB_ICONINFORMATION);
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
            // What the job reads, captured here: the job thread never touches
            // the player (NeuralJobRun).
            NeuralJobInputs job;
            job.target=m_hwnd;job.generation=generation;job.mediaUrl=mediaUrl;job.audioUrl=audioUrl;job.displayTitle=displayTitle;job.pageUrl=pageUrl;job.sourceKind=sourceKind;job.sourceQuality=sourceQuality;
            job.gpu=m_opt.detectedGpu.generation;job.driverVersion=m_opt.detectedGpu.driverVersion;job.moduleDirectory=ExecutableDirectory();job.cacheRoot=m_cacheRoot;
            job.guides=m_renderGuides;job.temporal=m_temporalSettings;job.settings=m_neuralSettings;job.pauseEvent=m_neuralPauseEvent;
            job.gpuColorConversion=m_gpuColorConversion;job.gpuSourceConversion=m_gpuSourceConversion;job.nvencPreset=m_nvencPreset;job.processingScale=m_processingScale;job.captureDither=m_captureDither;job.cacheQuality=m_cacheQuality;job.sourceDeband=m_sourceDeband;job.suppliedExposure=m_suppliedExposure;
            job.reuseSourceKey=reuseSourceKey;job.expectedDurationSeconds=expectedDurationSeconds;job.range=range;job.prepareOnly=prepareOnly;
            // The background acquisition of this very source, when one is in
            // flight: the job waits for it rather than downloading again.
            job.prefetch=(sourceKind==MediaSourceKind::YouTube&&!pageUrl.empty()&&pageUrl==m_prefetchPageUrl)?m_prefetchState:nullptr;
            job.progressMessages=&m_neuralProgressMessages;job.completions=&m_neuralCompletions;
            // An active session renders into its own directory of segment files;
            // the cache entry is the concatenation published when the job ends.
            // A resumed or retargeted session keeps every earlier job's files, so
            // each job gets its own subdirectory and its own run id. The id is
            // what makes a relaunch discard its own segments and nobody else's:
            // segments are sorted by timestamp now, so this job's are not
            // necessarily the tail of the index.
            job.liveIndex=kind==NeuralJobKind::Live?m_liveSegments:nullptr;
            job.liveRunId=job.liveIndex?uint64_t(++m_liveJobSerial):0u;
            if(job.liveIndex){
                job.liveDirectory=m_liveDirectory/(L"job"+std::to_wstring(job.liveRunId));
                std::error_code ec;std::filesystem::create_directories(job.liveDirectory,ec);
                if(ec){
                    // Begin() above already marked a job as running; leaving it
                    // there kept the spinner on and every render action greyed
                    // out for the rest of the file, with nothing to say why.
                    LOG("Active neural session could not create the segment directory "<<WideToUtf8(job.liveDirectory.wstring())<<": "<<ec.message());
                    m_neuralLifecycle.Invalidate();m_neuralProgress={};m_pendingNeuralTitle.clear();
                    m_neuralNotice=T(L"neural.live.directory_failed");
                    SyncSourceActionAvailability();UpdateCachedStatus();InvalidateRect(m_hwnd,nullptr,FALSE);
                    return false;
                }
            }
            // Nothing can be shown until the first file is muxed, so the first
            // one is short. Later files stay long: a boundary costs an encoder
            // start and a mux, and only the first one is on the user's clock.
            job.segmentFrames=kind==NeuralJobKind::Live?neural_job::SegmentFrames(m_decoder.FrameRate(),kLiveSegmentSeconds):0u;
            job.firstSegmentFrames=kind==NeuralJobKind::Live?neural_job::SegmentFrames(m_decoder.FrameRate(),kLiveFirstSegmentSeconds):0u;
            // The driver verdict and the latched preflight failure are read on
            // this thread; the job only needs the answers.
            job.driverNotice=NeuralDriverNoticeText();
            job.cacheFailureText=CacheFailureText();
            job.preflightKey=NeuralPreflightKey{m_opt.detectedGpu.description,m_opt.detectedGpu.driverVersion,std::string{}};
            job.preflightLatch=&m_preflightLatch;
            // The helper this player keeps between jobs. Reached by pointer, like
            // the latch above: one job runs at a time and the UI thread joins it
            // before touching either, so the job thread owns both while it runs.
            job.residentHelper=&m_residentHelper;
            // The request instant: every phase is measured from here, and the
            // total the acceptance criterion names ends when the first neural
            // frame reaches the screen.
            m_coldStart=std::make_shared<NeuralColdStartRecord>();
            job.coldStart=m_coldStart;
            job.sourceDigestMemo=m_sourceDigestMemo;
            m_neuralWorker=std::jthread([job=std::move(job)](std::stop_token stop){NeuralJobRun(job,std::move(stop)).Run();});
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
        // The pair settles on BGRA when the neural file cannot take NV12, and the
        // renderer is configured from m_decoder: describe the original again with
        // the layout the pair actually decodes to.
        if(m_synchronizedPlayback.Layout()!=m_decoder.PixelLayout()){
            LOG("Cached pair decodes to "<<(m_synchronizedPlayback.Layout()==VideoPixelLayout::Nv12?"NV12":"BGRA")<<"; the neural file could not share the original's layout.");
            if(!m_decoder.OpenMetadata(completion.sourcePath.wstring(),MediaSourceKind::LocalFile,{},m_synchronizedPlayback.Layout()==VideoPixelLayout::Nv12)||m_decoder.PixelLayout()!=m_synchronizedPlayback.Layout()){Unload();return false;}
        }
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
        m_neuralPath=completion.neuralPath;m_cachedRange=completion.range;m_cachedReceiptPath=completion.receiptPath;m_cachedSettings=completion.settings;m_cachedGuides=completion.guides;m_cachedTemporal=completion.temporal;m_currentSec=double(first.timestamp100ns)*1e-7;m_haveNext=false;m_cachedPlayback=true;m_comparisonView=desiredView;m_cachedPresentedFrames=1;RememberRenderedCachedPair();
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
        // The status line and the chips are cached text: a phase change is
        // what the warm-up line reports, so it has to reach them.
        if(m_liveSession)UpdateCachedStatus();
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
        candidate->renderer->SetSourceColor(completion.decoder->DecodedColor());
        candidate->renderer->SetPresentFollowsWindow(true);
        candidate->renderer->SetHdrOutputAllowed(true);
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
        m_cachedSettings={};m_cachedGuides={};m_cachedTemporal={};m_neuralPath.clear();m_markers={};
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
    // Where a live session's cold start is, while nothing is rendered yet.
    warm_up::Step WarmUpStep()const{
        return warm_up::Resolve(m_liveSession,NeuralJobActive(),!LiveCoverage().empty(),m_neuralProgress.phase,m_neuralProgress.completedFrames);
    }
    std::wstring LiveSessionStatusText()const{
        // The cold start names its step instead of "0.0 s buffered", which is
        // true and says nothing for the 17-22 s it lasts.
        if(const auto step=WarmUpStep();step!=warm_up::Step::None){
            std::wstring text=T(L"warmup.title")+L" \u00b7 "+T(warm_up::TextKey(step));
            if(m_liveBuffering)text+=L" \u00b7 "+T(LiveResumePending()?L"neural.live.will_play":L"neural.live.will_stay_paused");
            return text;
        }
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
        else if(!LiveForecast(m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate()).measured)
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
            if(const std::wstring hdr=HdrStatusText();!hdr.empty())text+=L" \u00b7 "+hdr;
            if(!CachedRangeCoversSource())text+=L" \u00b7 Range "+FormatTimecode(m_cachedRange.start100ns,m_decoder.FrameRate(),true)+L"\u2013"+FormatTimecode(m_cachedRange.end100ns,m_decoder.FrameRate(),true);
            if(const std::wstring markers=MarkerStatusText();!markers.empty())text+=L" \u00b7 "+markers;
            text+=L" \u00b7 "+NeuralSettingsSummary(m_cachedSettings,m_cachedGuides);
            if(!m_cachedTemporal.IsDefault())text+=L"/"+Utf8ToWide(CanonicalTemporalSettings(m_cachedTemporal));
            if(const std::wstring passthrough=AudioPassthroughNote();!passthrough.empty())text=passthrough+L" \u00b7 "+text;
            if(m_liveSession)text=LiveSessionStatusText()+L" \u00b7 "+text;
            if(m_seeking||m_seekPending)text=T(L"status.seeking")+L" \u00b7 "+text;
            if(const std::wstring dropped=m_dropNote.Visible(m_loaded,m_path);!dropped.empty())text=dropped+L" \u00b7 "+text;
            return text;
        }
        const PlayerRuntimeStatus runtime=RuntimeStatus();status.mediaLoaded=true;status.runtimeConfiguration=runtime.configuration;status.dlssState=runtime.dlssState;status.sourceWidth=m_decoder.NativeWidth();status.sourceHeight=m_decoder.NativeHeight();status.inputWidth=m_renderer->DLSSInputW();status.inputHeight=m_renderer->DLSSInputH();status.outputWidth=m_renderer->OutputW();status.outputHeight=m_renderer->OutputH();status.quality=QualityNameW(m_activeQuality);
        status.upscalingStatus=UpscalingStatus();status.frameGenerationStatus=FrameGenerationStatus();std::wstring text=BuildPlayerStatusText(status);
        // Ahead of the runtime detail only: it answers a toggle, and a narrow
        // window should lose the counters before it loses why the receiver is
        // getting PCM.
        if(const std::wstring passthrough=AudioPassthroughNote();!passthrough.empty())text=passthrough+L" \u00b7 "+text;
        // Lead with what was marked, or with how to mark, because the runtime
        // detail behind it is what a narrow window truncates.
        if(const std::wstring markers=MarkerStatusText();!markers.empty())text=markers+L" \u00b7 "+text;
        else if(!m_liveSession&&RangeRenderAvailable())text=T(L"status.render_hint")+L" \u00b7 "+text;
        // Ahead of the hint and the runtime detail: it says why the picture is not
        // the HDR one the file carries, and what the model will be shown.
        if(const std::wstring hdr=HdrStatusText();!hdr.empty())text=hdr+L" \u00b7 "+text;
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
    // An HDR source is decoded tone mapped to SDR (HdrPolicy.h), because the
    // neural pass is SDR; the line says so, or the flatter picture reads as a
    // fault in the player.
    std::wstring HdrStatusText()const{
        if(!m_loaded||m_decoder.SourceHdrSignal()==hdr_policy::HdrSignal::Sdr)return{};
        // On an HDR display the original is either HDR on screen or deliberately
        // compared at SDR, and which is the thing worth reading.
        if(m_renderer&&m_renderer->HdrOutputActive())
            return T(OriginalDecodedAsPq()?L"status.hdr_original":L"status.hdr_at_sdr");
        return T(L"status.hdr_tonemapped");
    }
    bool OriginalDecodedAsPq()const{
        return m_cachedPlayback?m_lastPair&&m_lastPair->original.pq
                               :m_lastPlaybackFrame&&m_lastPlaybackFrame->pq;
    }
    // HDR output and the original's decode (P3.1). The display is asked only when
    // something says it may have changed - a new renderer, a display change, the
    // end of a move - and the decode is re-decided every tick from state that is
    // free to read, so every path that changes a view, the upscaler or the file
    // is covered without each having to remember to ask.
    void SyncHdrPresentation(){
        if(!m_renderer)return;
        if(m_renderer->Instance()!=m_hdrRenderer||m_hdrRecheck){
            m_hdrRenderer=m_renderer->Instance();m_hdrRecheck=false;
            const D3D12Renderer::DisplayHdrState display=m_renderer->QueryDisplayHdr();
            if(display.known!=m_hdrDisplay.known||display.hdr!=m_hdrDisplay.hdr||display.device!=m_hdrDisplay.device||
               display.sdrWhiteNits!=m_hdrDisplay.sdrWhiteNits)
                LOG("Display under the player: "<<(display.known?WideToUtf8(display.device):std::string("none found"))
                    <<(display.hdr?" in HDR mode":" in SDR mode")<<", SDR white "<<display.sdrWhiteNits
                    <<" nits, peak "<<display.maxLuminanceNits<<" nits.");
            m_hdrDisplay=display;
            // Never the other way round: the player follows the display's mode and
            // never asks Windows to change it.
            if(!m_renderer->SetHdrOutput(display.hdr,display.sdrWhiteNits)&&display.hdr)
                LOG("HDR output could not be enabled on this display; presenting SDR.");
            UpdateCachedStatus();SyncFeatureMenuState();
        }
        SyncHdrOriginal();
    }
    void SyncHdrOriginal(){
        if(!m_loaded||!m_renderer||m_seeking)return;
        hdr_policy::OriginalPresentation state{};
        state.hdrSwapchain=m_renderer->HdrOutputActive();
        state.sourceHdr=m_decoder.SourceHdrSignal()!=hdr_policy::HdrSignal::Sdr;
        state.compareAtSdr=m_compareHdrAtSdr;
        state.sourceFeedsSdrConsumer=m_renderer->GuidesRequired();
        // The settled view, not a hold on the picture: a peek at the original from
        // Difference must not cost a decoder restart on the way in and out.
        ComparisonSettings settled=EffectiveComparison();
        if(m_peekOriginal&&ComparisonModesAvailable())settled.mode=SelectedComparisonMode();
        state.viewCombinesPixels=m_cachedPlayback&&ComparisonCombinesPixels(settled);
        const bool pq=hdr_policy::DecodeOriginalAsPq(state);
        bool changed=false;
        if(m_cachedPlayback)changed=m_synchronizedPlayback.SetOriginalHdrPresentation(pq);
        else if(m_decoder.HdrPresentation()!=pq){m_decoder.SetHdrPresentation(pq);changed=true;}
        // An SDR source decodes the same either way, and a stream's seek is a new
        // resolution, so a network source takes the new decode at its next seek.
        if(!changed||!state.sourceHdr||NetworkPlayback())return;
        LOG("HDR original "<<(pq?"decoded as PQ for the HDR display":"tone mapped to SDR")<<"; decoding again at "<<Position()<<" s.");
        RequestSeek(Position());
    }
    void ToggleCompareHdrAtSdr(){
        m_compareHdrAtSdr=!m_compareHdrAtSdr;
        LOG("Compare HDR at SDR "<<(m_compareHdrAtSdr?"on":"off")<<".");
        SyncHdrOriginal();UpdateCachedStatus();SyncFeatureMenuState();
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
        case ToolbarAction::Aspect:SetAspect(m_onePixel?false:!m_fill,false);break;
        case ToolbarAction::Adjustments:ShowAdjustments();break;
        case ToolbarAction::DebugView:ShowDebugMenu(anchor);break;
        case ToolbarAction::Fullscreen:ToggleFullscreen();break;
        case ToolbarAction::None:break;
        }
    }

    void FocusNextToolbarAction(bool reverse){
        RevealFullscreenControls();m_fullscreenKeyboardFocus=m_fullscreen;
        const auto items=FocusableItems();m_focusedToolbarAction=NextFocusableToolbarAction(items,m_focusedToolbarAction,reverse,ToolbarState());m_keyboardCues=true;InvalidateControls();
    }

    void ActivateFocusedToolbarAction(){
        if(m_focusedToolbarAction==ToolbarAction::None)return;
        const auto items=FocusableItems();for(const auto& item:items)if(item.action==m_focusedToolbarAction){ActivateToolbarAction(item.action,item.bounds);return;}
    }

    void MouseDown(int x,int y){
        SetFocus(m_hwnd);
        if(m_keyboardCues){m_keyboardCues=false;InvalidateControls();}
        m_fullscreenKeyboardFocus=false;
        if(ActivityBusy()&&PtIn(m_neuralCancelBounds,x,y)){if(m_liveSession)StopLiveNeuralSession(true);else if(NeuralJobActive())CancelNeuralJob();else CancelYouTubeResolution();return;}
        if(!ControlsVisible())return;
        if(!m_loaded){const auto items=FocusableItems();const ToolbarAction action=ResolveToolbarHover(items,POINT{x,y},ToolbarState());if(action==ToolbarAction::None&&ActivateStartScreen(x,y))return;if(action!=ToolbarAction::None){m_focusedToolbarAction=action;m_pressedToolbarAction=action;SetCapture(m_hwnd);for(const auto& item:items)if(item.action==action){InvalidateRect(m_hwnd,&item.bounds,FALSE);break;}}return;}
        if(CompareBarMouseDown(x,y))return;
        // A source with no length has no position to map a click to - every
        // click would seek to the start - so its bar is greyed and inert;
        // Left and Right still seek, which is what the hover says.
        if(!m_seeking){RECT tr=TimelineRect();if(PtIn(tr,x,y)&&m_decoder.DurationSeconds()>0.0){m_dragSeek=true;m_dragWasPlaying=m_playing;m_lastScrubSeek={};m_seekPreview=SecondsFromX(x);SetCapture(m_hwnd);InvalidateControls();return;}const auto vr=VolumeRect();if(vr&&PtIn(*vr,x,y)){m_dragVolume=true;SetCapture(m_hwnd);SetVolumeFromX(x);SyncSliderHover();return;}}
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
        if(m_dragVolume){m_dragVolume=false;if(GetCapture()==m_hwnd)ReleaseCapture();SyncSliderHover();return;}
        if(m_dragMix){m_dragMix=false;if(GetCapture()==m_hwnd)ReleaseCapture();SyncSliderHover();return;}
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
    void SetVolumeFromX(int x){const auto volumeRect=VolumeRect();if(!volumeRect)return;const float volume=float(slider::ValueFromX(*volumeRect,x,ActiveWindowDpi(m_hwnd)));const bool changed=volume!=m_volume||m_muted;if(!changed)return;m_volume=volume;m_muted=false;Audio().SetVolume(m_volume);InvalidateToolbarAction(ToolbarAction::Mute);InvalidateVolumeControls();}
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
        m_hoverAction=ToolbarAction::None;ResetHoverFades();m_pressedToolbarAction=ToolbarAction::None;
        StopFullscreenTimer();SetMenu(m_hwnd,nullptr);DrawMenuBar(m_hwnd);ClearTimelineHover();
        Layout();InvalidateRect(m_hwnd,nullptr,FALSE);
    }
    void ToggleFullscreen(){
        ClearTimelineHover();
        if(!m_fullscreen){
            m_savedStyle=GetWindowLongW(m_hwnd,GWL_STYLE);GetWindowRect(m_hwnd,&m_savedRect);
            {WINDOWPLACEMENT before{sizeof(before)};if(GetWindowPlacement(m_hwnd,&before))m_placementBeforeFullscreen=before;}
            m_fullscreenMenu=GetMenu(m_hwnd);m_fullscreen=true;m_fullscreenControlsHidden=true;
            m_fullscreenKeyboardFocus=false;m_focusedToolbarAction=ToolbarAction::None;
            m_hoverAction=ToolbarAction::None;ResetHoverFades();m_pressedToolbarAction=ToolbarAction::None;
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
        if(m!=0&&m==TaskbarButtonCreatedMessage()){CreateThumbBar();return 0;}
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
        case WM_MEDIA_BUTTON:HandleMediaButton(int(w));return 0;
        case WM_MEDIA_SEEK:if(m_loaded)RequestSeek(double(l)/1000.0);return 0;
        case WM_START_SCREEN:if(!m_loaded)InvalidateRect(h,nullptr,FALSE);return 0;
        case dark_mode::WM_UAHDRAWMENU:if(DrawDarkMenuBar(h,reinterpret_cast<const dark_mode::UAHMENU*>(l)))return TRUE;break;
        case dark_mode::WM_UAHDRAWMENUITEM:if(DrawDarkMenuBarItem(h,reinterpret_cast<const dark_mode::UAHDRAWMENUITEM*>(l)))return TRUE;break;
        case WM_NCPAINT:case WM_NCACTIVATE:{const LRESULT result=DefWindowProcW(h,m,w,l);PaintMenuBarSeparator(h);return result;}
        case WM_TIMER:if(w==kActivityTimerId){AnimateActivity();return 0;}if(w==kFullscreenTimerId){AutoHideFullscreenControls();return 0;}if(w==kPreviewTimerId){StartPausedSettingsPreview();return 0;}if(w==kModalTickTimerId){if(m_modalTickTimer)RunTick();return 0;}if(w==kChipFlashTimerId){AnimateStatusChips();return 0;}if(w==kPeekTimerId){PeekHoldElapsed();return 0;}if(w==kHoverTimerId){AnimateHover();return 0;}if(w==kGlowTimerId){AnimateCompletionGlow();return 0;}if(w==kToastTimerId){AnimateToast();return 0;}if(w==kBandTimerId){AnimateBandGrowth();return 0;}break;
        case WM_ENTERMENULOOP:m_fullscreenMenuLoop=true;RevealFullscreenControls();StartModalTick();break;
        case WM_EXITMENULOOP:m_fullscreenMenuLoop=false;m_fullscreenLastInput=Clock::now();StopModalTick();break;
        case WM_ENTERSIZEMOVE:m_inSizeMove=true;StartModalTick();break;
        // The window may have been dragged onto another display: HDR or not, and
        // its own SDR white level.
        case WM_EXITSIZEMOVE:m_inSizeMove=false;m_hdrRecheck=true;StopModalTick();break;
        case WM_SYSKEYDOWN:if(w==VK_MENU)RevealFullscreenControls();break;
        case WM_SYSCOMMAND:if((w&0xfff0)==SC_KEYMENU)RevealFullscreenControls();break;
        case WM_NCDESTROY:
            StopFullscreenTimer();
            if(m_fullscreenMenu){if(IsMenu(m_fullscreenMenu)&&GetMenu(h)!=m_fullscreenMenu)DestroyMenu(m_fullscreenMenu);m_fullscreenMenu=nullptr;}
            break;
        case WM_SETTINGCHANGE:ReadAnimationPreference();InvalidateRect(h,nullptr,FALSE);break;
        case WM_SHOWWINDOW:SyncActivityFeedback();break;
        case WM_DESTROY:SaveWindowPlacement();CancelExport();DrainStageExportMessages();if(m_activityTimer){KillTimer(h,m_activityTimer);m_activityTimer=0;}StopModalTick();CancelNeuralJob(false);DrainNeuralMessages();CancelYouTubeResolution(false);DrainYouTubeCompletions();CancelSourcePrefetch();CancelUpdateCheck();m_running=false;PostQuitMessage(0);return 0;
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
            Layout();InvalidateRect(h,nullptr,FALSE);
            // The tags on the picture are drawn at the window's DPI.
            if(ComparisonModesAvailable())ApplyComparison();
            return 0;
        }
        // A mode change on the monitor the window is already on keeps the same
        // HMONITOR, so the handle comparison cannot see it and the cached mode
        // has to be dropped here. Switching a 4K panel to 1080p is exactly this
        // case, and it moves the Auto rung.
        // Also what Windows sends when its HDR switch is turned on or off.
        case WM_DISPLAYCHANGE:m_hdrRecheck=true;Audio().NoteDisplayModeChanged();InvalidateMonitorMode();ReportUpscaleRungDrift();Layout();InvalidateRect(h,nullptr,FALSE);return 0;
        case WM_SIZE:Layout();SyncActivityFeedback();RefreshToolbarTips();if(m_shortcutSheetOpen)ShowShortcutSheet();ClearTimelineHover();return 0;
        case WM_MOVE:if(!m_inSizeMove)m_hdrRecheck=true;if(m_shortcutSheetOpen)ShowShortcutSheet();ClearTimelineHover();break;
        case WM_PAINT:Paint();return 0;
        case WM_NCMOUSEMOVE:{
            POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
            if(ScreenToClient(h,&point))FullscreenPointerMoved(h,MAKELPARAM(point.x,point.y));
            break;
        }
        case WM_MOUSEMOVE:{FullscreenPointerMoved(h,l);m_mouseX=GET_X_LPARAM(l);m_mouseY=GET_Y_LPARAM(l);if(!m_trackingMouse){TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,h,0};m_trackingMouse=TrackMouseEvent(&tracking)!=FALSE;}CompareBarMouseMove(m_mouseX,m_mouseY);if(m_dragSeek&&GetCapture()==h){const double preview=SecondsFromX(m_mouseX);if(preview!=m_seekPreview){m_seekPreview=preview;InvalidatePlaybackProgress();ScrubToPreview();}}if(m_dragVolume&&GetCapture()==h)SetVolumeFromX(m_mouseX);SetHoverAction(ToolbarActionAt(m_mouseX,m_mouseY));UpdateTimelineHover(m_mouseX,m_mouseY);if(!m_loaded)UpdateStartHover(m_mouseX,m_mouseY);SyncSliderHover();return 0;}
        case WM_NOTIFY:if(const auto* header=reinterpret_cast<const NMHDR*>(l);header&&header->code==TTN_GETDISPINFOW)return TipNotify(*header);break;
        case WM_MOUSELEAVE:m_trackingMouse=false;m_mouseX=-999;m_mouseY=-999;SetHoverAction(ToolbarAction::None);SetCompareHover({});if(!m_dragSeek)ClearTimelineHover();SyncSliderHover();return 0;
        case WM_LBUTTONDOWN:MouseDown(GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
        case WM_LBUTTONUP:MouseUp(GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
        case WM_CAPTURECHANGED:if(m_dragSeek){m_dragSeek=false;EndScrub();InvalidateControls();UpdateTimelineHover(m_mouseX,m_mouseY);}if(m_dragVolume)m_dragVolume=false;m_dragMix=false;SyncSliderHover();if(m_pressedToolbarAction!=ToolbarAction::None){m_pressedToolbarAction=ToolbarAction::None;InvalidateControls();}return 0;
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
        case WM_MOUSEWHEEL:{
            // Ctrl+wheel over the picture zooms at the pointer; the plain wheel keeps
            // its job, the volume, everywhere.
            if(m_loaded&&(GET_KEYSTATE_WPARAM(w)&MK_CONTROL)&&m_renderWnd){
                POINT pointer{GET_X_LPARAM(l),GET_Y_LPARAM(l)};RECT client{};
                if(ScreenToClient(m_renderWnd,&pointer)&&GetClientRect(m_renderWnd,&client)&&PtIn(client,pointer.x,pointer.y)){
                    ZoomBy(GET_WHEEL_DELTA_WPARAM(w)>0?1:-1,false,pointer);return 0;
                }
            }
            if(m_loaded){const float step=(GET_WHEEL_DELTA_WPARAM(w)>0)?0.05f:-0.05f;const float volume=std::clamp(m_volume+step,0.0f,1.0f);const bool changed=m_muted||volume!=m_volume;if(changed){m_muted=false;m_volume=volume;Audio().SetVolume(m_volume);InvalidateToolbarAction(ToolbarAction::Mute);InvalidateVolumeControls();}}return 0;}
        case WM_COMMAND:HandleCommand(LOWORD(w));return 0;
        case WM_HOTKEY:HandleHotkey(int(w));return 0;
        // ? is a character, not a key: it is Shift+/ on a US layout and
        // somewhere else on most others, so it is matched after translation.
        case WM_CHAR:if(w==L'?'){ToggleShortcutSheet();return 0;}break;
        case WM_KEYDOWN:
            // F1 is Windows' help key; nothing else in the player used it.
            if(w==VK_F1){ToggleShortcutSheet();return 0;}
            // Esc's order is escape_key::Resolve: the sheet, then fullscreen, then
            // the longest-running job. Nothing else in this handler takes Esc.
            if(w==VK_ESCAPE){
                using escape_key::Action;
                switch(escape_key::Resolve({m_shortcutSheetOpen,m_fullscreen,m_frameGenWorker.joinable()&&!m_frameGenCancelling,
                                            m_liveSession,NeuralJobActive(),m_youtubeLifecycle.IsResolving()})){
                case Action::CloseShortcutSheet:HideShortcutSheet();return 0;
                case Action::LeaveFullscreen:ToggleFullscreen();return 0;
                case Action::CancelFrameGeneration:CancelFrameGeneration();return 0;
                case Action::StopLiveSession:StopLiveNeuralSession(true);return 0;
                case Action::CancelNeuralJob:CancelNeuralJob();return 0;
                case Action::CancelYouTube:CancelYouTubeResolution();return 0;
                case Action::None:break;
                }
            }
            if(w==VK_F10)RevealFullscreenControls();
            if(w==VK_TAB){FocusNextToolbarAction((GetKeyState(VK_SHIFT)&0x8000)!=0);return 0;}if(w==VK_RETURN&&m_focusedToolbarAction!=ToolbarAction::None){ActivateFocusedToolbarAction();return 0;}if(app_menu::RoutesToOpenYouTube(app_menu::PlayerCommandRoute::KeyDown,static_cast<UINT>(w),(GetKeyState(VK_CONTROL)&0x8000)!=0)){ActivateYouTube();return 0;}if((GetKeyState(VK_CONTROL)&0x8000)&&w=='O'){OpenFromDialog();return 0;}if((GetKeyState(VK_CONTROL)&0x8000)&&w=='E'){ShowAdjustments();return 0;}if(const auto command=app_menu::CommandForPlayerKey(static_cast<UINT>(w),(GetKeyState(VK_CONTROL)&0x8000)!=0,(GetKeyState(VK_SHIFT)&0x8000)!=0)){HandleCommand(*command);return 0;}if(w==VK_SPACE){TogglePause();return 0;}if(w==VK_OEM_PERIOD){StepCachedFrame();return 0;}if(w==VK_LEFT){RequestSeek(Position()-10);return 0;}if(w==VK_RIGHT){RequestSeek(Position()+10);return 0;}if(w==VK_F11){ToggleFullscreen();return 0;}if(app_menu::RoutesToRehook(app_menu::PlayerCommandRoute::KeyDown,static_cast<UINT>(w))){Rehook();return 0;}if(w=='S'){StopPlayback();return 0;}if(w=='A'){SetAspect(m_onePixel?false:!m_fill,false);return 0;}if(w=='D'){ToggleNeuralRendering();return 0;}if(w=='M'){ToggleMute();return 0;}if(w=='1'){SetDebug(D3D12Renderer::DebugView::Final);return 0;}if(w=='2'){SetDebug(D3D12Renderer::DebugView::Input);return 0;}if(w=='3'){SetDebug(D3D12Renderer::DebugView::MotionVectors);return 0;}if(w=='4'){SetDebug(D3D12Renderer::DebugView::Depth);return 0;}break;
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
        if(id>=IDM_COMPARE_MASK_FEATHER_FIRST&&id<IDM_COMPARE_MASK_FEATHER_FIRST+IDM_COMPARE_MASK_FEATHER_COUNT){SetMaskFeather(size_t(id-IDM_COMPARE_MASK_FEATHER_FIRST));return;}
        if(id>=IDM_COMPARE_SECOND_MIX_FIRST&&id<IDM_COMPARE_SECOND_MIX_FIRST+IDM_COMPARE_SECOND_MIX_COUNT){SetSecondMix(compare_settings::kSecondMixes[size_t(id-IDM_COMPARE_SECOND_MIX_FIRST)]);return;}
        if(id>=IDM_COMPARE_VSR_QUALITY_FIRST&&id<IDM_COMPARE_VSR_QUALITY_FIRST+IDM_COMPARE_VSR_QUALITY_COUNT){SetVsrQuality(vsr_policy::kQualities[size_t(id-IDM_COMPARE_VSR_QUALITY_FIRST)]);return;}
        if(const auto quality=app_menu::YouTubeQualityForCommand(id)){SetYouTubeSourceQuality(*quality);return;}
        if(id>=IDM_AUDIO_TRACK_FIRST&&id<IDM_AUDIO_TRACK_FIRST+IDM_AUDIO_TRACK_COUNT){
            ChooseAudioTrack(int(id-IDM_AUDIO_TRACK_FIRST));return;}
        if(id>=IDM_SUBTITLE_TRACK_FIRST&&id<IDM_SUBTITLE_TRACK_FIRST+IDM_SUBTITLE_TRACK_COUNT){
            SelectSubtitleTrack(int(id-IDM_SUBTITLE_TRACK_FIRST),true);return;}
        switch(id){
        case IDM_OPEN:OpenFromDialog();break;case IDM_EXIT:DestroyWindow(m_hwnd);break;case IDM_PLAY:TogglePause();break;case IDM_STOP:StopPlayback();break;case IDM_BACK10:RequestSeek(Position()-10);break;case IDM_FWD10:RequestSeek(Position()+10);break;case IDM_MUTE:ToggleMute();break;case IDM_NEURAL_RENDERING:ToggleNeuralRendering();break;
        case IDM_DLSS_UPSCALING:ToggleUpscaling();break;
        case IDM_UPSCALE_AUTO:SetUpscaleTarget(0);break;
        case IDM_PROCESSING_SCALE_FIRST:case IDM_PROCESSING_SCALE_FIRST+1:case IDM_PROCESSING_SCALE_LAST:
            if(const auto percent=app_menu::ProcessingScaleForCommand(id))SetProcessingScale(*percent);
            break;
        case IDM_UPSCALE_1080:SetUpscaleTarget(1080);break;
        case IDM_UPSCALE_1440:SetUpscaleTarget(1440);break;
        case IDM_UPSCALE_2160:SetUpscaleTarget(2160);break;
        case IDM_UPSCALE_HISTORY_TEMPORAL:SetUpscalingHistory(UpscalingHistory::Temporal);break;
        case IDM_UPSCALE_HISTORY_PER_FRAME:SetUpscalingHistory(UpscalingHistory::PerFrame);break;
        case IDM_FRAME_GENERATION:if(m_frameGenWorker.joinable())CancelFrameGeneration();else StartFrameGeneration();break;
        case IDM_SHOW_FRAMEGEN_OUTPUT:ShowFrameGenerationOutput();break;
        case IDM_FRAMEGEN_2X:SetFrameGenerationPreference(1);break;
        case IDM_FRAMEGEN_3X:SetFrameGenerationPreference(2);break;
        case IDM_FRAMEGEN_4X:SetFrameGenerationPreference(3);break;
        case IDM_FRAMEGEN_5X:SetFrameGenerationPreference(4);break;
        case IDM_FRAMEGEN_MAX:SetFrameGenerationPreference(0);break;
        case IDM_FRAMEGEN_EVEN_ONLY:SetEvenCadenceOnly(!m_evenCadenceOnly);break;
        case IDM_FRAMEGEN_HOLD_DUPLICATES:SetHoldDuplicateFrames(!m_frameGenHoldDuplicates);break;
        case IDM_AUDIO_PASSTHROUGH:SetAudioPassthrough(!m_audioPassthrough);break;
        case IDM_EXPORT_CACHED_VIDEO:ExportCachedVideo();break;
        case IDM_VIEW_FINAL:SetDebug(D3D12Renderer::DebugView::Final);break;case IDM_VIEW_INPUT:SetDebug(D3D12Renderer::DebugView::Input);break;case IDM_VIEW_MV:SetDebug(D3D12Renderer::DebugView::MotionVectors);break;case IDM_VIEW_DEPTH:SetDebug(D3D12Renderer::DebugView::Depth);break;case IDM_VIDEO_ADJUSTMENTS:ShowAdjustments();break;case IDM_ASPECT_FIT:SetAspect(false,false);break;case IDM_ASPECT_FILL:SetAspect(true,false);break;case IDM_FULLSCREEN:ToggleFullscreen();break;case IDM_ADVANCED_SAFE_MODE:RestartInSafeMode();break;case IDM_CLEAR_NEURAL_CACHE:ClearNeuralCache();break;
        case IDM_MARK_IN:SetMarker(true,Position100ns());break;case IDM_MARK_OUT:SetMarker(false,Position100ns());break;case IDM_CLEAR_MARKS:ClearMarkers();break;case IDM_GOTO_TIMECODE:ShowTimecodeDialog();break;
        case IDM_PAUSE_NEURAL_RENDER:if(NeuralJobActive())SetNeuralJobPaused(!NeuralJobPaused());break;
        case IDM_PREVIEW_FRAME:PreviewCurrentFrame();break;case IDM_PREVIEW_CLIP:PreviewClip();break;case IDM_RENDER_RANGE:RenderMarkedRange();break;case IDM_RENDER_WHOLE:RenderWholeSource();break;
        case IDM_NEURAL_SETTINGS:ShowNeuralSettings();break;
        // Handled before the switch would reach an unknown id, because the
        // presets are a contiguous range rather than named commands.
        case IDM_NEURAL_PRESET_CUSTOM:break;// reports state; not selectable
case IDM_EXPORT_STAGES:if(m_exportWorker.joinable())CancelExport();else ShowExportStages();break;case IDM_ENCODER_SETTINGS:ShowEncoderSettings();break;case IDM_OPEN_RENDER_RECEIPT:OpenRenderReceipt();break;case IDM_RENDER_REPORT:ShowRenderReport();break;
        case IDM_CHECK_FOR_UPDATES:MaybeStartUpdateCheck(true);break;
        case IDM_KEYBOARD_SHORTCUTS:ToggleShortcutSheet();break;
        case IDM_SUBTITLE_OFF:SelectSubtitlesOff(true);break;case IDM_SUBTITLE_FILE:SelectSubtitleFile(true);break;
        case IDM_SUBTITLE_LOAD:LoadSubtitleFromDialog();break;case IDM_SUBTITLE_NEXT:NextSubtitles();break;
        case IDM_SUBTITLE_EARLIER:StepSubtitleDelay(-1);break;case IDM_SUBTITLE_LATER:StepSubtitleDelay(+1);break;
        case IDM_SUBTITLE_DELAY_RESET:StepSubtitleDelay(0);break;
        case IDM_COMPARE_TOGGLE:ToggleSideBySide();break;
        case IDM_COMPARE_HDR_AT_SDR:ToggleCompareHdrAtSdr();break;
        case IDM_UPDATE_AVAILABLE:ActivateUpdateBadge();break;
        // IDM_COMPARE_BLEND has no menu row any more; anything that still sends it gets
        // the view Blend became, the neural frame at the Mix.
        case IDM_COMPARE_NEURAL:case IDM_COMPARE_BLEND:SetComparisonMode(ComparisonMode::Neural);break;case IDM_COMPARE_ORIGINAL:SetComparisonMode(ComparisonMode::Original);break;case IDM_COMPARE_SPLIT:SetComparisonMode(ComparisonMode::SplitVertical);break;case IDM_COMPARE_WIPE:SetComparisonMode(ComparisonMode::Wipe);break;
        case IDM_COMPARE_BLEND_LESS:AdjustMix(-0.1f);break;case IDM_COMPARE_BLEND_MORE:AdjustMix(0.1f);break;case IDM_COMPARE_ZOOM:ToggleZoom();break;
        case IDM_SAVE_COMPARISON_IMAGE:SaveComparisonImage();break;
        case IDM_COMPARE_MASK_LOAD:LoadMaskFromDialog();break;case IDM_COMPARE_MASK_INVERT:ToggleMaskInvert();break;case IDM_COMPARE_MASK_CLEAR:ClearMaskForSource();break;
        case IDM_COMPARE_SWAP:ToggleSwap();break;case IDM_COMPARE_DIFFERENCE:SetComparisonMode(ComparisonMode::Difference);break;
        case IDM_COMPARE_SIDE_BY_SIDE:SetComparisonMode(ComparisonMode::SideBySide);break;case IDM_COMPARE_QUAD:SetComparisonMode(ComparisonMode::Quad);break;
        case IDM_COMPARE_VSR:SetComparisonMode(ComparisonMode::Vsr);break;case IDM_COMPARE_AGAINST_VSR:ToggleAgainstVsr();break;
        case IDM_COMPARE_DIFFERENCE_LESS:StepDifferenceGain(-1);break;case IDM_COMPARE_DIFFERENCE_MORE:StepDifferenceGain(+1);break;case IDM_COMPARE_DIFFERENCE_LUMA:ToggleDifferenceLuma();break;case IDM_COMPARE_ZOOM_OUT:ZoomBy(-1,false,PointerOverPicture());break;case IDM_COMPARE_ZOOM_FIT:ZoomToFit();break;case IDM_COMPARE_LOUPE:ToggleLoupe();break;
        case IDM_ASPECT_ONE_TO_ONE:SetAspect(false,true);break;case IDM_COMPARE_NEXT_MODE:CycleComparisonMode(false);break;case IDM_COMPARE_PREVIOUS_MODE:CycleComparisonMode(true);break;
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
    bool m_frameGenHoldDuplicates=false;
    // Playback > Audio > Passthrough to receiver; off by default. What the
    // status line last showed about it, so a restart that changes the answer
    // (a seek onto a device that refuses, a display-change reopen) is shown.
    bool m_audioPassthrough=false;
    audio_passthrough::Status m_shownPassthrough;
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
    AppOptions m_opt;Localizer m_loc;UiResources m_uiResources;D3D12Renderer::ColorSettings m_colorSettings{};NVSDK_NGX_PerfQuality_Value m_activeQuality=DefaultNeuralCarrierQuality();HWND m_hwnd=nullptr,m_viewport=nullptr,m_renderWnd=nullptr,m_adjustWnd=nullptr;HFONT m_font=nullptr,m_fontSmall=nullptr,m_iconFont=nullptr,m_fontTitle=nullptr;
    bool m_running=true,m_loaded=false,m_playing=false,m_haveNext=false,m_waitingForNetworkFrame=false,m_fill=false,m_fullscreen=false,m_dragSeek=false,m_dragVolume=false,m_muted=false,m_seekPending=false,m_seekResumePlaying=false,m_seeking=false,m_trackingMouse=false,m_iconFallbackLogged=false,m_neuralRequested=true;
    // Timeline scrubbing: what playback was doing before the drag, and when the
    // last preview seek went out so a drag cannot queue one per mouse move.
    bool m_dragWasPlaying=false;
    // Set by a frame step, which stops audio; the next resume restarts it.
    bool m_audioStaleAfterStep=false;
    Clock::time_point m_lastScrubSeek{};
    ToolbarAction m_pressedToolbarAction=ToolbarAction::None,m_focusedToolbarAction=ToolbarAction::None,m_hoverAction=ToolbarAction::None;
    // Indexed by ToolbarAction; m_hoverAnimating has a bit per fade that was
    // moving on the last frame, so the frame it lands gets painted too.
    std::array<chrome_motion::Fade,static_cast<size_t>(ToolbarAction::None)+1> m_hoverFades{};uint32_t m_hoverAnimating=0;UINT_PTR m_hoverTimer=0;
    chrome_motion::Glow m_completeGlow;chrome_motion::CompletionLatch m_completionLatch;UINT_PTR m_glowTimer=0;
    chrome_motion::Toast m_toast;
    std::optional<WINDOWPLACEMENT> m_placementBeforeFullscreen;
    chrome_motion::BandGrowth m_bandGrowth;UINT_PTR m_bandTimer=0;std::wstring m_toastText;COLORREF m_toastMark=ui_palette::PrimaryBlue;UINT_PTR m_toastTimer=0;
    Clock::time_point m_liveSessionStartedAt{};
    // The sliders' hover (knob size) and the volume's value bubble, which
    // lingers slider::kBubbleLingerMs after a drag ends before it fades.
    // The compare bar's segment tints by part and index, and its sliding mark.
    std::map<int,chrome_motion::Fade> m_compareFades;chrome_motion::Slide m_compareMark;bool m_compareWasMoving=false;
    // The comparison tags' fade-in after a mode change; at rest it is on.
    chrome_motion::Fade m_tagFade=[]{chrome_motion::Fade fade;fade.Reset(true);return fade;}();bool m_tagsWereMoving=false;
    chrome_motion::Fade m_volumeHot,m_mixHot,m_volumeBubble;std::optional<Clock::time_point> m_volumeBubbleOffAt;bool m_slidersWereMoving=false;
    // Windows' own rule for focus cues: hidden until the keyboard is used to move
    // between controls, hidden again by the mouse. The focused action is always the
    // first enabled one, so without this the Open button wore a ring from launch on.
    bool m_keyboardCues=false;
    HMENU m_fullscreenMenu=nullptr;
    // HDR output (SyncHdrPresentation): the renderer the display state was last
    // applied to, whether the display must be asked again, what it said, and the
    // viewer's choice to compare an HDR original as the model saw it.
    uint64_t m_hdrRenderer=0;bool m_hdrRecheck=true;bool m_inSizeMove=false;
    D3D12Renderer::DisplayHdrState m_hdrDisplay{};bool m_compareHdrAtSdr=false;
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
    bool m_guidesSkipped=false,m_layoutMismatchLogged=false;int64_t m_lastRenderedTs=-1;uint64_t m_droppedFrames=0;uint32_t m_historyGeneration=0;
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
    // Latched when the undocumented menu-bar drawing does not check out.
    bool m_darkMenuFailed=false;
    // What Windows' media controls and the taskbar thumbnail were last told.
    MediaTransportControls m_mediaTransport;media_transport::Status m_smtcStatus{media_transport::Status::Closed};
    std::wstring m_smtcTitle;media_transport::TimelinePush m_smtcTimeline{};
    media_transport::ThumbBar m_thumbBar{};bool m_thumbBarCreated=false;std::map<UiIcon,HICON> m_thumbIcons;
    // The start screen: the facts its worker has gathered (shared with the
    // worker, so neither outlives the other's data), the recent list they were
    // gathered for, what the pointer is over, and the worker itself.
    std::shared_ptr<StartScreenAnswers> m_startAnswers=std::make_shared<StartScreenAnswers>();
    bool m_startRequested=false;std::vector<std::string> m_startRequestedKeys;
    StartHover m_startHover=StartHover::None;size_t m_startHoverIndex=0;
    std::map<int,chrome_motion::Fade> m_tileFades;bool m_tilesWereMoving=false;
    std::jthread m_startWorker;
    // The curated trailers' YouTube thumbnails, fetched once the start screen
    // is first shown (TrailerThumbnail.h).
    std::unique_ptr<TrailerThumbnails> m_trailerThumbnails=std::make_unique<TrailerThumbnails>();
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
    // The render's temporal choices (TemporalSettings.h); cache-key terms like the guides.
    TemporalSettings m_temporalSettings;
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
    // The resolution the model runs at, one of kProcessingScaleRungs. 100 on a
    // fresh install and never moved down by anything but the user.
    uint32_t m_processingScale=kDefaultProcessingScale;
    // Super Resolution's history for playback and for an export's SR stage on its
    // own (UpscalingPolicy.h). Per-frame on a fresh install.
    UpscalingHistory m_upscalingHistory=kRecommendedUpscalingHistory;
    // Ordered dither at the 8-bit capture store. On by default - it removed 45-71 % of
    // the Standard rung's banding for VMAF -0.03 at worst - and a cache-key term; the
    // window present dithers (blue noise) regardless, since that costs the cache nothing.
    bool m_captureDither=true;
    // The quality ladder's rung for the cache and the exports made through the helper
    // (EncoderQuality). Standard is every render before the ladder existed.
    EncoderQuality m_cacheQuality=EncoderQuality::Standard;
    // The deband pre-pass on the source ahead of the model. Off by default and a
    // cache-key term; it applies to renders, not to the player's own live frames.
    bool m_sourceDeband=false;
    // A smoothed, metered exposure for the DLSS evaluate instead of its AutoExposure.
    // A cache-key term; its default follows the exposure A/B.
    bool m_suppliedExposure=false;
    NeuralSettings m_neuralSettings;
    // Frame-accurate in/out markers on the loaded source's timeline.
    RangeMarkers m_markers;
    // Presentation comparison of a synchronized pair (persisted in [Comparison]). Its
    // zoomScale is resolved from m_zoomStep in EffectiveComparison, because a step is
    // a number of screen pixels per output pixel and so depends on the window's size.
    ComparisonSettings m_comparison;
    int m_zoomStep=0;
    // The synced loupe follows the pointer over the picture while this is on.
    bool m_loupe=false;
    // View > 1:1 pixels: the render window is the output's size.
    bool m_onePixel=false;
    // The spatial mask on the Mix: the image as loaded (grey, shrunk to at most
    // compare_mask::kMaxSide) and the feathered copy the renderer is given, the source
    // they belong to, and which revision the renderer holds.
    compare_mask::Gray m_maskSource,m_maskFeathered;
    std::filesystem::path m_maskPath;
    int m_maskFeather=compare_mask::kDefaultFeather;
    bool m_maskInvert=false;
    uint64_t m_maskRevision=0,m_maskUploadedRevision=0,m_maskRefusedRevision=0;
    const D3D12Renderer* m_maskRefusedBy=nullptr;
    std::wstring m_maskForPath,m_maskForPage,m_maskSourceKey;
    // Subtitles (P3.2): the worker that probes and draws them, what the loaded
    // source offers (its streams, the file beside it, where its clock starts),
    // a file loaded for it, the viewer's choice and delay, what the overlay is
    // asked to draw, and the picture the renderer holds.
    SubtitleOverlay m_subtitles;
    std::wstring m_subtitlesForPath;
    bool m_subtitleDiscovering=false;
    std::vector<subtitle::Track> m_subtitleTracks;
    std::wstring m_subtitleSidecar;
    double m_subtitleOrigin=0.0;
    std::wstring m_subtitleFile,m_subtitleFileCharenc;
    std::string m_subtitleFileCodec;
    std::wstring m_subtitleProbeFile;
    bool m_subtitleProbeRemember=false;
    subtitle::Choice m_subtitleChoice;
    int m_subtitleDelayMs=0;
    std::optional<SubtitleOverlay::Source> m_subtitleWanted;
    std::shared_ptr<const subtitle::Frame> m_subtitleFrame;
    const D3D12Renderer* m_subtitleRenderer=nullptr;
    double m_subtitleClock=-1.0;
    uint32_t m_subtitleResizeW=0,m_subtitleResizeH=0;
    ULONGLONG m_subtitleResizeAt=0;
    // A middle-button pan in progress, and where the last pan step left the pointer.
    bool m_middlePan=false,m_renderTracking=false;
    POINT m_panLast{};
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
    // Each settings dialog's controls where they were built, and the dpi they
    // were built at, so a resize or a dpi change works from a fixed baseline.
    struct SettingsDesignLayout{UINT dpi{USER_DEFAULT_SCREEN_DPI};std::vector<std::pair<HWND,RECT>> items;};
    std::map<HWND,SettingsDesignLayout> m_settingsDesignLayout;
    // The font each open dialog draws in, made at its dpi and freed with it.
    std::map<HWND,HFONT> m_dialogFonts;
    // The group headings' label face, per dialog, at that dialog's dpi.
    std::map<HWND,HFONT> m_dialogHeadingFonts;
    POINT m_renderMouse{};
    bool m_renderMouseKnown=false,m_dragSplit=false;
    // The press on the picture in progress, and whether it is holding the original up.
    compare_gesture::State m_gesture{};
    bool m_peekOriginal=false;
    // The compare bar: the part under the pointer, and a Mix drag in progress.
    CompareHover m_compareHover{};
    bool m_dragMix=false;
    // What the renderer's tag atlas was drawn for; see EnsureLabelAtlas. Whatever
    // changes a tag's text bumps m_labelTextRevision.
    UINT m_labelAtlasDpi=0;
    uint64_t m_labelAtlasRevision=0,m_labelTextRevision=1;
    // The RTX VSR reason the tags were last drawn for; see ApplyComparison. Starts at
    // NotBuilt so that only a view that was Ready and then was refused is logged.
    vsr_policy::Reason m_lastVsrReason=vsr_policy::Reason::NotBuilt;
    const D3D12Renderer* m_labelAtlasRefusedBy=nullptr;
    // Settings the playing cache entry was rendered with (its receipt has the full record).
    NeuralSettings m_cachedSettings;
    GuideControls m_cachedGuides;
    TemporalSettings m_cachedTemporal;
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
    // Deletes retired segment files off the UI thread; see SweepRetiredLiveSegments.
    BackgroundFileReaper m_segmentReaper;
    // The forecast this session started on, kept so the cushion has a pace to
    // size against before RealtimeRatio has enough samples to report one.
    double m_liveForecastRatio=0.0;
    uint32_t m_livePaceWidth=0,m_livePaceHeight=0;
    // The processing scale the session started at, which is the rung its
    // pace is recorded under.
    uint32_t m_livePaceScale=kDefaultProcessingScale;
    playback_timing::RenderPaceProfile m_renderPace;
    // The same for the reduced processing-scale rungs, in kProcessingScaleRungs
    // order after 100.
    render_pace::ReducedProfiles m_reducedRenderPace{};
    // Every pace this GPU measured per geometry and rung, newest last;
    // m_renderPace and m_reducedRenderPace carry the median of each and are
    // what the forecast reads.
    std::vector<render_pace::History> m_paceHistory;
    // The cold start of the newest neural job, or null when no job has run.
    std::shared_ptr<NeuralColdStartRecord> m_coldStart;
    HWND m_bufferWnd=nullptr;
    RECT m_bufferAnchor{};
};

// ---- --render: Export with DLSS stages, headless -----------------------
//
// This executable is GUI-subsystem, so it starts with no console. Output that
// was redirected (a pipe, a file) arrives as inherited handles and is written
// as UTF-8; otherwise the process attaches to the console it was started from.
// cmd.exe does not wait for a GUI-subsystem program, so an interactive caller
// wants `start /wait`, and a script reads the exit code the same way.
class RenderConsole {
public:
    RenderConsole(){
        out_=Private(GetStdHandle(STD_OUTPUT_HANDLE));err_=Private(GetStdHandle(STD_ERROR_HANDLE));
        // Attached even when both streams are redirected: Ctrl+C is delivered
        // to the processes attached to a console, and `> log.txt` must not
        // make a render uncancellable.
        if(AttachConsole(ATTACH_PARENT_PROCESS)&&(!out_||!err_)){
            console_=CreateFileW(L"CONOUT$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
            if(console_==INVALID_HANDLE_VALUE)console_=nullptr;
        }
        // Everything else in this process loses the caller's stream. NGX
        // prints its whole startup log to a standard output it can see - over
        // 900 lines for one frame-generation export, plus a bare result code -
        // and a script reading this output wants the lines below, not that.
        // The private duplicates above keep the caller's handles alive through
        // the CRT closing its own.
        HANDLE nul=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(nul!=INVALID_HANDLE_VALUE){SetStdHandle(STD_OUTPUT_HANDLE,nul);SetStdHandle(STD_ERROR_HANDLE,nul);nul_=nul;}
        FILE* reopened=nullptr;
        freopen_s(&reopened,"NUL","w",stdout);
        freopen_s(&reopened,"NUL","w",stderr);
    }
    ~RenderConsole(){for(HANDLE handle:{out_,err_,console_,nul_})if(handle)CloseHandle(handle);}
    RenderConsole(const RenderConsole&)=delete;
    RenderConsole& operator=(const RenderConsole&)=delete;
    void Out(std::wstring_view line){Write(out_?out_:console_,line);}
    void Err(std::wstring_view line){Write(err_?err_:console_,line);}
private:
    // A handle the caller redirected (a pipe or a file), duplicated so it is
    // this object's alone; null when there is none, which is the normal case
    // for a GUI-subsystem program started from a console.
    static HANDLE Private(HANDLE handle){
        if(!handle||handle==INVALID_HANDLE_VALUE||GetFileType(handle)==FILE_TYPE_UNKNOWN)return nullptr;
        HANDLE duplicate=nullptr;
        if(!DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS))return nullptr;
        return duplicate;
    }
    static void Write(HANDLE handle,std::wstring_view line){
        if(!handle)return;
        std::wstring text;text.reserve(line.size()+2);
        for(const wchar_t character:line){if(character==L'\n')text+=L'\r';text+=character;}
        text+=L"\r\n";
        DWORD mode=0,written=0;
        if(GetConsoleMode(handle,&mode)){WriteConsoleW(handle,text.data(),static_cast<DWORD>(text.size()),&written,nullptr);return;}
        const std::string bytes=WideToUtf8(text);
        WriteFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr);
    }
    HANDLE out_{},err_{},console_{},nul_{};
};

// Ctrl+C and Ctrl+Break request the same stop the dialog's Cancel does. The
// handler runs on a thread the system creates, and std::stop_source is safe
// to signal from one.
static std::stop_source& RenderStopSource(){static std::stop_source source;return source;}
static BOOL WINAPI RenderConsoleControl(DWORD event){
    if(event!=CTRL_C_EVENT&&event!=CTRL_BREAK_EVENT)return FALSE;
    RenderStopSource().request_stop();
    return TRUE;
}

// Where the dialog stages its passes: the configured cache root, or the
// automatic one. Only the scratch directory is read from it, so the legacy
// path migration the player runs on startup does not matter here - an old
// absolute default is still a directory the passes can write to.
static std::filesystem::path RenderScratchRoot(const std::filesystem::path& settings){
    std::wstring directory(32768,L'\0');
    const DWORD length=GetPrivateProfileStringW(L"Storage",L"CacheDirectory",L"",directory.data(),static_cast<DWORD>(directory.size()),settings.c_str());
    directory.resize(length);
    const std::filesystem::path root(directory);
    const UINT automatic=GetPrivateProfileIntW(L"Storage",L"CacheDirectoryAutomatic",-1,settings.c_str());
    return root.is_absolute()&&length<32767&&automatic!=1?root:std::filesystem::path{};
}

static int RunRenderCommand(const render_command::Parsed& parsed,const std::vector<std::wstring>& userArguments){
    using namespace render_command;
    RenderConsole console;
    const Localizer loc;
    if(parsed.mode==Mode::Help){console.Out(Usage());return kExitOk;}
    if(parsed.mode==Mode::BadArguments){
        console.Err(L"error: "+parsed.error);console.Err(L"Run with --help for the options.");
        LOG("--render refused its arguments: "<<WideToUtf8(parsed.error));
        return kExitBadArguments;
    }
    const Command& command=parsed.command;
    const bool quiet=command.quiet;
    const auto say=[&](const std::wstring& line){if(!quiet)console.Out(line);};
    const auto refuse=[&](const std::wstring& reason){console.Err(L"refused: "+reason);LOG("--render refused: "<<WideToUtf8(reason));return kExitRefused;};
    const auto failed=[&](const std::wstring& reason){console.Err(L"failed: "+reason);LOG("--render failed: "<<WideToUtf8(reason));return kExitFailed;};
    std::wstring joined;for(const auto& argument:userArguments){if(!joined.empty())joined+=L' ';joined+=argument;}
    LOG("--render starting: "<<WideToUtf8(joined));

    std::error_code fileError;
    const std::filesystem::path input=std::filesystem::absolute(command.input,fileError);
    if(fileError||!std::filesystem::is_regular_file(input,fileError)){
        console.Err(L"error: the input is not a file: "+command.input);return kExitBadArguments;
    }
    const bool defaultOutput=command.output.empty();
    // The default's extension follows the source (a photo exports to PNG), so
    // it is settled, and checked for an existing file, once the source is read.
    std::filesystem::path output=defaultOutput?DefaultOutput(input):std::filesystem::absolute(command.output,fileError);
    if(fileError){console.Err(L"error: the output path is not usable: "+command.output);return kExitBadArguments;}
    if(output.has_parent_path()&&!std::filesystem::is_directory(output.parent_path(),fileError))
        return refuse(L"the output folder does not exist: "+output.parent_path().wstring());
    if(command.safeMode&&command.selection.neural)
        return refuse(L"--safe-mode turns neural rendering off, and the nr stage needs it.");

    std::filesystem::path executable;std::wstring pathError;
    if(!CurrentExecutablePath(executable,pathError))return failed(pathError);
    const std::filesystem::path helpers=executable.parent_path();
    const std::filesystem::path settings=helpers/L"DLSSVideoPlayer.ini";

    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE)))return failed(L"COM could not be initialized.");
    if(FAILED(MFStartup(MF_VERSION,MFSTARTUP_FULL))){CoUninitialize();return failed(L"Media Foundation could not be started.");}
    return RunPlayerRuntime([&]() -> int {
        VideoDecoder decoder;
        if(!decoder.OpenMetadata(input.wstring()))return failed(L"the input could not be read as a video or an image.");
        const uint32_t width=decoder.Width(),height=decoder.Height();
        const double fps=decoder.FrameRate(),duration=decoder.DurationSeconds();
        const bool still=decoder.IsStillImage();
        const bool animation=decoder.IsAnimation();
        decoder.Close();
        // The container follows the extension, as it does in the dialog, and
        // only the containers the dialog would offer this source are written.
        if(defaultOutput)output.replace_extension(ExportContainerExtension(ExportContainerChoices(still,animation).front()));
        else if(const auto container=ExportContainerFor(output.extension().wstring());!container||!ExportContainerOffered(*container,still,animation)){
            console.Err(still?L"error: --out must name a .png or .jpg file for a photo."
                        :animation?L"error: --out must name a .gif, .mp4 or .mkv file for an animation."
                        :L"error: --out must name a .mkv or .mp4 file for a video.");
            return kExitBadArguments;
        }
        if(defaultOutput&&std::filesystem::exists(output,fileError))
            return refuse(output.wstring()+L" already exists. Name the file to write with --out, which replaces it.");

        // The dialog's own order: measured only when frame generation was asked
        // for, because the probe brings up a device of its own.
        uint32_t maxMultiplier=2;
        if(command.selection.frameGeneration){
            const FrameGenerationCapability capability=QueryFrameGenerationCapability();
            maxMultiplier=capability.available?1u+capability.multiFrameCountMax:0u;
        }
        const ExportPlan plan=PlanExport(command.selection,width,height,fps,maxMultiplier,still);
        if(!plan.valid)return refuse(loc.Get(ExportRefusalKey(plan.refusal)));

        NeuralRenderRange range{};
        if(command.hasRange){
            if(!plan.workerStage)return refuse(L"--range needs the sr or nr stage.");
            const auto in=ParseTimecode(command.rangeStart,fps);
            const auto out=ParseTimecode(command.rangeEnd,fps);
            if(!in||!out){console.Err(L"error: --range holds a timecode this source cannot read.");return kExitBadArguments;}
            const auto snapped=RangeFromMarkers({in,out},fps,static_cast<int64_t>(std::llround(duration*10000000.0)));
            if(!snapped){console.Err(L"error: --range does not name a part of this source.");return kExitBadArguments;}
            range=*snapped;
        }
        if(plan.workerStage&&!std::filesystem::is_regular_file(helpers/L"neural-runtime"/L"NeuralWorker.exe",fileError))
            return refuse(L"the neural runtime is not installed beside the player, and the sr and nr stages run in it.");

        NeuralSettings neuralSettings{};
        if(command.preset)neuralSettings=neural_presets::kPresets[*command.preset].settings;
        else LoadNeuralSettings(settings,neuralSettings);
        NeuralCacheManager cache(RenderScratchRoot(settings));
        if(!cache.Valid())return failed(L"no writable cache directory for the intermediate passes.");
        const std::filesystem::path scratch=cache.Root()/L"export-stages";
        std::filesystem::create_directories(scratch,fileError);
        if(fileError)return failed(L"the intermediate directory could not be created: "+scratch.wstring());

        StageExportJob job;
        job.plan=plan;job.source=input;job.destination=output;job.scratch=scratch;job.helpers=helpers;
        job.sourceWidth=width;job.sourceHeight=height;job.fps=fps;job.duration=duration;job.range=range;
        job.nvencPreset=std::clamp<uint32_t>(uint32_t(GetPrivateProfileIntW(L"Encoding",L"NvencPreset",5,settings.c_str())),1,7);
        job.neuralSettings=neuralSettings;
        job.processingScale=command.processingScale?*command.processingScale:ReadProcessingScale(settings);
        job.upscalingHistory=ReadUpscalingHistory(settings);
        job.captureDither=GetPrivateProfileIntW(L"Encoding",L"CaptureDither",1,settings.c_str())!=0;
        job.quality=ReadCacheQuality(settings);
        job.sourceDeband=GetPrivateProfileIntW(L"Encoding",L"SourceDeband",0,settings.c_str())!=0;
        job.suppliedExposure=GetPrivateProfileIntW(L"Encoding",L"SuppliedExposure",0,settings.c_str())!=0;

        wchar_t summary[256];
        swprintf_s(summary,L"%u x %u at %.4g fps -> %u x %u at %.4g fps, %u pass%s",width,height,fps,
                   plan.outputWidth,plan.outputHeight,plan.outputFps,ExportStageCount(plan),ExportStageCount(plan)==1?L"":L"es");
        say(summary);
        say(L"writing "+output.wstring());
        LOG("--render plan: upscale="<<command.selection.upscale<<" neural="<<command.selection.neural
            <<" framegen="<<command.selection.frameGeneration<<" output="<<plan.outputWidth<<"x"<<plan.outputHeight
            <<" fps="<<plan.outputFps<<" range=["<<range.start100ns<<","<<range.end100ns<<") preset="
            <<(command.preset?std::string(neural_presets::kPresets[*command.preset].key):std::string("saved")));

        SetConsoleCtrlHandler(nullptr,FALSE);
        SetConsoleCtrlHandler(RenderConsoleControl,TRUE);
        // A line when a pass starts, when it reaches its last frame, and at most
        // one a second in between: a two-hour film is 170,000 frames, and a
        // caller redirecting this to a file wants a trace, not a frame log.
        uint32_t lastPass=0;uint64_t lastDone=~uint64_t{0};auto lastLine=std::chrono::steady_clock::time_point{};
        const StageExportOutcome outcome=RunStageExport(job,RenderStopSource().get_token(),[&](const StageExportUpdate& update){
            if(quiet)return;
            if(!update.passKey){console.Out(loc.Get(L"export.progress.writing"));return;}
            const auto now=std::chrono::steady_clock::now();
            const bool finished=update.totalFrames&&update.completedFrames>=update.totalFrames;
            // The pass reports its last frame again while it encodes and
            // validates; one line says it got there.
            if(update.pass==lastPass&&(update.completedFrames==lastDone||(!finished&&now-lastLine<std::chrono::seconds(1))))return;
            lastPass=update.pass;lastDone=update.completedFrames;lastLine=now;
            console.Out(ProgressLine(update.pass,update.passes,loc.Get(update.passKey),update.completedFrames,update.totalFrames));
        });
        SetConsoleCtrlHandler(RenderConsoleControl,FALSE);
        if(outcome.status==StageExportStatus::Cancelled||RenderStopSource().stop_requested()){
            console.Err(L"cancelled");LOG("--render cancelled.");return kExitCancelled;
        }
        if(outcome.status==StageExportStatus::Refused)return refuse(outcome.detail);
        if(outcome.status!=StageExportStatus::Done)return failed(outcome.detail.empty()?std::wstring(L"the export did not finish."):outcome.detail);
        console.Out(L"done: "+output.wstring());
        LOG("--render finished: "<<WideToUtf8(output.wstring()));
        return kExitOk;
    },[]{MFShutdown();},[]{CoUninitialize();});
}

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int)
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    // Before anything that can fault, so an early crash still leaves a dump
    // beside the log rather than nothing at all.
    crash_dump::Install();
    EnablePerMonitorDpiAwareness();
    EnableDarkPopupMenus();
    AppOptions options=ParseArgs();
    // --render and --help run headless and never reach the player, its
    // bootstrap or a window. ParseRuntimeArguments has already run, so the
    // safe-mode flag means the same thing to both.
    if(options.argumentsOk){
        const auto render=render_command::Parse(options.userArguments);
        if(render.mode!=render_command::Mode::Player)return RunRenderCommand(render,options.userArguments);
    }
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
