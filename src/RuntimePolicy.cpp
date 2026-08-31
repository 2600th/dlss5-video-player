#include "RuntimePolicy.h"

#include <cwctype>
#include <string>

#include <dxgi1_6.h>

namespace {

constexpr uint32_t kNvidiaVendorId = 0x10DE;

bool ContainsCaseInsensitive(std::wstring_view text, std::wstring_view needle)
{
    if (needle.empty() || text.size() < needle.size()) {
        return needle.empty();
    }

    for (size_t offset = 0; offset <= text.size() - needle.size(); ++offset) {
        bool matches = true;
        for (size_t index = 0; index < needle.size(); ++index) {
            if (std::towlower(text[offset + index]) != std::towlower(needle[index])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

} // namespace

GpuGeneration ClassifyGpu(uint32_t vendorId, std::wstring_view description)
{
    if (vendorId != kNvidiaVendorId) {
        return GpuGeneration::Unsupported;
    }
    if (ContainsCaseInsensitive(description, L"GeForce RTX 40")) {
        return GpuGeneration::Rtx40Ada;
    }
    if (ContainsCaseInsensitive(description, L"GeForce RTX 50")) {
        return GpuGeneration::Rtx50Blackwell;
    }
    return GpuGeneration::OtherNvidia;
}

bool NeuralAddonDesired(GpuGeneration gpu, bool safeMode)
{
    return !safeMode && (gpu == GpuGeneration::Rtx40Ada || gpu == GpuGeneration::Rtx50Blackwell);
}

DetectedGpu DetectHighPerformanceGpu()
{
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        return {};
    }

    for (UINT index = 0;; ++index) {
        IDXGIAdapter4* adapter = nullptr;
        const HRESULT result = factory->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(result)) {
            break;
        }

        DXGI_ADAPTER_DESC3 adapterDescription{};
        const HRESULT descriptionResult = adapter->GetDesc3(&adapterDescription);
        adapter->Release();
        if (FAILED(descriptionResult) || (adapterDescription.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0) {
            continue;
        }

        factory->Release();
        return {ClassifyGpu(adapterDescription.VendorId, adapterDescription.Description), adapterDescription.Description};
    }

    factory->Release();
    return {};
}
