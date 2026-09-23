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
inline constexpr int kNeural = 0, kOriginal = 1, kBlend = 2, kSplit = 3, kWipe = 4, kDifference = 5;

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

// The Difference view's gain, a doubling ladder: 1x shows the raw difference, which
// for a subtle render is near black, and 32x turns a one-code-value change into a
// visible one. Shift+[ and Shift+] walk it; 4x is where it starts.
inline constexpr std::array<float, 6> kDifferenceGains{1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f};
inline constexpr float kDefaultDifferenceGain = 4.0f;

inline float StepDifferenceGain(float gain, int direction)
{
    size_t index = 0;
    for (size_t candidate = 0; candidate < kDifferenceGains.size(); ++candidate)
        if (std::abs(kDifferenceGains[candidate] - gain) < std::abs(kDifferenceGains[index] - gain)) index = candidate;
    if (direction > 0 && index + 1 < kDifferenceGains.size()) ++index;
    if (direction < 0 && index > 0) --index;
    return kDifferenceGains[index];
}

// A saved gain is snapped onto the ladder, so a hand-edited 5 reads as 4.
inline float LoadDifferenceGain(float saved)
{
    return std::isfinite(saved) && saved > 0.0f ? StepDifferenceGain(saved, 0) : kDefaultDifferenceGain;
}

} // namespace compare_settings

namespace compare_zoom {

// The zoom ladder, in screen pixels per OUTPUT pixel: Fit, then 1:1, 2x, 4x and 8x,
// which is what pixel-peeping asks for - "show me the render's own pixels, bigger"
// - rather than multiples of however large the window happens to be. The shader's
// zoom is relative to the fitted picture (uv = (s - c)/z + c over the view), so a
// step becomes a scale through the output's width and the view's: z = k*outputW/viewW.
// A step whose scale would not magnify - 1:1 of a 1080p render in a 4K window - is
// skipped. With no output size known yet the ladder falls back to multiples of Fit.
inline constexpr std::array<int, 5> kPixelsPerTexel{0, 1, 2, 4, 8};
inline constexpr int kSteps = int(kPixelsPerTexel.size());

inline float ScaleForStep(int step, uint32_t outputW, int viewW)
{
    if (step <= 0) return 1.0f;
    const float perTexel = float(kPixelsPerTexel[size_t(std::min(step, kSteps - 1))]);
    if (!outputW || viewW <= 0) return std::max(1.0f, perTexel);
    return std::max(1.0f, perTexel * float(outputW) / float(viewW));
}

inline bool Reachable(int step, uint32_t outputW, int viewW)
{
    return step == 0 || ScaleForStep(step, outputW, viewW) > 1.0001f;
}

// The next step in `direction` (+1 in, -1 out) that changes the picture. Zooming in
// past 8x stays at 8x unless `wrap`, which is what Z does - one key walks the whole
// ladder and comes back to Fit. Zooming out from the lowest step that magnifies is Fit.
inline int Step(int step, int direction, uint32_t outputW, int viewW, bool wrap)
{
    for (int next = step + direction; next >= 0 && next < kSteps; next += direction)
        if (Reachable(next, outputW, viewW)) return next;
    if (direction < 0) return 0;
    return wrap ? 0 : step;
}

struct Centre {
    float x = 0.5f;
    float y = 0.5f;
};

// The image point shown at view position `view` (both in [0,1]) for a centre and
// scale: the shader's uv = (s - c)/z + c.
inline float ImageAt(float centre, float scale, float view)
{
    return (view - centre) / std::max(scale, 1.0f) + centre;
}

// The centre that keeps the image point under `anchor` where it is when the scale goes
// from `from` to `to`: wheel zoom "at the cursor". Solving (a - c')/to + c' = u for c'
// gives c' = (u - a/to)/(1 - 1/to), clamped so the view never leaves the picture.
inline Centre ZoomAt(Centre centre, float from, float to, float anchorX, float anchorY)
{
    if (to <= 1.0f) return Centre{};
    const auto axis = [&](float c, float anchor) {
        const float image = ImageAt(c, from, anchor);
        return std::clamp((image - anchor / to) / (1.0f - 1.0f / to), 0.0f, 1.0f);
    };
    return Centre{axis(centre.x, anchorX), axis(centre.y, anchorY)};
}

// The centre after the pointer dragged the picture by (dx, dy) of the view: the image
// moves with the pointer, so the offset c*(1 - 1/z) moves by -d/z.
inline Centre Pan(Centre centre, float scale, float dx, float dy)
{
    if (scale <= 1.0f) return centre;
    const float span = 1.0f - 1.0f / scale;
    const auto axis = [&](float c, float d) { return std::clamp((c * span - d / scale) / span, 0.0f, 1.0f); };
    return Centre{axis(centre.x, dx), axis(centre.y, dy)};
}

} // namespace compare_zoom

namespace compare_loupe {

// The synced loupe: two circles side by side, the original on the left and DLSS 5 on
// the right, both showing the image point under the pointer at the same magnification.
// They sit above the pointer so it does not cover what they show, drop below it near
// the top edge, and slide sideways to stay inside the view.
struct Placement {
    POINT left{};
    POINT right{};
};

inline Placement Place(POINT pointer, int viewW, int viewH, int radius, int gap)
{
    const int span = 4 * radius + gap;
    const int left = std::clamp(int(pointer.x) - span / 2, 0, std::max(0, viewW - span));
    int y = int(pointer.y) - gap - radius;
    if (y - radius < 0) y = int(pointer.y) + gap + radius;
    if (y + radius > viewH) y = std::max(radius, viewH - radius);
    return Placement{POINT{left + radius, y}, POINT{left + 3 * radius + gap, y}};
}

// Screen pixels per output texel inside the loupe: at least 4, and always twice what
// the view itself is showing, so the loupe still magnifies a view zoomed to 4x.
inline float Magnification(float viewPixelsPerTexel)
{
    return std::clamp(2.0f * viewPixelsPerTexel, 4.0f, 32.0f);
}

} // namespace compare_loupe

namespace compare_view {

enum class Fit { Fit, Fill, Pixels };

// Where the render window goes in the video area. Fit and Fill keep the picture's
// aspect inside or over the area; Pixels makes the window exactly the renderer's
// output, so its backbuffer is the output's size and the present is 1:1 - which the
// renderer already draws exactly - cropped by the area when it is larger. Without an
// output size Pixels is Fit.
inline RECT RenderRect(int areaW, int areaH, double aspect, Fit fit, uint32_t outputW, uint32_t outputH)
{
    areaW = std::max(1, areaW);
    areaH = std::max(1, areaH);
    const double ar = aspect > 0.0 ? aspect : 16.0 / 9.0;
    const double areaAr = double(areaW) / double(areaH);
    int rw = 0, rh = 0;
    if (fit == Fit::Pixels && outputW && outputH) {
        rw = int(outputW);
        rh = int(outputH);
    } else if (fit == Fit::Fill) {
        if (areaAr > ar) { rw = areaW; rh = int(std::lround(areaW / ar)); }
        else { rh = areaH; rw = int(std::lround(areaH * ar)); }
    } else {
        if (areaAr > ar) { rh = areaH; rw = int(std::lround(areaH * ar)); }
        else { rw = areaW; rh = int(std::lround(areaW / ar)); }
    }
    rw = std::max(1, rw);
    rh = std::max(1, rh);
    const int left = (areaW - rw) / 2, top = (areaH - rh) / 2;
    return RECT{left, top, left + rw, top + rh};
}

} // namespace compare_view

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
