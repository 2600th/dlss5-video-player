#include "FrameGenerationPass.h"

#include "DLSSGBackend.h"
#include "Log.h"
#include "MediaPipeline.h"
#include "NeuralPreflight.h"
#include "VideoDecoder.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// The format the feature is created for and the frames travel in. It is the
// player's present format, it is what DlssgEvaluateSmoke measured production
// at, and it is what RawVideoEncoder consumes directly with
// EncoderPixelFormat::Bgra - so the pass never converts a pixel.
constexpr DXGI_FORMAT kBackbufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

// Where QueryFrameGenerationCapability asks its question. The cap is a property
// of the runtime and not of a geometry, and 1920x1080 is the geometry every
// DLSS-G measurement in this project was made at, so the answer is comparable
// with the ones already recorded in DLSSGBackend.h.
constexpr uint32_t kCapabilityProbeWidth = 1920;
constexpr uint32_t kCapabilityProbeHeight = 1080;

// A decoded frame has no depth. This is the flat proxy DlssgEvaluateSmoke
// handed over: one constant plane, clear of both ends of the runtime's
// linearization (lin = 1/(1-depth) with depthInverted false), which is singular
// at 1.0.
constexpr float kFlatDepthProxy = 0.5f;

void Append(std::wstring& detail, std::wstring sentence)
{
    if (sentence.empty()) return;
    if (!detail.empty()) detail += L"; ";
    detail += std::move(sentence);
}

std::wstring HexResult(NVSDK_NGX_Result result) { return HexResultTextWide(uint32_t(result)); }

const wchar_t* EncodeErrorText(EncodeError error)
{
    switch (error) {
    case EncodeError::None: return L"no error";
    case EncodeError::InvalidSpecification: return L"the encoder refused the specification";
    case EncodeError::HelperMissing: return L"FFmpeg is unavailable";
    case EncodeError::StartFailed: return L"FFmpeg could not be started";
    case EncodeError::WriteFailed: return L"the encoder stopped accepting frames";
    case EncodeError::FinishFailed: return L"FFmpeg did not finish the output file";
    case EncodeError::Cancelled: return L"the encode was cancelled";
    case EncodeError::InvalidFrame: return L"a frame of the wrong size reached the encoder";
    }
    return L"an unknown encoder error";
}

// The renderer's adapter choice (D3D12Renderer::CreateDevice), so the pass
// converts on the GPU the player renders on: DXGI's high-performance order,
// software adapters skipped, an NVIDIA one preferred over the first that merely
// creates a device.
ComPtr<IDXGIAdapter1> SelectAdapter(IDXGIFactory6* factory)
{
    ComPtr<IDXGIAdapter1> fallback;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr))) {
            continue;
        }
        if (!fallback) fallback = adapter;
        if (desc.VendorId == 0x10DE) return adapter;
    }
    return fallback;
}

// One command list, submitted and waited out between stages. NGX records the
// feature create on the list it is handed and NVIDIA's DLSS Programming Guide
// requires that work to have retired before the first evaluate, and the
// interpolated frames have to reach the CPU before they can be encoded, so this
// pass ends every stage at an idle GPU rather than pipelining. The conversion
// is bounded by NVENC and by the evaluate itself, not by this.
struct GpuContext {
    GpuContext() = default;
    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;
    ~GpuContext()
    {
        if (idle) CloseHandle(idle);
    }

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    HANDLE idle = nullptr;
    uint64_t signalled = 0;

    bool Flush()
    {
        if (FAILED(cmd->Close())) return false;
        ID3D12CommandList* lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), ++signalled))) return false;
        if (FAILED(fence->SetEventOnCompletion(signalled, idle))) return false;
        // A 1080p evaluate chain is milliseconds of GPU work; thirty seconds is
        // a hung or removed device, not a busy one.
        if (WaitForSingleObject(idle, 30000) != WAIT_OBJECT_0) return false;
        return SUCCEEDED(allocator->Reset()) && SUCCEEDED(cmd->Reset(allocator.Get(), nullptr));
    }
};

// Both entry points in this file bring the device up exactly here, so the
// capability query answers for the adapter the conversion would run on.
bool BringUpGpu(GpuContext& gpu, std::wstring& detail)
{
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        detail = L"DXGI could not be created on this machine.";
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter = SelectAdapter(factory.Get());
    if (!adapter) {
        detail = L"No Direct3D 12 hardware adapter is available.";
        return false;
    }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&gpu.device)))) {
        detail = L"A Direct3D 12 device could not be created on the selected adapter.";
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    gpu.idle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!gpu.idle ||
        FAILED(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&gpu.queue))) ||
        FAILED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&gpu.allocator))) ||
        FAILED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&gpu.cmd))) ||
        FAILED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)))) {
        detail = L"A Direct3D 12 command queue could not be created.";
        return false;
    }
    return true;
}

D3D12_RESOURCE_DESC Tex2D(uint32_t width, uint32_t height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

ComPtr<ID3D12Resource> CreateTexture(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                                     D3D12_RESOURCE_FLAGS flags, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_RESOURCE_DESC desc = Tex2D(width, height, format, flags);
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&texture)))) {
        return nullptr;
    }
    texture->SetName(name);
    return texture;
}

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(&buffer)))) {
        return nullptr;
    }
    return buffer;
}

void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &barrier);
}

struct Footprint {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
};

Footprint DescribeCopy(ID3D12Device* device, ID3D12Resource* texture)
{
    Footprint footprint;
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint.placed, &footprint.rows, &footprint.rowBytes,
                                  &footprint.totalBytes);
    return footprint;
}

// An upload staging buffer created once and refilled per frame. The conversion
// uploads one frame per source frame for the length of the file, and allocating
// and mapping a 3.5 MB buffer per frame is work the pass can simply not do: the
// GPU is idle at every frame boundary here (GpuContext::Flush), so the same
// buffer is safe to overwrite.
struct StagingUpload {
    ComPtr<ID3D12Resource> buffer;
    Footprint footprint;
    uint8_t* mapped = nullptr;

    bool Create(ID3D12Device* device, ID3D12Resource* texture)
    {
        footprint = DescribeCopy(device, texture);
        buffer = CreateBuffer(device, footprint.totalBytes, D3D12_HEAP_TYPE_UPLOAD,
                              D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!buffer) return false;
        // Mapped for the pass's lifetime: an upload heap stays CPU-visible, and
        // a map/unmap pair per frame buys nothing.
        return SUCCEEDED(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    }

    void Fill(const uint8_t* source, size_t sourcePitch) const
    {
        for (UINT row = 0; row < footprint.rows; ++row) {
            std::memcpy(mapped + size_t(row) * footprint.placed.Footprint.RowPitch,
                        source + size_t(row) * sourcePitch, size_t(footprint.rowBytes));
        }
    }

    void Record(ID3D12GraphicsCommandList* cmd, ID3D12Resource* texture) const
    {
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = buffer.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint.placed;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = texture;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        cmd->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
};

void RecordReadback(ID3D12GraphicsCommandList* cmd, ID3D12Resource* texture, ID3D12Resource* readback,
                    const Footprint& footprint)
{
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint.placed;
    cmd->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
}

// The encoder the pass writes through, carrying the project's NVENC-to-software
// fallback. ShouldRetryWithSoftware admits StartFailed, WriteFailed and
// FinishFailed, and a refused NVENC session is only visible to a raw-pipe
// producer as a broken pipe on the first frame (BuildEncoderArguments says why
// the CUDA device is created up front). So the retry is offered while nothing
// has been written yet, where re-sending the frame costs nothing, and never
// after - restarting the encoder mid-file would mean decoding the source again
// from the beginning, which is not a fallback but a second conversion.
class FallbackEncoder {
public:
    explicit FallbackEncoder(std::filesystem::path helperDirectory)
        : encoder_(std::move(helperDirectory)) {}

    EncodeError Start(const EncoderSpec& spec, const std::filesystem::path& output)
    {
        spec_ = spec;
        output_ = output;
        EncodeError error = encoder_.Start(spec_, output_);
        if (error != EncodeError::None && ShouldRetryWithSoftware(spec_.kind, error)) {
            spec_.kind = EncoderKind::H264Software;
            usedSoftware_ = true;
            error = encoder_.Start(spec_, output_);
        }
        return error;
    }

    EncodeError Write(std::span<const uint8_t> frame, std::stop_token stop)
    {
        EncodeError error = encoder_.WriteFrame(frame, stop);
        if (error != EncodeError::None && written_ == 0 && !stop.stop_requested() &&
            ShouldRetryWithSoftware(spec_.kind, error)) {
            spec_.kind = EncoderKind::H264Software;
            usedSoftware_ = true;
            error = encoder_.Start(spec_, output_);
            if (error != EncodeError::None) return error;
            error = encoder_.WriteFrame(frame, stop);
        }
        if (error == EncodeError::None) ++written_;
        return error;
    }

    EncodeError Finish(std::stop_token stop) { return encoder_.Finish(stop); }
    bool UsedSoftware() const { return usedSoftware_; }

private:
    RawVideoEncoder encoder_;
    EncoderSpec spec_{};
    std::filesystem::path output_;
    uint64_t written_ = 0;
    bool usedSoftware_ = false;
};

} // namespace

FrameGenerationPass::FrameGenerationPass(std::filesystem::path helperDirectory)
    : helperDirectory_(std::move(helperDirectory)) {}

FrameGenerationResult FrameGenerationPass::Run(const FrameGenerationRequest& request, std::stop_token stop,
                                               std::function<void(const FrameGenerationProgress&)> onProgress)
{
    FrameGenerationResult result;
    const auto fail = [&result](FrameGenerationError error, std::wstring detail) {
        result.ok = false;
        result.error = error;
        Append(result.detail, std::move(detail));
        return result;
    };
    const auto cancelled = [&fail] {
        return fail(FrameGenerationError::Cancelled, L"The frame-generation pass was cancelled.");
    };

    if (request.source.empty() || request.output.empty()) {
        return fail(FrameGenerationError::InvalidRequest,
                    L"A frame-generation pass needs a source file and an output file.");
    }
    if (request.multiplier < 2) {
        return fail(FrameGenerationError::InvalidRequest,
                    L"A multiplier below 2 generates no frames at all, so there is nothing for this pass to do.");
    }
    if (stop.stop_requested()) return cancelled();

    // Declared before the encoder and the backend so it is destroyed AFTER
    // them: the FFmpeg child has to be gone before the file it was writing is
    // deleted. Every failure path below is therefore a plain return - this
    // removes a half-written output, ~RawVideoEncoder terminates the child's
    // job object, and ~DLSSGBackend releases the feature and the NGX session.
    struct OutputGuard {
        std::filesystem::path path;
        bool keep = false;
        ~OutputGuard()
        {
            if (keep || path.empty()) return;
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } outputGuard{request.output};

    VideoDecoder::Settings settings;
    settings.helperDirectory = helperDirectory_.wstring();
    VideoDecoder decoder(std::move(settings));
    // Sequential, and deliberately NOT NV12. The feature is created for
    // B8G8R8A8_UNORM and the encoder consumes BGRA directly, so a BGRA decode
    // is the layout that reaches both without this pass owning a colour
    // conversion shader - and the decoder child is already doing the
    // conversion ffmpeg would otherwise do on the encode side.
    if (!decoder.OpenSequential(request.source.wstring(), MediaSourceKind::LocalFile, stop,
                                /*preferNv12=*/false)) {
        if (stop.stop_requested()) return cancelled();
        return fail(FrameGenerationError::Source, L"The source video could not be opened for decoding.");
    }
    if (decoder.IsStillImage()) {
        return fail(FrameGenerationError::InvalidRequest,
                    L"A still image has no successor frame to interpolate toward, so there is nothing to generate.");
    }

    const uint32_t width = decoder.Width();
    const uint32_t height = decoder.Height();
    const double sourceFps = decoder.FrameRate();
    const double sourceSeconds = decoder.DurationSeconds();
    result.width = width;
    result.height = height;
    result.sourceFps = sourceFps;
    if (!width || !height || !std::isfinite(sourceFps) || sourceFps <= 0.0) {
        return fail(FrameGenerationError::Source,
                    L"The source video reported no usable geometry or frame rate.");
    }
    const size_t frameBytes = FrameBytes(VideoPixelLayout::Bgra, width, height);

    GpuContext gpu;
    std::wstring deviceDetail;
    if (!BringUpGpu(gpu, deviceDetail)) return fail(FrameGenerationError::Device, std::move(deviceDetail));

    // The interpolated output is the only resource the runtime writes, so it is
    // the only one that needs a UAV, and a format that cannot carry one cannot
    // carry a generated frame either. Refused here with the reason rather than
    // by an invalid barrier later.
    D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport{kBackbufferFormat, D3D12_FORMAT_SUPPORT1_NONE,
                                                    D3D12_FORMAT_SUPPORT2_NONE};
    if (FAILED(gpu.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport,
                                               sizeof(formatSupport))) ||
        !(formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)) {
        return fail(FrameGenerationError::Device,
                    L"This adapter does not support an unordered-access view on B8G8R8A8_UNORM, which the "
                    L"generated frame is written through.");
    }

    DLSSGBackend backend;
    if (!backend.Initialize(gpu.device.Get(), gpu.cmd.Get(), width, height, kBackbufferFormat)) {
        // SessionEstablished separates the two failures the caller has to tell
        // apart: NGX never came up on this device at all, which is a device
        // verdict, from NGX coming up and the runtime refusing the feature,
        // which is a verdict about the runtime.
        const bool session = backend.SessionEstablished();
        return fail(session ? FrameGenerationError::Runtime : FrameGenerationError::Device,
                    (session ? L"The runtime refused CreateFeature(FrameGeneration): "
                             : L"NGX could not be initialized on this device: ") +
                        HexResult(backend.LastResult()) +
                        (session ? L". nvngx_dlssg.dll must be resolvable beside the executable." : L"."));
    }

    // The runtime's own ceiling, never a constant: MultiFrameCountMax is how
    // many frames it will generate between one source pair, so the largest
    // multiplier is one more than that.
    const uint32_t admitted = 1u + backend.MultiFrameCountMax();
    uint32_t multiplier = request.multiplier;
    if (multiplier > admitted) {
        Append(result.detail, L"The requested " + std::to_wstring(request.multiplier) +
                                  L"x was clamped to " + std::to_wstring(admitted) +
                                  L"x: this runtime reports DLSSG.MultiFrameCountMax=" +
                                  std::to_wstring(backend.MultiFrameCountMax()) + L".");
        multiplier = admitted;
    }
    const uint32_t generatedPerSource = multiplier - 1;
    result.outputFps = sourceFps * double(multiplier);

    ComPtr<ID3D12Resource> backbuffer =
        CreateTexture(gpu.device.Get(), width, height, kBackbufferFormat, D3D12_RESOURCE_FLAG_NONE,
                      L"FrameGeneration_SourceFrame");
    // Zero motion and a flat depth plane, uploaded once and read by every
    // evaluate. This is not a placeholder: DlssgEvaluateSmoke measured that a
    // motion buffer claiming nothing moved produces the same interpolated frame
    // as the true 200 px displacement does, so the tagged DLSSG.MVecs do not
    // drive this runtime's output and estimating them would be spending GPU
    // time to change nothing. The declaration still has to be honest, which is
    // why the buffer says "nothing moved" rather than carrying invented
    // vectors, and DLSSGBackend states an unreachable
    // motionVectorsInvalidValue so those zeros read as a measured still.
    ComPtr<ID3D12Resource> motion =
        CreateTexture(gpu.device.Get(), width, height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                      L"FrameGeneration_MotionVectors_Zero_RG16F");
    ComPtr<ID3D12Resource> depth =
        CreateTexture(gpu.device.Get(), width, height, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                      L"FrameGeneration_FlatDepthProxy_R32F");
    ComPtr<ID3D12Resource> interpolated =
        CreateTexture(gpu.device.Get(), width, height, kBackbufferFormat,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"FrameGeneration_OutputInterpolated");
    if (!backbuffer || !motion || !depth || !interpolated) {
        return fail(FrameGenerationError::Device, L"The conversion's textures could not be created.");
    }

    const Footprint outputFootprint = DescribeCopy(gpu.device.Get(), interpolated.Get());
    const size_t outputRowPitch = outputFootprint.placed.Footprint.RowPitch;
    // One readback per generated frame, so a whole source pair's generated
    // frames are produced in a single submission instead of one flush each.
    std::vector<ComPtr<ID3D12Resource>> readbacks(generatedPerSource);
    for (ComPtr<ID3D12Resource>& readback : readbacks) {
        readback = CreateBuffer(gpu.device.Get(), outputFootprint.totalBytes, D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_STATE_COPY_DEST);
        if (!readback) {
            return fail(FrameGenerationError::Device, L"The generated-frame readback buffers could not be created.");
        }
    }

    StagingUpload frameUpload;
    if (!frameUpload.Create(gpu.device.Get(), backbuffer.Get())) {
        return fail(FrameGenerationError::Device, L"The frame upload buffer could not be created.");
    }

    EncoderSpec spec;
    spec.width = width;
    spec.height = height;
    spec.fps = result.outputFps;
    spec.kind = EncoderKind::HevcNvenc;
    spec.pixelFormat = EncoderPixelFormat::Bgra;
    spec.nvencPreset = request.nvencPreset;
    if (ExpectedFrameBytes(spec) != frameBytes) {
        return fail(FrameGenerationError::InvalidRequest,
                    L"The source geometry and frame rate cannot be encoded at this multiplier.");
    }
    FallbackEncoder encoder(helperDirectory_);
    if (const EncodeError error = encoder.Start(spec, request.output); error != EncodeError::None) {
        return fail(error == EncodeError::Cancelled ? FrameGenerationError::Cancelled
                                                    : FrameGenerationError::Encoder,
                    std::wstring(L"The frame-generation encoder could not be started: ") +
                        EncodeErrorText(error) + L".");
    }

    using Clock = std::chrono::steady_clock;
    FrameGenerationProgress progress;
    // 0 when the source duration is unreadable, which is the "no answer" the
    // contract asks for rather than a guess a progress bar would divide by.
    progress.sourceFramesTotal = std::isfinite(sourceSeconds) && sourceSeconds > 0.0
                                     ? uint64_t(std::llround(sourceSeconds * sourceFps))
                                     : 0;
    Clock::time_point lastReport = Clock::now();
    const auto report = [&](bool force) {
        if (!onProgress) return;
        const Clock::time_point now = Clock::now();
        // The same 250 ms the download path reports on, and for the same
        // reason: one constant, so a UI that already paces itself to the
        // materializer does not need a second cadence for this.
        if (!force && now - lastReport < media_pipeline_detail::kMediaProgressInterval) return;
        lastReport = now;
        progress.framesWritten = result.framesWritten;
        onProgress(progress);
    };

    const auto writeFrame = [&](std::span<const uint8_t> frame) {
        const EncodeError error = encoder.Write(frame, stop);
        if (error == EncodeError::None) ++result.framesWritten;
        return error;
    };
    // Scratch for the readback repack below, allocated at most once for the
    // whole conversion: the readback row pitch is 256-aligned and the encoder
    // takes tightly packed frames, so a geometry whose tight pitch is not
    // already aligned needs the padding taken out. BGRA at 1280 and 1920 is
    // already aligned (5120 = 20*256, 7680 = 30*256) and is handed to the
    // encoder straight out of the mapped readback with no copy at all, which is
    // why this stays empty on the geometry this pass was measured at.
    std::vector<uint8_t> repacked;
    const auto encodeFailure = [&](EncodeError error) {
        return fail(error == EncodeError::Cancelled ? FrameGenerationError::Cancelled
                                                    : FrameGenerationError::Encoder,
                    std::wstring(L"The encoder stopped after ") + std::to_wstring(result.framesWritten) +
                        L" frames: " + EncodeErrorText(error) + L".");
    };

    // The first source frame, which also carries the feature-create work NGX
    // recorded on this list, plus the two guides every later evaluate reads.
    VideoFrame decoded;
    if (const VideoReadResult read = decoder.ReadNextBlocking(decoded, stop);
        read != VideoReadResult::FrameReady) {
        if (read == VideoReadResult::Cancelled || stop.stop_requested()) return cancelled();
        return fail(FrameGenerationError::Source, L"The source video produced no frames.");
    }
    if (decoded.bgra.size() != frameBytes) {
        return fail(FrameGenerationError::Source, L"The decoder produced a frame of an unexpected size.");
    }
    std::vector<uint8_t> previous = std::move(decoded.bgra);
    ++progress.sourceFramesRead;
    progress.sourceSeconds = double(decoded.timestamp100ns) / 10000000.0;

    {
        StagingUpload motionUpload, depthUpload;
        if (!motionUpload.Create(gpu.device.Get(), motion.Get()) ||
            !depthUpload.Create(gpu.device.Get(), depth.Get())) {
            return fail(FrameGenerationError::Device, L"The guide upload buffers could not be created.");
        }
        std::memset(motionUpload.mapped, 0, size_t(motionUpload.footprint.totalBytes));
        for (UINT row = 0; row < depthUpload.footprint.rows; ++row) {
            float* texels = reinterpret_cast<float*>(depthUpload.mapped +
                                                     size_t(row) * depthUpload.footprint.placed.Footprint.RowPitch);
            std::fill(texels, texels + width, kFlatDepthProxy);
        }
        motionUpload.Record(gpu.cmd.Get(), motion.Get());
        depthUpload.Record(gpu.cmd.Get(), depth.Get());
        frameUpload.Fill(previous.data(), size_t(width) * 4);
        frameUpload.Record(gpu.cmd.Get(), backbuffer.Get());
        // NON_PIXEL_SHADER_RESOURCE is the state the DLSS-SR path hands its
        // guides over in (D3D12Renderer::GuideReadState), so the inputs here
        // are in the same state NGX already reads guides from in this project.
        Barrier(gpu.cmd.Get(), motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(gpu.cmd.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(gpu.cmd.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(gpu.cmd.Get(), interpolated.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // The create and the uploads share this submission, and the wait is
        // what retires the create before the first evaluate reads it.
        if (!gpu.Flush()) {
            return fail(FrameGenerationError::Device,
                        L"The GPU did not retire the feature create and the first upload.");
        }
    }

    // Establishes the temporal history the pairs interpolate from. Its output
    // is discarded: there is no frame before the first one, so a frame
    // "generated" here would sit between nothing and something. reset=true is
    // the documented way to state that discontinuity, and it is the same call
    // DlssgEvaluateSmoke makes on frame A before the measured evaluate.
    const auto establishHistory = [&] {
        return backend.Evaluate(gpu.cmd.Get(), backbuffer.Get(), motion.Get(), depth.Get(), interpolated.Get(),
                                generatedPerSource, 1, /*reset=*/true) &&
               gpu.Flush();
    };
    if (!establishHistory()) {
        result.evaluations = backend.EvaluationCount();
        return fail(FrameGenerationError::Runtime,
                    L"The first DLSS-G evaluate failed: " + HexResult(backend.LastResult()));
    }
    result.evaluations = backend.EvaluationCount();
    report(true);

    for (;;) {
        if (stop.stop_requested()) return cancelled();

        const VideoReadResult read = decoder.ReadNextBlocking(decoded, stop);
        if (read == VideoReadResult::EndOfStream) break;
        if (read == VideoReadResult::Cancelled || stop.stop_requested()) return cancelled();
        if (read != VideoReadResult::FrameReady) {
            return fail(FrameGenerationError::Source,
                        L"The source video stopped decoding after " +
                            std::to_wstring(progress.sourceFramesRead) + L" frames.");
        }
        if (decoded.bgra.size() != frameBytes) {
            return fail(FrameGenerationError::Source, L"The decoder produced a frame of an unexpected size.");
        }
        ++progress.sourceFramesRead;
        progress.sourceSeconds = double(decoded.timestamp100ns) / 10000000.0;

        // Presentation order: the older frame of the pair, then what is
        // generated between it and the newer one.
        if (const EncodeError error = writeFrame(previous); error != EncodeError::None) {
            return encodeFailure(error);
        }

        Barrier(gpu.cmd.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        frameUpload.Fill(decoded.bgra.data(), size_t(width) * 4);
        frameUpload.Record(gpu.cmd.Get(), backbuffer.Get());
        Barrier(gpu.cmd.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // A discontinuity says these two frames are not a pair - a decoder
        // restart happened between them - so nothing is generated across it:
        // the evaluate is a reset that only re-establishes history, and the
        // slot is filled by holding the older frame. A scene cut is the same
        // situation and is NOT detected here; generating across a cut is a
        // known artifact of this pass, and TemporalGuides, which already
        // classifies cuts for Super Resolution, is where a later slice should
        // take that signal from.
        const bool pairIsContinuous = !decoded.discontinuity;
        if (!pairIsContinuous) {
            if (!establishHistory()) {
                result.evaluations = backend.EvaluationCount();
                return fail(FrameGenerationError::Runtime,
                            L"The DLSS-G evaluate after a decoder discontinuity failed: " +
                                HexResult(backend.LastResult()));
            }
            for (uint32_t held = 0; held < generatedPerSource; ++held) {
                if (const EncodeError error = writeFrame(previous); error != EncodeError::None) {
                    return encodeFailure(error);
                }
            }
        } else {
            for (uint32_t index = 1; index <= generatedPerSource; ++index) {
                if (!backend.Evaluate(gpu.cmd.Get(), backbuffer.Get(), motion.Get(), depth.Get(),
                                      interpolated.Get(), generatedPerSource, index, /*reset=*/false)) {
                    result.evaluations = backend.EvaluationCount();
                    return fail(FrameGenerationError::Runtime,
                                L"DLSS-G evaluate " + std::to_wstring(index) + L" of " +
                                    std::to_wstring(generatedPerSource) + L" failed after " +
                                    std::to_wstring(progress.sourceFramesRead) + L" source frames: " +
                                    HexResult(backend.LastResult()));
                }
                Barrier(gpu.cmd.Get(), interpolated.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
                RecordReadback(gpu.cmd.Get(), interpolated.Get(), readbacks[index - 1].Get(), outputFootprint);
                Barrier(gpu.cmd.Get(), interpolated.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            if (!gpu.Flush()) {
                result.evaluations = backend.EvaluationCount();
                return fail(FrameGenerationError::Device,
                            L"The GPU did not retire the generated frames for source frame " +
                                std::to_wstring(progress.sourceFramesRead) + L".");
            }
            result.evaluations = backend.EvaluationCount();

            for (uint32_t index = 0; index < generatedPerSource; ++index) {
                uint8_t* mapped = nullptr;
                if (FAILED(readbacks[index]->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
                    return fail(FrameGenerationError::Device, L"A generated frame could not be read back.");
                }
                EncodeError error = EncodeError::None;
                if (outputRowPitch == size_t(width) * 4) {
                    error = writeFrame(std::span<const uint8_t>(mapped, frameBytes));
                } else {
                    // Only reached where the tight pitch is not 256-aligned,
                    // where the padding has to come out before the encoder
                    // sees the frame.
                    if (repacked.size() != frameBytes) repacked.resize(frameBytes);
                    for (uint32_t row = 0; row < height; ++row) {
                        std::memcpy(repacked.data() + size_t(row) * width * 4,
                                    mapped + size_t(row) * outputRowPitch, size_t(width) * 4);
                    }
                    error = writeFrame(repacked);
                }
                readbacks[index]->Unmap(0, nullptr);
                if (error != EncodeError::None) return encodeFailure(error);
                ++result.generatedFrames;
            }
        }

        decoder.RecycleFrameBuffer(std::move(previous));
        previous = std::move(decoded.bgra);
        report(false);
    }

    if (stop.stop_requested()) return cancelled();

    // The last source frame, and then its slot: there is no successor to
    // interpolate toward, so it is held for the remaining multiplier-1 frames.
    // That is what brings the total to exactly sourceFrames * multiplier and
    // keeps the output the same length as the source.
    if (const EncodeError error = writeFrame(previous); error != EncodeError::None) {
        return encodeFailure(error);
    }
    for (uint32_t held = 0; held < generatedPerSource; ++held) {
        if (const EncodeError error = writeFrame(previous); error != EncodeError::None) {
            return encodeFailure(error);
        }
    }

    if (const EncodeError error = encoder.Finish(stop); error != EncodeError::None) {
        return fail(error == EncodeError::Cancelled ? FrameGenerationError::Cancelled
                                                    : FrameGenerationError::Encoder,
                    std::wstring(L"The converted file was not finished: ") + EncodeErrorText(error) + L".");
    }
    if (encoder.UsedSoftware()) {
        Append(result.detail, L"NVENC refused this encode, so it was completed with the software H.264 encoder.");
    }

    outputGuard.keep = true;
    result.ok = true;
    result.error = FrameGenerationError::None;
    report(true);
    LOG("Frame generation wrote " << std::dec << result.framesWritten << " frames ("
        << result.generatedFrames << " generated by DLSS-G, " << result.evaluations << " evaluates) from "
        << progress.sourceFramesRead << " source frames at " << width << "x" << height << " "
        << result.sourceFps << " -> " << result.outputFps << " fps");
    return result;
}

FrameGenerationCapability QueryFrameGenerationCapability() noexcept
{
    FrameGenerationCapability capability;
    // Never throws, because the player asks this from a worker thread while it
    // is deciding what to offer the user: a bad_alloc out of a diagnostic
    // string must degrade to "not available", not unwind into the caller.
    try {
        GpuContext gpu;
        std::wstring deviceDetail;
        if (!BringUpGpu(gpu, deviceDetail)) {
            capability.detail = std::move(deviceDetail);
            return capability;
        }

        DLSSGBackend backend;
        const DLSSGCapability probed = backend.Probe(gpu.device.Get(), gpu.cmd.Get(), kCapabilityProbeWidth,
                                                     kCapabilityProbeHeight, kBackbufferFormat);
        // Probe releases the feature but the create work it recorded is still
        // on this list, so it is submitted and waited out: a create the runtime
        // accepted but the GPU could not execute has to surface here rather
        // than be reported as an admission.
        const bool flushed = gpu.Flush();
        backend.Shutdown();

        capability.available = probed.available && flushed;
        capability.multiFrameCountMax = capability.available ? probed.multiFrameCountMax : 0;
        capability.detail = probed.detail;
        if (!capability.available) {
            Append(capability.detail,
                   (probed.available ? L"the feature was admitted but the GPU did not retire its create: "
                                     : L"CreateFeature(FrameGeneration) did not admit the feature: ") +
                       HexResultTextWide(uint32_t(probed.createResult)));
        }
    } catch (...) {
        capability.available = false;
        capability.multiFrameCountMax = 0;
        capability.detail = L"The frame-generation capability query failed unexpectedly.";
    }
    return capability;
}
