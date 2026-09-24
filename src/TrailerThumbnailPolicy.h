#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// What the start screen's trailer tiles may fetch, from where, and when: the
// pure half of TrailerThumbnail.h. A tile shows the publisher's thumbnail from
// YouTube's own image CDN, fetched at run time rather than shipped, because
// the artwork is the publishers' and the release zip is MIT. So the rules are
// narrow on purpose: one HTTPS host, two fixed file names, the curated list's
// ids only, a size cap, and a cache that is refreshed monthly rather than on
// every launch.
namespace trailer_thumbnail {

inline constexpr std::wstring_view kHost = L"i.ytimg.com";
// maxresdefault.jpg runs 80-220 KB for the curated trailers (measured
// 2026-09-24); 2 MB is an order of magnitude over that and still refuses
// anything that is not a thumbnail.
inline constexpr size_t kMaximumBytes = 2u * 1024u * 1024u;
// Publishers do swap thumbnails after a launch, but a month-old picture of the
// right trailer is still the right picture.
inline constexpr int64_t kRefreshSeconds = 30ll * 24 * 60 * 60;
// Shorter than the update check's 8 s: nothing waits on this, but a tile that
// stays blank for 8 s per stage on a captive portal is the case to cut short.
inline constexpr uint32_t kTimeoutMilliseconds = 5000;

// maxresdefault is 1280x720 and exists for every curated trailer; hqdefault
// (480x360, letterboxed) exists for every video and is the fallback.
enum class Variant { MaxRes, High };

// A YouTube video id: eleven characters of the URL-safe base64 alphabet.
// Nothing else reaches a path or a file name.
inline bool ValidVideoId(std::wstring_view id)
{
    if (id.size() != 11) return false;
    for (const wchar_t c : id) {
        const bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') ||
                        c == L'-' || c == L'_';
        if (!ok) return false;
    }
    return true;
}

// The id of a curated entry's URL, which ExampleVideos.h spells exactly as
// https://www.youtube.com/watch?v=<id>. Any other shape answers nothing: this
// is not a general URL parser, and a pasted link never gets a thumbnail.
inline std::optional<std::wstring> VideoIdFromWatchUrl(std::wstring_view url)
{
    constexpr std::wstring_view prefix = L"https://www.youtube.com/watch?v=";
    if (!url.starts_with(prefix)) return std::nullopt;
    url.remove_prefix(prefix.size());
    if (!ValidVideoId(url)) return std::nullopt;
    return std::wstring(url);
}

inline std::wstring_view FileName(Variant variant)
{
    return variant == Variant::MaxRes ? L"maxresdefault.jpg" : L"hqdefault.jpg";
}

// The request path on kHost, or empty for an invalid id.
inline std::wstring RequestPath(std::wstring_view id, Variant variant)
{
    if (!ValidVideoId(id)) return {};
    return L"/vi/" + std::wstring(id) + L"/" + std::wstring(FileName(variant));
}

inline std::wstring ThumbnailUrl(std::wstring_view id, Variant variant)
{
    const std::wstring path = RequestPath(id, variant);
    return path.empty() ? std::wstring() : L"https://" + std::wstring(kHost) + path;
}

// The allowlist, checked on the exact URL before any request is made: HTTPS,
// the one host with no port, user info, query or fragment, and one of the two
// file names under a valid id. Redirects are not followed, so this is also
// the only place a fetch can go.
inline bool AllowedThumbnailUrl(std::wstring_view url)
{
    constexpr std::wstring_view scheme = L"https://";
    if (!url.starts_with(scheme)) return false;
    url.remove_prefix(scheme.size());
    if (!url.starts_with(kHost)) return false;
    url.remove_prefix(kHost.size());
    constexpr std::wstring_view vi = L"/vi/";
    if (!url.starts_with(vi)) return false;
    url.remove_prefix(vi.size());
    if (url.size() < 12 || !ValidVideoId(url.substr(0, 11)) || url[11] != L'/') return false;
    url.remove_prefix(12);
    return url == FileName(Variant::MaxRes) || url == FileName(Variant::High);
}

// What a tile's thumbnail needs this time. Disabled fetching still shows a
// picture already on disk - showing it costs no network - and a stamp from the
// future counts as stale, so a clock that was wrong once cannot pin a picture.
enum class CacheDecision {
    UseCache,         // fresh on disk: no request
    Fetch,            // nothing on disk
    Refresh,          // stale on disk: show it, fetch, keep it if the fetch fails
    CacheOnly,        // fetching is off, a picture is on disk
    Placeholder,      // fetching is off and nothing is on disk
};

inline CacheDecision Decide(bool fetchEnabled, std::optional<int64_t> cachedUnix, int64_t nowUnix)
{
    if (!fetchEnabled) return cachedUnix ? CacheDecision::CacheOnly : CacheDecision::Placeholder;
    if (!cachedUnix) return CacheDecision::Fetch;
    if (*cachedUnix > nowUnix || nowUnix - *cachedUnix >= kRefreshSeconds) return CacheDecision::Refresh;
    return CacheDecision::UseCache;
}

// A JPEG starts FF D8 FF. Checked before anything is written to the cache, so
// an error page or a captive portal's HTML never lands under a .jpg name.
inline bool LooksLikeJpeg(std::string_view bytes)
{
    return bytes.size() >= 3 && bytes.size() <= kMaximumBytes && static_cast<unsigned char>(bytes[0]) == 0xFF &&
           static_cast<unsigned char>(bytes[1]) == 0xD8 && static_cast<unsigned char>(bytes[2]) == 0xFF;
}

// The centred region of a width x height image with the tile's 16:9 aspect.
// hqdefault.jpg is 4:3 with the 16:9 frame letterboxed inside it, so this
// takes the bars off; a 16:9 maxresdefault comes back whole.
struct Crop { uint32_t x{}, y{}, width{}, height{}; };

inline Crop CoverCrop(uint32_t width, uint32_t height)
{
    if (!width || !height) return {};
    // width/height > 16/9: wider than the tile, trim the sides.
    if (uint64_t(width) * 9u > uint64_t(height) * 16u) {
        const uint32_t cropped = static_cast<uint32_t>(uint64_t(height) * 16u / 9u);
        return {(width - cropped) / 2u, 0u, cropped, height};
    }
    const uint32_t cropped = static_cast<uint32_t>(uint64_t(width) * 9u / 16u);
    return {0u, (height - cropped) / 2u, width, cropped};
}

} // namespace trailer_thumbnail
