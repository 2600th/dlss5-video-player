#include "VsrUpscalePass.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
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

    VideoFrame frame;
    std::vector<uint8_t> composed;
    const float frameMs = float(1000.0 / fps);
    bool first = true;
    while (!stop.stop_requested() && decoder.ReadNext(frame)) {
        DrainMessages(window.hwnd);
        // A seek lands on or before the range start; frames before it are not the export's.
        if (frame.timestamp100ns < rangeStart) continue;
        if (rangeEnd > rangeStart && frame.timestamp100ns >= rangeEnd) break;
        uint32_t composedW = 0, composedH = 0;
        if (!renderer->RenderFrame(frame.bgra.data(), frame.bgra.size(), nullptr, 0, gridW, gridH, first, false, frameMs))
            return fail(L"The GPU stopped while upscaling with RTX VSR.");
        first = false;
        if (!renderer->PlaybackVsrShown())
            return fail(L"RTX VSR did not upscale frame " + std::to_wstring(result.framesWritten + 1) + L"; see the log.");
        if (!renderer->CaptureComposedView(composed, composedW, composedH) ||
            composedW != request.outputWidth || composedH != request.outputHeight)
            return fail(L"The upscaled frame could not be read back.");
        // The capture is RGBA; the encoder takes BGRA.
        for (size_t i = 0; i + 3 < composed.size(); i += 4) std::swap(composed[i], composed[i + 2]);
        if (const EncodeError error = encoder.WriteFrame(composed, stop); error != EncodeError::None) {
            if (error == EncodeError::Cancelled || stop.stop_requested()) break;
            return fail(L"The RTX VSR encoder stopped while writing.");
        }
        ++result.framesWritten;
        report.framesWritten = result.framesWritten;
        if (progress && (result.framesWritten % 8 == 0)) progress(report);
    }
    result.evaluations = renderer->VsrEvaluations();
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
