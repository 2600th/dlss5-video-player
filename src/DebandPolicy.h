#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// A deband pre-pass on the decoded source, ahead of the neural model: libplacebo's
// deband (pl_shader_deband) at its default parameters - one iteration, threshold 3,
// radius 16, grain 4 - folded into the pass that converts the decoded frame to linear
// for the model (PSConvertDebanded in D3D12Renderer.cpp).
//
// Per pixel it takes the average of four samples a random distance (up to the radius)
// away at quarter turns of a random angle, and replaces the pixel with that average
// only where the two differ by less than the threshold: a band edge in a compressed
// gradient is a step of about one code value and is smoothed away, a real edge is
// larger and is kept. A little grain then covers what the average left flat. Both
// work in the decoded, gamma-encoded values, as libplacebo's do.
//
// One departure, on purpose: libplacebo draws its random offsets and its grain from
// a generator seeded per frame, so the pattern changes every frame. Here they are a
// hash of the pixel alone. A pattern that moves every frame is temporal noise in the
// model's input, which its history would carry into flicker, and it would make two
// renders of one source differ byte for byte. A fixed pattern costs neither.
//
// Off by default and a cache-key term: it changes what the model is shown. Measured
// (docs/measurements/deband-20260923/), the model does not amplify a source's banding -
// sources scoring CAMBI 1.6-10.9 came out at 0.004-1.9 at 10 bits - so the pass takes an
// already invisible residue to zero while its grain and averaging move textured and text
// clips measurably (VMAF 97.3-97.7 against the plain render). The banding a viewer sees
// is made after the model, by the Standard rung's 8-bit encode, where this cannot reach.
//
// The four parameters reach the shader from here (pasted as text, like the dither
// map in DitherPolicy.h), and DebandPixel below mirrors the shader's arithmetic so the
// decision can be tested without a device.
#define DLSS_DEBAND_ITERATIONS 1
#define DLSS_DEBAND_THRESHOLD 3.0
#define DLSS_DEBAND_RADIUS 16.0
#define DLSS_DEBAND_GRAIN 4.0

namespace deband {

inline constexpr int kIterations = DLSS_DEBAND_ITERATIONS;
inline constexpr float kThreshold = static_cast<float>(DLSS_DEBAND_THRESHOLD);
inline constexpr float kRadius = static_cast<float>(DLSS_DEBAND_RADIUS);
inline constexpr float kGrain = static_cast<float>(DLSS_DEBAND_GRAIN);

// The shader's per-pixel hash (DebandHash): an integer mix of the pixel and a stream
// index, so every pixel draws its own fixed numbers and the streams are independent.
constexpr uint32_t Hash(uint32_t x, uint32_t y, uint32_t stream)
{
    uint32_t v = x * 1664525u + y * 1013904223u + stream * 2654435761u;
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

// A uniform number in [0, 1) from the top 24 bits, exactly as the shader forms it.
constexpr float Random(uint32_t x, uint32_t y, uint32_t stream)
{
    return static_cast<float>(Hash(x, y, stream) >> 8) * (1.0f / 16777216.0f);
}

// One channel of the pass at pixel (x, y), for a test's image `at(x, y)` of
// gamma-encoded values in [0, 1], sampled with the nearest texel (the shader samples
// bilinearly; a test's image is built so the two agree). Mirrors PSConvertDebanded
// up to the linearisation that follows it.
template <typename Sample>
float DebandPixel(const Sample& at, int x, int y)
{
    float value = at(x, y);
    for (int i = 1; i <= kIterations; ++i) {
        const float distance = Random(uint32_t(x), uint32_t(y), 0) * kRadius * float(i);
        const float angle = Random(uint32_t(x), uint32_t(y), 1) * 6.2831853f;
        const int dx = static_cast<int>(std::lround(distance * std::cos(angle)));
        const int dy = static_cast<int>(std::lround(distance * std::sin(angle)));
        const float average = 0.25f * (at(x + dx, y + dy) + at(x - dx, y + dy) +
                                       at(x - dx, y - dy) + at(x + dx, y - dy));
        // Kept where the difference reaches the bound, as the shader's step() keeps it.
        const float bound = kThreshold / (1000.0f * float(i));
        if (std::abs(value - average) < bound) value = average;
    }
    // Grain, scaled down towards black so true black stays black.
    const float strength = std::min(std::abs(value), kGrain / 1000.0f);
    value += strength * (Random(uint32_t(x), uint32_t(y), 2) - 0.5f);
    return std::clamp(value, 0.0f, 1.0f);
}

} // namespace deband
