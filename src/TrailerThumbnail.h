#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "TrailerThumbnailPolicy.h"

// The start screen's trailer thumbnails: fetched from i.ytimg.com on a thread
// of their own, cached as the JPEG YouTube served under
// <cache root>/thumbs/<id>.jpg, and decoded with WIC to 32-bit BGR rows at the
// tile's width. The screen asks for a picture on every paint and draws its
// clean placeholder until one is there, so nothing here can hold it up.

struct TrailerPicture {
    SIZE size{};
    std::vector<uint8_t> bgra;   // top-down rows, size.cx * 4 bytes each
};

// The JPEG cropped to 16:9 (trailer_thumbnail::CoverCrop) and scaled to
// width x width*9/16. Needs COM on the calling thread.
std::shared_ptr<const TrailerPicture> DecodeTrailerThumbnail(std::string_view jpeg, int width);

struct TrailerThumbnailFetch {
    bool ok{};
    std::string jpeg;
    std::wstring error;   // empty when cancelled
};

// maxresdefault.jpg, then hqdefault.jpg if that one is missing, through the
// shared HttpsGet with redirects off and the allowlist checked first.
TrailerThumbnailFetch FetchTrailerThumbnail(std::wstring_view id, std::stop_token stop);

class TrailerThumbnails {
public:
    using Fetcher = std::function<TrailerThumbnailFetch(std::wstring_view id, std::stop_token stop)>;
    struct Settings {
        std::filesystem::path cacheRoot;   // the neural cache root; pictures go in thumbs/
        bool fetchEnabled{true};           // [Start] ThumbnailFetch
        int width{};                       // the tile's thumbnail width in pixels
        Fetcher fetch;                     // FetchTrailerThumbnail unless a test says otherwise
    };

    TrailerThumbnails() = default;
    ~TrailerThumbnails() { Stop(); }
    TrailerThumbnails(const TrailerThumbnails&) = delete;
    TrailerThumbnails& operator=(const TrailerThumbnails&) = delete;

    // Once per process: a second call does nothing, so the screen can ask on
    // every sync. Returns at once; each picture that becomes available is
    // announced with `message` posted to `window`.
    void Start(std::vector<std::wstring> ids, Settings settings, HWND window, UINT message);
    bool Started() const { return m_started; }
    // The worker has run to its end: every picture it could find is published.
    bool Finished() const { return m_finished.load(); }
    void Stop();

    std::shared_ptr<const TrailerPicture> Picture(std::wstring_view id) const;
    void Publish(const std::wstring& id, std::shared_ptr<const TrailerPicture> picture);

    static std::filesystem::path CachePath(const std::filesystem::path& cacheRoot, std::wstring_view id);

private:
    void Run(std::vector<std::wstring> ids, Settings settings, HWND window, UINT message, std::stop_token stop);

    mutable std::mutex m_mutex;
    std::map<std::wstring, std::shared_ptr<const TrailerPicture>, std::less<>> m_pictures;
    bool m_started{};
    std::atomic<bool> m_finished{};
    std::jthread m_thread;
};
