#include "VsrUpscalePass.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "D3D12Renderer.h"
#include "Log.h"
#include "Utf8Text.h"
#include "TemporalGuides.h"
#include "VideoDecoder.h"

namespace {

// The renderer presents into this window, which is never shown: it exists so the
// swapchain is the output's size and VSR maps texel for pixel, as the measurement
// ran it. Destroyed on every path.
struct HiddenWindow {
    HWND hwnd{};
    HiddenWindow(uint32_t width, uint32_t height)
        : hwnd(CreateWindowExW(0, L"STATIC", L"RTX VSR export", WS_POPUP, 0, 0, int(width), int(height), nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr)) {}
    ~HiddenWindow() { if (hwnd) DestroyWindow(hwnd); }
    HiddenWindow(const HiddenWindow&) = delete;
    HiddenWindow& operator=(const HiddenWindow&) = delete;
};

// The window belongs to this thread, so its messages are this thread's to take.
void DrainMessages(HWND window)
{
    MSG message{};
    while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
}

// Writes the upscaled frames to the encoder on a thread of its own, in the order they
// are handed over. The pipe to the encoder child blocks for as long as the child is
// busy with the frame before, and on the pass's thread that held up the decode and
// the GPU work of every frame behind it. A frame is read straight out of the readback
// slot it was copied into, so the slot is not enqueued again until its frame is
// written (WaitUntilAtMost).
class FrameWriter {
public:
    using View = D3D12Renderer::ComposedReadbackView;

    FrameWriter(RawVideoEncoder& encoder, std::stop_token stop)
        : encoder_(encoder), stop_(std::move(stop)), thread_([this] { Run(); }) {}
    ~FrameWriter() { Abandon(); }
    FrameWriter(const FrameWriter&) = delete;
    FrameWriter& operator=(const FrameWriter&) = delete;

    // Waits until at most `limit` frames handed over are not yet written. The first
    // write error, or None.
    EncodeError WaitUntilAtMost(size_t limit)
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return unwritten_ <= limit || error_ != EncodeError::None; });
        return error_;
    }
    void Push(const View& view)
    {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(view);
            ++unwritten_;
        }
        changed_.notify_all();
    }
    // Every frame handed over written, then the thread gone. The first write error, or None.
    EncodeError Finish() { Stop(false); return error_; }
    // The thread gone after the frame it is writing; the frames still queued are dropped.
    void Abandon() { Stop(true); }
    uint64_t Written() const { return written_.load(); }

private:
    void Stop(bool drop)
    {
        {
            std::lock_guard lock(mutex_);
            if (drop) { unwritten_ -= queue_.size(); queue_.clear(); }
            closing_ = true;
        }
        changed_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    void Run()
    {
        std::vector<uint8_t> packed;
        for (;;) {
            View view;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return closing_ || !queue_.empty(); });
                if (queue_.empty()) return;
                view = queue_.front();
                queue_.pop_front();
            }
            const EncodeError error = Write(view, packed);
            {
                std::lock_guard lock(mutex_);
                --unwritten_;
                if (error != EncodeError::None) {
                    error_ = error;
                    unwritten_ -= queue_.size();
                    queue_.clear();
                } else {
                    ++written_;
                }
            }
            changed_.notify_all();
            if (error != EncodeError::None) return;
        }
    }
    EncodeError Write(const View& view, std::vector<uint8_t>& packed)
    {
        const size_t row = size_t(view.width) * 4u;
        if (view.rowPitch == row) return encoder_.WriteFrame({view.base, row * view.height}, stop_);
        // A padded row pitch: the encoder reads the frame as one tightly packed block.
        packed.resize(row * view.height);
        for (uint32_t y = 0; y < view.height; ++y) std::memcpy(packed.data() + row * y, view.base + view.rowPitch * y, row);
        return encoder_.WriteFrame(packed, stop_);
    }

    RawVideoEncoder& encoder_;
    const std::stop_token stop_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<View> queue_;
    size_t unwritten_ = 0;  // handed over and not yet written: the queue and the frame being written
    bool closing_ = false;
    EncodeError error_ = EncodeError::None;
    std::atomic<uint64_t> written_{0};
    std::thread thread_;  // last: it starts running Run() as the object is built
};

} // namespace

VsrUpscaleResult RunVsrUpscalePass(const std::filesystem::path& helperDirectory, const VsrUpscaleRequest& request,
                                   std::stop_token stop, const std::function<void(const VsrUpscaleProgress&)>& progress)
{
    VsrUpscaleResult result;
    const auto fail = [&](std::wstring detail) {
        result.ok = false;
        result.detail = std::move(detail);
        LOG("RTX VSR export pass failed: " << utf8_text::FromWide(result.detail));
        return result;
    };
    if (!request.outputWidth || !request.outputHeight) return fail(L"The RTX VSR pass was given no output size.");

    VideoDecoder decoder;
    if (!decoder.Open(request.source.wstring(), MediaSourceKind::LocalFile))
        return fail(L"The source could not be opened for RTX VSR.");
    const uint32_t width = decoder.Width(), height = decoder.Height();
    const double fps = decoder.FrameRate();
    if (!width || !height || !(fps > 0.0)) return fail(L"The source has no frame size or rate to upscale.");
    const int64_t rangeStart = request.range.Whole() ? 0 : request.range.start100ns;
    const int64_t rangeEnd = request.range.Whole() ? 0 : request.range.end100ns;
    if (rangeStart > 0 && !decoder.SeekSeconds(double(rangeStart) * 1e-7))
        return fail(L"The source could not be read from the start of the range.");
    const double seconds = rangeEnd > rangeStart ? double(rangeEnd - rangeStart) * 1e-7
                                                 : std::max(0.0, decoder.DurationSeconds() - double(rangeStart) * 1e-7);
    VsrUpscaleProgress report{0, seconds > 0.0 ? uint64_t(std::llround(seconds * fps)) : 0};

    HiddenWindow window(request.outputWidth, request.outputHeight);
    if (!window.hwnd) return fail(L"The RTX VSR pass could not create its window.");
    auto renderer = MakeD3D12Renderer();
    renderer->SetPresentFollowsWindow(true);
    const auto [gridW, gridH] = TemporalGuideGenerator::AnalysisGrid(width, height, fps);
    if (!renderer->Initialize(window.hwnd, width, height, width, height, gridW, gridH, DefaultNeuralCarrierQuality()))
        return fail(L"The GPU could not be prepared for RTX VSR.");
    renderer->SetDLSS(false);
    if (renderer->VsrReason() != vsr_policy::Reason::Ready)
    {
        LOG("RTX VSR export pass: VSR not ready, reason " << static_cast<int>(renderer->VsrReason()));
        return fail(L"RTX VSR cannot run on this GPU or build; choose DLSS Super Resolution for this export.");
    }
    ComparisonSettings view;
    view.playbackVsr = true;
    view.playbackVsrQuality = request.quality;
    view.labels = false;
    renderer->SetComparison(view);

    EncoderSpec spec{request.outputWidth, request.outputHeight, fps,
                     request.encode == EncoderQuality::Lossless ? EncoderKind::Ffv1 : EncoderKind::HevcNvenc,
                     EncoderPixelFormat::Bgra};
    spec.nvencPreset = request.nvencPreset;
    spec.quality = request.encode;
    RawVideoEncoder encoder(helperDirectory);
    if (const EncodeError error = encoder.Start(spec, request.output); error != EncodeError::None) {
        if (error == EncodeError::Cancelled) { result.cancelled = true; return result; }
        return fail(L"The RTX VSR encoder could not be started.");
    }

    // kSlots frames in flight. Frame N's capture is enqueued before frame N-1's is
    // resolved, so the GPU renders and copies N while N-1 is handed to the writer, and
    // the writer writes N-1 while this thread decodes N+1. Frame N goes into slot
    // N % kSlots, which frame N - kSlots left, so it waits for that frame to be written.
    constexpr uint32_t kSlots = D3D12Renderer::ComposedReadbackSlots;
    FrameWriter writer(encoder, stop);
    uint64_t enqueued = 0;
    const auto handOver = [&](uint64_t index) {
        D3D12Renderer::ComposedReadbackView view;
        if (!renderer->ResolveComposedViewCapture(uint32_t(index % kSlots), view) ||
            view.width != request.outputWidth || view.height != request.outputHeight)
            return false;
        writer.Push(view);
        return true;
    };
    VideoFrame frame;
    const float frameMs = float(1000.0 / fps);
    bool first = true;
    EncodeError writeError = EncodeError::None;
    while (!stop.stop_requested() && decoder.ReadNext(frame)) {
        DrainMessages(window.hwnd);
        // A seek lands on or before the range start; frames before it are not the export's.
        if (frame.timestamp100ns < rangeStart) continue;
        if (rangeEnd > rangeStart && frame.timestamp100ns >= rangeEnd) break;
        if (!renderer->RenderFrame(frame.bgra.data(), frame.bgra.size(), nullptr, 0, gridW, gridH, first, false, frameMs))
            return fail(L"The GPU stopped while upscaling with RTX VSR.");
        first = false;
        if (!renderer->PlaybackVsrShown())
            return fail(L"RTX VSR did not upscale frame " + std::to_wstring(enqueued + 1) + L"; see the log.");
        // Handed over so far: every frame before N-1. Frame N - kSlots is written once
        // at most kSlots - 2 of them are not.
        if ((writeError = writer.WaitUntilAtMost(kSlots - 2)) != EncodeError::None) break;
        // The capture is the composed view in the encoder's BGRA, read back from the
        // same program and constants CaptureComposedView reads it with.
        if (!renderer->EnqueueComposedViewCapture(uint32_t(enqueued % kSlots)) || (enqueued && !handOver(enqueued - 1)))
            return fail(L"The upscaled frame could not be read back.");
        ++enqueued;
        const uint64_t written = writer.Written();
        if (progress && written / 8 != report.framesWritten / 8) { report.framesWritten = written; progress(report); }
    }
    if (writeError == EncodeError::None && enqueued && !stop.stop_requested()) {
        if (!handOver(enqueued - 1)) return fail(L"The upscaled frame could not be read back.");
        writeError = writer.Finish();
    } else {
        writer.Abandon();
    }
    result.framesWritten = writer.Written();
    report.framesWritten = result.framesWritten;
    result.evaluations = renderer->VsrEvaluations();
    if (writeError != EncodeError::None && writeError != EncodeError::Cancelled && !stop.stop_requested())
        return fail(L"The RTX VSR encoder stopped while writing.");
    if (stop.stop_requested()) { encoder.Cancel(); result.cancelled = true; return result; }
    if (!result.framesWritten) return fail(L"No frames were read for the RTX VSR pass.");
    if (const EncodeError error = encoder.Finish(stop); error != EncodeError::None) {
        if (error == EncodeError::Cancelled) { result.cancelled = true; return result; }
        return fail(L"The RTX VSR encode could not be finished.");
    }
    if (progress) progress(report);
    LOG("RTX VSR export pass: " << result.framesWritten << " frames " << width << "x" << height << " -> " << request.outputWidth
        << "x" << request.outputHeight << " at quality " << static_cast<int>(request.quality) << ", " << result.evaluations
        << " evaluations.");
    result.ok = true;
    return result;
}
