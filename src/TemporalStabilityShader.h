#pragma once

// The temporal stability pass (TemporalStabilityPolicy.h), as HLSL text of its own.
//
// It is not in the renderer's shared program text for one reason that outweighs the
// convenience: that text is where PSPresent lives, and the cache capture runs
// PSPresent, so its bytecode is part of every cached render on disk. A pass added
// there is a pass compiled into the same translation unit as the capture shader.
// Here it cannot touch it, and a test can hand it to D3DCompile without a device.
//
// Bindings, one descriptor table of five, in the order the renderer writes them:
//   t0 this frame's neural output (linear FP16, output size)
//   t1 the stabilized output of the frame before (linear FP16, output size)
//   t2 the motion NGX was given (R16G16F, current -> previous, DLSS input pixels)
//   t3 this frame's decoded source (8-bit sRGB-encoded BGRA, source size)
//   t4 the decoded source of the frame before (same format and size)
inline constexpr char kTemporalStabilityHlsl[] = R"(
Texture2D Neural:register(t0); Texture2D History:register(t1); Texture2D<float2> Motion:register(t2);
Texture2D Source:register(t3); Texture2D PreviousSource:register(t4);
SamplerState Linear:register(s0);
float3 SRGBToLinear(float3 c){float3 lo=c/12.92;float3 hi=pow(max((c+0.055)/1.055,0),2.4);return lerp(hi,lo,step(c,0.04045));}
float3 LinearToSRGB(float3 c){c=max(c,0);float3 lo=c*12.92;float3 hi=1.055*pow(c,1.0/2.4)-0.055;return saturate(lerp(hi,lo,step(c,0.0031308)));}
float Luma709(float3 c){return dot(c,float3(0.2126,0.7152,0.0722));}
// MotionToUv turns a DLSS-input-pixel vector into UV. Weight is the rung's history
// weight, zero on a reset, where the pass is a copy. TrustLow/TrustHigh bound the
// source mismatch between full trust and none. SourceTexel is one source texel in UV.
cbuffer Params:register(b0){ float2 MotionToUv; float Weight; float TrustLow; float TrustHigh; float2 SourceTexel; float Unused; };
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);V o;o.uv=uv;o.p=float4(uv.x*2-1,1-uv.y*2,0,1);return o;}
// Catmull-Rom from nine bilinear taps. A plain bilinear fetch of the history is a
// tent filter applied once per frame, and a recursive filter compounds it: a
// surface under a slow pan would soften a little more every frame it is held.
// The kernel can overshoot below zero at an edge, and a negative light is clamped.
float3 SampleHistory(float2 uv){
    float2 size;History.GetDimensions(size.x,size.y);
    float2 position=uv*size;
    float2 origin=floor(position-0.5)+0.5;
    float2 f=position-origin;
    float2 w0=f*(-0.5+f*(1.0-0.5*f));
    float2 w1=1.0+f*f*(-2.5+1.5*f);
    float2 w2=f*(0.5+f*(2.0-1.5*f));
    float2 w3=f*f*(-0.5+0.5*f);
    float2 w12=w1+w2;
    float2 t0=(origin-1.0)/size,t3=(origin+2.0)/size,t12=(origin+w2/w12)/size;
    float3 c=0;
    c+=History.SampleLevel(Linear,float2(t0.x,t0.y),0).rgb*w0.x*w0.y;
    c+=History.SampleLevel(Linear,float2(t12.x,t0.y),0).rgb*w12.x*w0.y;
    c+=History.SampleLevel(Linear,float2(t3.x,t0.y),0).rgb*w3.x*w0.y;
    c+=History.SampleLevel(Linear,float2(t0.x,t12.y),0).rgb*w0.x*w12.y;
    c+=History.SampleLevel(Linear,float2(t12.x,t12.y),0).rgb*w12.x*w12.y;
    c+=History.SampleLevel(Linear,float2(t3.x,t12.y),0).rgb*w3.x*w12.y;
    c+=History.SampleLevel(Linear,float2(t0.x,t3.y),0).rgb*w0.x*w3.y;
    c+=History.SampleLevel(Linear,float2(t12.x,t3.y),0).rgb*w12.x*w3.y;
    c+=History.SampleLevel(Linear,float2(t3.x,t3.y),0).rgb*w3.x*w3.y;
    return max(c,0.0);
}
float4 PSTemporalStability(V i):SV_Target{
    float3 current=Neural.Load(int3(int2(i.p.xy),0)).rgb;
    // A reset is a copy, and returns before anything else is read: the history of a
    // reset frame names no valid texture content, and NaN times zero is NaN.
    if(Weight<=0.0)return float4(current,1);
    uint2 motionSize;Motion.GetDimensions(motionSize.x,motionSize.y);
    // Nearest, as the flow resolve does: across an occlusion boundary neighbouring
    // vectors describe different surfaces, and interpolating them invents one.
    float2 motion=Motion.Load(int3(min(uint2(i.uv*float2(motionSize)),motionSize-1),0));
    float2 previous=i.uv+motion*MotionToUv;
    if(any(previous<0.0)||any(previous>1.0))return float4(current,1);
    // The source the same vector lands on, in a 2x2 footprint of bilinear taps. Two
    // things are read off it. The first is how much the source's brightness changed
    // along the vector - a fade, a lighting change, a slow gradient - as one linear
    // gain, which the history is carried through so that it follows every change the
    // source made and holds back only the change the model added. Without it, a
    // trusted pixel lags a real change by the filter's time constant: measured on
    // highlights-gradients, 1.4 dB of PSNR against the source at the highest rung.
    float3 now[4],before[4];
    float lumaNow=0,lumaBefore=0;
    [unroll]for(int k=0;k<4;++k){
        float2 o=(float2(k&1,k>>1)-0.5)*SourceTexel;
        now[k]=Source.SampleLevel(Linear,i.uv+o,0).rgb;
        before[k]=PreviousSource.SampleLevel(Linear,previous+o,0).rgb;
        lumaNow+=Luma709(SRGBToLinear(now[k]));
        lumaBefore+=Luma709(SRGBToLinear(before[k]));
    }
    float gain=clamp((lumaNow+0.004)/(lumaBefore+0.004),0.5,2.0);
    // The second is what is left once that gain is applied: structure that did not
    // line up. It is a mean of per-tap magnitudes, not the magnitude of a mean -
    // a texture misregistered by a fraction of a pixel has signed differences that
    // cancel, and blending it anyway is exactly how a filter like this blurs detail.
    // Measured in the sRGB-encoded values the decoder delivers, where one code is
    // one step a viewer can see.
    float mismatch=0;
    [unroll]for(int t=0;t<4;++t){
        float3 d=abs(now[t]-LinearToSRGB(SRGBToLinear(before[t])*gain));
        mismatch+=max(d.r,max(d.g,d.b));
    }
    float trust=1.0-smoothstep(TrustLow,TrustHigh,mismatch*0.25);
    return float4(lerp(current,SampleHistory(previous)*gain,Weight*trust),1);
}
)";
