#pragma once

#include "FrameIdentity.h"

#include <cstdint>
#include <optional>
#include <string_view>

// Motion-compensated temporal stability for the neural render (P2.5).
//
// The neural pass relights each frame on its own, and what it adds that the source
// did not have is shimmer: a surface that holds still in the video changes tone or
// texture from one frame to the next in the render. The old plan - one history
// texture and one lerp - ghosts on anything that moves. This one follows Lai et al.
// (ECCV 2018) in the part that matters: the previous OUTPUT is warped by the flow
// before it is blended, and a pixel is blended only where the warp is trusted.
//
// Trust is judged on the source, not on the output: the previous decoded frame is
// warped by the same vector and compared with this one. Where the two agree, the
// content at that pixel is the content the history holds, so any change in the
// neural output there is change the model added and the history may pull it back.
// Where they disagree - an occlusion, a disocclusion, a vector FlowGate's round
// trip already rejected to zero over something that moved, a cut the detector let
// through - the current frame is taken as it is. A change of brightness along the
// vector is not a disagreement: it is measured as a gain, taken out before the
// comparison and carried into the history, so a fade or a lighting change is
// followed rather than lagged. The test needs no estimate of its own to be right,
// which is why it can sit on either motion source (NVOFA or the CPU matcher).
//
// The pass runs in the helper's capture path, after the add-on has written the
// neural frame and before the cache capture reads it, so what it produces is what
// is cached. It is a ladder with Off as the default until measurement says
// otherwise, and every rung is a cache-key term (TemporalSettings.h).
enum class TemporalStability : uint8_t { Off, Low, Medium, High };

inline constexpr std::string_view TemporalStabilityName(TemporalStability level) noexcept
{
    switch (level) {
    case TemporalStability::Off: return "off";
    case TemporalStability::Low: return "low";
    case TemporalStability::Medium: return "medium";
    case TemporalStability::High: return "high";
    }
    return "off";
}

inline constexpr std::optional<TemporalStability> ParseTemporalStability(std::string_view text) noexcept
{
    for (const TemporalStability candidate : {TemporalStability::Off, TemporalStability::Low,
                                              TemporalStability::Medium, TemporalStability::High})
        if (TemporalStabilityName(candidate) == text) return candidate;
    return std::nullopt;
}

namespace temporal_stability {

// What one rung asks of the pass. `historyWeight` is how much of the warped history
// a fully trusted pixel keeps; the output is a recursive filter over the history, so
// on a static pixel frame-to-frame noise of the model is scaled by
// sqrt((1-w)/(1+w)): 0.73 at 0.3, 0.58 at 0.5, 0.42 at 0.7. `trustLow`/`trustHigh`
// bound the source mismatch left once its brightness change is taken out - 8-bit
// sRGB codes / 255, the mean over four taps of the worst channel - below which a
// pixel is fully trusted and above which not at all.
struct Blend {
    float historyWeight{};
    float trustLow{};
    float trustHigh{};

    friend constexpr bool operator==(const Blend&, const Blend&) = default;
};

// Measured 2026-09-23 with tools/benchmark (run.py profiles passing --temporal, then
// analyze.py) on the thirteen clips a clean checkout builds, RTX 4080 SUPER. Added
// temporal sigma - output minus source, 8-bit codes, lower is steadier - and PSNR
// against the source, Off -> Low / Medium / High, on the four real captures:
//   real-game-motion  sigma+ 1.199 -> 1.163 / 1.131 / 1.081   PSNR 31.00 -> 31.00 / 31.00 / 31.01
//   real-dissolve     sigma+ 0.793 -> 0.697 / 0.588 / 0.377   PSNR 30.07 -> 30.05 / 30.00 / 29.87
//   real-game-cuts    sigma+ -0.362 -> -0.391 / -0.414 / -0.443  PSNR 32.55 -> 32.60 / 32.65 / 32.73
//   real-film-cuts    sigma+ 0.046 -> 0.035 / 0.029 / 0.024   PSNR 35.13 -> 35.19 / 35.23 / 35.27
// Over all thirteen the mean falls -1.106 -> -1.144 / -1.178 / -1.256 and mean PSNR
// moves 28.73 -> 28.72 / 28.67 / 28.59. The cost is detail on the synthetic
// fine-detail clip, SSIM 0.693 -> 0.685 / 0.678 / 0.666, which is why this is a
// ladder and why Off stays the default until a blind comparison says otherwise.
// The first version of the pass, without the source gain and with a signed
// mismatch, bought less sigma for more fidelity (High: -1.231 at PSNR 28.40).
//
// The trust band is the same on every rung: it decides WHERE the history may be
// used, which is a question about the source and not about how strong the user
// wants the effect, and loosening it is how ghosting gets in.
inline constexpr float kTrustLow = 3.0f / 255.0f;
inline constexpr float kTrustHigh = 10.0f / 255.0f;
inline constexpr float kLowWeight = 0.30f;
inline constexpr float kMediumWeight = 0.50f;
inline constexpr float kHighWeight = 0.70f;

// nullopt for Off, where the pass does not run at all and the capture reads the
// neural output exactly as it did before the pass existed.
inline constexpr std::optional<Blend> BlendFor(TemporalStability level) noexcept
{
    switch (level) {
    case TemporalStability::Off: break;
    case TemporalStability::Low: return Blend{kLowWeight, kTrustLow, kTrustHigh};
    case TemporalStability::Medium: return Blend{kMediumWeight, kTrustLow, kTrustHigh};
    case TemporalStability::High: return Blend{kHighWeight, kTrustLow, kTrustHigh};
    }
    return std::nullopt;
}

// The two-slot history the renderer keeps: slot `current` holds the stabilized
// output of `id` and, beside it in the same slot, the decoded source that output
// was made from; when `hasBase`, the other slot holds both for the frame before.
// The second slot is what makes a re-submitted frame - the offline job's receipt
// gate evaluates its first frame up to 120 times - blend against the same history
// every time instead of against its own previous attempt.
struct History {
    FrameIdentity id{};
    bool valid{};
    bool hasBase{};
    uint32_t current{};
};

enum class Step : uint8_t {
    Reset,    // no usable history: the output is the neural frame, which starts one
    Advance,  // the successor of the frame the history holds: blend, then keep both
    Repeat,   // the same frame again: blend against the same history as last time
};

struct Plan {
    Step step{Step::Reset};
    uint32_t read{};   // slot the history and the previous source are read from
    uint32_t write{};  // slot this frame's output and source are written to
    bool blend{};      // false on a reset: `read` then names nothing valid
};

// `frameReset` is true when the frame itself started a new history - a scene cut,
// a seek, a first frame, a recreated feature - whatever the identity says.
inline constexpr Plan PlanFrame(const History& history, const FrameIdentity& frame, bool frameReset) noexcept
{
    const uint32_t other = history.current ^ 1u;
    if (!history.valid || frameReset || frame.reset != HistoryReset::None)
        return Plan{Step::Reset, other, other, false};
    const FrameIdentity& held = history.id;
    const bool sameStream = frame.jobId == held.jobId && frame.sourceGeneration == held.sourceGeneration;
    if (sameStream && frame.frameNumber == held.frameNumber && frame.pts100ns == held.pts100ns) {
        // A repeat of a frame that had no history of its own is a reset again, into
        // the same slot, so the slot holding the frame before it is left alone.
        if (!history.hasBase) return Plan{Step::Reset, history.current, history.current, false};
        return Plan{Step::Repeat, other, history.current, true};
    }
    // Anything but the direct successor - a gap the guides did not declare, another
    // job, another decode - has no history this pass can vouch for.
    if (!sameStream || frame.frameNumber != held.frameNumber + 1u) return Plan{Step::Reset, other, other, false};
    return Plan{Step::Advance, history.current, other, true};
}

inline constexpr History Commit(const History& history, const Plan& plan, const FrameIdentity& frame) noexcept
{
    History next = history;
    next.id = frame;
    next.valid = true;
    next.current = plan.write;
    // After an advance the slot read from holds the predecessor; a repeat leaves the
    // base as it was; a reset leaves nothing behind it.
    next.hasBase = plan.step == Step::Advance || (plan.step == Step::Repeat && history.hasBase);
    return next;
}

} // namespace temporal_stability
