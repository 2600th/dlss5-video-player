#pragma once

#include <string_view>

// The two one-line passes that carry the AMD runtime's frame across the
// boundary between its format and the temporal accumulation's.
//
// The accumulation itself is deliberately NOT here. It is kTemporalStabilityHlsl
// (TemporalStabilityShader.h), the same pass the NVIDIA capture path runs, and
// this file exists so that it can be reused unchanged. Its contract is linear
// FP16 in and linear FP16 out - t0 is the neural frame already in linear light,
// and t3/t4 are the decoded source as 8-bit sRGB, which the pass linearises
// itself - while the AMD runtime publishes and is read back as 8-bit sRGB BGRA.
// Something has to cross that boundary, and it is here rather than inside the
// pass so that the pass stays one program shared by two backends instead of two
// programs that have to be kept in step.
//
// Decode is a pass rather than an _SRGB view of the runtime's texture because
// the pass reads its own input with Load. Leaning on the SRV format to linearise
// would make the reuse rest on a format-conversion rule that is never written
// down anywhere in this tree, and a wrong guess about it would show up as a
// gamma error in the picture and nothing else. A line of shader text says the
// same thing and can be read.
//
// Bindings, one texture:
//   t0 the texture to convert (decode: the runtime's own output; encode: the
//      history slot the accumulation just wrote)
//
// Both entry points write what their target format wants: PSDecode writes
// linear light into an FP16 target, PSEncode writes sRGB-encoded codes into an
// 8-bit UNORM target, which is exactly what the runtime's own output carries.
inline constexpr char kAmdTemporalConvertHlsl[] = R"(
Texture2D Input:register(t0);
float3 SRGBToLinear(float3 c){float3 lo=c/12.92;float3 hi=pow(max((c+0.055)/1.055,0),2.4);return lerp(hi,lo,step(c,0.04045));}
float3 LinearToSRGB(float3 c){c=max(c,0);float3 lo=c*12.92;float3 hi=1.055*pow(c,1.0/2.4)-0.055;return saturate(lerp(hi,lo,step(c,0.0031308)));}
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
// The same full-screen triangle kTemporalStabilityHlsl draws. Repeated rather
// than lifted into a shared string because the two programs are compiled
// separately and a shared fragment would have to be kept byte-identical for no
// gain; this one is two lines and cannot drift in a way that matters.
V VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);V o;o.uv=uv;o.p=float4(uv.x*2-1,1-uv.y*2,0,1);return o;}
float4 PSDecode(V i):SV_Target{return float4(SRGBToLinear(Input.Load(int3(int2(i.p.xy),0)).rgb),1);}
float4 PSEncode(V i):SV_Target{return float4(LinearToSRGB(Input.Load(int3(int2(i.p.xy),0)).rgb),1);}
)";

// The profile the entry point `entry` compiles under, for either of the two
// programs this backend compiles.
//
// Derived from the entry point's name instead of taken as an argument at the
// call site, because the call sites compile five entry points in a row out of
// two strings and the mistake is invisible until a device compiles one:
// D3DCompile answers with X4502 about an invalid semantic on the shader's own
// struct, naming neither the entry point nor the profile. Five lines that
// cannot pair "VS" with ps_5_1 are cheaper than reading that.
//
// Both programs declare exactly one vertex entry point and spell it `VS`;
// everything else - PSDecode, PSEncode, PSTemporalStability - is a pixel
// shader. A new entry point under any other name is a pixel shader until
// someone says otherwise here.
inline constexpr const char* ShaderProfileFor(std::string_view entry) noexcept
{
    return entry == "VS" ? "vs_5_1" : "ps_5_1";
}
