#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace playback_timing {

// Both of these deliberately ignore the playback clock: what the timeline and a
// pause show is the frame the player last presented, never where the clock has
// drifted to. The clock argument used to be passed and discarded, so callers
// computed a position for nothing.
inline double TimelinePosition(bool dragging,
                               double previewPosition,
                               bool seekPending,
                               double pendingPosition,
                               double lastPresentedPosition)
{
    if (dragging) return previewPosition;
    if (seekPending) return pendingPosition;
    return lastPresentedPosition;
}

inline double PausePosition(double lastPresentedPosition)
{
    return lastPresentedPosition;
}

inline double LateFrameThreshold(double frameDuration)
{
    return std::max(0.0, frameDuration) * 1.5;
}

// Rendering cost per frame is a fixed part plus a part proportional to pixel
// count. Measured on the reference GPU (RTX 5090, driver 616.64) by timing
// segment arrivals, which excludes job startup: 12.50 ms/frame at 1920x1080,
// 16.60 at 2560x1440 and 28.07 at 3840x2160. A least-squares fit of those three
// points is 7.35 ms + 2.50 ms per megapixel and reproduces them to within
// 0.06 ms. The fixed part is the guide pass, the DLSS evaluate and the capture's
// fence wait; the proportional part is the readback and the pixel work.
//
// A session can only follow live playback while that cost fits inside one frame
// interval. At these rates 4K30 has about 19% of the interval to spare and 4K60
// does not fit at all.
//
// These constants are the seed for a machine that has not measured itself yet,
// and 0.17.0 left them behind: the same GPU and the same clips now cost 8.4
// ms/frame at 1080p, 15.4 at 1440p and 42.0 at 4K (docs/VERIFICATION-2026-09-10
// -RTX5090.md), so the seed reads ~1.5x high below 1440p. They are kept because
// those three points no longer fit one line - the 4K clip is a 6.3 Mbit/s
// re-encode whose decode and encode set its pace, and a fit through it puts the
// fixed term below zero - and because a seed that overstates cost asks before a
// marginal session instead of dropping frames in it. One session replaces the
// seed with this machine's own pace at that geometry.
inline constexpr double kNeuralFrameFixedMs = 7.35;
inline constexpr double kNeuralMillisecondsPerMegapixel = 2.50;

// Other GPUs run the same pipeline at a different speed, but not at a
// different speed uniformly: a 0.16.0 session measured 0.95x the reference cost
// at 1080p, 1.04x at 1440p and 1.53x at 4K on the reference GPU itself, so one
// scalar taken at 1080p let a 4K30 session start and drop 848 of 869 frames.
// On 0.17.0 the same three geometries measure 0.67x, 0.93x and 1.48x, which is
// the same lesson with wider spread. The player therefore keeps one measured
// pace per source geometry and predicts from them:
//   - a sample at the requested geometry is used as is;
//   - two or more geometries fit their own fixed + per-megapixel line;
//   - one sample extrapolates conservatively: the larger of the reference
//     shape scaled through the sample and a purely proportional cost;
//   - no sample falls back to the generation's prior scale on the reference
//     model, and a prior of 0 means "unknown".
struct RenderPaceModel {
    double fixedMs = kNeuralFrameFixedMs;
    double msPerMegapixel = kNeuralMillisecondsPerMegapixel;
    double MsPerFrame(uint32_t width, uint32_t height) const
    {
        return fixedMs + (double(width) * double(height)) / 1'000'000.0 * msPerMegapixel;
    }
};

inline double Megapixels(uint32_t width, uint32_t height)
{
    return (double(width) * double(height)) / 1'000'000.0;
}

// Ratio of a measured per-frame cost to what the reference model predicts for
// that geometry. Returns 0 when nothing usable was measured.
inline double RenderPaceScale(double measuredMsPerFrame, uint32_t width, uint32_t height,
                              RenderPaceModel reference = {})
{
    const double predicted = reference.MsPerFrame(width, height);
    if (!(measuredMsPerFrame > 0.0) || !(predicted > 0.0) || width == 0 || height == 0) return 0.0;
    return measuredMsPerFrame / predicted;
}

struct RenderPaceSample {
    uint32_t width{};
    uint32_t height{};
    double msPerFrame{};
    friend bool operator==(const RenderPaceSample&, const RenderPaceSample&) = default;
};

// Measured paces of one GPU, at most one per geometry, newest replacing oldest.
struct RenderPaceProfile {
    static constexpr size_t kMaxSamples = 6;
    std::vector<RenderPaceSample> samples;

    void Record(RenderPaceSample sample)
    {
        if (sample.width == 0 || sample.height == 0 || !(sample.msPerFrame > 0.0)) return;
        for (RenderPaceSample& existing : samples) {
            if (existing.width == sample.width && existing.height == sample.height) {
                existing.msPerFrame = sample.msPerFrame;
                return;
            }
        }
        if (samples.size() == kMaxSamples) samples.erase(samples.begin());
        samples.push_back(sample);
    }
};

// Predicted cost of one frame at `width` x `height`, or 0 when unknown.
inline double PredictRenderMs(const RenderPaceProfile& profile, uint32_t width, uint32_t height,
                              double priorScale, RenderPaceModel reference = {})
{
    const double megapixels = Megapixels(width, height);
    if (!(megapixels > 0.0)) return 0.0;

    // Least squares over distinct geometries; also finds an exact match.
    double sumX = 0.0, sumY = 0.0, sumXX = 0.0, sumXY = 0.0;
    size_t count = 0;
    const RenderPaceSample* single = nullptr;
    for (const RenderPaceSample& sample : profile.samples) {
        if (!(sample.msPerFrame > 0.0)) continue;
        if (sample.width == width && sample.height == height) return sample.msPerFrame;
        const double x = Megapixels(sample.width, sample.height);
        if (!(x > 0.0)) continue;
        sumX += x; sumY += sample.msPerFrame; sumXX += x * x; sumXY += x * sample.msPerFrame;
        ++count;
        single = &sample;
    }
    if (count >= 2) {
        const double denominator = double(count) * sumXX - sumX * sumX;
        if (denominator > 0.0) {
            const double slope = (double(count) * sumXY - sumX * sumY) / denominator;
            const double intercept = (sumY - slope * sumX) / double(count);
            // A line that goes down with resolution is measurement noise, not a
            // model; fall through to the conservative single-sample rule.
            if (slope > 0.0) return std::max(0.0, intercept) + slope * megapixels;
        }
    }
    if (single) {
        const double sampleMegapixels = Megapixels(single->width, single->height);
        const double shaped = RenderPaceScale(single->msPerFrame, single->width, single->height, reference) *
                              reference.MsPerFrame(width, height);
        const double proportional = single->msPerFrame * megapixels / sampleMegapixels;
        return std::max(shaped, proportional);
    }
    return priorScale > 0.0 ? priorScale * reference.MsPerFrame(width, height) : 0.0;
}

struct LiveRenderForecast {
    double msPerFrame;      // predicted render cost of one frame
    double renderFps;       // frames the renderer can produce per second
    double sourceFps;       // frames playback consumes per second
    double realtimeRatio;   // renderFps / sourceFps; 1.0 means it exactly keeps up
    bool keepsUp;
    // False when nothing measured or modelled applies to this GPU, so `keepsUp`
    // is a default rather than a verdict. A caller that tells the user their card
    // was checked has to know the difference.
    bool measured;
};

// `priorScale` multiplies the reference cost when the profile has nothing to
// say: 1.0 is the reference GPU, 0 means the pace of this GPU is unknown, and
// an unknown pace never blocks the user on a guess.
inline LiveRenderForecast ForecastLiveRender(uint32_t width, uint32_t height, double sourceFps,
                                             const RenderPaceProfile& profile = {},
                                             double priorScale = 1.0,
                                             RenderPaceModel reference = {})
{
    LiveRenderForecast forecast{};
    forecast.sourceFps = sourceFps;
    const double msPerFrame = sourceFps > 0.0 ? PredictRenderMs(profile, width, height, priorScale, reference) : 0.0;
    if (!(msPerFrame > 0.0)) {
        // Nothing measurable to forecast: never block the user on a guess.
        forecast.keepsUp = true;
        return forecast;
    }
    forecast.measured = true;
    forecast.msPerFrame = msPerFrame;
    forecast.renderFps = 1000.0 / msPerFrame;
    forecast.realtimeRatio = forecast.renderFps / sourceFps;
    // A couple of percent either way is inside run-to-run noise; only warn when
    // the shortfall is real.
    forecast.keepsUp = forecast.realtimeRatio >= 0.98;
    return forecast;
}

} // namespace playback_timing
