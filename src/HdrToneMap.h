#pragma once

#include "HdrPolicy.h"
#include "MediaSource.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// The HDR -> SDR tone map as integer arithmetic over a 3D table, so the GPU pass
// that runs it (HdrToneMapGpu) and the CPU fallback produce the same bytes.
//
// The curve is HdrPolicy.h's: the source's PQ or HLG code values to linear light
// at 100 nits to 1.0, BT.2020 -> BT.709 primaries, Hable over the brightest
// channel with no desaturation (ffmpeg's `tonemap`), then the inverse of BT.1886.
// What changed is where it runs. ffmpeg ran it on the CPU in 32-bit float over
// four full-frame passes - 30.7 fps and 89 s of CPU time for 10 s of 4K, which
// is every core the machine has. Here the curve is evaluated once, in double,
// at 65^3 points of the source's R'G'B' (a perceptual domain for PQ and HLG),
// and every pixel is a fixed-point Y'CbCr -> R'G'B' conversion and a tetrahedral
// interpolation between four of those points. Integer arithmetic is the whole
// point: HLSL and C++ agree on it exactly, so where a frame was tone mapped is
// not part of what it looks like, and the render cache key (ToneMapIdentityTerm)
// names the algorithm, not the device. Against ffmpeg's float chain the bytes
// differ by a code value or two, so the term is a new version.
//
// Input is P010 as ffmpeg writes it (-pix_fmt p010le): a Y plane of 16-bit
// samples with the ten bits at the top, then the interleaved CbCr plane at half
// resolution. Chroma is upsampled in integers with MPEG-2 siting - co-sited with
// even columns, midway between row pairs - which is ffmpeg's and zimg's default
// for 4:2:0. The matrix is BT.2020 non-constant-luminance, the only one HDR10 and
// HLG streams use; any other declared matrix keeps ffmpeg's float chain (v1).
namespace hdr_tonemap {

inline constexpr int kLutPoints = 65;          // per axis; 64 intervals over [0,1]
inline constexpr int kFractionBits = 12;       // tetrahedral weights sum to 4096
inline constexpr int kMatrixShift = 13;        // Y'CbCr -> R'G'B' fixed point
inline constexpr size_t kLutEntries = size_t(kLutPoints) * kLutPoints * kLutPoints * 3;

// Whether a source is tone mapped here rather than by ffmpeg's float chain. P010
// needs even dimensions; the matrix must be BT.2020 NCL, or undeclared (ffmpeg's
// chain assumes BT.2020 NCL for an undeclared HDR matrix too).
inline bool Supported(const SourceColorDescription& color, uint32_t width, uint32_t height)
{
    if (hdr_policy::SignalOf(color) == hdr_policy::HdrSignal::Sdr) return false;
    if (!width || !height || (width & 1u) || (height & 1u)) return false;
    return color.matrix == ColorMatrix::Bt2020Ncl || color.matrix == ColorMatrix::Unspecified;
}

// Fixed-point Y'CbCr -> R'G'B' for ten-bit samples scaled by 8 (the upsampled
// chroma carries that scale; luma is multiplied up to match): R' in 0..65535 is
// (yTerm + crR*Cr) >> kMatrixShift, and so on. Every product stays inside int32
// for any ten-bit input, which cs_5_0 needs: it has no 64-bit integers.
struct Matrix {
    int32_t yOffset = 0;   // subtracted from 8*Y
    int32_t y = 0, crR = 0, cbG = 0, crG = 0, cbB = 0;
};

inline Matrix MatrixFor(bool fullRange)
{
    // BT.2020 NCL: Kr 0.2627, Kb 0.0593.
    constexpr double kr = 0.2627, kb = 0.0593, kg = 1.0 - kr - kb;
    const double yScale = fullRange ? 1023.0 : 876.0, cScale = fullRange ? 1023.0 : 896.0;
    const double unit = 65535.0 * double(1 << kMatrixShift) / 8.0;
    const auto fixed = [](double value) { return static_cast<int32_t>(std::lround(value)); };
    Matrix m;
    m.yOffset = fullRange ? 0 : 64 * 8;
    m.y = fixed(unit / yScale);
    m.crR = fixed(unit * 2.0 * (1.0 - kr) / cScale);
    m.cbB = fixed(unit * 2.0 * (1.0 - kb) / cScale);
    m.cbG = fixed(unit * 2.0 * kb * (1.0 - kb) / kg / cScale);
    m.crG = fixed(unit * 2.0 * kr * (1.0 - kr) / kg / cScale);
    return m;
}

// ---- The curve, in double, for the table -----------------------------------

inline double PqToNits(double code)
{
    constexpr double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    constexpr double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    const double p = std::pow(std::max(code, 0.0), 1.0 / m2);
    return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

inline double HlgToScene(double code)
{
    constexpr double a = 0.17883277, b = 1.0 - 4.0 * a;
    const double c = 0.5 - a * std::log(4.0 * a);
    code = std::max(code, 0.0);
    return code <= 0.5 ? code * code / 3.0 : (std::exp((code - c) / a) + b) / 12.0;
}

inline double Hable(double x)
{
    constexpr double a = 0.15, b = 0.50, c = 0.10, d = 0.20, e = 0.02, f = 0.30;
    return (x * (x * a + b * c) + d * e) / (x * (x * a + b) + d * f) - e / f;
}

// One point of the curve: BT.2020 R'G'B' code values in [0,1] to BT.709 linear
// light after the tone map, signed - out-of-gamut colours go negative on the way
// to BT.709 and are clipped only at the very end (Encode), as ffmpeg's chain
// clips them.
inline void Curve(hdr_policy::HdrSignal signal, double peakNits, const double in[3], double out[3])
{
    double linear[3];
    if (signal == hdr_policy::HdrSignal::Hlg) {
        // zimg's HLG decode: the inverse OETF, then the system gamma of a nominal
        // 1000-nit display, 1.2 - applied per channel, as zimg does, not on scene
        // luminance as BT.2100's OOTF has it. Fitted on the ffmpeg chain's own
        // output: per channel matches it to 0.43 of a code value on average,
        // on luminance leaves a 1.4 hue shift (green up, red and blue down).
        for (int i = 0; i < 3; ++i)
            linear[i] = hdr_policy::kHlgPeakNits / hdr_policy::kToneMapReferenceNits * std::pow(HlgToScene(in[i]), 1.2);
    } else {
        for (int i = 0; i < 3; ++i) linear[i] = PqToNits(in[i]) / hdr_policy::kToneMapReferenceNits;
    }
    static constexpr double k2020To709[3][3] = {
        {1.6604910021, -0.5876411388, -0.0728498633},
        {-0.1245504745, 1.1328998971, -0.0083494226},
        {-0.0181507634, -0.1005788980, 1.1187296614}};
    double rgb[3];
    for (int i = 0; i < 3; ++i)
        rgb[i] = k2020To709[i][0] * linear[0] + k2020To709[i][1] * linear[1] + k2020To709[i][2] * linear[2];
    // ffmpeg's tonemap: scale all three by the curve of the brightest.
    const double peak = peakNits / hdr_policy::kToneMapReferenceNits;
    const double signal0 = std::max({rgb[0], rgb[1], rgb[2], 1e-6});
    const double scale = Hable(signal0) / Hable(peak) / signal0;
    for (int i = 0; i < 3; ++i) out[i] = rgb[i] * scale;
}

// The end of the chain: clip to [0,1] and the inverse of BT.1886, to 8 bits.
inline int Encode(double linear)
{
    return int(std::lround(std::pow(std::clamp(linear, 0.0, 1.0), 1.0 / 2.4) * 255.0));
}

// The table is interpolated in linear light, not in the encoded output: the
// encode's 1/2.4 power is vertical at zero, and a saturated colour whose weakest
// channel sits near zero was off by up to 38 codes interpolated after it, 15 before
// (numpy, 400k samples). Linear light near black needs more than 16 bits, so a node
// is 24: (x + 1) * 2^23 over x in [-1, 1), and the interpolation splits each node
// into two 12-bit halves to keep every product in int32.
inline constexpr int32_t kLinearOne = 1 << 23;
inline constexpr size_t kThresholdOffset = kLutEntries;           // 256 after the nodes
inline constexpr size_t kTableWords = kLutEntries + 256;

// 65^3 R, G, B triples of 24-bit signed linear light, index ((r * 65 + g) * 65 + b)
// * 3, then the 255 encode thresholds: code k is the first whose threshold a
// positive linear value is below, T[k] = ceil(((k - 0.5) / 255)^2.4 * 2^23), so
// the encode is a binary search and integer on both sides.
inline std::vector<uint32_t> BuildLut(hdr_policy::HdrSignal signal, double peakNits)
{
    std::vector<uint32_t> table(kTableWords, 0);
    for (int r = 0; r < kLutPoints; ++r)
        for (int g = 0; g < kLutPoints; ++g)
            for (int b = 0; b < kLutPoints; ++b) {
                const double in[3] = {r / 64.0, g / 64.0, b / 64.0};
                double out[3];
                Curve(signal, peakNits, in, out);
                uint32_t* node = &table[((size_t(r) * kLutPoints + g) * kLutPoints + b) * 3];
                for (int i = 0; i < 3; ++i) {
                    const double code = std::round((std::clamp(out[i], -1.0, 1.0) + 1.0) * kLinearOne);
                    node[i] = uint32_t(std::clamp(code, 0.0, double((1 << 24) - 1)));
                }
            }
    for (int k = 1; k < 256; ++k)
        table[kThresholdOffset + k] = uint32_t(std::ceil(std::pow((k - 0.5) / 255.0, 2.4) * kLinearOne));
    return table;
}

// ---- The per-pixel integer path (mirrored line for line in HdrToneMapGpu) ----

inline int32_t ClampCode(int32_t value) { return value < 0 ? 0 : value > 65535 ? 65535 : value; }

// A linear value in table units to its 8-bit code.
inline uint32_t EncodeLinear(const uint32_t* table, int32_t linear)
{
    if (linear <= 0) return 0;
    uint32_t code = 0;
    for (uint32_t step = 128; step; step >>= 1)
        if (code + step <= 255 && uint32_t(linear) >= table[kThresholdOffset + code + step]) code += step;
    return code;
}

// R'G'B' in 0..65535 through the table, to one 8-bit BGRA pixel.
inline uint32_t Interpolate(const uint32_t* table, int32_t r, int32_t g, int32_t b)
{
    const int32_t xr = r * 64, xg = g * 64, xb = b * 64;
    const int32_t ir = xr >> 16, ig = xg >> 16, ib = xb >> 16;
    const int32_t fr = (xr & 0xFFFF) >> 4, fg = (xg & 0xFFFF) >> 4, fb = (xb & 0xFFFF) >> 4;
    const auto at = [&](int32_t dr, int32_t dg, int32_t db) {
        return table + ((size_t(ir + dr) * kLutPoints + size_t(ig + dg)) * kLutPoints + size_t(ib + db)) * 3;
    };
    // The tetrahedron the point is in: the diagonal walk from the cell's first
    // corner to its last, one axis at a time in order of the largest fraction.
    const uint32_t* c0 = at(0, 0, 0);
    const uint32_t* c3 = at(1, 1, 1);
    const uint32_t *c1, *c2;
    int32_t w0, w1, w2, w3;
    if (fr >= fg) {
        if (fg >= fb) { c1 = at(1, 0, 0); c2 = at(1, 1, 0); w0 = 4096 - fr; w1 = fr - fg; w2 = fg - fb; w3 = fb; }
        else if (fr >= fb) { c1 = at(1, 0, 0); c2 = at(1, 0, 1); w0 = 4096 - fr; w1 = fr - fb; w2 = fb - fg; w3 = fg; }
        else { c1 = at(0, 0, 1); c2 = at(1, 0, 1); w0 = 4096 - fb; w1 = fb - fr; w2 = fr - fg; w3 = fg; }
    } else {
        if (fb >= fg) { c1 = at(0, 0, 1); c2 = at(0, 1, 1); w0 = 4096 - fb; w1 = fb - fg; w2 = fg - fr; w3 = fr; }
        else if (fb >= fr) { c1 = at(0, 1, 0); c2 = at(0, 1, 1); w0 = 4096 - fg; w1 = fg - fb; w2 = fb - fr; w3 = fr; }
        else { c1 = at(0, 1, 0); c2 = at(1, 1, 0); w0 = 4096 - fg; w1 = fg - fr; w2 = fr - fb; w3 = fb; }
    }
    uint32_t pixel = 0xFF000000u;
    for (int channel = 0; channel < 3; ++channel) {
        // (sum w * node) / 4096, with node = high * 4096 + low: sum w * high, plus
        // the rounded sum w * low / 4096. Both sums are below 2^24.
        const auto high = [](uint32_t node) { return int32_t(node >> 12); };
        const auto low = [](uint32_t node) { return int32_t(node & 0xFFFu); };
        const int32_t highs = w0 * high(c0[channel]) + w1 * high(c1[channel]) + w2 * high(c2[channel]) + w3 * high(c3[channel]);
        const int32_t lows = w0 * low(c0[channel]) + w1 * low(c1[channel]) + w2 * low(c2[channel]) + w3 * low(c3[channel]);
        const int32_t linear = highs + ((lows + (1 << (kFractionBits - 1))) >> kFractionBits) - kLinearOne;
        pixel |= EncodeLinear(table, linear) << (16 - 8 * channel);  // R at 16, G at 8, B at 0: BGRA in memory
    }
    return pixel;
}

// One pixel of a P010 frame: `luma` and `chroma` are the two planes as 16-bit
// samples, `x`/`y` the pixel, `w`/`h` the frame (both even).
inline uint32_t ToneMapP010Pixel(const uint16_t* luma, const uint16_t* chroma, uint32_t w, uint32_t h,
                                 uint32_t x, uint32_t y, const Matrix& m, const uint32_t* lut)
{
    const uint32_t cw = w / 2, ch = h / 2;
    const uint32_t j = x >> 1, k = y >> 1;
    const uint32_t jn = (x & 1u) ? std::min(j + 1, cw - 1) : j;
    const uint32_t kn = (y & 1u) ? std::min(k + 1, ch - 1) : (k ? k - 1 : 0);
    const auto sample = [&](uint32_t col, uint32_t row, uint32_t component) {
        return int32_t(chroma[(size_t(row) * cw + col) * 2 + component] >> 6);
    };
    // Horizontal first (x2), then vertical 3:1 (x4): both at 8x.
    const auto upsampled = [&](uint32_t component) {
        const int32_t nearRow = sample(j, k, component) + sample(jn, k, component);
        const int32_t farRow = sample(j, kn, component) + sample(jn, kn, component);
        return 3 * nearRow + farRow;
    };
    const int32_t cb = upsampled(0) - 4096, cr = upsampled(1) - 4096;
    const int32_t yTerm = (int32_t(luma[size_t(y) * w + x] >> 6) * 8 - m.yOffset) * m.y;
    constexpr int32_t round = 1 << (kMatrixShift - 1);
    const int32_t r = ClampCode((yTerm + m.crR * cr + round) >> kMatrixShift);
    const int32_t g = ClampCode((yTerm - m.cbG * cb - m.crG * cr + round) >> kMatrixShift);
    const int32_t b = ClampCode((yTerm + m.cbB * cb + round) >> kMatrixShift);
    return Interpolate(lut, r, g, b);
}

// Rows [rowBegin, rowEnd) of a P010 frame to BGRA, the CPU fallback.
inline void ToneMapP010Rows(const uint8_t* p010, uint32_t w, uint32_t h, const Matrix& m, const uint32_t* lut,
                            uint8_t* bgra, uint32_t rowBegin, uint32_t rowEnd)
{
    const auto* luma = reinterpret_cast<const uint16_t*>(p010);
    const uint16_t* chroma = luma + size_t(w) * h;
    for (uint32_t y = rowBegin; y < rowEnd; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t pixel = ToneMapP010Pixel(luma, chroma, w, h, x, y, m, lut);
            std::memcpy(bgra + (size_t(y) * w + x) * 4u, &pixel, 4);
        }
}

inline size_t P010FrameBytes(uint32_t w, uint32_t h) { return size_t(w) * h * 3u; }

} // namespace hdr_tonemap
