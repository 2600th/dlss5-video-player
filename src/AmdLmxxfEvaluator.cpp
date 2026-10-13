#include "AmdLmxxfEvaluator.h"

#include "AmdLmxxfPolicy.h"
#include "AmdTemporalAccumulationShader.h"
#include "AmdUpscaleShader.h"
#include "GuideFiles.h"
#include "TemporalStabilityShader.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <d3dcompiler.h>
#include <system_error>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

// The colour resource is handed to the runtime as a shader input, so the state
// the caller declares and the state the upload pass leaves it in are the same
// pair: both the pixel and the non-pixel shader read bit. The stage A probe
// proved this pair against the real binary; it is not a guess.
constexpr D3D12_RESOURCE_STATES kColorState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// What the runtime's own code leaves job.private_output in: PrepareFrame creates
// it in NON_PIXEL_SHADER_RESOURCE and RecordOutputs' copy-out restores that
// state, so the readback starts from exactly where the runtime left it and puts
// it back. Declaring COMMON here (the stage A probe did, copying after Retire)
// works by accident of the promotion rule; this does not rely on that.
constexpr D3D12_RESOURCE_STATES kPrivateOutputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

constexpr DWORD kFenceWaitMs = 30000;

// The state the guide textures are left in and the state every frame declares
// them in. Unlike the colour texture they are not written by the runtime, so
// this is a state the host owns end to end: a rewrite brackets its copy with a
// transition out of it and back into it, in the same list, so the frame info
// always describes the truth and the runtime never has to guess.
constexpr D3D12_RESOURCE_STATES kGuideState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Collection answers UNAVAILABLE while the runtime still holds the job, and it
// is asked only after the fence that covers the submission has been waited on,
// so the first answer is normally OK. The bound is so a runtime that never
// releases cannot spin this frame loop forever.
constexpr int kCollectAttempts = 64;

// The temporal accumulation's descriptor layout, laid out by hand because the
// heap is small and a wrong index here reads somebody else's texture without
// saying so.
//
// Exactly one of these is rewritten between frames: the runtime publishes a new
// private_output every frame, so decode's input is the descriptor that cannot
// be built once. Everything else is either a texture that never changes or a
// choice between two tables - the accumulation's t1/t3/t4 name the history slot
// the pass is reading, so which table is bound is which slot it reads and no
// descriptor is edited to change it.
constexpr UINT kSrvDecode = 0;
constexpr UINT kSrvEncodeBase = 1;          // + the history slot being encoded
constexpr UINT kSrvAccumulateBase = 3;      // + the history slot being read * 5
constexpr UINT kAccumulateTableSize = 5;    // t0..t4, the pass's whole binding
constexpr UINT kSrvCount = 3 + 2 * kAccumulateTableSize;

constexpr UINT kRtvStabilised = 0;
constexpr UINT kRtvHistoryBase = 1;         // + the history slot being written
constexpr UINT kRtvNeuralLinear = 3;
constexpr UINT kRtvCount = 4;

std::string Narrow(const std::wstring& text)
{
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                          nullptr, nullptr);
    return out;
}

// A separate name rather than an overload: a Windows wide character array
// converts to both std::wstring and std::filesystem::path, so an overload set
// makes every DXGI_ADAPTER_DESC1::Description call ambiguous.
std::string NarrowPath(const std::filesystem::path& path) { return Narrow(path.wstring()); }

// Eight hex digits, which is the shape every HRESULT takes in this file's log.
std::string Hex32(long value)
{
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(value));
    return buffer;
}

// One transition of one resource, whole-resource. The frame path has enough
// barriers in a row that writing them out in full at each site buries the order
// they have to be in; this is the same barrier the file already builds by hand,
// with the three fields that change as arguments.
void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

// A descriptor handle `index` entries along a heap, in the two flavours the
// frame path needs: the CPU one for creating views, the GPU one for binding
// them.
D3D12_CPU_DESCRIPTOR_HANDLE OffsetHandle(D3D12_CPU_DESCRIPTOR_HANDLE base, UINT size, UINT index)
{
    base.ptr += static_cast<SIZE_T>(size) * index;
    return base;
}

D3D12_GPU_DESCRIPTOR_HANDLE OffsetHandle(D3D12_GPU_DESCRIPTOR_HANDLE base, UINT size, UINT index)
{
    base.ptr += static_cast<UINT64>(size) * index;
    return base;
}

// A compiler blob as text, without the terminator GetBufferSize() counts. Used
// for the messages D3DCompile and D3D12SerializeRootSignature hand back, which
// are the only place a shader problem is ever explained.
std::string BlobText(ID3DBlob* blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr) return {};
    std::string text(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
    while (!text.empty() && text.back() == '\0') text.pop_back();
    return text;
}

} // namespace

AmdLmxxfEvaluator::AmdLmxxfEvaluator(AmdLmxxfEvaluatorConfig config) : config_(std::move(config))
{
    if (config_.modulesDirectory.empty())
        config_.modulesDirectory = config_.runtimeDirectory / "lmxxf-modules";
    if (config_.weightsDirectory.empty())
        config_.weightsDirectory = config_.runtimeDirectory / "native-game-tiled-assets";
}

AmdLmxxfEvaluator::~AmdLmxxfEvaluator()
{
    TearDown();
}

void AmdLmxxfEvaluator::Log(std::string_view text) const
{
    if (config_.log) config_.log(text);
}

void AmdLmxxfEvaluator::Report(std::string_view call, int32_t status)
{
    if (!config_.log) return;
    std::string line("  ");
    line += call;
    line += " -> ";
    line += amd_lmxxf::LmxxfNrStatusName(status);
    line += " (";
    line += std::to_string(status);
    line += ")";
    if (status != static_cast<int32_t>(LMXXF_NR_OK)) {
        const std::string runtimeError = runtime_.LastError();
        if (!runtimeError.empty()) {
            line += " | runtime: ";
            line += runtimeError;
        }
        if (session_.has_value() && !session_->LastError().empty()) {
            line += " | session: ";
            line += session_->LastError();
        }
        const std::string runtimeStatus = runtime_.Status();
        if (!runtimeStatus.empty()) {
            line += " | status: ";
            line += runtimeStatus;
        }
    }
    config_.log(line);
}

bool AmdLmxxfEvaluator::Fail(NeuralRenderFailure failure, std::string detail)
{
    lastFailure_ = failure;
    lastError_ = std::move(detail);
    return false;
}

bool AmdLmxxfEvaluator::Refuse(std::string detail)
{
    lastError_ = std::move(detail);
    return false;
}

bool AmdLmxxfEvaluator::Ready() const noexcept
{
    if (!(prepared_ && session_.has_value() && device_ != nullptr && queue_ != nullptr &&
          color_ != nullptr && uploadMapped_ != nullptr && readback_ != nullptr))
        return false;
    // A rung is not something this evaluator can attempt without its pass, and
    // Ready() is what every frame path asks before it starts: a frame that got
    // past this point and then found no pass would already have uploaded itself
    // and be halfway through a command list.
    if (config_.stability == TemporalStability::Off) return true;
    return psoDecode_ != nullptr && psoAccumulate_ != nullptr && psoEncode_ != nullptr &&
           convertRootSig_ != nullptr && accumulationRootSig_ != nullptr &&
           sourceSlot_[0] != nullptr && sourceSlot_[1] != nullptr &&
           historySlot_[0] != nullptr && historySlot_[1] != nullptr &&
           neuralLinear_ != nullptr && stabilised_ != nullptr && zeroMotion_ != nullptr;
}

bool AmdLmxxfEvaluator::BringUpDevice(std::string_view why)
{
    if (device_ != nullptr && queue_ != nullptr) return true;

    if (config_.device != nullptr && config_.queue != nullptr) {
        device_ = config_.device;
        queue_ = config_.queue;
        Log(std::string("borrowed D3D12 device and queue (") + std::string(why) + ")");
    } else {
        Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
        if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
            return Refuse("CreateDXGIFactory2 failed, so no adapter can be enumerated");

        Microsoft::WRL::ComPtr<IDXGIAdapter1> chosen;
        Microsoft::WRL::ComPtr<ID3D12Device> chosenDevice;
        for (UINT index = 0;; ++index) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                    IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(adapter->GetDesc1(&description))) continue;
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            if (FAILED(::D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                           IID_PPV_ARGS(&device))))
                continue;
            // Only gfx1200/gfx1201 modules exist, so the AMD adapter is the one
            // to hand the runtime. Anything else is kept as a fallback so a
            // wrong adapter is reported by the runtime's own Create error
            // rather than here.
            if (!chosenDevice) {
                chosen = adapter;
                chosenDevice = device;
            }
            if (description.VendorId == amd_lmxxf::kAmdVendorId) {
                chosen = adapter;
                chosenDevice = device;
                DXGI_ADAPTER_DESC1 amdDescription{};
                if (SUCCEEDED(chosen->GetDesc1(&amdDescription)))
                    adapterName_ = Narrow(amdDescription.Description);
                break;
            }
        }
        if (!chosenDevice) return Refuse("no D3D12 hardware adapter is available");
        if (adapterName_.empty()) {
            DXGI_ADAPTER_DESC1 fallback{};
            if (SUCCEEDED(chosen->GetDesc1(&fallback))) adapterName_ = Narrow(fallback.Description);
        }

        device_ = chosenDevice;
        // IDXGIAdapter3 is what carries QueryVideoMemoryInfo, and the adapter we
        // just enumerated is the shortest honest way to it.
        if (chosen != nullptr) (void)chosen.As(&adapter_);
        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device_->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue_))))
            return Refuse("CreateCommandQueue failed");
        Log("created a D3D12 device on " + adapterName_ + " (" + std::string(why) + ")");
    }

    // A BORROWED device has no enumerated adapter to reach for, and
    // IDXGIDevice::GetAdapter did not hand back one that answered IDXGIAdapter3,
    // so the adapter is resolved by the LUID the device reports. That also proves
    // the borrowed device IS the adapter the runtime is about to be handed, which
    // is the check a stage C worker needs. Without this the video memory sampling
    // had nothing to query on that path and silently reported zero.
    if (adapter_ == nullptr) {
        const LUID luid = device_->GetAdapterLuid();
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        if (SUCCEEDED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> match;
            if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&match))) && match != nullptr)
                (void)match.As(&adapter_);
        }
        if (adapter_ == nullptr)
            Log("no IDXGIAdapter3 matched the D3D12 device's adapter LUID; video memory sampling "
                "will report nothing");
    }
    if (adapterName_.empty() && adapter_ != nullptr) {
        DXGI_ADAPTER_DESC1 borrowedDescription{};
        if (SUCCEEDED(adapter_->GetDesc1(&borrowedDescription)))
            adapterName_ = Narrow(borrowedDescription.Description);
    }

    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&allocator_))))
        return Refuse("CreateCommandAllocator failed");
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(),
                                          nullptr, IID_PPV_ARGS(&list_))))
        return Refuse("CreateCommandList failed");
    if (FAILED(list_->Close())) return Refuse("the initial command list would not close");
    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
        return Refuse("CreateFence failed");
    fenceEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent_ == nullptr) return Refuse("CreateEventW failed for the frame fence");
    return true;
}

bool AmdLmxxfEvaluator::CreateFrameResources()
{
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC colorDescription{};
    colorDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    colorDescription.Width = width_;
    colorDescription.Height = height_;
    colorDescription.DepthOrArraySize = 1;
    colorDescription.MipLevels = 1;
    colorDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    colorDescription.SampleDesc.Count = 1;
    colorDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    colorDescription.Flags =
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
                                                &colorDescription,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&color_))))
        return Refuse("CreateCommittedResource for the colour texture failed");

    UINT rowCount = 0;
    UINT64 rowBytes = 0;
    UINT64 uploadBytes = 0;
    device_->GetCopyableFootprints(&colorDescription, 0, 1, 0, &footprint_, &rowCount, &rowBytes,
                                   &uploadBytes);
    if (rowBytes < static_cast<UINT64>(width_) * 4u || rowCount < height_)
        return Refuse("GetCopyableFootprints reports a footprint too small for the frame");

    D3D12_RESOURCE_DESC bufferDescription{};
    bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDescription.Width = uploadBytes;
    bufferDescription.Height = 1;
    bufferDescription.DepthOrArraySize = 1;
    bufferDescription.MipLevels = 1;
    bufferDescription.Format = DXGI_FORMAT_UNKNOWN;
    bufferDescription.SampleDesc.Count = 1;
    bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
                                                &bufferDescription,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&upload_))))
        return Refuse("CreateCommittedResource for the upload buffer failed");

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(device_->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
                                                &bufferDescription,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&readback_))))
        return Refuse("CreateCommittedResource for the readback buffer failed");

    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(upload_->Map(0, &noRead, &mapped)))
        return Refuse("the upload buffer would not map");
    uploadMapped_ = static_cast<uint8_t*>(mapped);

    colorState_ = D3D12_RESOURCE_STATE_COPY_DEST;
    colorUploaded_ = false;
    Log("frame resources: " + std::to_string(width_) + "x" + std::to_string(height_) +
        " BGRA, row pitch " + std::to_string(footprint_.Footprint.RowPitch) + ", readback " +
        std::to_string(uploadBytes) + " bytes, one persistent colour texture");
    return true;
}

bool AmdLmxxfEvaluator::CreateGuideTextures()
{
    // Motion is sampled as a pair and depth as one channel, and both formats are
    // in the runtime's whitelist, so neither is a conversion it has to do.
    //
    // They are FULL FRAME rather than the analysis grid a video host would
    // naturally compute. The runtime normalises the stored vector against
    // motion_width/height and the kernel divides by motion_width, so handing it
    // the frame's own width is the reading that needs no calibration: the scale
    // comes out at 1 and the stored value IS the frame-pixel displacement. An
    // analysis grid would put a second unknown on top of an unmeasured one.
    //
    // A file source therefore arrives at this size too, not at whatever size it
    // was written: UploadGuides resamples each frame's field onto this texture
    // before the runtime sees it, so the frame info can declare the same
    // motion_width/height and the same scale every frame whichever source the
    // guides came from.
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC motionDescription{};
    motionDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    motionDescription.Width = width_;
    motionDescription.Height = height_;
    motionDescription.DepthOrArraySize = 1;
    motionDescription.MipLevels = 1;
    motionDescription.Format = DXGI_FORMAT_R32G32_FLOAT;
    motionDescription.SampleDesc.Count = 1;
    motionDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // Created as a copy destination because that is what the upload needs; the
    // barrier that follows leaves them in kGuideState, and each per-frame rewrite
    // transitions back out of it and into it again.
    if (FAILED(device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
                                                &motionDescription,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&motion_))))
        return Refuse("CreateCommittedResource for the motion guide failed");

    D3D12_RESOURCE_DESC depthDescription = motionDescription;
    depthDescription.Format = DXGI_FORMAT_R32_FLOAT;
    if (FAILED(device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
                                                &depthDescription,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&depth_))))
        return Refuse("CreateCommittedResource for the depth guide failed");

    // One upload buffer carrying both. It stays mapped for the life of the
    // session: the initial fill below is the constant answer, and every frame
    // after it rewrites the same bytes in place when a guide directory was
    // configured, so nothing here allocates per frame.
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 motionBytes = 0;
    UINT64 depthBytes = 0;
    device_->GetCopyableFootprints(&motionDescription, 0, 1, 0, &motionFootprint_, &rows, &rowBytes,
                                   &motionBytes);
    const UINT64 depthBase = (motionBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u) /
                             D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT *
                             D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    device_->GetCopyableFootprints(&depthDescription, 0, 1, depthBase, &depthFootprint_, &rows,
                                   &rowBytes, &depthBytes);

    D3D12_RESOURCE_DESC bufferDescription{};
    bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDescription.Width = depthBase + depthBytes;
    bufferDescription.Height = 1;
    bufferDescription.DepthOrArraySize = 1;
    bufferDescription.MipLevels = 1;
    bufferDescription.Format = DXGI_FORMAT_UNKNOWN;
    bufferDescription.SampleDesc.Count = 1;
    bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
                                                &bufferDescription,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&guideUpload_))))
        return Refuse("CreateCommittedResource for the guide upload buffer failed");

    {
        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        if (FAILED(guideUpload_->Map(0, &noRead, &mapped)))
            return Refuse("the guide upload buffer would not map");
        guideUploadMapped_ = static_cast<uint8_t*>(mapped);
        auto* base = guideUploadMapped_;
        std::fill_n(base, static_cast<size_t>(bufferDescription.Width), uint8_t{0});
        for (uint32_t row = 0; row < height_; ++row) {
            auto* motionRow = reinterpret_cast<float*>(
                base + motionFootprint_.Offset +
                static_cast<size_t>(row) * motionFootprint_.Footprint.RowPitch);
            auto* depthRow = reinterpret_cast<float*>(
                base + depthFootprint_.Offset +
                static_cast<size_t>(row) * depthFootprint_.Footprint.RowPitch);
            for (uint32_t column = 0; column < width_; ++column) {
                motionRow[column * 2u + 0] = config_.motionPixelsPerFrameX;
                motionRow[column * 2u + 1] = config_.motionPixelsPerFrameY;
                depthRow[column] = config_.depthValue;
            }
        }
        // Deliberately left mapped. The per-frame rewrite writes through this
        // same pointer, and unmapping between frames would only add a pair of
        // Map/Unmap calls to a path that is already waiting on the queue.
    }

    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr)))
        return Refuse("the command list would not reset for the guide upload");

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = guideUpload_.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

    destination.pResource = motion_.Get();
    source.PlacedFootprint = motionFootprint_;
    list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    destination.pResource = depth_.Get();
    source.PlacedFootprint = depthFootprint_;
    list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    // Straight into the read state. This one transition is also what makes the
    // initial fill visible to the runtime; a per-frame rewrite repeats the same
    // pair of transitions around its own copy.
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (int index = 0; index < 2; ++index) {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index].Transition.pResource = index == 0 ? motion_.Get() : depth_.Get();
        barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[index].Transition.StateAfter = kGuideState;
    }
    list_->ResourceBarrier(2, barriers);

    if (FAILED(list_->Close())) return Refuse("the guide upload list would not close");
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    if (!WaitForGpu()) return false;

    std::string guideSource = "held for the whole run";
    if (!config_.motionGuideDirectory.empty() || !config_.depthGuideDirectory.empty()) {
        guideSource = "initial contents only, rewritten every frame from ";
        guideSource += config_.motionGuideDirectory.empty()
                           ? "no motion directory (motion stays constant)"
                           : NarrowPath(config_.motionGuideDirectory);
        guideSource += " and ";
        guideSource += config_.depthGuideDirectory.empty()
                           ? "no depth directory (depth stays constant)"
                           : NarrowPath(config_.depthGuideDirectory);
    }
    Log("temporal guides: motion " + std::to_string(width_) + "x" + std::to_string(height_) +
        " R32G32_FLOAT carrying (" + std::to_string(config_.motionPixelsPerFrameX) + ", " +
        std::to_string(config_.motionPixelsPerFrameY) + ") px/frame, depth R32_FLOAT = " +
        std::to_string(config_.depthValue) + ", " + guideSource +
        "; one upload buffer and one texture pair, pointers and sizes fixed for the life of the "
        "session");
    return true;
}

bool AmdLmxxfEvaluator::UploadGuides(ID3D12GraphicsCommandList* list, uint64_t sourceFrameNumber)
{
    // No directories is the constant-guide run 5 path: nothing to read, nothing
    // to record, and the textures keep whatever CreateGuideTextures put in them.
    if (config_.motionGuideDirectory.empty() && config_.depthGuideDirectory.empty()) return true;
    if (guideUploadMapped_ == nullptr)
        return Refuse("the guide upload buffer is not mapped, so a per-frame guide cannot be written");

    // Both guides go through one read, so the failure text names the guide and
    // the file rather than only the frame.
    struct Updated {
        ID3D12Resource* texture;
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* footprint;
    };
    Updated updated[2]{};
    uint32_t updatedCount = 0;
    std::vector<float> motionValues;
    std::vector<float> depthValues;

    auto read = [&](const std::filesystem::path& directory, uint32_t channels, const char* what,
                    std::vector<float>& values) {
        const std::filesystem::path path = guide_files::FramePath(directory, sourceFrameNumber);
        std::string why;
        const std::optional<guide_files::Plane> plane = guide_files::ReadPfmFile(path, &why);
        if (!plane)
            return Refuse(std::string(what) + " guide for source frame " +
                          std::to_string(sourceFrameNumber) + ": " + why);
        if (plane->channels != channels)
            return Refuse(std::string(what) + " guide " + NarrowPath(path) + " has " +
                          std::to_string(plane->channels) + " channels; a " +
                          (channels == 1 ? "depth map is one (Pf)" : "motion field is three (PF)"));
        // Any size, resampled onto the texture. The field a video host computes
        // is the analysis grid - 160x90 for a 30 fps 1080p source - and this is
        // the same area average the renderer's own expansion pass applies, so a
        // file at the grid size blocks out exactly the way the estimator's own
        // field does. At equal size it copies rather than averages.
        values = guide_files::ResampleArea(*plane, width_, height_).values;
        return true;
    };

    bool writeMotion = false;
    bool writeDepth = false;
    if (!config_.motionGuideDirectory.empty()) {
        if (!read(config_.motionGuideDirectory, 3, "motion", motionValues)) return false;
        updated[updatedCount++] = {motion_.Get(), &motionFootprint_};
        writeMotion = true;
    }
    if (!config_.depthGuideDirectory.empty()) {
        if (!read(config_.depthGuideDirectory, 1, "depth", depthValues)) return false;
        updated[updatedCount++] = {depth_.Get(), &depthFootprint_};
        writeDepth = true;
    }

    // Row by row rather than one memcpy, because the footprint's row pitch is
    // the 256-byte-aligned one the copy needs and not the tight row.
    auto write = [&](const std::vector<float>& values,
                     const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, uint32_t channels) {
        for (uint32_t row = 0; row < height_; ++row) {
            auto* target = reinterpret_cast<float*>(
                guideUploadMapped_ + footprint.Offset +
                static_cast<size_t>(row) * footprint.Footprint.RowPitch);
            std::copy_n(values.data() + static_cast<size_t>(row) * width_ * channels,
                        static_cast<size_t>(width_) * channels, target);
        }
    };
    if (writeMotion) write(motionValues, motionFootprint_, 2u);
    if (writeDepth) write(depthValues, depthFootprint_, 1u);

    // Out of the read state, in with the copy, back into the read state - all in
    // this frame's producer list, which is what the runtime runs behind on the
    // same queue. The frame info this frame declares says kGuideState, and it is
    // the truth both before the list executes (the previous frame left them
    // there) and after it.
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (uint32_t index = 0; index < updatedCount; ++index) {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index].Transition.pResource = updated[index].texture;
        barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[index].Transition.StateBefore = kGuideState;
        barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    list->ResourceBarrier(updatedCount, barriers);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = guideUpload_.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    for (uint32_t index = 0; index < updatedCount; ++index) {
        destination.pResource = updated[index].texture;
        source.PlacedFootprint = *updated[index].footprint;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }

    for (uint32_t index = 0; index < updatedCount; ++index)
        std::swap(barriers[index].Transition.StateBefore, barriers[index].Transition.StateAfter);
    list->ResourceBarrier(updatedCount, barriers);

    ++guideFrames_;
    return true;
}

bool AmdLmxxfEvaluator::BringUpRuntime()
{
    // The runtime reads its weights and shaders through the environment rather
    // than through Create. It says so in its own error text: "set
    // LMXXF_WEIGHTS_DIR to tiled assets, not 0.24.2 HIP/".
    std::error_code weightsError;
    if (std::filesystem::exists(config_.weightsDirectory, weightsError)) {
        ::SetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", config_.weightsDirectory.wstring().c_str());
        Log("LMXXF_WEIGHTS_DIR = " + NarrowPath(config_.weightsDirectory));
    } else {
        Log("LMXXF_WEIGHTS_DIR not set: " + NarrowPath(config_.weightsDirectory) + " is absent");
    }

    std::wstring loadError;
    if (!runtime_.Load(config_.runtimeDirectory, &loadError))
        return Refuse("Runtime::Load: " + Narrow(loadError));
    Log("LmxxfNrRuntime loaded from " + NarrowPath(runtime_.ModulePath()));
    Log("  abi_version " + std::to_string(runtime_.Api().abi_version) + ", table " +
        std::to_string(runtime_.Api().struct_size) + " bytes");

    std::wstring capabilityError;
    if (!runtime_.QueryCapabilities(&capabilityError))
        return Refuse("QueryCapabilities: " + Narrow(capabilityError));
    const LmxxfNrCapabilities& capabilities = runtime_.Capabilities();
    Log("  capabilities: max input " + std::to_string(capabilities.max_input_width) + "x" +
        std::to_string(capabilities.max_input_height) + ", history_supported " +
        std::to_string(capabilities.history_supported) + ", overlap " +
        std::to_string(capabilities.overlap_supported) + ", graph " +
        std::to_string(capabilities.graph_supported));
    if (width_ > capabilities.max_input_width || height_ > capabilities.max_input_height) {
        // Admission is by pixel budget, so an ultrawide narrower than the
        // always-admitted box is still legal - but a size over both axes is not,
        // and the runtime would refuse it later with a less specific message.
        if (static_cast<uint64_t>(width_) * height_ >
            static_cast<uint64_t>(capabilities.max_input_width) * capabilities.max_input_height)
            return Refuse("the frame " + std::to_string(width_) + "x" + std::to_string(height_) +
                          " exceeds the runtime's admitted pixel budget " +
                          std::to_string(capabilities.max_input_width) + "x" +
                          std::to_string(capabilities.max_input_height));
    }

    std::error_code directoryError;
    if (!std::filesystem::is_directory(config_.modulesDirectory, directoryError))
        return Refuse("the modules directory " + NarrowPath(config_.modulesDirectory) +
                      " does not exist; Create wants the 40-per-arch lmxxf-modules layout, not "
                      "native-game-tiled-assets/HIP");
    // The temporal model runs under the lease lifecycle and nowhere else, so the
    // create flag and the frame path are one decision rather than two. Asking
    // for history without it is what makes the status line answer
    // "unsupported-lifecycle" before a single guide check runs.
    leases_ = config_.temporalHistory;
    const uint32_t createFlags =
        config_.createFlags | (leases_ ? LMXXF_NR_CREATE_FLAG_RECORDING_LEASES : 0u);

    std::wstring createError;
    if (!runtime_.CreateSession(device_.Get(), queue_.Get(), config_.modulesDirectory, createFlags,
                                &createError)) {
        std::string detail = Narrow(createError);
        const std::string last = runtime_.LastError();
        if (!last.empty()) detail += " | " + last;
        return Refuse("CreateSession on " + NarrowPath(config_.modulesDirectory) + ": " + detail);
    }
    Log("session created on " + NarrowPath(config_.modulesDirectory) +
        (leases_ ? " with LMXXF_NR_CREATE_FLAG_RECORDING_LEASES" : " in the default job model"));

    std::wstring prepareError;
    if (!runtime_.PrepareSession(&prepareError)) {
        std::string detail = Narrow(prepareError);
        const std::string last = runtime_.LastError();
        if (!last.empty()) detail += " | " + last;
        return Refuse("PrepareSession: " + detail);
    }
    runtimeStatus_ = runtime_.Status();
    Log("PrepareSession OK" + (runtimeStatus_.empty() ? std::string() : " | status: " + runtimeStatus_));

    session_.emplace(runtime_.Api(), runtime_.Context(), leases_);
    prepared_ = true;
    return true;
}

void AmdLmxxfEvaluator::TearDown()
{
    session_.reset();
    runtime_.Unload();
    if (uploadMapped_ != nullptr && upload_ != nullptr) {
        upload_->Unmap(0, nullptr);
        uploadMapped_ = nullptr;
    }
    if (guideUploadMapped_ != nullptr && guideUpload_ != nullptr) {
        guideUpload_->Unmap(0, nullptr);
        guideUploadMapped_ = nullptr;
    }
    guideUpload_.Reset();
    depth_.Reset();
    motion_.Reset();
    readback_.Reset();
    upload_.Reset();
    color_.Reset();
    // The accumulation's objects go with the frame ones they are sized to, and
    // the history is dropped with them: a device that comes back up at another
    // geometry starts a new history rather than blending into a frame of the
    // size it no longer is.
    accumulationHistory_ = {};
    zeroMotion_.Reset();
    stabilised_.Reset();
    neuralLinear_.Reset();
    historySlot_[0].Reset();
    historySlot_[1].Reset();
    sourceSlot_[0].Reset();
    sourceSlot_[1].Reset();
    psoEncode_.Reset();
    psoAccumulate_.Reset();
    psoDecode_.Reset();
    accumulationRootSig_.Reset();
    convertRootSig_.Reset();
    accumulationRtvHeap_.Reset();
    accumulationSrvHeap_.Reset();
    accumulationSrvDescriptorSize_ = 0;
    upscaleWidth_ = 0;
    upscaleHeight_ = 0;
    upscaleTarget_.Reset();
    sharpenedTarget_.Reset();
    psoUpscale_.Reset();
    psoCas_.Reset();
    upscaleRootSig_.Reset();
    upscaleSrvHeap_.Reset();
    upscaleSrvDescriptorSize_ = 0;
    outputReadback_.Reset();
    fence_.Reset();
    consumerList_.Reset();
    consumerAllocator_.Reset();
    list_.Reset();
    allocator_.Reset();
    if (fenceEvent_ != nullptr) {
        ::CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
    if (config_.device == nullptr) {
        queue_.Reset();
        device_.Reset();
    }
    adapter_.Reset();
    prepared_ = false;
    colorUploaded_ = false;
    colorState_ = D3D12_RESOURCE_STATE_COPY_DEST;
}

bool AmdLmxxfEvaluator::Initialize(HWND, uint32_t width, uint32_t height, uint32_t outputWidth,
                                   uint32_t outputHeight, double, const GuideControls&)
{
    lastFailure_ = NeuralRenderFailure::None;
    lastError_.clear();

    if (width == 0 || height == 0) return Refuse("Initialize was handed a zero-sized frame");
    // The runtime is a 1:1 denoiser: job.private_output is the colour geometry.
    // A capture at another size is therefore not the runtime's to produce - it
    // is the upscale capture below, this evaluator's own stretch-and-sharpen
    // over the runtime's output. A capture SMALLER than the frame is still
    // refused here rather than executed as a downsize: no caller asks for one,
    // and a pass that quietly shrank the picture would be a render nobody
    // ordered.
    if (outputWidth < width || outputHeight < height)
        return Refuse("a capture of " + std::to_string(outputWidth) + "x" +
                      std::to_string(outputHeight) + " from a " + std::to_string(width) + "x" +
                      std::to_string(height) +
                      " source is smaller than the source, which is not an upscale");
    const uint32_t wantedUpscaleW = (outputWidth != width || outputHeight != height) ? outputWidth : 0;
    const uint32_t wantedUpscaleH = wantedUpscaleW ? outputHeight : 0;

    const bool sameGeometry = prepared_ && width_ == width && height_ == height &&
                              upscaleWidth_ == wantedUpscaleW && upscaleHeight_ == wantedUpscaleH;
    if (sameGeometry) {
        Log("Initialize re-entered at the same geometry; the session is reused");
        return true;
    }

    TearDown();
    width_ = width;
    height_ = height;
    upscaleWidth_ = wantedUpscaleW;
    upscaleHeight_ = wantedUpscaleH;
    frameIndex_ = 0;
    evaluations_ = 0;
    neuralEvaluations_ = 0;
    accumulationFrames_ = 0;

    if (!BringUpDevice("the evaluator owns them; a stage C worker would hand its own")) return false;
    if (!CreateFrameResources()) return false;
    // Before the runtime, so a shader that will not compile is refused before a
    // session is armed for a job that could never run at its rung.
    if (!CreateTemporalResources()) return false;
    if (!CreateUpscaleResources()) return false;
    if (!BringUpRuntime()) return false;
    // After the runtime, so a guide failure reports with the session's context
    // already in the log. Only the lease path has a consumer list to build.
    if (leases_) {
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&consumerAllocator_))))
            return Refuse("CreateCommandAllocator for the consumer list failed");
        if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              consumerAllocator_.Get(), nullptr,
                                              IID_PPV_ARGS(&consumerList_))))
            return Refuse("CreateCommandList for the consumer list failed");
        if (FAILED(consumerList_->Close()))
            return Refuse("the consumer command list would not close after creation");
        if (!CreateGuideTextures()) return false;
    }
    return true;
}

bool AmdLmxxfEvaluator::WaitForGpu()
{
    const UINT64 value = ++fenceValue_;
    if (FAILED(queue_->Signal(fence_.Get(), value)))
        return Fail(NeuralRenderFailure::Neural, "the frame fence would not signal");
    if (fence_->GetCompletedValue() >= value) return true;
    if (FAILED(fence_->SetEventOnCompletion(value, fenceEvent_)))
        return Fail(NeuralRenderFailure::Neural, "SetEventOnCompletion failed");
    const DWORD waited = ::WaitForSingleObject(fenceEvent_, kFenceWaitMs);
    if (waited == WAIT_TIMEOUT)
        return Fail(NeuralRenderFailure::GpuStall, "the frame fence did not signal within 30s");
    if (waited != WAIT_OBJECT_0)
        return Fail(NeuralRenderFailure::Neural, "WaitForSingleObject failed on the frame fence");
    if (device_->GetDeviceRemovedReason() != S_OK)
        return Fail(NeuralRenderFailure::DeviceRemoved, "the D3D12 device was removed");
    return true;
}

bool AmdLmxxfEvaluator::UploadColor(const std::vector<uint8_t>& bgra, int sourceSlot)
{
    // The pointer never moves; only the contents do. The upload buffer is
    // persistent and mapped, so a frame costs one memcpy per row and no
    // allocation, which is what the "do not create a texture per frame" rule in
    // the integration plan is about.
    for (uint32_t row = 0; row < height_; ++row) {
        std::memcpy(uploadMapped_ + footprint_.Offset +
                        static_cast<size_t>(row) * footprint_.Footprint.RowPitch,
                    bgra.data() + static_cast<size_t>(row) * width_ * 4u,
                    static_cast<size_t>(width_) * 4u);
    }

    if (colorState_ != D3D12_RESOURCE_STATE_COPY_DEST)
        Transition(list_.Get(), color_.Get(), colorState_, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = color_.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload_.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint_;
    list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    // The accumulation's trust test compares this frame's source with the frame
    // before it, so this frame has to be in the slot its own output will go into
    // before the pass reads either. It is taken from the colour texture rather
    // than uploaded a second time because they are the same bytes and this
    // texture already holds them.
    D3D12_RESOURCE_STATES stateAfterUpload = D3D12_RESOURCE_STATE_COPY_DEST;
    if (sourceSlot == 0 || sourceSlot == 1) {
        ID3D12Resource* slot = sourceSlot_[static_cast<size_t>(sourceSlot)].Get();
        Transition(list_.Get(), color_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(list_.Get(), slot, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        list_->CopyResource(slot, color_.Get());
        Transition(list_.Get(), slot, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        stateAfterUpload = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    Transition(list_.Get(), color_.Get(), stateAfterUpload, kColorState);

    colorState_ = kColorState;
    colorUploaded_ = true;
    return true;
}

bool AmdLmxxfEvaluator::CopyProducedToReadback(ID3D12GraphicsCommandList* list)
{
    auto* produced = static_cast<ID3D12Resource*>(session_->Job().private_output);
    if (produced == nullptr)
        return Fail(NeuralRenderFailure::Neural,
                    "the runtime published no job.private_output, so there is no produced frame");

    const D3D12_RESOURCE_DESC description = produced->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        description.Width != width_ || description.Height != height_)
        return Fail(NeuralRenderFailure::Neural,
                    "job.private_output is " + std::to_string(description.Width) + "x" +
                        std::to_string(description.Height) + ", not the " + std::to_string(width_) +
                        "x" + std::to_string(height_) + " frame that was submitted");
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        description.Format != DXGI_FORMAT_B8G8R8X8_UNORM)
        return Fail(NeuralRenderFailure::Neural,
                    "job.private_output format " + std::to_string(static_cast<int>(description.Format)) +
                        " is not a 32-bit BGRA layout this evaluator can read");

    Transition(list, produced, kPrivateOutputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    CopyTextureToReadback(list, produced, description.Format);
    // Put it back exactly where the runtime left it, so the next frame's own
    // barriers still describe the truth.
    Transition(list, produced, D3D12_RESOURCE_STATE_COPY_SOURCE, kPrivateOutputState);

    lastOutput_ = produced;
    lastOutputWasPrivate_ = produced != static_cast<void*>(color_.Get());
    return true;
}

void AmdLmxxfEvaluator::CopyTextureToReadback(ID3D12GraphicsCommandList* list, ID3D12Resource* from,
                                             DXGI_FORMAT format)
{
    // The frame's own footprint, carrying the source's format: the two paths
    // that reach this differ only in the texture they hand it, and the format
    // has to come from that texture because a copy region does not convert.
    // An upscaling capture reads its own buffer at the capture's footprint -
    // the sharpened target is the capture's size, and a copy region demands
    // that the two sides agree.
    const bool upscaling = Upscaling();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = upscaling ? outputFootprint_ : footprint_;
    footprint.Footprint.Format = format;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = upscaling ? outputReadback_.Get() : readback_.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = from;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
}

bool AmdLmxxfEvaluator::CreateUpscaleResources()
{
    if (!Upscaling()) return true;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    // One description, two targets: both are capture-sized BGRA, both written
    // by a draw and read by the next pass or the copy.
    D3D12_RESOURCE_DESC target{};
    target.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target.Width = upscaleWidth_;
    target.Height = upscaleHeight_;
    target.DepthOrArraySize = 1;
    target.MipLevels = 1;
    target.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    target.SampleDesc.Count = 1;
    target.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &target,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                nullptr, IID_PPV_ARGS(upscaleTarget_.GetAddressOf()))))
        return Refuse("CreateCommittedResource for the stretched target failed");
    if (FAILED(device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &target,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                nullptr, IID_PPV_ARGS(sharpenedTarget_.GetAddressOf()))))
        return Refuse("CreateCommittedResource for the sharpened target failed");

    D3D12_DESCRIPTOR_HEAP_DESC srvHeap{};
    srvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeap.NumDescriptors = 2;  // the frame's texture, then the stretched one
    srvHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&srvHeap, IID_PPV_ARGS(upscaleSrvHeap_.GetAddressOf()))))
        return Refuse("CreateDescriptorHeap for the upscale capture failed");
    upscaleSrvDescriptorSize_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeap{};
    rtvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeap.NumDescriptors = 2;  // one per target
    if (FAILED(device_->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(upscaleRtvHeap_.GetAddressOf()))))
        return Refuse("CreateDescriptorHeap(RTV) for the upscale capture failed");
    const UINT rtvSize = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device_->CreateRenderTargetView(upscaleTarget_.Get(), nullptr,
                                    upscaleRtvHeap_->GetCPUDescriptorHandleForHeapStart());
    device_->CreateRenderTargetView(
        sharpenedTarget_.Get(), nullptr,
        OffsetHandle(upscaleRtvHeap_->GetCPUDescriptorHandleForHeapStart(), rtvSize, 1));

    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    view.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(
        upscaleTarget_.Get(), &view,
        OffsetHandle(upscaleSrvHeap_->GetCPUDescriptorHandleForHeapStart(),
                     upscaleSrvDescriptorSize_, 1));

    // The static sampler is POINT because both passes sample at exact texel
    // centres: the Catmull-Rom weights are the whole filter, and the CAS taps
    // sit on the target's own pixels. A linear sampler would apply the stretch
    // a second time.
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_PARAMETER parameters[2] = {parameter, parameter};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 8;
    parameters[1].Constants.ShaderRegister = 0;
    {
        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = 2;
        description.pParameters = parameters;
        description.NumStaticSamplers = 1;
        description.pStaticSamplers = &sampler;
        description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        Microsoft::WRL::ComPtr<ID3DBlob> signature, error;
        const HRESULT hr = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                                       signature.GetAddressOf(),
                                                       error.GetAddressOf());
        if (FAILED(hr))
            return Refuse("the upscale capture's root signature would not serialize: " +
                          (error ? BlobText(error.Get()) : Hex32(hr)));
        if (FAILED(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                                signature->GetBufferSize(),
                                                IID_PPV_ARGS(upscaleRootSig_.GetAddressOf()))))
            return Refuse("CreateRootSignature for the upscale capture failed");
    }

    auto compile = [&](const char* entry, Microsoft::WRL::ComPtr<ID3DBlob>& blob) -> bool {
        const char* profile = UpscaleShaderProfileFor(entry);
        Microsoft::WRL::ComPtr<ID3DBlob> shaderError;
        const HRESULT hr = D3DCompile(kAmdUpscaleHlsl, sizeof(kAmdUpscaleHlsl) - 1, "amd-upscale",
                                      nullptr, nullptr, entry, profile,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.GetAddressOf(),
                                      shaderError.GetAddressOf());
        if (FAILED(hr))
            return Refuse(std::string("the upscale capture's ") + entry + " (" + profile +
                          ") would not compile: " +
                          (shaderError ? BlobText(shaderError.Get()) : Hex32(hr)));
        return true;
    };
    Microsoft::WRL::ComPtr<ID3DBlob> vs, upscalePs, casPs;
    if (!compile("VS", vs)) return false;
    if (!compile("PSAmdUpscale", upscalePs)) return false;
    if (!compile("PSAmdCas", casPs)) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pso.SampleDesc = {1, 0};
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.pRootSignature = upscaleRootSig_.Get();
    pso.PS = {upscalePs->GetBufferPointer(), upscalePs->GetBufferSize()};
    if (FAILED(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(psoUpscale_.GetAddressOf()))))
        return Refuse("CreateGraphicsPipelineState for the upscale pass failed");
    pso.PS = {casPs->GetBufferPointer(), casPs->GetBufferSize()};
    if (FAILED(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(psoCas_.GetAddressOf()))))
        return Refuse("CreateGraphicsPipelineState for the CAS pass failed");

    // The capture's own readback: the frame one is sized to the source, and a
    // capture at another size is a different footprint with a different pitch.
    device_->GetCopyableFootprints(&target, 0, 1, 0, &outputFootprint_, nullptr, nullptr, nullptr);
    D3D12_RESOURCE_DESC bufferDescription{};
    bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDescription.Width = outputFootprint_.Footprint.RowPitch *
                              static_cast<UINT64>(upscaleHeight_);
    bufferDescription.Height = 1;
    bufferDescription.DepthOrArraySize = 1;
    bufferDescription.MipLevels = 1;
    bufferDescription.SampleDesc = {1, 0};
    bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(device_->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
                                                &bufferDescription,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(outputReadback_.GetAddressOf()))))
        return Refuse("CreateCommittedResource for the capture's readback failed");
    return true;
}

bool AmdLmxxfEvaluator::RecordUpscale(ID3D12GraphicsCommandList* list, ID3D12Resource* from)
{
    if (from == nullptr)
        return Fail(NeuralRenderFailure::Neural,
                    "the upscale capture was recorded with no frame to stretch");
    const D3D12_RESOURCE_DESC description = from->GetDesc();
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        description.Format != DXGI_FORMAT_B8G8R8X8_UNORM)
        return Fail(NeuralRenderFailure::Neural,
                    "the upscale capture's input format " +
                        std::to_string(static_cast<int>(description.Format)) +
                        " is not a 32-bit BGRA layout this evaluator can read");

    // The one descriptor that cannot be built once, for the same reason the
    // accumulation's is not: this backend's producer publishes a new texture
    // for every frame.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Format = description.Format;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(
            from, &view,
            OffsetHandle(upscaleSrvHeap_->GetCPUDescriptorHandleForHeapStart(),
                         upscaleSrvDescriptorSize_, 0));
    }

    ID3D12DescriptorHeap* heaps[] = {upscaleSrvHeap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(upscaleRootSig_.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const D3D12_GPU_DESCRIPTOR_HANDLE srvBase =
        upscaleSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    const float constants[8] = {
        static_cast<float>(description.Width), static_cast<float>(description.Height),
        static_cast<float>(upscaleWidth_), static_cast<float>(upscaleHeight_),
        std::clamp(config_.upscaleSharpness, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f};
    auto draw = [&](ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE target,
                    UINT srvIndex) {
        D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(upscaleWidth_),
                                static_cast<float>(upscaleHeight_), 0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(upscaleWidth_),
                           static_cast<LONG>(upscaleHeight_)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &target, FALSE, nullptr);
        list->SetPipelineState(pso);
        list->SetGraphicsRootDescriptorTable(
            0, OffsetHandle(srvBase, upscaleSrvDescriptorSize_, srvIndex));
        list->SetGraphicsRoot32BitConstants(1, 8, constants, 0);
        list->DrawInstanced(3, 1, 0, 0);
    };

    // ---- stretch: the frame into the capture's geometry -------------------
    Transition(list, upscaleTarget_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    draw(psoUpscale_.Get(), upscaleRtvHeap_->GetCPUDescriptorHandleForHeapStart(), 0);
    Transition(list, upscaleTarget_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // ---- sharpen: CAS over the stretched frame's own neighbourhood ---------
    const UINT rtvSize = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    Transition(list, sharpenedTarget_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    draw(psoCas_.Get(),
         OffsetHandle(upscaleRtvHeap_->GetCPUDescriptorHandleForHeapStart(), rtvSize, 1), 1);
    Transition(list, sharpenedTarget_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    CopyTextureToReadback(list, sharpenedTarget_.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
    Transition(list, sharpenedTarget_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // What a reader of this frame gets is the sharpened capture, not the
    // runtime's own 1:1 output - the same claim the accumulation's encode makes.
    lastOutput_ = sharpenedTarget_.Get();
    lastOutputWasPrivate_ = false;
    return true;
}

bool AmdLmxxfEvaluator::CreateTemporalResources()
{
    if (config_.stability == TemporalStability::Off) return true;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Every texture here is the frame's own size, because the runtime is a 1:1
    // denoiser (Initialize refuses anything else), so the only fields that vary
    // between them are the format and the flags.
    auto texture = [&](DXGI_FORMAT format, uint32_t w, uint32_t h, D3D12_RESOURCE_FLAGS flags,
                       D3D12_RESOURCE_STATES initial, Microsoft::WRL::ComPtr<ID3D12Resource>& out,
                       const wchar_t* name) -> bool {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = w;
        description.Height = h;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = flags;
        if (FAILED(device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &description,
                                                    initial, nullptr, IID_PPV_ARGS(out.GetAddressOf()))))
            return Refuse("CreateCommittedResource for " + Narrow(std::wstring(name)) + " failed");
        out->SetName(name);
        return true;
    };

    for (UINT slot = 0; slot < 2; ++slot) {
        if (!texture(DXGI_FORMAT_B8G8R8A8_UNORM, width_, height_, D3D12_RESOURCE_FLAG_NONE,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, sourceSlot_[slot],
                     slot ? L"AMD_Temporal_Source_B" : L"AMD_Temporal_Source_A"))
            return false;
        if (!texture(DXGI_FORMAT_R16G16B16A16_FLOAT, width_, height_,
                     D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, historySlot_[slot],
                     slot ? L"AMD_Temporal_History_B" : L"AMD_Temporal_History_A"))
            return false;
    }
    if (!texture(DXGI_FORMAT_R16G16B16A16_FLOAT, width_, height_,
                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, neuralLinear_,
                 L"AMD_Temporal_NeuralLinear"))
        return false;
    if (!texture(DXGI_FORMAT_B8G8R8A8_UNORM, width_, height_,
                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, stabilised_,
                 L"AMD_Temporal_Stabilised"))
        return false;
    if (!texture(DXGI_FORMAT_R16G16_FLOAT, 1, 1, D3D12_RESOURCE_FLAG_NONE,
                 D3D12_RESOURCE_STATE_COPY_DEST, zeroMotion_, L"AMD_Temporal_ZeroMotion"))
        return false;

    // The zero motion field, through a throwaway staging buffer. The pass reads
    // a vector from this texture and adds it to the sample position, so a zero
    // vector is what makes the frame it accumulates from the same position as
    // the frame it accumulates into.
    {
        const uint16_t zero[2] = {0, 0};
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = sizeof(zero);
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.Format = DXGI_FORMAT_UNKNOWN;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        if (FAILED(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(staging.GetAddressOf()))))
            return Refuse("CreateCommittedResource for the zero-motion staging buffer failed");
        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        if (FAILED(staging->Map(0, &noRead, &mapped)))
            return Refuse("the zero-motion staging buffer would not map");
        std::memcpy(mapped, zero, sizeof(zero));
        staging->Unmap(0, nullptr);

        // It has to go through a list, and this is the only one that exists
        // before a frame does. Every frame path resets this pair at its own
        // start, so leaving it closed here is the state they expect.
        if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr)))
            return Refuse("the command list would not reset for the zero-motion upload");
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = zeroMotion_.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = staging.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Offset = 0;
        source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16_FLOAT;
        source.PlacedFootprint.Footprint.Width = 1;
        source.PlacedFootprint.Footprint.Height = 1;
        source.PlacedFootprint.Footprint.Depth = 1;
        source.PlacedFootprint.Footprint.RowPitch = sizeof(zero);
        list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        Transition(list_.Get(), zeroMotion_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (FAILED(list_->Close()))
            return Refuse("the zero-motion list would not close");
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        if (!WaitForGpu()) return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvHeap{};
    srvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeap.NumDescriptors = kSrvCount;
    srvHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&srvHeap, IID_PPV_ARGS(accumulationSrvHeap_.GetAddressOf()))))
        return Refuse("CreateDescriptorHeap for the accumulation's SRVs failed");
    accumulationSrvDescriptorSize_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeap{};
    rtvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeap.NumDescriptors = kRtvCount;
    if (FAILED(device_->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(accumulationRtvHeap_.GetAddressOf()))))
        return Refuse("CreateDescriptorHeap for the accumulation's render targets failed");

    const D3D12_CPU_DESCRIPTOR_HANDLE rtvBase =
        accumulationRtvHeap_->GetCPUDescriptorHandleForHeapStart();
    const UINT rtvSize = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device_->CreateRenderTargetView(stabilised_.Get(), nullptr,
                                    OffsetHandle(rtvBase, rtvSize, kRtvStabilised));
    device_->CreateRenderTargetView(neuralLinear_.Get(), nullptr,
                                    OffsetHandle(rtvBase, rtvSize, kRtvNeuralLinear));
    for (UINT slot = 0; slot < 2; ++slot)
        device_->CreateRenderTargetView(historySlot_[slot].Get(), nullptr,
                                        OffsetHandle(rtvBase, rtvSize, kRtvHistoryBase + slot));

    const D3D12_CPU_DESCRIPTOR_HANDLE srvBase =
        accumulationSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    auto makeView = [&](ID3D12Resource* resource, DXGI_FORMAT format, UINT index) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Format = format;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(resource, &view,
                                          OffsetHandle(srvBase, accumulationSrvDescriptorSize_, index));
    };
    for (UINT slot = 0; slot < 2; ++slot)
        makeView(historySlot_[slot].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kSrvEncodeBase + slot);
    // One table per history slot the pass can read. Which table is bound is
    // which slot it reads, so the whole choice costs a descriptor-table bind
    // rather than a descriptor edit.
    for (UINT read = 0; read < 2; ++read) {
        const UINT base = kSrvAccumulateBase + read * kAccumulateTableSize;
        makeView(neuralLinear_.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, base + 0);
        makeView(historySlot_[read].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, base + 1);
        makeView(zeroMotion_.Get(), DXGI_FORMAT_R16G16_FLOAT, base + 2);
        // t3 is this frame's source and t4 the frame before it. The slot this
        // frame's output goes into is the slot its source goes into, so when the
        // pass reads slot `read` it is the other one that is current - and on a
        // reset, where the two are the same slot, neither is read at all.
        makeView(sourceSlot_[read ^ 1u].Get(), DXGI_FORMAT_B8G8R8A8_UNORM, base + 3);
        makeView(sourceSlot_[read].Get(), DXGI_FORMAT_B8G8R8A8_UNORM, base + 4);
    }
    // Decode's table is rewritten every frame with the runtime's own texture.
    // Until then it points at a real texture, because a bound table of null
    // descriptors is what a debug layer calls a bug and a frame is what fixes.
    makeView(sourceSlot_[0].Get(), DXGI_FORMAT_B8G8R8A8_UNORM, kSrvDecode);

    if (!CreateTemporalPipeline()) return false;

    Log("temporal accumulation " + std::string(TemporalStabilityName(config_.stability)) + " at " +
        std::to_string(width_) + "x" + std::to_string(height_) +
        ": two FP16 history slots, two source frames, one zero motion field");
    return true;
}

bool AmdLmxxfEvaluator::CreateTemporalPipeline()
{
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    auto compile = [&](const char* text, size_t length, const char* entry,
                       Microsoft::WRL::ComPtr<ID3DBlob>& blob) -> bool {
        // The profile is the entry point's (AmdTemporalAccumulationShader.h):
        // this loop compiles two vertex shaders and three pixel shaders, and a
        // fixed target here is a mistake only a device can see.
        const char* profile = ShaderProfileFor(entry);
        Microsoft::WRL::ComPtr<ID3DBlob> error;
        const HRESULT hr = D3DCompile(text, length, "amd-temporal", nullptr, nullptr, entry, profile,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.GetAddressOf(),
                                      error.GetAddressOf());
        if (FAILED(hr))
            return Refuse(std::string("the temporal accumulation's ") + entry + " (" + profile +
                          ") would not compile: " + (error ? BlobText(error.Get()) : Hex32(hr)));
        return true;
    };

    Microsoft::WRL::ComPtr<ID3DBlob> convertVs, decodePs, encodePs, accumulateVs, accumulatePs;
    if (!compile(kAmdTemporalConvertHlsl, sizeof(kAmdTemporalConvertHlsl) - 1, "VS", convertVs))
        return false;
    if (!compile(kAmdTemporalConvertHlsl, sizeof(kAmdTemporalConvertHlsl) - 1, "PSDecode", decodePs))
        return false;
    if (!compile(kAmdTemporalConvertHlsl, sizeof(kAmdTemporalConvertHlsl) - 1, "PSEncode", encodePs))
        return false;
    if (!compile(kTemporalStabilityHlsl, sizeof(kTemporalStabilityHlsl) - 1, "VS", accumulateVs))
        return false;
    if (!compile(kTemporalStabilityHlsl, sizeof(kTemporalStabilityHlsl) - 1, "PSTemporalStability",
                 accumulatePs))
        return false;

    // The conversions take one texture and no constants, so their signature is a
    // table of one and nothing else.
    D3D12_DESCRIPTOR_RANGE convertRange{};
    convertRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    convertRange.NumDescriptors = 1;
    convertRange.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER convertParameter{};
    convertParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    convertParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    convertParameter.DescriptorTable.NumDescriptorRanges = 1;
    convertParameter.DescriptorTable.pDescriptorRanges = &convertRange;
    {
        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = 1;
        description.pParameters = &convertParameter;
        description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        Microsoft::WRL::ComPtr<ID3DBlob> signature, error;
        const HRESULT hr =
            D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                        signature.GetAddressOf(), error.GetAddressOf());
        if (FAILED(hr))
            return Refuse("the conversion root signature would not serialize: " +
                          (error ? BlobText(error.Get()) : Hex32(hr)));
        if (FAILED(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                                signature->GetBufferSize(),
                                                IID_PPV_ARGS(convertRootSig_.GetAddressOf()))))
            return Refuse("CreateRootSignature for the conversions failed");
    }

    // The accumulation takes the pass's own five textures and its eight
    // constants - the same shape the NVIDIA capture path builds for it.
    D3D12_DESCRIPTOR_RANGE accumulateRange{};
    accumulateRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    accumulateRange.NumDescriptors = kAccumulateTableSize;
    accumulateRange.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER accumulateParameters[2]{};
    accumulateParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    accumulateParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    accumulateParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    accumulateParameters[0].DescriptorTable.pDescriptorRanges = &accumulateRange;
    accumulateParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    accumulateParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    accumulateParameters[1].Constants.Num32BitValues = 8;
    accumulateParameters[1].Constants.ShaderRegister = 0;
    {
        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = 2;
        description.pParameters = accumulateParameters;
        description.NumStaticSamplers = 1;
        description.pStaticSamplers = &sampler;
        description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        Microsoft::WRL::ComPtr<ID3DBlob> signature, error;
        const HRESULT hr =
            D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                        signature.GetAddressOf(), error.GetAddressOf());
        if (FAILED(hr))
            return Refuse("the accumulation root signature would not serialize: " +
                          (error ? BlobText(error.Get()) : Hex32(hr)));
        if (FAILED(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                                signature->GetBufferSize(),
                                                IID_PPV_ARGS(accumulationRootSig_.GetAddressOf()))))
            return Refuse("CreateRootSignature for the accumulation failed");
    }

    // One description, three states: the conversions differ in their pixel
    // shader and target format, the accumulation in its own pair.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.VS = {convertVs->GetBufferPointer(), convertVs->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.SampleDesc = {1, 0};
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;

    pso.pRootSignature = convertRootSig_.Get();
    pso.PS = {decodePs->GetBufferPointer(), decodePs->GetBufferSize()};
    pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (FAILED(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(psoDecode_.GetAddressOf()))))
        return Refuse("CreateGraphicsPipelineState for the decode pass failed");
    pso.PS = {encodePs->GetBufferPointer(), encodePs->GetBufferSize()};
    pso.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (FAILED(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(psoEncode_.GetAddressOf()))))
        return Refuse("CreateGraphicsPipelineState for the encode pass failed");

    pso.pRootSignature = accumulationRootSig_.Get();
    pso.VS = {accumulateVs->GetBufferPointer(), accumulateVs->GetBufferSize()};
    pso.PS = {accumulatePs->GetBufferPointer(), accumulatePs->GetBufferSize()};
    pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (FAILED(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(psoAccumulate_.GetAddressOf()))))
        return Refuse("CreateGraphicsPipelineState for the accumulation pass failed");
    return true;
}

bool AmdLmxxfEvaluator::RecordTemporalAccumulation(ID3D12GraphicsCommandList* list,
                                                   const temporal_stability::Plan& plan,
                                                   const FrameIdentity& frame)
{
    const std::optional<temporal_stability::Blend> blend =
        temporal_stability::BlendFor(config_.stability);
    if (!blend)
        return Fail(NeuralRenderFailure::Neural,
                    "the temporal accumulation was recorded at a rung with no blend");

    auto* produced = static_cast<ID3D12Resource*>(session_->Job().private_output);
    if (produced == nullptr)
        return Fail(NeuralRenderFailure::Neural,
                    "the runtime published no job.private_output, so there is no frame to stabilise");
    const D3D12_RESOURCE_DESC description = produced->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        description.Width != width_ || description.Height != height_)
        return Fail(NeuralRenderFailure::Neural,
                    "job.private_output is " + std::to_string(description.Width) + "x" +
                        std::to_string(description.Height) + ", not the " + std::to_string(width_) +
                        "x" + std::to_string(height_) + " frame that was submitted");
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        description.Format != DXGI_FORMAT_B8G8R8X8_UNORM)
        return Fail(NeuralRenderFailure::Neural,
                    "job.private_output format " +
                        std::to_string(static_cast<int>(description.Format)) +
                        " is not a 32-bit BGRA layout this evaluator can read");

    // The one descriptor that cannot be built once: the runtime publishes a new
    // texture for every frame, as EnsureFrameResources' counterpart in the
    // renderer does not have to, because this backend's producer is not ours.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Format = description.Format;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(
            produced, &view,
            OffsetHandle(accumulationSrvHeap_->GetCPUDescriptorHandleForHeapStart(),
                         accumulationSrvDescriptorSize_, kSrvDecode));
    }

    ID3D12DescriptorHeap* heaps[] = {accumulationSrvHeap_.Get()};
    list->SetDescriptorHeaps(1, heaps);

    const UINT read = plan.read;
    const UINT write = plan.write;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvBase =
        accumulationRtvHeap_->GetCPUDescriptorHandleForHeapStart();
    const UINT rtvSize = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_GPU_DESCRIPTOR_HANDLE srvBase =
        accumulationSrvHeap_->GetGPUDescriptorHandleForHeapStart();

    // Weight is zero on a reset, where the pass returns the neural pixel before
    // its first history fetch. Everything else in the block is the rung's own.
    float constants[8] = {
        0.0f, 0.0f,  // the motion field is one texel of zero, in texel units
        plan.blend ? blend->historyWeight : 0.0f,
        blend->trustLow,
        blend->trustHigh,
        width_ ? 1.0f / static_cast<float>(width_) : 0.0f,
        height_ ? 1.0f / static_cast<float>(height_) : 0.0f,
        0.0f};

    auto draw = [&](ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE target, UINT srvIndex,
                    const float* constants) {
        D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_),
                                static_cast<float>(height_), 0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &target, FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->SetPipelineState(pso);
        list->SetGraphicsRootDescriptorTable(
            0, OffsetHandle(srvBase, accumulationSrvDescriptorSize_, srvIndex));
        if (constants != nullptr) list->SetGraphicsRoot32BitConstants(1, 8, constants, 0);
        list->DrawInstanced(3, 1, 0, 0);
    };

    // ---- decode: the runtime's 8-bit sRGB frame into linear FP16 ----------
    Transition(list, produced, kPrivateOutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->SetGraphicsRootSignature(convertRootSig_.Get());
    Transition(list, neuralLinear_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    draw(psoDecode_.Get(), OffsetHandle(rtvBase, rtvSize, kRtvNeuralLinear), kSrvDecode, nullptr);
    Transition(list, neuralLinear_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // ---- accumulate: the pass itself, into this frame's history slot ------
    list->SetGraphicsRootSignature(accumulationRootSig_.Get());
    Transition(list, historySlot_[write].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    draw(psoAccumulate_.Get(), OffsetHandle(rtvBase, rtvSize, kRtvHistoryBase + write),
         kSrvAccumulateBase + read * kAccumulateTableSize, constants);
    Transition(list, historySlot_[write].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // ---- encode: back to the codes the readback and its readers carry -----
    list->SetGraphicsRootSignature(convertRootSig_.Get());
    Transition(list, stabilised_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    draw(psoEncode_.Get(), OffsetHandle(rtvBase, rtvSize, kRtvStabilised),
         kSrvEncodeBase + write, nullptr);
    Transition(list, stabilised_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // An upscaling capture stretches the stabilised frame instead of copying
    // it; the plain capture copies. Both leave the frame where it was.
    if (Upscaling()) {
        if (!RecordUpscale(list, stabilised_.Get())) return false;
    } else {
        Transition(list, stabilised_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        CopyTextureToReadback(list, stabilised_.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
        Transition(list, stabilised_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // The runtime's own texture goes back exactly where it left it, so the next
    // frame's barriers still describe the truth.
    Transition(list, produced, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kPrivateOutputState);

    accumulationHistory_ = temporal_stability::Commit(accumulationHistory_, plan, frame);
    // What a reader of this frame gets is the stabilized texture - or, on an
    // upscaling capture, the sharpened capture the record left there - and
    // saying so is the point of the two accessors.
    if (!Upscaling()) lastOutput_ = stabilised_.Get();
    lastOutputWasPrivate_ = false;
    ++accumulationFrames_;
    return true;
}

bool AmdLmxxfEvaluator::ReadBackProduced(std::vector<uint8_t>& out)
{
    // An upscaling capture reads the capture's own buffer at the capture's
    // footprint; the plain path reads the frame one at the frame's.
    const bool upscaling = Upscaling();
    ID3D12Resource* buffer = upscaling ? outputReadback_.Get() : readback_.Get();
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint =
        upscaling ? outputFootprint_ : footprint_;
    const uint32_t rows = upscaling ? upscaleHeight_ : height_;
    const uint32_t columns = upscaling ? upscaleWidth_ : width_;

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(footprint.Footprint.Height) *
                                        footprint.Footprint.RowPitch};
    if (FAILED(buffer->Map(0, &readRange, &mapped)))
        return Fail(NeuralRenderFailure::Neural, "the readback buffer would not map");
    out.assign(static_cast<size_t>(columns) * rows * 4u, 0);
    for (uint32_t row = 0; row < rows; ++row) {
        std::memcpy(out.data() + static_cast<size_t>(row) * columns * 4u,
                    static_cast<const uint8_t*>(mapped) + footprint.Offset +
                        static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                    static_cast<size_t>(columns) * 4u);
    }
    const D3D12_RANGE noWrite{0, 0};
    buffer->Unmap(0, &noWrite);
    return true;
}

bool AmdLmxxfEvaluator::SignalFence(uint64_t& value, int32_t& status)
{
    value = ++fenceValue_;
    const HRESULT result = queue_->Signal(fence_.Get(), value);
    // EndRecording wants the real HRESULT rather than a bool: it stores the pair
    // as this job's completion certificate and refuses to guess whether the work
    // landed. A zero value and UINT64_MAX are refused too, and value starts at 1.
    status = static_cast<int32_t>(result);
    if (value == 0 || value == UINT64_MAX)
        return Fail(NeuralRenderFailure::Neural, "the frame fence value is not one the ABI accepts");
    return true;
}

void AmdLmxxfEvaluator::RefreshHistoryStatus()
{
    const std::string status = runtime_.Status();
    // GetStatus is one flat line of space-separated key=value tokens and history=
    // is one of them. It is the only place the temporal model's decision is ever
    // published: no return code says whether history engaged, and no error text
    // says why it did not.
    constexpr std::string_view key = "history=";
    const size_t at = status.find(key);
    if (at == std::string::npos) {
        // No token at all. A status line that came back empty and one that
        // simply has no such key are different problems, so they read
        // differently here - neither is "the frame rendered spatially".
        lastHistoryStatus_ = status.empty() ? std::string("unreadable") : std::string("absent");
        return;
    }
    const size_t begin = at + key.size();
    const size_t end = status.find(' ', begin);
    lastHistoryStatus_ =
        status.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

bool AmdLmxxfEvaluator::SubmitFrameLease(const std::vector<uint8_t>& bgra, bool resetRequested,
                                        uint64_t sourceFrameNumber, std::vector<uint8_t>& out)
{
    amd_lmxxf::Session& session = *session_;

    if (motion_ == nullptr || depth_ == nullptr)
        return Fail(NeuralRenderFailure::Neural,
                    "the session is in the lease job model but no guide textures were created");

    // Every failure path below runs through this, and every cleanup call it
    // makes overwrites Session::LastError() with its own outcome - so the reason
    // is copied out first, or the interesting string is the one that gets lost.
    // The status is passed in rather than read back off the session for the same
    // reason: by then it belongs to whichever cleanup step ran last.
    auto abandon = [&](std::string_view stage, int32_t failureStatus) {
        const std::string reason = session.LastError();
        (void)list_->Close();
        (void)consumerList_->Close();
        if (session.Phase() != amd_lmxxf::CallPhase::Idle) {
            (void)session.InvalidateRecording();
            // Collection answering UNAVAILABLE is the documented "retain and ask
            // again", so this asks again instead of treating the first one as
            // the end. The bound stops a runtime that never releases from
            // spinning this frame loop.
            for (int attempt = 0; attempt < kCollectAttempts; ++attempt) {
                if (session.CollectRecording() != static_cast<int32_t>(LMXXF_NR_UNAVAILABLE))
                    break;
            }
        }
        return Fail(failureStatus == static_cast<int32_t>(LMXXF_NR_DEVICE_LOST)
                        ? NeuralRenderFailure::DeviceRemoved
                        : NeuralRenderFailure::Neural,
                    std::string(stage) + ": " + reason);
    };

    if (resetRequested && !session.ResetHistory())
        return Fail(NeuralRenderFailure::Neural, "ResetHistory: " + session.LastError());

    // ---- record both lists before submitting either ----------------------
    //
    // This is the one place the lease order differs in structure rather than in
    // which function is called: RecordInputs and RecordOutputs both refuse once
    // the job has started, and Begin is what starts it. So the whole recording
    // phase is one block and the whole submission phase is another, with Begin
    // between them - where in the default order the output recording sits
    // *after* the first submission because the producer's marker, not a Begin
    // call, is what advances that machine.
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr)) ||
        FAILED(consumerAllocator_->Reset()) ||
        FAILED(consumerList_->Reset(consumerAllocator_.Get(), nullptr)))
        return Fail(NeuralRenderFailure::Neural, "a command list would not reset");

    // Where this frame sits in the accumulation's history, decided before the
    // upload because the upload is what puts this frame's source into the slot
    // the plan says the pass will read it from.
    const bool accumulates = config_.stability != TemporalStability::Off;
    FrameIdentity accumulationFrame{};
    temporal_stability::Plan accumulationPlan{};
    int sourceSlot = -1;
    if (accumulates) {
        // The evaluator is one job over one source for its whole life, so the
        // two fields that would tell two streams apart carry the same value on
        // every frame and the frame number is what decides: the successor
        // advances the history, a repeat of the same frame blends against the
        // same history again, and a gap is a reset. The caller's own reset
        // overrides all three.
        accumulationFrame.frameNumber = sourceFrameNumber;
        accumulationPlan =
            temporal_stability::PlanFrame(accumulationHistory_, accumulationFrame, resetRequested);
        sourceSlot = static_cast<int>(accumulationPlan.write);
    }

    if (!UploadColor(bgra, sourceSlot))
        return Fail(NeuralRenderFailure::Neural, "the frame could not be uploaded");

    // The guides go in after the colour upload and before PrepareFrame, so one
    // list carries both and the runtime - which runs behind this list on the
    // same queue - reads this frame's field rather than the last one's. A file
    // that is missing or malformed stops the frame here, with nothing submitted
    // and so nothing to release: the session is still Idle and the next frame
    // resets both lists.
    if (!UploadGuides(list_.Get(), sourceFrameNumber)) {
        (void)list_->Close();
        (void)consumerList_->Close();
        return Fail(NeuralRenderFailure::Source, lastError_);
    }

    const uint64_t frameId = ++frameIndex_;
    LmxxfNrFrameInfo frameInfo =
        amd_lmxxf::MakeFrameInfo(color_.Get(), width_, height_,
                                 static_cast<uint32_t>(kColorState), list_.Get(), 1, frameId,
                                 frameId);

    amd_lmxxf::TemporalGuides guides;
    guides.motion = motion_.Get();
    guides.depth = depth_.Get();
    guides.motionState = static_cast<uint32_t>(kGuideState);
    guides.depthState = static_cast<uint32_t>(kGuideState);
    guides.motionWidth = width_;
    guides.motionHeight = height_;
    // The stored vector is already a frame-pixel displacement, so the value that
    // maps it onto the frame is the frame's own width. The kernel divides by
    // motion_width, which leaves the scale at 1 and this quantity with nothing
    // left to calibrate - see CreateGuideTextures for why that was the point.
    //
    // That holds for the file source too, and it is why it holds: GuideFiles.h
    // defines its motion in DLSS input pixels, the same unit, so whatever size a
    // field file arrives at, ResampleArea turns it into frame pixels before this
    // pair ever sees it. A host that stored normalised vectors instead would pass
    // 1.0f here and nothing else in this function would change.
    guides.motionScaleX = static_cast<float>(width_);
    guides.motionScaleY = static_cast<float>(height_);
    guides.jitterX = 0.0f;
    guides.jitterY = 0.0f;
    guides.flags = amd_lmxxf::kTemporalVideoFlags;
    // The renderer declares resets on the identity, and the runtime declares its
    // own on any break in the frame_id sequence or a gap over 500 ms. This is the
    // host's half of one decision, not the whole of it.
    guides.reset = resetRequested;
    amd_lmxxf::ApplyTemporalGuides(frameInfo, guides);

    if (!session.PrepareFrame(frameInfo)) {
        Report("PrepareFrame", session.LastStatus());
        return abandon("PrepareFrame", session.LastStatus());
    }
    if (!session.RecordInputs(list_.Get())) {
        Report("RecordInputs", session.LastStatus());
        return abandon("RecordInputs", session.LastStatus());
    }
    if (FAILED(list_->Close()))
        return Fail(NeuralRenderFailure::Neural, "the producer list would not close");
    if (!session.RecordOutputs(consumerList_.Get())) {
        Report("RecordOutputs", session.LastStatus());
        return abandon("RecordOutputs", session.LastStatus());
    }
    if (accumulates) {
        if (!RecordTemporalAccumulation(consumerList_.Get(), accumulationPlan, accumulationFrame))
            return abandon("the produced frame could not be stabilized",
                           static_cast<int32_t>(LMXXF_NR_FAILED));
    } else if (!CopyProducedToReadback(consumerList_.Get())) {
        return abandon("the produced frame could not be read",
                       static_cast<int32_t>(LMXXF_NR_FAILED));
    }
    if (FAILED(consumerList_->Close()))
        return Fail(NeuralRenderFailure::Neural, "the consumer list would not close");

    // ---- Begin, then submit, then End ------------------------------------
    //
    // Begin sits exactly where the ABI puts it: after both lists are closed and
    // before the producer is on the queue. It is also what the runtime uses to
    // set up this frame's history chain, so a frame that reaches the queue
    // without it has no history to fuse with.
    if (!session.BeginRecording(queue_.Get())) {
        Report("BeginRecordingExecution", session.LastStatus());
        return abandon("BeginRecording", session.LastStatus());
    }

    {
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
    }
    // The producer is on the queue, so the runtime may submit behind it - the
    // same "very next thing on the same queue" rule the default order has.
    if (!session.EnqueueHip(queue_.Get())) {
        Report("EnqueueHip", session.LastStatus());
        return abandon("EnqueueHip", session.LastStatus());
    }
    if (!WaitForGpu()) return false;

    if (config_.log) {
        uint32_t state = 0;
        const int32_t pollStatus =
            runtime_.Api().Poll(runtime_.Context(), session.Job().handle, &state);
        Log("  Poll -> " + std::string(amd_lmxxf::LmxxfNrStatusName(pollStatus)) +
            " (job state " + std::to_string(state) + ")");
    }

    {
        ID3D12CommandList* lists[] = {consumerList_.Get()};
        queue_->ExecuteCommandLists(1, lists);
    }
    // The certificate EndRecording stores has to be the host's own tail Signal
    // over the submission. Both submission bits are set because both lists went
    // onto this one queue and this one fence covers them.
    uint64_t completionValue = 0;
    int32_t completionStatus = 0;
    if (!SignalFence(completionValue, completionStatus))
        return Fail(NeuralRenderFailure::Neural, "the completion fence would not signal");
    // WaitForGpu signals a later value on the same queue, so the certificate's
    // value is already complete by the time Collection looks at it and the
    // retry loop below is a formality rather than a wait.
    if (!WaitForGpu()) return false;

    if (!session.EndRecording(queue_.Get(),
                              LMXXF_NR_SUBMITTED_PRODUCER | LMXXF_NR_SUBMITTED_CONSUMER,
                              fence_.Get(), completionValue, completionStatus)) {
        Report("EndRecordingExecution", session.LastStatus());
        return Fail(NeuralRenderFailure::Neural, "EndRecording: " + session.LastError());
    }
    Report("EndRecordingExecution", static_cast<int32_t>(LMXXF_NR_OK));

    // Release it the lease way: declare no more work, then collect. Retire does
    // not exist in this job model.
    if (!session.InvalidateRecording()) {
        Report("InvalidateRecording", session.LastStatus());
        return Fail(NeuralRenderFailure::Neural, "InvalidateRecording: " + session.LastError());
    }
    int32_t collectStatus = static_cast<int32_t>(LMXXF_NR_OK);
    for (int attempt = 0; attempt < kCollectAttempts; ++attempt) {
        collectStatus = session.CollectRecording();
        if (collectStatus != static_cast<int32_t>(LMXXF_NR_UNAVAILABLE)) break;
    }
    if (collectStatus != static_cast<int32_t>(LMXXF_NR_OK) &&
        collectStatus != static_cast<int32_t>(LMXXF_NR_DEVICE_LOST)) {
        Report("CollectRecording", collectStatus);
        return Fail(NeuralRenderFailure::Neural, "CollectRecording: " + session.LastError());
    }
    Report("CollectRecording", collectStatus);
    // DEVICE_LOST here means the handle was consumed after a confirmed removal,
    // so the frame is readable but the device is not - that is a real stop, not
    // a cleanup detail.
    if (collectStatus == static_cast<int32_t>(LMXXF_NR_DEVICE_LOST))
        return Fail(NeuralRenderFailure::DeviceRemoved,
                    "CollectRecording released the job after the device was removed");

    // ---- read back -------------------------------------------------------
    if (!ReadBackProduced(out)) return false;

    LmxxfNrTimings timings;
    std::string timingError;
    if (runtime_.GetTimings(timings, &timingError) && timings.valid != 0)
        lastNeuralGpuMs_ = static_cast<double>(timings.network_ms);

    // Last, because GetStatus clears the runtime's own last-error string and
    // nothing after this needs the diagnostics that failure path would carry.
    RefreshHistoryStatus();
    SampleLocalVram();
    ++evaluations_;
    ++neuralEvaluations_;
    ++temporalFrames_;
    lastFailure_ = NeuralRenderFailure::None;
    lastError_.clear();
    return true;
}

bool AmdLmxxfEvaluator::SubmitFrame(const std::vector<uint8_t>& bgra, bool resetRequested,
                                    uint64_t sourceFrameNumber, std::vector<uint8_t>& out)
{
    // Two call orders, one per job model. The lease order is the only one the
    // temporal model runs under, so it is selected by the same flag that put the
    // session into that model rather than by anything about this frame.
    if (leases_) return SubmitFrameLease(bgra, resetRequested, sourceFrameNumber, out);

    amd_lmxxf::Session& session = *session_;
    const LmxxfNrApi& api = runtime_.Api();

    if (resetRequested) {
        // The renderer declares the reset on the identity, so the evaluator's own
        // decision and the caller's cannot disagree. ResetTemporal() carries the
        // same call for the seek path, which happens between passes.
        if (!session.ResetHistory()) {
            Report("ResetHistory", static_cast<int32_t>(LMXXF_NR_FAILED));
            return Fail(NeuralRenderFailure::Neural, "ResetHistory: " + session.LastError());
        }
    }

    // ---- producer: upload, PrepareFrame, RecordInputs --------------------
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr)))
        return Fail(NeuralRenderFailure::Neural, "the producer command list would not reset");

    // The same plan the lease path takes, for the same reason: where this
    // frame's source goes is decided before the upload puts it there. The two
    // call orders differ in where the runtime is asked to render, not in what
    // the accumulation around it is. A stacked pass suspends it: the pass runs
    // on the first evaluation's result only, so "what stacking does" and "what
    // blending does" stay two questions while this is being measured.
    const bool accumulates = config_.stability != TemporalStability::Off;
    FrameIdentity accumulationFrame{};
    temporal_stability::Plan accumulationPlan{};
    int sourceSlot = -1;
    if (accumulates) {
        accumulationFrame.frameNumber = sourceFrameNumber;
        accumulationPlan =
            temporal_stability::PlanFrame(accumulationHistory_, accumulationFrame, resetRequested);
        sourceSlot = static_cast<int>(accumulationPlan.write);
    }

    if (!UploadColor(bgra, sourceSlot))
        return Fail(NeuralRenderFailure::Neural, "the produced frame could not be uploaded");

    const uint64_t frameId = ++frameIndex_;
    const LmxxfNrFrameInfo frameInfo =
        amd_lmxxf::MakeFrameInfo(color_.Get(), width_, height_,
                                 static_cast<uint32_t>(kColorState), list_.Get(), 1, frameId, frameId);
    // Every call from here on goes through the Session, because that is what
    // advances its phase machine. Calling the raw table entries directly leaves
    // the phase stuck at Prepared, so a later Retire refuses as out-of-order
    // even though every individual call returned OK - the first hardware run
    // failed exactly that way.
    if (!session.PrepareFrame(frameInfo)) {
        Report("PrepareFrame", session.LastStatus());
        (void)list_->Close();
        return Fail(NeuralRenderFailure::Neural, "PrepareFrame: " + session.LastError());
    }

    if (!session.RecordInputs(list_.Get())) {
        Report("RecordInputs", session.LastStatus());
        (void)session.Cancel();
        (void)list_->Close();
        return Fail(session.LastStatus() == static_cast<int32_t>(LMXXF_NR_DEVICE_LOST)
                        ? NeuralRenderFailure::DeviceRemoved
                        : NeuralRenderFailure::Neural,
                    "RecordInputs: " + session.LastError());
    }

    if (FAILED(list_->Close()))
        return Fail(NeuralRenderFailure::Neural, "the producer list would not close");
    {
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
    }
    // The producer list is on the queue now, and the Session needs that fact
    // before it will accept EnqueueHip. Upstream's call-order note is explicit
    // that EnqueueHip is the very next thing on the same queue: no fence wait
    // and no Flush in between.
    if (!session.ProducerSubmitted())
        return Fail(NeuralRenderFailure::Neural, "producer marker: " + session.LastError());

    if (!session.EnqueueHip(queue_.Get())) {
        Report("EnqueueHip", session.LastStatus());
        (void)session.Cancel();
        return Fail(session.LastStatus() == static_cast<int32_t>(LMXXF_NR_DEVICE_LOST)
                        ? NeuralRenderFailure::DeviceRemoved
                        : NeuralRenderFailure::Neural,
                    "EnqueueHip: " + session.LastError());
    }
    if (!WaitForGpu()) return false;

    // Poll is read-only diagnostics: it reports the job state but advances
    // nothing, so it stays a direct call rather than a phase transition.
    if (config_.log) {
        uint32_t state = 0;
        const int32_t pollStatus = api.Poll(runtime_.Context(), session.Job().handle, &state);
        Log("  Poll -> " + std::string(amd_lmxxf::LmxxfNrStatusName(pollStatus)) +
            " (job state " + std::to_string(state) + ")");
    }

    // ---- consumer: RecordOutputs, copy the produced texture out ----------
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr)))
        return Fail(NeuralRenderFailure::Neural, "the consumer command list would not reset");
    if (!session.RecordOutputs(list_.Get())) {
        Report("RecordOutputs", session.LastStatus());
        (void)session.Cancel();
        (void)list_->Close();
        return Fail(session.LastStatus() == static_cast<int32_t>(LMXXF_NR_DEVICE_LOST)
                        ? NeuralRenderFailure::DeviceRemoved
                        : NeuralRenderFailure::Neural,
                    "RecordOutputs: " + session.LastError());
    }
    if (accumulates) {
        if (!RecordTemporalAccumulation(list_.Get(), accumulationPlan, accumulationFrame)) {
            (void)session.Cancel();
            (void)list_->Close();
            return false;
        }
    } else {
        auto* produced = static_cast<ID3D12Resource*>(session_->Job().private_output);
        if (produced == nullptr) {
            (void)session.Cancel();
            (void)list_->Close();
            return Fail(NeuralRenderFailure::Neural,
                        "the runtime published no job.private_output, so there is no frame to read");
        }
        // The stretch reads the frame as a shader resource; the plain capture
        // copies it instead. Both put it back where the runtime left it.
        if (Upscaling()) {
            Transition(list_.Get(), produced, kPrivateOutputState,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            const bool recorded =
                RecordUpscale(list_.Get(), produced);
            Transition(list_.Get(), produced, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       kPrivateOutputState);
            if (!recorded) {
                (void)session.Cancel();
                (void)list_->Close();
                return false;
            }
        } else if (!CopyProducedToReadback(list_.Get())) {
            (void)session.Cancel();
            (void)list_->Close();
            return false;
        }
    }
    if (FAILED(list_->Close()))
        return Fail(NeuralRenderFailure::Neural, "the consumer list would not close");
    {
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
    }
    if (!WaitForGpu()) return false;

    // The output reader is on the queue before Retire, which is the contract the
    // ABI states for output reuse and destruction.
    if (!session.Retire()) {
        Report("Retire", session.LastStatus());
        return Fail(NeuralRenderFailure::Neural, "Retire: " + session.LastError());
    }
    Report("Retire", static_cast<int32_t>(LMXXF_NR_OK));

    // ---- read back -------------------------------------------------------
    if (!ReadBackProduced(out)) return false;

    LmxxfNrTimings timings;
    std::string timingError;
    if (runtime_.GetTimings(timings, &timingError) && timings.valid != 0)
        lastNeuralGpuMs_ = static_cast<double>(timings.network_ms);

    SampleLocalVram();
    ++evaluations_;
    ++neuralEvaluations_;
    lastFailure_ = NeuralRenderFailure::None;
    lastError_.clear();
    return true;
}

bool AmdLmxxfEvaluator::Submit(const OfflineDecodedFrame& frame, const FrameIdentity& id,
                               bool capture, OfflineEvaluation& out)
{
    if (!Ready()) return Fail(NeuralRenderFailure::Neural, "Submit before a successful Initialize");
    const size_t inputBytes = static_cast<size_t>(width_) * height_ * 4u;
    if (frame.bgra.size() < inputBytes) {
        return Fail(NeuralRenderFailure::Source,
                    "the decoded frame is " + std::to_string(frame.bgra.size()) +
                        " bytes, short of the " + std::to_string(inputBytes) +
                        " the persistent colour texture needs");
    }

    const auto started = std::chrono::steady_clock::now();
    std::vector<uint8_t> produced;
    // The source's own frame number, which is what names this frame's guide
    // files. The evaluator's internal frameIndex_ is a submission counter and
    // deliberately not used for that: a renderer that evaluates the same source
    // frame twice - a preroll, a retry, a repeat - has to be handed the same
    // guides both times, and a counter would drift away from the decoder.
    if (!SubmitFrame(frame.bgra, id.reset != HistoryReset::None, id.frameNumber, produced))
        return false;
    lastSubmitMs_ =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    if (capture) {
        // An upscaling capture reads back at the capture's size; the frame
        // check is for the plain path, where the readback IS the frame.
        const size_t readbackBytes = Upscaling()
                                         ? static_cast<size_t>(upscaleWidth_) * upscaleHeight_ * 4u
                                         : inputBytes;
        if (produced.size() != readbackBytes)
            return Fail(NeuralRenderFailure::Neural,
                        "the readback is " + std::to_string(produced.size()) +
                            " bytes, not the " + std::to_string(readbackBytes) +
                            " the capture reads back");
        out.bgra = std::move(produced);
    }
    out.id = id;
    return true;
}

bool AmdLmxxfEvaluator::FeatureCreated() const
{
    // The AMD runtime has no NGX-style feature to create: what the renderer's
    // priming loop waits for on the NVIDIA path is a D3D12Renderer-side effect
    // that does not exist here. A prepared session IS the capability, and
    // answering true is what tells the priming loop it has nothing to wait for
    // (docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md, the ABI 3 note on hip_ready
    // and GetStatus).
    return prepared_;
}

uint64_t AmdLmxxfEvaluator::EvaluationCount() const { return evaluations_; }

std::string AmdLmxxfEvaluator::LiveRuntimeStatus() const
{
    if (!runtime_.Loaded()) return {};
    return runtime_.Status();
}

uint64_t AmdLmxxfEvaluator::NeuralEvaluations() const { return neuralEvaluations_; }

void AmdLmxxfEvaluator::ResetTemporal()
{
    // The host's own history goes with the runtime's. This is called between
    // passes, for a seek, where the next frame is not the successor of the last
    // one - and it is called without anything on the frame saying so, so a
    // history left standing here would be blended into a shot it does not
    // belong to.
    accumulationHistory_ = {};
    if (!session_.has_value()) return;
    if (!session_->ResetHistory())
        lastError_ = "ResetHistory: " + session_->LastError();
}

NeuralRenderFailure AmdLmxxfEvaluator::LastFailure() const { return lastFailure_; }

double AmdLmxxfEvaluator::LastNeuralGpuMs() const { return lastNeuralGpuMs_; }

uint64_t AmdLmxxfEvaluator::PeakLocalVideoMemoryMiB() const { return peakLocalVramMib_; }

void AmdLmxxfEvaluator::SampleLocalVram()
{
    // The device's own view of the process's local segment is the D3D12-blessed
    // one, and it stays correct when the adapter was borrowed rather than
    // enumerated here, so it is asked first.
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (adapter_ == nullptr) {
        if (localVramNote_.empty())
            localVramNote_ = "no IDXGIAdapter3 could be obtained from the D3D12 device, so the "
                             "runtime's own video memory use could not be measured";
        return;
    }
    const HRESULT query = adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
    if (FAILED(query)) {
        if (localVramNote_.empty())
            localVramNote_ = "QueryVideoMemoryInfo(LOCAL) returned 0x" + Hex32(query) +
                             ", so the runtime's own video memory use could not be measured";
        return;
    }
    peakLocalVramMib_ = std::max(peakLocalVramMib_, info.CurrentUsage / (1024ull * 1024ull));
    // Assigned rather than maximised: this is the point sample, and the whole
    // reason it exists beside the peak is that the two differ while a helper
    // sits idle between jobs.
    currentLocalVramMib_ = info.CurrentUsage / (1024ull * 1024ull);
    localVramBudgetMib_ = std::max(localVramBudgetMib_, info.Budget / (1024ull * 1024ull));
    localVramNote_.clear();
}

std::string AmdLmxxfEvaluator::LocalVideoMemoryNote() const
{
    if (!localVramNote_.empty()) return localVramNote_;
    if (localVramBudgetMib_ == 0) return "the local segment budget also read as zero";
    // CurrentUsage is documented as process-wide, and some drivers leave it at
    // zero on a resizable-BAR part while still honouring Budget, so the budget is
    // reported beside the usage instead of being folded into it.
    return "local segment usage " + std::to_string(peakLocalVramMib_) + " MiB of a " +
           std::to_string(localVramBudgetMib_) + " MiB budget";
}
