#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ParallelFor.h"

// NV12 -> BGRA for the comparison reference.
//
// Limited range: Y spans 16..235 over 219, chroma 16..240 centred on 128 over
// 224. The matrix is Rec.709's inverse. Chroma is half resolution in both axes
// and sampled nearest, which is what the reference is for - a side-by-side
// against the neural frame, not a mastering path.
//
// This was written for a paused inspection, where a CPU pass over one frame
// costs nothing anyone can perceive. It is not only that any more: since
// playback started decoding NV12, the comparison reference is converted on
// every presented frame whenever the viewer picks Blend, Split or Wipe, or
// moves neural strength off 1.0. A scalar double pass over 3.69 Mpx does not
// fit a 16.68 ms budget, so comparison-while-playing became a slideshow.
//
// The arithmetic below is deliberately unchanged. Every per-pixel value is
// still the same IEEE double computed by the same operations in the same
// order - the tables hold exactly the subexpressions that depend on one
// sample, so the output is bit-identical to the scalar version. What went
// away is three divisions and two multiplications per pixel, and the rows now
// run in parallel.
namespace nv12 {

struct Bt709LimitedTables {
    double luma[256];      // (Y - 16) / 219
    double red[256];       // 1.5748 * (Cr - 128) / 224
    double greenBlue[256]; // 0.1873 * (Cb - 128) / 224
    double greenRed[256];  // 0.4681 * (Cr - 128) / 224
    double blue[256];      // 1.8556 * (Cb - 128) / 224
};

inline const Bt709LimitedTables& Tables()
{
    static const Bt709LimitedTables tables = [] {
        Bt709LimitedTables built{};
        for (int value = 0; value < 256; ++value) {
            const double luminance = (double(value) - 16.0) / 219.0;
            const double difference = (double(value) - 128.0) / 224.0;
            built.luma[value] = luminance;
            built.red[value] = 1.5748 * difference;
            built.greenBlue[value] = 0.1873 * difference;
            built.greenRed[value] = 0.4681 * difference;
            built.blue[value] = 1.8556 * difference;
        }
        return built;
    }();
    return tables;
}

// Rows are independent and each reads a disjoint luma band plus one shared
// chroma row, so this is the same split DownsampleLuma uses.
inline constexpr size_t kRowGrain = 16;

inline void ToBgraBt709Limited(const uint8_t* nv12, uint32_t width, uint32_t height,
                               std::vector<uint8_t>& bgra)
{
    if (!nv12 || !width || !height || (width | height) & 1u) return;
    bgra.resize(size_t(width) * height * 4u);
    const uint8_t* luma = nv12;
    const uint8_t* chroma = nv12 + size_t(width) * height;
    const Bt709LimitedTables& table = Tables();
    uint8_t* const destination = bgra.data();

    ParallelForRanges(size_t(height), kRowGrain, [&](size_t beginRow, size_t endRow) {
        for (uint32_t y = uint32_t(beginRow); y < uint32_t(endRow); ++y) {
            const uint8_t* lumaRow = luma + size_t(y) * width;
            const uint8_t* chromaRow = chroma + size_t(y / 2u) * width;
            uint8_t* out = destination + size_t(y) * width * 4u;
            for (uint32_t x = 0; x < width; ++x) {
                const double luminance = table.luma[lumaRow[x]];
                const uint8_t cb = chromaRow[(x & ~1u)];
                const uint8_t cr = chromaRow[(x & ~1u) + 1u];
                const double red = luminance + table.red[cr];
                const double green = luminance - table.greenBlue[cb] - table.greenRed[cr];
                const double blue = luminance + table.blue[cb];
                const auto clamp8 = [](double value) {
                    return uint8_t(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
                };
                out[size_t(x) * 4u + 0u] = clamp8(blue);
                out[size_t(x) * 4u + 1u] = clamp8(green);
                out[size_t(x) * 4u + 2u] = clamp8(red);
                out[size_t(x) * 4u + 3u] = 255u;
            }
        }
    });
}

} // namespace nv12
