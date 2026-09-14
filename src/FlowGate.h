#pragma once

// Forward/backward round-trip test for one optical-flow cell.
//
// A forward vector is evidence that something moved only if the backward vector at the
// place it points to comes back. Where the content is occluded in the reference frame,
// or where the estimator locked onto a repeating pattern, the two contradict each other,
// and a reconstruction told that such a cell moved smears it. The criterion below is the
// scale-free standard form: the round-trip residual is judged against the energy of the
// pair rather than against a fixed pixel budget, so a 40 px/frame pan is allowed the
// proportionally larger error that a 1 px/frame drift is not.
//
// alpha = 0.01 and beta = 0.5 px^2 are Sundaram/Brox's published occlusion thresholds
// (Dense Point Trajectories by GPU-accelerated Large Displacement Optical Flow, ECCV
// 2010) and are what most forward/backward consistency code has used unchanged since.
// beta is the part that keeps a still cell from being rejected for sub-pixel noise: it
// admits a round-trip residual of about 0.7 px, which on the engine's 1/32 px grid is a
// real disagreement rather than quantisation. They are literature defaults and not
// numbers measured here - nothing in this tree has yet run the engine's backward field
// against a labelled occlusion mask - so they are a starting point, not a result.
//
// The two are macros as well as constants because NvofResolveShader.h has to paste the
// same numbers into HLSL text and a constexpr float cannot be turned back into a token.
// This header is the only place either number appears.
#define FLOW_GATE_ALPHA 0.01f
#define FLOW_GATE_BETA_PX2 0.5f

namespace flow_gate {

inline constexpr float kAlpha = FLOW_GATE_ALPHA;
inline constexpr float kBetaPx2 = FLOW_GATE_BETA_PX2;

// True when the pair disagrees, i.e. when the cell is occluded or the estimator
// contradicted itself, and the vector must not be emitted. Both vectors are in the unit
// beta is written in: input pixels. `backward` is the reverse field sampled at the
// destination the forward vector points to, so on a cell that round-trips the two sum to
// zero regardless of how far the content travelled.
constexpr bool Disagrees(float forwardX, float forwardY,
                         float backwardX, float backwardY) noexcept
{
    const float residualX = forwardX + backwardX;
    const float residualY = forwardY + backwardY;
    const float residual = residualX * residualX + residualY * residualY;
    const float energy = forwardX * forwardX + forwardY * forwardY +
                         backwardX * backwardX + backwardY * backwardY;
    return residual > kAlpha * energy + kBetaPx2;
}

}  // namespace flow_gate
