#pragma once

// Linear-light FP16 -> sRGB-encoded BGRA8, the colour domain the working
// reference (MPCVR-FSR-FrameGen) feeds FidelityFX frame generation and the only
// one its behaviour is validated in. The playback chain runs in FP16 linear
// light, and the runtime's SCRGB transfer path expects a different luminance
// convention than this chain's [0,1] range - with the input left in FP16 the
// generated frames ghosted even after the configure step and the guide states
// were made correct, so the conversion lands the runtime in the exact
// domain its motion estimation was built and tested against.
//
// D3D12 note: writing float4(r,g,b,a) through a UAV on a B8G8R8A8_UNORM
// texture stores channels in the format's byte order but reads back in RGBA
// order, so the shader does linear->sRGB encoding only; no channel swizzle.
//
// ============================ CONTRACT ============================
// What this pass writes is sRGB-ENCODED, and so is everything downstream of it
// inside the runtime's domain: the island's output and the envelope clamp both
// copy these values through unchanged. The textures are created
// DXGI_FORMAT_B8G8R8A8_UNORM because that is the format FidelityFX is handed,
// and a UNORM texture does not carry a gamma tag - the ENCODING is a property
// of what was stored, not of the resource.
//
// The backbuffer pass is the one reader that must undo it, and it does not do
// so in the shader: it works in linear light and applies COMPOSE_ENCODE
// (LinearToSRGB for an SDR backbuffer) at the very end, exactly as it does for
// the FP16 LINEAR textures it otherwise reads (m_dlssColor, m_dlssOutput).
// Therefore EVERY SRV through which a generated frame reaches that pass must be
// DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, so the SAMPLER performs the decode. Created
// as plain _UNORM instead, the pass receives the encoded value, believes it is
// linear, and encodes it a second time - the whole picture arrives about 1.49x
// brighter every other present, which is the white flash reported with playback
// frame generation on.
//
// The clamp's descriptors are the deliberate exception: it compares and writes
// in the encoded domain, where the transfer is monotonic so the min/max
// envelope is exact, and it samples through its own heap (m_fgClampHeap) whose
// views stay _UNORM because the values it copies must survive unchanged.
//
// See CreateFrameGenResources in D3D12Renderer.cpp for the two views, and
// run-fg/probe_srgb_domain.py for the measurement that pins the ratios.
namespace frame_gen_convert {

constexpr char kHlsl[] = R"hlsl(
Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);
cbuffer Params : register(b0) { uint gW; uint gH; };

float3 LinearToSrgb(float3 c)
{
    // The exact sRGB transfer, both branches: a sqrt-only approximation bends
    // the dark end, where the matcher's SAD lives.
    float3 lo = 12.92f * c;
    float3 hi = 1.055f * pow(abs(c), 1.0f / 2.4f) - 0.055f;
    return c <= 0.0031308f ? lo : hi;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    // `linear` is a reserved HLSL keyword (interpolation modifier) and cannot
    // name a local - the first build of this shader failed to compile on it
    // and the player silently fell back to the matcher path.
    float3 rgb = saturate(Src[int2(id.xy)].rgb);
    Dst[int2(id.xy)] = float4(LinearToSrgb(rgb), 1.0f);
}
)hlsl";

constexpr uint32_t kThreadGroup = 8;

} // namespace frame_gen_convert
