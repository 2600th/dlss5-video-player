#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <vector>

#include "CompareMaskPolicy.h"

// Still images in and out of the comparison view through WIC, which every Windows
// install carries: a mask image read as grey (P2.6). The codecs' CLSIDs and pixel
// format GUIDs live in windowscodecs.lib, linked from here the way CrashDump.h links
// dbghelp, so no target has to list it.
#pragma comment(lib, "windowscodecs.lib")

namespace compare_image {

// Larger than any sensible mask or screenshot, small enough that width*height fits a
// 32-bit stride product with room to spare.
inline constexpr UINT kMaxDimension = 16384;

// Any image WIC decodes, first frame, as 8-bit grey (WIC's own luminance conversion).
// Transparency is not coverage here: the mask is its brightness, which is what a
// painted black-and-white PNG means. The calling thread must have COM initialised.
inline HRESULT LoadGray(const std::filesystem::path& path, compare_mask::Gray& out)
{
    out = {};
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return hr;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(hr = decoder->GetFrame(0, &frame))) return hr;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(hr = factory->CreateFormatConverter(&converter))) return hr;
    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat8bppGray, WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;
    UINT width = 0, height = 0;
    if (FAILED(hr = converter->GetSize(&width, &height))) return hr;
    if (!width || !height || width > kMaxDimension || height > kMaxDimension) return WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
    compare_mask::Gray gray;
    gray.width = width;
    gray.height = height;
    gray.pixels.resize(size_t(width) * height);
    hr = converter->CopyPixels(nullptr, width, UINT(gray.pixels.size()), gray.pixels.data());
    if (FAILED(hr)) return hr;
    out = std::move(gray);
    return S_OK;
}

} // namespace compare_image
