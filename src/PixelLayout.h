#pragma once
#include <cstddef>
#include <cstdint>

// Byte layout of a raw source frame as it travels decoder -> guides -> renderer.
//   Bgra: 4 bytes per pixel, rows tightly packed.
//   Nv12: full-resolution Y plane followed by a half-resolution interleaved UV plane,
//         BT.709 limited range, exactly ffmpeg's `-pix_fmt nv12` rawvideo layout. Even
//         dimensions only. 4.2 MB instead of 11.1 MB per 2578x1080 frame, which is why
//         the export decodes to it.
enum class PixelLayout { Bgra, Nv12 };

inline size_t PixelLayoutFrameBytes(PixelLayout layout, uint32_t width, uint32_t height)
{
    const size_t pixels = size_t(width) * size_t(height);
    return layout == PixelLayout::Nv12 ? pixels + pixels / 2u : pixels * 4u;
}
