#pragma once

#include <string_view>

// The two passes that turn the AMD runtime's 1:1 output into an upscaled
// capture. The runtime is a fixed-box denoiser - job.private_output is always
// the submitted geometry - so an export that asked for 1440p from a 1080p
// source gets its size here rather than from the network: this pass stretches
// the frame to the capture size and sharpens it, and everything downstream
// (readback, encoder, metrics) sees the capture size as before.
//
// Two passes rather than one. The stretch is a 16-tap Catmull-Rom, which
// reproduces a linear gradient exactly and rides edges with the overshoot
// that makes a bicubic read as sharp; the sharpen is AMD's Contrast Adaptive
// Sharpening (FidelityFX CAS, Lou Kramer, AMD Developer Day 2019 - the same
// algorithm the ReShade and mpv ports carry), which adapts its weight to the
// local contrast so edges sharpen and flat regions do not. Running CAS on the
// Catmull-Rom's own 3x3 output neighbourhood is what the two-pass shape buys:
// a single pass would have to recompute the stretch for every CAS tap, nine
// stretch evaluations per pixel for the same picture.
//
// Both passes work on the display codes the frame already carries - B8G8R8A8
// UNORM in, B8G8R8A8 UNORM out, no gamma conversion in either direction. That
// is FSR's own shipped behaviour for CAS (the game's backbuffer codes), and
// the readback's consumers all treat these codes as display-encoded anyway;
// the relinearising refinement some ports add is a look, not a correctness
// fix, and it is not this pass's to make.
inline constexpr char kAmdUpscaleHlsl[] = R"(
Texture2D Input:register(t0);
SamplerState PointClamp:register(s0);
// srcW/srcH name the frame the runtime published; outW/outH the capture the
// readback carries. Sharpness scales the CAS weight: 0 is the stretch alone,
// 1 is the filter at its official strength.
cbuffer Params:register(b0){float SrcW;float SrcH;float OutW;float OutH;float Sharpness;float Pad0;float Pad1;float Pad2;};
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);V o;o.uv=uv;o.p=float4(uv.x*2-1,1-uv.y*2,0,1);return o;}

// Catmull-Rom weights over the four texels i-1..i+2 around a sample that sits
// t of the way between texel i and i+1. The weights sum to one at every t and
// reproduce a linear ramp exactly - both checked against a CPU reference
// before this shipped (t=0.5 gives -1/16, 9/16, 9/16, -1/16, the classic
// bicubic overshoot).
void CatmullRom1D(float t,out float4 w){
    w.x=(( -0.5*t + 1.0)*t - 0.5)*t;
    w.y=((  1.5*t - 2.5)*t)*t + 1.0;
    w.z=(( -1.5*t + 2.0)*t + 0.5)*t;
    w.w=(  0.5*t - 0.5)*t*t;
}

// The stretch: the output pixel's position mapped back into source texel
// space, then the 4x4 Catmull-Rom neighbourhood around it, sampled at the
// texel centres. Point sampling at exact centres, because the weights above
// are the whole filter - letting a linear sampler blend the taps would apply
// it twice.
float4 PSAmdUpscale(V i):SV_Target{
    float2 srcPos=(i.uv)*float2(SrcW,SrcH)-0.5;
    float2 base=floor(srcPos);
    float2 frac=srcPos-base;
    float4 wx,wy;CatmullRom1D(frac.x,wx);CatmullRom1D(frac.y,wy);
    float2 texel=float2(1.0/SrcW,1.0/SrcH);
    // The texel centre of base-1 is at (base-1+0.5)/srcSize; each tap advances
    // one texel. Rows first, then the vertical weights over the four rows.
    float3 rows[4];
    [unroll]for(int r=0;r<4;++r){
        float2 rowUV=float2(base.x-0.5,(base.y-1.0)+float(r)+0.5)*texel;
        float3 c0=Input.SampleLevel(PointClamp,rowUV,0).rgb;
        float3 c1=Input.SampleLevel(PointClamp,rowUV+float2(texel.x,0),0).rgb;
        float3 c2=Input.SampleLevel(PointClamp,rowUV+float2(texel.x*2,0),0).rgb;
        float3 c3=Input.SampleLevel(PointClamp,rowUV+float2(texel.x*3,0),0).rgb;
        rows[r]=c0*wx.x+c1*wx.y+c2*wx.z+c3*wx.w;
    }
    float3 c=rows[0]*wy.x+rows[1]*wy.y+rows[2]*wy.z+rows[3]*wy.w;
    // No saturate here: the target decides. An 8-bit UNORM target clamps the
    // write to [0,1] by its own definition, which is what the export path
    // wants; the FP16 target the playback path writes keeps the bicubic's
    // overshoot, which is what a linear-light present chain expects.
    return float4(c,1);
}

// Contrast Adaptive Sharpening on the stretched frame, the official filter:
// a soft min and max over the 3x3 neighbourhood shape the weight per channel,
// so flat regions get almost none and single-pixel edges get the most. The
// filter is the cross b+d+f+h against the centre; the diagonals enter only
// through the shaped range. Sharpness scales the weight, 0 leaving the frame
// exactly as the stretch wrote it.
float4 PSAmdCas(V i):SV_Target{
    float2 texel=float2(1.0/OutW,1.0/OutH);
    float3 a=Input.SampleLevel(PointClamp,i.uv+texel*float2(-1,-1),0).rgb;
    float3 b=Input.SampleLevel(PointClamp,i.uv+texel*float2( 0,-1),0).rgb;
    float3 c=Input.SampleLevel(PointClamp,i.uv+texel*float2( 1,-1),0).rgb;
    float3 d=Input.SampleLevel(PointClamp,i.uv+texel*float2(-1, 0),0).rgb;
    float3 e=Input.SampleLevel(PointClamp,i.uv,0).rgb;
    float3 f=Input.SampleLevel(PointClamp,i.uv+texel*float2( 1, 0),0).rgb;
    float3 g=Input.SampleLevel(PointClamp,i.uv+texel*float2(-1, 1),0).rgb;
    float3 h=Input.SampleLevel(PointClamp,i.uv+texel*float2( 0, 1),0).rgb;
    float3 jj=Input.SampleLevel(PointClamp,i.uv+texel*float2( 1, 1),0).rgb;
    // Soft min and max, twice factored by two as the official filter writes
    // them: the cross first, then the diagonals folded in at half weight.
    float3 mn=min(min(min(d,e),min(f,b)),h);
    float3 mn2=min(mn,min(min(a,c),min(g,jj)));
    mn+=mn2;
    float3 mx=max(max(max(d,e),max(f,b)),h);
    float3 mx2=max(mx,max(max(a,c),max(g,jj)));
    mx+=mx2;
    // Smooth minimum distance to the signal limit over the smooth max, then
    // the shaping: rqrt of that ratio is the adaptive part, and the official
    // peak of 8 is Contrast at its default of zero.
    //
    // The statistics are read off the LDR part of the signal: this filter runs
    // twice, once into an 8-bit UNORM target on the export path and once into
    // FP16 on the playback path, and FP16 carries highlights the official [0,1]
    // arithmetic cannot survive - min(mn,2-mx) goes negative and rsqrt of it
    // does not exist. Clamping the STATISTICS leaves the shaping conservative
    // over highlights (they keep their value, they simply do not sharpen),
    // while the filtered output below still blends the unclamped taps, so the
    // highlight itself reaches the target untouched.
    float3 mnc=saturate(mn),mxc=saturate(mx);
    float3 amp=saturate(min(mnc,2.0-mxc)/max(mxc,1e-5));
    amp=rsqrt(amp);
    float3 w=-1.0/(amp*8.0)*Sharpness;
    // The blend runs on the unclamped taps for the same reason: the FP16
    // playback target keeps what the filter produces, the UNORM export target
    // clamps it on write.
    float3 outC=(((b+d)+(f+h))*w+e)/(1.0+4.0*w);
    return float4(outC,1);
}
)";

// The profile the entry point `entry` compiles under, for either of the two
// programs this backend compiles. Same rule and the same reasoning as the
// temporal accumulation's copy (AmdTemporalAccumulationShader.h): derived from
// the name, because a VS handed to ps_5_1 fails with an X4502 that names
// neither the entry point nor the profile.
inline constexpr const char* UpscaleShaderProfileFor(std::string_view entry) noexcept
{
    return entry == "VS" ? "vs_5_1" : "ps_5_1";
}
