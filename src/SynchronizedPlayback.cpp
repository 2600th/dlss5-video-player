#include "SynchronizedPlayback.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace {

class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual bool Open(const std::filesystem::path&, std::stop_token) = 0;
    virtual void Close() = 0;
    virtual VideoReadResult Read(VideoFrame&, std::stop_token) = 0;
    virtual bool SeekSeconds(double) = 0;
    virtual uint32_t Width() const = 0;
    virtual uint32_t Height() const = 0;
    virtual double FrameRate() const = 0;
    virtual double DurationSeconds() const = 0;
};

#ifdef SYNCHRONIZED_PLAYBACK_TESTING
class TestFrameSource final : public FrameSource {
public:
    explicit TestFrameSource(ISynchronizedFrameSource& source) : source_(source) {}
    bool Open(const std::filesystem::path& path,std::stop_token stop)override{return source_.Open(path,stop);}
    void Close()override{source_.Close();}
    VideoReadResult Read(VideoFrame& frame,std::stop_token stop)override{return source_.Read(frame,stop);}
    bool SeekSeconds(double seconds)override{return source_.SeekSeconds(seconds);}
    uint32_t Width()const override{return source_.Width();}
    uint32_t Height()const override{return source_.Height();}
    double FrameRate()const override{return source_.FrameRate();}
    double DurationSeconds()const override{return source_.DurationSeconds();}
private:
    ISynchronizedFrameSource& source_;
};
#else
class DecoderFrameSource final : public FrameSource {
public:
    bool Open(const std::filesystem::path& path,std::stop_token stop)override{
        return decoder_.Open(path.wstring(),MediaSourceKind::LocalFile,stop);
    }
    void Close()override{decoder_.Close();}
    VideoReadResult Read(VideoFrame& frame,std::stop_token stop)override{
        return decoder_.ReadNextAvailable(frame,stop);
    }
    bool SeekSeconds(double seconds)override{return decoder_.SeekSeconds(seconds);}
    uint32_t Width()const override{return decoder_.Width();}
    uint32_t Height()const override{return decoder_.Height();}
    double FrameRate()const override{return decoder_.FrameRate();}
    double DurationSeconds()const override{return decoder_.DurationSeconds();}
private:
    VideoDecoder decoder_;
};
#endif

SynchronizedReadResult ConvertRead(VideoReadResult result)
{
    switch(result){
        case VideoReadResult::FrameReady:return SynchronizedReadResult::PairReady;
        case VideoReadResult::NotReady:return SynchronizedReadResult::NotReady;
        case VideoReadResult::EndOfStream:return SynchronizedReadResult::EndOfStream;
        case VideoReadResult::Cancelled:return SynchronizedReadResult::Cancelled;
        default:return SynchronizedReadResult::Error;
    }
}

} // namespace

struct SynchronizedPlayback::Impl {
    std::unique_ptr<FrameSource> original;
    std::unique_ptr<FrameSource> neural;
    std::optional<VideoFrame> pendingOriginal;
    std::optional<VideoFrame> pendingNeural;
    std::optional<SynchronizedFramePair> current;
    ComparisonView view{ComparisonView::Original};
    bool opened{};
    bool paused{};
    bool stepRequested{};
    int64_t tolerance100ns{333334};

    void ResetPublished()
    {
        pendingOriginal.reset();pendingNeural.reset();current.reset();
        view=ComparisonView::Original;paused=false;stepRequested=false;
    }

    SynchronizedReadResult ReadOne(FrameSource& source,std::optional<VideoFrame>& pending,
                                   std::stop_token stop)
    {
        if(pending)return SynchronizedReadResult::PairReady;
        VideoFrame frame;const VideoReadResult read=source.Read(frame,stop);
        if(read==VideoReadResult::FrameReady){pending=std::move(frame);return SynchronizedReadResult::PairReady;}
        return ConvertRead(read);
    }

    SynchronizedReadResult BuildPair(SynchronizedFramePair& pair,std::stop_token stop)
    {
        if(!original)return SynchronizedReadResult::Error;
        for(size_t guard=0;guard<4096;++guard){
            const auto originalRead=ReadOne(*original,pendingOriginal,stop);
            if(originalRead!=SynchronizedReadResult::PairReady){
                if(!neural||originalRead!=SynchronizedReadResult::EndOfStream)return originalRead;
                if(pendingNeural)return SynchronizedReadResult::OutOfSync;
                VideoFrame extra;const auto neuralRead=neural->Read(extra,stop);
                if(neuralRead==VideoReadResult::EndOfStream)return SynchronizedReadResult::EndOfStream;
                if(neuralRead==VideoReadResult::NotReady)return SynchronizedReadResult::NotReady;
                if(neuralRead==VideoReadResult::Cancelled)return SynchronizedReadResult::Cancelled;
                if(neuralRead==VideoReadResult::FrameReady)return SynchronizedReadResult::OutOfSync;
                return SynchronizedReadResult::Error;
            }
            if(!neural){
                pair.original=std::move(*pendingOriginal);pair.neural={};
                pair.timestamp100ns=pair.original.timestamp100ns;pendingOriginal.reset();return SynchronizedReadResult::PairReady;
            }
            const auto neuralRead=ReadOne(*neural,pendingNeural,stop);
            if(neuralRead!=SynchronizedReadResult::PairReady){
                if(neuralRead==SynchronizedReadResult::EndOfStream)return SynchronizedReadResult::OutOfSync;
                return neuralRead;
            }
            const int64_t difference=pendingOriginal->timestamp100ns-pendingNeural->timestamp100ns;
            if(std::llabs(difference)<=tolerance100ns){
                pair.original=std::move(*pendingOriginal);pair.neural=std::move(*pendingNeural);
                pair.timestamp100ns=pair.original.timestamp100ns;
                pendingOriginal.reset();pendingNeural.reset();return SynchronizedReadResult::PairReady;
            }
            if(difference<0)pendingOriginal.reset();else pendingNeural.reset();
        }
        return SynchronizedReadResult::OutOfSync;
    }
};

SynchronizedPlayback::SynchronizedPlayback() : impl_(std::make_unique<Impl>())
{
#ifndef SYNCHRONIZED_PLAYBACK_TESTING
    impl_->original=std::make_unique<DecoderFrameSource>();
#endif
}

#ifdef SYNCHRONIZED_PLAYBACK_TESTING
SynchronizedPlayback::SynchronizedPlayback(ISynchronizedFrameSource& original,
                                             ISynchronizedFrameSource& neural)
    : impl_(std::make_unique<Impl>())
{
    impl_->original=std::make_unique<TestFrameSource>(original);
    impl_->neural=std::make_unique<TestFrameSource>(neural);
}
#endif

SynchronizedPlayback::~SynchronizedPlayback(){Close();}
SynchronizedPlayback::SynchronizedPlayback(SynchronizedPlayback&&) noexcept=default;
SynchronizedPlayback& SynchronizedPlayback::operator=(SynchronizedPlayback&&) noexcept=default;

bool SynchronizedPlayback::Open(const std::filesystem::path& originalPath,
                                const std::filesystem::path& neuralPath,std::stop_token stop)
{
    Close();if(!impl_->original||originalPath.empty())return false;
    if(!impl_->original->Open(originalPath,stop))return false;
    const double originalFps=impl_->original->FrameRate();
    if(!impl_->original->Width()||!impl_->original->Height()||
       !std::isfinite(originalFps)||originalFps<=0.0){
        impl_->original->Close();return false;
    }
    const bool useNeural=!neuralPath.empty();
#ifndef SYNCHRONIZED_PLAYBACK_TESTING
    if(useNeural)impl_->neural=std::make_unique<DecoderFrameSource>();
#endif
    if(useNeural){
        if(!impl_->neural||!impl_->neural->Open(neuralPath,stop)){impl_->original->Close();return false;}
        const double fps=originalFps;
        const double neuralFps=impl_->neural->FrameRate();
        const double durationTolerance=fps>0.0?1.0/fps:0.0;
        if(impl_->original->Width()!=impl_->neural->Width()||
           impl_->original->Height()!=impl_->neural->Height()||
           !std::isfinite(fps)||fps<=0.0||!std::isfinite(neuralFps)||
           std::abs(fps-neuralFps)>0.01||
           std::abs(impl_->original->DurationSeconds()-impl_->neural->DurationSeconds())>
                durationTolerance+1e-6){
            impl_->neural->Close();impl_->original->Close();return false;
        }
        impl_->tolerance100ns=static_cast<int64_t>(std::ceil(10000000.0/fps));
    }else{
#ifdef SYNCHRONIZED_PLAYBACK_TESTING
        impl_->neural.reset();
#else
        impl_->neural.reset();
#endif
        const double fps=originalFps;
        if(std::isfinite(fps)&&fps>0.0)
            impl_->tolerance100ns=static_cast<int64_t>(std::ceil(10000000.0/fps));
    }
    impl_->ResetPublished();impl_->opened=true;return true;
}

void SynchronizedPlayback::Close()
{
    if(!impl_)return;
    if(impl_->original)impl_->original->Close();if(impl_->neural)impl_->neural->Close();
    impl_->opened=false;impl_->ResetPublished();
}

SynchronizedReadResult SynchronizedPlayback::ReadNextAvailable(std::stop_token stop)
{
    if(!impl_->opened)return SynchronizedReadResult::Error;
    if(impl_->paused&&!impl_->stepRequested)return SynchronizedReadResult::NotReady;
    SynchronizedFramePair pair;const auto result=impl_->BuildPair(pair,stop);
    if(result==SynchronizedReadResult::PairReady)impl_->current=std::move(pair);
    if(impl_->stepRequested&&result!=SynchronizedReadResult::NotReady)impl_->stepRequested=false;
    return result;
}

bool SynchronizedPlayback::SeekSeconds(double seconds,std::stop_token stop)
{
    if(!impl_->opened||!std::isfinite(seconds)||seconds<0.0||stop.stop_requested())return false;
    if(!impl_->original->SeekSeconds(seconds))return false;
    if(impl_->neural&&!impl_->neural->SeekSeconds(seconds)){Close();return false;}
    impl_->pendingOriginal.reset();impl_->pendingNeural.reset();
    SynchronizedFramePair candidate;
    if(impl_->BuildPair(candidate,stop)!=SynchronizedReadResult::PairReady){
        Close();return false;
    }
    impl_->current=std::move(candidate);impl_->stepRequested=false;return true;
}

bool SynchronizedPlayback::SetView(ComparisonView view)
{
    if(!impl_->opened)return false;
    if(view==ComparisonView::Neural&&(!impl_->neural||!impl_->current))return false;
    impl_->view=view;return true;
}

ComparisonView SynchronizedPlayback::View()const{return impl_->view;}
const VideoFrame* SynchronizedPlayback::VisibleFrame()const
{
    if(!impl_->current)return nullptr;
    return impl_->view==ComparisonView::Neural?&impl_->current->neural:&impl_->current->original;
}
const SynchronizedFramePair* SynchronizedPlayback::CurrentPair()const
{return impl_->current?&*impl_->current:nullptr;}
void SynchronizedPlayback::SetPaused(bool paused){impl_->paused=paused;if(!paused)impl_->stepRequested=false;}
bool SynchronizedPlayback::Paused()const{return impl_->paused;}
bool SynchronizedPlayback::Step(){if(!impl_->opened||!impl_->paused)return false;impl_->stepRequested=true;return true;}
bool SynchronizedPlayback::NeuralAvailable()const{return impl_->opened&&impl_->neural!=nullptr;}
