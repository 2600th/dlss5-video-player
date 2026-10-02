#include "StageExport.h"

#include <windows.h>

#include <cwchar>
#include <string_view>
#include <system_error>

#include "FrameGenerationPass.h"
#include "Log.h"
#include "NeuralWorker.h"
#include "ReShadeConfig.h"
#include "RuntimeModulePolicy.h"
#include "Utf8Text.h"
#include "VsrUpscalePass.h"

const wchar_t* ExportRefusalKey(ExportRefusal refusal){
    switch(refusal){
    case ExportRefusal::NothingSelected:return L"export.stages.refusal.nothing";
    case ExportRefusal::SourceGeometryUnknown:return L"export.stages.refusal.geometry";
    case ExportRefusal::AlreadyAtTarget:return L"export.stages.refusal.target";
    case ExportRefusal::MultiplierUnsupported:return L"export.stages.refusal.multiplier";
    case ExportRefusal::VsrUnavailable:return L"export.stages.refusal.vsr";
    case ExportRefusal::StillImage:return L"export.stages.refusal.still";
    case ExportRefusal::None:break;
    }
    return L"export.stages.refusal.nothing";
}

std::wstring StageExportResultSummary(const ExportPlan& plan){
    wchar_t line[256];
    swprintf_s(line,L"%u × %u at %.4g fps · %u pass%s",plan.outputWidth,plan.outputHeight,
               plan.outputFps,ExportStageCount(plan),ExportStageCount(plan)==1?L"":L"es");
    return line;
}

std::wstring StageExportPlanSummary(const ExportPlan& plan,uint32_t sourceWidth,uint32_t sourceHeight,double sourceFps){
    wchar_t summary[256];
    swprintf_s(summary,L"%u x %u at %.4g fps -> %u x %u at %.4g fps, %u pass%s",sourceWidth,sourceHeight,sourceFps,
               plan.outputWidth,plan.outputHeight,plan.outputFps,ExportStageCount(plan),ExportStageCount(plan)==1?L"":L"es");
    return summary;
}

std::wstring StageExportEncodeSummary(const ExportPlan& plan,uint32_t sourceWidth,EncoderQuality quality,UpscalingHistory history){
    return L"encode: "+utf8_text::ToWide(std::string(EncoderQualityName(quality)))+
        (plan.vsrStage?L", upscaler RTX VSR":
         plan.outputWidth!=sourceWidth&&!plan.requireNeural?L", upscaler DLSS, history "+utf8_text::ToWide(std::string(UpscalingHistoryName(history))):std::wstring());
}

std::vector<NeuralAddonOverride> RenderAddonOverrides(const std::filesystem::path& ini,
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

std::wstring RuntimeLockRefusal(std::span<const RuntimeLockCheck> checks){
    if(RuntimeLockSatisfied(checks))return {};
    return L"The neural runtime does not match the locked stack: "+DescribeRuntimeLockDrift(checks);
}

std::wstring UnlockedRuntimeModulesRefusal(const std::filesystem::path& runtimeDirectory,const RuntimeLock& lock){
    const auto unlocked=FindUnlockedRuntimeModules(runtimeDirectory,lock);
    return unlocked?runtime_modules::UnlockedModulesRefusal(*unlocked):std::wstring{};
}

std::wstring StageExportRuntimeRefusal(const std::filesystem::path& runtimeDirectory,const RuntimeLock& lock,std::stop_token stop){
    const auto checks=VerifyRuntimeLock(runtimeDirectory,lock,stop);
    if(std::wstring refusal=RuntimeLockRefusal(checks);!refusal.empty())return refusal;
    return UnlockedRuntimeModulesRefusal(runtimeDirectory,lock);
}

StageExportOutcome RunStageExport(const StageExportJob& job,std::stop_token stop,
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
    if(plan.vsrStage){
        VsrUpscaleRequest request{};
        request.source=produced;request.output=stageOne;
        request.outputWidth=plan.outputWidth;request.outputHeight=plan.outputHeight;
        request.quality=job.vsrQuality;request.range=job.range;
        request.nvencPreset=job.nvencPreset;request.encode=job.quality;
        report({1,passes,L"export.progress.pass_vsr",0,0});
        const VsrUpscaleResult result=RunVsrUpscalePass(job.helpers,request,stop,
            [&](const VsrUpscaleProgress& p){report({1,passes,L"export.progress.pass_vsr",p.framesWritten,p.framesTotal});});
        if(!result.ok){
            sweep();
            return {result.cancelled?StageExportStatus::Cancelled:StageExportStatus::Failed,result.detail};
        }
        produced=stageOne;
    }
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
        // Refused on what refuses a live render, before anything is written:
        // a file that drifted from the lock, or a module the lock does not
        // name beside feature 18. A Super Resolution-only pass too (P1.33): it
        // is not presented as neural, but its helper loads the same proxy and
        // Streamline modules from this directory, and the helper's own check
        // of stray modules at startup came after the settings were written.
        {
            const std::wstring refusal=StageExportRuntimeRefusal(runtimeDirectory,job.runtimeLock?*job.runtimeLock:EmbeddedRuntimeLock(),stop);
            if(stop.stop_requested())return {StageExportStatus::Cancelled,{}};
            if(!refusal.empty()){
                LOG("Stage export refused before the helper: "<<utf8_text::FromWide(refusal));
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
            LOG("Stage export could not prepare the neural add-on: "<<utf8_text::FromWide(configured.error));
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
    finish.displayAspect=job.displayAspect;
    if(!job.range.Whole()){
        finish.rangeStartSeconds=double(job.range.start100ns)*1e-7;
        if(job.range.end100ns>job.range.start100ns)finish.rangeDurationSeconds=double(job.range.end100ns-job.range.start100ns)*1e-7;
    }
    report({});
    const MaterializeResult finished=MuxStageExport(job.helpers,finish,stop);
    sweep();
    if(!finished.ok){
        if(finished.error==MaterializeError::Cancelled)return {StageExportStatus::Cancelled,{}};
        LOG("Stage export could not write "<<utf8_text::FromWide(job.destination.wstring())<<": "<<utf8_text::FromWide(finished.detail));
        return {StageExportStatus::Failed,finished.detail};
    }
    LOG("Stage export wrote "<<utf8_text::FromWide(job.destination.wstring())<<(finished.detail.empty()?"":" - ")<<utf8_text::FromWide(finished.detail));
    // The note (what the container left out) goes to the dialog and to
    // --render's console, not only the log (P1.33).
    return {StageExportStatus::Done,finished.detail};
}
