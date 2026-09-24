#include "GpuTestGate.h"
// Opt-in hardware smoke test: registered under the `gpu` CTest label, which
// the portable suite excludes (`ctest -LE gpu`), and run on an RTX machine with
// `ctest -L gpu` against the demo clip in docs/media.
#include "D3D12Renderer.h"
#include "TemporalGuides.h"
#include "VideoDecoder.h"
#include "UpscalingPolicy.h"
#include "Utf8Text.h"
#include <d3d12sdklayers.h>
#include <mfapi.h>
#include <chrono>
#include <iostream>
#include <sstream>
#include "GuideControls.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iomanip>
#include <string>
#include <vector>

// With five arguments it becomes a guide A/B probe instead: it renders the first
// N frames through the real DLSS-SR path with the named guides and writes every
// captured output frame to a raw BGRA file, so two runs can be compared pixel by
// pixel. That is how the question "does this guide reach the consumer" is
// answered for the upscaling feature, which is a different NGX feature from the
// neural rendering the helper drives.
// A sixth argument, `per-frame`, renders the same frames with Super Resolution's
// history reset on every frame (UpscalingHistory::PerFrame), for the A/B in
// docs/measurements/sr-history-20260924.
int RunGuideProbe(const wchar_t* source,uint32_t targetHeight,const GuideControls& controls,const wchar_t* rawOut,uint32_t frames,
                  UpscalingHistory history=kDefaultUpscalingHistory);

// `device-loss` as the third argument runs the renderer's failure paths on the real
// GPU instead, which no headless test reaches: a Present that refuses a frame whose
// command lists are already on the queue, and a device removed between frames.
int RunDeviceLossProbe(const wchar_t* source,uint32_t targetHeight);

// `hdr-output` as the second argument drives HDR output (P3.1) on whatever display
// this machine has: the swapchain is switched to R10G10B10A2 / ST 2084 whether or
// not the output is in HDR mode, then PQ source frames, a PQ comparison reference,
// every comparison mode, both debug views and the composed-view capture are run on
// it, and the swapchain is switched back. Under the debug layer, when it is
// installed, every step must leave no error. Source: an HDR10 clip.
int RunHdrOutputProbe(const wchar_t* source);

// `sr-quality` as the second argument, after FFmpeg's folder and before a scratch
// folder, is the Super Resolution quality gate (W4-SR). The flow engine reports a
// fixed sub-pixel field for a pair of IDENTICAL frames - up to 0.35 px, on half the
// pixels - and Super Resolution, handed that as motion, re-sampled its history by it
// every frame: a held frame lost 17 VMAF in 60 frames, and moving clips lost about 11.
// The resolve pass now zeroes any vector that explains the pair no better than no
// motion. This renders a generated clip at 960x540 up to 1920x1080, scores it against
// its 1080p original, and fails when a held frame decays. It also prints bicubic's
// score beside SR's, through the same BGRA path, without asserting an order between
// them: on band-limited video DLSS SR measured below bicubic on every clip tried,
// frame 0 included, where no history or motion vector is involved at all - see
// docs/measurements/sr-quality-20260924/REPORT.md. It then renders the held frame
// again with Per-frame history and fails (10) unless every frame comes out
// byte-identical, which is what a reset on every frame of identical input gives.
int RunSrQualityProbe(const wchar_t* ffmpegDirectory,const wchar_t* workDirectory);
// The generated original, an FFmpeg lavfi source at 1080p30: a Mandelbrot zoom,
// because it has detail at every scale for the engine to be wrong about.
inline constexpr wchar_t kSrQualitySource[]=L"mandelbrot=s=1920x1080:r=30";

int wmain(int argc,wchar_t** argv) {
    if (const int skip = gpu_test_gate::SkipWithoutGpu()) return skip;
    if(argc==4&&std::wstring_view(argv[2])==L"sr-quality")return RunSrQualityProbe(argv[1],argv[3]);
    if(argc==6||(argc==7&&std::wstring_view(argv[6])==L"per-frame")){
        const std::wstring wide(argv[4]);
        std::string text;for(const wchar_t c:wide){if(c>0x7F)return 2;text.push_back(char(c));}
        const auto controls=ParseGuideControls(text);
        if(!controls)return 2;
        return RunGuideProbe(argv[1],std::wcstoul(argv[2],nullptr,10),*controls,argv[3],std::wcstoul(argv[5],nullptr,10),
                             argc==7?UpscalingHistory::PerFrame:kDefaultUpscalingHistory);
    }
    if(argc==4&&std::wstring_view(argv[3])==L"device-loss")return RunDeviceLossProbe(argv[1],std::wcstoul(argv[2],nullptr,10));
    if(argc==3&&std::wstring_view(argv[2])==L"hdr-output")return RunHdrOutputProbe(argv[1]);
    if(argc!=3)return 2; // source path, target height (1440 or 2160)
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);MFStartup(MF_VERSION);
    int code=1;
    {
        VideoDecoder decoder;
        if(!decoder.Open(argv[1],MediaSourceKind::LocalFile))return 3;
        const auto target=UpscalingTarget(decoder.Width(),decoder.Height(),std::wcstoul(argv[2],nullptr,10));
        if(!target.grows)return 4;
        HWND window=CreateWindowExW(0,L"STATIC",L"SR hardware smoke",WS_POPUP,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        auto renderer=MakeD3D12Renderer();
        const auto [gw,gh]=TemporalGuideGenerator::AnalysisGrid(decoder.Width(),decoder.Height(),decoder.FrameRate());
        if(renderer->Initialize(window,decoder.Width(),decoder.Height(),target.width,target.height,gw,gh,
            NVSDK_NGX_PerfQuality_Value_MaxQuality,true)&&renderer->DLSSAvailable()){
            TemporalGuideGenerator guides;VideoFrame frame;uint32_t count=0;
            const float frameMs=float(1000/decoder.FrameRate());
            const auto start=std::chrono::steady_clock::now();
            bool ok=renderer->DLSSInputW()==decoder.Width()&&renderer->DLSSInputH()==decoder.Height();
            while(ok&&count<120&&decoder.ReadNext(frame)){
                GuideFrame guide;
                const FrameIdentity id=IdentityOf(frame,guides.HistoryGeneration(),0,count==0?HistoryReset::FirstFrame:HistoryReset::None);
                ok=guides.Generate(frame.bgra.data(),frame.bgra.size(),decoder.Width(),decoder.Height(),decoder.Width(),decoder.Height(),decoder.FrameRate(),id,guide)&&
                    renderer->UploadReferenceFrame(frame.bgra.data(),frame.bgra.size())&&
                    renderer->RenderFrame(frame.bgra.data(),frame.bgra.size(),id,guide,frameMs)&&renderer->LastFrameUsedDLSS();
                if(ok)++count;
            }
            const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            // A guide generated for a different frame must be rejected before any GPU work.
            bool rejected=false;
            if(ok&&count>0){
                GuideFrame stale;FrameIdentity other=IdentityOf(frame,guides.HistoryGeneration(),0,HistoryReset::None);
                other.frameNumber+=1000;
                rejected=guides.Generate(frame.bgra.data(),frame.bgra.size(),decoder.Width(),decoder.Height(),decoder.Width(),decoder.Height(),decoder.FrameRate(),other,stale)&&
                    !renderer->RenderFrame(frame.bgra.data(),frame.bgra.size(),IdentityOf(frame,guides.HistoryGeneration(),0,HistoryReset::None),stale,frameMs);
            }
            // Cycle the paused presentation through every comparison mode against the
            // uploaded reference; each PresentCurrent must succeed without device loss.
            static constexpr ComparisonMode modes[]={ComparisonMode::Neural,ComparisonMode::Original,ComparisonMode::Blend,ComparisonMode::SplitVertical,ComparisonMode::Wipe};
            uint32_t presented=0;
            for(const ComparisonMode mode:modes){
                if(!ok)break;
                ComparisonSettings cmp;cmp.mode=mode;cmp.amount=0.35f;cmp.splitX=0.6f;cmp.zoomScale=mode==ComparisonMode::Wipe?2.0f:1.0f;cmp.zoomCenterX=0.3f;cmp.zoomCenterY=0.7f;
                renderer->SetComparison(cmp);
                ok=renderer->PresentCurrent()&&!renderer->GpuUnusable();
                if(ok)++presented;
            }
            renderer->SetComparison({});
            // The accepted output can be smaller than the requested one when the
            // source could not reach it, so report both.
            std::cout<<"source="<<decoder.Width()<<"x"<<decoder.Height()<<" output="<<renderer->OutputW()<<"x"<<renderer->OutputH()
                <<((renderer->OutputW()!=target.width||renderer->OutputH()!=target.height)
                    ?" requested="+std::to_string(target.width)+"x"+std::to_string(target.height):std::string{})
                <<" frames="<<count<<" evaluations="<<renderer->DLSSEvaluations()<<" elapsed="<<seconds<<" throughput="<<count/seconds<<" fps"
                <<" neuralGpuMs="<<renderer->LastNeuralGpuMs()<<" peakLocalVramMiB="<<renderer->PeakLocalVideoMemoryMiB()
                <<" comparisonModes="<<presented<<" identityRejected="<<rejected<<" fenceWait="<<int(renderer->LastFenceWaitResult())<<"\n";
            code=ok&&count>0&&renderer->DLSSEvaluations()==count&&rejected&&presented==5&&renderer->LastNeuralGpuMs()>0.0&&
                !GetModuleHandleW(L"renodx-dlss5.addon64")&&!GetModuleHandleW(L"nvngx_dlssnr.dll")?0:5;
        }else std::cout<<"SR initialization rejected; see DLSSVideoPlayer.log\n";
        renderer.reset();DestroyWindow(window);
    }
    MFShutdown();CoUninitialize();return code;
}

int RunGuideProbe(const wchar_t* source,uint32_t targetHeight,const GuideControls& controls,const wchar_t* rawOut,uint32_t frames,
                  UpscalingHistory history)
{
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);MFStartup(MF_VERSION);
    int code=1;
    {
        VideoDecoder decoder;
        if(!decoder.Open(source,MediaSourceKind::LocalFile))return 3;
        const auto target=UpscalingTarget(decoder.Width(),decoder.Height(),targetHeight);
        if(!target.grows)return 4;
        HWND window=CreateWindowExW(0,L"STATIC",L"guide probe",WS_POPUP,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        auto renderer=MakeD3D12Renderer();
        const auto [gw,gh]=TemporalGuideGenerator::AnalysisGrid(decoder.Width(),decoder.Height(),decoder.FrameRate());
        // This arm drains the capture ring via RenderFrameForCache.
        if(renderer->Initialize(window,decoder.Width(),decoder.Height(),target.width,target.height,gw,gh,
            NVSDK_NGX_PerfQuality_Value_MaxQuality,true,true)&&renderer->DLSSAvailable()){
            TemporalGuideGenerator guides;guides.SetControls(controls);
            renderer->SetUpscalingHistory(history);
            VideoFrame frame;uint32_t count=0;bool ok=true;
            std::ofstream out(std::filesystem::path(rawOut),std::ios::binary|std::ios::trunc);
            const float frameMs=float(1000/decoder.FrameRate());
            while(ok&&count<frames&&decoder.ReadNext(frame)){
                GuideFrame guide;CapturedVideoFrame captured;
                const FrameIdentity id=IdentityOf(frame,guides.HistoryGeneration(),0,count==0?HistoryReset::FirstFrame:HistoryReset::None);
                ok=guides.Generate(frame.bgra.data(),frame.bgra.size(),decoder.Width(),decoder.Height(),decoder.Width(),decoder.Height(),decoder.FrameRate(),id,guide)&&
                   renderer->RenderFrameForCache(frame.bgra.data(),frame.bgra.size(),id,guide,frameMs,captured);
                if(ok){out.write(reinterpret_cast<const char*>(captured.pixels.data()),std::streamsize(captured.pixels.size()));++count;}
            }
            out.close();
            std::cout<<"guides="<<CanonicalGuideControls(controls)<<" history="<<UpscalingHistoryName(history)
                <<" frames="<<count<<" output="<<target.width<<"x"<<target.height
                <<" evaluations="<<renderer->DLSSEvaluations()<<"\n";
            code=ok&&count==frames?0:5;
        }else std::cout<<"SR initialization rejected; see DLSSVideoPlayer.log\n";
        renderer.reset();DestroyWindow(window);
    }
    MFShutdown();CoUninitialize();return code;
}

// The renderer's private state this probe has to reach: the hooks, to ask for DRED
// before the device exists; the device, to remove it and to read the debug layer's
// messages and DRED off it; the present flag, to make one Present refuse; and the
// frame ring, to see a refused frame's slot published all the same.
struct D3D12RendererTestAccess {
    static D3D12RendererTestHooks& Hooks(D3D12Renderer& r){
        if(!r.m_testHooks)r.m_testHooks=std::make_unique<D3D12RendererTestHooks>();
        return *r.m_testHooks;
    }
    static ID3D12Device* Device(D3D12Renderer& r){return r.m_device.Get();}
    // The tearing flag on a swapchain built without it makes Present answer
    // DXGI_ERROR_INVALID_CALL after the frame's command lists are already queued.
    static void PresentWithTearingFlag(D3D12Renderer& r,bool on){r.m_allowTearing=on;}
    static uint32_t FrameSlot(const D3D12Renderer& r){return r.m_frameSlot;}
    static uint64_t FenceValue(const D3D12Renderer& r){return r.m_fenceValue;}
    static uint64_t FrameFence(const D3D12Renderer& r,uint32_t slot){return r.m_frameFence[slot];}
    static constexpr uint32_t FrameCount=D3D12Renderer::FrameCount;
};

namespace {

// Every ERROR-or-worse message the debug layer stored for this device, printed, and
// the count of the ones that name an allocator or list reused while the GPU still
// had it - which is exactly what an unpublished frame slot produces.
struct DebugLayerReport{uint32_t errors=0,syncErrors=0;bool available=false;};
DebugLayerReport ReadDebugLayer(ID3D12Device* device,const char* stage){
    DebugLayerReport report;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;
    if(!device||FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))return report;
    report.available=true;
    const UINT64 stored=queue->GetNumStoredMessages();
    std::vector<char> buffer;
    for(UINT64 index=0;index<stored;++index){
        SIZE_T length=0;
        if(FAILED(queue->GetMessage(index,nullptr,&length))||!length)continue;
        buffer.resize(length);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if(FAILED(queue->GetMessage(index,message,&length)))continue;
        if(message->Severity>D3D12_MESSAGE_SEVERITY_ERROR)continue;
        ++report.errors;
        const bool sync=message->ID==D3D12_MESSAGE_ID_COMMAND_ALLOCATOR_SYNC||message->ID==D3D12_MESSAGE_ID_COMMAND_LIST_SYNC||
                        message->ID==D3D12_MESSAGE_ID_COMMAND_ALLOCATOR_CANNOT_RESET||message->ID==D3D12_MESSAGE_ID_OBJECT_DELETED_WHILE_STILL_IN_USE;
        if(sync)++report.syncErrors;
        std::cout<<"  debug-layer["<<stage<<"] id="<<int(message->ID)<<(sync?" SYNC ":" ")<<std::string_view(message->pDescription,message->DescriptionByteLength?message->DescriptionByteLength-1:0)<<"\n";
    }
    queue->ClearStoredMessages();
    return report;
}

// Every line of this process's log that carries `needle`. Log truncates the file at
// start-up, so nothing an earlier run wrote can answer for this one.
std::vector<std::string> LogLines(std::string_view needle){
    wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);
    std::ifstream log(std::filesystem::path(module).parent_path()/L"DLSSVideoPlayer.log",std::ios::binary);
    std::vector<std::string> lines;
    for(std::string line;std::getline(log,line);)if(line.find(needle)!=std::string::npos)lines.push_back(std::move(line));
    return lines;
}

std::string Hex(HRESULT value){std::ostringstream out;out<<std::hex<<value;return out.str();}

} // namespace

int RunDeviceLossProbe(const wchar_t* source,uint32_t targetHeight)
{
    // Process-wide, so it has to be on before the renderer creates its device. Without
    // the SDK layers installed the probe still checks the ring bookkeeping and the
    // classification; only the sync-error count is unavailable, and it says so.
    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    const bool debugLayer=SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if(debugLayer)debug->EnableDebugLayer();
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);MFStartup(MF_VERSION);
    using Access=D3D12RendererTestAccess;
    int code=1;
    {
        VideoDecoder decoder;
        if(!decoder.Open(source,MediaSourceKind::LocalFile))return 3;
        const auto target=UpscalingTarget(decoder.Width(),decoder.Height(),targetHeight);
        if(!target.grows)return 4;
        const auto [gw,gh]=TemporalGuideGenerator::AnalysisGrid(decoder.Width(),decoder.Height(),decoder.FrameRate());
        const float frameMs=float(1000/decoder.FrameRate());
        TemporalGuideGenerator guides;VideoFrame frame;uint32_t rendered=0;
        auto render=[&](D3D12Renderer& renderer){
            GuideFrame guide;
            const FrameIdentity id=IdentityOf(frame,guides.HistoryGeneration(),0,rendered==0?HistoryReset::FirstFrame:HistoryReset::None);
            const bool ok=guides.Generate(frame.bgra.data(),frame.bgra.size(),decoder.Width(),decoder.Height(),decoder.Width(),decoder.Height(),decoder.FrameRate(),id,guide)&&
                renderer.RenderFrame(frame.bgra.data(),frame.bgra.size(),id,guide,frameMs);
            if(ok)++rendered;
            return ok;
        };
        auto makeRenderer=[&](HWND window,D3D12RendererOwner& renderer){
            renderer=MakeD3D12Renderer();
            Access::Hooks(*renderer).dred=true;
            return renderer->Initialize(window,decoder.Width(),decoder.Height(),target.width,target.height,gw,gh,
                NVSDK_NGX_PerfQuality_Value_MaxQuality,true)&&renderer->DLSSAvailable();
        };

        // 1. A refused Present. Two good frames first, then one whose Present answers
        //    DXGI_ERROR_INVALID_CALL after its lists are queued: the frame fails, the
        //    device stays usable, and the slot is published - fence recorded, ring
        //    advanced - exactly as if the Present had succeeded. The same for the
        //    static present. Then enough frames to wrap the ring past both slots, so
        //    a slot that had NOT been published would be reused here and the debug
        //    layer would see its allocator reset under a list it still tracks.
        HWND window=CreateWindowExW(0,L"STATIC",L"device-loss probe",WS_POPUP,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        D3D12RendererOwner renderer;
        bool ok=makeRenderer(window,renderer);
        if(!ok){std::cout<<"SR initialization rejected; see DLSSVideoPlayer.log\n";renderer.reset();DestroyWindow(window);MFShutdown();CoUninitialize();return 5;}
        for(uint32_t i=0;ok&&i<2;++i)ok=decoder.ReadNext(frame)&&render(*renderer);
        const uint32_t slotBefore=Access::FrameSlot(*renderer);const uint64_t fenceBefore=Access::FenceValue(*renderer);
        Access::PresentWithTearingFlag(*renderer,true);
        const bool presentRefused=ok&&decoder.ReadNext(frame)&&!render(*renderer);
        Access::PresentWithTearingFlag(*renderer,false);
        const bool framePublished=Access::FrameSlot(*renderer)==(slotBefore+1)%Access::FrameCount&&
            Access::FenceValue(*renderer)==fenceBefore+1&&Access::FrameFence(*renderer,slotBefore)==fenceBefore+1&&!renderer->GpuUnusable();
        const uint32_t staticSlot=Access::FrameSlot(*renderer);const uint64_t staticFence=Access::FenceValue(*renderer);
        Access::PresentWithTearingFlag(*renderer,true);
        const bool staticRefused=!renderer->PresentCurrent();
        Access::PresentWithTearingFlag(*renderer,false);
        const bool staticPublished=Access::FrameSlot(*renderer)==(staticSlot+1)%Access::FrameCount&&
            Access::FenceValue(*renderer)==staticFence+1&&Access::FrameFence(*renderer,staticSlot)==staticFence+1&&!renderer->GpuUnusable();
        uint32_t wrapped=0;
        while(ok&&wrapped<Access::FrameCount+2&&decoder.ReadNext(frame)){ok=render(*renderer);if(ok)++wrapped;}
        const bool stillUsable=ok&&!renderer->GpuUnusable()&&renderer->PresentCurrent();
        const DebugLayerReport afterRefusal=ReadDebugLayer(Access::Device(*renderer),"refused-present");
        renderer.reset();
        std::cout<<"refused-present: presentRefused="<<presentRefused<<" framePublished="<<framePublished
            <<" staticRefused="<<staticRefused<<" staticPublished="<<staticPublished
            <<" wrapped="<<wrapped<<" stillUsable="<<stillUsable
            <<" debugLayer="<<(afterRefusal.available?"on":"off")<<" errors="<<afterRefusal.errors<<" syncErrors="<<afterRefusal.syncErrors<<"\n";

        // 2. A removed device. A fresh renderer, two frames, then RemoveDevice between
        //    frames. Fewer than FrameCount frames have gone through, so the next slot
        //    has no fence to wait on and the frame reaches its allocator Reset, Close
        //    and Present on the dead device - where the classification has to happen.
        //    The renderer must come out latched as DeviceRemoved, log the device's own
        //    reason, refuse further work up front, and still tear down. DRED, which the
        //    hook turned on before the device existed, has to answer this device (S_OK,
        //    not DXGI_ERROR_UNSUPPORTED) and the renderer has to have written what it
        //    said. An explicit removal never carries breadcrumbs: the runtime links them
        //    only for work still outstanding, and RemoveDevice signals every fence first
        //    - a frame parked behind an unsignalled fence comes out empty too - so the
        //    line to expect is the "enabled, nothing outstanding" one. A GPU fault would
        //    be the only way to see per-list breadcrumbs, and it is not provoked here.
        bool lossOk=makeRenderer(window,renderer);
        rendered=0;guides.Reset();
        for(uint32_t i=0;lossOk&&i<2;++i)lossOk=decoder.ReadNext(frame)&&render(*renderer);
        Microsoft::WRL::ComPtr<ID3D12Device5> device5;
        const bool removable=lossOk&&SUCCEEDED(Access::Device(*renderer)->QueryInterface(IID_PPV_ARGS(&device5)));
        if(removable)device5->RemoveDevice();
        const HRESULT reason=removable?Access::Device(*renderer)->GetDeviceRemovedReason():S_OK;
        const bool lossRefused=removable&&!render(*renderer);
        const bool latched=renderer->GpuUnusable()&&renderer->LastFenceWaitResult()==d3d12_renderer_detail::FenceWaitResult::DeviceRemoved;
        const bool refusedUpFront=!render(*renderer)&&!renderer->PresentCurrent();
        const std::string expected="D3D12 device removed: reason=0x"+Hex(reason);
        const bool logged=!LogLines(expected).empty();
        Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dredData;
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
        const HRESULT dredHr=removable&&SUCCEEDED(Access::Device(*renderer)->QueryInterface(IID_PPV_ARGS(&dredData)))
            ?dredData->GetAutoBreadcrumbsOutput(&breadcrumbs):E_FAIL;
        const std::vector<std::string> dred=LogLines("DRED ");
        for(const std::string& line:dred)std::cout<<"  "<<line<<"\n";
        const bool dredLogged=!LogLines("DRED enabled;").empty();
        const DebugLayerReport afterLoss=ReadDebugLayer(Access::Device(*renderer),"device-removed");
        renderer.reset();DestroyWindow(window);
        std::cout<<"device-removed: removed="<<removable<<" reason=0x"<<Hex(reason)<<" renderRefused="<<lossRefused
            <<" gpuUnusable="<<latched<<" fenceWait="<<int(d3d12_renderer_detail::FenceWaitResult::DeviceRemoved)
            <<" refusedUpFront="<<refusedUpFront<<" logged=\""<<expected<<"\"="<<logged
            <<" dredHr=0x"<<Hex(dredHr)<<" dredNodes="<<(breadcrumbs.pHeadAutoBreadcrumbNode?1:0)<<" dredLines="<<dred.size()<<" dredEnabledLogged="<<dredLogged
            <<" errors="<<afterLoss.errors<<" syncErrors="<<afterLoss.syncErrors<<" tornDown=1\n";
        // On the live device nothing the debug layer calls an error is tolerated; once
        // the device is gone only the reuse-under-the-GPU class is held against it.
        code=presentRefused&&framePublished&&staticRefused&&staticPublished&&wrapped==Access::FrameCount+2&&stillUsable&&
             afterRefusal.errors==0&&removable&&FAILED(reason)&&lossRefused&&latched&&refusedUpFront&&logged&&
             SUCCEEDED(dredHr)&&dredLogged&&afterLoss.syncErrors==0?0:6;
    }
    MFShutdown();CoUninitialize();return code;
}

int RunHdrOutputProbe(const wchar_t* source)
{
    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    const bool debugLayer=SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if(debugLayer)debug->EnableDebugLayer();
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);MFStartup(MF_VERSION);
    using Access=D3D12RendererTestAccess;
    int code=1;
    {
        // The same source twice: as the model sees it and as an HDR display shows it.
        VideoDecoder sdr,pq;
        pq.SetHdrPresentation(true);
        if(!sdr.Open(source,MediaSourceKind::LocalFile)||!pq.Open(source,MediaSourceKind::LocalFile))return 3;
        if(sdr.SourceHdrSignal()==hdr_policy::HdrSignal::Sdr){std::cout<<"not an HDR source\n";return 4;}
        const uint32_t w=sdr.Width(),h=sdr.Height();
        const auto [gw,gh]=TemporalGuideGenerator::AnalysisGrid(w,h,sdr.FrameRate());
        HWND window=CreateWindowExW(0,L"STATIC",L"hdr output probe",WS_POPUP,0,0,640,360,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        auto renderer=MakeD3D12Renderer();
        renderer->SetPresentFollowsWindow(true);
        renderer->SetHdrOutputAllowed(true);
        bool ok=renderer->Initialize(window,w,h,w,h,gw,gh,DefaultNeuralCarrierQuality());
        if(ok)renderer->SetDLSS(false);
        const D3D12Renderer::DisplayHdrState display=ok?renderer->QueryDisplayHdr():D3D12Renderer::DisplayHdrState{};
        // Forced, whatever the display is: the path has to hold up on any output.
        const bool hdrOn=ok&&renderer->SetHdrOutput(true,203.0f)&&renderer->HdrOutputActive();
        VideoFrame sdrFrame,pqFrame;uint32_t frames=0;
        const float frameMs=float(1000.0/sdr.FrameRate());
        const auto render=[&](const VideoFrame& frame){
            renderer->SetNextSourcePq(frame.pq);
            return renderer->RenderFrame(frame.bgra.data(),frame.bgra.size(),nullptr,0,gw,gh,frames==0,false,frameMs);
        };
        // The original as HDR through the frame path, and as the reference beside the
        // tone-mapped frame the model would see.
        for(uint32_t i=0;ok&&hdrOn&&i<8;++i){
            ok=sdr.ReadNext(sdrFrame)&&pq.ReadNext(pqFrame)&&pqFrame.pq&&!sdrFrame.pq&&
               renderer->UploadReferenceFrame(pqFrame.bgra.data(),pqFrame.bgra.size(),true)&&
               render(i%2?pqFrame:sdrFrame);
            if(ok)++frames;
        }
        // A subtitle canvas over every mode below: on the HDR backbuffer it is the
        // last layer, at SDR white. Half-transparent white text-sized band.
        bool subtitles=false;
        if(ok&&hdrOn){
            const uint32_t sw=640,sh=360;std::vector<uint8_t> canvas(size_t(sw)*sh*4u,0);
            for(uint32_t y=300;y<330;++y)for(uint32_t x=100;x<540;++x){uint8_t* p=&canvas[(size_t(y)*sw+x)*4u];p[0]=p[1]=p[2]=p[3]=200;}
            subtitles=renderer->SetSubtitleOverlay(canvas.data(),sw,sh);
            ok=subtitles;
        }
        static constexpr ComparisonMode modes[]={ComparisonMode::Neural,ComparisonMode::Original,ComparisonMode::SplitVertical,
            ComparisonMode::Wipe,ComparisonMode::Difference,ComparisonMode::SideBySide,ComparisonMode::Quad};
        uint32_t presented=0;
        for(const ComparisonMode mode:modes){
            if(!ok||!hdrOn)break;
            ComparisonSettings cmp;cmp.mode=mode;cmp.splitX=0.5f;cmp.loupe=mode==ComparisonMode::SplitVertical;
            cmp.loupeRadius=40.0f;cmp.loupeLeftX=100.0f;cmp.loupeLeftY=100.0f;cmp.loupeRightX=300.0f;cmp.loupeRightY=100.0f;
            renderer->SetComparison(cmp);
            ok=renderer->PresentCurrent()&&!renderer->GpuUnusable();
            if(ok)++presented;
        }
        renderer->SetComparison({});
        // The debug views draw guides, so each is shown on a frame rendered with one:
        // presenting a guide view before any guided frame reads textures that were
        // never drawn, which the debug layer reports whatever the swapchain is.
        uint32_t debugViews=0;TemporalGuideGenerator guides;
        for(const auto view:{D3D12Renderer::DebugView::MotionVectors,D3D12Renderer::DebugView::Depth,D3D12Renderer::DebugView::Final}){
            if(!ok||!hdrOn)break;
            renderer->SetDebugView(view);
            if(view!=D3D12Renderer::DebugView::Final){
                GuideFrame guide;
                const FrameIdentity id=IdentityOf(sdrFrame,guides.HistoryGeneration(),0,HistoryReset::FirstFrame);
                ok=guides.Generate(sdrFrame.bgra.data(),sdrFrame.bgra.size(),w,h,w,h,sdr.FrameRate(),id,guide)&&
                   renderer->RenderFrame(sdrFrame.bgra.data(),sdrFrame.bgra.size(),id,guide,frameMs);
            }
            ok=ok&&renderer->PresentCurrent()&&!renderer->GpuUnusable();
            if(ok)++debugViews;
        }
        // The saved comparison stays 8-bit sRGB on an HDR display: an HDR original in
        // it is the Original view, read back through the SDR compositor.
        std::vector<uint8_t> composed;uint32_t cw=0,ch=0;uint64_t brightness=0;
        if(ok&&hdrOn){
            ComparisonSettings original;original.mode=ComparisonMode::Original;renderer->SetComparison(original);
            ok=renderer->PresentCurrent()&&renderer->CaptureComposedView(composed,cw,ch);
            for(size_t i=0;i+3<composed.size();i+=4)brightness+=composed[i];
            renderer->SetComparison({});
        }
        const DebugLayerReport afterHdr=ReadDebugLayer(Access::Device(*renderer),"hdr-output");
        // And back: the SDR swapchain presents as it did before any of this.
        const bool hdrOff=ok&&renderer->SetHdrOutput(false,203.0f)&&!renderer->HdrOutputActive();
        const bool sdrAgain=hdrOff&&sdr.ReadNext(sdrFrame)&&render(sdrFrame)&&renderer->PresentCurrent()&&!renderer->GpuUnusable();
        const DebugLayerReport afterSdr=ReadDebugLayer(Access::Device(*renderer),"sdr-again");
        std::cout<<"display: known="<<display.known<<" hdr="<<display.hdr<<" sdrWhite="<<display.sdrWhiteNits
            <<" peak="<<display.maxLuminanceNits<<" device="<<utf8_text::FromWide(display.device)<<"\n"
            <<"hdr-output: forcedOn="<<hdrOn<<" frames="<<frames<<" subtitles="<<subtitles<<" comparisonModes="<<presented<<" debugViews="<<debugViews
            <<" composed="<<cw<<"x"<<ch<<" meanBlue="<<(composed.empty()?0:brightness/(composed.size()/4))
            <<" backToSdr="<<hdrOff<<" sdrAgain="<<sdrAgain
            <<" debugLayer="<<(debugLayer?"on":"off")<<" errors="<<afterHdr.errors+afterSdr.errors<<"\n";
        // A display that refuses the ST 2084 colour space is an answer, not a failure,
        // as long as the renderer stayed in SDR and still presents.
        if(ok&&!hdrOn){
            std::cout<<"This output refused the ST 2084 colour space; checking that SDR still presents.\n";
            code=!renderer->HdrOutputActive()&&sdr.ReadNext(sdrFrame)&&render(sdrFrame)&&renderer->PresentCurrent()?0:7;
        }else{
            code=ok&&hdrOn&&frames==8&&subtitles&&presented==std::size(modes)&&debugViews==3&&cw>0&&ch>0&&brightness>0&&
                 hdrOff&&sdrAgain&&afterHdr.errors==0&&afterSdr.errors==0?0:6;
        }
        renderer.reset();DestroyWindow(window);
    }
    MFShutdown();CoUninitialize();return code;
}

namespace {

// Runs one FFmpeg child in `directory` and waits for it. The libvmaf log is
// named relative to that folder because a drive colon inside a filter argument
// needs an escape the filter parser and the command line disagree about.
bool RunFfmpeg(const std::filesystem::path& ffmpegDirectory,const std::filesystem::path& directory,const std::wstring& arguments)
{
    const std::filesystem::path exe=ffmpegDirectory/L"ffmpeg.exe";
    std::wstring command=L"\""+exe.wstring()+L"\" -hide_banner -nostdin -loglevel error -y "+arguments;
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,
                       directory.c_str(),&startup,&process))return false;
    CloseHandle(process.hThread);
    const bool finished=WaitForSingleObject(process.hProcess,15u*60u*1000u)==WAIT_OBJECT_0;
    DWORD code=1;
    if(finished)GetExitCodeProcess(process.hProcess,&code);else TerminateProcess(process.hProcess,1);
    CloseHandle(process.hProcess);
    if(!finished||code!=0)std::wcout<<L"ffmpeg failed: "<<arguments<<L"\n";
    return finished&&code==0;
}

// Every per-frame score in a libvmaf JSON log: each frame's metrics hold
// `"vmaf": <number>`, and the pooled block writes `"vmaf": {` instead.
std::vector<double> ReadVmafFrames(const std::filesystem::path& log)
{
    std::ifstream file(log,std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(file),{}};
    std::vector<double> frames;
    for(size_t at=text.find("\"vmaf\": ");at!=std::string::npos;at=text.find("\"vmaf\": ",at+1)){
        const char* begin=text.c_str()+at+8;
        if(*begin=='{')continue;
        char* end=nullptr;const double value=std::strtod(begin,&end);
        if(end!=begin)frames.push_back(value);
    }
    return frames;
}

double MeanOf(const std::vector<double>& values,size_t from,size_t to)
{
    to=std::min(to,values.size());
    if(from>=to)return 0.0;
    double sum=0.0;for(size_t i=from;i<to;++i)sum+=values[i];
    return sum/double(to-from);
}

// The clip through the real Super Resolution path exactly as an export's SR
// stage runs it - preserve-source, the capture ring, guides from the frames -
// written raw for scoring. False when any frame did not come through DLSS.
bool RenderSuperResolution(const std::filesystem::path& source,const std::filesystem::path& raw,uint32_t& frames,
                           UpscalingHistory history=kDefaultUpscalingHistory)
{
    frames=0;
    VideoDecoder decoder;
    if(!decoder.Open(source.c_str(),MediaSourceKind::LocalFile))return false;
    const auto target=UpscalingTarget(decoder.Width(),decoder.Height(),1080);
    if(!target.grows)return false;
    HWND window=CreateWindowExW(0,L"STATIC",L"sr quality",WS_POPUP,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    bool ok=false;
    {
        auto renderer=MakeD3D12Renderer();
        const auto [gw,gh]=TemporalGuideGenerator::AnalysisGrid(decoder.Width(),decoder.Height(),decoder.FrameRate());
        if(renderer->Initialize(window,decoder.Width(),decoder.Height(),target.width,target.height,gw,gh,
               NVSDK_NGX_PerfQuality_Value_MaxQuality,true,true)&&renderer->DLSSAvailable()&&
           renderer->OutputW()==target.width&&renderer->OutputH()==target.height){
            TemporalGuideGenerator guides;VideoFrame frame;ok=true;
            renderer->SetUpscalingHistory(history);
            std::ofstream out(raw,std::ios::binary|std::ios::trunc);
            const float frameMs=float(1000.0/decoder.FrameRate());
            while(ok&&decoder.ReadNext(frame)){
                GuideFrame guide;CapturedVideoFrame captured;
                const FrameIdentity id=IdentityOf(frame,guides.HistoryGeneration(),0,frames==0?HistoryReset::FirstFrame:HistoryReset::None);
                ok=guides.Generate(frame.bgra.data(),frame.bgra.size(),decoder.Width(),decoder.Height(),decoder.Width(),decoder.Height(),
                                   decoder.FrameRate(),id,guide,frame.layout)&&
                   renderer->RenderFrameForCache(frame.bgra.data(),frame.bgra.size(),id,guide,frameMs,captured)&&
                   captured.pixels.size()==size_t(target.width)*target.height*4u;
                if(ok){out.write(reinterpret_cast<const char*>(captured.pixels.data()),std::streamsize(captured.pixels.size()));++frames;}
            }
            ok=ok&&frames>0&&renderer->DLSSEvaluations()==frames;
        }else std::cout<<"SR initialization rejected; see DLSSVideoPlayer.log\n";
    }
    DestroyWindow(window);
    return ok;
}

} // namespace

int RunSrQualityProbe(const wchar_t* ffmpegDirectory,const wchar_t* workDirectory)
{
    const std::filesystem::path ffmpeg(ffmpegDirectory),work(workDirectory);
    std::error_code error;std::filesystem::create_directories(work,error);
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);MFStartup(MF_VERSION);
    const std::wstring tagged=L" -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv -c:v ffv1 ";
    const std::wstring bt709=L"setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv";
    // A moving clip and its first frame held for two seconds, each area-reduced to
    // 960x540, which is what SR and bicubic are both handed.
    const std::wstring generated=kSrQualitySource;
    struct Clip{const wchar_t* name;std::wstring source;};
    const Clip clips[]={
        {L"moving",L"-f lavfi -i "+generated+L" -frames:v 90 -vf "+bt709},
        {L"still",L"-f lavfi -i "+generated+L" -vf select=eq(n\\,0),loop=loop=59:size=1:start=0,setpts=N/30/TB,"+bt709+L" -frames:v 60"},
    };
    bool ok=true;
    struct Result{std::vector<double> sr,bicubic;};
    std::vector<Result> results;
    // Both upscales are scored from BGRA, the way the capture hands SR's over: the
    // BGRA round trip alone costs about 3 VMAF on dark footage, and bicubic scored
    // straight from YUV would be given that for free.
    const std::wstring score=L"[d];[1:v]format=yuv420p,setpts=N/30/TB[r];[d][r]libvmaf=n_threads=8:log_fmt=json:log_path=";
    const std::wstring fromBgra=L" -lavfi [0:v]scale=out_color_matrix=bt709:out_range=tv,format=yuv420p,setpts=N/30/TB";
    for(const Clip& clip:clips){
        const std::wstring name(clip.name);
        const std::wstring ref=name+L"-ref.mkv",reduced=name+L"-540.mkv",bicubic=name+L"-bicubic.raw",raw=name+L"-sr.raw";
        ok=ok&&RunFfmpeg(ffmpeg,work,clip.source+tagged+ref)&&
           RunFfmpeg(ffmpeg,work,L"-i "+ref+L" -vf scale=960:540:flags=area,"+bt709+tagged+reduced)&&
           RunFfmpeg(ffmpeg,work,L"-i "+reduced+L" -vf scale=in_color_matrix=bt709:in_range=tv,format=bgra,"
                                 L"scale=1920:1080:flags=bicubic -f rawvideo "+bicubic);
        uint32_t frames=0;
        ok=ok&&RenderSuperResolution(work/reduced,work/raw,frames);
        for(const auto& [input,log]:{std::pair{raw,name+L"-sr.json"},std::pair{bicubic,name+L"-bicubic.json"}})
            ok=ok&&RunFfmpeg(ffmpeg,work,L"-f rawvideo -pix_fmt bgra -s 1920x1080 -r 30 -i "+input+L" -i "+ref+
                                           fromBgra+score+log+L" -f null -");
        Result result{ReadVmafFrames(work/(name+L"-sr.json")),ReadVmafFrames(work/(name+L"-bicubic.json"))};
        ok=ok&&result.sr.size()==frames&&result.bicubic.size()==frames&&frames>=20;
        std::filesystem::remove(work/raw,error);std::filesystem::remove(work/bicubic,error);
        results.push_back(std::move(result));
        if(!ok)break;
    }
    // Per-frame history (UpscalingPolicy.h) resets the evaluate on every frame, so a
    // held frame is the same single-frame upscale sixty times over: every captured
    // frame must be byte-identical to the first. A per-frame choice that never reached
    // the evaluate would accumulate instead, and the frames would drift apart.
    bool perFrameHeld=false;
    if(ok){
        uint32_t frames=0;
        const std::filesystem::path held=work/L"still-sr-per-frame.raw";
        if(RenderSuperResolution(work/L"still-540.mkv",held,frames,UpscalingHistory::PerFrame)&&frames>=20){
            std::ifstream in(held,std::ios::binary);
            const size_t frameBytes=size_t(1920)*1080*4u;
            std::vector<char> first(frameBytes),next(frameBytes);
            perFrameHeld=bool(in.read(first.data(),std::streamsize(frameBytes)));
            uint32_t same=perFrameHeld?1u:0u;
            while(perFrameHeld&&in.read(next.data(),std::streamsize(frameBytes))){
                if(next!=first){perFrameHeld=false;break;}
                ++same;
            }
            perFrameHeld=perFrameHeld&&same==frames;
            std::cout<<"sr-quality: per-frame held frame "<<(perFrameHeld?"identical":"DIFFERS")<<" across "<<frames<<" frames\n";
        }
        std::filesystem::remove(held,error);
    }
    int code=8;
    if(ok&&results.size()==2){
        const Result& moving=results[0];const Result& still=results[1];
        const double movingSr=MeanOf(moving.sr,0,moving.sr.size()),movingBicubic=MeanOf(moving.bicubic,0,moving.bicubic.size());
        const double stillFirst=MeanOf(still.sr,0,10),stillLast=MeanOf(still.sr,still.sr.size()-10,still.sr.size());
        const double stillBicubic=MeanOf(still.bicubic,0,still.bicubic.size());
        std::cout<<std::fixed<<std::setprecision(2)<<"sr-quality: moving SR="<<movingSr<<" bicubic="<<movingBicubic
            <<" | still SR first10="<<stillFirst<<" last10="<<stillLast<<" bicubic="<<stillBicubic<<"\n";
        // A held frame must hold its score. With nothing moving there is nothing new
        // to accumulate, so the last ten frames may only match or beat the first ten;
        // the engine's field on identical frames took them down by more than ten VMAF.
        code=stillLast>=stillFirst-1.0?(perFrameHeld?0:10):9;
    }
    MFShutdown();CoUninitialize();return code;
}
