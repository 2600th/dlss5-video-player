#include "TrailerThumbnail.h"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>

#include "AtomicFile.h"
#include "HexText.h"
#include "Log.h"
#include "UpdateCheck.h"
#include "Utf8Text.h"

// WIC's CLSIDs and pixel-format GUIDs live here; linked from the file that
// uses them, as CompareImageIO.h does.
#pragma comment(lib, "windowscodecs.lib")

namespace {

using Microsoft::WRL::ComPtr;

// A thumbnail is 1280x720 at most; anything far larger is not one.
constexpr UINT kMaximumDimension = 4096;

int64_t UnixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<int64_t> WrittenUnix(const std::filesystem::path& file)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) return std::nullopt;
    const auto written = std::filesystem::last_write_time(file, error);
    if (error) return std::nullopt;
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(written);
    return std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
}

std::optional<std::string> ReadCapped(const std::filesystem::path& file)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size == 0 || size > trailer_thumbnail::kMaximumBytes) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() != size) return std::nullopt;
    return bytes;
}

std::string Narrow(std::wstring_view text) { return utf8_text::FromWide(text); }

// COM for this thread, released on the way out only if this call started it.
class ComScope {
public:
    ComScope() : initialized_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() { if (initialized_) CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool initialized_;
};

} // namespace

std::shared_ptr<const TrailerPicture> DecodeTrailerThumbnail(std::string_view jpeg, int width)
{
    if (width <= 0 || width > int(kMaximumDimension) || !trailer_thumbnail::LooksLikeJpeg(jpeg)) return nullptr;
    const UINT outWidth = static_cast<UINT>(width), outHeight = std::max(1u, outWidth * 9u / 16u);
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return nullptr;
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(jpeg.data())),
                                            static_cast<DWORD>(jpeg.size()))))
        return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), &GUID_ContainerFormatJpeg,
                                                WICDecodeMetadataCacheOnDemand, &decoder)))
        return nullptr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;
    UINT sourceWidth = 0, sourceHeight = 0;
    if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) || !sourceWidth || !sourceHeight ||
        sourceWidth > kMaximumDimension || sourceHeight > kMaximumDimension)
        return nullptr;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom)))
        return nullptr;
    const auto crop = trailer_thumbnail::CoverCrop(sourceWidth, sourceHeight);
    if (!crop.width || !crop.height) return nullptr;
    ComPtr<IWICBitmapClipper> clipper;
    const WICRect region{static_cast<INT>(crop.x), static_cast<INT>(crop.y), static_cast<INT>(crop.width),
                         static_cast<INT>(crop.height)};
    if (FAILED(factory->CreateBitmapClipper(&clipper)) || FAILED(clipper->Initialize(converter.Get(), &region)))
        return nullptr;
    // Fant, not the GDI stretch at paint time: a 1280-wide JPEG down to a
    // 192-dip tile aliases badly with anything that does not average.
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(clipper.Get(), outWidth, outHeight, WICBitmapInterpolationModeFant)))
        return nullptr;
    auto picture = std::make_shared<TrailerPicture>();
    picture->size = SIZE{static_cast<LONG>(outWidth), static_cast<LONG>(outHeight)};
    picture->bgra.resize(size_t(outWidth) * outHeight * 4u);
    if (FAILED(scaler->CopyPixels(nullptr, outWidth * 4u, static_cast<UINT>(picture->bgra.size()),
                                  picture->bgra.data())))
        return nullptr;
    return picture;
}

TrailerThumbnailFetch FetchTrailerThumbnail(std::wstring_view id, std::stop_token stop)
{
    using trailer_thumbnail::Variant;
    TrailerThumbnailFetch result;
    for (const Variant variant : {Variant::MaxRes, Variant::High}) {
        const std::wstring url = trailer_thumbnail::ThumbnailUrl(id, variant);
        if (!trailer_thumbnail::AllowedThumbnailUrl(url)) {
            result.error = L"the thumbnail URL is not on the allowlist";
            return result;
        }
        const HttpsGetResult response = HttpsGet({trailer_thumbnail::kHost, trailer_thumbnail::RequestPath(id, variant),
                                                  {}, trailer_thumbnail::kMaximumBytes,
                                                  trailer_thumbnail::kTimeoutMilliseconds, false},
                                                 stop);
        switch (response.outcome) {
        case HttpsGetResult::Outcome::Ok:
            if (!trailer_thumbnail::LooksLikeJpeg(response.body)) {
                result.error = L"the reply was not a JPEG";
                return result;
            }
            result.ok = true;
            result.jpeg = response.body;
            return result;
        case HttpsGetResult::Outcome::Cancelled: result.error.clear(); return result;
        case HttpsGetResult::Outcome::TransportFailed: result.error = response.error; return result;
        case HttpsGetResult::Outcome::TooLarge: result.error = L"the reply was over the 2 MB cap"; return result;
        case HttpsGetResult::Outcome::HttpStatus:
            // 404 is how YouTube says a video has no maxres picture; try the
            // one every video has. Anything else is an answer to stop on.
            result.error = L"HTTP status " + std::to_wstring(response.status);
            if (response.status != 404) return result;
            break;
        }
    }
    return result;
}

std::filesystem::path TrailerThumbnails::CachePath(const std::filesystem::path& cacheRoot, std::wstring_view id)
{
    if (cacheRoot.empty() || !trailer_thumbnail::ValidVideoId(id)) return {};
    return cacheRoot / L"thumbs" / (std::wstring(id) + L".jpg");
}

void TrailerThumbnails::Start(std::vector<std::wstring> ids, Settings settings, HWND window, UINT message)
{
    if (m_started) return;
    m_started = true;
    if (!settings.fetch) settings.fetch = FetchTrailerThumbnail;
    try {
        m_thread = std::jthread([this, ids = std::move(ids), settings = std::move(settings), window,
                                 message](std::stop_token stop) mutable {
            Run(std::move(ids), std::move(settings), window, message, stop);
        });
    } catch (const std::system_error&) {
        m_finished.store(true);
        LOG("Trailer thumbnails: the worker could not start; the tiles keep their placeholders.");
    }
}

void TrailerThumbnails::Stop()
{
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_thread.join();
    }
}

std::shared_ptr<const TrailerPicture> TrailerThumbnails::Picture(std::wstring_view id) const
{
    std::scoped_lock lock(m_mutex);
    const auto found = m_pictures.find(id);
    return found == m_pictures.end() ? nullptr : found->second;
}

void TrailerThumbnails::Publish(const std::wstring& id, std::shared_ptr<const TrailerPicture> picture)
{
    std::scoped_lock lock(m_mutex);
    m_pictures[id] = std::move(picture);
}

void TrailerThumbnails::Run(std::vector<std::wstring> ids, Settings settings, HWND window, UINT message,
                            std::stop_token stop)
{
    const ComScope com;
    struct MarkFinished {
        std::atomic<bool>& finished;
        ~MarkFinished() { finished.store(true); }
    } markFinished{m_finished};
    const auto announce = [&] { if (window) PostMessageW(window, message, 0, 0); };
    using trailer_thumbnail::CacheDecision;
    // Two passes: every picture already on disk first, so a warm start fills
    // all its tiles at once, and only then the network, one id at a time.
    std::vector<std::wstring> toFetch;
    const int64_t now = UnixNow();
    for (const auto& id : ids) {
        if (stop.stop_requested()) return;
        const auto path = CachePath(settings.cacheRoot, id);
        if (path.empty()) continue;
        const auto written = WrittenUnix(path);
        const CacheDecision decision = trailer_thumbnail::Decide(settings.fetchEnabled, written, now);
        if (decision == CacheDecision::Fetch || decision == CacheDecision::Refresh) toFetch.push_back(id);
        if (!written) continue;
        const auto bytes = ReadCapped(path);
        if (const auto picture = bytes ? DecodeTrailerThumbnail(*bytes, settings.width) : nullptr) {
            Publish(id, picture);
            announce();
        } else if (decision != CacheDecision::Fetch && decision != CacheDecision::Refresh && settings.fetchEnabled) {
            // A cached file that no longer decodes is fetched again.
            toFetch.push_back(id);
        }
    }
    size_t fetched = 0;
    for (const auto& id : toFetch) {
        if (stop.stop_requested()) return;
        const auto result = settings.fetch(id, stop);
        if (stop.stop_requested()) return;
        if (!result.ok) {
            // Logged once per id, and the tile keeps a stale picture or its
            // placeholder. Offline is the common case, so this is not an error.
            if (!result.error.empty())
                LOG("Trailer thumbnail " << Narrow(id) << " not fetched: " << Narrow(result.error)
                                         << "; the tile keeps what it has.");
            continue;
        }
        const auto picture = DecodeTrailerThumbnail(result.jpeg, settings.width);
        if (!picture) {
            LOG("Trailer thumbnail " << Narrow(id) << " did not decode (" << result.jpeg.size()
                                     << " bytes); not cached.");
            continue;
        }
        const auto path = CachePath(settings.cacheRoot, id);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        const auto written = error ? atomic_file::Outcome{atomic_file::Step::CreateTemporary, DWORD(error.value())}
                                   : atomic_file::Replace(path, result.jpeg);
        if (!written)
            LOG("Trailer thumbnail " << Narrow(id) << " shown but not cached (" << HexText(written.error) << ").");
        Publish(id, picture);
        announce();
        ++fetched;
    }
    if (!toFetch.empty())
        LOG("Trailer thumbnails: fetched " << fetched << " of " << toFetch.size() << " from "
                                           << Narrow(trailer_thumbnail::kHost) << ".");
}
