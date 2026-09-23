#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// A supplied, smoothed exposure for the DLSS evaluate, in place of the feature's own
// AutoExposure.
//
// DLSS reads an exposure: either one the application supplies in a 1x1 texture, or one
// it meters itself when the feature is created with AutoExposure. NVIDIA's guide asks
// for the former whenever the application knows it, and a neural renderer driven off
// the auto path has been reported (OptiScaler-DLSSNR PR #77) to let its white point
// drift under lighting changes. On video that is a candidate cause of brightness
// pumping, and of a slow recovery after a cut, because nothing tells an auto meter
// that the shot changed.
//
// This supplies the exposure an auto meter would compute - middle grey over the
// frame's log-average luminance - with two differences that are the point of it:
//  * it is smoothed over time, as libplacebo smooths its detected peak
//    (peak_smoothing_period, default 20 frames): a one-pole low-pass in the log
//    domain, so a flash or a brief dark frame moves it a little rather than all the
//    way;
//  * the smoothing restarts on every history reset - a scene cut the temporal guides
//    classified (ClassifySceneCut), a seek, the first frame - so a new shot is metered
//    on its own at once instead of inheriting the last shot's brightness.
//
// The meter reads the temporal guide generator's analysis grid, the per-cell luma it
// already computes for the scene-cut test, so the frame is metered at no extra cost and
// by exactly the evidence that decided whether it was a cut. The value is uploaded into
// the 1x1 exposure texture the evaluate reads (D3D12Renderer, SetSuppliedExposure).
//
// Off by default and a cache-key term, on the A/B in docs/measurements/exposure-ab-20260923/:
// on this runtime (DLSS-NR 310.8.0 through RenoDX 6.5.3) every real-footage clip rendered
// bit-identically with it on, and the synthetic clips it did change moved the wrong way
// on average - added flicker +0.0023 codes, added sigma +0.0048, dE +0.025 - so it buys
// nothing yet. It stays because a runtime that reads the exposure would change that.
namespace exposure {

// Middle grey, the key every auto-exposure meter targets.
inline constexpr float kKey = 0.18f;
// Four stops either way. Video is already exposed for display, so a meter that asks
// for more is metering a black or a white frame, not a scene.
inline constexpr float kMinExposure = 1.0f / 16.0f;
inline constexpr float kMaxExposure = 16.0f;
// The luminance floor inside the log, so a black cell cannot pull the mean to -inf.
inline constexpr float kFloor = 1.0f / 1024.0f;
// libplacebo's peak_smoothing_period default, in frames.
inline constexpr double kSmoothingPeriodFrames = 20.0;

// sRGB's decoding curve: the grid holds gamma-encoded luma in [0, 1].
inline float LinearFromEncoded(float encoded)
{
    const float v = std::clamp(encoded, 0.0f, 1.0f);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

// Log-average (geometric mean) linear luminance of `count` encoded cells; the floor
// when there are none.
inline float MeterLogAverage(const float* cells, size_t count)
{
    if (!cells || !count) return kFloor;
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i)
        sum += std::log(std::max(LinearFromEncoded(cells[i]), kFloor));
    return static_cast<float>(std::exp(sum / double(count)));
}

// Exposure that brings a meter reading to middle grey, bounded.
inline float ExposureFor(float meter)
{
    return std::clamp(kKey / std::max(meter, kFloor), kMinExposure, kMaxExposure);
}

// Per-frame blend weight of the one-pole low-pass: 1 - e^(-1/period).
inline double SmoothingWeight()
{
    return 1.0 - std::exp(-1.0 / kSmoothingPeriodFrames);
}

// The smoothed meter. Update once per submitted frame: a reset restarts it at the
// frame's own reading, a re-submission of the frame it last saw (the first-frame
// evidence loop presents one frame many times) leaves it alone, and every new frame
// moves it the smoothing weight of the way towards the new reading, in log units.
class Smoother {
public:
    float Update(float meter, uint64_t frameNumber, bool reset)
    {
        const double reading = std::log(std::max(meter, kFloor));
        if (reset || !primed_) {
            smoothed_ = reading;
            primed_ = true;
        } else if (frameNumber != lastFrame_) {
            smoothed_ += SmoothingWeight() * (reading - smoothed_);
        }
        lastFrame_ = frameNumber;
        return Exposure();
    }
    float Exposure() const
    {
        return primed_ ? ExposureFor(static_cast<float>(std::exp(smoothed_))) : 1.0f;
    }
    void Reset() { primed_ = false; }

private:
    double smoothed_{};
    uint64_t lastFrame_{};
    bool primed_{};
};

} // namespace exposure
