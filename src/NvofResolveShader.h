#pragma once

#include "FlowGate.h"

// The pass that turns the NVOFA flow grid into the full-resolution motion texture NGX
// reads. It is compiled on its own rather than with the shared source in
// D3D12Renderer.cpp because it reads integer and unsigned textures where that source
// declares float ones at the same registers; the root signature is the same one, so it
// binds exactly like every other pass.
//
// The text lives in a header of its own so a test can hand it to D3DCompile without a
// device or an optical-flow engine. That is the only proof of this pass available on a
// machine without an RTX GPU, and it is worth having: the shader is otherwise compiled
// for the first time on a user's machine.

// The gate's two constants are pasted in from FlowGate.h. Stringifying the macro is what
// keeps HLSL from carrying a second copy of numbers that C++ also has to agree with.
#define NVOF_RESOLVE_TOKEN_(value) #value
#define NVOF_RESOLVE_TOKEN(value) NVOF_RESOLVE_TOKEN_(value)

inline constexpr char kNvofResolveHlsl[] =
    R"(
Texture2D<int2> Flow:register(t0); Texture2D<uint> Cost:register(t1); Texture2D<int2> BackFlow:register(t2);
// CellsPerPixel turns a vector in input pixels into flow cells - the reciprocal of the
// engine's output grid - and is zero when no backward field is bound. The round-trip
// gate below is then absent rather than neutral, which is what makes a device that only
// offered forward flow emit exactly what it emitted before the gate existed.
cbuffer Params:register(b0){ float2 FlowScale; float2 Gate; float CellsPerPixel; };
static const float GateAlpha=)" NVOF_RESOLVE_TOKEN(FLOW_GATE_ALPHA) R"(;
static const float GateBeta=)" NVOF_RESOLVE_TOKEN(FLOW_GATE_BETA_PX2) R"(;
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);V o;o.uv=uv;o.p=float4(uv.x*2-1,1-uv.y*2,0,1);return o;}
// NVOFA writes S10.5 fixed point: one unit is 1/32 of an input pixel. With
// inputFrame = this frame and referenceFrame = the previous one the vector already
// points from the current pixel back to where that content was, which is verbatim the
// DLSS convention, so there is no sign flip here.
//
// The fetch is NEAREST, not bilinear. Across a disocclusion the neighbouring cells
// describe different surfaces and interpolating them manufactures a vector no cell
// measured, widening the band instead of narrowing it. Where the field is smooth it
// varies far more slowly than one cell and the two filters agree anyway.
float2 PSNvofMotion(V i):SV_Target{
    uint2 dim; Flow.GetDimensions(dim.x,dim.y);
    int2 cell=int2(min(uint2(i.uv*float2(dim)),dim-1));
    float2 flow=float2(Flow.Load(int3(cell,0)))*FlowScale;
    float2 motion=flow;
    // Gate.x == Gate.y disables the confidence gate, which is the default: the cost
    // thresholds are not measured yet and inventing them would be a guess that silently
    // deletes real motion. When they are set, a high-cost cell fades toward zero rather
    // than switching off, so the decision cannot alternate frame to frame.
    if(Gate.y>Gate.x){
        float cost=float(Cost.Load(int3(cell,0)));
        motion*=saturate((Gate.y-cost)/(Gate.y-Gate.x));
    }
    // The round trip. The backward field is estimated on the reference frame, so it is
    // read where the forward vector lands rather than where it starts, and the landing
    // cell is clamped because content that left the frame has nowhere to come back from
    // and the border cell is the closest honest answer. The test judges the measured
    // pair, not the cost-faded vector above, and rejects by zeroing rather than fading:
    // an occluded cell has no motion to scale down, and zero is already what the rest of
    // this path means by nothing moved here.
    if(CellsPerPixel>0){
        int2 dest=clamp(cell+int2(round(flow*CellsPerPixel)),int2(0,0),int2(dim)-1);
        float2 back=float2(BackFlow.Load(int3(dest,0)))*FlowScale;
        float2 residual=flow+back;
        if(dot(residual,residual)>GateAlpha*(dot(flow,flow)+dot(back,back))+GateBeta)motion=float2(0,0);
    }
    return motion;
}
)";
