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

// FSR1 EASU (FidelityFX Super Resolution 1, the edge-adaptive spatial
// upsample) is the middle rung of the upscale chain: it replaces ONLY the
// Catmull-Rom stretch, writing the same linear FP16 frame at the same output
// rung into the same target, so the CAS sharpen below runs unchanged. It is a
// pure shader - no runtime - so where the FSR 4 backend cannot start (no
// runtime, a GPU it refuses, a ratio past 3.0x) EASU is the upscaler, and
// only a PSO compile failure drops the chain to the Catmull-Rom stretch.
//
// The 12-tap kernel, the direction/length accumulation and the
// Lanczos-with-lobes weight are the AMD reference algorithm (ffxFsrEasuFloat,
// FidelityFX SDK). The reference packs four texels per gather fetch; that
// packing is flattened here into 12 point samples at exact texel centres -
// the player already reads the frame through a POINT-clamped sampler, so the
// taps land on centres and the frame edge clamps instead of reading garbage.
// con0 and con1 are computed on the CPU from the render and output sizes
// (see CreateUpscalePresentResources) and passed as float4; there is no bit
// packing because this is the host path, not the SDK's GPU-constant macros.
cbuffer EasuParams:register(b0){float4 con0;float4 con1;};
void EasuSet(inout float2 dir,inout float len,float w,float lA,float lB,float lC,float lD,float lE){
    // Direction is the '+' diff of the local gradient; length is the gradient
    // reversal shaped to 0..1. w is the bilinear weight for this of the four
    // sub-positions - a compile-time immediate, so the branches fold out.
    float dc=lD-lC;float cb=lC-lB;
    float lx=max(abs(dc),abs(cb));lx=(lx>1e-7)?1.0/lx:0.0;
    float dx=lD-lB;dir.x+=dx*w;lx=saturate(abs(dx)*lx);lx*=lx;len+=lx*w;
    float ec=lE-lC;float ca=lC-lA;
    float ly=max(abs(ec),abs(ca));ly=(ly>1e-7)?1.0/ly:0.0;
    float dy=lE-lA;dir.y+=dy*w;ly=saturate(abs(dy)*ly);ly*=ly;len+=ly*w;
}
void EasuTap(inout float3 aC,inout float aW,float2 pixelOffset,float2 dir,float2 len2,float lob,float clp,float3 color){
    // Rotate the offset by the gradient direction, stretch it anisotropically,
    // then the lanczos2-with-lobes weight as the reference writes it.
    float2 ro;
    ro.x=pixelOffset.x*dir.x+pixelOffset.y*dir.y;
    ro.y=pixelOffset.x*(-dir.y)+pixelOffset.y*dir.x;
    ro*=len2;
    float d2=ro.x*ro.x+ro.y*ro.y;
    d2=min(d2,clp);
    float wB=(2.0/5.0)*d2-1.0;
    float wA=lob*d2-1.0;
    wB*=wB;wA*=wA;
    wB=(25.0/16.0)*wB+(-(25.0/16.0-1.0));
    float w=wB*wA;
    aC+=color*w;aW+=w;
}
float4 PSAmdEasu(V i):SV_Target{
    uint2 ip=(uint2)i.p.xy;
    float2 pp=(float2)ip*con0.xy+con0.zw;
    float2 fp=floor(pp);
    pp-=fp;
    int2 base=(int2)fp;
    // 1/renderSize, to turn each integer tap into a texel-centre UV.
    float2 texel=con1.xy;
    float3 b =Input.SampleLevel(PointClamp,(float2(base.x+0,base.y-1)+0.5)*texel,0).rgb;
    float3 c =Input.SampleLevel(PointClamp,(float2(base.x+1,base.y-1)+0.5)*texel,0).rgb;
    float3 e =Input.SampleLevel(PointClamp,(float2(base.x-1,base.y+0)+0.5)*texel,0).rgb;
    float3 f =Input.SampleLevel(PointClamp,(float2(base.x+0,base.y+0)+0.5)*texel,0).rgb;
    float3 g =Input.SampleLevel(PointClamp,(float2(base.x+1,base.y+0)+0.5)*texel,0).rgb;
    float3 h =Input.SampleLevel(PointClamp,(float2(base.x+2,base.y+0)+0.5)*texel,0).rgb;
    float3 i2=Input.SampleLevel(PointClamp,(float2(base.x-1,base.y+1)+0.5)*texel,0).rgb;
    float3 j =Input.SampleLevel(PointClamp,(float2(base.x+0,base.y+1)+0.5)*texel,0).rgb;
    float3 k =Input.SampleLevel(PointClamp,(float2(base.x+1,base.y+1)+0.5)*texel,0).rgb;
    float3 l =Input.SampleLevel(PointClamp,(float2(base.x+2,base.y+1)+0.5)*texel,0).rgb;
    float3 n =Input.SampleLevel(PointClamp,(float2(base.x+0,base.y+2)+0.5)*texel,0).rgb;
    float3 o =Input.SampleLevel(PointClamp,(float2(base.x+1,base.y+2)+0.5)*texel,0).rgb;
    // Luma proxy per tap (luma*2 in two FMAs) - the reference's scalar weight.
    float bL=0.5*b.b+0.5*b.r+b.g;
    float cL=0.5*c.b+0.5*c.r+c.g;
    float eL=0.5*e.b+0.5*e.r+e.g;
    float fL=0.5*f.b+0.5*f.r+f.g;
    float gL=0.5*g.b+0.5*g.r+g.g;
    float hL=0.5*h.b+0.5*h.r+h.g;
    float iL=0.5*i2.b+0.5*i2.r+i2.g;
    float jL=0.5*j.b+0.5*j.r+j.g;
    float kL=0.5*k.b+0.5*k.r+k.g;
    float lL=0.5*l.b+0.5*l.r+l.g;
    float nL=0.5*n.b+0.5*n.r+n.g;
    float oL=0.5*o.b+0.5*o.r+o.g;
    // Accumulate direction and edge length at the four bilinear sub-positions.
    float2 dir=float2(0.0,0.0);
    float len=0.0;
    EasuSet(dir,len,(1.0-pp.x)*(1.0-pp.y),bL,eL,fL,gL,jL);
    EasuSet(dir,len,pp.x*(1.0-pp.y),      cL,fL,gL,hL,kL);
    EasuSet(dir,len,(1.0-pp.x)*pp.y,      fL,iL,jL,kL,nL);
    EasuSet(dir,len,pp.x*pp.y,            gL,jL,kL,lL,oL);
    // Normalise the direction; a near-zero gradient points along +x.
    float2 dir2=dir*dir;
    float dirR=dir2.x+dir2.y;
    bool zro=dirR<(1.0/32768.0);
    dirR=zro?1.0:rsqrt(dirR);
    dir.x=zro?1.0:dir.x;
    dir*=dirR;
    // Shape the length and the anisotropic window.
    len=len*0.5;len*=len;
    float stretch=(dir.x*dir.x+dir.y*dir.y)*rcp(max(abs(dir.x),abs(dir.y)));
    float2 len2v=float2(1.0+(stretch-1.0)*len,1.0+(-0.5)*len);
    float lob=0.5+((1.0/4.0-0.04)-0.5)*len;
    float clp=rcp(lob);
    // Accumulate the 12 weighted taps.
    float3 aC=float3(0.0,0.0,0.0);
    float aW=0.0;
    EasuTap(aC,aW,float2(0.0,-1.0)-pp,dir,len2v,lob,clp,b);
    EasuTap(aC,aW,float2(1.0,-1.0)-pp,dir,len2v,lob,clp,c);
    EasuTap(aC,aW,float2(-1.0,1.0)-pp,dir,len2v,lob,clp,i2);
    EasuTap(aC,aW,float2(0.0,1.0)-pp,dir,len2v,lob,clp,j);
    EasuTap(aC,aW,float2(0.0,0.0)-pp,dir,len2v,lob,clp,f);
    EasuTap(aC,aW,float2(-1.0,0.0)-pp,dir,len2v,lob,clp,e);
    EasuTap(aC,aW,float2(1.0,1.0)-pp,dir,len2v,lob,clp,k);
    EasuTap(aC,aW,float2(2.0,1.0)-pp,dir,len2v,lob,clp,l);
    EasuTap(aC,aW,float2(2.0,0.0)-pp,dir,len2v,lob,clp,h);
    EasuTap(aC,aW,float2(1.0,0.0)-pp,dir,len2v,lob,clp,g);
    EasuTap(aC,aW,float2(1.0,2.0)-pp,dir,len2v,lob,clp,o);
    EasuTap(aC,aW,float2(0.0,2.0)-pp,dir,len2v,lob,clp,n);
    // Dering against the four nearest neighbours (f, g, j, k - the reference's
    // packed min/max set), so a single bright or dark texel cannot ring.
    float3 min4=min(min(f,g),min(j,k));
    float3 max4=max(max(f,g),max(j,k));
    float3 pix=min(max4,max(min4,aC/aW));
    return float4(pix,1.0);
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
