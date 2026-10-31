#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "DroppedFilesPolicy.h"

// The clipboard (P2.41): what Ctrl+V opens, and the bitmap Ctrl+Shift+C puts
// beside the PNG. The decisions and the pixel conversions are here, free of
// the clipboard itself, so they can be tested; main.cpp opens the clipboard,
// reads or writes it, and closes it again before it acts.
namespace clipboard {

// ---- What a paste opens --------------------------------------------------------

enum class PasteKind { Nothing, File, YouTube, Image, UnsupportedLink };

// What the clipboard held, read once while it was open.
struct PasteContent {
    std::vector<std::wstring> files;  // CF_HDROP, as Explorer copies files
    std::wstring text;                // CF_UNICODETEXT, empty when none
    bool image = false;               // a PNG or a device-independent bitmap
};

struct PasteDecision {
    PasteKind kind = PasteKind::Nothing;
    // The file to open, or the YouTube address; empty for an image and for nothing.
    std::wstring target;
    // Files on the clipboard that will not be opened.
    size_t ignored = 0;
};

// The one line of pasted text that could be an address or a path: the first
// that is not blank, without the whitespace around it or the quotes Explorer's
// "Copy as path" puts around a path, or the angle brackets a mail client puts
// around a link.
inline std::wstring PastedLine(std::wstring_view text)
{
    while (!text.empty()) {
        const size_t end = text.find_first_of(L"\r\n");
        std::wstring_view line = text.substr(0, end);
        text = end == std::wstring_view::npos ? std::wstring_view{} : text.substr(end + 1);
        while (!line.empty() && std::iswspace(line.front())) line.remove_prefix(1);
        while (!line.empty() && std::iswspace(line.back())) line.remove_suffix(1);
        if (line.size() >= 2 && ((line.front() == L'"' && line.back() == L'"') || (line.front() == L'<' && line.back() == L'>'))) {
            line.remove_prefix(1);
            line.remove_suffix(1);
        }
        if (!line.empty()) return std::wstring(line);
    }
    return {};
}

inline bool LooksLikeLink(std::wstring_view line)
{
    const auto starts = [&](std::wstring_view prefix) {
        if (line.size() < prefix.size()) return false;
        for (size_t index = 0; index < prefix.size(); ++index)
            if (std::towlower(line[index]) != prefix[index]) return false;
        return true;
    };
    return starts(L"http://") || starts(L"https://") || starts(L"www.");
}

// Files first, as a drop of them would open; then text that is a YouTube link
// or names a file; then a picture; and otherwise nothing, with a reason. Text
// wins over a picture only when it is one of those two, so a page copied with
// an image on it still opens the image.
template <typename IsDirectory, typename IsFile, typename IsYouTube>
PasteDecision Decide(const PasteContent& content, std::wstring_view patterns, IsDirectory isDirectory, IsFile isFile,
                     IsYouTube isYouTube)
{
    PasteDecision decision;
    if (!content.files.empty()) {
        const dropped_files::Choice choice = dropped_files::Choose(content.files, patterns, isDirectory);
        if (choice.open) {
            decision.kind = PasteKind::File;
            decision.target = content.files[*choice.open];
            decision.ignored = choice.ignored;
            return decision;
        }
    }
    const std::wstring line = PastedLine(content.text);
    if (!line.empty() && isYouTube(line)) {
        decision.kind = PasteKind::YouTube;
        decision.target = line;
        return decision;
    }
    if (!line.empty() && !LooksLikeLink(line) && isFile(line)) {
        decision.kind = PasteKind::File;
        decision.target = line;
        return decision;
    }
    if (content.image) {
        decision.kind = PasteKind::Image;
        return decision;
    }
    if (LooksLikeLink(line)) decision.kind = PasteKind::UnsupportedLink;
    return decision;
}

// ---- Pixels ------------------------------------------------------------------

// The 8-byte signature every PNG file starts with.
inline bool IsPng(std::span<const uint8_t> bytes)
{
    static constexpr uint8_t kSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    return bytes.size() >= sizeof(kSignature) && std::memcmp(bytes.data(), kSignature, sizeof(kSignature)) == 0;
}

// A 24-bit row's length in a DIB: whole DWORDs.
inline uint32_t DibStride(uint32_t width) { return (width * 3u + 3u) & ~3u; }

// The largest side either conversion takes, which keeps every product below in
// 32 bits with room to spare.
inline constexpr uint32_t kMaxSide = 16384;

// CF_DIB for 24-bit BGR rows `stride` bytes apart, top row first: a
// BITMAPINFOHEADER and the rows bottom-up, which every program that pastes a
// bitmap reads. Windows makes CF_DIBV5 and CF_BITMAP from it on request.
inline std::vector<uint8_t> DibFromBgr(const uint8_t* bgr, uint32_t width, uint32_t height, uint32_t stride)
{
    if (!bgr || !width || !height || width > kMaxSide || height > kMaxSide || stride < width * 3u) return {};
    const uint32_t rowBytes = DibStride(width);
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = LONG(width);
    header.biHeight = LONG(height);  // positive: bottom-up
    header.biPlanes = 1;
    header.biBitCount = 24;
    header.biCompression = BI_RGB;
    header.biSizeImage = rowBytes * height;
    std::vector<uint8_t> dib(sizeof(header) + size_t(rowBytes) * height, 0);
    std::memcpy(dib.data(), &header, sizeof(header));
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(dib.data() + sizeof(header) + size_t(height - 1 - y) * rowBytes, bgr + size_t(y) * stride, size_t(width) * 3u);
    return dib;
}

// A pasted CF_DIB or CF_DIBV5 as 24-bit BGR rows, top row first, DibStride
// apart, for the PNG encoder. Takes what screenshots and browsers put there -
// 24 or 32 bits, BI_RGB or BI_BITFIELDS with the usual masks, either row order,
// any of the three header sizes - and refuses the rest (palettes, compression,
// odd masks) rather than guessing. Alpha is dropped: the player opens the
// picture as a photo.
inline bool BgrFromDib(std::span<const uint8_t> dib, std::vector<uint8_t>& bgr, uint32_t& width, uint32_t& height,
                       uint32_t& stride)
{
    bgr.clear();
    width = height = stride = 0;
    if (dib.size() < sizeof(BITMAPINFOHEADER)) return false;
    BITMAPINFOHEADER header{};
    std::memcpy(&header, dib.data(), sizeof(header));
    if (header.biSize != sizeof(BITMAPINFOHEADER) && header.biSize != sizeof(BITMAPV4HEADER) &&
        header.biSize != sizeof(BITMAPV5HEADER))
        return false;
    if (header.biSize > dib.size() || header.biPlanes != 1 || header.biWidth <= 0 || header.biHeight == 0) return false;
    const bool topDown = header.biHeight < 0;
    const int64_t rows = topDown ? -int64_t(header.biHeight) : int64_t(header.biHeight);
    if (header.biWidth > LONG(kMaxSide) || rows > int64_t(kMaxSide)) return false;
    const uint32_t bits = header.biBitCount;
    if (bits != 24 && bits != 32) return false;
    size_t offset = header.biSize;
    if (header.biCompression == BI_BITFIELDS) {
        if (bits != 32) return false;
        uint32_t masks[3]{};
        if (header.biSize == sizeof(BITMAPINFOHEADER)) {
            // The three masks follow a plain header.
            if (dib.size() < offset + sizeof(masks)) return false;
            std::memcpy(masks, dib.data() + offset, sizeof(masks));
            offset += sizeof(masks);
        } else {
            // A V4 or V5 header carries them itself, after the plain fields.
            std::memcpy(masks, dib.data() + sizeof(BITMAPINFOHEADER), sizeof(masks));
        }
        if (masks[0] != 0x00FF0000u || masks[1] != 0x0000FF00u || masks[2] != 0x000000FFu) return false;
    } else if (header.biCompression != BI_RGB) {
        return false;
    }
    if (header.biClrUsed != 0) offset += size_t(header.biClrUsed) * 4u;
    width = uint32_t(header.biWidth);
    height = uint32_t(rows);
    const size_t sourceStride = ((size_t(width) * bits + 31u) / 32u) * 4u;
    if (offset > dib.size() || (dib.size() - offset) / sourceStride < height) {
        width = height = 0;
        return false;
    }
    stride = DibStride(width);
    bgr.assign(size_t(stride) * height, 0);
    const size_t step = bits / 8u;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* from = dib.data() + offset + size_t(topDown ? y : height - 1 - y) * sourceStride;
        uint8_t* to = bgr.data() + size_t(y) * stride;
        for (uint32_t x = 0; x < width; ++x) {
            to[x * 3u] = from[x * step];
            to[x * 3u + 1] = from[x * step + 1];
            to[x * 3u + 2] = from[x * step + 2];
        }
    }
    return true;
}

} // namespace clipboard
