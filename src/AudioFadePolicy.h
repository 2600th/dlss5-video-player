#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

// The short ramps that keep a seek, a pause and a resume from clicking.
//
// A seek tears the stream down mid-waveform and the next one starts
// mid-waveform, so the endpoint is handed a step from some arbitrary sample
// value to zero and back to another. That step is broadband - it is a
// discontinuity, and its spectrum is everything - which is what a click is.
// It lands on the controls a viewer uses most.
//
// A raised cosine rather than a straight line. A linear ramp removes the step
// in the signal but leaves one in its first derivative at both ends, which is
// audible as a softer tick at the same moments. The raised cosine has zero
// slope at both ends, so there is nothing to hear at either join, and out-ramp
// plus in-ramp sum to exactly one - the standard equal-gain crossfade shape.
namespace audio_fade {

// Four milliseconds: the middle of the 2-5 ms range that removes the
// discontinuity without the ramp itself becoming an audible amplitude
// envelope. At 48 kHz it is 192 frames, which is under a fifth of the
// endpoint's own buffer, so a fade never dominates the queue it lives in.
inline constexpr double kFadeSeconds = 0.004;

// Below this the last frame is already silence and there is no step to
// remove. Far under the least significant bit of any format a listener has.
inline constexpr float kSilenceFloor = 1e-6f;

inline uint32_t FrameCount(uint32_t sampleRate)
{
    if (!sampleRate) return 0;
    return static_cast<uint32_t>(double(sampleRate) * kFadeSeconds + 0.5);
}

// 0 at the start of the ramp, 1 at its end and after it. A zero-length fade
// is "no fade", never "silence": returning 0 there would mute the stream.
inline float RampInGain(uint64_t framesIntoFade, uint32_t fadeFrames)
{
    if (!fadeFrames || framesIntoFade >= fadeFrames) return 1.0f;
    constexpr double pi = 3.14159265358979323846;
    const double position = double(framesIntoFade) / double(fadeFrames);
    return static_cast<float>(0.5 * (1.0 - std::cos(pi * position)));
}

// The reflection, so a fade out running into a fade in has no step anywhere.
inline float RampOutGain(uint64_t framesIntoFade, uint32_t fadeFrames)
{
    return 1.0f - RampInGain(framesIntoFade, fadeFrames);
}

// Opens `frames` interleaved frames in place, continuing a ramp that
// `framesFaded` frames of are already done and advancing it. Every channel of
// a frame takes the same gain - a per-channel ramp would swing the stereo
// image while it ran.
inline void ApplyFadeIn(float* interleaved, uint32_t frames, uint16_t channels,
                        uint32_t fadeFrames, uint64_t& framesFaded)
{
    if (!interleaved || !channels) return;
    for (uint32_t frame = 0; frame < frames; ++frame, ++framesFaded) {
        if (framesFaded >= fadeFrames) {
            // The rest of the block is past the ramp. Count it and leave it
            // alone rather than multiplying by a one.
            framesFaded += uint64_t(frames) - frame;
            return;
        }
        const float gain = RampInGain(framesFaded, fadeFrames);
        float* const start = interleaved + size_t(frame) * channels;
        for (uint16_t channel = 0; channel < channels; ++channel) start[channel] *= gain;
    }
}

// The tail written after the last real frame, decaying that frame to silence.
//
// It cannot be built from source that has not been decoded - by the time a
// seek is known about, the samples after it will never exist - so it decays
// the last frame the endpoint was actually handed. That makes the first tail
// frame identical to the one before it, which is the whole point: starting
// anywhere else would insert the step the tail exists to remove.
inline void BuildFadeOutTail(const float* lastFrame, uint16_t channels, uint32_t fadeFrames,
                             std::vector<float>& tail)
{
    tail.clear();
    if (!lastFrame || !channels || !fadeFrames) return;
    bool audible = false;
    for (uint16_t channel = 0; channel < channels; ++channel)
        if (std::abs(lastFrame[channel]) > kSilenceFloor) { audible = true; break; }
    if (!audible) return;

    tail.resize(size_t(fadeFrames) * channels);
    for (uint32_t frame = 0; frame < fadeFrames; ++frame) {
        const float gain = RampOutGain(frame, fadeFrames);
        float* const start = tail.data() + size_t(frame) * channels;
        for (uint16_t channel = 0; channel < channels; ++channel) start[channel] = lastFrame[channel] * gain;
    }
}

} // namespace audio_fade
