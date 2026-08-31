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
#include "Log.h"
#include "ReShadeConfig.h"
#include "RuntimePolicy.h"

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
static constexpr int CONTROL_H = 112;

static const wchar_t* kVideoPatterns =
    L"*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi;*.wmv;*.asf;*.flv;*.f4v;"
    L"*.ts;*.m2ts;*.mts;*.mpg;*.mpeg;*.mpe;*.vob;*.ogv;*.ogg;*.3gp;*.3g2;"
    L"*.mxf;*.nut;*.rm;*.rmvb;*.divx;*.dv;*.y4m;*.ivf;*.hevc;*.h265;*.h264;*.264;*.av1;*.vp9";

enum : UINT {
    IDM_OPEN=100, IDM_EXIT,
    IDM_PLAY=200, IDM_STOP, IDM_BACK10, IDM_FWD10, IDM_MUTE,
    IDM_DLSS=300, IDM_REHOOK, IDM_VIEW_FINAL, IDM_VIEW_INPUT, IDM_VIEW_MV, IDM_VIEW_DEPTH, IDM_VIEW_MASK, IDM_DEPTH_MODE,
    IDM_QUALITY_AUTO=330, IDM_QUALITY_QUALITY, IDM_QUALITY_BALANCED, IDM_QUALITY_PERFORMANCE, IDM_QUALITY_ULTRAPERF, IDM_QUALITY_DLAA,
    IDM_ASPECT_FIT=400, IDM_ASPECT_FILL, IDM_FULLSCREEN, IDM_VIDEO_ADJUSTMENTS,
    IDM_ADVANCED_SAFE_MODE=450,
    IDM_LANG_BASE=500
};


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
    DetectedGpu detectedGpu;
    std::vector<std::wstring> userArguments;
    std::wstring file;
};

static AppOptions ParseArgs() {
    AppOptions o; int argc=0; LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (!argv) return o;
    for(int i=1;i<argc;++i) {
        std::wstring argument=argv[i];
        if(argument==L"--addon-bootstrap-restarted") { o.addonBootstrapRestarted=true; continue; }
        o.userArguments.push_back(argument);
        if(argument==L"--safe-mode") o.safeMode=true;
    }
    for(int i=1;i<argc;++i) {
        std::wstring a=argv[i];
        if(a==L"--addon-bootstrap-restarted" || a==L"--safe-mode") {
            continue;
        } else if(a==L"--output" && i+1<argc) {
            std::wstring v=argv[++i]; auto x=v.find(L'x'); if(x==std::wstring::npos) x=v.find(L'X');
            if(x!=std::wstring::npos) { o.maxW=std::max(64,_wtoi(v.substr(0,x).c_str())); o.maxH=std::max(64,_wtoi(v.substr(x+1).c_str())); }
        } else if(a==L"--quality" && i+1<argc) {
            std::wstring q=argv[++i]; std::transform(q.begin(),q.end(),q.begin(),::towlower);
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
    LocalFree(argv); return o;
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
    const bool priorEnabled=update.changed?!update.addonEnabled:update.addonEnabled;
    const BootstrapAction action=DecideBootstrap(options.neuralAddonRequested,priorEnabled,options.addonBootstrapRestarted,update.ok);
    if(!update.ok) return FailBootstrap(update.error);
    if(update.addonEnabled!=options.neuralAddonRequested) return FailBootstrap(L"ReShade.ini still reports the wrong neural add-on state after the update attempt");
    options.neuralAddonConfigured=update.addonEnabled;

    if(action==BootstrapAction::Fail) return FailBootstrap(L"The neural add-on required a second correction after the bootstrap restart marker");
    if(action==BootstrapAction::Relaunch) {
        std::vector<std::wstring> arguments=options.userArguments;
        arguments.push_back(L"--addon-bootstrap-restarted");
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
    ~PlayerApp(){SaveVideoSettings();if(m_adjustWnd)DestroyWindow(m_adjustWnd);UnregisterOverlayHotkeys();Unload(); if(m_font)DeleteObject(m_font); if(m_fontSmall)DeleteObject(m_fontSmall);}

    bool Create(HINSTANCE hi) {
        m_loc.Initialize();
        LoadVideoSettings();
        INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_BAR_CLASSES};InitCommonControlsEx(&icc);
        WNDCLASSW r{}; r.style=CS_DBLCLKS|CS_OWNDC; r.lpfnWndProc=RenderWndProcStatic; r.hInstance=hi; r.lpszClassName=L"DLSSVideoRenderClassV11"; r.hCursor=LoadCursor(nullptr,IDC_ARROW); r.hbrBackground=nullptr; RegisterClassW(&r);
        WNDCLASSW v{}; v.lpfnWndProc=ViewportWndProcStatic; v.hInstance=hi; v.lpszClassName=L"DLSSVideoViewportClassV11"; v.hCursor=LoadCursor(nullptr,IDC_ARROW); v.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH); RegisterClassW(&v);
        WNDCLASSW a{}; a.lpfnWndProc=AdjustWndProcStatic; a.hInstance=hi; a.lpszClassName=L"DLSSVideoAdjustmentsClassV11"; a.hCursor=LoadCursor(nullptr,IDC_ARROW); a.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1); RegisterClassW(&a);
        WNDCLASSW w{}; w.lpfnWndProc=WndProcStatic; w.hInstance=hi; w.lpszClassName=L"DLSSVideoPlayerV11Class"; w.hCursor=LoadCursor(nullptr,IDC_ARROW); w.hbrBackground=CreateSolidBrush(RGB(18,19,21)); RegisterClassW(&w);
        RECT rc{0,0,1440,880}; AdjustWindowRect(&rc,WS_OVERLAPPEDWINDOW,TRUE);
        const std::wstring appTitle=m_loc.Get(L"app.title");
        m_hwnd=CreateWindowExW(WS_EX_ACCEPTFILES,w.lpszClassName,appTitle.c_str(),WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,rc.right-rc.left,rc.bottom-rc.top,nullptr,CreateMenuBar(),hi,this);
        if(!m_hwnd) return false;
        RegisterOverlayHotkeys();
        BOOL dark=TRUE; DwmSetWindowAttribute(m_hwnd,20,&dark,sizeof(dark)); DWORD corner=2; DwmSetWindowAttribute(m_hwnd,33,&corner,sizeof(corner));
        m_viewport=CreateWindowExW(0,v.lpszClassName,nullptr,WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,100,100,m_hwnd,nullptr,hi,nullptr);
        m_renderWnd=CreateWindowExW(WS_EX_ACCEPTFILES,L"DLSSVideoRenderClassV11",nullptr,WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,0,0,100,100,m_viewport,nullptr,hi,this);
        m_font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        m_fontSmall=CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
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
        if(!m_haveNext){m_playing=false;m_audio.Pause(true);InvalidateRect(m_hwnd,nullptr,FALSE);return;}
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
        if((++m_uiTick%15)==0) UpdateTitle();
        InvalidateRect(m_hwnd,nullptr,FALSE);
    }

    bool Running()const{return m_running;}
    bool NeedsRealtimeTick()const{return m_loaded;}
    DWORD TickSleepMs()const{return (m_loaded&&!m_playing&&!m_seekPending&&!m_seeking)?8u:0u;}


private:
    std::wstring T(const wchar_t* key)const{return m_loc.Get(key);}

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
        RECT bar{0,std::max<LONG>(0,c.bottom-CONTROL_H),c.right,c.bottom};InvalidateRect(m_hwnd,&bar,FALSE);
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

    HMENU CreateMenuBar() {
        HMENU bar=CreateMenu(),file=CreatePopupMenu(),play=CreatePopupMenu(),video=CreatePopupMenu(),dlss=CreatePopupMenu(),quality=CreatePopupMenu(),advanced=CreatePopupMenu(),language=CreatePopupMenu();
        auto add=[&](HMENU m,UINT id,const wchar_t* key){std::wstring s=T(key);AppendMenuW(m,MF_STRING,id,s.c_str());};
        add(file,IDM_OPEN,L"menu.open"); AppendMenuW(file,MF_SEPARATOR,0,nullptr); add(file,IDM_EXIT,L"menu.exit");
        add(play,IDM_PLAY,L"menu.playpause"); add(play,IDM_STOP,L"menu.stop"); add(play,IDM_BACK10,L"menu.back10"); add(play,IDM_FWD10,L"menu.forward10"); add(play,IDM_MUTE,L"menu.mute");
        add(video,IDM_ASPECT_FIT,L"menu.aspectfit"); add(video,IDM_ASPECT_FILL,L"menu.aspectfill"); add(video,IDM_VIDEO_ADJUSTMENTS,L"menu.adjustments"); AppendMenuW(video,MF_SEPARATOR,0,nullptr);
        add(video,IDM_VIEW_FINAL,L"menu.final"); add(video,IDM_VIEW_INPUT,L"menu.input"); add(video,IDM_VIEW_MV,L"menu.mv"); add(video,IDM_VIEW_DEPTH,L"menu.depth"); add(video,IDM_VIEW_MASK,L"menu.mask"); AppendMenuW(video,MF_SEPARATOR,0,nullptr); add(video,IDM_FULLSCREEN,L"menu.fullscreen");
        add(quality,IDM_QUALITY_AUTO,L"menu.quality_auto"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_QUALITY,L"Quality"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_BALANCED,L"Balanced"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_PERFORMANCE,L"Performance"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_ULTRAPERF,L"Ultra Performance"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_DLAA,L"DLAA");
        add(dlss,IDM_DLSS,L"menu.dlss_toggle"); add(dlss,IDM_REHOOK,L"menu.rehook"); add(dlss,IDM_DEPTH_MODE,L"menu.depthmode"); std::wstring qualityName=T(L"menu.quality"); AppendMenuW(dlss,MF_POPUP,reinterpret_cast<UINT_PTR>(quality),qualityName.c_str());
        AppendMenuW(advanced,MF_STRING,IDM_ADVANCED_SAFE_MODE,L"Restart in DLSS SR safe mode");
        m_languageCodes.clear();
        const auto packs=m_loc.AvailableLanguages();
        for(size_t i=0;i<packs.size()&&i<100;++i){
            m_languageCodes.push_back(packs[i].code);
            UINT flags=MF_STRING|(m_loc.Code()==packs[i].code?MF_CHECKED:MF_UNCHECKED);
            AppendMenuW(language,flags,IDM_LANG_BASE+static_cast<UINT>(i),packs[i].name.c_str());
        }
        std::wstring sFile=T(L"menu.file"),sPlay=T(L"menu.playback"),sVideo=T(L"menu.video"),sDlss=T(L"menu.dlss"),sLang=T(L"menu.language");
        AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(file),sFile.c_str()); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(play),sPlay.c_str()); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(video),sVideo.c_str()); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(dlss),sDlss.c_str()); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(advanced),L"Advanced"); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(language),sLang.c_str());
        return bar;
    }

    void ApplyLanguage(const std::wstring& code) {
        const bool reopenAdjust=(m_adjustWnd!=nullptr);if(m_adjustWnd)DestroyWindow(m_adjustWnd);
        m_loc.SetLanguage(code,true); HMENU old=GetMenu(m_hwnd),fresh=CreateMenuBar(); SetMenu(m_hwnd,fresh); DrawMenuBar(m_hwnd); if(old)DestroyMenu(old); UpdateTitle(); InvalidateRect(m_hwnd,nullptr,TRUE);
        if(reopenAdjust)ShowAdjustments();
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
        m_haveNext=m_decoder.ReadNext(m_next);m_audio.Start(path,m_currentSec);m_audio.SetVolume(m_muted?0.0f:m_volume);m_playing=true;m_playStartSec=m_currentSec;m_playStart=Clock::now();m_loaded=true;m_path=path;m_droppedFrames=0;m_uiTick=0;m_seekPending=false;m_seeking=false;m_fpsWindowStart=Clock::now();m_fpsWindowFrames=0;m_submitFps=0.0;
        UpdateTitle();Layout();InvalidateRect(m_hwnd,nullptr,TRUE);return true;
    }

    void Unload() {
        m_seekPending=false;m_seeking=false;m_audio.Stop(); if(m_renderer){m_renderer->WaitGPU();m_renderer.reset();} m_decoder.Close();m_guides.Reset();m_haveNext=false;m_next=VideoFrame{};m_loaded=false;m_playing=false;m_currentSec=0;m_lastRenderedTs=-1;m_path.clear();
        if(m_viewport)ShowWindow(m_viewport,SW_HIDE); UpdateTitle(); if(m_hwnd)InvalidateRect(m_hwnd,nullptr,TRUE);
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
        m_pendingSeekSec=sec;m_seekResumePlaying=resumeAfter;m_seekPending=true;m_playing=false;m_audio.Pause(true);m_seekPreview=sec;InvalidateRect(m_hwnd,nullptr,FALSE);
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
            LOG("Seek failed without crashing; playback remains paused.");m_playing=false;m_seeking=false;m_currentSec=sec;UpdateTitle();InvalidateRect(m_hwnd,nullptr,FALSE);return false;
        }
        m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;
        if(!RenderVideoFrame(f,true)){LOG("Seek frame render failed.");m_playing=false;m_seeking=false;return false;}
        m_currentSec=double(f.timestamp100ns)*1e-7;m_haveNext=m_decoder.ReadNext(m_next);
        const bool audioOk=m_audio.Start(m_path,m_currentSec);if(audioOk){m_audio.SetVolume(m_muted?0.0f:m_volume);m_audio.Pause(!resumeAfter);}else LOG("Seek: no audio stream/output; using steady-clock video pacing.");
        m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=resumeAfter&&m_haveNext;m_guideReset=false;m_dlssReset=false;m_seeking=false;UpdateTitle();InvalidateRect(m_hwnd,nullptr,FALSE);LOG("Seek complete actual="<<m_currentSec);return true;
    }

    void SetPaused(bool pause){if(!m_loaded||m_seeking)return;if(pause==!m_playing)return;if(pause){m_currentSec=Position();m_playing=false;m_audio.Pause(true);}else{if(!m_haveNext&&m_decoder.DurationSeconds()>0){RequestSeek(0,true);return;}m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=true;m_audio.Pause(false);}InvalidateRect(m_hwnd,nullptr,FALSE);}
    void TogglePause(){SetPaused(m_playing);}
    void StopPlayback(){RequestSeek(0,false);}

    void UpdateTitle(){
        if(!m_hwnd)return; if(!m_loaded||!m_renderer){SetWindowTextW(m_hwnd,T(L"app.title").c_str());return;}
        std::wstringstream s;s<<L"DLSS Video Player V11 | source "<<m_decoder.NativeWidth()<<L"x"<<m_decoder.NativeHeight();if(m_decoder.Width()!=m_decoder.NativeWidth()||m_decoder.Height()!=m_decoder.NativeHeight())s<<L" decode "<<m_decoder.Width()<<L"x"<<m_decoder.Height();s<<L" | "<<QualityNameW(m_activeQuality)<<L" | DLSS "<<m_renderer->DLSSInputW()<<L"x"<<m_renderer->DLSSInputH()<<L" -> "<<m_renderer->OutputW()<<L"x"<<m_renderer->OutputH()<<L" | "<<m_decoder.BackendName()<<L" | NGX "<<(m_renderer->DLSSFeatureCreated()?L"CREATE OK":(m_renderer->DLSSAvailable()?L"READY":L"FALLBACK"))<<L" | "<<(m_renderer->DLSSLastEvaluationUsedC()?L"evalC ":L"eval ")<<m_renderer->DLSSEvaluations()<<L" | result 0x"<<std::hex<<uint32_t(m_renderer->DLSSLastResult())<<std::dec;SetWindowTextW(m_hwnd,s.str().c_str());
    }

    void Layout(){
        if(!m_hwnd||!m_viewport||!m_renderWnd)return;RECT c{};GetClientRect(m_hwnd,&c);int W=static_cast<int>(std::max<LONG>(1,c.right-c.left)),H=static_cast<int>(std::max<LONG>(1,c.bottom-c.top));
        if(!m_loaded){MoveWindow(m_viewport,0,0,W,H,TRUE);return;}
        int areaH=std::max(1,H-CONTROL_H);MoveWindow(m_viewport,0,0,W,areaH,TRUE);double ar=m_dar>0?m_dar:16.0/9.0;double areaAr=double(W)/areaH;int rw=0,rh=0;
        if(m_fill){if(areaAr>ar){rw=W;rh=int(std::lround(W/ar));}else{rh=areaH;rw=int(std::lround(areaH*ar));}}else{if(areaAr>ar){rh=areaH;rw=int(std::lround(areaH*ar));}else{rw=W;rh=int(std::lround(W/ar));}}
        SetWindowPos(m_renderWnd,nullptr,(W-rw)/2,(areaH-rh)/2,std::max(1,rw),std::max(1,rh),SWP_NOZORDER|SWP_NOACTIVATE);
        InvalidateRect(m_viewport,nullptr,FALSE);InvalidateControls();
    }

    RECT TimelineRect()const{RECT c{};GetClientRect(m_hwnd,&c);return RECT{18,c.bottom-24,c.right-18,c.bottom-14};}
    RECT VolumeRect()const{RECT c{};GetClientRect(m_hwnd,&c);return RECT{c.right-185,c.bottom-69,c.right-95,c.bottom-61};}
    RECT EmptyOpenRect()const{RECT c{};GetClientRect(m_hwnd,&c);int cx=(c.left+c.right)/2,cy=(c.top+c.bottom)/2;return RECT{cx-95,cy+46,cx+95,cy+88};}
    RECT ButtonRect(int idx,int width)const{RECT c{};GetClientRect(m_hwnd,&c);int x=14;const int widths[]={68,44,62,48,44,54,82,72,66,88,44,56,52,54};for(int i=0;i<idx&&i<14;++i)x+=widths[i]+5;return RECT{x,c.bottom-96,x+width,c.bottom-58};}
    bool PtIn(const RECT&r,int x,int y)const{return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}

    void DrawButton(HDC dc,const RECT&r,const std::wstring&text,bool active=false){bool hover=PtIn(r,m_mouseX,m_mouseY);COLORREF fill=active?RGB(34,112,190):(hover?RGB(62,65,70):RGB(47,49,53));HBRUSH b=CreateSolidBrush(fill);HPEN p=CreatePen(PS_SOLID,1,active?RGB(70,155,235):RGB(75,78,84));auto ob=SelectObject(dc,b),op=SelectObject(dc,p);RoundRect(dc,r.left,r.top,r.right,r.bottom,8,8);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(b);DeleteObject(p);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(240,240,242));auto of=SelectObject(dc,m_font);RECT t=r;DrawTextW(dc,text.c_str(),-1,&t,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);SelectObject(dc,of);}

    void Paint(){
        PAINTSTRUCT ps{};HDC dc=BeginPaint(m_hwnd,&ps);RECT c{};GetClientRect(m_hwnd,&c);
        if(!m_loaded){
            HBRUSH bg=CreateSolidBrush(RGB(18,19,21));FillRect(dc,&c,bg);DeleteObject(bg);SetBkMode(dc,TRANSPARENT);
            RECT title{40,(c.bottom/2)-62,c.right-40,(c.bottom/2)-18};SetTextColor(dc,RGB(242,243,245));auto of=SelectObject(dc,m_font);std::wstring tt=T(L"idle.title");DrawTextW(dc,tt.c_str(),-1,&title,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            RECT sub{40,(c.bottom/2)-17,c.right-40,(c.bottom/2)+24};SetTextColor(dc,RGB(160,164,172));SelectObject(dc,m_fontSmall);std::wstring ss=T(L"idle.subtitle");DrawTextW(dc,ss.c_str(),-1,&sub,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,of);
            DrawButton(dc,EmptyOpenRect(),T(L"idle.open"));EndPaint(m_hwnd,&ps);return;
        }
        RECT bar{0,c.bottom-CONTROL_H,c.right,c.bottom};HBRUSH bg=CreateSolidBrush(RGB(27,28,31));FillRect(dc,&bar,bg);DeleteObject(bg);HPEN line=CreatePen(PS_SOLID,1,RGB(54,56,61));auto op=SelectObject(dc,line);MoveToEx(dc,0,bar.top,nullptr);LineTo(dc,c.right,bar.top);SelectObject(dc,op);DeleteObject(line);
        const int widths[]={68,44,62,48,44,54,82,72,66,88,44,56,52,54};
        DrawButton(dc,ButtonRect(0,widths[0]),T(L"button.open"));DrawButton(dc,ButtonRect(1,widths[1]),L"-10");DrawButton(dc,ButtonRect(2,widths[2]),m_playing?T(L"button.pause"):T(L"button.play"),m_playing);DrawButton(dc,ButtonRect(3,widths[3]),T(L"button.stop"));DrawButton(dc,ButtonRect(4,widths[4]),L"+10");DrawButton(dc,ButtonRect(5,widths[5]),m_muted?T(L"button.sound"):T(L"button.mute"),m_muted);DrawButton(dc,ButtonRect(6,widths[6]),m_renderer&&m_renderer->DLSSEnabled()?L"DLSS ON":L"DLSS OFF",m_renderer&&m_renderer->DLSSEnabled());DrawButton(dc,ButtonRect(7,widths[7]),m_fill?T(L"button.crop"):T(L"button.aspect"));DrawButton(dc,ButtonRect(8,widths[8]),T(L"button.color"),m_adjustWnd!=nullptr);DrawButton(dc,ButtonRect(9,widths[9]),T(L"button.rehook"));DrawButton(dc,ButtonRect(10,widths[10]),L"MV",m_renderer&&m_renderer->GetDebugView()==D3D12Renderer::DebugView::MotionVectors);DrawButton(dc,ButtonRect(11,widths[11]),L"Depth",m_renderer&&m_renderer->GetDebugView()==D3D12Renderer::DebugView::Depth);DrawButton(dc,ButtonRect(12,widths[12]),L"Mask",m_renderer&&m_renderer->GetDebugView()==D3D12Renderer::DebugView::BiasMask);DrawButton(dc,ButtonRect(13,widths[13]),T(L"button.full"),m_fullscreen);
        RECT vr=VolumeRect();HPEN vp=CreatePen(PS_SOLID,4,RGB(94,98,105));op=SelectObject(dc,vp);MoveToEx(dc,vr.left,(vr.top+vr.bottom)/2,nullptr);LineTo(dc,vr.right,(vr.top+vr.bottom)/2);SelectObject(dc,op);DeleteObject(vp);int vx=vr.left+int((vr.right-vr.left)*(m_muted?0.0f:m_volume));HBRUSH vb=CreateSolidBrush(RGB(230,232,235));Ellipse(dc,vx-5,(vr.top+vr.bottom)/2-5,vx+5,(vr.top+vr.bottom)/2+5);DeleteObject(vb);
        double shown=m_dragSeek?m_seekPreview:(m_seekPending?m_pendingSeekSec:Position());RECT tr=TimelineRect();HBRUSH tb=CreateSolidBrush(RGB(68,71,77));FillRect(dc,&tr,tb);DeleteObject(tb);double d=m_decoder.DurationSeconds(),f=d>0?std::clamp(shown/d,0.0,1.0):0;RECT done=tr;done.right=done.left+int((done.right-done.left)*f);HBRUSH db=CreateSolidBrush(RGB(55,139,226));FillRect(dc,&done,db);DeleteObject(db);int kx=done.right;HBRUSH kb=CreateSolidBrush(RGB(246,246,248));Ellipse(dc,kx-5,tr.top-3,kx+5,tr.bottom+3);DeleteObject(kb);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(206,208,212));auto of=SelectObject(dc,m_fontSmall);std::wstring time=TimeText(shown)+L" / "+TimeText(d);TextOutW(dc,18,c.bottom-50,time.c_str(),int(time.size()));
        std::wstringstream st;if(m_seeking||m_seekPending)st<<T(L"status.seeking")<<L"  |  ";st<<RuntimeModeText()<<L"  |  source "<<m_decoder.NativeWidth()<<L"x"<<m_decoder.NativeHeight();if(m_decoder.Width()!=m_decoder.NativeWidth()||m_decoder.Height()!=m_decoder.NativeHeight())st<<L" -> decode "<<m_decoder.Width()<<L"x"<<m_decoder.Height();st<<L"  |  "<<QualityNameW(m_activeQuality)<<L"  |  input "<<m_renderer->DLSSInputW()<<L"x"<<m_renderer->DLSSInputH()<<L"  |  output "<<m_renderer->OutputW()<<L"x"<<m_renderer->OutputH()<<L"  |  NGX create "<<(m_renderer->DLSSFeatureCreated()?L"OK":L"-")<<L"  "<<(m_renderer->DLSSLastEvaluationUsedC()?L"evalC ":L"eval ")<<m_renderer->DLSSEvaluations()<<L"  0x"<<std::hex<<uint32_t(m_renderer->DLSSLastResult())<<std::dec<<L"  |  fps "<<int(std::lround(m_submitFps))<<L"/"<<int(std::lround(m_decoder.FrameRate()))<<L"  |  drop "<<m_droppedFrames<<L"  |  MV global "<<int(std::lround(m_lastGlobalX))<<L","<<int(std::lround(m_lastGlobalY));std::wstring status=st.str();RECT sr{145,c.bottom-53,c.right-205,c.bottom-34};DrawTextW(dc,status.c_str(),-1,&sr,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);std::wstring vol=m_muted?T(L"status.muted"):(T(L"status.volume")+L" "+std::to_wstring(int(m_volume*100))+L"%");TextOutW(dc,vr.right+8,vr.top-6,vol.c_str(),int(vol.size()));SelectObject(dc,of);
        EndPaint(m_hwnd,&ps);
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
    std::wstring RuntimeModeText()const{
        const wchar_t* gpu=L"Unsupported GPU";
        switch(m_opt.detectedGpu.generation){case GpuGeneration::Rtx40Ada:gpu=L"RTX 40 Ada";break;case GpuGeneration::Rtx50Blackwell:gpu=L"RTX 50 Blackwell";break;case GpuGeneration::OtherNvidia:gpu=L"Other NVIDIA";break;case GpuGeneration::Unsupported:break;}
        std::wstring mode;
        if(m_opt.safeMode) mode=L"DLSS SR safe mode";
        else if(m_opt.neuralAddonConfigured) mode=L"Neural addon enabled (experimental)";
        else mode=L"Neural addon disabled";
        return std::wstring(gpu)+L" - "+mode;
    }
    void RestartInSafeMode(){
        const int answer=MessageBoxW(m_hwnd,L"Restart the player in DLSS SR safe mode?\n\nThis disables the experimental neural add-on for this launch.",L"Restart in DLSS SR safe mode",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2);
        if(answer!=IDYES)return;
        std::vector<std::wstring> arguments=m_opt.userArguments;
        if(std::find(arguments.begin(),arguments.end(),L"--safe-mode")==arguments.end())arguments.push_back(L"--safe-mode");
        std::wstring launchError;
        if(!LaunchSameExecutable(arguments,launchError)){
            LOG("Safe-mode restart failed: "<<WideToUtf8(launchError));
            MessageBoxW(m_hwnd,L"The player could not restart in DLSS SR safe mode.\n\nSee DLSSVideoPlayer.log for details.",L"DLSS Video Player",MB_OK|MB_ICONERROR);
            return;
        }
        DestroyWindow(m_hwnd);
    }
    void MouseDown(int x,int y){
        SetFocus(m_hwnd);if(!m_loaded){if(PtIn(EmptyOpenRect(),x,y))OpenFromDialog();return;}if(m_seeking)return;
        RECT tr=TimelineRect();if(PtIn(tr,x,y)){m_dragSeek=true;m_seekPreview=SecondsFromX(x);SetCapture(m_hwnd);InvalidateRect(m_hwnd,nullptr,FALSE);return;}RECT vr=VolumeRect();if(PtIn(vr,x,y)){m_muted=false;m_dragVolume=true;SetCapture(m_hwnd);SetVolumeFromX(x);return;}
        const int widths[]={68,44,62,48,44,54,82,72,66,88,44,56,52,54};if(PtIn(ButtonRect(0,widths[0]),x,y))OpenFromDialog();else if(PtIn(ButtonRect(1,widths[1]),x,y))RequestSeek(Position()-10);else if(PtIn(ButtonRect(2,widths[2]),x,y))TogglePause();else if(PtIn(ButtonRect(3,widths[3]),x,y))StopPlayback();else if(PtIn(ButtonRect(4,widths[4]),x,y))RequestSeek(Position()+10);else if(PtIn(ButtonRect(5,widths[5]),x,y))ToggleMute();else if(PtIn(ButtonRect(6,widths[6]),x,y))ToggleDLSS();else if(PtIn(ButtonRect(7,widths[7]),x,y)){m_fill=!m_fill;Layout();}else if(PtIn(ButtonRect(8,widths[8]),x,y))ShowAdjustments();else if(PtIn(ButtonRect(9,widths[9]),x,y))Rehook();else if(PtIn(ButtonRect(10,widths[10]),x,y))ToggleDebug(D3D12Renderer::DebugView::MotionVectors);else if(PtIn(ButtonRect(11,widths[11]),x,y))ToggleDebug(D3D12Renderer::DebugView::Depth);else if(PtIn(ButtonRect(12,widths[12]),x,y))ToggleDebug(D3D12Renderer::DebugView::BiasMask);else if(PtIn(ButtonRect(13,widths[13]),x,y))ToggleFullscreen();
    }
    double SecondsFromX(int x)const{RECT r=TimelineRect();const LONG span=(r.right>r.left)?(r.right-r.left):LONG(1);double t=double(LONG(x)-r.left)/double(span);return std::clamp(t,0.0,1.0)*m_decoder.DurationSeconds();}
    void SetVolumeFromX(int x){RECT r=VolumeRect();const LONG span=(r.right>r.left)?(r.right-r.left):LONG(1);m_volume=float(std::clamp(double(LONG(x)-r.left)/double(span),0.0,1.0));m_audio.SetVolume(m_volume);InvalidateControls();}
    void ToggleMute(){m_muted=!m_muted;m_audio.SetVolume(m_muted?0.0f:m_volume);InvalidateControls();}
    void ToggleDLSS(){if(!m_renderer)return;m_renderer->SetDLSS(!m_renderer->DLSSEnabled());m_dlssReset=true;if(!m_playing)m_renderer->PresentCurrent();InvalidateControls();}
    void Rehook(){if(m_renderer){m_renderer->RequestDLSSRecreate();m_dlssReset=true;}}
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
        case WM_SIZE:Layout();return 0;
        case WM_PAINT:Paint();return 0;
        case WM_MOUSEMOVE:m_mouseX=GET_X_LPARAM(l);m_mouseY=GET_Y_LPARAM(l);if(m_dragSeek&&GetCapture()==h)m_seekPreview=SecondsFromX(m_mouseX);if(m_dragVolume&&GetCapture()==h)SetVolumeFromX(m_mouseX);InvalidateControls();return 0;
        case WM_LBUTTONDOWN:MouseDown(GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
        case WM_LBUTTONUP:if(m_dragSeek){double target=m_seekPreview;m_dragSeek=false;if(GetCapture()==h)ReleaseCapture();RequestSeek(target);}else if(m_dragVolume){m_dragVolume=false;if(GetCapture()==h)ReleaseCapture();}return 0;
        case WM_CAPTURECHANGED:if(m_dragSeek){m_dragSeek=false;InvalidateRect(m_hwnd,nullptr,FALSE);}if(m_dragVolume)m_dragVolume=false;return 0;
        case WM_DROPFILES:{HDROP d=reinterpret_cast<HDROP>(w);wchar_t p[32768]{};UINT count=DragQueryFileW(d,0xFFFFFFFF,nullptr,0);if(count>0&&DragQueryFileW(d,0,p,static_cast<UINT>(std::size(p))))Load(p);DragFinish(d);return 0;}
        case WM_MOUSEWHEEL:{if(m_loaded){m_muted=false;float step=(GET_WHEEL_DELTA_WPARAM(w)>0)?0.05f:-0.05f;m_volume=std::clamp(m_volume+step,0.0f,1.0f);m_audio.SetVolume(m_volume);InvalidateControls();}return 0;}
        case WM_COMMAND:HandleCommand(LOWORD(w));return 0;
        case WM_HOTKEY:HandleHotkey(int(w));return 0;
        case WM_KEYDOWN:
            if((GetKeyState(VK_CONTROL)&0x8000)&&w=='O'){OpenFromDialog();return 0;}if((GetKeyState(VK_CONTROL)&0x8000)&&w=='E'){ShowAdjustments();return 0;}if(w==VK_SPACE){TogglePause();return 0;}if(w==VK_LEFT){RequestSeek(Position()-10);return 0;}if(w==VK_RIGHT){RequestSeek(Position()+10);return 0;}if(w==VK_F11){ToggleFullscreen();return 0;}if(w==VK_F6){Rehook();return 0;}if(w=='D'){ToggleDLSS();return 0;}if(w=='G'){ToggleDepthMode();return 0;}if(w=='M'){ToggleMute();return 0;}if(w=='1'){SetDebug(D3D12Renderer::DebugView::Final);return 0;}if(w=='2'){SetDebug(D3D12Renderer::DebugView::Input);return 0;}if(w=='3'){SetDebug(D3D12Renderer::DebugView::MotionVectors);return 0;}if(w=='4'){SetDebug(D3D12Renderer::DebugView::Depth);return 0;}if(w=='5'){SetDebug(D3D12Renderer::DebugView::BiasMask);return 0;}if(w==VK_ESCAPE&&m_fullscreen){ToggleFullscreen();return 0;}break;
        }
        return DefWindowProcW(h,m,w,l);
    }

    void HandleCommand(UINT id){
        const UINT langEnd=IDM_LANG_BASE+static_cast<UINT>(m_languageCodes.size());if(id>=IDM_LANG_BASE && id<langEnd){ApplyLanguage(m_languageCodes[id-IDM_LANG_BASE]);return;}
        switch(id){
        case IDM_OPEN:OpenFromDialog();break;case IDM_EXIT:DestroyWindow(m_hwnd);break;case IDM_PLAY:TogglePause();break;case IDM_STOP:StopPlayback();break;case IDM_BACK10:RequestSeek(Position()-10);break;case IDM_FWD10:RequestSeek(Position()+10);break;case IDM_MUTE:ToggleMute();break;case IDM_DLSS:ToggleDLSS();break;case IDM_REHOOK:Rehook();break;
        case IDM_QUALITY_AUTO:SetQualityMode(true,NVSDK_NGX_PerfQuality_Value_MaxQuality);break;case IDM_QUALITY_QUALITY:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_MaxQuality);break;case IDM_QUALITY_BALANCED:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_Balanced);break;case IDM_QUALITY_PERFORMANCE:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_MaxPerf);break;case IDM_QUALITY_ULTRAPERF:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_UltraPerformance);break;case IDM_QUALITY_DLAA:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_DLAA);break;
        case IDM_VIEW_FINAL:SetDebug(D3D12Renderer::DebugView::Final);break;case IDM_VIEW_INPUT:SetDebug(D3D12Renderer::DebugView::Input);break;case IDM_VIEW_MV:SetDebug(D3D12Renderer::DebugView::MotionVectors);break;case IDM_VIEW_DEPTH:SetDebug(D3D12Renderer::DebugView::Depth);break;case IDM_VIEW_MASK:SetDebug(D3D12Renderer::DebugView::BiasMask);break;case IDM_DEPTH_MODE:ToggleDepthMode();break;case IDM_VIDEO_ADJUSTMENTS:ShowAdjustments();break;case IDM_ASPECT_FIT:m_fill=false;Layout();break;case IDM_ASPECT_FILL:m_fill=true;Layout();break;case IDM_FULLSCREEN:ToggleFullscreen();break;case IDM_ADVANCED_SAFE_MODE:RestartInSafeMode();break;
        }
    }

    AppOptions m_opt;Localizer m_loc;std::vector<std::wstring> m_languageCodes;D3D12Renderer::ColorSettings m_colorSettings{};NVSDK_NGX_PerfQuality_Value m_activeQuality=NVSDK_NGX_PerfQuality_Value_MaxQuality;HWND m_hwnd=nullptr,m_viewport=nullptr,m_renderWnd=nullptr,m_adjustWnd=nullptr;HFONT m_font=nullptr,m_fontSmall=nullptr;
    bool m_running=true,m_loaded=false,m_playing=false,m_haveNext=false,m_fill=false,m_fullscreen=false,m_dragSeek=false,m_dragVolume=false,m_muted=false,m_seekPending=false,m_seekResumePlaying=false,m_seeking=false;
    LONG m_savedStyle=0;RECT m_savedRect{};double m_dar=16.0/9.0,m_currentSec=0,m_playStartSec=0,m_seekPreview=0,m_pendingSeekSec=0;float m_volume=1.0f,m_lastGlobalX=0,m_lastGlobalY=0;int m_mouseX=-999,m_mouseY=-999;
    Clock::time_point m_playStart=Clock::now(),m_fpsWindowStart=Clock::now(),m_lastStaticPresent=Clock::now();double m_submitFps=0.0;uint64_t m_fpsWindowFrames=0;std::wstring m_path;VideoDecoder m_decoder;VideoFrame m_next;std::unique_ptr<D3D12Renderer>m_renderer;TemporalGuideGenerator m_guides;AudioPlayer m_audio;
    bool m_guideReset=true,m_dlssReset=true;int64_t m_lastRenderedTs=-1;uint64_t m_droppedFrames=0,m_uiTick=0;
};

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int){AppOptions options=ParseArgs();const StartupResult startup=RunNeuralAddonBootstrap(options);if(startup==StartupResult::ExitSuccess)return 0;if(startup==StartupResult::ExitFailure)return 1;if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE)))return 1;if(FAILED(MFStartup(MF_VERSION,MFSTARTUP_FULL))){CoUninitialize();return 1;}PlayerApp app(std::move(options));if(!app.Create(hi)){MFShutdown();CoUninitialize();return 1;}MSG msg{};bool quit=false;while(app.Running()&&!quit){while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT){quit=true;break;}TranslateMessage(&msg);DispatchMessageW(&msg);}if(quit)break;app.Tick();if(app.NeedsRealtimeTick())Sleep(app.TickSleepMs());else WaitMessage();}MFShutdown();CoUninitialize();return 0;}
