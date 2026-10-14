#pragma once

// The optical-flow block matcher, shared by the offline converter
// (AmdFrameGenBackend) and the playback renderer (D3D12Renderer). One copy of
// the maths, two callers, the same arrangement FrameGenBlendShader.h uses.
//
// Why the renderer needs its own copy of this at all: the playback chain
// already has a motion field (m_motion, from the guide stage), but it is a
// 128x72 compact grid expanded to full resolution - one vector per ~15x15
// pixels, bilinearly stretched. That is smooth enough for the neural renderer's
// temporal accumulation, and far too coarse for warping: sampling the previous
// frame with a 15-pixel-resolution flow leaves the two frames visibly
// misregistered, which reads as ghosting rather than as motion blur. The
// matcher here resolves 8x8, an order of magnitude finer. (That the finer field
// actually removes the ghosting is a visual observation, not a measured one; on
// screen it did not, which is why AMD FidelityFX - which does its own, vendor-
// validated motion estimation - is now the preferred path on AMD. This matcher
// remains as the runtime-free fallback.)
//
// The cost is the reason this used to be avoided in the player. A 1080p frame
// is 43.8 billion texture fetches naive; with the group-wide search and the
// early-out below it measures ~20 ms a frame on an RX 9070 XT, which fits a
// 30 fps source on a 144 Hz panel (33 ms per source frame) and does not fit
// 60 fps (16.6 ms). kSearch is the speed/coverage dial: the cost is quadratic in
// it, so 16 is 4x cheaper and still tracks moderate motion.
//
// Output is one R16G16_FLOAT displacement per BLOCKxBLOCK tile, in pixels,
// with the SAME convention the blend expects (FrameGenBlendShader.h):
// curr(p + mv) ~= prev(p). Both files state that convention once; if you change
// one, change the other.
namespace frame_gen_flow {

constexpr char kHlsl[] = R"hlsl(
cbuffer Params : register(b0) { uint gW; uint gH; uint gBlock; uint gSearch; };
Texture2D<float4> Prev : register(t0);
Texture2D<float4> Curr : register(t1);
RWTexture2D<float2> Flow : register(u0);

// The reduction's scratch, one slot per thread in the group.
groupshared float sSad[64];
groupshared int   sDx[64];
groupshared int   sDy[64];
// One 8x8 THREAD GROUP per BLOCKxBLOCK tile: the group splits the +/-gSearch
// window across its 64 threads, each measures a strided subset of the candidate
// displacements, and a groupshared reduction picks the winner. A thread per tile
// (what this started as) left the machine 98% idle - one thread cannot cover a
// texture fetch's latency, and this shader is nothing but texture fetches.
[numthreads(8, 8, 1)]
void main(uint3 groupId : SV_GroupID, uint3 lid : SV_GroupThreadID) {
    int bw = (int)gW, bh = (int)gH;
    int originX = (int)groupId.x * (int)gBlock;
    int originY = (int)groupId.y * (int)gBlock;
    // Uniform across the group, because the tile index comes from SV_GroupID and
    // not from the thread id: a lane leaving early would strand its peers on the
    // barrier below.
    if (originX >= bw || originY >= bh) return;

    int search = (int)gSearch;
    int span = 2 * search + 1;
    int total = span * span;
    int tid = (int)lid.y * 8 + (int)lid.x;

    float bestSAD = 1e30f; int bestdx = 0, bestdy = 0;
    for (int i = tid; i < total; i += 64) {
        int dy = i / span - search;
        int dx = i % span - search;
        float sad = 0.0f;
        // Every term is a non-negative difference, so a partial sum that
        // already exceeds this thread's best cannot win: the candidate is
        // abandoned the moment it is hopeless.
        for (int sy = 0; sy < (int)gBlock; sy += 2) {
            for (int sx = 0; sx < (int)gBlock; sx += 2) {
                int px = originX + sx, py = originY + sy;
                int qx = px + dx, qy = py + dy;
                if (qx < 0 || qy < 0 || qx >= bw || qy >= bh) { sad += 1e9f; break; }
                float3 a = Prev[int2(px, py)].rgb;
                float3 b = Curr[int2(qx, qy)].rgb;
                sad += abs(a.r - b.r) + abs(a.g - b.g) + abs(a.b - b.b);
                if (sad >= bestSAD) break;
            }
            if (sad >= bestSAD) break;
        }
        if (sad < bestSAD) { bestSAD = sad; bestdx = dx; bestdy = dy; }
    }

    sSad[tid] = bestSAD; sDx[tid] = bestdx; sDy[tid] = bestdy;
    GroupMemoryBarrierWithGroupSync();
    for (int stride = 32; stride > 0; stride >>= 1) {
        if (tid < stride && sSad[tid + stride] < sSad[tid]) {
            sSad[tid] = sSad[tid + stride];
            sDx[tid] = sDx[tid + stride];
            sDy[tid] = sDy[tid + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (tid == 0) Flow[int2((int)groupId.x, (int)groupId.y)] = float2((float)sDx[0], (float)sDy[0]);
}
)hlsl";

} // namespace frame_gen_flow