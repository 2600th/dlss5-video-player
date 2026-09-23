#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "AtomicFile.h"
#include "CompareMaskPolicy.h"

// Still images in and out of the comparison view through WIC, which every Windows
// install carries: a mask image read as grey (P2.6) and a saved comparison written as
// PNG (P2.21). The codecs' CLSIDs and pixel
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

// Writes 24-bit BGR rows (`stride` bytes apart) as a PNG. Encoded in memory and
// published through atomic_file::Replace, so a failed save never leaves half a PNG -
// or anything else - under the name the user chose.
inline HRESULT SavePngBgr(const std::filesystem::path& path, const uint8_t* bgr, UINT width, UINT height, UINT stride)
{
    if (!bgr || !width || !height || width > kMaxDimension || height > kMaxDimension || stride < width * 3u) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    Microsoft::WRL::ComPtr<IStream> memory;
    if (FAILED(hr = CreateStreamOnHGlobal(nullptr, TRUE, &memory))) return hr;
    {
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        if (SUCCEEDED(hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(hr = encoder->Initialize(memory.Get(), WICBitmapEncoderNoCache))) {
            Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
            Microsoft::WRL::ComPtr<IPropertyBag2> properties;
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            if (SUCCEEDED(hr = encoder->CreateNewFrame(&frame, &properties)) &&
                SUCCEEDED(hr = frame->Initialize(properties.Get())) &&
                SUCCEEDED(hr = frame->SetSize(width, height)) &&
                SUCCEEDED(hr = frame->SetPixelFormat(&format))) {
                // The PNG encoder takes 24bppBGR as it is; anything else would need a
                // converter this function does not have.
                if (format != GUID_WICPixelFormat24bppBGR) hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
                else if (SUCCEEDED(hr = frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(bgr))) &&
                         SUCCEEDED(hr = frame->Commit()))
                    hr = encoder->Commit();
            }
        }
    }
    if (FAILED(hr)) return hr;
    HGLOBAL global = nullptr;
    if (FAILED(hr = GetHGlobalFromStream(memory.Get(), &global))) return hr;
    STATSTG stat{};
    if (FAILED(hr = memory->Stat(&stat, STATFLAG_NONAME))) return hr;
    const void* bytes = GlobalLock(global);
    if (!bytes) return HRESULT_FROM_WIN32(GetLastError());
    const auto outcome = atomic_file::Replace(path, std::string_view(static_cast<const char*>(bytes), size_t(stat.cbSize.QuadPart)));
    GlobalUnlock(global);
    return outcome ? S_OK : HRESULT_FROM_WIN32(outcome.error);
}

} // namespace compare_image

namespace compare_provenance {

// What a saved comparison says about itself, one field per fact, all already strings.
struct Facts {
    std::wstring application;  // "DLSS 5 Video Player 0.25.0"
    std::wstring source;       // the title shown in the window
    std::wstring timecode;     // hh:mm:ss:ff
    uint64_t frame = 0;
    std::wstring view;         // "Split 50% - Mix 100% - Zoom 2x"
    std::wstring settings;     // digest of the render's settings, or why there is none
    std::wstring runtime;      // the locked neural runtime's version
    std::wstring saved;        // local date and time
};

// The footer under a saved comparison: three lines, in the order a reader checks
// them - what and where, what was on screen, what made it. The image is evidence, so
// it carries its own provenance rather than depending on a file name.
inline std::vector<std::wstring> FooterLines(const Facts& facts)
{
    const std::wstring dot = L" \u00b7 ";
    return {facts.application + dot + facts.source,
            facts.timecode + dot + L"frame " + std::to_wstring(facts.frame) + dot + facts.view,
            L"Settings " + facts.settings + dot + L"Runtime " + facts.runtime + dot + L"Saved " + facts.saved};
}

// A file name for the save dialog: the title and timecode with every character a
// Windows file name cannot hold replaced.
inline std::wstring SuggestedName(std::wstring_view title, std::wstring_view timecode)
{
    std::wstring name = std::wstring(title.empty() ? std::wstring_view(L"comparison") : title) + L" " +
                        std::wstring(timecode) + L" comparison";
    for (wchar_t& c : name)
        if (c < L' ' || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' || c == L'|' ||
            c == L'?' || c == L'*')
            c = L'-';
    if (name.size() > 120) name.resize(120);
    return name + L".png";
}

} // namespace compare_provenance
