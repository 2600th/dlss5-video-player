#include "UpscalingPolicy.h"
#include "FlowGate.h"
#include "NgxSession.h"
#include "NvofResolveShader.h"
#include "TestSupport.h"

#include <d3dcompiler.h>
#include <d3d12shader.h>

int main() {
    const auto hd = UpscalingTarget(1920,1080,1440);
    CHECK_EQ(hd.width,2560u); CHECK_EQ(hd.height,1440u); CHECK(hd.grows);
    const auto uhd = UpscalingTarget(1920,1080,2160);
    CHECK_EQ(uhd.width,3840u); CHECK_EQ(uhd.height,2160u); CHECK(uhd.grows);
    const auto native = UpscalingTarget(3840,2160,1440);
    CHECK_EQ(native.width,3840u); CHECK_EQ(native.height,2160u); CHECK(!native.grows);
    const auto wide = UpscalingTarget(1920,800,1440);
    CHECK_EQ(wide.width,2560u); CHECK_EQ(wide.height,1066u); CHECK(wide.grows);
    CHECK(!UpscalingTarget(0,1080,1440).grows);
    CHECK(!UpscalingTarget(1920,1080,720).grows);
    // The 1080 rung. A 720p source on a 1080p panel is the case the two-rung
    // menu could only answer by rendering 1440 lines for a 1080-line display.
    const auto fhd = UpscalingTarget(1280,720,1080);
    CHECK_EQ(fhd.width,1920u); CHECK_EQ(fhd.height,1080u); CHECK(fhd.grows);
    CHECK(!UpscalingTarget(1920,1080,1080).grows);
    CHECK_EQ(UpscaleRungWidth(1080),1920u);
    CHECK_EQ(UpscaleRungWidth(1440),2560u);
    CHECK_EQ(UpscaleRungWidth(2160),3840u);
    CHECK_EQ(UpscaleRungWidth(720),0u);
    CHECK_EQ(UpscaleRungWidth(0),0u);

    // Auto takes the largest rung the panel can scan out, never one above it.
    CHECK_EQ(AutoUpscaleTargetHeight(2160),2160u);
    CHECK_EQ(AutoUpscaleTargetHeight(1440),1440u);
    CHECK_EQ(AutoUpscaleTargetHeight(1600),1440u);
    CHECK_EQ(AutoUpscaleTargetHeight(1080),1080u);
    CHECK_EQ(AutoUpscaleTargetHeight(1200),1080u);
    // Above the largest rung the target stays at 4K: an 8K panel is not a
    // reason to pay for 8K of DLSS evaluate per frame.
    CHECK_EQ(AutoUpscaleTargetHeight(4320),2160u);
    // Below the smallest rung, and an unreadable monitor, both refuse.
    CHECK_EQ(AutoUpscaleTargetHeight(1050),0u);
    CHECK_EQ(AutoUpscaleTargetHeight(0),0u);
    CHECK(!UpscalingTarget(1280,720,AutoUpscaleTargetHeight(0)).grows);

    // The four cases the adaptive policy exists for, end to end.
    CHECK_EQ(UpscalingTarget(2560,1440,AutoUpscaleTargetHeight(2160)).height,2160u); // 2K source, 4K panel
    CHECK(UpscalingTarget(2560,1440,AutoUpscaleTargetHeight(2160)).grows);
    CHECK(!UpscalingTarget(3840,2160,AutoUpscaleTargetHeight(2160)).grows);          // 4K source, 4K panel
    CHECK(UpscalingTarget(1920,1080,AutoUpscaleTargetHeight(2160)).grows);           // 1080p source, 4K panel
    CHECK(!UpscalingTarget(1920,1080,AutoUpscaleTargetHeight(1080)).grows);          // 1080p source, 1080p panel
    CHECK(SourceFitsDLSSRange(1920,1080,2560,1440,1280,720,2560,1440));
    CHECK(!SourceFitsDLSSRange(1920,1080,2560,1440,1280,720,1706,960));
    CHECK(!SourceFitsDLSSRange(3840,2160,2560,1440,1,1,3840,2160));
    CHECK(!SourceFitsDLSSRange(1920,1080,2560,1440,0,0,0,0));
    // The RTX 2060 report: a 436x573 photo asked to reach 1440 lines. For a
    // 1096x1440 output the runtime advertised a 548x720 minimum, so the source
    // fell short and upscaling was refused outright instead of aiming lower.
    const auto reduced=AdmissibleDLSSOutput(436,573,1096,1440,548,720);
    CHECK(reduced.grows);CHECK_EQ(reduced.width,872u);CHECK_EQ(reduced.height,1146u);
    CHECK(SourceFitsDLSSRange(436,573,reduced.width,reduced.height,436,573,872,1146));
    // A source already inside the advertised range needs no reduction.
    CHECK(!AdmissibleDLSSOutput(1920,1080,2560,1440,1280,720).grows);
    // And a reduction that would land on the source is not an upscale.
    CHECK(!AdmissibleDLSSOutput(600,400,1200,800,1199,799).grows);
    bool recreate=false;bool created=false;
    const auto first=ngx_session_detail::PrepareFeatureForFrame(true,false,1,recreate,
        [&]{created=true;return true;},[]{return false;},true);
    CHECK(created);CHECK(first.selected);CHECK(first.needsFlush);

    // The forward/backward round trip. A vector the reverse field cancels is motion that
    // happened; one it repeats is an occlusion or a mismatch, and how much disagreement
    // is tolerated follows the magnitude of the pair rather than a fixed pixel budget.
    static_assert(flow_gate::Disagrees(4.0f,-3.0f,4.0f,-3.0f),
                  "the gate is composed into HLSL, so it has to fold at compile time");
    CHECK(!flow_gate::Disagrees(4.0f,-3.0f,-4.0f,3.0f));
    CHECK(!flow_gate::Disagrees(0.0f,0.0f,0.0f,0.0f));
    // Sub-pixel disagreement on a nearly still cell is the grid's own quantisation.
    CHECK(!flow_gate::Disagrees(0.2f,0.1f,-0.1f,-0.2f));
    // One pixel of round-trip error either way: kept on a 40 px/frame pan, rejected on a
    // half-pixel drift, which is what scale-free means here.
    CHECK(!flow_gate::Disagrees(40.0f,0.0f,-39.0f,0.0f));
    CHECK(flow_gate::Disagrees(0.5f,0.0f,0.5f,0.0f));

    // The resolve pass itself, compiled from the same string the renderer compiles, with
    // its entry points, targets and flags. No device and no flow engine are involved, so
    // this is the one check of that shader a machine without an RTX GPU can make - and
    // the shader is otherwise first compiled on a user's.
    auto compiles=[](const char* entry, const char* target) {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT hr = D3DCompile(kNvofResolveHlsl, sizeof(kNvofResolveHlsl) - 1, "nvof",
                                      nullptr, nullptr, entry, target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors) {
            if (FAILED(hr)) std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << '\n';
            errors->Release();
        }
        const bool compiled = SUCCEEDED(hr) && code && code->GetBufferSize() > 0;
        if (code) code->Release();
        return compiled;
    };
    CHECK(compiles("VS","vs_5_1"));
    CHECK(compiles("PSNvofMotion","ps_5_1"));

    // The zero-motion test's contract with the renderer (w4-sr): eight root constants,
    // the eighth switching the test on, and the engine's two input frames at t3/t4
    // through the table the renderer binds at RootOverlay. A constant the shader
    // dropped or a register it moved would leave the test silently off - the flow
    // engine's fixed field on a still frame then decays Super Resolution again.
    {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT hr = D3DCompile(kNvofResolveHlsl, sizeof(kNvofResolveHlsl) - 1, "nvof",
                                      nullptr, nullptr, "PSNvofMotion", "ps_5_1",
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors) errors->Release();
        ID3D12ShaderReflection* reflection = nullptr;
        CHECK(SUCCEEDED(hr) && code &&
              SUCCEEDED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&reflection))));
        if (reflection) {
            D3D12_SHADER_BUFFER_DESC params{};
            CHECK(SUCCEEDED(reflection->GetConstantBufferByName("Params")->GetDesc(&params)));
            D3D12_SHADER_VARIABLE_DESC zero{};
            CHECK(SUCCEEDED(reflection->GetConstantBufferByName("Params")->GetVariableByName("ZeroMotionTest")->GetDesc(&zero)));
            CHECK_EQ(28u, zero.StartOffset);
            CHECK((zero.uFlags & D3D_SVF_USED) != 0);
            D3D12_SHADER_INPUT_BIND_DESC current{}, previous{};
            CHECK(SUCCEEDED(reflection->GetResourceBindingDescByName("Current", &current)));
            CHECK(SUCCEEDED(reflection->GetResourceBindingDescByName("Previous", &previous)));
            CHECK_EQ(3u, current.BindPoint);
            CHECK_EQ(4u, previous.BindPoint);
            reflection->Release();
        }
        if (code) code->Release();
    }
    return test_support::failure_count==0?0:1;
}
