#pragma once

#include "PixelLayout.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Per-render temporal metrics the player can afford to compute while it renders
// (P2.11). PSNR or VMAF against the source penalise exactly the change the user
// asked the model for, so these measure what the model did to MOTION instead, the
// way tools/benchmark does offline:
//
//   * warping error - the mean change of a sample from the previous frame, after
//     that frame was moved along the guide generator's own flow; measured on the
//     source and on the output, and the difference is the flicker the render
//     added (Lai et al., ECCV 2018, without the learned occlusion mask: the
//     source's own residue sits in both terms and cancels);
//   * temporal sigma - per-sample standard deviation of luma inside a shot, on
//     both, which is analyze.py's sigma on a sample grid instead of every pixel;
//   * colour delta - how far the output's colour sits from the source's.
//
// Everything is in 8-bit codes of full-range BT.709 Y'CbCr, whatever layout either
// side arrived in, so an NV12 capture and a BGRA one report the same numbers for
// the same picture. They describe a render and are recorded in its receipt; they
// are never part of what identifies one.
struct TemporalMetrics {
    uint64_t frames{};          // output frames sampled against their source
    uint64_t pairs{};           // consecutive in-shot pairs the warping error covers
    uint32_t shots{};           // shots long enough to carry a temporal sigma
    double sourceWarpError{};   // mean |Y(t) - warp(Y(t-1))| of the source
    double outputWarpError{};   // the same, of the output
    double sourceSigma{};       // frame-weighted mean per-sample temporal sigma
    double outputSigma{};
    double lumaShift{};         // mean signed Y, output minus source
    double colorDelta{};        // mean length of the (dY, dCb, dCr) difference

    bool Measured() const noexcept { return frames > 0; }
    double FlickerAdded() const noexcept { return outputWarpError - sourceWarpError; }
    double SigmaAdded() const noexcept { return outputSigma - sourceSigma; }
    friend bool operator==(const TemporalMetrics&, const TemporalMetrics&) = default;
};

namespace temporal_metrics {

// One frame reduced to a grid of cells: the mean of four stratified taps per cell,
// the scheme TemporalGuideGenerator::DownsampleLuma uses, so a cell here is the cell
// the guide generator solved a vector for and that vector moves it exactly.
struct Plane {
    std::vector<float> y, cb, cr;
    uint32_t width{}, height{};
};

// Shots shorter than this carry no sigma, as in analyze.py: a standard deviation
// over two samples is mostly the one difference between them.
inline constexpr uint32_t kMinShotFrames = 3;

inline void YCbCrFromBgra(const uint8_t* p, float& y, float& cb, float& cr) noexcept
{
    const float b = p[0], g = p[1], r = p[2];
    y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    cb = (b - y) / 1.8556f;
    cr = (r - y) / 1.5748f;
}

// How a captured frame stores its samples. NV12 and P010 are the GPU capture's
// two planar forms (D3D12Renderer's CaptureFormat), BT.709 limited range; P010
// holds the 10-bit code in the top bits of each 16-bit sample.
enum class SampleLayout { Bgra, Nv12, P010 };

constexpr SampleLayout SampleLayoutFor(PixelLayout layout)
{
    return layout == PixelLayout::Nv12 ? SampleLayout::Nv12 : SampleLayout::Bgra;
}

// The sampler itself, over row accessors: `luma(y)` is row y of the Y plane (or of
// the packed BGRA frame) and `chroma(y)` row y of the interleaved half-width UV
// plane, either null when that row is not available. Each grid cell averages four
// point samples - the cell's first row and column and its middle ones - so what a
// frame contributes is fixed by the grid, which is what lets a capture that is
// never read back whole hand over just those rows (SampledRows) and get the same
// plane, sample for sample, as the whole frame gives.
template <class LumaRow, class ChromaRow>
inline bool SampleRows(SampleLayout layout, uint32_t width, uint32_t height, uint32_t gridWidth,
                       uint32_t gridHeight, LumaRow&& luma, ChromaRow&& chroma, Plane& out)
{
    if (!width || !height || !gridWidth || !gridHeight) return false;
    if (layout != SampleLayout::Bgra && ((width | height) & 1u)) return false;
    const size_t cells = size_t(gridWidth) * gridHeight;
    out.width = gridWidth;
    out.height = gridHeight;
    out.y.assign(cells, 0.0f);
    out.cb.assign(cells, 0.0f);
    out.cr.assign(cells, 0.0f);
    for (uint32_t gy = 0; gy < gridHeight; ++gy) {
        const uint32_t y0 = uint32_t((uint64_t(gy) * height) / gridHeight);
        const uint32_t y1 = std::max(y0 + 1, uint32_t((uint64_t(gy + 1) * height) / gridHeight));
        const uint32_t ys[2] = {y0, std::min(height - 1, (y0 + y1) / 2)};
        for (uint32_t gx = 0; gx < gridWidth; ++gx) {
            const uint32_t x0 = uint32_t((uint64_t(gx) * width) / gridWidth);
            const uint32_t x1 = std::max(x0 + 1, uint32_t((uint64_t(gx + 1) * width) / gridWidth));
            const uint32_t xs[2] = {x0, std::min(width - 1, (x0 + x1) / 2)};
            float sy = 0.0f, scb = 0.0f, scr = 0.0f;
            for (const uint32_t py : ys) {
                const uint8_t* row = luma(py);
                const uint8_t* uvRow = layout == SampleLayout::Bgra ? row : chroma(py / 2);
                if (!row || !uvRow) return false;
                for (const uint32_t px : xs) {
                    float ly, lcb, lcr;
                    if (layout == SampleLayout::Nv12) {
                        const uint8_t* uv = uvRow + size_t(px / 2) * 2;
                        ly = (float(row[px]) - 16.0f) * (255.0f / 219.0f);
                        lcb = (float(uv[0]) - 128.0f) * (255.0f / 224.0f);
                        lcr = (float(uv[1]) - 128.0f) * (255.0f / 224.0f);
                    } else if (layout == SampleLayout::P010) {
                        // The 10-bit code over four is the 8-bit code it refines, so a
                        // P010 capture reports on the NV12 scale; exact for any 8-bit
                        // value the 10-bit one holds.
                        const auto code = [](const uint8_t* sample) {
                            return float(uint32_t(sample[0] | (sample[1] << 8)) >> 6) * 0.25f;
                        };
                        const uint8_t* uv = uvRow + size_t(px / 2) * 4;
                        ly = (code(row + size_t(px) * 2) - 16.0f) * (255.0f / 219.0f);
                        lcb = (code(uv) - 128.0f) * (255.0f / 224.0f);
                        lcr = (code(uv + 2) - 128.0f) * (255.0f / 224.0f);
                    } else {
                        YCbCrFromBgra(row + size_t(px) * 4u, ly, lcb, lcr);
                    }
                    sy += ly; scb += lcb; scr += lcr;
                }
            }
            const size_t cell = size_t(gy) * gridWidth + gx;
            out.y[cell] = sy * 0.25f;
            out.cb[cell] = scb * 0.25f;
            out.cr[cell] = scr * 0.25f;
        }
    }
    return true;
}

// Bytes of one row of the luma plane, which is also one row of the interleaved
// chroma plane, or of a packed BGRA frame.
constexpr size_t SampleRowBytes(SampleLayout layout, uint32_t width)
{
    return size_t(width) * (layout == SampleLayout::Bgra ? 4u : layout == SampleLayout::P010 ? 2u : 1u);
}

// Samples a whole frame: tightly packed BGRA, NV12 or P010 in BT.709 limited range,
// onto a gridWidth x gridHeight plane. False when the buffer is too small for the
// layout.
inline bool Sample(std::span<const uint8_t> pixels, SampleLayout layout, uint32_t width, uint32_t height,
                   uint32_t gridWidth, uint32_t gridHeight, Plane& out)
{
    const size_t row = SampleRowBytes(layout, width);
    const size_t lumaBytes = row * height;
    const size_t bytes = layout == SampleLayout::Bgra ? lumaBytes : lumaBytes + row * (height / 2u);
    if (pixels.size() < bytes || ((layout != SampleLayout::Bgra) && ((width | height) & 1u))) return false;
    const uint8_t* data = pixels.data();
    return SampleRows(layout, width, height, gridWidth, gridHeight,
                      [&](uint32_t y) { return data + row * y; },
                      [&](uint32_t y) { return data + lumaBytes + row * y; }, out);
}

inline bool Sample(std::span<const uint8_t> pixels, PixelLayout layout, uint32_t width, uint32_t height,
                   uint32_t gridWidth, uint32_t gridHeight, Plane& out)
{
    if (pixels.size() < PixelLayoutFrameBytes(layout, width, height)) return false;
    return Sample(pixels, SampleLayoutFor(layout), width, height, gridWidth, gridHeight, out);
}

// The rows SampleRows reads from a planar frame of `height` rows on a grid
// `gridHeight` cells tall: luma rows ascending, and the chroma rows they use.
struct RowSet {
    std::vector<uint32_t> luma, chroma;
    friend bool operator==(const RowSet&, const RowSet&) = default;
};

inline RowSet SampledRows(uint32_t height, uint32_t gridHeight)
{
    RowSet rows;
    if (!height || !gridHeight) return rows;
    for (uint32_t gy = 0; gy < gridHeight; ++gy) {
        const uint32_t y0 = uint32_t((uint64_t(gy) * height) / gridHeight);
        const uint32_t y1 = std::max(y0 + 1, uint32_t((uint64_t(gy + 1) * height) / gridHeight));
        rows.luma.push_back(y0);
        rows.luma.push_back(std::min(height - 1, (y0 + y1) / 2));
    }
    std::sort(rows.luma.begin(), rows.luma.end());
    rows.luma.erase(std::unique(rows.luma.begin(), rows.luma.end()), rows.luma.end());
    for (const uint32_t y : rows.luma) rows.chroma.push_back(y / 2);
    rows.chroma.erase(std::unique(rows.chroma.begin(), rows.chroma.end()), rows.chroma.end());
    return rows;
}

// A frame reduced to those rows: every luma row of `rows` in order, then every
// chroma row, each tightly packed. The direct NVENC capture reads back this and
// not the frame (D3D12Renderer::CopyCaptureView).
constexpr size_t RowPayloadBytes(SampleLayout layout, uint32_t width, size_t lumaRows, size_t chromaRows)
{
    return SampleRowBytes(layout, width) * (lumaRows + chromaRows);
}

// SampleRows over such a payload: the same plane the whole frame gives, as long
// as `rows` is SampledRows for this height and grid. False otherwise.
inline bool SampleRowPayload(std::span<const uint8_t> payload, SampleLayout layout, uint32_t width,
                             uint32_t height, uint32_t gridWidth, uint32_t gridHeight, const RowSet& rows,
                             Plane& out)
{
    if (layout == SampleLayout::Bgra) return false;
    const size_t row = SampleRowBytes(layout, width);
    if (payload.size() < RowPayloadBytes(layout, width, rows.luma.size(), rows.chroma.size())) return false;
    const uint8_t* data = payload.data();
    const uint8_t* chromaData = data + row * rows.luma.size();
    const auto find = [](const std::vector<uint32_t>& list, uint32_t y) -> ptrdiff_t {
        const auto at = std::lower_bound(list.begin(), list.end(), y);
        return at != list.end() && *at == y ? at - list.begin() : -1;
    };
    return SampleRows(layout, width, height, gridWidth, gridHeight,
                      [&](uint32_t y) -> const uint8_t* {
                          const ptrdiff_t index = find(rows.luma, y);
                          return index < 0 ? nullptr : data + row * size_t(index);
                      },
                      [&](uint32_t y) -> const uint8_t* {
                          const ptrdiff_t index = find(rows.chroma, y);
                          return index < 0 ? nullptr : chromaData + row * size_t(index);
                      }, out);
}

inline float Bilinear(const std::vector<float>& plane, uint32_t width, uint32_t height, float x, float y) noexcept
{
    const int x0 = std::clamp(int(std::floor(x)), 0, int(width) - 1);
    const int y0 = std::clamp(int(std::floor(y)), 0, int(height) - 1);
    const int x1 = std::min(x0 + 1, int(width) - 1), y1 = std::min(y0 + 1, int(height) - 1);
    const float tx = std::clamp(x - float(x0), 0.0f, 1.0f), ty = std::clamp(y - float(y0), 0.0f, 1.0f);
    const auto at = [&](int px, int py) { return plane[size_t(py) * width + size_t(px)]; };
    return (at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx) * (1.0f - ty) +
           (at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx) * ty;
}

// Feeds frames in presentation order and accumulates TemporalMetrics.
class Accumulator {
public:
    // `motion` holds, per cell, the displacement from this frame back to where its
    // content was in the previous one, in cells, x and y interleaved - the guide
    // generator's own field - or is empty for none. `newShot` says this frame began
    // a new history (a first frame, a cut, a seek): the pair across it is not a
    // pair, and the shot the sigma is taken over ends there.
    void Add(const Plane& source, const Plane& output, std::span<const float> motion, bool newShot)
    {
        if (source.width != output.width || source.height != output.height || source.y.empty()) return;
        const size_t cells = source.y.size();
        const bool haveMotion = motion.size() == cells * 2;
        if (newShot || previousSource_.width != source.width || previousSource_.height != source.height) FlushShot();
        else if (!previousSource_.y.empty()) {
            // The warping error of this pair, both sides moved by the same vectors.
            double sourceError = 0.0, outputError = 0.0;
            size_t counted = 0;
            for (uint32_t gy = 0; gy < source.height; ++gy) {
                for (uint32_t gx = 0; gx < source.width; ++gx) {
                    const size_t cell = size_t(gy) * source.width + gx;
                    const float px = float(gx) + (haveMotion ? motion[cell * 2] : 0.0f);
                    const float py = float(gy) + (haveMotion ? motion[cell * 2 + 1] : 0.0f);
                    // Content that came from outside the frame has no previous sample.
                    if (px < 0.0f || py < 0.0f || px > float(source.width - 1) || py > float(source.height - 1))
                        continue;
                    sourceError += std::abs(source.y[cell] -
                                            Bilinear(previousSource_.y, source.width, source.height, px, py));
                    outputError += std::abs(output.y[cell] -
                                            Bilinear(previousOutput_.y, source.width, source.height, px, py));
                    ++counted;
                }
            }
            if (counted) {
                sourceWarp_ += sourceError / double(counted);
                outputWarp_ += outputError / double(counted);
                ++metrics_.pairs;
            }
        }
        // The shot's running sums, and the colour of this frame against its source.
        if (shotSource_.size() != cells) {
            shotSource_.assign(cells, {});
            shotOutput_.assign(cells, {});
        }
        double luma = 0.0, colour = 0.0;
        for (size_t cell = 0; cell < cells; ++cell) {
            shotSource_[cell].Add(source.y[cell]);
            shotOutput_[cell].Add(output.y[cell]);
            const double dy = double(output.y[cell]) - source.y[cell];
            const double dcb = double(output.cb[cell]) - source.cb[cell];
            const double dcr = double(output.cr[cell]) - source.cr[cell];
            luma += dy;
            colour += std::sqrt(dy * dy + dcb * dcb + dcr * dcr);
        }
        lumaShift_ += luma / double(cells);
        colorDelta_ += colour / double(cells);
        ++shotFrames_;
        ++metrics_.frames;
        previousSource_ = source;
        previousOutput_ = output;
    }

    // Closes the open shot and returns the metrics so far. Safe to call again.
    TemporalMetrics Finish()
    {
        FlushShot();
        TemporalMetrics result = metrics_;
        if (result.frames) {
            result.lumaShift = lumaShift_ / double(result.frames);
            result.colorDelta = colorDelta_ / double(result.frames);
        }
        if (result.pairs) {
            result.sourceWarpError = sourceWarp_ / double(result.pairs);
            result.outputWarpError = outputWarp_ / double(result.pairs);
        }
        if (sigmaFrames_) {
            result.sourceSigma = sourceSigma_ / double(sigmaFrames_);
            result.outputSigma = outputSigma_ / double(sigmaFrames_);
        }
        return result;
    }

private:
    // Double, as analyze.py found: at float the variance of a steady shot disappears
    // into the rounding of E[x^2] - E[x]^2.
    struct Moments {
        double sum{}, squares{};
        void Add(double value) noexcept { sum += value; squares += value * value; }
        double Sigma(uint32_t frames) const noexcept
        {
            const double mean = sum / frames;
            return std::sqrt(std::max(0.0, squares / frames - mean * mean));
        }
    };

    void FlushShot()
    {
        if (shotFrames_ >= kMinShotFrames && !shotSource_.empty()) {
            double source = 0.0, output = 0.0;
            for (size_t cell = 0; cell < shotSource_.size(); ++cell) {
                source += shotSource_[cell].Sigma(shotFrames_);
                output += shotOutput_[cell].Sigma(shotFrames_);
            }
            // Frame-weighted, so a long shot counts for its length.
            sourceSigma_ += source / double(shotSource_.size()) * shotFrames_;
            outputSigma_ += output / double(shotSource_.size()) * shotFrames_;
            sigmaFrames_ += shotFrames_;
            ++metrics_.shots;
        }
        std::fill(shotSource_.begin(), shotSource_.end(), Moments{});
        std::fill(shotOutput_.begin(), shotOutput_.end(), Moments{});
        shotFrames_ = 0;
        previousSource_ = {};
        previousOutput_ = {};
    }

    TemporalMetrics metrics_{};
    Plane previousSource_, previousOutput_;
    std::vector<Moments> shotSource_, shotOutput_;
    uint32_t shotFrames_{};
    uint64_t sigmaFrames_{};
    double sourceWarp_{}, outputWarp_{}, sourceSigma_{}, outputSigma_{}, lumaShift_{}, colorDelta_{};
};

} // namespace temporal_metrics
