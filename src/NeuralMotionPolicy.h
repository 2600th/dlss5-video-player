#pragma once

#include <optional>
#include <string>

// Whether a neural render at the source's size puts the hardware flow field through
// the resolve pass's zero-motion test (NvofResolveShader.h), and the cache-key term
// that choice carries.
//
// The flow engine does not answer zero for two identical frames: on a held frame it
// reports a fixed sub-pixel field on about half the pixels (w4-sr,
// docs/measurements/sr-quality-20260924). A Super Resolution session has taken the
// test since that finding, unconditionally - the renderer turns it on for every
// preserve-source session, which includes the neural job's SR carrier below 100 %
// processing scale. This is the other half: the DLAA carrier a neural render at
// source size runs on, whose motion guide RenoDX's feature 18 reads.
//
// On, measured in docs/measurements/neural-mv-gate-20260924/REPORT.md: on a held
// frame the render with the test is byte-identical to one with the motion guide
// switched off - the exact answer - where without it the model's output crept
// (per-pixel temporal sigma p99 2.15 -> 1.41, sharpness against the source
// 1.12 -> 1.07 by the last frames, PSNR 36.34 -> 36.50 dB). On the NR-processed
// real clips and a 0.5 px/frame pan it lowered sigma, flicker or warp error with
// PSNR within 0.1 dB and colour within 0.02 dE. The one cost is on a synthetic
// near/far pan: +0.24 dE, -0.007 SSIM, a smoother-than-source picture.
inline constexpr bool kNeuralZeroMotionTest = true;

// What a render at the source's size runs with: the shipped choice, unless the
// benchmark's `--zero-motion-test 0|1` (GuideFiles.h) forced one arm of the A/B.
// The player can never set the override, so every cached render is made with the
// shipped choice and keyed by NeuralMotionIdentityTerm of it.
inline constexpr bool NeuralZeroMotionTest(std::optional<bool> benchmarkOverride) noexcept
{
    return benchmarkOverride.value_or(kNeuralZeroMotionTest);
}

// The cache-key term, appended to the pipeline identity by the caller. Empty with
// the test off, which is the field every render before it was made with; with it
// on, a render is a different picture on every frame the engine reported motion
// the pair did not have, and must never be served for one made without it.
inline std::string NeuralMotionIdentityTerm(bool zeroMotionTest)
{
    return zeroMotionTest ? std::string("|mv-zero-test-v1") : std::string{};
}
