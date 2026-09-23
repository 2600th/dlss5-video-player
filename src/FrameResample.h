#pragma once

#include "ParallelFor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Area-average downscale of a BGRA8 frame, for the processing-scale rungs: the
// frame the neural model is shown is the source reduced to 75% or 50%, and
// DLSS Super Resolution brings the result back to the source size.
//
// Area averaging rather than the renderer's bilinear sample because a
// reduction is where a point-sampling filter aliases: at 50% a bilinear tap
// reads 2 of every 4 source pixels and drops the other two, and fine detail -
// text, grain, fences - comes out as moire the model then treats as content.
// Each output pixel here is the exact coverage-weighted mean of the source
// pixels under it, which at exactly 50% is the plain 2x2 box.
//
// It averages the stored values, as every video scaler does with gamma-coded
// samples; the result is deterministic for a given input and size, whatever
// the thread count, because every output row is computed by the same
// arithmetic in the same order.
namespace frame_resample {

// Source pixels one output pixel covers along one axis: `first`, then `count`
// weights starting at `offset` in the axis' weight table. Weights sum to 1.
struct AxisSpan {
    uint32_t first{};
    uint32_t count{};
    uint32_t offset{};
};

struct Axis {
    std::vector<AxisSpan> spans;
    std::vector<float> weights;
};

// The coverage of [i * ratio, (i + 1) * ratio) over each source pixel, for a
// reduction from `source` to `target` samples (target <= source).
inline Axis BuildAxis(uint32_t source, uint32_t target)
{
    Axis axis;
    if (!source || !target || target > source) return axis;
    axis.spans.resize(target);
    const double ratio = double(source) / double(target);
    for (uint32_t index = 0; index < target; ++index) {
        const double begin = index * ratio;
        const double end = std::min(double(source), (index + 1) * ratio);
        const uint32_t first = uint32_t(std::floor(begin));
        const uint32_t last = std::min(source - 1, uint32_t(std::ceil(end)) - 1);
        AxisSpan& span = axis.spans[index];
        span.first = first;
        span.offset = uint32_t(axis.weights.size());
        for (uint32_t sample = first; sample <= last; ++sample) {
            const double covered = std::min(end, double(sample + 1)) - std::max(begin, double(sample));
            if (covered <= 0.0) continue;
            if (span.count == 0) span.first = sample;
            axis.weights.push_back(float(covered / ratio));
            ++span.count;
        }
    }
    return axis;
}

// A pool of its own: the job resamples on the decode-prefetch thread while the
// render thread fans the temporal guides out over the shared pool, and a
// second dispatcher into a busy pool runs serially rather than overlapping.
inline parallel_detail::WorkerPool& ResamplePool()
{
    static parallel_detail::WorkerPool pool(std::max<size_t>(1, parallel_detail::WorkerPool::DefaultWidth() / 2));
    return pool;
}

// `destination` is resized to targetWidth * targetHeight * 4 bytes. Returns
// false, leaving it untouched, for a size that is not a reduction or a source
// buffer that does not hold one full frame.
inline bool DownscaleBgraArea(const std::vector<uint8_t>& source, uint32_t sourceWidth,
                              uint32_t sourceHeight, uint32_t targetWidth, uint32_t targetHeight,
                              std::vector<uint8_t>& destination)
{
    if (!sourceWidth || !sourceHeight || !targetWidth || !targetHeight ||
        targetWidth > sourceWidth || targetHeight > sourceHeight ||
        source.size() != size_t(sourceWidth) * sourceHeight * 4u) return false;
    // Built per call: a few thousand weights, against a frame of millions of
    // pixels, and it keeps the function free of state a caller could share.
    const Axis columns = BuildAxis(sourceWidth, targetWidth);
    const Axis rows = BuildAxis(sourceHeight, targetHeight);
    destination.resize(size_t(targetWidth) * targetHeight * 4u);
    const size_t sourceStride = size_t(sourceWidth) * 4u;
    ParallelForRangesIn(ResamplePool(), targetHeight, 16, [&](size_t begin, size_t end) {
        // One output row's worth of horizontally reduced source rows, summed
        // with their vertical weights as they are produced.
        std::vector<float> accumulated(size_t(targetWidth) * 4u);
        for (size_t row = begin; row < end; ++row) {
            std::fill(accumulated.begin(), accumulated.end(), 0.0f);
            const AxisSpan& vertical = rows.spans[row];
            for (uint32_t tap = 0; tap < vertical.count; ++tap) {
                const float rowWeight = rows.weights[vertical.offset + tap];
                const uint8_t* line = source.data() + (vertical.first + tap) * sourceStride;
                for (uint32_t column = 0; column < targetWidth; ++column) {
                    const AxisSpan& horizontal = columns.spans[column];
                    float b = 0.0f, g = 0.0f, r = 0.0f, a = 0.0f;
                    for (uint32_t sample = 0; sample < horizontal.count; ++sample) {
                        const float weight = columns.weights[horizontal.offset + sample];
                        const uint8_t* pixel = line + size_t(horizontal.first + sample) * 4u;
                        b += weight * pixel[0]; g += weight * pixel[1];
                        r += weight * pixel[2]; a += weight * pixel[3];
                    }
                    float* sum = accumulated.data() + size_t(column) * 4u;
                    sum[0] += rowWeight * b; sum[1] += rowWeight * g;
                    sum[2] += rowWeight * r; sum[3] += rowWeight * a;
                }
            }
            uint8_t* out = destination.data() + row * size_t(targetWidth) * 4u;
            for (size_t channel = 0; channel < accumulated.size(); ++channel)
                out[channel] = uint8_t(std::clamp(std::lround(accumulated[channel]), 0L, 255L));
        }
    });
    return true;
}

} // namespace frame_resample
