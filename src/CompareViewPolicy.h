#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>

// The decisions behind the player's comparison view that need no window and no GPU:
// what a press on the picture means, how the old strength and blend settings become
// the one Mix, and how a GDI-drawn tag becomes the premultiplied texel the compositor
// blends. main.cpp owns the Win32 side of each and PSPresentScaled the pixels.

namespace compare_gesture {

// A press on the picture is one of three things, told apart by what the pointer does
// next. It has to be, because all three already had owners: a click in split or wipe
// puts the divider under the pointer (and a drag moves it), a drag on the timeline
// scrubs - that one is in the control strip, not here - and holding still is the
// press-and-hold A/B that Topaz and NVIDIA ICAT taught people to expect. So:
//
//  * press            - in split/wipe the divider jumps to the pointer, as it always did
//  * move past kSlop  - a drag: the divider follows, or the view pans (see Press)
//  * hold kHoldMs     - the original replaces the picture until release, or until the
//                       pointer moves past kSlop, which turns the hold into the drag
//
// The divider follows the pointer below the slop too; a divider that ignored the first
// few pixels of a deliberate drag would feel stuck.
inline constexpr unsigned kHoldMs = 150;
inline constexpr int kSlopDip = 4;

enum class Phase { Idle, Pressed, Peeking, Dragging };

struct State {
    Phase phase = Phase::Idle;
    POINT origin{};
    bool divider = false;  // the press landed in split or wipe
    bool pan = false;      // a drag pans the zoomed view instead
};

struct Step {
    bool setDivider = false;  // put the divider at the pointer
    bool startPeek = false;
    bool endPeek = false;
    bool pan = false;         // move the view by the pointer's delta
};

inline bool Moved(POINT origin, POINT point, int slop)
{
    return std::abs(point.x - origin.x) > slop || std::abs(point.y - origin.y) > slop;
}

inline Step Press(State& state, POINT point, bool dividerMode, bool panAvailable)
{
    state = State{Phase::Pressed, point, dividerMode, panAvailable && !dividerMode};
    Step step;
    step.setDivider = dividerMode;
    return step;
}

inline Step Move(State& state, POINT point, int slop)
{
    Step step;
    switch (state.phase) {
    case Phase::Idle:
        return step;
    case Phase::Pressed:
        step.setDivider = state.divider;
        if (Moved(state.origin, point, slop)) {
            state.phase = Phase::Dragging;
            step.pan = state.pan;
        }
        return step;
    case Phase::Peeking:
        if (!Moved(state.origin, point, slop)) return step;
        state.phase = Phase::Dragging;
        step.endPeek = true;
        step.setDivider = state.divider;
        step.pan = state.pan;
        return step;
    case Phase::Dragging:
        step.setDivider = state.divider;
        step.pan = state.pan;
        return step;
    }
    return step;
}

// The hold timer fired. Only a press that has not become anything else turns into a
// peek; a drag that paused for a moment is still a drag.
inline Step HoldElapsed(State& state)
{
    Step step;
    if (state.phase != Phase::Pressed) return step;
    state.phase = Phase::Peeking;
    step.startPeek = true;
    return step;
}

inline Step Release(State& state)
{
    Step step;
    step.endPeek = state.phase == Phase::Peeking;
    state = State{};
    return step;
}

} // namespace compare_gesture

namespace compare_settings {

// Modes as persisted in [Comparison] Mode; the numbers are ComparisonMode's.
inline constexpr int kNeural = 0, kOriginal = 1, kBlend = 2, kSplit = 3, kWipe = 4;

struct Loaded {
    float mix = 1.0f;
    int mode = kNeural;
};

// Neural strength ([VideoAdjustments] NeuralStrength, 0..2) and Blend ([Comparison]
// Mode=2 at Amount, 0..1) were two controls for one composite: the shader drew
// lerp(original, neural, x) for both, so Blend at 0.4 and a strength of 0.4 were the
// same pixels. They are one control now, the Mix ([Comparison] Mix, 0..2), and Blend is
// no longer a mode. A file written before that is read like this:
//
//  * no Mix key: the Mix is the old strength, so the neural view looks as it did;
//  * and Mode=Blend: the view becomes Neural at the Mix that draws the same picture,
//    Amount x strength. Exact for any strength up to 1, where both are lerps of lerps.
//    Above 1 the old picture was a blend of the extended frame, which one Mix cannot
//    draw; Amount alone is the nearest, and a Blend that ever sat there was an
//    unusual pair of dials to leave behind.
//
// Original is a view the D key chooses, not a mode to come back to: a stored 1 - or
// any number this build does not know - reads as Neural.
inline Loaded Migrate(std::optional<float> savedMix, int savedMode, float savedAmount, float savedStrength,
                      std::span<const int> knownModes)
{
    Loaded loaded;
    const float strength = std::clamp(std::isfinite(savedStrength) ? savedStrength : 1.0f, 0.0f, 2.0f);
    const float amount = std::clamp(std::isfinite(savedAmount) ? savedAmount : 0.5f, 0.0f, 1.0f);
    if (savedMix && std::isfinite(*savedMix)) loaded.mix = std::clamp(*savedMix, 0.0f, 2.0f);
    else if (savedMode == kBlend) loaded.mix = strength <= 1.0f ? amount * strength : amount;
    else loaded.mix = strength;
    for (const int mode : knownModes)
        if (mode == savedMode && mode != kOriginal && mode != kBlend) loaded.mode = mode;
    return loaded;
}

// Steps the Mix by `delta` on a 0.05 grid. [ and ] step it by a tenth, as they stepped
// the blend.
inline float StepMix(float mix, float delta)
{
    return std::clamp(std::round((mix + delta) * 20.0f) / 20.0f, 0.0f, 2.0f);
}

} // namespace compare_settings

namespace compare_labels {

// The tags are drawn by GDI, which writes colour but no alpha, onto an opaque plate.
// A pixel GDI produced is therefore `rendered = t*text + (1-t)*plate` for some
// coverage t, and what the compositor needs is the premultiplied texel that gives the
// same result over the picture when the plate itself is only `plateAlpha` opaque:
//
//     alpha = plateAlpha + t*(1 - plateAlpha)
//     rgb   = rendered - (1-t)*(1-plateAlpha)*plate
//
// t is recovered per pixel from the green channel against the known text and plate
// colours, which is exact for the grayscale antialiasing the atlas is drawn with.
// Pixels outside every plate are left fully transparent by the caller.
struct Bgra { uint8_t b, g, r, a; };

inline Bgra Premultiply(Bgra rendered, Bgra plate, Bgra text, float plateAlpha)
{
    const float span = float(text.g) - float(plate.g);
    const float t = span == 0.0f ? 0.0f : std::clamp((float(rendered.g) - float(plate.g)) / span, 0.0f, 1.0f);
    const float residual = (1.0f - t) * (1.0f - plateAlpha);
    const auto channel = [&](uint8_t value, uint8_t plateValue) {
        const float premultiplied = float(value) - residual * float(plateValue);
        return uint8_t(std::clamp(std::lround(premultiplied), 0L, 255L));
    };
    const float alpha = plateAlpha + t * (1.0f - plateAlpha);
    return Bgra{channel(rendered.b, plate.b), channel(rendered.g, plate.g), channel(rendered.r, plate.r),
                uint8_t(std::clamp(std::lround(alpha * 255.0f), 0L, 255L))};
}

} // namespace compare_labels
