#include "SynchronizedPlayback.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <functional>
#include <future>
#include <system_error>
#include <optional>
#include <thread>
#include <utility>

namespace {

class DecoderFrameSource final : public ISynchronizedFrameSource {
public:
    bool Open(const std::filesystem::path& path,std::stop_token stop)override{
        return decoder_.Open(path.wstring(),MediaSourceKind::LocalFile,stop,preferNv12_);
    }
    bool OpenKnown(const std::filesystem::path& path,const VideoDecoder::KnownMedia& media,
                   std::stop_token stop)override{
        return decoder_.OpenKnown(path.wstring(),media,MediaSourceKind::LocalFile,stop,preferNv12_);
    }
    PixelLayout Layout()const override{return decoder_.PixelLayout();}
    void PreferNv12(bool prefer)override{preferNv12_=prefer;}
    void Close()override{decoder_.Close();}
    VideoReadResult Read(VideoFrame& frame,std::stop_token stop)override{
        return decoder_.ReadNextAvailable(frame,stop);
    }
    bool SeekSeconds(double seconds)override{return decoder_.SeekSeconds(seconds);}
    uint32_t Width()const override{return decoder_.Width();}
    uint32_t Height()const override{return decoder_.Height();}
    double FrameRate()const override{return decoder_.FrameRate();}
    double DurationSeconds()const override{return decoder_.DurationSeconds();}
    VideoDecoder::KnownMedia Media()const override{return decoder_.Media();}
private:
    VideoDecoder decoder_;
    bool preferNv12_{};
};

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
    // A decoded frame plus whether its frame number is authoritative. Frame 0
    // legitimately carries frameNumber 0, so a zero number is only trusted when
    // the (pre-offset) pts is also zero; otherwise the source did not stamp
    // identities and pairing falls back to timestamp tolerance.
    struct Pending {
        VideoFrame frame;
        bool numbered{};
    };
    // Outcome of comparing the two pending members.
    enum class Match { Pair, Mismatch, Skew };
    // The two comparison members are either owned (the default constructor builds
    // real decoders) or borrowed from the caller that injected them, so the raw
    // pointer is what the code reads and the unique_ptr only carries ownership
    // when there is any.
    ISynchronizedFrameSource* original{};
    std::unique_ptr<ISynchronizedFrameSource> originalOwned;
    ISynchronizedFrameSource* neural{};
    std::unique_ptr<ISynchronizedFrameSource> neuralOwned;
    // Set only by the injecting two-source constructor: a reopen has to restore
    // the caller's neural member, which a no-neural Open cleared.
    ISynchronizedFrameSource* injectedNeural{};
    // Only a playback that owns its sources may build a decoder for a neural
    // member on demand; an injected one must not invent a real decoder.
    bool ownsSources{};
    std::optional<Pending> pendingOriginal;
    std::optional<Pending> pendingNeural;
    // Shared, not optional-by-value: PlayerApp retains the presented pair for
    // paused redraws and the neural toggle, and copying two full frames out of
    // it per presented frame was 11 MB of memcpy at 1440p against a 16.7 ms
    // budget. One small control-block allocation per pair buys both retentions
    // an alias. const so no holder can mutate a pair another holder is reading.
    std::shared_ptr<const SynchronizedFramePair> current;
    ComparisonView view{ComparisonView::Original};
    bool opened{};
    bool paused{};
    bool stepRequested{};
    int64_t tolerance100ns{333334};
    SynchronizedRange range{};
    uint64_t rangeOffsetFrames{};
    // First frame index outside the range; 0 when the range is open-ended.
    uint64_t rangeEndFrames{};
    // Live mode: the neural member is a growing list of finalized files instead
    // of one whole render. One decoder serves the segment under the playhead
    // while a second one is opened ahead of the next boundary.
    std::function<std::unique_ptr<ISynchronizedFrameSource>()> makeSegmentSource;
    std::shared_ptr<const NeuralSegmentIndex> segments;
    std::unique_ptr<ISynchronizedFrameSource> segmentSource;
    std::unique_ptr<ISynchronizedFrameSource> prefetchSource;
    NeuralSegment segment{};
    NeuralSegment prefetchSegment{};
    std::optional<Pending> prefetchPending;
    // Parameters of the first segment opened in this session. Every segment of
    // one render is the same encoder at the same geometry, so the ones after it
    // are opened without a probe.
    VideoDecoder::KnownMedia segmentMedia{};
    // A segment open in flight on another thread. Opening one costs a child
    // process (and a probe, before segmentMedia is known); doing that on the
    // thread that presents frames cost a measured 45% of them on a machine
    // whose antivirus scans the spawn.
    struct PendingOpen {
        std::future<std::unique_ptr<ISynchronizedFrameSource>> future;
        std::stop_source stop;
        NeuralSegment segment;
    };
    std::optional<PendingOpen> pendingOpen;
    bool live{};
    // Set when the open segment file ended before its declared window.
    bool segmentExhausted{};
    // Why the last read reported OutOfSync. A modal warning without a record
    // of the frame numbers behind it cannot be diagnosed after the fact.
    struct Fault {
        const char* reason{};
        uint64_t originalFrame{},neuralFrame{};
        int64_t original100ns{},neural100ns{};
        uint64_t segmentIndex{};
        bool exhausted{},numbered{};
    };
    Fault fault{};
    int64_t prefetchLead100ns{10000000};
    // What the caller asked the members to decode to. Every segment opened
    // later this session inherits it, so one answer covers the whole pair.
    bool preferNv12{};

    void ResetPublished()
    {
        pendingOriginal.reset();pendingNeural.reset();current.reset();
        view=ComparisonView::Original;paused=false;stepRequested=false;
    }

    void CloseSegmentSources()
    {
        CancelPendingOpen();
        if(segmentSource)segmentSource->Close();
        if(prefetchSource)prefetchSource->Close();
        segmentSource.reset();prefetchSource.reset();
        segment=NeuralSegment{};prefetchSegment=NeuralSegment{};
        prefetchPending.reset();segmentExhausted=false;
    }

    void CancelPendingOpen()
    {
        if(!pendingOpen)return;
        pendingOpen->stop.request_stop();
        if(pendingOpen->future.valid()){
            auto source=pendingOpen->future.get();
            if(source)source->Close();
        }
        pendingOpen.reset();
    }

    // Starts the open of `wanted` on another thread. Failures are silent: the
    // boundary itself opens the file if this never produces one.
    void StartAsyncOpen(NeuralSegment wanted)
    {
        if(pendingOpen||!makeSegmentSource)return;
        PendingOpen pending{};
        pending.segment=wanted;
        // The worker touches nothing this object owns: it gets its own copies.
        auto factory=makeSegmentSource;
        const VideoDecoder::KnownMedia media=segmentMedia;
        const std::filesystem::path path=wanted.path;
        const bool nv12=preferNv12;
        try{
            pending.future=std::async(std::launch::async,
                [factory,media,path,nv12,stop=pending.stop.get_token()]()->std::unique_ptr<ISynchronizedFrameSource>{
                    auto source=factory();
                    if(!source)return nullptr;
                    source->PreferNv12(nv12);
                    const bool ready=media.Valid()?source->OpenKnown(path,media,stop)
                                                  :source->Open(path,stop);
                    if(!ready){source->Close();return nullptr;}
                    return source;
                });
        }catch(const std::system_error&){
            return;
        }
        pendingOpen=std::move(pending);
    }

    // Promotes a finished background open to the prefetch slot. Geometry is
    // checked here, so a segment that does not match the original never becomes
    // one; the synchronous path then reports it.
    void HarvestAsyncOpen()
    {
        if(!pendingOpen||!pendingOpen->future.valid())return;
        if(pendingOpen->future.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        auto source=pendingOpen->future.get();
        NeuralSegment landed=pendingOpen->segment;
        pendingOpen.reset();
        if(!source)return;
        // Layout beside geometry, and for the same reason: both members feed
        // one renderer whose source layout was fixed at Initialize. A segment
        // that answered a different one cannot be presented, and a silent
        // acceptance here would reach the screen as tearing rather than as an
        // error. It takes the synchronous path's refusal, which reports.
        if(source->Width()!=original->Width()||source->Height()!=original->Height()||
           source->Layout()!=original->Layout()){
            source->Close();return;
        }
        if(prefetchSource)prefetchSource->Close();
        prefetchSource=std::move(source);prefetchSegment=std::move(landed);
        prefetchPending.reset();
    }

    // End of the playable window in seconds on the original timeline.
    double EndSeconds()const
    {
        double end=range.end100ns>0?double(range.end100ns)*1e-7:original->DurationSeconds();
        if(neural)end=std::min(end,double(range.start100ns)*1e-7+neural->DurationSeconds());
        return end;
    }

    // A seeked FFmpeg source rebases its timestamps a few ticks below the
    // canonical CFR grid, so the first out-of-range frame can still compare
    // less than range.end100ns. Decide on the frame index when the source
    // stamps identities, and keep a half-frame margin for sources that do not.
    bool PastRangeEnd(const Pending& pending)const
    {
        if(pending.numbered&&rangeEndFrames)return pending.frame.frameNumber>=rangeEndFrames;
        return pending.frame.timestamp100ns>=range.end100ns-tolerance100ns/2;
    }

    SynchronizedReadResult ReadOneShifted(ISynchronizedFrameSource& source,std::optional<Pending>& pending,
                                          std::stop_token stop,int64_t timestampShift100ns,
                                          uint64_t frameShift)
    {
        if(pending)return SynchronizedReadResult::PairReady;
        VideoFrame frame;const VideoReadResult read=source.Read(frame,stop);
        if(read!=VideoReadResult::FrameReady)return ConvertRead(read);
        Pending next{std::move(frame),false};
        next.numbered=next.frame.frameNumber!=0||next.frame.timestamp100ns==0;
        next.frame.timestamp100ns+=timestampShift100ns;
        next.frame.frameNumber+=frameShift;
        pending=std::move(next);return SynchronizedReadResult::PairReady;
    }

    SynchronizedReadResult ReadOne(ISynchronizedFrameSource& source,std::optional<Pending>& pending,
                                   std::stop_token stop,bool neuralMember)
    {
        // The neural render starts at its own zero; place it on the original timeline.
        if(!neuralMember)return ReadOneShifted(source,pending,stop,0,0);
        return ReadOneShifted(source,pending,stop,range.start100ns,rangeOffsetFrames);
    }

    SynchronizedReadResult CommitPair(SynchronizedFramePair& pair)
    {
        pair.original=std::move(pendingOriginal->frame);
        pair.neural=pendingNeural?std::move(pendingNeural->frame):VideoFrame{};
        pair.timestamp100ns=pair.original.timestamp100ns;pair.frameNumber=pair.original.frameNumber;
        pendingOriginal.reset();pendingNeural.reset();return SynchronizedReadResult::PairReady;
    }

    // Both members are pending: decide whether they are the same source frame.
    // A mismatch drops the earlier member so the next read can resynchronize.
    Match MatchPending()
    {
        fault=Fault{nullptr,pendingOriginal->frame.frameNumber,pendingNeural->frame.frameNumber,
                    pendingOriginal->frame.timestamp100ns,pendingNeural->frame.timestamp100ns,
                    segment.index,segmentExhausted,
                    pendingOriginal->numbered&&pendingNeural->numbered};
        if(pendingOriginal->numbered&&pendingNeural->numbered){
            const uint64_t originalNumber=pendingOriginal->frame.frameNumber;
            const uint64_t neuralNumber=pendingNeural->frame.frameNumber;
            if(originalNumber==neuralNumber)return Match::Pair;
            // Live mode enters a segment by seeking the file, and a segment holds
            // one keyframe at its own start: a seek into the middle of it lands
            // back on that keyframe, so the first frames it hands over are BEHIND
            // the playhead. Walking forward over them reaches the frame the
            // original is holding, bounded by the caller's resync guard - which
            // is what the timestamp path below has always done. Refusing instead
            // made every seek into the middle of a rendered segment fail: one
            // measured session reported original=165 against neural=150, exactly
            // the half second between the playhead and the segment's start, and
            // handed playback back to the original over rendered frames.
            //
            // Cached playback keeps the hard refusal: there the two files are a
            // published pair, and a numbered disagreement is the identity failure
            // this check exists to catch.
            if(originalNumber<neuralNumber)pendingOriginal.reset();else pendingNeural.reset();
            return live?Match::Skew:Match::Mismatch;
        }
        const int64_t difference=pendingOriginal->frame.timestamp100ns-pendingNeural->frame.timestamp100ns;
        if(std::llabs(difference)<=tolerance100ns)return Match::Pair;
        if(difference<0)pendingOriginal.reset();else pendingNeural.reset();
        return Match::Skew;
    }

    // Records why pairing gave up, then reports it.
    SynchronizedReadResult Desync(const char* reason)
    {
        fault.reason=reason;
        fault.segmentIndex=segment.index;fault.exhausted=segmentExhausted;
        return SynchronizedReadResult::OutOfSync;
    }

    SynchronizedReadResult BuildPair(SynchronizedFramePair& pair,std::stop_token stop)
    {
        if(!original)return SynchronizedReadResult::Error;
        for(size_t guard=0;guard<4096;++guard){
            const auto originalRead=ReadOne(*original,pendingOriginal,stop,false);
            if(originalRead!=SynchronizedReadResult::PairReady){
                if(!neural||originalRead!=SynchronizedReadResult::EndOfStream)return originalRead;
                if(pendingNeural)return Desync("original-ended-with-neural-pending");
                VideoFrame extra;const auto neuralRead=neural->Read(extra,stop);
                if(neuralRead==VideoReadResult::EndOfStream)return SynchronizedReadResult::EndOfStream;
                if(neuralRead==VideoReadResult::NotReady)return SynchronizedReadResult::NotReady;
                if(neuralRead==VideoReadResult::Cancelled)return SynchronizedReadResult::Cancelled;
                if(neuralRead==VideoReadResult::FrameReady)return Desync("original-ended-before-neural");
                return SynchronizedReadResult::Error;
            }
            if(range.end100ns>0&&PastRangeEnd(*pendingOriginal)){
                pendingOriginal.reset();return SynchronizedReadResult::EndOfStream;
            }
            if(!neural)return CommitPair(pair);
            const auto neuralRead=ReadOne(*neural,pendingNeural,stop,true);
            if(neuralRead!=SynchronizedReadResult::PairReady){
                if(neuralRead==SynchronizedReadResult::EndOfStream)return Desync("neural-ended-first");
                return neuralRead;
            }
            switch(MatchPending()){
                case Match::Pair:return CommitPair(pair);
                case Match::Mismatch:return Desync("frame-mismatch");
                case Match::Skew:break;
            }
        }
        return Desync("resync-guard");
    }

    // A run numbers its own segments, so a segment's identity is the pair: two
    // runs both publishing an index 0 is the normal case once a session has been
    // retargeted at a hole.
    static bool SameSegment(const NeuralSegment& a,const NeuralSegment& b)
    {
        return a.runId==b.runId&&a.index==b.index&&a.firstTimestamp100ns==b.firstTimestamp100ns;
    }

    bool SegmentCovers(const NeuralSegment& candidate,const Pending& pending)const
    {
        // Frame numbers are exact on the CFR grid; timestamps of a seeked source
        // are not, and a boundary is exactly where that fuzz would mispick.
        if(pending.numbered&&candidate.frameCount)
            return pending.frame.frameNumber>=candidate.firstFrameNumber&&
                   pending.frame.frameNumber<candidate.firstFrameNumber+candidate.frameCount;
        return pending.frame.timestamp100ns>=candidate.firstTimestamp100ns&&
               pending.frame.timestamp100ns<candidate.end100ns;
    }


    // `allowLateStart` is for one caller: the open file ended inside its own
    // declared window, so the playhead is still nominally in it while the frames
    // it should have served are missing. The continuation legitimately begins
    // AFTER the playhead there, and entering it lets the matcher walk the
    // original forward over the missing frames - which is how a short file has
    // always been tolerated. Refusing it stalled playback at that boundary for
    // good, because a waiting read never advances the original either.
    SynchronizedReadResult AdoptSegment(NeuralSegment wanted,int64_t timestamp100ns,std::stop_token stop,
                                        bool allowLateStart=false)
    {
        // Any other file that begins after the playhead cannot serve it.
        // Reaching an earlier region from a later one used to land here with a
        // negative entry offset, skip the seek, and pair that region's frame 0
        // against the original until the resync guard fired.
        if(!allowLateStart&&timestamp100ns+tolerance100ns/2<wanted.firstTimestamp100ns)
            return SynchronizedReadResult::WaitingForRender;
        // Only a disagreement between number and timestamp coverage can ask for
        // the file already open; serving it beats reopening it every frame.
        if(segmentSource&&SameSegment(segment,wanted))return SynchronizedReadResult::PairReady;
        // The boundary arrived before the background open finished: wait for it
        // rather than starting a second process for the same file.
        if(pendingOpen&&SameSegment(pendingOpen->segment,wanted)&&pendingOpen->future.valid())
            pendingOpen->future.wait();
        HarvestAsyncOpen();
        // The boundary is free when prefetch already opened and warmed the file.
        if(prefetchSource&&SameSegment(prefetchSegment,wanted)){
            if(segmentSource)segmentSource->Close();
            segmentSource=std::move(prefetchSource);segment=std::move(prefetchSegment);
            pendingNeural=std::move(prefetchPending);prefetchPending.reset();
            prefetchSegment=NeuralSegment{};segmentExhausted=false;
            return SynchronizedReadResult::PairReady;
        }
        // A prefetch for a segment the playhead skipped would otherwise sit
        // there and block every later one.
        if(prefetchSource){
            prefetchSource->Close();prefetchSource.reset();
            prefetchSegment=NeuralSegment{};prefetchPending.reset();
        }
        if(pendingOpen)CancelPendingOpen();
        auto source=makeSegmentSource?makeSegmentSource():nullptr;
        if(!source)return SynchronizedReadResult::Error;
        source->PreferNv12(preferNv12);
        const bool ready=segmentMedia.Valid()?source->OpenKnown(wanted.path,segmentMedia,stop)
                                             :source->Open(wanted.path,stop);
        if(!ready)
            return stop.stop_requested()?SynchronizedReadResult::Cancelled:SynchronizedReadResult::Error;
        if(source->Width()!=original->Width()||source->Height()!=original->Height()||
           source->Layout()!=original->Layout()){
            source->Close();return SynchronizedReadResult::Error;
        }
        // The first segment of a session is the one that pays for a probe.
        if(!segmentMedia.Valid())segmentMedia=source->Media();
        // Segment files start at their own zero, so entering one mid-way (a seek)
        // seeks the file, not the source timeline.
        const int64_t local=timestamp100ns-wanted.firstTimestamp100ns;
        if(local>=tolerance100ns/2&&!source->SeekSeconds(double(local)*1e-7)){
            source->Close();return SynchronizedReadResult::Error;
        }
        if(segmentSource)segmentSource->Close();
        segmentSource=std::move(source);segment=std::move(wanted);
        pendingNeural.reset();segmentExhausted=false;
        return SynchronizedReadResult::PairReady;
    }

    // Makes segmentSource the decoder that serves the pending original frame.
    SynchronizedReadResult SelectSegment(int64_t timestamp100ns,std::stop_token stop)
    {
        if(segmentSource&&!segmentExhausted&&SegmentCovers(segment,*pendingOriginal))
            return SynchronizedReadResult::PairReady;
        // Coverage is decided on frame numbers, so the lookup is too; only a
        // source that does not stamp identities falls back to timestamps.
        auto covering=pendingOriginal->numbered
                          ? segments->ContainingFrame(pendingOriginal->frame.frameNumber)
                          : std::nullopt;
        if(!covering)covering=segments->Containing(timestamp100ns);
        // The open file ended inside its own declared window and the lookup hands
        // it straight back, so step to what follows rather than reopening it
        // every frame. Only a file that continues this region will do: the first
        // file of a region across a hole is not this playhead's.
        if(segmentExhausted&&segmentSource&&covering&&SameSegment(*covering,segment)){
            auto following=segments->After(segment.firstTimestamp100ns);
            if(!following||following->firstTimestamp100ns>segment.end100ns)
                return SynchronizedReadResult::WaitingForRender;
            return AdoptSegment(std::move(*following),timestamp100ns,stop,true);
        }
        // Nothing rendered here yet. Coverage is a set of rendered regions, and
        // the gaps between them are ordinary unrendered video that the session
        // fills as the user reaches them - a wait, never a fault. Before the
        // first region, inside a hole and past the newest region are the same
        // answer here; where the video ENDS is the playable range's business,
        // which is the only thing that knows it.
        if(!covering)return SynchronizedReadResult::WaitingForRender;
        return AdoptSegment(std::move(*covering),timestamp100ns,stop);
    }

    // A boundary costs a process start (and a probe, for the first segment of a
    // session), so the next file is opened on another thread once the playhead
    // enters the lead window, and warmed here once that open lands. Failures are
    // silent: the boundary itself opens the file if none of this worked.
    void PrefetchNextSegment(int64_t timestamp100ns,std::stop_token stop)
    {
        if(!segmentSource||stop.stop_requested())return;
        HarvestAsyncOpen();
        if(!prefetchSource){
            if(pendingOpen)return;
            if(timestamp100ns<segment.end100ns-prefetchLead100ns)return;
            auto following=segments->After(segment.firstTimestamp100ns);
            // Only the file that continues this region is worth a process start.
            if(!following||following->firstTimestamp100ns>segment.end100ns)return;
            StartAsyncOpen(std::move(*following));
            return;
        }
        if(!prefetchPending)
            ReadOneShifted(*prefetchSource,prefetchPending,stop,prefetchSegment.firstTimestamp100ns,
                           prefetchSegment.firstFrameNumber);
    }

    SynchronizedReadResult BuildLivePair(SynchronizedFramePair& pair,std::stop_token stop)
    {
        if(!original||!segments)return SynchronizedReadResult::Error;
        for(size_t guard=0;guard<4096;++guard){
            const auto originalRead=ReadOne(*original,pendingOriginal,stop,false);
            if(originalRead!=SynchronizedReadResult::PairReady)return originalRead;
            if(range.end100ns>0&&PastRangeEnd(*pendingOriginal)){
                pendingOriginal.reset();return SynchronizedReadResult::EndOfStream;
            }
            const int64_t timestamp100ns=pendingOriginal->frame.timestamp100ns;
            const auto selected=SelectSegment(timestamp100ns,stop);
            if(selected!=SynchronizedReadResult::PairReady)return selected;
            const auto neuralRead=ReadOneShifted(*segmentSource,pendingNeural,stop,
                                                 segment.firstTimestamp100ns,segment.firstFrameNumber);
            if(neuralRead==SynchronizedReadResult::EndOfStream){
                // The file ended inside its declared window; continue in the
                // next segment rather than reporting the source as broken.
                segmentExhausted=true;continue;
            }
            if(neuralRead!=SynchronizedReadResult::PairReady)return neuralRead;
            switch(MatchPending()){
                case Match::Pair:{
                    const auto committed=CommitPair(pair);
                    PrefetchNextSegment(pair.timestamp100ns,stop);
                    return committed;
                }
                case Match::Mismatch:return Desync("live-frame-mismatch");
                case Match::Skew:break;
            }
        }
        return Desync("live-resync-guard");
    }
};

SynchronizedPlayback::SynchronizedPlayback() : impl_(std::make_unique<Impl>())
{
    impl_->ownsSources=true;
    impl_->originalOwned=std::make_unique<DecoderFrameSource>();
    impl_->original=impl_->originalOwned.get();
    impl_->makeSegmentSource=[]{return std::unique_ptr<ISynchronizedFrameSource>(std::make_unique<DecoderFrameSource>());};
}

SynchronizedPlayback::SynchronizedPlayback(ISynchronizedFrameSource& original,
                                             ISynchronizedFrameSource& neural)
    : impl_(std::make_unique<Impl>())
{
    impl_->original=&original;
    impl_->neural=&neural;
    impl_->injectedNeural=&neural;
}

SynchronizedPlayback::SynchronizedPlayback(ISynchronizedFrameSource& original,
                                             SegmentSourceFactory segments)
    : impl_(std::make_unique<Impl>())
{
    impl_->original=&original;
    impl_->makeSegmentSource=[factory=std::move(segments)]()->std::unique_ptr<ISynchronizedFrameSource>{
        return factory?factory():nullptr;
    };
}

SynchronizedPlayback::~SynchronizedPlayback(){Close();}
SynchronizedPlayback::SynchronizedPlayback(SynchronizedPlayback&&) noexcept=default;
SynchronizedPlayback& SynchronizedPlayback::operator=(SynchronizedPlayback&&) noexcept=default;

bool SynchronizedPlayback::Open(const std::filesystem::path& originalPath,
                                const std::filesystem::path& neuralPath,std::stop_token stop,
                                SynchronizedRange range,bool preferNv12)
{
    Close();if(!impl_->original||originalPath.empty())return false;
    if(range.start100ns<0||range.end100ns<0||(range.end100ns>0&&range.end100ns<=range.start100ns))return false;
    impl_->preferNv12=preferNv12;
    impl_->original->PreferNv12(preferNv12);
    if(!impl_->original->Open(originalPath,stop))return false;
    const double originalFps=impl_->original->FrameRate();
    if(!impl_->original->Width()||!impl_->original->Height()||
       !std::isfinite(originalFps)||originalFps<=0.0){
        impl_->original->Close();return false;
    }
    const double startSeconds=double(range.start100ns)*1e-7;
    const double originalDuration=impl_->original->DurationSeconds();
    if(range.end100ns>0&&double(range.end100ns)*1e-7>originalDuration+1.0/originalFps+1e-6){
        impl_->original->Close();return false;
    }
    if(range.start100ns>0&&(startSeconds>=originalDuration||!impl_->original->SeekSeconds(startSeconds))){
        impl_->original->Close();return false;
    }
    const bool useNeural=!neuralPath.empty();
    // An owning playback built a fresh decoder on every Open before the sources
    // became injectable, and still does: reusing one that a previous Open left
    // behind is a lifetime the old code never exercised. An injected playback
    // instead restores the member its caller handed over, which a no-neural Open
    // may have cleared.
    if(useNeural){
        if(impl_->ownsSources){
            impl_->neuralOwned=std::make_unique<DecoderFrameSource>();
            impl_->neural=impl_->neuralOwned.get();
        }else if(!impl_->neural&&impl_->injectedNeural){
            impl_->neural=impl_->injectedNeural;
        }
    }
    if(useNeural){
        if(!impl_->neural||!impl_->neural->Open(neuralPath,stop)){impl_->original->Close();return false;}
        const double fps=originalFps;
        const double neuralFps=impl_->neural->FrameRate();
        const double durationTolerance=fps>0.0?1.0/fps:0.0;
        const double expectedDuration=
            (range.end100ns>0?double(range.end100ns)*1e-7:originalDuration)-startSeconds;
        if(impl_->original->Width()!=impl_->neural->Width()||
           impl_->original->Height()!=impl_->neural->Height()||
           !std::isfinite(fps)||fps<=0.0||!std::isfinite(neuralFps)||
           std::abs(fps-neuralFps)>0.01||
           std::abs(expectedDuration-impl_->neural->DurationSeconds())>durationTolerance+1e-6){
            impl_->neural->Close();impl_->original->Close();return false;
        }
        impl_->tolerance100ns=static_cast<int64_t>(std::ceil(10000000.0/fps));
    }else{
        impl_->neural=nullptr;impl_->neuralOwned.reset();
        const double fps=originalFps;
        if(std::isfinite(fps)&&fps>0.0)
            impl_->tolerance100ns=static_cast<int64_t>(std::ceil(10000000.0/fps));
    }
    impl_->range=range;
    impl_->rangeOffsetFrames=static_cast<uint64_t>(std::llround(startSeconds*originalFps));
    impl_->rangeEndFrames=range.end100ns>0
        ? static_cast<uint64_t>(std::llround(double(range.end100ns)*1e-7*originalFps)) : 0;
    impl_->ResetPublished();impl_->opened=true;return true;
}

bool SynchronizedPlayback::OpenLive(const std::filesystem::path& originalPath,
                                    std::shared_ptr<const NeuralSegmentIndex> segments,
                                    SynchronizedRange range,std::stop_token stop,
                                    const VideoDecoder::KnownMedia& originalMedia,bool preferNv12)
{
    Close();
    if(!impl_->original||!impl_->makeSegmentSource||originalPath.empty()||!segments)return false;
    if(range.start100ns<0||range.end100ns<0||(range.end100ns>0&&range.end100ns<=range.start100ns))return false;
    impl_->preferNv12=preferNv12;
    impl_->original->PreferNv12(preferNv12);
    const bool ready=originalMedia.Valid()
        ? impl_->original->OpenKnown(originalPath,originalMedia,stop)
        : impl_->original->Open(originalPath,stop);
    if(!ready)return false;
    const double originalFps=impl_->original->FrameRate();
    if(!impl_->original->Width()||!impl_->original->Height()||
       !std::isfinite(originalFps)||originalFps<=0.0){
        impl_->original->Close();return false;
    }
    const double startSeconds=double(range.start100ns)*1e-7;
    const double originalDuration=impl_->original->DurationSeconds();
    if(range.end100ns>0&&double(range.end100ns)*1e-7>originalDuration+1.0/originalFps+1e-6){
        impl_->original->Close();return false;
    }
    if(range.start100ns>0&&(startSeconds>=originalDuration||!impl_->original->SeekSeconds(startSeconds))){
        impl_->original->Close();return false;
    }
    // Nothing to validate against: the render is still producing its files.
    impl_->neural=nullptr;impl_->neuralOwned.reset();
    impl_->tolerance100ns=static_cast<int64_t>(std::ceil(10000000.0/originalFps));
    impl_->range=range;
    impl_->rangeOffsetFrames=static_cast<uint64_t>(std::llround(startSeconds*originalFps));
    impl_->rangeEndFrames=range.end100ns>0
        ? static_cast<uint64_t>(std::llround(double(range.end100ns)*1e-7*originalFps)) : 0;
    // One second of lead is more than an ffmpeg start plus a first decode.
    impl_->prefetchLead100ns=std::max<int64_t>(10000000,2*impl_->tolerance100ns);
    impl_->segments=std::move(segments);impl_->live=true;
    // A different render can be a different encoder or geometry: this session's
    // first segment establishes the parameters again.
    impl_->segmentMedia={};
    impl_->ResetPublished();impl_->opened=true;return true;
}

SynchronizedRange SynchronizedPlayback::Range()const{return impl_->range;}

void SynchronizedPlayback::Close()
{
    if(!impl_)return;
    if(impl_->original)impl_->original->Close();if(impl_->neural)impl_->neural->Close();
    impl_->CloseSegmentSources();impl_->segments.reset();impl_->live=false;impl_->segmentMedia={};
    impl_->opened=false;impl_->ResetPublished();
}

SynchronizedReadResult SynchronizedPlayback::ReadNextAvailable(std::stop_token stop)
{
    if(!impl_->opened)return SynchronizedReadResult::Error;
    if(impl_->paused&&!impl_->stepRequested)return SynchronizedReadResult::NotReady;
    SynchronizedFramePair pair;
    const auto result=impl_->live?impl_->BuildLivePair(pair,stop):impl_->BuildPair(pair,stop);
    if(result==SynchronizedReadResult::PairReady)impl_->current=std::make_shared<const SynchronizedFramePair>(std::move(pair));
    // WaitingForRender produced no frame, so a requested step is still pending.
    if(impl_->stepRequested&&result!=SynchronizedReadResult::NotReady&&
       result!=SynchronizedReadResult::WaitingForRender)impl_->stepRequested=false;
    return result;
}

bool SynchronizedPlayback::SeekSeconds(double seconds,std::stop_token stop)
{
    if(!impl_->opened||!std::isfinite(seconds)||seconds<0.0||stop.stop_requested())return false;
    if(impl_->live)return SeekLive(seconds,stop);
    // The timeline endpoint is after the last frame, not a decodable timestamp.
    const double start=double(impl_->range.start100ns)*1e-7;
    const double duration=impl_->EndSeconds();
    const double frameDuration=1.0/impl_->original->FrameRate();
    if(std::isfinite(duration)&&duration>0.0)
        seconds=std::min(seconds,std::max(start,duration-frameDuration));
    seconds=std::max(seconds,start);
    // Restarting FFmpeg is asynchronous. Retain either ready half of the pair
    // while the other decoder warms up; NotReady is not a failed seek.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(int attempt=0;attempt<2;++attempt){
        if(stop.stop_requested()||!impl_->original->SeekSeconds(seconds)||
           (impl_->neural&&!impl_->neural->SeekSeconds(seconds-start))){Close();return false;}
        impl_->pendingOriginal.reset();impl_->pendingNeural.reset();
        SynchronizedFramePair candidate;
        SynchronizedReadResult result;
        for(;;){
            result=impl_->BuildPair(candidate,stop);
            if(stop.stop_requested()||std::chrono::steady_clock::now()>=deadline){Close();return false;}
            if(result!=SynchronizedReadResult::NotReady)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if(result==SynchronizedReadResult::PairReady){
            impl_->current=std::make_shared<const SynchronizedFramePair>(std::move(candidate));impl_->stepRequested=false;return true;
        }
        // Container/audio duration may round past the last video PTS. At the
        // tail only, retry one frame earlier instead of unloading a valid pair.
        if(attempt==0&&seconds>start&&seconds>=duration-2.0*frameDuration&&
           (result==SynchronizedReadResult::EndOfStream||result==SynchronizedReadResult::OutOfSync)){
            seconds=std::max(start,seconds-frameDuration);continue;
        }
        break;
    }
    Close();return false;
}

// Live mode: only a finalized segment is seekable. An uncovered target is not a
// failure of the session, so nothing is closed and the caller decides.
bool SynchronizedPlayback::SeekLive(double seconds,std::stop_token stop)
{
    const double start=double(impl_->range.start100ns)*1e-7;
    if(seconds<start)seconds=start;
    const int64_t target=static_cast<int64_t>(std::llround(seconds*10000000.0));
    // Every refusal below reached the caller as the same "not rendered", which
    // covers four unrelated failures: a target past the range, coverage that has
    // not reached it, the original's own seek failing - on a network source that
    // is an HTTP re-open, measured at 0.2-2.0 s and able to fail on its own - and
    // a pair that never assembled inside the deadline. Only the desync path set a
    // reason, so the other three were undiagnosable after the fact. The target
    // rides along so the reason can be read against the coverage.
    //
    // The fault is cleared on entry and on success because `Desync` is its only
    // other writer: without that, a refusal here would report whatever pairing
    // last disagreed about - a `live-frame-mismatch` from minutes ago reads as
    // this seek's reason, which is worse than saying nothing - and the branch
    // below that trusts a reason to already be set would trust a stale one.
    impl_->fault=Impl::Fault{};
    const auto refuse=[&](const char* reason){
        impl_->fault=Impl::Fault{};
        impl_->fault.reason=reason;
        impl_->fault.original100ns=target;
        return false;
    };
    if(impl_->range.end100ns>0&&target>=impl_->range.end100ns)return refuse("live-seek-past-range-end");
    if(!impl_->segments||!impl_->segments->Containing(target))return refuse("live-seek-uncovered");
    if(!impl_->original->SeekSeconds(seconds))return refuse("live-seek-original-failed");
    impl_->pendingOriginal.reset();impl_->pendingNeural.reset();
    // Segment decoders are positioned for the old playhead; pairing is driven by
    // the original's pts, so the next read reopens whatever now covers it.
    impl_->CloseSegmentSources();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    SynchronizedFramePair candidate;
    for(;;){
        const auto result=impl_->BuildLivePair(candidate,stop);
        if(result==SynchronizedReadResult::PairReady){
            impl_->current=std::make_shared<const SynchronizedFramePair>(std::move(candidate));impl_->stepRequested=false;
            impl_->fault=Impl::Fault{};
            return true;
        }
        if(result!=SynchronizedReadResult::NotReady){
            // A desync already recorded which frames disagreed; the rest say only
            // what the pair builder returned.
            if(impl_->fault.reason)return false;
            return refuse(result==SynchronizedReadResult::WaitingForRender?"live-seek-waiting-for-render"
                          :result==SynchronizedReadResult::EndOfStream?"live-seek-pair-end"
                                                                      :"live-seek-pair-failed");
        }
        if(stop.stop_requested()||std::chrono::steady_clock::now()>=deadline)return refuse("live-seek-deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool SynchronizedPlayback::SetView(ComparisonView view)
{
    if(!impl_->opened)return false;
    if(view==ComparisonView::Neural&&((!impl_->neural&&!impl_->live)||!impl_->current))return false;
    impl_->view=view;return true;
}

ComparisonView SynchronizedPlayback::View()const{return impl_->view;}
const VideoFrame* SynchronizedPlayback::VisibleFrame()const
{
    if(!impl_->current)return nullptr;
    return impl_->view==ComparisonView::Neural?&impl_->current->neural:&impl_->current->original;
}
const SynchronizedFramePair* SynchronizedPlayback::CurrentPair()const
{return impl_->current.get();}
std::shared_ptr<const SynchronizedFramePair> SynchronizedPlayback::CurrentPairShared()const
{return impl_->current;}
void SynchronizedPlayback::SetPaused(bool paused){impl_->paused=paused;if(!paused)impl_->stepRequested=false;}
bool SynchronizedPlayback::Paused()const{return impl_->paused;}
bool SynchronizedPlayback::Step(){if(!impl_->opened||!impl_->paused)return false;impl_->stepRequested=true;return true;}
bool SynchronizedPlayback::NeuralAvailable()const
{return impl_->opened&&(impl_->neural!=nullptr||(impl_->live&&impl_->segments!=nullptr));}
bool SynchronizedPlayback::Live()const{return impl_->opened&&impl_->live;}
int64_t SynchronizedPlayback::LiveHead100ns()const
{return impl_->live&&impl_->segments?impl_->segments->Head100ns():0;}

std::string SynchronizedPlayback::LastFault()const
{
    const Impl::Fault& fault=impl_->fault;
    if(!fault.reason)return {};
    std::string text=fault.reason;
    text+=" original="+std::to_string(fault.originalFrame)+"@"+std::to_string(fault.original100ns);
    text+=" neural="+std::to_string(fault.neuralFrame)+"@"+std::to_string(fault.neural100ns);
    text+=fault.numbered?" numbered=1":" numbered=0";
    if(impl_->live){
        text+=" segment="+std::to_string(fault.segmentIndex);
        text+=fault.exhausted?" exhausted=1":" exhausted=0";
        // A refusal is judged against coverage, and coverage is now a set: the
        // regions themselves are printed, because "head" alone cannot say
        // whether a target sits in a hole or beyond everything rendered.
        if(impl_->segments){
            const auto covered=impl_->segments->CoveredRanges();
            text+=" segments="+std::to_string(impl_->segments->Count())+" coverage=";
            if(covered.empty())text+="none";
            for(size_t i=0;i<covered.size();++i){
                if(i)text+=",";
                text+="["+std::to_string(covered[i].start100ns)+","+std::to_string(covered[i].end100ns)+")";
            }
        }
    }
    return text;
}
