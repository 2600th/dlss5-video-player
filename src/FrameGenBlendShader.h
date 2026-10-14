#pragma once

// The frame-generation blend, shared by the offline converter (AmdFrameGenBackend)
// and the playback renderer (D3D12Renderer). One copy of the maths, two callers:
// the converter feeds it a flow field it computed itself, the renderer feeds it
// the motion field the guide stage already produced. The same shape as
// AmdUpscaleShader.h, which the AMD capture and the export share.
//
// Both callers hold the same vector convention - curr(p + mv) ~= prev(p) - but
// at different resolutions, and that difference is the whole reason this is
// shared as text with a switch rather than as one compiled blob:
//
//   default (FLOW_PER_BLOCK)  the converter's own matcher writes one R16G16_FLOAT
//                             displacement per 8x8 tile. It is read with bilinear
//                             interpolation, not per-tile lookup: neighbouring
//                             tiles often disagree by a pixel, and warping one
//                             tile with its own vector while the next uses a
//                             different one is what put an 8x8 grid on screen.
//   -DFLOW_PER_PIXEL          the renderer's motion guide is full-resolution
//                             RG16F (DLSS_MotionVectors_CurrentToPrevious), so a
//                             pixel reads its own vector. Indexing that field per
//                             tile would read out of bounds the moment the flow
//                             texture is smaller than the frame.
//                             (Kept for a per-pixel field; the playback path no
//                             longer uses it - it runs the block matcher itself,
//                             see FrameGenFlowShader.h.)
//
// Warp + blend. The previous frame's content advances by mv per frame, so at
// time t the point now at p was at p - t*mv in the previous frame and at
// p + (1-t)*mv in the current one. We sample both and average. Occlusions (a
// coordinate landing outside the frame) are clamped by the bilinear helper, so
// they fade toward the nearer frame rather than sampling garbage.
//
// HLSL has no lambdas, which cost this shader its first compile (D3DCompile
// rejects 'auto at = [&](...)' with X3000) - keep it a plain function set.
namespace frame_gen_blend {

// The tile edge the per-block flow is written on; unused in the per-pixel
// build, but the constant buffer carries it in both so one layout serves both.
constexpr int kBlock = 8;

constexpr char kHlsl[] = R"hlsl(
cbuffer Params : register(b0) { uint gW; uint gH; uint gBlock; uint gTBits; };
Texture2D<float4> Prev : register(t0);
Texture2D<float4> Curr : register(t1);
Texture2D<float2> Flow : register(t2);
RWTexture2D<float4> Out : register(u0);
float4 sampleClamped(Texture2D<float4> tex, int x, int y, int w, int h) {
    x = clamp(x, 0, w - 1); y = clamp(y, 0, h - 1);
    return tex[int2(x, y)];
}
float4 bilinear(Texture2D<float4> tex, float2 p, int w, int h) {
    float2 f = p - 0.5f;
    float2 fl = floor(f);
    float2 t = f - fl;
    int x0 = (int)fl.x, y0 = (int)fl.y;
    int x1 = x0 + 1, y1 = y0 + 1;
    float4 s00 = sampleClamped(tex, x0, y0, w, h);
    float4 s10 = sampleClamped(tex, x1, y0, w, h);
    float4 s01 = sampleClamped(tex, x0, y1, w, h);
    float4 s11 = sampleClamped(tex, x1, y1, w, h);
    return lerp(lerp(s00, s10, t.x), lerp(s01, s11, t.x), t.y);
}
// The per-tile flow field, read with the SAME bilinear helper as the frames.
//
// Reading it per tile (Flow[x/gBlock, y/gBlock]) is what put visible 8x8
// squares in the interpolated frame: a block matcher on a weakly textured tile
// reports a vector that differs from its neighbour's, the blend then warps the
// two tiles' pixels to different places, and the seam between them shows as a
// grid. Interpolating the field means a pixel near a tile edge uses a blend of
// both vectors, so the displacement varies smoothly across the frame and the
// discontinuity disappears. The cost is four fetches instead of one per pixel.
float2 flowBilinear(Texture2D<float2> flow, int x, int y, int block, int tilesX, int tilesY) {
    float2 p = float2((float)x, (float)y) / (float)block - 0.5f;
    float2 fl = floor(p);
    float2 t = p - fl;
    int x0 = clamp((int)fl.x, 0, tilesX - 1);
    int y0 = clamp((int)fl.y, 0, tilesY - 1);
    int x1 = clamp(x0 + 1, 0, tilesX - 1);
    int y1 = clamp(y0 + 1, 0, tilesY - 1);
    float2 s00 = flow[int2(x0, y0)];
    float2 s10 = flow[int2(x1, y0)];
    float2 s01 = flow[int2(x0, y1)];
    float2 s11 = flow[int2(x1, y1)];
    return lerp(lerp(s00, s10, t.x), lerp(s01, s11, t.x), t.y);
}
[numthreads(8, 8, 1)]
void main(uint3 gid : SV_DispatchThreadID) {
    int x = (int)gid.x, y = (int)gid.y;
    if (x >= (int)gW || y >= (int)gH) return;
#ifdef FLOW_PER_PIXEL
    float2 mv = Flow[int2(x, y)];
#else
    int tilesX = ((int)gW + (int)gBlock - 1) / (int)gBlock;
    int tilesY = ((int)gH + (int)gBlock - 1) / (int)gBlock;
    float2 mv = flowBilinear(Flow, x, y, (int)gBlock, tilesX, tilesY);
#endif
    float t = asfloat(gTBits);
    // mv is the BACKWARD vector: curr(P1) == prev(P1 + mv), which is what the
    // matcher writes (FrameGenFlowShader.h) and what DLSS_MotionVectors_
    // CurrentToPrevious holds.
    //
    // To show the frame at fraction t along the pair, every pixel p of the output
    // has to LOOK AT ONE PLACE - p + t*mv - and mix the two frames there by how
    // far along the interval that place falls:
    //
    //     out(p) = (1 - t) * prev(p + t*mv) + t * curr(p + t*mv)
    //
    // Both offsets are the SAME displacement. This is the part that is easy to
    // get wrong, and was: the first version sampled prev at p + t*mv and curr at
    // p - (1-t)*mv, which is the "warp each frame to where the content will be"
    // reading. Those two coordinates are a whole mv apart, so the average was
    // blending two DIFFERENT pieces of the picture - measured on a synthetic
    // 4 px/frame slide, it reproduced a 8 px smear on every edge. Sign flips
    // cannot fix that, because the magnitudes are wrong too; only the endpoints
    // ever looked right, since at t=0 and t=1 the second term drops out.
    //
    // NOTE: the claim that this geometry was verified against a known-answer clip
    // is NOT established. A synthetic-slide harness was built
    // (build-ninja/fg_quality.py) but its numbers turned out to be dominated by
    // the H.264 encode the pass writes the result through - the measured error
    // was the same to three decimals for two different backends, which is a
    // measurement that cannot see the thing it claims to measure. Do not repeat
    // those figures until the harness reads the frames before they are encoded.
    float2 warped = float2((float)x, (float)y) + t * mv;
    float3 pW = bilinear(Prev, warped, (int)gW, (int)gH).rgb;
    float3 cW = bilinear(Curr, warped, (int)gW, (int)gH).rgb;
    Out[int2(x, y)] = float4(lerp(pW, cW, t), 1.0f);
}
)hlsl";

} // namespace frame_gen_blend