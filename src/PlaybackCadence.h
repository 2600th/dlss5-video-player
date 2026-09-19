#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// What live neural playback does when it cannot present every frame.
//
// Ordinary playback drops a late frame by decoding the next one and throwing
// the old one away. That is cheap when decoding is cheap. It is NOT cheap here:
// a neural session advances by decoding a PAIR - the original and the rendered
// member, both full-rate - so discarding a frame costs exactly what presenting
// one costs. There is no cheap skip, and that turns falling behind into a state
// nothing recovers from: every tick spends its whole budget discarding, presents
// at most one frame, and ends further behind than it started.
//
// Measured on an RTX 5090 at 2560x1440 with a render job running, from the
// player's own playback-health line:
//
//   source 59.94 fps  (budget 16.68 ms/frame)  ->  12.3 fps presented
//   source 119.88 fps (budget  8.34 ms/frame)  ->   0.31 fps presented,
//                                                   203 discarded in 11.6 s
//
// The second reads as a frozen picture, and it is what a frame-generated 2x
// conversion asks for: generation halves the budget without changing the cost.
// The advance cost itself measured about 21 ms per frame, so the pipeline is
// over budget at 60 fps and two and a half times over at 120.
//
// Two levers, because the cost has two halves and only one of them is optional.
//
//   STRIDE presents one pair in N and skips the PRESENTATION work for the rest -
//     the upload, the guide pass and the present. Every pair is still decoded,
//     so this buys back only the present half. That half is the larger one, and
//     an even stride is the difference between 120 fps that cannot be shown and
//     60 fps that can: the same motion, half the frames, no judder. A viewer
//     reads that as smooth. They read the alternative as broken.
//
//   RE-ANCHOR stops walking and seeks to where the clock already is. A seek
//     costs roughly half a second on this machine (see "Seek timing" in the
//     log) and skips arbitrarily far, while walking costs a pair decode for
//     every frame in between - 240 of them for two seconds of a 120 fps source,
//     which is five seconds of work to cover two. Past about a second of
//     lateness, walking is the slower way to arrive.
//
// Stride is tried first and re-anchoring is the fallback, because stride
// degrades the picture smoothly and a seek is a visible discontinuity.
namespace playback_cadence {

// Presenting one pair in eight is the floor. Past that the result is a
// slideshow whichever way it is reached, and a session that needs more than
// this is one the machine cannot follow - which is a thing to say out loud
// (see CanFollowLive) rather than to approximate with an ever-coarser stride.
inline constexpr uint32_t kMaxStride = 8;

// Lateness that stride is expected to absorb, in frame intervals. Below this
// the cadence is holding; above it, the stride is not yet coarse enough.
inline constexpr double kWidenAtFrames = 2.0;

// Lateness that says walking has lost - seek instead. One second is about two
// seek costs here, so it is the point where covering the gap frame by frame
// stops being the cheaper way to close it.
inline constexpr double kReanchorSeconds = 1.0;

// Lateness below which the stride narrows again, in frame intervals. This is a
// small POSITIVE number, and the first version of it was negative - it required
// the pipeline to run EARLY before it would give a frame back. Nothing that is
// merely keeping up ever runs early: it sits at about zero lateness, which is
// below the widen threshold and was above the narrow one, so the stride
// ratcheted up on one transient and stayed there for good.
//
// Measured, on the run that found it: a 59.94 fps source presenting 55.99 fps
// with zero drops at stride 1, then one hiccup, then stride 1-in-8 held for the
// rest of the session - advancing 120 pairs every two seconds, exactly the
// source rate, and showing 15 of them. Keeping up perfectly and discarding 7
// frames in 8.
//
// Widen above kWidenAtFrames, narrow below this, hold in between: the band is
// what stops it oscillating, and it has to be a band rather than a ratchet.
inline constexpr double kNarrowUnderFrames = 0.5;

// How many pairs one tick may walk past before giving up on walking. Without a
// bound, a single tick can spend hundreds of milliseconds discarding - which is
// what made the playhead recede faster than the loop could chase it.
inline constexpr uint32_t kMaxWalkPerTick = 4;

enum class Action : uint8_t {
    Present,   // show this pair
    Skip,      // advance past it without presenting; stride is doing its work
    Reanchor,  // too far behind to walk: seek to the clock
};

struct Decision {
    Action action = Action::Present;
    uint32_t stride = 1;  // the cadence to carry into the next frame
};

// `behindSeconds` is the clock minus this frame's timestamp: positive means
// late. `phase` counts pairs since the last presented one.
inline Decision Decide(double behindSeconds, double frameIntervalSeconds,
                       uint32_t stride, uint32_t phase)
{
    Decision decision{};
    if (!(frameIntervalSeconds > 0.0) || !std::isfinite(frameIntervalSeconds)) {
        decision.stride = 1;
        return decision;
    }
    stride = std::clamp(stride, 1u, kMaxStride);
    decision.stride = stride;
    if (!std::isfinite(behindSeconds)) return decision;

    // Walking cannot close this gap for less than a seek. Say so before
    // touching the stride: the cadence that got here is not the thing to
    // adjust, the position is.
    if (behindSeconds >= kReanchorSeconds) {
        decision.action = Action::Reanchor;
        return decision;
    }
    const double lateFrames = behindSeconds / frameIntervalSeconds;
    if (lateFrames > kWidenAtFrames) decision.stride = std::min(kMaxStride, stride + 1u);
    else if (lateFrames < kNarrowUnderFrames && stride > 1u) decision.stride = stride - 1u;

    // Phase is counted against the stride in force for THIS frame, not the one
    // just chosen: widening takes effect from the next presented frame, so the
    // cadence never skips a beat it had already committed to.
    decision.action = (phase % stride) == 0 ? Action::Present : Action::Skip;
    return decision;
}

// Phase advances for every pair consumed, whichever way the decision went.
// Stated here because getting it wrong is silent: a caller that resets the
// phase when it presents makes (phase % stride) true on every frame, so the
// stride widens to its cap and skips nothing. That shipped once, and the only
// visible trace was a health line reading stride=1in8 beside dropped=0.
inline uint32_t NextPhase(uint32_t phase, uint32_t stride) noexcept
{
    const uint32_t wrap = std::max(1u, stride);
    return (phase + 1u) % wrap;
}

// Whether a session can follow this source at all, given what one pair costs.
// `advanceCostSeconds` is the decode of a pair on its own - the part stride
// cannot remove - so a source whose frame interval is shorter than that can
// never be followed live, however coarse the cadence gets. The player says this
// at attach time rather than letting the viewer discover it as a frozen
// picture.
inline bool CanFollowLive(double frameIntervalSeconds, double advanceCostSeconds)
{
    if (!(frameIntervalSeconds > 0.0) || !std::isfinite(frameIntervalSeconds)) return true;
    if (!(advanceCostSeconds > 0.0) || !std::isfinite(advanceCostSeconds)) return true;
    return advanceCostSeconds <= frameIntervalSeconds;
}

} // namespace playback_cadence
