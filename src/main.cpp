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
#include "resources.h"

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
using namespace app_menu;
static constexpr int CONTROL_H_DIP = 112;

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

static int MinimumPlayerWindowTrackWidth(HWND window, UINT dpi)
{
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    const BOOL hasMenu = GetMenu(window) != nullptr;
    const int minimumClientWidth = MinimumToolbarClientWidth(dpi);
    const RECT client{0, 0, minimumClientWidth, 1};
    RECT outer = client;

    using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const auto adjustForDpi = reinterpret_cast<AdjustWindowRectExForDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
    BOOL adjusted = adjustForDpi && adjustForDpi(&outer, style, hasMenu, exStyle, dpi);
    if (!adjusted) {
        outer = client;
        adjusted = AdjustWindowRectEx(&outer, style, hasMenu, exStyle);
    }
    if (!adjusted) return minimumClientWidth;
    return static_cast<int>(outer.right - outer.left);
}

static const wchar_t* kVideoPatterns =
    L"*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi;*.wmv;*.asf;*.flv;*.f4v;"
    L"*.ts;*.m2ts;*.mts;*.mpg;*.mpeg;*.mpe;*.vob;*.ogv;*.ogg;*.3gp;*.3g2;"
    L"*.mxf;*.nut;*.rm;*.rmvb;*.divx;*.dv;*.y4m;*.ivf;*.hevc;*.h265;*.h264;*.264;*.av1;*.vp9";

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

struct AppOptions {
    uint32_t maxW=3840, maxH=2160;
    NVSDK_NGX_PerfQuality_Value quality=NVSDK_NGX_PerfQuality_Value_MaxQuality;
    bool qualityExplicit=false;
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
            if(x!=std::wstring::npos) { o.maxW=std::max(64,_wtoi(v.substr(0,x).c_str())); o.maxH=std::max(64,_wtoi(v.substr(x+1).c_str())); }
        } else if(a==L"--quality" && i+1<o.userArguments.size()) {
            std::wstring q=o.userArguments[++i]; std::transform(q.begin(),q.end(),q.begin(),::towlower);
            if(q==L"auto") { o.qualityExplicit=false; }
            else {
                o.qualityExplicit=true;
                if(q==L"performance"||q==L"perf") o.quality=NVSDK_NGX_PerfQuality_Value_MaxPerf;
                else if(q==L"balanced") o.quality=NVSDK_NGX_PerfQuality_Value_Balanced;
                else if(q==L"ultra-performance"||q==L"ultraperf") o.quality=NVSDK_NGX_PerfQuality_Value_UltraPerformance;
                else if(q==L"dlaa") o.quality=NVSDK_NGX_PerfQuality_Value_DLAA;
                else o.quality=NVSDK_NGX_PerfQuality_Value_MaxQuality;
            }
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
    options.neuralAddonRequested=NeuralAddonDesired(options.detectedGpu.generation,options.safeMode);
    const ConfigUpdate update=ConfigureNeuralAddon(executable.parent_path()/L"ReShade.ini",options.neuralAddonRequested);
    const BootstrapAction action=DecideBootstrapFromObservedUpdate(
        options.neuralAddonRequested,
        update.previousAddonEnabled,
        update.addonEnabled,
        options.addonBootstrapRestarted,
        update.ok);
    if(!update.ok) return FailBootstrap(update.error);
    if(update.addonEnabled!=options.neuralAddonRequested) return FailBootstrap(L"ReShade.ini still reports the wrong neural add-on state after the update attempt");
    options.neuralAddonConfigured=update.addonEnabled;

    if(action==BootstrapAction::Fail) return FailBootstrap(L"The neural add-on required a second correction after the bootstrap restart marker");
    if(action==BootstrapAction::Relaunch) {
        const std::vector<std::wstring> arguments=BuildBootstrapRelaunchArguments(options.userArguments);
        std::wstring launchError;
        if(!LaunchSameExecutable(arguments,launchError)) return FailBootstrap(launchError);
        LOG("Neural addon configuration changed; relaunched before renderer creation.");
        return StartupResult::ExitSuccess;
    }
    LOG("Neural addon configuration verified before renderer creation: desired=" << (options.neuralAddonRequested?"enabled":"disabled") << " gpu=" << WideToUtf8(options.detectedGpu.description));
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

class PlayerApp {
public:
    explicit PlayerApp(AppOptions o):m_opt(std::move(o)){}
    ~PlayerApp(){SaveVideoSettings();if(m_adjustWnd)DestroyWindow(m_adjustWnd);UnregisterOverlayHotkeys();Unload(); if(m_font)DeleteObject(m_font); if(m_fontSmall)DeleteObject(m_fontSmall); if(m_iconFont)DeleteObject(m_iconFont);}

    bool Create(HINSTANCE hi) {
        m_loc.Initialize();
        if(!m_uiResources.Load(hi))LOG("Embedded Tabler icon font unavailable; continuing with label-only controls.");
        LoadVideoSettings();
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
        m_hwnd=CreateWindowExW(WS_EX_ACCEPTFILES,w.lpszClassName,appTitle.c_str(),WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,rc.right-rc.left,rc.bottom-rc.top,nullptr,app_menu::CreateMenuBar(m_loc),hi,this);
        if(!m_hwnd) return false;
        RegisterOverlayHotkeys();
        BOOL dark=TRUE; DwmSetWindowAttribute(m_hwnd,20,&dark,sizeof(dark)); DWORD corner=2; DwmSetWindowAttribute(m_hwnd,33,&corner,sizeof(corner));
        m_viewport=CreateWindowExW(0,v.lpszClassName,nullptr,WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,100,100,m_hwnd,nullptr,hi,nullptr);
        m_renderWnd=CreateWindowExW(WS_EX_ACCEPTFILES,L"DLSSVideoRenderClassV11",nullptr,WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,0,0,100,100,m_viewport,nullptr,hi,this);
        UpdateFontsForDpi(ActiveWindowDpi(m_hwnd));
        DragAcceptFiles(m_hwnd,TRUE); DragAcceptFiles(m_renderWnd,TRUE); ShowWindow(m_viewport,SW_HIDE); Layout(); UpdateTitle();
        if(!m_opt.file.empty()) Load(m_opt.file); // No startup file picker: the player opens idle by default.
        return true;
    }

    void Tick() {
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
        if(!m_loaded||!m_playing||!m_haveNext||m_seeking) return;
        double now=Position(); const double frameDur=1.0/std::max(1.0,m_decoder.FrameRate());
        bool dropped=false;
        while(m_haveNext) {
            double due=double(m_next.timestamp100ns)*1e-7;
            if(now-due <= std::max(0.085,frameDur*2.25)) break;
            VideoFrame skip=std::move(m_next); (void)skip; ++m_droppedFrames; dropped=true;
            if(!m_decoder.ReadNext(m_next)){m_haveNext=false;break;}
        }
        if(dropped){m_guides.Reset();m_guideReset=true;m_dlssReset=true;}
        if(!m_haveNext){m_playing=false;m_audio.Pause(true);InvalidateControls();InvalidatePlaybackProgress();UpdateCachedStatus();return;}
        double due=double(m_next.timestamp100ns)*1e-7;
        if(now+0.001<due) return;
        if(RenderVideoFrame(m_next,m_next.discontinuity||m_guideReset)) {
            ++m_fpsWindowFrames;
            const auto fpsNow=Clock::now();
            const double fpsElapsed=std::chrono::duration<double>(fpsNow-m_fpsWindowStart).count();
            if(fpsElapsed>=0.75){m_submitFps=double(m_fpsWindowFrames)/fpsElapsed;m_fpsWindowFrames=0;m_fpsWindowStart=fpsNow;}
        }
        m_currentSec=due; m_guideReset=false; m_dlssReset=false;
        if(!m_decoder.ReadNext(m_next)){m_haveNext=false;m_playing=false;m_audio.Pause(true);}
        InvalidatePlaybackProgress();
        UpdateCachedStatus();
    }

    bool Running()const{return m_running;}
    bool NeedsRealtimeTick()const{return m_loaded;}
    DWORD TickSleepMs()const{return (m_loaded&&!m_playing&&!m_seekPending&&!m_seeking)?8u:0u;}


private:
    std::wstring T(const wchar_t* key)const{return m_loc.Get(key);}

    int Dip(int value)const{return MulDiv(value,static_cast<int>(ActiveWindowDpi(m_hwnd)),USER_DEFAULT_SCREEN_DPI);}
    int ControlHeight()const{return Dip(CONTROL_H_DIP);}
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

    void LoadVideoSettings(){
        m_colorSettings.brightness=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Brightness",0.0f),-2.0f,2.0f);
        m_colorSettings.contrast=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Contrast",1.0f),0.0f,3.0f);
        m_colorSettings.saturation=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Saturation",1.0f),0.0f,3.0f);
        m_colorSettings.gamma=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Gamma",1.0f),0.25f,3.0f);
        m_colorSettings.temperature=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Temperature",0.0f),-1.0f,1.0f);
        m_colorSettings.tint=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Tint",0.0f),-1.0f,1.0f);
    }

    void SaveVideoSettings()const{
        WriteIniFloat(L"VideoAdjustments",L"Brightness",m_colorSettings.brightness);
        WriteIniFloat(L"VideoAdjustments",L"Contrast",m_colorSettings.contrast);
        WriteIniFloat(L"VideoAdjustments",L"Saturation",m_colorSettings.saturation);
        WriteIniFloat(L"VideoAdjustments",L"Gamma",m_colorSettings.gamma);
        WriteIniFloat(L"VideoAdjustments",L"Temperature",m_colorSettings.temperature);
        WriteIniFloat(L"VideoAdjustments",L"Tint",m_colorSettings.tint);
    }

    void ApplyVideoAdjustments(bool refreshPaused=true){
        if(m_renderer){
            m_renderer->SetColorSettings(m_colorSettings);
            if(refreshPaused&&!m_playing&&!m_seeking)m_renderer->PresentCurrent();
        }
    }

    void InvalidateControls(){
        if(!m_hwnd)return;RECT c{};GetClientRect(m_hwnd,&c);
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

    void CreateAdjustmentRow(HWND h,int id,const wchar_t* labelKey,int y){
        HFONT f=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND label=CreateWindowExW(0,L"STATIC",T(labelKey).c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,16,y,116,20,h,nullptr,nullptr,nullptr);
        HWND track=CreateWindowExW(0,TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_NOTICKS,132,y-6,236,30,h,(HMENU)(INT_PTR)id,nullptr,nullptr);
        HWND value=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_RIGHT,370,y,64,20,h,(HMENU)(INT_PTR)(id+100),nullptr,nullptr);
        SendMessageW(label,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(track,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(value,WM_SETFONT,(WPARAM)f,TRUE);
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

    static LRESULT CALLBACK WndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        PlayerApp* a=nullptr;
        if(m==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
        else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        return a?a->WndProc(h,m,w,l):DefWindowProcW(h,m,w,l);
    }
    static LRESULT CALLBACK ViewportWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
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
            if(m==WM_LBUTTONDOWN){SetFocus(a->m_hwnd);return 0;}
            if(m==WM_LBUTTONDBLCLK){a->ToggleFullscreen();return 0;}
            if(m==WM_MOUSEWHEEL||m==WM_KEYDOWN||m==WM_SYSKEYDOWN)return SendMessageW(a->m_hwnd,m,w,l);
            if(m==WM_DROPFILES)return SendMessageW(a->m_hwnd,m,w,l); // main window owns DragFinish().
        }
        return DefWindowProcW(h,m,w,l);
    }

    bool Load(const std::wstring& path) {
        if(path.empty())return false;
        Unload();
        if(!m_decoder.Open(path)){std::wstring e=T(L"error.decode"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);return false;}
        m_dar=m_decoder.DisplayAspectRatio(); if(!std::isfinite(m_dar)||m_dar<0.2)m_dar=double(m_decoder.Width())/std::max(1u,m_decoder.Height());
        auto [ow,oh]=OutputForAspect(m_dar,m_opt.maxW,m_opt.maxH);
        m_activeQuality = m_opt.qualityExplicit ? m_opt.quality : AutoQuality(m_decoder.NativeWidth(),m_decoder.NativeHeight(),ow,oh,m_decoder.FrameRate());
        LOG("DLSS quality policy: " << (m_opt.qualityExplicit?"explicit":"auto-realtime") << " -> " << QualityNameA(m_activeQuality));
        const auto [decodeW,decodeH]=RecommendedDecodeSize(m_decoder.NativeWidth(),m_decoder.NativeHeight(),ow,oh,m_activeQuality);
        if((decodeW!=m_decoder.Width()||decodeH!=m_decoder.Height()) && !m_decoder.SetDecodeSize(decodeW,decodeH))
            LOG("Realtime decode scaling unavailable; continuing at native decoder resolution.");
        const auto [guideW,guideH]=TemporalGuideGenerator::AnalysisGrid(m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate());
        ShowWindow(m_viewport,SW_SHOW); Layout();
        m_renderer=std::make_unique<D3D12Renderer>();
        if(!m_renderer->Initialize(m_renderWnd,m_decoder.Width(),m_decoder.Height(),ow,oh,guideW,guideH,m_activeQuality)){std::wstring e=T(L"error.renderer"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);m_renderer.reset();m_decoder.Close();ShowWindow(m_viewport,SW_HIDE);return false;}
        m_renderer->SetColorSettings(m_colorSettings);
        VideoFrame first; if(!m_decoder.ReadNext(first)){std::wstring e=T(L"error.frame"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);Unload();return false;}
        m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;RenderVideoFrame(first,true);m_currentSec=double(first.timestamp100ns)*1e-7;
        m_haveNext=m_decoder.ReadNext(m_next);m_audio.Start(path,m_currentSec);m_audio.SetVolume(m_muted?0.0f:m_volume);m_playing=true;m_playStartSec=m_currentSec;m_playStart=Clock::now();m_loaded=true;m_path=path;m_droppedFrames=0;m_seekPending=false;m_seeking=false;m_fpsWindowStart=Clock::now();m_fpsWindowFrames=0;m_submitFps=0.0;
        UpdateTitle();UpdateCachedStatus();Layout();InvalidateRect(m_hwnd,nullptr,TRUE);return true;
    }

    void Unload() {
        m_seekPending=false;m_seeking=false;m_audio.Stop(); if(m_renderer){m_renderer->WaitGPU();m_renderer.reset();} m_decoder.Close();m_guides.Reset();m_haveNext=false;m_next=VideoFrame{};m_loaded=false;m_playing=false;m_currentSec=0;m_lastRenderedTs=-1;m_path.clear();m_cachedStatus.clear();
        if(m_viewport)ShowWindow(m_viewport,SW_HIDE);RefreshHoverForCurrentLayout();UpdateTitle(); if(m_hwnd)InvalidateRect(m_hwnd,nullptr,TRUE);
    }

    bool RenderVideoFrame(const VideoFrame& f,bool resetGuide) {
        if(!m_renderer)return false; GuideFrame g;
        if(!m_guides.Generate(f.bgra.data(),m_decoder.Width(),m_decoder.Height(),m_renderer->DLSSInputW(),m_renderer->DLSSInputH(),m_decoder.FrameRate(),resetGuide,g))return false;
        float ms=float(1000.0/std::max(1.0,m_decoder.FrameRate()));
        if(m_lastRenderedTs>=0 && f.timestamp100ns>m_lastRenderedTs){double d=double(f.timestamp100ns-m_lastRenderedTs)*1e-4;if(d>0.1&&d<500.0)ms=float(d);}
        bool r=m_dlssReset||resetGuide||!g.hasHistory;
        bool ok=m_renderer->RenderFrame(f.bgra.data(),f.bgra.size(),g.guideGridRGBA32F.data(),g.guideGridRGBA32F.size()*sizeof(float),g.gridW,g.gridH,r,ms);
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
        double audio=m_audio.PositionSeconds();
        if(audio>=0.0){double d=m_decoder.DurationSeconds();return d>0?std::clamp(audio,0.0,d):audio;}
        double s=m_playStartSec+std::chrono::duration<double>(Clock::now()-m_playStart).count();double d=m_decoder.DurationSeconds();return d>0?std::clamp(s,0.0,d):std::max(0.0,s);
    }

    double ClampSeek(double sec)const{double dur=m_decoder.DurationSeconds();if(dur>0)return std::clamp(sec,0.0,dur);return std::max(0.0,sec);}

    void RequestSeek(double sec) {
        const bool resume=m_seekPending?m_seekResumePlaying:m_playing; RequestSeek(sec,resume);
    }

    void RequestSeek(double sec,bool resumeAfter) {
        if(!m_loaded)return; sec=ClampSeek(sec);
        if(!m_seekPending) m_currentSec=Position();
        m_pendingSeekSec=sec;m_seekResumePlaying=resumeAfter;m_seekPending=true;m_playing=false;m_audio.Pause(true);m_seekPreview=sec;InvalidateControls();InvalidatePlaybackProgress();UpdateCachedStatus();
    }

    bool PerformSeek(double sec,bool resumeAfter) {
        if(!m_loaded||m_seeking)return false;m_seeking=true;sec=ClampSeek(sec);LOG("Seek begin target="<<sec<<" resume="<<resumeAfter);
        // Seek is deliberately transactional and performed from Tick(), never from a mouse message.
        // Shut down the audio producer first, wait for GPU work, then restart the video decoder.
        m_audio.Stop(); if(m_renderer)m_renderer->WaitGPU(); m_haveNext=false;m_next=VideoFrame{};
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
            m_decoder.Close(); if(m_decoder.Open(m_path))got=readAt(sec,f);
        }
        if(!got){
            LOG("Seek failed without crashing; playback remains paused.");m_playing=false;m_seeking=false;m_currentSec=sec;UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();return false;
        }
        m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;
        if(!RenderVideoFrame(f,true)){LOG("Seek frame render failed.");m_playing=false;m_seeking=false;return false;}
        m_currentSec=double(f.timestamp100ns)*1e-7;m_haveNext=m_decoder.ReadNext(m_next);
        const bool audioOk=m_audio.Start(m_path,m_currentSec);if(audioOk){m_audio.SetVolume(m_muted?0.0f:m_volume);m_audio.Pause(!resumeAfter);}else LOG("Seek: no audio stream/output; using steady-clock video pacing.");
        m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=resumeAfter&&m_haveNext;m_guideReset=false;m_dlssReset=false;m_seeking=false;UpdateCachedStatus();InvalidateControls();InvalidatePlaybackProgress();LOG("Seek complete actual="<<m_currentSec);return true;
    }

    void SetPaused(bool pause){if(!m_loaded||m_seeking)return;if(pause==!m_playing)return;if(pause){m_currentSec=Position();m_playing=false;m_audio.Pause(true);}else{if(!m_haveNext&&m_decoder.DurationSeconds()>0){RequestSeek(0,true);return;}m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=true;m_audio.Pause(false);}InvalidateControls();InvalidatePlaybackProgress();}
    void TogglePause(){SetPaused(m_playing);}
    void StopPlayback(){RequestSeek(0,false);}

    void UpdateTitle(){
        if(!m_hwnd)return;
        const std::wstring appTitle=T(L"app.title");
        std::wstring mediaTitle;
        if(m_loaded&&!m_path.empty()){
            mediaTitle=std::filesystem::path(m_path).stem().wstring();
            if(mediaTitle.empty())mediaTitle=std::filesystem::path(m_path).filename().wstring();
        }
        const std::wstring title=BuildPlayerWindowTitle(appTitle,mediaTitle,120);
        if(title==m_cachedWindowTitle)return;
        m_cachedWindowTitle=title;SetWindowTextW(m_hwnd,title.c_str());
    }

    void Layout(){
        if(!m_hwnd||!m_viewport||!m_renderWnd)return;RECT c{};GetClientRect(m_hwnd,&c);int W=static_cast<int>(std::max<LONG>(1,c.right-c.left)),H=static_cast<int>(std::max<LONG>(1,c.bottom-c.top));
        if(!m_loaded){MoveWindow(m_viewport,0,0,W,H,TRUE);RefreshHoverForCurrentLayout();InvalidateControls();return;}
        int areaH=std::max(1,H-ControlHeight());MoveWindow(m_viewport,0,0,W,areaH,TRUE);double ar=m_dar>0?m_dar:16.0/9.0;double areaAr=double(W)/areaH;int rw=0,rh=0;
        if(m_fill){if(areaAr>ar){rw=W;rh=int(std::lround(W/ar));}else{rh=areaH;rw=int(std::lround(areaH*ar));}}else{if(areaAr>ar){rh=areaH;rw=int(std::lround(areaH*ar));}else{rw=W;rh=int(std::lround(W/ar));}}
        SetWindowPos(m_renderWnd,nullptr,(W-rw)/2,(areaH-rh)/2,std::max(1,rw),std::max(1,rh),SWP_NOZORDER|SWP_NOACTIVATE);
        RefreshHoverForCurrentLayout();InvalidateRect(m_viewport,nullptr,FALSE);InvalidateControls();
    }

    RECT TimelineRect()const{RECT c{};GetClientRect(m_hwnd,&c);return RECT{Dip(18),c.bottom-Dip(24),c.right-Dip(18),c.bottom-Dip(14)};}
    IdleSurfaceLayout IdleLayout()const{RECT c{};GetClientRect(m_hwnd,&c);return LayoutIdleSurface(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd));}
    std::vector<ToolbarItem> ToolbarItems()const{RECT c{};GetClientRect(m_hwnd,&c);return LayoutToolbar(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd));}
    std::vector<ToolbarItem> FocusableItems()const{if(m_loaded)return ToolbarItems();const auto idle=IdleLayout();return{idle.actions.begin(),idle.actions.end()};}
    ToolbarAvailability ToolbarState()const{return{m_loaded,m_seeking,m_renderer!=nullptr,false};}
    std::optional<RECT> VolumeRect()const{RECT c{};GetClientRect(m_hwnd,&c);const auto items=ToolbarItems();return LayoutVolumeSlider(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd),items);}
    bool PtIn(const RECT&r,int x,int y)const{return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}

    RECT TimeTextRect()const{RECT c{};GetClientRect(m_hwnd,&c);return RECT{Dip(16),c.bottom-Dip(55),Dip(142),c.bottom-Dip(32)};}
    RECT StatusRect()const{RECT c{};GetClientRect(m_hwnd,&c);return RECT{Dip(143),c.bottom-Dip(55),VolumeRect()?c.right-Dip(203):c.right-Dip(16),c.bottom-Dip(32)};}
    void InvalidatePlaybackProgress(){
        if(!m_hwnd||!m_loaded)return;
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
        m_cachedStatus=status;if(m_hwnd&&m_loaded){const RECT dirty=StatusRect();InvalidateRect(m_hwnd,&dirty,FALSE);}
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

    struct ToolbarButtonContent{UiIcon icon;std::wstring label;bool enabled;bool active;};

    ToolbarButtonContent ButtonContent(ToolbarAction action)const{
        const bool rendererReady=m_renderer!=nullptr;
        const bool enabled=IsToolbarActionEnabled(action,ToolbarState());
        switch(action){
        case ToolbarAction::Open:return{UiIcon::Open,L"Open",enabled,false};
        case ToolbarAction::OpenYouTube:return{UiIcon::YouTube,T(L"idle.youtube"),enabled,false};
        case ToolbarAction::Back10:return{UiIcon::Rewind,L"10s",enabled,false};
        case ToolbarAction::PlayPause:return{m_playing?UiIcon::Pause:UiIcon::Play,m_playing?L"Pause":L"Play",enabled,m_playing};
        case ToolbarAction::Stop:return{UiIcon::Stop,L"Stop",enabled,false};
        case ToolbarAction::Forward10:return{UiIcon::FastForward,L"10s",enabled,false};
        case ToolbarAction::Mute:return{m_muted?UiIcon::VolumeOff:UiIcon::Volume,m_muted?L"Sound":L"Mute",enabled,m_muted};
        case ToolbarAction::ToggleDlss:{const bool active=rendererReady&&m_renderer->DLSSEnabled();return{UiIcon::Sparkles,active?L"DLSS on":L"DLSS off",enabled,active};}
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
        const bool stacked=compact&&showIcon;
        if(showIcon){
            const HGDIOBJ oldFont=SelectObject(dc,m_iconFont);
            RECT iconRect=r;
            if(stacked){iconRect.bottom=iconRect.top+(iconRect.bottom-iconRect.top)*3/5;iconRect.top+=Dip(1);}
            else{iconRect.right=iconRect.left+Dip(18);iconRect.left+=Dip(1);}
            DrawTextW(dc,&glyph,1,&iconRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            SelectObject(dc,oldFont);
        }
        const HGDIOBJ oldFont=SelectObject(dc,m_fontSmall?m_fontSmall:m_font);
        RECT textRect=r;
        if(stacked){textRect.top=textRect.top+(textRect.bottom-textRect.top)/2;textRect.left+=Dip(2);textRect.right-=Dip(2);}
        else if(showIcon){textRect.left+=Dip(18);textRect.right-=Dip(1);}
        else{InflateRect(&textRect,-Dip(3),0);}
        DrawTextW(dc,label.c_str(),-1,&textRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        SelectObject(dc,oldFont);
        if(visual.drawFocus&&action!=ToolbarAction::None){RECT focusRect=r;InflateRect(&focusRect,-Dip(3),-Dip(3));DrawFocusRect(dc,&focusRect);}
    }

    void RenderUi(HDC dc,const RECT& c){
        HBRUSH windowBg=CreateSolidBrush(ui_palette::Window);FillRect(dc,&c,windowBg);DeleteObject(windowBg);
        if(!m_loaded){
            const IdleSurfaceLayout idle=IdleLayout();
            SetBkMode(dc,TRANSPARENT);
            SetTextColor(dc,RGB(242,243,245));auto of=SelectObject(dc,m_font);std::wstring tt=T(L"idle.title");RECT title=idle.title;DrawTextW(dc,tt.c_str(),-1,&title,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            SetTextColor(dc,ui_palette::SecondaryText);SelectObject(dc,m_fontSmall);std::wstring ss=T(L"idle.subtitle");RECT subtitle=idle.subtitle;DrawTextW(dc,ss.c_str(),-1,&subtitle,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);SelectObject(dc,of);
            for(const auto& item:idle.actions){const auto content=ButtonContent(item.action);DrawButton(dc,item.action,content.icon,content.label,item.bounds,content.enabled,false,content.enabled&&m_hoverAction==item.action,m_pressedToolbarAction==item.action,GetFocus()==m_hwnd&&m_focusedToolbarAction==item.action,false);}
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,ui_palette::SecondaryText);of=SelectObject(dc,m_fontSmall);std::wstring reason=T(L"idle.youtube_unavailable");RECT reasonRect=idle.youtubeReason;DrawTextW(dc,reason.c_str(),-1,&reasonRect,DT_CENTER|DT_TOP|DT_WORDBREAK|DT_END_ELLIPSIS|DT_NOPREFIX);SelectObject(dc,of);return;
        }
        RECT bar{0,c.bottom-ControlHeight(),c.right,c.bottom};HBRUSH bg=CreateSolidBrush(ui_palette::ControlSurface);FillRect(dc,&bar,bg);DeleteObject(bg);HPEN line=CreatePen(PS_SOLID,1,RGB(54,56,61));auto op=SelectObject(dc,line);MoveToEx(dc,0,bar.top,nullptr);LineTo(dc,c.right,bar.top);SelectObject(dc,op);DeleteObject(line);
        const auto toolbarItems=ToolbarItems();
        for(const auto& item:toolbarItems){const auto content=ButtonContent(item.action);const bool hover=content.enabled&&m_hoverAction==item.action;DrawButton(dc,item.action,content.icon,content.label,item.bounds,content.enabled,content.active,hover,m_pressedToolbarAction==item.action,GetFocus()==m_hwnd&&m_focusedToolbarAction==item.action,item.compact);}
        const auto volumeRect=LayoutVolumeSlider(static_cast<int>(c.right-c.left),static_cast<int>(c.bottom-c.top),ActiveWindowDpi(m_hwnd),toolbarItems);if(volumeRect){const RECT& vr=*volumeRect;HPEN vp=CreatePen(PS_SOLID,std::max(1,Dip(4)),RGB(94,98,105));op=SelectObject(dc,vp);MoveToEx(dc,vr.left,(vr.top+vr.bottom)/2,nullptr);LineTo(dc,vr.right,(vr.top+vr.bottom)/2);SelectObject(dc,op);DeleteObject(vp);int vx=vr.left+int((vr.right-vr.left)*(m_muted?0.0f:m_volume));HBRUSH vb=CreateSolidBrush(RGB(230,232,235));const int knob=std::max(3,Dip(5));Ellipse(dc,vx-knob,(vr.top+vr.bottom)/2-knob,vx+knob,(vr.top+vr.bottom)/2+knob);DeleteObject(vb);}
        double shown=m_dragSeek?m_seekPreview:(m_seekPending?m_pendingSeekSec:Position());RECT tr=TimelineRect();HBRUSH tb=CreateSolidBrush(RGB(68,71,77));FillRect(dc,&tr,tb);DeleteObject(tb);double d=m_decoder.DurationSeconds(),f=d>0?std::clamp(shown/d,0.0,1.0):0;RECT done=tr;done.right=done.left+int((done.right-done.left)*f);HBRUSH db=CreateSolidBrush(RGB(55,139,226));FillRect(dc,&done,db);DeleteObject(db);int kx=done.right;HBRUSH kb=CreateSolidBrush(RGB(246,246,248));Ellipse(dc,kx-5,tr.top-3,kx+5,tr.bottom+3);DeleteObject(kb);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(206,208,212));auto of=SelectObject(dc,m_fontSmall);std::wstring time=TimeText(shown)+L" / "+TimeText(d);TextOutW(dc,Dip(18),c.bottom-Dip(50),time.c_str(),int(time.size()));
        RECT sr{Dip(145),c.bottom-Dip(53),volumeRect?c.right-Dip(205):c.right-Dip(18),c.bottom-Dip(34)};DrawTextW(dc,m_cachedStatus.c_str(),-1,&sr,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);if(volumeRect){const RECT& vr=*volumeRect;std::wstring vol=m_muted?T(L"status.muted"):(T(L"status.volume")+L" "+std::to_wstring(int(m_volume*100))+L"%");TextOutW(dc,vr.right+Dip(8),vr.top-Dip(6),vol.c_str(),int(vol.size()));}SelectObject(dc,of);
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
        switch(id){case HK_PLAY_PAUSE:case HK_MEDIA_PLAY_PAUSE:TogglePause();break;case HK_BACK_10:RequestSeek(Position()-10);break;case HK_FORWARD_10:RequestSeek(Position()+10);break;case HK_MUTE:ToggleMute();break;case HK_DLSS:ToggleDLSS();break;case HK_ADJUSTMENTS:ShowAdjustments();break;}
    }

    void OpenFromDialog(){auto p=PickVideoFile(m_hwnd,m_loc);if(!p.empty())Load(p);}
    PlayerRuntimeMode RuntimeMode()const{
        if(!m_renderer||!m_renderer->DLSSEnabled()||!m_renderer->DLSSFeatureCreated())return PlayerRuntimeMode::ScalerFallback;
        if(m_opt.safeMode)return PlayerRuntimeMode::DlssSrSafeMode;
        return m_opt.neuralAddonConfigured?PlayerRuntimeMode::NeuralAddonExperimental:PlayerRuntimeMode::ScalerFallback;
    }
    std::wstring BuildStatusText()const{
        if(!m_loaded||!m_renderer)return{};
        PlayerStatusSnapshot status{};status.mediaLoaded=true;status.runtimeMode=RuntimeMode();status.sourceWidth=m_decoder.NativeWidth();status.sourceHeight=m_decoder.NativeHeight();status.inputWidth=m_renderer->DLSSInputW();status.inputHeight=m_renderer->DLSSInputH();status.outputWidth=m_renderer->OutputW();status.outputHeight=m_renderer->OutputH();status.quality=QualityNameW(m_activeQuality);status.renderedFps=m_submitFps;status.sourceFps=m_decoder.FrameRate();status.droppedFrames=m_droppedFrames;
        std::wstring text=BuildPlayerStatusText(status);if(m_seeking||m_seekPending)text=T(L"status.seeking")+L" \u00b7 "+text;return text;
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
    void ShowDebugMenu(const RECT& anchor){
        UINT selected=IDM_VIEW_FINAL;
        if(m_renderer){switch(m_renderer->GetDebugView()){case D3D12Renderer::DebugView::Input:selected=IDM_VIEW_INPUT;break;case D3D12Renderer::DebugView::MotionVectors:selected=IDM_VIEW_MV;break;case D3D12Renderer::DebugView::Depth:selected=IDM_VIEW_DEPTH;break;case D3D12Renderer::DebugView::BiasMask:selected=IDM_VIEW_MASK;break;case D3D12Renderer::DebugView::Final:break;}}
        HMENU menu=app_menu::CreateDebugViewMenu(selected);if(!menu)return;
        POINT point{anchor.left,anchor.top};ClientToScreen(m_hwnd,&point);
        const UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_LEFTALIGN|TPM_BOTTOMALIGN|TPM_RIGHTBUTTON,point.x,point.y,0,m_hwnd,nullptr);
        DestroyMenu(menu);if(command)HandleCommand(command);
    }

    void ActivateToolbarAction(ToolbarAction action,const RECT& anchor){
        if(!ToolbarActionEnabled(action))return;
        switch(action){
        case ToolbarAction::Open:OpenFromDialog();break;
        case ToolbarAction::OpenYouTube:break;
        case ToolbarAction::Back10:RequestSeek(Position()-10);break;
        case ToolbarAction::PlayPause:TogglePause();break;
        case ToolbarAction::Stop:StopPlayback();break;
        case ToolbarAction::Forward10:RequestSeek(Position()+10);break;
        case ToolbarAction::Mute:ToggleMute();break;
        case ToolbarAction::ToggleDlss:ToggleDLSS();break;
        case ToolbarAction::Aspect:m_fill=!m_fill;Layout();break;
        case ToolbarAction::Adjustments:ShowAdjustments();break;
        case ToolbarAction::DebugView:ShowDebugMenu(anchor);break;
        case ToolbarAction::Fullscreen:ToggleFullscreen();break;
        case ToolbarAction::None:break;
        }
    }

    void FocusNextToolbarAction(bool reverse){
        const auto items=FocusableItems();m_focusedToolbarAction=NextFocusableToolbarAction(items,m_focusedToolbarAction,reverse,ToolbarState());InvalidateControls();
    }

    void ActivateFocusedToolbarAction(){
        if(m_focusedToolbarAction==ToolbarAction::None)return;
        const auto items=FocusableItems();for(const auto& item:items)if(item.action==m_focusedToolbarAction){ActivateToolbarAction(item.action,item.bounds);return;}
    }

    void MouseDown(int x,int y){
        SetFocus(m_hwnd);
        if(!m_loaded){const auto items=FocusableItems();const ToolbarAction action=ResolveToolbarHover(items,POINT{x,y},ToolbarState());if(action!=ToolbarAction::None){m_focusedToolbarAction=action;m_pressedToolbarAction=action;SetCapture(m_hwnd);for(const auto& item:items)if(item.action==action){InvalidateRect(m_hwnd,&item.bounds,FALSE);break;}}return;}
        if(!m_seeking){RECT tr=TimelineRect();if(PtIn(tr,x,y)){m_dragSeek=true;m_seekPreview=SecondsFromX(x);SetCapture(m_hwnd);InvalidateControls();return;}const auto vr=VolumeRect();if(vr&&PtIn(*vr,x,y)){m_dragVolume=true;SetCapture(m_hwnd);SetVolumeFromX(x);return;}}
        const auto items=ToolbarItems();const ToolbarAction action=ResolveToolbarHover(items,POINT{x,y},ToolbarState());if(action!=ToolbarAction::None){m_focusedToolbarAction=action;m_pressedToolbarAction=action;SetCapture(m_hwnd);InvalidateControls();}
    }

    void MouseUp(int x,int y){
        if(m_dragSeek){double target=m_seekPreview;m_dragSeek=false;if(GetCapture()==m_hwnd)ReleaseCapture();RequestSeek(target);return;}
        if(m_dragVolume){m_dragVolume=false;if(GetCapture()==m_hwnd)ReleaseCapture();return;}
        const ToolbarAction pressed=m_pressedToolbarAction;if(pressed==ToolbarAction::None)return;
        m_pressedToolbarAction=ToolbarAction::None;if(GetCapture()==m_hwnd)ReleaseCapture();
        if(!m_loaded){const auto items=FocusableItems();for(const auto& item:items)if(item.action==pressed){InvalidateRect(m_hwnd,&item.bounds,FALSE);if(PtIn(item.bounds,x,y)&&ToolbarActionEnabled(pressed))ActivateToolbarAction(pressed,item.bounds);break;}return;}
        const auto items=ToolbarItems();const ToolbarAction released=ResolveToolbarHover(items,POINT{x,y},ToolbarState());InvalidateControls();if(released==pressed){for(const auto& item:items)if(item.action==pressed){ActivateToolbarAction(pressed,item.bounds);break;}}
    }
    double SecondsFromX(int x)const{RECT r=TimelineRect();const LONG span=(r.right>r.left)?(r.right-r.left):LONG(1);double t=double(LONG(x)-r.left)/double(span);return std::clamp(t,0.0,1.0)*m_decoder.DurationSeconds();}
    void SetVolumeFromX(int x){const auto volumeRect=VolumeRect();if(!volumeRect)return;const RECT& r=*volumeRect;const LONG span=(r.right>r.left)?(r.right-r.left):LONG(1);const float volume=float(std::clamp(double(LONG(x)-r.left)/double(span),0.0,1.0));const bool changed=volume!=m_volume||m_muted;if(!changed)return;m_volume=volume;m_muted=false;m_audio.SetVolume(m_volume);InvalidateToolbarAction(ToolbarAction::Mute);InvalidateVolumeControls();}
    void ToggleMute(){m_muted=!m_muted;m_audio.SetVolume(m_muted?0.0f:m_volume);InvalidateToolbarAction(ToolbarAction::Mute);InvalidateVolumeControls();}
    void ToggleDLSS(){if(!m_renderer)return;m_renderer->SetDLSS(!m_renderer->DLSSEnabled());m_dlssReset=true;if(!m_playing)m_renderer->PresentCurrent();UpdateCachedStatus();InvalidateControls();}
    void Rehook(){if(!m_renderer)return;const std::wstring message=T(L"rehook.confirm"),title=T(L"rehook.title");const int answer=MessageBoxW(m_hwnd,message.c_str(),title.c_str(),MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2);if(!AcceptRehookConfirmation(answer))return;m_renderer->RequestDLSSRecreate();m_dlssReset=true;}
    void SetQualityMode(bool automatic,NVSDK_NGX_PerfQuality_Value q){m_opt.qualityExplicit=!automatic;m_opt.quality=q;if(m_loaded&&!m_path.empty()){std::wstring p=m_path;double keep=Position();bool wasPlaying=m_playing;if(Load(p))RequestSeek(keep,wasPlaying);}}
    void ToggleDepthMode(){auto n=m_guides.GetDepthMode()==TemporalGuideGenerator::DepthMode::Estimated?TemporalGuideGenerator::DepthMode::Flat:TemporalGuideGenerator::DepthMode::Estimated;m_guides.SetDepthMode(n);m_guideReset=true;m_dlssReset=true;UpdateTitle();}
    void SetDebug(D3D12Renderer::DebugView v){if(m_renderer){m_renderer->SetDebugView(v);if(!m_playing)m_renderer->PresentCurrent();InvalidateControls();}}
    void ToggleDebug(D3D12Renderer::DebugView v){if(!m_renderer)return;m_renderer->SetDebugView(m_renderer->GetDebugView()==v?D3D12Renderer::DebugView::Final:v);if(!m_playing)m_renderer->PresentCurrent();InvalidateControls();}
    void ToggleFullscreen(){if(!m_fullscreen){m_savedStyle=GetWindowLongW(m_hwnd,GWL_STYLE);GetWindowRect(m_hwnd,&m_savedRect);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(m_hwnd,MONITOR_DEFAULTTONEAREST),&mi);SetWindowLongW(m_hwnd,GWL_STYLE,m_savedStyle&~(WS_CAPTION|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_SYSMENU));SetWindowPos(m_hwnd,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_FRAMECHANGED);m_fullscreen=true;}else{SetWindowLongW(m_hwnd,GWL_STYLE,m_savedStyle);SetWindowPos(m_hwnd,nullptr,m_savedRect.left,m_savedRect.top,m_savedRect.right-m_savedRect.left,m_savedRect.bottom-m_savedRect.top,SWP_NOZORDER|SWP_FRAMECHANGED);m_fullscreen=false;}Layout();}

    LRESULT WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        switch(m){
        case WM_ERASEBKGND:return 1;
        case WM_DESTROY:m_running=false;PostQuitMessage(0);return 0;
        case WM_CLOSE:DestroyWindow(h);return 0;
        case WM_GETMINMAXINFO:{
            auto* info=reinterpret_cast<MINMAXINFO*>(l);
            if(info){const UINT dpi=ActiveWindowDpi(h);info->ptMinTrackSize.x=std::max<LONG>(info->ptMinTrackSize.x,MinimumPlayerWindowTrackWidth(h,dpi));}
            return 0;
        }
        case WM_DPICHANGED:{
            UpdateFontsForDpi(HIWORD(w));
            const auto* suggested=reinterpret_cast<const RECT*>(l);
            if(suggested)SetWindowPos(h,nullptr,suggested->left,suggested->top,suggested->right-suggested->left,suggested->bottom-suggested->top,SWP_NOZORDER|SWP_NOACTIVATE);
            Layout();InvalidateRect(h,nullptr,FALSE);return 0;
        }
        case WM_SIZE:Layout();return 0;
        case WM_PAINT:Paint();return 0;
        case WM_MOUSEMOVE:{m_mouseX=GET_X_LPARAM(l);m_mouseY=GET_Y_LPARAM(l);if(!m_trackingMouse){TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,h,0};m_trackingMouse=TrackMouseEvent(&tracking)!=FALSE;}if(m_dragSeek&&GetCapture()==h){const double preview=SecondsFromX(m_mouseX);if(preview!=m_seekPreview){m_seekPreview=preview;InvalidatePlaybackProgress();}}if(m_dragVolume&&GetCapture()==h)SetVolumeFromX(m_mouseX);SetHoverAction(ToolbarActionAt(m_mouseX,m_mouseY));return 0;}
        case WM_MOUSELEAVE:m_trackingMouse=false;m_mouseX=-999;m_mouseY=-999;SetHoverAction(ToolbarAction::None);return 0;
        case WM_LBUTTONDOWN:MouseDown(GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
        case WM_LBUTTONUP:MouseUp(GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
        case WM_CAPTURECHANGED:if(m_dragSeek){m_dragSeek=false;InvalidateControls();}if(m_dragVolume)m_dragVolume=false;if(m_pressedToolbarAction!=ToolbarAction::None){m_pressedToolbarAction=ToolbarAction::None;InvalidateControls();}return 0;
        case WM_SETFOCUS:InvalidateControls();return 0;
        case WM_KILLFOCUS:InvalidateControls();return 0;
        case WM_DROPFILES:{HDROP d=reinterpret_cast<HDROP>(w);wchar_t p[32768]{};UINT count=DragQueryFileW(d,0xFFFFFFFF,nullptr,0);if(count>0&&DragQueryFileW(d,0,p,static_cast<UINT>(std::size(p))))Load(p);DragFinish(d);return 0;}
        case WM_MOUSEWHEEL:{if(m_loaded){const float step=(GET_WHEEL_DELTA_WPARAM(w)>0)?0.05f:-0.05f;const float volume=std::clamp(m_volume+step,0.0f,1.0f);const bool changed=m_muted||volume!=m_volume;if(changed){m_muted=false;m_volume=volume;m_audio.SetVolume(m_volume);InvalidateToolbarAction(ToolbarAction::Mute);InvalidateVolumeControls();}}return 0;}
        case WM_COMMAND:HandleCommand(LOWORD(w));return 0;
        case WM_HOTKEY:HandleHotkey(int(w));return 0;
        case WM_KEYDOWN:
            if(w==VK_TAB){FocusNextToolbarAction((GetKeyState(VK_SHIFT)&0x8000)!=0);return 0;}if(w==VK_RETURN&&m_focusedToolbarAction!=ToolbarAction::None){ActivateFocusedToolbarAction();return 0;}if((GetKeyState(VK_CONTROL)&0x8000)&&w=='O'){OpenFromDialog();return 0;}if((GetKeyState(VK_CONTROL)&0x8000)&&w=='E'){ShowAdjustments();return 0;}if(w==VK_SPACE){TogglePause();return 0;}if(w==VK_LEFT){RequestSeek(Position()-10);return 0;}if(w==VK_RIGHT){RequestSeek(Position()+10);return 0;}if(w==VK_F11){ToggleFullscreen();return 0;}if(w==VK_F6){Rehook();return 0;}if(w=='S'){StopPlayback();return 0;}if(w=='A'){m_fill=!m_fill;Layout();return 0;}if(w=='D'){ToggleDLSS();return 0;}if(w=='G'){ToggleDepthMode();return 0;}if(w=='M'){ToggleMute();return 0;}if(w=='1'){SetDebug(D3D12Renderer::DebugView::Final);return 0;}if(w=='2'){SetDebug(D3D12Renderer::DebugView::Input);return 0;}if(w=='3'){SetDebug(D3D12Renderer::DebugView::MotionVectors);return 0;}if(w=='4'){SetDebug(D3D12Renderer::DebugView::Depth);return 0;}if(w=='5'){SetDebug(D3D12Renderer::DebugView::BiasMask);return 0;}if(w==VK_ESCAPE&&m_fullscreen){ToggleFullscreen();return 0;}break;
        }
        return DefWindowProcW(h,m,w,l);
    }

    void HandleCommand(UINT id){
        switch(id){
        case IDM_OPEN:OpenFromDialog();break;case IDM_EXIT:DestroyWindow(m_hwnd);break;case IDM_PLAY:TogglePause();break;case IDM_STOP:StopPlayback();break;case IDM_BACK10:RequestSeek(Position()-10);break;case IDM_FWD10:RequestSeek(Position()+10);break;case IDM_MUTE:ToggleMute();break;case IDM_DLSS:ToggleDLSS();break;case IDM_REHOOK:Rehook();break;
        case IDM_QUALITY_AUTO:SetQualityMode(true,NVSDK_NGX_PerfQuality_Value_MaxQuality);break;case IDM_QUALITY_QUALITY:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_MaxQuality);break;case IDM_QUALITY_BALANCED:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_Balanced);break;case IDM_QUALITY_PERFORMANCE:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_MaxPerf);break;case IDM_QUALITY_ULTRAPERF:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_UltraPerformance);break;case IDM_QUALITY_DLAA:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_DLAA);break;
        case IDM_VIEW_FINAL:SetDebug(D3D12Renderer::DebugView::Final);break;case IDM_VIEW_INPUT:SetDebug(D3D12Renderer::DebugView::Input);break;case IDM_VIEW_MV:SetDebug(D3D12Renderer::DebugView::MotionVectors);break;case IDM_VIEW_DEPTH:SetDebug(D3D12Renderer::DebugView::Depth);break;case IDM_VIEW_MASK:SetDebug(D3D12Renderer::DebugView::BiasMask);break;case IDM_DEPTH_MODE:ToggleDepthMode();break;case IDM_VIDEO_ADJUSTMENTS:ShowAdjustments();break;case IDM_ASPECT_FIT:m_fill=false;Layout();break;case IDM_ASPECT_FILL:m_fill=true;Layout();break;case IDM_FULLSCREEN:ToggleFullscreen();break;case IDM_ADVANCED_SAFE_MODE:RestartInSafeMode();break;
        }
    }

    AppOptions m_opt;Localizer m_loc;UiResources m_uiResources;D3D12Renderer::ColorSettings m_colorSettings{};NVSDK_NGX_PerfQuality_Value m_activeQuality=NVSDK_NGX_PerfQuality_Value_MaxQuality;HWND m_hwnd=nullptr,m_viewport=nullptr,m_renderWnd=nullptr,m_adjustWnd=nullptr;HFONT m_font=nullptr,m_fontSmall=nullptr,m_iconFont=nullptr;
    bool m_running=true,m_loaded=false,m_playing=false,m_haveNext=false,m_fill=false,m_fullscreen=false,m_dragSeek=false,m_dragVolume=false,m_muted=false,m_seekPending=false,m_seekResumePlaying=false,m_seeking=false,m_trackingMouse=false,m_iconFallbackLogged=false;
    ToolbarAction m_pressedToolbarAction=ToolbarAction::None,m_focusedToolbarAction=ToolbarAction::None,m_hoverAction=ToolbarAction::None;
    LONG m_savedStyle=0;RECT m_savedRect{};double m_dar=16.0/9.0,m_currentSec=0,m_playStartSec=0,m_seekPreview=0,m_pendingSeekSec=0;float m_volume=1.0f,m_lastGlobalX=0,m_lastGlobalY=0;int m_mouseX=-999,m_mouseY=-999;
    Clock::time_point m_playStart=Clock::now(),m_fpsWindowStart=Clock::now(),m_lastStaticPresent=Clock::now();double m_submitFps=0.0;uint64_t m_fpsWindowFrames=0;std::wstring m_path,m_cachedStatus,m_cachedWindowTitle;VideoDecoder m_decoder;VideoFrame m_next;std::unique_ptr<D3D12Renderer>m_renderer;TemporalGuideGenerator m_guides;AudioPlayer m_audio;
    bool m_guideReset=true,m_dlssReset=true;int64_t m_lastRenderedTs=-1;uint64_t m_droppedFrames=0;
};

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int){EnablePerMonitorDpiAwareness();AppOptions options=ParseArgs();if(!options.argumentsOk){FailBootstrap(options.argumentError);return 1;}const StartupResult startup=RunNeuralAddonBootstrap(options);if(startup==StartupResult::ExitSuccess)return 0;if(startup==StartupResult::ExitFailure)return 1;if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE)))return 1;if(FAILED(MFStartup(MF_VERSION,MFSTARTUP_FULL))){CoUninitialize();return 1;}PlayerApp app(std::move(options));if(!app.Create(hi)){MFShutdown();CoUninitialize();return 1;}MSG msg{};bool quit=false;while(app.Running()&&!quit){while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT){quit=true;break;}TranslateMessage(&msg);DispatchMessageW(&msg);}if(quit)break;app.Tick();if(app.NeedsRealtimeTick())Sleep(app.TickSleepMs());else WaitMessage();}MFShutdown();CoUninitialize();return 0;}
