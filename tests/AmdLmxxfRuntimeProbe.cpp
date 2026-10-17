// Stage A of docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md.
//
// Loads the third-party amd-neural-runtime/LmxxfNrRuntime.dll, proves the
// vendored C ABI (third_party/lmxxf/LmxxfNrApi.h) against the real binary and
// then walks the documented call order as far as the runtime lets it go. Every
// step prints its own status code, the runtime's own GetLastError text and its
// GetStatus line, so a stop says which call refused and what it wanted.
//
// It drives the LmxxfNrApi table directly rather than through
// src/AmdLmxxfSession.h: the session folds the status code into a sentence and
// this probe's whole purpose is to keep the code visible. The order walked here
// is the order that session enforces.
//
// Nothing here is wired into the player: this is a probe, and its job is to
// turn "the plan says this should work" into a status code per call.
//
// The synthetic frame is a gradient in red and green, never flat, so the output
// can be judged twice over: a black frame means the runtime produced nothing,
// and a frame identical to the input means it produced the input again.
//
// Exit codes:
//   0    every reachable step passed. A call the runtime answers
//        LMXXF_NR_NOT_IMPLEMENTED is reported as an upstream gap and does not
//        fail the probe by itself - but the frame gate below still can.
//   1    a step the runtime claims to implement failed, the frame gate did, or
//        the binary does not accept the ABI the vendored header declares. The
//        last case is the interesting one: GetApi states its contract through
//        two different rejections and the negotiation table prints both, so a
//        stale header is named with the exact abi_version and struct_size the
//        binary wants instead of surfacing as "the compiler cannot find it".
//   125  no runtime pack staged beside this checkout, so the probe is skipped
//        rather than failed (the ctest convention for absent hardware assets).

#include "AmdLmxxfPolicy.h"
#include "AmdLmxxfRuntime.h"
#include "CompareImageIO.h"

#include <windows.h>
#include <bcrypt.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

#ifndef DLSS5_AMD_RUNTIME_DIR
#define DLSS5_AMD_RUNTIME_DIR "amd-neural-runtime"
#endif

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitSkipped = 125;

// The plan's stage A gate is 1080p, one pass, so the synthetic frame is one.
constexpr UINT kWidth = 1920;
constexpr UINT kHeight = 1080;
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

// The colour resource is handed to the runtime as a shader input, so the state
// the caller declares and the state the upload pass leaves it in are the same
// pair: both the pixel and the non-pixel shader read bit.
constexpr D3D12_RESOURCE_STATES kColorState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

int failures = 0;

void Line(std::string_view text) { std::cout << text << std::endl; }

void Step(std::string_view title)
{
    std::cout << std::endl << "--- " << title << " ---" << std::endl;
}

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

std::string Narrow(const std::filesystem::path& path) { return Narrow(path.wstring()); }

// The ABI 3 enum has one more member than the upstream v1 one - DEVICE_LOST,
// returned when Collect releases an invalidated job after device removal - so
// the names come from the shared header rather than a local copy that would
// silently print UNKNOWN for it.
const char* StatusName(int32_t status)
{
    return amd_lmxxf::LmxxfNrStatusName(status).data();
}

std::string RuntimeLastError(const LmxxfNrApi& api)
{
    if (api.GetLastError == nullptr) return {};
    char buffer[1024]{};
    if (api.GetLastError(buffer, static_cast<uint32_t>(sizeof(buffer))) !=
        static_cast<int32_t>(LMXXF_NR_OK))
        return {};
    return buffer;
}

// Prints one call's outcome and answers whether the walk may continue past it.
// NOT_IMPLEMENTED is upstream's own TODO - its binary says "(HIP/codec next)" -
// so it stops the walk without being counted as a probe failure.
bool Report(std::string_view call, int32_t status, const std::string& lastError)
{
    std::string text = "  ";
    text += call;
    text += " -> ";
    text += StatusName(status);
    text += " (";
    text += std::to_string(status);
    text += ")";
    if (!lastError.empty()) text += "\n      last_error: " + lastError;
    if (status != static_cast<int32_t>(LMXXF_NR_OK)) {
        if (status == static_cast<int32_t>(LMXXF_NR_NOT_IMPLEMENTED))
            text += "\n      upstream gap: this ABI revision does not implement the call yet";
        else {
            text += "\n      FAILED";
            ++failures;
        }
    }
    Line(text);
    return status == static_cast<int32_t>(LMXXF_NR_OK);
}

bool Sha256File(const std::filesystem::path& path, std::string& hex, std::string& error)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    NTSTATUS status = ::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) {
        error = "BCryptOpenAlgorithmProvider";
        return false;
    }

    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD written = 0;
    ::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                        sizeof(objectLength), &written, 0);
    ::BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength),
                        sizeof(hashLength), &written, 0);

    std::vector<UCHAR> object(objectLength);
    BCRYPT_HASH_HANDLE hash = nullptr;
    status = ::BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0);
    if (status < 0) {
        ::BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "BCryptCreateHash";
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        ::BCryptDestroyHash(hash);
        ::BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "open";
        return false;
    }

    std::vector<char> chunk(1u << 20);
    while (stream) {
        stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize read = stream.gcount();
        if (read <= 0) break;
        status = ::BCryptHashData(hash, reinterpret_cast<PUCHAR>(chunk.data()),
                                  static_cast<ULONG>(read), 0);
        if (status < 0) {
            ::BCryptDestroyHash(hash);
            ::BCryptCloseAlgorithmProvider(algorithm, 0);
            error = "BCryptHashData";
            return false;
        }
    }

    std::vector<UCHAR> digest(hashLength);
    status = ::BCryptFinishHash(hash, digest.data(), hashLength, 0);
    ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        error = "BCryptFinishHash";
        return false;
    }

    static const char* digits = "0123456789abcdef";
    hex.clear();
    hex.reserve(digest.size() * 2);
    for (const UCHAR byte : digest) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0x0f]);
    }
    return true;
}

std::filesystem::path ResolveRuntimeDirectory(int argc, char** argv)
{
    std::vector<std::filesystem::path> candidates;
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') candidates.emplace_back(argv[1]);
    candidates.emplace_back(DLSS5_AMD_RUNTIME_DIR);
    std::error_code cwdError;
    const std::filesystem::path cwd = std::filesystem::current_path(cwdError);
    if (!cwdError) candidates.emplace_back(cwd / "amd-neural-runtime");

    for (const std::filesystem::path& candidate : candidates) {
        std::error_code existsError;
        if (std::filesystem::exists(candidate / "LmxxfNrRuntime.dll", existsError)) return candidate;
    }
    return candidates.front();
}

struct DeviceSet
{
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    std::wstring adapterName;
};

bool CreateDeviceSet(DeviceSet& out, std::string& error)
{
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        error = "CreateDXGIFactory2";
        return false;
    }

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
        if (FAILED(::D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
            continue;
        // Only gfx1200/gfx1201 modules exist, so the AMD adapter is the one to
        // hand the runtime. Anything else is kept as a fallback so a wrong
        // adapter is reported by the runtime's own error rather than here.
        if (!chosenDevice) {
            chosen = adapter;
            chosenDevice = device;
        }
        if (description.VendorId == amd_lmxxf::kAmdVendorId) {
            chosen = adapter;
            chosenDevice = device;
            break;
        }
    }

    if (!chosenDevice) {
        error = "no D3D12 hardware adapter";
        return false;
    }

    out.adapter = chosen;
    out.device = chosenDevice;
    DXGI_ADAPTER_DESC1 chosenDescription{};
    if (SUCCEEDED(chosen->GetDesc1(&chosenDescription))) out.adapterName = chosenDescription.Description;

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(out.device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&out.queue)))) {
        error = "CreateCommandQueue";
        return false;
    }
    return true;
}

struct FrameResources
{
    Microsoft::WRL::ComPtr<ID3D12Resource> color;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent{};
    UINT64 fenceValue{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    std::vector<uint8_t> pattern;
};

bool WaitForGpu(ID3D12CommandQueue* queue, ID3D12Fence* fence, HANDLE fenceEvent, UINT64& fenceValue)
{
    const UINT64 value = ++fenceValue;
    if (FAILED(queue->Signal(fence, value))) return false;
    if (fence->GetCompletedValue() >= value) return true;
    if (FAILED(fence->SetEventOnCompletion(value, fenceEvent))) return false;
    return ::WaitForSingleObject(fenceEvent, 30000) == WAIT_OBJECT_0;
}

bool SubmitAndWait(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, ID3D12Fence* fence,
                   HANDLE fenceEvent, UINT64& fenceValue)
{
    if (FAILED(list->Close())) return false;
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    return WaitForGpu(queue, fence, fenceEvent, fenceValue);
}

// A red/green gradient over a mid blue, so "black" and "unchanged" are both
// measurable from the output alone.
std::vector<uint8_t> MakePattern()
{
    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4);
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            const size_t offset = (static_cast<size_t>(y) * kWidth + x) * 4;
            pixels[offset + 0] = static_cast<uint8_t>(x * 255u / (kWidth - 1u));   // B
            pixels[offset + 1] = static_cast<uint8_t>(y * 255u / (kHeight - 1u));  // G
            pixels[offset + 2] = static_cast<uint8_t>(x * 255u / (kWidth - 1u));   // R
            pixels[offset + 3] = 255;
        }
    }
    return pixels;
}

bool CreateFrameResources(ID3D12Device* device, FrameResources& out, std::string& error)
{
    out.pattern = MakePattern();

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC colorDescription{};
    colorDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    colorDescription.Width = kWidth;
    colorDescription.Height = kHeight;
    colorDescription.DepthOrArraySize = 1;
    colorDescription.MipLevels = 1;
    colorDescription.Format = kColorFormat;
    colorDescription.SampleDesc.Count = 1;
    colorDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    colorDescription.Flags =
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &colorDescription,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&out.color)))) {
        error = "CreateCommittedResource(color)";
        return false;
    }

    UINT rowCount = 0;
    UINT64 rowBytes = 0;
    UINT64 uploadBytes = 0;
    device->GetCopyableFootprints(&colorDescription, 0, 1, 0, &out.footprint, &rowCount, &rowBytes,
                                  &uploadBytes);
    if (rowBytes < static_cast<UINT64>(kWidth) * 4u || rowCount < kHeight) {
        error = "GetCopyableFootprints reports a footprint too small for the frame";
        return false;
    }

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
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&out.upload)))) {
        error = "CreateCommittedResource(upload)";
        return false;
    }

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
                                               &bufferDescription, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&out.readback)))) {
        error = "CreateCommittedResource(readback)";
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(out.upload->Map(0, &noRead, &mapped))) {
        error = "upload Map";
        return false;
    }
    for (UINT row = 0; row < kHeight; ++row) {
        std::memcpy(static_cast<uint8_t*>(mapped) + out.footprint.Offset +
                        static_cast<size_t>(row) * out.footprint.Footprint.RowPitch,
                    out.pattern.data() + static_cast<size_t>(row) * kWidth * 4,
                    static_cast<size_t>(kWidth) * 4);
    }
    out.upload->Unmap(0, nullptr);

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&out.allocator)))) {
        error = "CreateCommandAllocator";
        return false;
    }
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, out.allocator.Get(),
                                         nullptr, IID_PPV_ARGS(&out.list)))) {
        error = "CreateCommandList";
        return false;
    }
    if (FAILED(out.list->Close())) {
        error = "initial command list Close";
        return false;
    }
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&out.fence)))) {
        error = "CreateFence";
        return false;
    }
    out.fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (out.fenceEvent == nullptr) {
        error = "CreateEvent";
        return false;
    }
    return true;
}

bool UploadPattern(ID3D12CommandQueue* queue, FrameResources& resources, std::string& error)
{
    if (FAILED(resources.allocator->Reset()) ||
        FAILED(resources.list->Reset(resources.allocator.Get(), nullptr))) {
        error = "command list Reset";
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = resources.color.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = resources.upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = resources.footprint;
    resources.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resources.color.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = kColorState;
    resources.list->ResourceBarrier(1, &barrier);

    if (!SubmitAndWait(queue, resources.list.Get(), resources.fence.Get(), resources.fenceEvent,
                       resources.fenceValue)) {
        error = "upload submit or fence wait";
        return false;
    }
    return true;
}

// Copies the colour resource into the readback buffer and answers with the
// numbers the plan's gate is about.
bool ReadBackColor(ID3D12CommandQueue* queue, FrameResources& resources, std::vector<uint8_t>& bgra,
                   std::string& error)
{
    if (FAILED(resources.allocator->Reset()) ||
        FAILED(resources.list->Reset(resources.allocator.Get(), nullptr))) {
        error = "readback command list Reset";
        return false;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resources.color.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = kColorState;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    resources.list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = resources.readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = resources.footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = resources.color.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    resources.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    if (!SubmitAndWait(queue, resources.list.Get(), resources.fence.Get(), resources.fenceEvent,
                       resources.fenceValue)) {
        error = "readback submit or fence wait";
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(resources.footprint.Footprint.Height) *
                                        resources.footprint.Footprint.RowPitch};
    if (FAILED(resources.readback->Map(0, &readRange, &mapped))) {
        error = "readback Map";
        return false;
    }
    bgra.assign(static_cast<size_t>(kWidth) * kHeight * 4, 0);
    for (UINT row = 0; row < kHeight; ++row) {
        std::memcpy(bgra.data() + static_cast<size_t>(row) * kWidth * 4,
                    static_cast<const uint8_t*>(mapped) + resources.footprint.Offset +
                        static_cast<size_t>(row) * resources.footprint.Footprint.RowPitch,
                    static_cast<size_t>(kWidth) * 4);
    }
    const D3D12_RANGE noWrite{0, 0};
    resources.readback->Unmap(0, &noWrite);
    return true;
}

// Copies a texture the runtime owns into the existing readback buffer. The
// runtime's private output is B8G8R8X8_UNORM: the same four bytes per pixel and
// the same 7680-byte row pitch as the host colour texture, so the footprint the
// colour upload computed is reusable with only the format swapped. The host
// declared no state for a resource it did not create, so the transition starts
// from COMMON, which D3D12 treats as a promotion rather than a mismatch.
bool ReadBackRuntimeTexture(ID3D12CommandQueue* queue, FrameResources& resources,
                            ID3D12Resource* texture, std::vector<uint8_t>& bgra,
                            std::string& error)
{
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        description.Width != kWidth || description.Height != kHeight) {
        error = "runtime output is not the frame the probe asked for";
        return false;
    }
    if (description.Format != DXGI_FORMAT_B8G8R8X8_UNORM &&
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        error = "runtime output format " + std::to_string(static_cast<int>(description.Format)) +
                " is not a 32-bit BGRA layout the probe can read";
        return false;
    }
    if (FAILED(resources.allocator->Reset()) ||
        FAILED(resources.list->Reset(resources.allocator.Get(), nullptr))) {
        error = "runtime readback command list Reset";
        return false;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    resources.list->ResourceBarrier(1, &barrier);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = resources.footprint;
    footprint.Footprint.Format = description.Format;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = resources.readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    resources.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    if (!SubmitAndWait(queue, resources.list.Get(), resources.fence.Get(), resources.fenceEvent,
                       resources.fenceValue)) {
        error = "runtime readback submit or fence wait";
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(footprint.Footprint.Height) *
                                       footprint.Footprint.RowPitch};
    if (FAILED(resources.readback->Map(0, &readRange, &mapped))) {
        error = "runtime readback Map";
        return false;
    }
    bgra.assign(static_cast<size_t>(kWidth) * kHeight * 4, 0);
    for (UINT row = 0; row < kHeight; ++row) {
        std::memcpy(bgra.data() + static_cast<size_t>(row) * kWidth * 4,
                    static_cast<const uint8_t*>(mapped) + footprint.Offset +
                        static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                    static_cast<size_t>(kWidth) * 4);
    }
    const D3D12_RANGE noWrite{0, 0};
    resources.readback->Unmap(0, &noWrite);
    return true;
}

double MeanLuminance(const std::vector<uint8_t>& bgra)
{
    if (bgra.empty()) return 0.0;
    double total = 0.0;
    const size_t pixels = bgra.size() / 4;
    for (size_t index = 0; index < pixels; ++index) {
        const uint8_t* pixel = bgra.data() + index * 4;
        total += 0.114 * pixel[0] + 0.587 * pixel[1] + 0.299 * pixel[2];
    }
    return total / static_cast<double>(pixels);
}

// The runtime's private output is B8G8R8X8_UNORM: the fourth byte is undefined,
// so comparing it would let any output pass the "is it still the input" test on
// the alpha channel alone. Colour is the only channel that carries the result.
double MeanAbsoluteDifferenceRgb(const std::vector<uint8_t>& left,
                                 const std::vector<uint8_t>& right)
{
    if (left.size() != right.size() || left.empty()) return 0.0;
    double total = 0.0;
    size_t compared = 0;
    for (size_t pixel = 0; pixel + 3 < left.size(); pixel += 4) {
        for (size_t channel = 0; channel < 3; ++channel) {
            total += std::abs(static_cast<int>(left[pixel + channel]) -
                              static_cast<int>(right[pixel + channel]));
            ++compared;
        }
    }
    return compared == 0 ? 0.0 : total / static_cast<double>(compared);
}

} // namespace

int main(int argc, char** argv)
{
    ::SetConsoleOutputCP(CP_UTF8);
    const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Line("AmdLmxxfRuntimeProbe - stage A: real LmxxfNrRuntime.dll, real D3D12 device.");
    Line(std::string("  process: ") + (sizeof(void*) == 8 ? "x64" : "x86") + ", C++20");

    const std::filesystem::path runtimeDirectory = ResolveRuntimeDirectory(argc, argv);
    Line("  runtime dir: " + Narrow(runtimeDirectory));

    std::error_code existsError;
    if (!std::filesystem::exists(runtimeDirectory / "LmxxfNrRuntime.dll", existsError)) {
        Line("");
        Line("SKIPPED: no amd-neural-runtime/LmxxfNrRuntime.dll beside this checkout.");
        Line("Stage the AMD neural runtime pack there (docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md),");
        Line("or pass the directory holding it as the first argument.");
        if (SUCCEEDED(comResult)) ::CoUninitialize();
        return kExitSkipped;
    }

    // Steps 1-4: the runtime module, its ABI table and the exports it offers.
    // The module is loaded here rather than through src/AmdLmxxfRuntime.h so the
    // contract can be interrogated even when the wrapper's pinned ABI no longer
    // matches the binary - which is exactly the failure this probe exists to
    // name. src/AmdLmxxfRuntime.h is exercised again further down.
    Step("step 1-4  Load, LmxxfNrGetApi, ABI table");
    const std::filesystem::path modulePath = runtimeDirectory / "LmxxfNrRuntime.dll";
    HMODULE module = ::LoadLibraryExW(modulePath.c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                          LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) {
        Line("  LoadLibraryExW -> FAILED, GetLastError=" + std::to_string(::GetLastError()));
        ++failures;
        Line("");
        Line("RESULT: FAIL " + std::to_string(failures) + " step(s)");
        if (SUCCEEDED(comResult)) ::CoUninitialize();
        return kExitFailed;
    }
    Line("  module: " + Narrow(modulePath));
    std::string digest;
    std::string digestError;
    if (Sha256File(modulePath, digest, digestError)) Line("  sha256: " + digest);
    else Line("  sha256: unavailable (" + digestError + ")");

    const char* namedExports[] = {"LmxxfNrGetApi", "LmxxfNrGetTimingApi",
                                  "LmxxfNrResolveArchModules"};
    Line("  exports:");
    for (const char* name : namedExports) {
        const bool present = ::GetProcAddress(module, name) != nullptr;
        Line(std::string("    ") + name + ": " + (present ? "present" : "absent"));
    }

    // The ABI the binary will actually talk. GetApi answers two ways about this
    // and both are informative: LMXXF_NR_UNSUPPORTED_ABI means the version is
    // wrong, LMXXF_NR_INVALID_ARGUMENT means the version is right and the table
    // size is not. Sweeping both axes pins the contract down without guessing,
    // and a rejected call writes nothing, so the buffer only needs to be big
    // enough for the largest table any version could hand back.
    Step("ABI negotiation  GetApi(abi_version, struct_size)");
    const auto getApi = reinterpret_cast<int32_t (*)(uint32_t, void*)>(
        ::GetProcAddress(module, "LmxxfNrGetApi"));
    if (getApi == nullptr) {
        Line("  LmxxfNrGetApi absent: there is no version of this ABI to negotiate");
        ++failures;
        Line("");
        Line("RESULT: FAIL " + std::to_string(failures) + " step(s)");
        if (SUCCEEDED(comResult)) ::CoUninitialize();
        return kExitFailed;
    }
    const uint32_t probeVersions[] = {1u, 2u, 3u, 4u, 5u, 6u};
    // V1 (upstream a9b7a521), V2 (V1 + GetTimings), V3 (V2 + the four recording
    // functions, and the size this header compiles to), plus 160 as a control.
    // V4..V6 are swept because the 2026-10 pack raised the integer without
    // changing the table: the host negotiates 6, then 3 (AmdLmxxfApi.h).
    const uint32_t probeSizes[] = {LMXXF_NR_API_V1_SIZE, LMXXF_NR_API_V2_SIZE, 160u,
                                   LMXXF_NR_API_V3_SIZE};
    uint32_t negotiatedVersion = 0;
    uint32_t negotiatedSize = 0;
    for (const uint32_t version : probeVersions) {
        std::string row = "  abi " + std::to_string(version) + ":";
        for (const uint32_t size : probeSizes) {
            std::vector<uint8_t> table(512, 0);
            std::memcpy(table.data(), &size, sizeof(size));
            const int32_t status = getApi(version, table.data());
            row += "  " + std::to_string(size) + "->";
            if (status == static_cast<int32_t>(LMXXF_NR_OK)) {
                row += "OK";
                if (negotiatedVersion == 0) {
                    negotiatedVersion = version;
                    negotiatedSize = size;
                }
            } else {
                row += std::to_string(status);
            }
        }
        Line(row);
    }
    Line("  table sizes tried: " + std::to_string(probeSizes[0]) + " (V1), " +
         std::to_string(probeSizes[1]) + " (V2), " + std::to_string(probeSizes[2]) + ", " +
         std::to_string(probeSizes[3]) + " (V3 = this header, 0xB0)");
    Line("  0=OK 1=UNSUPPORTED_ABI 2=INVALID_ARGUMENT 3=NOT_IMPLEMENTED 4=UNAVAILABLE 5=FAILED");

    if (negotiatedVersion == 0) {
        Line("");
        Line("STALE HEADER: no abi_version/struct_size pair this binary accepts was found.");
        ++failures;
        Line("");
        Line("RESULT: FAIL " + std::to_string(failures) + " step(s)");
        if (SUCCEEDED(comResult)) ::CoUninitialize();
        return kExitFailed;
    }
    if (!amd_lmxxf::IsAcceptedAbiVersion(negotiatedVersion)) {
        Line("");
        Line("STALE HEADER: this binary speaks abi_version " + std::to_string(negotiatedVersion) +
             " with a " + std::to_string(negotiatedSize) + "-byte table;");
        Line("third_party/lmxxf/LmxxfNrApi.h declares abi_version " +
             std::to_string(LMXXF_NR_ABI_VERSION) + " and a " +
             std::to_string(static_cast<uint32_t>(sizeof(LmxxfNrApi))) + "-byte one.");
        Line("The table layout beyond the pinned revision is unknown, so this probe stops here");
        Line("rather than calling through a struct it cannot vouch for.");
        ++failures;
        Line("");
        Line("RESULT: FAIL " + std::to_string(failures) + " step(s)");
        if (SUCCEEDED(comResult)) ::CoUninitialize();
        return kExitFailed;
    }
    Line("  negotiated: abi_version " + std::to_string(negotiatedVersion) + ", struct_size " +
         std::to_string(negotiatedSize) +
         (negotiatedVersion == LMXXF_NR_ABI_VERSION
              ? " - the vendored header is current"
              : " - served by the same table layout, host negotiates it in AmdLmxxfApi.h"));

    // Step 1-2 again, through the wrapper the player will actually use.
    Step("step 1-2  src/AmdLmxxfRuntime.h Load");
    std::wstring loadError;
    amd_lmxxf::Runtime runtime;
    if (!runtime.Load(runtimeDirectory, &loadError)) {
        Line("  Runtime::Load -> FAILED: " + Narrow(loadError));
        ++failures;
        Line("");
        Line("RESULT: FAIL " + std::to_string(failures) + " step(s)");
        if (SUCCEEDED(comResult)) ::CoUninitialize();
        return kExitFailed;
    }
    Line("  Runtime::Load -> OK");
    Line("  api struct_size: " + std::to_string(runtime.Api().struct_size) + " (V1 is " +
         std::to_string(LMXXF_NR_API_V1_SIZE) + ", this header is " +
         std::to_string(static_cast<uint32_t>(sizeof(LmxxfNrApi))) + ")");
    Line("  api abi_version: " + std::to_string(runtime.Api().abi_version));
    Line(std::string("  GetTimings present: ") + (runtime.TimingAvailable() ? "yes" : "no") +
         " (optional: appended after the first ABI)");
    Line(std::string("  EnqueueHip and ExecuteAfterProducer are ") +
         (runtime.Api().EnqueueHip == runtime.Api().ExecuteAfterProducer ? "the same entry point"
                                                                        : "separate entry points") +
         " (upstream's call-order note calls them one step)");
    {
        // The ABI 3 table is 8 bytes of header then 21 function pointers. A
        // runtime that hands back OK but leaves holes would be advertising a
        // revision it does not implement, so the count is worth printing before
        // anything is called through it.
        constexpr size_t kFunctionCount = (sizeof(LmxxfNrApi) - 8u) / sizeof(void*);
        const auto* slots = reinterpret_cast<void* const*>(&runtime.Api());
        size_t populated = 0;
        for (size_t index = 0; index < kFunctionCount; ++index)
            if (slots[index + 1] != nullptr) ++populated;
        Line("  table populated: " + std::to_string(populated) + "/" +
             std::to_string(kFunctionCount) + " function pointers");
    }

    // Step 5: capabilities.
    Step("step 5  QueryCapabilities");
    std::wstring capabilityError;
    if (!runtime.QueryCapabilities(&capabilityError)) {
        Line("  QueryCapabilities -> FAILED: " + Narrow(capabilityError));
        ++failures;
        Line("");
        Line("RESULT: FAIL " + std::to_string(failures) + " step(s)");
        if (SUCCEEDED(comResult)) ::CoUninitialize();
        return kExitFailed;
    }
    {
        const LmxxfNrCapabilities& capabilities = runtime.Capabilities();
        Line("  max_input        : " + std::to_string(capabilities.max_input_width) + "x" +
             std::to_string(capabilities.max_input_height));
        Line("  history_supported: " + std::to_string(capabilities.history_supported));
        Line("  overlap_supported: " + std::to_string(capabilities.overlap_supported));
        Line("  graph_supported  : " + std::to_string(capabilities.graph_supported));
        Line("  gfx1201_target   : " + std::to_string(capabilities.gfx1201_target) +
             "  (deprecated in ABI 3: query GetStatus for the active arch)");
        Line("  capabilities size: " + std::to_string(capabilities.struct_size) + " (" +
             std::to_string(static_cast<uint32_t>(sizeof(LmxxfNrCapabilities))) +
             " in this header; ABI 1 was 36 with an extra hip_ready field)");
    }

    // Step 7: a real D3D12 device and queue, which Create needs.
    Step("step 7  D3D12 device and queue");
    DeviceSet devices;
    std::string deviceError;
    if (!CreateDeviceSet(devices, deviceError)) {
        Line("  device set -> FAILED: " + deviceError);
        ++failures;
        Line("");
        Line("RESULT: FAIL " + std::to_string(failures) + " step(s)");
        if (SUCCEEDED(comResult)) ::CoUninitialize();
        return kExitFailed;
    }
    Line("  adapter: " + Narrow(devices.adapterName));

    // Step 6: Create and PrepareSession. The runtime takes its modules directory
    // as the assets directory; the tiled assets holding weights and shaders are
    // a second directory it reads through the environment, per its own error
    // text: "set LMXXF_WEIGHTS_DIR to tiled assets, not 0.24.2 HIP/".
    Step("step 6  Create and PrepareSession");
    const std::filesystem::path tiledAssets = runtimeDirectory / "native-game-tiled-assets";
    if (std::filesystem::exists(tiledAssets, existsError)) {
        ::SetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", tiledAssets.wstring().c_str());
        Line("  LMXXF_WEIGHTS_DIR = " + Narrow(tiledAssets));
    } else {
        Line("  LMXXF_WEIGHTS_DIR not set: " + Narrow(tiledAssets) + " is absent");
    }

    std::vector<std::filesystem::path> moduleCandidates;
    moduleCandidates.push_back(runtimeDirectory / std::wstring(amd_lmxxf::kAssetsDirectoryName));
    moduleCandidates.push_back(runtimeDirectory / "lmxxf-modules");
    moduleCandidates.push_back(runtimeDirectory);
    moduleCandidates.push_back(runtimeDirectory / "native-game-tiled-assets" / "HIP");

    std::filesystem::path moduleDirectory;
    for (const std::filesystem::path& candidate : moduleCandidates) {
        std::error_code directoryError;
        if (!std::filesystem::is_directory(candidate, directoryError)) {
            Line("  candidate " + Narrow(candidate) + " -> not a directory, skipped");
            continue;
        }
        std::wstring createError;
        if (runtime.CreateSession(devices.device.Get(), devices.queue.Get(), candidate, 0,
                                  &createError)) {
            moduleDirectory = candidate;
            Line("  modules directory: " + Narrow(candidate) + " -> Create OK");
            break;
        }
        std::string detail = Narrow(createError);
        const std::string lastError = runtime.LastError();
        if (!lastError.empty()) detail += " | " + lastError;
        Line("  candidate " + Narrow(candidate) + " -> Create refused: " + detail);
    }

    if (moduleDirectory.empty()) {
        Line("  Create -> FAILED for every candidate modules directory");
        ++failures;
        Line("");
        Line("RESULT: FAIL " + std::to_string(failures) + " step(s)");
        if (SUCCEEDED(comResult)) ::CoUninitialize();
        return kExitFailed;
    }

    std::wstring prepareError;
    if (!runtime.PrepareSession(&prepareError)) {
        const std::string lastError = runtime.LastError();
        Line("  PrepareSession -> FAILED: " + Narrow(prepareError) +
             (lastError.empty() ? "" : " | " + lastError));
        ++failures;
        Line("");
        Line("RESULT: FAIL " + std::to_string(failures) + " step(s)");
        if (SUCCEEDED(comResult)) ::CoUninitialize();
        return kExitFailed;
    }
    Line("  PrepareSession -> OK");
    {
        const std::string status = runtime.Status();
        if (!status.empty()) Line("  status: " + status);
        const std::string lastError = runtime.LastError();
        if (!lastError.empty()) Line("  last_error: " + lastError);
    }

    // ABI 3 dropped hip_ready: what used to be a capabilities flag is now the
    // GetStatus line, which is also where the active arch is reported.
    if (runtime.QueryCapabilities(&capabilityError))
        Line("  capabilities re-read after Create -> OK (size " +
             std::to_string(runtime.Capabilities().struct_size) + ")");

    // Step 8: the synthetic frame the gate is measured on.
    Step("step 8  Upload one 1920x1080 BGRA frame");
    FrameResources frames;
    std::string resourceError;
    if (!CreateFrameResources(devices.device.Get(), frames, resourceError)) {
        Line("  resources -> FAILED: " + resourceError);
        ++failures;
        Line("");
        Line("RESULT: FAIL " + std::to_string(failures) + " step(s)");
        if (SUCCEEDED(comResult)) ::CoUninitialize();
        return kExitFailed;
    }
    if (!UploadPattern(devices.queue.Get(), frames, resourceError)) {
        Line("  upload -> FAILED: " + resourceError);
        ++failures;
        Line("");
        Line("RESULT: FAIL " + std::to_string(failures) + " step(s)");
        if (SUCCEEDED(comResult)) ::CoUninitialize();
        return kExitFailed;
    }
    Line("  uploaded " + std::to_string(kWidth) + "x" + std::to_string(kHeight) + ", " +
         std::to_string(frames.pattern.size()) + " bytes, red/green gradient (never flat)");
    Line("  input mean luminance: " + std::to_string(MeanLuminance(frames.pattern)));

    // Steps 9-13: the documented order, one call at a time, every status kept.
    Step("step 9-13  PrepareFrame, RecordInputs, producer, EnqueueHip, RecordOutputs, Retire");
    const LmxxfNrApi& api = runtime.Api();
    void* context = runtime.Context();
    LmxxfNrJob job = amd_lmxxf::MakeJob();
    amd_lmxxf::StampJobForAbi(job, api.abi_version);
    bool proceed = true;

    if (FAILED(frames.allocator->Reset()) ||
        FAILED(frames.list->Reset(frames.allocator.Get(), nullptr))) {
        Line("  command list Reset -> FAILED");
        ++failures;
        proceed = false;
    }

    if (proceed) {
        LmxxfNrFrameInfo frameInfo =
            amd_lmxxf::MakeFrameInfo(frames.color.Get(), kWidth, kHeight,
                                     static_cast<uint32_t>(kColorState), frames.list.Get(), 1, 1, 1,
                                     1.0f, 1.0f, 1.0f);
        amd_lmxxf::StampFrameInfoForAbi(frameInfo, api.abi_version);
        proceed = Report("PrepareFrame", api.PrepareFrame(context, &frameInfo, &job),
                         RuntimeLastError(api));
        if (proceed)
            Line("      job.handle=" + std::to_string(reinterpret_cast<uintptr_t>(job.handle)));
    }

    // ABI 3 hands the caller a handle in job.handle and resolves every later
    // call through Session::FindJob(handle), which accepts only nullptr or the
    // session's own job address - never the caller's struct address. So the
    // handle travels, and &job stops being the job reference after this call.
    if (proceed) {
        proceed = Report("RecordInputs", api.RecordInputs(context, job.handle, frames.list.Get()),
                         RuntimeLastError(api));
    }

    // The producer list is submitted whether or not the runtime recorded
    // anything into it: an empty list is a legal submission, and the job must
    // not be left recording. Upstream's call-order note is explicit that
    // EnqueueHip is the very next thing on the same queue - no fence wait and no
    // Flush in between - and that EnqueueHip IS the ExecuteAfterProducer step,
    // so exactly one of the two entries is called. Which one is reported above.
    if (proceed) {
        if (FAILED(frames.list->Close())) {
            Line("  producer Close -> FAILED");
            ++failures;
            proceed = false;
        } else {
            ID3D12CommandList* lists[] = {frames.list.Get()};
            devices.queue->ExecuteCommandLists(1, lists);
            proceed = Report("EnqueueHip", api.EnqueueHip(context, job.handle, devices.queue.Get()),
                             RuntimeLastError(api));
            if (!WaitForGpu(devices.queue.Get(), frames.fence.Get(), frames.fenceEvent,
                            frames.fenceValue))
                Line("  producer fence wait -> FAILED");
        }
    }

    if (proceed) {
        uint32_t state = 0;
        Report("Poll", api.Poll(context, job.handle, &state), RuntimeLastError(api));
        Line("      job state: " + std::to_string(state));
        if (FAILED(frames.list->Reset(frames.allocator.Get(), nullptr))) {
            Line("  consumer command list Reset -> FAILED");
            ++failures;
            proceed = false;
        } else {
            proceed = Report("RecordOutputs",
                             api.RecordOutputs(context, job.handle, frames.list.Get()),
                             RuntimeLastError(api));
            if (!proceed && FAILED(frames.list->Close())) Line("  consumer Close -> FAILED");
        }
    }

    if (proceed) {
        if (!SubmitAndWait(devices.queue.Get(), frames.list.Get(), frames.fence.Get(),
                           frames.fenceEvent, frames.fenceValue))
            Line("  consumer submit or fence wait -> FAILED");
        Report("Retire", api.Retire(context, job.handle), RuntimeLastError(api));
        Line("      job.private_output=" +
             std::to_string(reinterpret_cast<uintptr_t>(job.private_output)));
    } else {
        // Leave the runtime with no recording job rather than a dangling one.
        Report("CancelUnsubmitted", api.CancelUnsubmitted(context, job.handle),
               RuntimeLastError(api));
        if (FAILED(frames.list->Close())) Line("  command list Close -> FAILED");
    }

    {
        const std::string status = runtime.Status();
        if (!status.empty()) Line("  status: " + status);
    }

    // Step 14: the gate. The ABI calls job.private_output the SR result and the
    // staged runtime means it: it hands the produced frame back in its own
    // texture and leaves the host colour texture holding the input, because that
    // texture is the codec's input. So the produced frame is what gets judged,
    // and the host colour texture is read back only as evidence it was left
    // alone - judging it would measure the probe's own upload.
    Step("step 14  Read back and judge the frame");
    ID3D12Resource* resultTexture = frames.color.Get();
    bool resultIsPrivate = false;
    if (job.private_output == nullptr) {
        Line("  private_output is null: the runtime published no result texture");
        ++failures;
    } else if (job.private_output != static_cast<void*>(frames.color.Get())) {
        resultTexture = static_cast<ID3D12Resource*>(job.private_output);
        resultIsPrivate = true;
        const D3D12_RESOURCE_DESC privateDesc = resultTexture->GetDesc();
        Line("  private_output is a separate resource: " + std::to_string(privateDesc.Width) + "x" +
             std::to_string(privateDesc.Height) + ", format " +
             std::to_string(static_cast<int>(privateDesc.Format)) + ", uav " +
             ((privateDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 ? "yes" : "no"));
    } else {
        Line("  private_output is the host colour resource: the runtime wrote through it");
    }

    std::vector<uint8_t> output;
    const bool readBack = resultIsPrivate
                              ? ReadBackRuntimeTexture(devices.queue.Get(), frames, resultTexture,
                                                       output, resourceError)
                              : ReadBackColor(devices.queue.Get(), frames, output, resourceError);
    if (!readBack) {
        Line("  readback -> FAILED: " + resourceError);
        ++failures;
    } else {
        // RGB only: the private output's fourth byte is undefined, so including
        // it would let any frame pass the "still the input" test on alpha alone.
        const double luminance = MeanLuminance(output);
        const double difference = MeanAbsoluteDifferenceRgb(output, frames.pattern);
        Line("  produced mean luminance : " + std::to_string(luminance));
        Line("  mean |produced - input| (RGB) : " + std::to_string(difference));
        if (luminance <= 1.0) {
            Line("  GATE NOT MET: the produced frame is black");
            ++failures;
        } else if (difference <= 1.0) {
            Line("  GATE NOT MET: the produced frame still is the input frame");
            ++failures;
        } else {
            Line("  gate: the produced frame is neither black nor the input frame");
        }

        std::filesystem::path outputPath;
        if (argc > 2 && argv[2] != nullptr && argv[2][0] != '\0') {
            outputPath = argv[2];
        } else {
            std::error_code cwdError;
            outputPath = std::filesystem::current_path(cwdError) / "amd-lmxxf-probe-output.png";
        }
        std::vector<uint8_t> bgr(static_cast<size_t>(kWidth) * kHeight * 3);
        for (size_t index = 0; index < static_cast<size_t>(kWidth) * kHeight; ++index) {
            bgr[index * 3 + 0] = output[index * 4 + 0];
            bgr[index * 3 + 1] = output[index * 4 + 1];
            bgr[index * 3 + 2] = output[index * 4 + 2];
        }
        const HRESULT hr = compare_image::SavePngBgr(outputPath, bgr.data(), kWidth, kHeight,
                                                     kWidth * 3u);
        if (SUCCEEDED(hr)) Line("  wrote " + Narrow(outputPath));
        else Line("  SavePngBgr failed with hresult " + std::to_string(static_cast<long>(hr)));
    }

    // Evidence, not a gate: what did the runtime leave in the texture it was
    // handed? A frame equal to the input here is the expected shape of this ABI.
    if (resultIsPrivate) {
        std::vector<uint8_t> hostColor;
        if (ReadBackColor(devices.queue.Get(), frames, hostColor, resourceError)) {
            Line("  host colour mean |colour - input| (RGB) : " +
                 std::to_string(MeanAbsoluteDifferenceRgb(hostColor, frames.pattern)));
        } else {
            Line("  host colour readback -> FAILED: " + resourceError);
        }
    }

    if (frames.fenceEvent != nullptr) ::CloseHandle(frames.fenceEvent);
    runtime.Unload();
    ::FreeLibrary(module);
    if (SUCCEEDED(comResult)) ::CoUninitialize();

    std::cout << std::endl;
    if (failures == 0) Line("RESULT: ok (see the step list above for any upstream gap)");
    else Line("RESULT: FAIL " + std::to_string(failures) + " step(s)");
    return failures == 0 ? kExitOk : kExitFailed;
}
