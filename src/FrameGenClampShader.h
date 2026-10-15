#pragma once

// Per-pixel envelope clamp on the island's generated frame: every pixel is
// pulled into [min(prev,cur), max(prev,cur)] of the two converted source
// frames it was interpolated between. With no engine motion vectors the
// runtime has no way to warp content, so a pixel outside the envelope of its
// own two inputs cannot be legitimate motion reveal - it is the runtime's
// colour-consistency correction overshooting, which measured as bright
// speckles on high-contrast edges (worst frames ~1% of pixels brighter than
// both sources, 2026-10-15). The clamp touches only the pixels already outside
// the envelope (0.08% on average), so the cost is a negligible dispatch and
// that overshoot is removed by construction.
//
// This is an artefact guard, NOT the fix for the reported white flash. That was
// a double sRGB encoding in the PRESENT chain, which made every generated frame
// arrive ~1.49x brighter than its two sources - whole-frame, on any content,
// not an edge effect. See the CONTRACT note in FrameGenColorConvertShader.h.
//
// All three inputs and the output are sRGB-encoded BGRA8; clamping in the
// encoded domain is exact because the transfer is monotonic, which is why this
// pass's descriptors in m_fgClampHeap deliberately stay DXGI_FORMAT_*_UNORM:
// the values it copies must survive unchanged. Only the views the backbuffer
// pass samples through are _UNORM_SRGB.
namespace frame_gen_clamp {

constexpr char kHlsl[] = R"hlsl(
Texture2D<float4> Gen : register(t0);
Texture2D<float4> Cur : register(t1);
Texture2D<float4> Prev : register(t2);
RWTexture2D<float4> Dst : register(u0);
cbuffer Params : register(b0) { uint gW; uint gH; };

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    float3 lo = min(Cur[int2(id.xy)].rgb, Prev[int2(id.xy)].rgb);
    float3 hi = max(Cur[int2(id.xy)].rgb, Prev[int2(id.xy)].rgb);
    Dst[int2(id.xy)] = float4(clamp(Gen[int2(id.xy)].rgb, lo, hi), 1.0f);
}
)hlsl";

constexpr uint32_t kThreadGroup = 8;

} // namespace frame_gen_clamp
