// Opt-in hardware smoke test, not part of the portable CTest suite.
#include "D3D12Renderer.h"
#include "TemporalGuides.h"
#include "VideoDecoder.h"
#include "UpscalingPolicy.h"
#include <mfapi.h>
#include <chrono>
#include <iostream>

int wmain(int argc,wchar_t** argv) {
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
                ok=guides.Generate(frame.bgra.data(),decoder.Width(),decoder.Height(),decoder.Width(),decoder.Height(),decoder.FrameRate(),id,guide)&&
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
                rejected=guides.Generate(frame.bgra.data(),decoder.Width(),decoder.Height(),decoder.Width(),decoder.Height(),decoder.FrameRate(),other,stale)&&
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
            std::cout<<"source="<<decoder.Width()<<"x"<<decoder.Height()<<" output="<<target.width<<"x"<<target.height
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
