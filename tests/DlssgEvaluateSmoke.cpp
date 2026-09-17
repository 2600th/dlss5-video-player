// Opt-in hardware experiment: registered under the `gpu` CTest label, which the
// portable suite excludes (`ctest -LE gpu`) and an RTX machine opts into with
// `ctest -L gpu`.
//
// DlssgProbeSmoke established that the raw NGX path ADMITS
// NVSDK_NGX_Feature_FrameGeneration on this machine. Admission is not
// production: NVSDK_NGX_Result_Success with an untouched output texture is a
// negative result, and a return code alone cannot tell the two apart. This
// experiment settles it by measurement.
//
// A real clip cannot settle it. Nothing in a decoded pair distinguishes "the
// runtime interpolated" from "the runtime copied one input", because both look
// like plausible video. So the inputs here are synthetic with exactly known
// ground truth: one bright 200x200 square on a black field, translated by
// exactly +200 px horizontally between frame A and frame B, and nothing else in
// the image moves. The true intermediate frame therefore has exactly one
// correct answer - the square's horizontal centroid sits midway between A's and
// B's - so the output is read back to the CPU and that centroid is measured.
// A copy of either input lands on that input's centroid, 100 px away from the
// midpoint, and a runtime that wrote nothing leaves the sentinel fill intact.
//
// The exit code reports whether the experiment RAN, not what it found. A
// runtime that produces no interpolation is a finding to report - it is the
// answer this program exists to obtain - so every verdict, negative included,
// exits 0. Only a device, an NGX session or a feature that could not be brought
// up at all exits non-zero, because those are the cases where no question was
// asked.
#include <windows.h>

#include "DLSSGBackend.h"
#include "NeuralPreflight.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;
constexpr DXGI_FORMAT kBackbufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr size_t kPixelCount = size_t(kWidth) * size_t(kHeight);

// The ground truth. A square this size is far larger than any interpolation
// kernel, so its centroid is decided by where the runtime put the square and
// not by how it treated the edges.
constexpr uint32_t kRectSize = 200;
constexpr uint32_t kRectAX = 600;
constexpr uint32_t kRectBX = 800;
constexpr uint32_t kRectY = (kHeight - kRectSize) / 2;
constexpr float kRectShiftX = float(kRectBX) - float(kRectAX);

// Written into the output before every evaluate, so "the runtime wrote nothing"
// is a distinct measurement from "the runtime wrote black". No pixel of the
// synthetic pair carries this value, so any pixel that still has it after an
// evaluate was not written.
constexpr uint8_t kSentinelB = 0x20;
constexpr uint8_t kSentinelG = 0x00;
constexpr uint8_t kSentinelR = 0x40;

// A decoded frame has no depth. This is a flat proxy: one constant plane for
// the whole image, which is the position TemporalGuides already takes for
// DLSS-SR, where the depth guide is derived from luma and motion rather than
// measured. 0.5 is clear of both ends of the runtime's linearization
// (lin = 1/(1-depth) with depthInverted false, nvsdk_ngx_defs_dlssg.h), which
// is singular at 1.0.
constexpr float kFlatDepth = 0.5f;

// Which convention the motion-vector texture is written in. Both are run and
// both are reported: DLSSG.MvecScale{X,Y} are documented as normalizing the
// buffer into [-1,1], but whether the raw path applies them at all was unknown,
// and handing the same -200 px displacement over under both conventions is the
// only way to find out. Under Pixels the texel holds -200.0 and the backend
// scales by 1/extent; under Normalized the texel already holds -200/1920 and
// the scale is 1.0. A runtime that applies MvecScale sees the same motion in
// both; one that ignores it sees the true motion in exactly one of them.
enum class MvEncoding { Pixels, Normalized };

// The third run hands over a motion buffer that says nothing moved while the
// colour pair still shows the square jumping 200 px. It is the control that
// separates the two ways a runtime could reach a correct intermediate frame:
// if a deliberately wrong guide changes the output, the motion buffer drove the
// interpolation, and if it does not, the runtime derived the motion from the
// colour pair itself and the guide was decoration. Both answers matter to a
// video path that has to decide whether to produce motion vectors at all, and
// neither is visible from a return code or from the two honest encodings on
// their own - those two describe the same displacement, so agreeing proves
// only that MvecScale was applied consistently.

std::string Narrow(std::wstring_view text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string narrow(size_t(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), narrow.data(), length, nullptr, nullptr);
    return narrow;
}

std::string HexResult(NVSDK_NGX_Result result)
{
    return Narrow(HexResultTextWide(uint32_t(result)));
}

// IEEE 754 binary32 -> binary16, truncating the mantissa. The three values this
// has to carry are 0, -200 (exact in half) and -200/1920, whose half
// quantization step is 6.1e-5 - 0.12 px once the backend scales it back up by
// the frame width, which is two orders of magnitude below the 100 px that
// separates an interpolation from a copy.
uint16_t FloatToHalf(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int exponent = int((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent >= 31) return uint16_t(sign | 0x7C00u);
    if (exponent <= 0) {
        if (exponent < -10) return uint16_t(sign);
        mantissa |= 0x800000u;
        return uint16_t(sign | (mantissa >> uint32_t(14 - exponent)));
    }
    return uint16_t(sign | (uint32_t(exponent) << 10) | (mantissa >> 13));
}

// One frame of the pair: an opaque white square on an opaque black field.
std::vector<uint8_t> BuildFrame(uint32_t rectX)
{
    std::vector<uint8_t> image(kPixelCount * 4, 0);
    for (size_t index = 3; index < image.size(); index += 4) image[index] = 0xFF; // opaque black field
    for (uint32_t y = kRectY; y < kRectY + kRectSize; ++y) {
        std::memset(image.data() + (size_t(y) * kWidth + rectX) * 4, 0xFF, size_t(kRectSize) * 4);
    }
    return image;
}

// Motion points from the current frame back to the previous one, the direction
// both DLSS-SR and DLSS-G take, so the vector lives inside the square's
// footprint in B and reads -200 px on x, 0 on y. The rest of frame B is the
// unchanged black field: it did not move, so its vector is zero - a measured
// "still", not a missing value, which is why DLSSGBackend declares an
// unreachable motionVectorsInvalidValue rather than 0.
std::vector<uint8_t> BuildMotionVectors(MvEncoding encoding, bool moving)
{
    const float motionX = !moving ? 0.0f
                                  : (encoding == MvEncoding::Pixels ? -kRectShiftX
                                                                    : -kRectShiftX / float(kWidth));
    const uint16_t packedX = FloatToHalf(motionX);
    const uint16_t packedY = FloatToHalf(0.0f);
    std::vector<uint8_t> texels(kPixelCount * 4, 0);
    if (!moving) return texels;
    for (uint32_t y = kRectY; y < kRectY + kRectSize; ++y) {
        uint16_t* row = reinterpret_cast<uint16_t*>(texels.data()) + (size_t(y) * kWidth + kRectBX) * 2;
        for (uint32_t x = 0; x < kRectSize; ++x) {
            row[x * 2 + 0] = packedX;
            row[x * 2 + 1] = packedY;
        }
    }
    return texels;
}

std::vector<uint8_t> BuildFlatDepth()
{
    std::vector<uint8_t> texels(kPixelCount * 4);
    for (size_t pixel = 0; pixel < kPixelCount; ++pixel) {
        std::memcpy(texels.data() + pixel * 4, &kFlatDepth, sizeof(kFlatDepth));
    }
    return texels;
}

std::vector<uint8_t> BuildSentinel()
{
    std::vector<uint8_t> image(kPixelCount * 4);
    for (size_t pixel = 0; pixel < kPixelCount; ++pixel) {
        uint8_t* bgra = image.data() + pixel * 4;
        bgra[0] = kSentinelB;
        bgra[1] = kSentinelG;
        bgra[2] = kSentinelR;
        bgra[3] = 0xFF;
    }
    return image;
}

double Luma(const uint8_t* bgra)
{
    return 0.0722 * bgra[0] + 0.7152 * bgra[1] + 0.2126 * bgra[2];
}

// Mean x of the bright pixels, with the horizontal extent they span. The square
// is white on black, so any threshold in the middle of the range separates
// them; 128 is used so a softened interpolation edge contributes symmetrically
// and cannot bias the mean. The extent is what tells one warped square from two
// ghosts of it: a single 200 px square spans about 200 px wherever it landed,
// while a cross-fade of both inputs spans the 400 px from A's left edge to B's
// right edge and would sit at the midpoint for the wrong reason.
struct Centroid {
    double x = 0.0;
    uint64_t brightPixels = 0;
    uint32_t minX = kWidth;
    uint32_t maxX = 0;
};

Centroid MeasureCentroid(const uint8_t* image, size_t rowPitch)
{
    Centroid measured;
    double sumX = 0.0;
    for (uint32_t y = 0; y < kHeight; ++y) {
        const uint8_t* row = image + size_t(y) * rowPitch;
        for (uint32_t x = 0; x < kWidth; ++x) {
            if (Luma(row + size_t(x) * 4) <= 128.0) continue;
            sumX += double(x);
            ++measured.brightPixels;
            if (x < measured.minX) measured.minX = x;
            if (x > measured.maxX) measured.maxX = x;
        }
    }
    if (measured.brightPixels) measured.x = sumX / double(measured.brightPixels);
    return measured;
}

// Mean absolute per-channel difference over the whole frame, in 0..255 units.
// This is what decides "differs from input": a copy of an input is 0 here even
// when its centroid happens to look plausible.
double MeanAbsoluteDifference(const uint8_t* image, size_t rowPitch, const std::vector<uint8_t>& reference)
{
    double total = 0.0;
    for (uint32_t y = 0; y < kHeight; ++y) {
        const uint8_t* row = image + size_t(y) * rowPitch;
        const uint8_t* referenceRow = reference.data() + size_t(y) * kWidth * 4;
        for (uint32_t x = 0; x < kWidth; ++x) {
            for (uint32_t channel = 0; channel < 3; ++channel) {
                total += std::abs(double(row[size_t(x) * 4 + channel]) - double(referenceRow[size_t(x) * 4 + channel]));
            }
        }
    }
    return total / double(kPixelCount * 3);
}

uint64_t CountSentinel(const uint8_t* image, size_t rowPitch)
{
    uint64_t remaining = 0;
    for (uint32_t y = 0; y < kHeight; ++y) {
        const uint8_t* row = image + size_t(y) * rowPitch;
        for (uint32_t x = 0; x < kWidth; ++x) {
            const uint8_t* bgra = row + size_t(x) * 4;
            if (bgra[0] == kSentinelB && bgra[1] == kSentinelG && bgra[2] == kSentinelR) ++remaining;
        }
    }
    return remaining;
}

// The renderer's adapter choice, which is what the player would evaluate on:
// DXGI's high-performance order, software adapters skipped, an NVIDIA one
// preferred over the first that merely creates a device.
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
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr))) continue;
        if (!fallback) fallback = adapter;
        if (desc.VendorId == 0x10DE) return adapter;
    }
    return fallback;
}

// One command list, submitted and waited out between every stage. NGX records
// the feature create on the list it is handed and the DLSS Programming Guide
// requires that work to have retired before the first evaluate, so every stage
// here ends at an idle GPU rather than pipelining.
struct Gpu {
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
        if (WaitForSingleObject(idle, 30000) != WAIT_OBJECT_0) return false;
        return SUCCEEDED(allocator->Reset()) && SUCCEEDED(cmd->Reset(allocator.Get(), nullptr));
    }
};

D3D12_RESOURCE_DESC Tex2D(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

ComPtr<ID3D12Resource> CreateTexture(ID3D12Device* device, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                     D3D12_RESOURCE_STATES state, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_RESOURCE_DESC desc = Tex2D(format, flags);
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
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

// Records a texture upload; the returned staging buffer has to stay alive until
// the caller's Flush. `texture` must already be in COPY_DEST.
ComPtr<ID3D12Resource> RecordUpload(Gpu& gpu, ID3D12Resource* texture, const std::vector<uint8_t>& pixels)
{
    const Footprint footprint = DescribeCopy(gpu.device.Get(), texture);
    ComPtr<ID3D12Resource> staging =
        CreateBuffer(gpu.device.Get(), footprint.totalBytes, D3D12_HEAP_TYPE_UPLOAD,
                     D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!staging) return nullptr;

    uint8_t* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return nullptr;
    const size_t sourcePitch = pixels.size() / kHeight;
    for (UINT row = 0; row < footprint.rows; ++row) {
        std::memcpy(mapped + size_t(row) * footprint.placed.Footprint.RowPitch,
                    pixels.data() + size_t(row) * sourcePitch, size_t(footprint.rowBytes));
    }
    staging->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = staging.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint.placed;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    gpu.cmd->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    return staging;
}

void RecordReadback(Gpu& gpu, ID3D12Resource* texture, ID3D12Resource* readback, const Footprint& footprint)
{
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint.placed;
    gpu.cmd->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
}

// One motion-vector story handed to the same feature: a label, the texture that
// carries it, and the units the backend should read that texture in.
struct RunPlan {
    const char* label;
    ID3D12Resource* motion;
    DLSSGBackend::MotionVectorUnits units;
};

struct RunReport {
    const char* label = "";
    bool ran = false;
    NVSDK_NGX_Result resetEvaluate = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Result evaluate = NVSDK_NGX_Result_Fail;
    Centroid interpolated;
    double meanAbsDiffFromA = 0.0;
    double meanAbsDiffFromB = 0.0;
    double meanAbsDiffFromFirstRun = 0.0;
    uint64_t sentinelRemaining = 0;
    uint64_t evaluations = 0;
    std::vector<uint8_t> image; // packed BGRA copy, so runs can be compared to each other
};

// The verdict, from the measurement and nothing else. Reaching an
// "interpolated" answer at all needs the image to differ from both inputs AND
// the square to have landed between them, so a copy cannot get there and
// neither can an output that merely looks plausible. Landing between them
// without landing on the midpoint is its own answer: it is a real intermediate
// frame with a bias, which is a different finding from no interpolation and has
// to be reported as the different thing it is.
const char* Verdict(const RunReport& report, double centroidA, double centroidB, double expectedMidpoint)
{
    constexpr double kIdenticalImage = 0.5;    // 0..255 mean channel difference
    constexpr double kMidpointTolerance = 8.0; // px, against the 100 px copy/interpolate gap
    constexpr double kCopyMargin = 20.0;       // px clear of either input's centroid
    if (!report.ran) return "notRun";
    if (report.sentinelRemaining == kPixelCount) return "outputUntouched";
    if (report.meanAbsDiffFromA <= kIdenticalImage) return "copyOfA";
    if (report.meanAbsDiffFromB <= kIdenticalImage) return "copyOfB";
    if (!report.interpolated.brightPixels) return "noSquareInOutput";
    if (report.interpolated.x < std::min(centroidA, centroidB) + kCopyMargin ||
        report.interpolated.x > std::max(centroidA, centroidB) - kCopyMargin) {
        return "squareNotBetweenInputs";
    }
    if (std::abs(report.interpolated.x - expectedMidpoint) <= kMidpointTolerance) {
        return "interpolatedAtMidpoint";
    }
    return "interpolatedOffMidpoint";
}

} // namespace

int wmain()
{
    std::cout << std::fixed << std::setprecision(2);

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        std::cout << "experiment=unreachable reason=CreateDXGIFactory2 failed\n";
        return 2;
    }
    ComPtr<IDXGIAdapter1> adapter = SelectAdapter(factory.Get());
    if (!adapter) {
        std::cout << "experiment=unreachable reason=no D3D12 hardware adapter\n";
        return 2;
    }
    DXGI_ADAPTER_DESC1 adapterDesc{};
    adapter->GetDesc1(&adapterDesc);

    Gpu gpu;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&gpu.device)))) {
        std::cout << "experiment=unreachable reason=D3D12CreateDevice failed\n";
        return 2;
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    gpu.idle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!gpu.idle ||
        FAILED(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&gpu.queue))) ||
        FAILED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator))) ||
        FAILED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&gpu.cmd))) ||
        FAILED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)))) {
        std::cout << "experiment=unreachable reason=command queue/list bring-up failed\n";
        return 2;
    }

    // The interpolated output is the only resource the runtime writes, so it is
    // the only one created with a UAV. A real swapchain backbuffer cannot carry
    // that flag, which is one reason Streamline owns the swapchain; this
    // experiment hands over its own texture instead and reports the format's
    // UAV support so a machine where that is the obstacle says so.
    D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport{kBackbufferFormat, D3D12_FORMAT_SUPPORT1_NONE,
                                                    D3D12_FORMAT_SUPPORT2_NONE};
    gpu.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport));
    const bool outputUavSupported =
        (formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0;

    ComPtr<ID3D12Resource> frameA =
        CreateTexture(gpu.device.Get(), kBackbufferFormat, D3D12_RESOURCE_FLAG_NONE,
                      D3D12_RESOURCE_STATE_COPY_DEST, L"DLSSG_FrameA_SquareAt600");
    ComPtr<ID3D12Resource> frameB =
        CreateTexture(gpu.device.Get(), kBackbufferFormat, D3D12_RESOURCE_FLAG_NONE,
                      D3D12_RESOURCE_STATE_COPY_DEST, L"DLSSG_FrameB_SquareAt800");
    ComPtr<ID3D12Resource> depth =
        CreateTexture(gpu.device.Get(), DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                      D3D12_RESOURCE_STATE_COPY_DEST, L"DLSSG_FlatDepthProxy_R32F");
    ComPtr<ID3D12Resource> motionStill =
        CreateTexture(gpu.device.Get(), DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                      D3D12_RESOURCE_STATE_COPY_DEST, L"DLSSG_MotionVectors_Zero_RG16F");
    ComPtr<ID3D12Resource> motionPixels =
        CreateTexture(gpu.device.Get(), DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                      D3D12_RESOURCE_STATE_COPY_DEST, L"DLSSG_MotionVectors_Pixels_RG16F");
    ComPtr<ID3D12Resource> motionNormalized =
        CreateTexture(gpu.device.Get(), DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                      D3D12_RESOURCE_STATE_COPY_DEST, L"DLSSG_MotionVectors_Normalized_RG16F");
    ComPtr<ID3D12Resource> output =
        CreateTexture(gpu.device.Get(), kBackbufferFormat,
                      outputUavSupported ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE,
                      D3D12_RESOURCE_STATE_COPY_DEST, L"DLSSG_OutputInterpolated");
    if (!frameA || !frameB || !depth || !motionStill || !motionPixels || !motionNormalized || !output) {
        std::cout << "experiment=unreachable reason=synthetic resource creation failed\n";
        return 2;
    }

    const Footprint outputFootprint = DescribeCopy(gpu.device.Get(), output.Get());
    ComPtr<ID3D12Resource> readback = CreateBuffer(gpu.device.Get(), outputFootprint.totalBytes,
                                                   D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback) {
        std::cout << "experiment=unreachable reason=readback buffer creation failed\n";
        return 2;
    }

    const std::vector<uint8_t> imageA = BuildFrame(kRectAX);
    const std::vector<uint8_t> imageB = BuildFrame(kRectBX);
    const std::vector<uint8_t> sentinel = BuildSentinel();

    // Every input is uploaded once and left in the state NGX reads guides from,
    // which is the same NON_PIXEL_SHADER_RESOURCE the DLSS-SR path uses
    // (D3D12Renderer::GuideReadState).
    ComPtr<ID3D12Resource> stagingA = RecordUpload(gpu, frameA.Get(), imageA);
    ComPtr<ID3D12Resource> stagingB = RecordUpload(gpu, frameB.Get(), imageB);
    ComPtr<ID3D12Resource> stagingDepth = RecordUpload(gpu, depth.Get(), BuildFlatDepth());
    ComPtr<ID3D12Resource> stagingStill =
        RecordUpload(gpu, motionStill.Get(), BuildMotionVectors(MvEncoding::Pixels, false));
    ComPtr<ID3D12Resource> stagingPixels =
        RecordUpload(gpu, motionPixels.Get(), BuildMotionVectors(MvEncoding::Pixels, true));
    ComPtr<ID3D12Resource> stagingNormalized =
        RecordUpload(gpu, motionNormalized.Get(), BuildMotionVectors(MvEncoding::Normalized, true));
    if (!stagingA || !stagingB || !stagingDepth || !stagingStill || !stagingPixels || !stagingNormalized) {
        std::cout << "experiment=unreachable reason=synthetic upload staging failed\n";
        return 2;
    }
    Barrier(gpu.cmd.Get(), frameA.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(gpu.cmd.Get(), frameB.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(gpu.cmd.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(gpu.cmd.Get(), motionStill.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(gpu.cmd.Get(), motionPixels.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(gpu.cmd.Get(), motionNormalized.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    DLSSGBackend backend;
    const bool created = backend.Initialize(gpu.device.Get(), gpu.cmd.Get(), kWidth, kHeight, kBackbufferFormat);
    // The uploads and the feature create share this submission, and the wait
    // retires both before the first evaluate reads either.
    const bool flushed = gpu.Flush();

    std::cout << "adapter=" << Narrow(adapterDesc.Description) << "\n"
              << "geometry=" << kWidth << "x" << kHeight << " format=" << int(kBackbufferFormat) << "\n"
              << "outputUavSupported=" << (outputUavSupported ? "true" : "false") << "\n"
              << "featureCreated=" << (created && backend.FeatureCreated() ? "true" : "false") << "\n"
              << "createResult=" << HexResult(backend.LastResult()) << "\n";
    if (!created || !flushed) {
        std::cout << "experiment=unreachable reason="
                  << (created ? "command list submission failed" : "CreateFeature(FrameGeneration) refused")
                  << "\n";
        return backend.SessionEstablished() ? 4 : 3;
    }

    const Centroid centroidA = MeasureCentroid(imageA.data(), size_t(kWidth) * 4);
    const Centroid centroidB = MeasureCentroid(imageB.data(), size_t(kWidth) * 4);
    const double expectedMidpoint = 0.5 * (centroidA.x + centroidB.x);

    const RunPlan plans[] = {
        {"backbufferPixels", motionPixels.Get(), DLSSGBackend::MotionVectorUnits::BackbufferPixels},
        {"normalizedScreen", motionNormalized.Get(), DLSSGBackend::MotionVectorUnits::NormalizedScreen},
        // The control: the same colour pair with a motion buffer that claims
        // nothing moved. Units are irrelevant to an all-zero buffer, so the
        // default pixel units are kept and only the story changes.
        {"zeroMotionControl", motionStill.Get(), DLSSGBackend::MotionVectorUnits::BackbufferPixels},
    };
    constexpr size_t kRunCount = sizeof(plans) / sizeof(plans[0]);
    RunReport reports[kRunCount];
    for (size_t run = 0; run < kRunCount; ++run) {
        RunReport& report = reports[run];
        report.label = plans[run].label;
        backend.SetMotionVectorUnits(plans[run].units);
        ID3D12Resource* motion = plans[run].motion;

        // Frame A establishes history. reset=true says the previous frame has no
        // connection to this one, which is true of the first frame of the run and
        // is also what makes the second run independent of the first: the same
        // feature is reused, and reset is the documented way to break the
        // continuity it holds rather than re-creating it and re-initializing NGX.
        ComPtr<ID3D12Resource> stagingSentinelA = RecordUpload(gpu, output.Get(), sentinel);
        Barrier(gpu.cmd.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool evaluatedA = backend.Evaluate(gpu.cmd.Get(), frameA.Get(), motionStill.Get(), depth.Get(),
                                                 output.Get(), 1, 1, true);
        report.resetEvaluate = backend.LastResult();
        Barrier(gpu.cmd.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_DEST);
        if (!gpu.Flush()) {
            std::cout << "experiment=unreachable reason=submission failed after the reset evaluate\n";
            return 4;
        }

        // Frame B is the measurement. The output is re-filled with the sentinel
        // first, so whatever is read back came from this evaluate alone.
        ComPtr<ID3D12Resource> stagingSentinelB = RecordUpload(gpu, output.Get(), sentinel);
        Barrier(gpu.cmd.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool evaluatedB = backend.Evaluate(gpu.cmd.Get(), frameB.Get(), motion, depth.Get(), output.Get(),
                                                 1, 1, false);
        report.evaluate = backend.LastResult();
        Barrier(gpu.cmd.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        RecordReadback(gpu, output.Get(), readback.Get(), outputFootprint);
        Barrier(gpu.cmd.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!gpu.Flush()) {
            std::cout << "experiment=unreachable reason=submission failed after the measured evaluate\n";
            return 4;
        }

        report.ran = evaluatedA && evaluatedB;
        report.evaluations = backend.EvaluationCount();

        uint8_t* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
            std::cout << "experiment=unreachable reason=output readback map failed\n";
            return 4;
        }
        const size_t rowPitch = outputFootprint.placed.Footprint.RowPitch;
        report.interpolated = MeasureCentroid(mapped, rowPitch);
        report.meanAbsDiffFromA = MeanAbsoluteDifference(mapped, rowPitch, imageA);
        report.meanAbsDiffFromB = MeanAbsoluteDifference(mapped, rowPitch, imageB);
        report.sentinelRemaining = CountSentinel(mapped, rowPitch);
        // Kept so the runs can be compared to each other rather than only to the
        // inputs: whether a wrong motion buffer changes the output at all is the
        // question the control run exists to answer.
        report.image.resize(kPixelCount * 4);
        for (uint32_t y = 0; y < kHeight; ++y) {
            std::memcpy(report.image.data() + size_t(y) * kWidth * 4, mapped + size_t(y) * rowPitch,
                        size_t(kWidth) * 4);
        }
        readback->Unmap(0, nullptr);
        report.meanAbsDiffFromFirstRun =
            run == 0 ? 0.0 : MeanAbsoluteDifference(report.image.data(), size_t(kWidth) * 4, reports[0].image);
    }

    for (const RunReport& report : reports) {
        std::cout << "\nmvecEncoding=" << report.label << "\n"
                  << "centroidA=" << centroidA.x << "\n"
                  << "centroidB=" << centroidB.x << "\n"
                  << "centroidInterpolated=" << report.interpolated.x << "\n"
                  << "expectedMidpoint=" << expectedMidpoint << "\n"
                  << "deltaFromMidpoint=" << report.interpolated.x - expectedMidpoint << "\n"
                  << "differsFromA=" << (report.meanAbsDiffFromA > 0.5 ? "true" : "false") << "\n"
                  << "differsFromB=" << (report.meanAbsDiffFromB > 0.5 ? "true" : "false") << "\n"
                  << "evaluateResult=" << HexResult(report.evaluate) << "\n"
                  << "evaluations=" << report.evaluations << "\n"
                  << "resetEvaluateResult=" << HexResult(report.resetEvaluate) << "\n"
                  << "meanAbsDiffFromA=" << report.meanAbsDiffFromA << "\n"
                  << "meanAbsDiffFromB=" << report.meanAbsDiffFromB << "\n"
                  << "meanAbsDiffFromFirstRun=" << report.meanAbsDiffFromFirstRun << "\n"
                  << "brightPixelsA=" << centroidA.brightPixels << "\n"
                  << "brightPixelsB=" << centroidB.brightPixels << "\n"
                  << "brightPixelsInterpolated=" << report.interpolated.brightPixels << "\n"
                  << "brightSpanA=" << centroidA.minX << ".." << centroidA.maxX << "\n"
                  << "brightSpanB=" << centroidB.minX << ".." << centroidB.maxX << "\n"
                  << "brightSpanInterpolated=" << report.interpolated.minX << ".." << report.interpolated.maxX
                  << "\n"
                  << "sentinelPixelsRemaining=" << report.sentinelRemaining << " of " << kPixelCount << "\n"
                  << "verdict=" << Verdict(report, centroidA.x, centroidB.x, expectedMidpoint) << "\n";
    }

    std::cout << "\nexperiment=ran\n"
              << "deviceRemovedReason=0x" << std::hex << uint32_t(gpu.device->GetDeviceRemovedReason()) << std::dec
              << "\n";
    return 0;
}
