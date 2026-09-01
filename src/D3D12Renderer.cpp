#include "D3D12Renderer.h"
#include "D3D12FenceWait.h"
#include "Log.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <cmath>

using Microsoft::WRL::ComPtr;

static bool HR(HRESULT hr, const char* what) {
    if (FAILED(hr)) { LOG(what << " failed hr=0x" << std::hex << hr); return false; }
    return true;
}
static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{}; p.Type=type; p.CreationNodeMask=1; p.VisibleNodeMask=1; return p;
}
static D3D12_RESOURCE_DESC Tex2D(DXGI_FORMAT fmt,uint32_t w,uint32_t h,D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=w; d.Height=h;
    d.DepthOrArraySize=1; d.MipLevels=1; d.Format=fmt; d.SampleDesc={1,0}; d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags=flags; return d;
}
static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource=r;
    x.Transition.StateBefore=a; x.Transition.StateAfter=b; x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; return x;
}

D3D12RendererOwner MakeD3D12Renderer(){return D3D12RendererOwner(new D3D12Renderer());}

void D3D12RendererDeleter::operator()(D3D12Renderer* renderer)const noexcept{
    if(!renderer)return;
    const auto result=renderer->DrainForRetirement();
    if(result==d3d12_renderer_detail::FenceWaitResult::Completed||
       result==d3d12_renderer_detail::FenceWaitResult::DeviceRemoved){
        delete renderer;
        return;
    }
    LOG("Renderer retirement retained after bounded GPU drain failure.");
}

D3D12Renderer::~D3D12Renderer() {
#if defined(D3D12_RENDERER_TESTING)
    m_testOwnedResource.reset();
#endif
    for (uint32_t i=0;i<FrameCount;++i) {
        if (m_upload[i] && m_uploadMapped[i]) m_upload[i]->Unmap(0,nullptr);
        if (m_guideUpload[i] && m_guideMapped[i]) m_guideUpload[i]->Unmap(0,nullptr);
        m_uploadMapped[i]=nullptr;
        m_guideMapped[i]=nullptr;
    }
    m_dlss.Shutdown();
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
}

bool D3D12Renderer::Initialize(HWND hwnd,uint32_t sourceW,uint32_t sourceH,uint32_t outputW,uint32_t outputH,uint32_t gridW,uint32_t gridH,NVSDK_NGX_PerfQuality_Value quality) {
    m_hwnd=hwnd; m_sourceW=sourceW; m_sourceH=sourceH; m_outputW=outputW; m_outputH=outputH; m_gridW=gridW; m_gridH=gridH; m_quality=quality;
    if(!m_gridW||!m_gridH)return false;
    if(!CreateDeviceAndSwapchain(hwnd) || !CreateHeapsAndBackbuffers() || !CreatePipelines()) return false;
    bool gpuSynchronized=false;
    if(!InitializeDLSS(gpuSynchronized)) {
        if(!gpuSynchronized)return false;
        LOG("DLSS unavailable; using D3D12 scaler fallback.");
        m_renderW=std::max(1u,outputW*2u/3u); m_renderH=std::max(1u,outputH*2u/3u);
    }
    if(!CreateVideoResources()) return false;
    LOG("V11 guide contract: compact CPU optical-flow grid expanded on GPU into full R16G16_FLOAT MVs + R8 bias; depth is written directly into the same R32_TYPELESS/D32_FLOAT resource passed to NGX; temporal reset only on discontinuities.");
    return true;
}

bool D3D12Renderer::CreateDeviceAndSwapchain(HWND hwnd) {
    UINT ff=0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> dbg; if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); ff|=DXGI_CREATE_FACTORY_DEBUG; }
#endif
    if(!HR(CreateDXGIFactory2(ff,IID_PPV_ARGS(&m_factory)),"CreateDXGIFactory2")) return false;
    ComPtr<IDXGIAdapter1> fallback;
    for(UINT i=0;;++i){
        ComPtr<IDXGIAdapter1>a; if(m_factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&a))==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d); if(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if(FAILED(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_12_0,_uuidof(ID3D12Device),nullptr))) continue;
        if(!fallback) fallback=a; if(d.VendorId==0x10DE){m_adapter=a;break;}
    }
    if(!m_adapter)m_adapter=fallback; if(!m_adapter){LOG("No D3D12 hardware adapter.");return false;}
    DXGI_ADAPTER_DESC1 ad{};m_adapter->GetDesc1(&ad);LOG("D3D12 adapter vendor=0x"<<std::hex<<ad.VendorId<<" device=0x"<<ad.DeviceId);
    if(!HR(D3D12CreateDevice(m_adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&m_device)),"D3D12CreateDevice"))return false;
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(!HR(m_device->CreateCommandQueue(&q,IID_PPV_ARGS(&m_queue)),"CreateCommandQueue"))return false;
    for(uint32_t i=0;i<FrameCount;++i) {
        if(!HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&m_allocators[i])),"CreateCommandAllocator"))return false;
    }
    for(uint32_t i=0;i<FrameCount;++i) {
        if(!HR(m_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,m_allocators[i].Get(),nullptr,IID_PPV_ARGS(&m_cmds[i])),"CreateCommandList"))return false;
        m_cmds[i]->Close();
    }
    BOOL tearing=FALSE;if(SUCCEEDED(m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing))))m_allowTearing=tearing==TRUE;
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=m_outputW;sd.Height=m_outputH;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc={1,0};sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount=FrameCount;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sd.Scaling=DXGI_SCALING_STRETCH;sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE;sd.Flags=m_allowTearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
    ComPtr<IDXGISwapChain1>sc1;if(!HR(m_factory->CreateSwapChainForHwnd(m_queue.Get(),hwnd,&sd,nullptr,nullptr,&sc1),"CreateSwapChainForHwnd"))return false;
    m_factory->MakeWindowAssociation(hwnd,DXGI_MWA_NO_ALT_ENTER);sc1.As(&m_swapchain);
    if(m_swapchain) m_swapchain->SetMaximumFrameLatency(2);
    if(!HR(m_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&m_fence)),"CreateFence"))return false;
    m_fenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);return m_fenceEvent!=nullptr;
}

bool D3D12Renderer::CreateHeapsAndBackbuffers(){
    D3D12_DESCRIPTOR_HEAP_DESC rh{};rh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rh.NumDescriptors=FrameCount+3;
    if(!HR(m_device->CreateDescriptorHeap(&rh,IID_PPV_ARGS(&m_rtvHeap)),"Create RTV heap"))return false;m_rtvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for(uint32_t i=0;i<FrameCount;++i){if(!HR(m_swapchain->GetBuffer(i,IID_PPV_ARGS(&m_backbuffers[i])),"Get backbuffer"))return false;m_device->CreateRenderTargetView(m_backbuffers[i].Get(),nullptr,RTV(i));}
    D3D12_DESCRIPTOR_HEAP_DESC sh{};sh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;sh.NumDescriptors=7;sh.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(!HR(m_device->CreateDescriptorHeap(&sh,IID_PPV_ARGS(&m_srvHeap)),"Create SRV heap"))return false;
    m_srvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC dh{};dh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;dh.NumDescriptors=1;
    if(!HR(m_device->CreateDescriptorHeap(&dh,IID_PPV_ARGS(&m_dsvHeap)),"Create DSV heap"))return false;
    m_dsvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return true;
}

bool D3D12Renderer::CreatePipelines(){
    const char* hlsl=R"(
Texture2D T:register(t0); SamplerState S:register(s0);
cbuffer Params:register(b0){
    float2 JitterUV;
    float2 Misc;
    float4 ColorA; // brightness, contrast, saturation, gamma
    float4 ColorB; // temperature, tint, reserved, reserved
}
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);V o;o.uv=uv;o.p=float4(uv.x*2-1,1-uv.y*2,0,1);return o;}
float3 SRGBToLinear(float3 c){float3 lo=c/12.92;float3 hi=pow(max((c+0.055)/1.055,0),2.4);return lerp(hi,lo,step(c,0.04045));}
float3 LinearToSRGB(float3 c){c=max(c,0);float3 lo=c*12.92;float3 hi=1.055*pow(c,1.0/2.4)-0.055;return saturate(lerp(hi,lo,step(c,0.0031308)));}
float4 PSConvert(V i):SV_Target{float3 c=T.SampleLevel(S,i.uv+JitterUV,0).rgb;return float4(SRGBToLinear(c),1);}
float3 ApplyVideoAdjustments(float3 c){
    float brightness=ColorA.x;
    float contrast=max(ColorA.y,0.0);
    float saturation=max(ColorA.z,0.0);
    float gamma=max(ColorA.w,0.05);
    float temperature=clamp(ColorB.x,-1.0,1.0);
    float tint=clamp(ColorB.y,-1.0,1.0);

    c=max(c,0.0);
    c*=exp2(brightness);
    c=(c-0.18)*contrast+0.18;
    float l=dot(c,float3(0.2126,0.7152,0.0722));
    c=lerp(l.xxx,c,saturation);
    c*=float3(1.0+0.12*temperature,1.0,1.0-0.12*temperature);
    c*=float3(1.0+0.05*tint,1.0-0.10*tint,1.0+0.05*tint);
    c=pow(max(c,0.0),1.0/gamma);
    return c;
}
float4 PSPresent(V i):SV_Target{float3 c=T.SampleLevel(S,i.uv,0).rgb;c=ApplyVideoAdjustments(c);return float4(LinearToSRGB(c),1);}
float3 hsv2rgb(float3 c){float4 K=float4(1,2.0/3.0,1.0/3.0,3);float3 p=abs(frac(c.xxx+K.xyz)*6-K.www);return c.z*lerp(K.xxx,saturate(p-K.xxx),c.y);}
float4 PSMotion(V i):SV_Target{float2 m=T.SampleLevel(S,i.uv,0).rg;float mag=length(m);float h=frac(atan2(-m.y,m.x)/6.2831853+1.0);float v=saturate(0.22+mag/24.0);float3 c=hsv2rgb(float3(h,saturate(mag/1.0),v));return float4(c,1);}
float4 PSDepth(V i):SV_Target{float d=saturate(T.SampleLevel(S,i.uv,0).r);d=pow(d,0.7);return float4(d,d,d,1);}
    // Depth comes directly from compact-guide B and is written through SV_Depth into
    // the exact typeless/D32 resource that NGX receives later in the frame.
    float PSWriteDepth(V i):SV_Depth{return saturate(T.SampleLevel(S,i.uv+JitterUV,0).b);}
    struct GuideOut{float2 mv:SV_Target0;float bias:SV_Target1;};
    GuideOut PSExpandGuides(V i){float4 g=T.SampleLevel(S,i.uv+JitterUV,0);GuideOut o;o.mv=g.xy;o.bias=g.w>=0.5?1.0:0.0;return o;}
)";
    UINT flags=D3DCOMPILE_OPTIMIZATION_LEVEL3;ComPtr<ID3DBlob>vs,convert,present,motion,depth,depthWrite,expand,err;
    auto C=[&](const char*entry,const char*target,ComPtr<ID3DBlob>&out)->bool{err.Reset();HRESULT hr=D3DCompile(hlsl,strlen(hlsl),nullptr,nullptr,nullptr,entry,target,flags,0,&out,&err);if(FAILED(hr)){if(err)LOG((char*)err->GetBufferPointer());return false;}return true;};
    if(!C("VS","vs_5_1",vs)||!C("PSConvert","ps_5_1",convert)||!C("PSPresent","ps_5_1",present)||!C("PSMotion","ps_5_1",motion)||!C("PSDepth","ps_5_1",depth)||!C("PSWriteDepth","ps_5_1",depthWrite)||!C("PSExpandGuides","ps_5_1",expand))return false;
    D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;range.BaseShaderRegister=0;
    D3D12_ROOT_PARAMETER rp[2]{};rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[0].DescriptorTable.NumDescriptorRanges=1;rp[0].DescriptorTable.pDescriptorRanges=&range;
    rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;rp[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[1].Constants.Num32BitValues=12;rp[1].Constants.ShaderRegister=0;
    D3D12_STATIC_SAMPLER_DESC smp{};smp.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;smp.AddressU=smp.AddressV=smp.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;smp.ShaderRegister=0;smp.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;smp.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=2;rs.pParameters=rp;rs.NumStaticSamplers=1;rs.pStaticSamplers=&smp;rs.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob>sig;if(!HR(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err),"SerializeRootSignature"))return false;
    if(!HR(m_device->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&m_rootSig)),"CreateRootSignature"))return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=m_rootSig.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={convert->GetBufferPointer(),convert->GetBufferSize()};
    p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.BlendState.RenderTarget[1].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.BlendState.RenderTarget[2].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask=UINT_MAX;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
    p.DepthStencilState.DepthEnable=FALSE;p.DepthStencilState.StencilEnable=FALSE;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.SampleDesc={1,0};
    p.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoConvert)),"Create convert PSO"))return false;
    p.PS={present->GetBufferPointer(),present->GetBufferSize()};
    p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoPresent)),"Create present PSO"))return false;
    p.PS={motion->GetBufferPointer(),motion->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoMotionDebug)),"Create MV debug PSO"))return false;
    p.PS={depth->GetBufferPointer(),depth->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoDepthDebug)),"Create depth debug PSO"))return false;
    p.PS={expand->GetBufferPointer(),expand->GetBufferSize()};p.NumRenderTargets=2;p.RTVFormats[0]=DXGI_FORMAT_R16G16_FLOAT;p.RTVFormats[1]=DXGI_FORMAT_R8_UNORM;p.RTVFormats[2]=DXGI_FORMAT_UNKNOWN;
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoExpandGuides)),"Create GPU guide expansion PSO"))return false;
    p.PS={depthWrite->GetBufferPointer(),depthWrite->GetBufferSize()};
    p.NumRenderTargets=0;p.RTVFormats[0]=DXGI_FORMAT_UNKNOWN;p.RTVFormats[1]=DXGI_FORMAT_UNKNOWN;p.RTVFormats[2]=DXGI_FORMAT_UNKNOWN;p.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    p.DepthStencilState.DepthEnable=TRUE;p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;p.DepthStencilState.StencilEnable=FALSE;
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoDepthWrite)),"Create real depth-buffer PSO"))return false;
    return true;
}

bool D3D12Renderer::InitializeDLSS(bool& gpuSynchronized){
    auto* cmd=m_cmds[0].Get();
    m_allocators[0]->Reset();cmd->Reset(m_allocators[0].Get(),nullptr);bool ok=m_dlss.Initialize(m_device.Get(),cmd,m_sourceW,m_sourceH,m_outputW,m_outputH,m_quality);
    if(ok){m_renderW=m_dlss.RenderWidth();m_renderH=m_dlss.RenderHeight();}
    cmd->Close();ID3D12CommandList*l[]={cmd};m_queue->ExecuteCommandLists(1,l);
    gpuSynchronized=WaitGPUForContinuedUse();
    return ok&&gpuSynchronized;
}

bool D3D12Renderer::CreateUploadForTexture(const D3D12_RESOURCE_DESC&desc,ComPtr<ID3D12Resource>&upload,uint8_t*&mapped,D3D12_PLACED_SUBRESOURCE_FOOTPRINT&fp,uint32_t&rows,uint64_t&rowBytes,uint64_t&total,const char*name){
    m_device->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&rowBytes,&total);D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=total;b.Height=1;b.DepthOrArraySize=1;b.MipLevels=1;b.SampleDesc={1,0};b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto hp=HeapProps(D3D12_HEAP_TYPE_UPLOAD);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),name))return false;D3D12_RANGE r{0,0};return HR(upload->Map(0,&r,reinterpret_cast<void**>(&mapped)),"Map upload resource");
}

bool D3D12Renderer::CreateVideoResources(){
    auto hp=HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto src=Tex2D(DXGI_FORMAT_B8G8R8A8_UNORM,m_sourceW,m_sourceH,D3D12_RESOURCE_FLAG_NONE);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&src,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_decodedTexture)),"Create decoded texture"))return false;
    m_decodedTexture->SetName(L"Video_Decoded_BGRA_sRGB");
    for(uint32_t i=0;i<FrameCount;++i) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; uint32_t rows=0; uint64_t rowBytes=0,total=0;
        if(!CreateUploadForTexture(src,m_upload[i],m_uploadMapped[i],fp,rows,rowBytes,total,"Create video upload"))return false;
        if(i==0){m_uploadFootprint=fp;m_numRows=rows;m_rowSize=rowBytes;m_uploadBytes=total;}
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
    srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;m_device->CreateShaderResourceView(m_decodedTexture.Get(),&srv,SRVCPU(0));

    D3D12_CLEAR_VALUE cv{};cv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;auto col=Tex2D(cv.Format,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&col,D3D12_RESOURCE_STATE_RENDER_TARGET,&cv,IID_PPV_ARGS(&m_dlssColor)),"Create DLSS color"))return false;m_dlssColor->SetName(L"DLSS_Color_Input_Linear_FP16");m_device->CreateRenderTargetView(m_dlssColor.Get(),nullptr,RTV(FrameCount));
    srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;m_device->CreateShaderResourceView(m_dlssColor.Get(),&srv,SRVCPU(4));

    auto mot=Tex2D(DXGI_FORMAT_R16G16_FLOAT,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&mot,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_motion)),"Create motion guide"))return false;
    m_motion->SetName(L"DLSS_MotionVectors_CurrentToPrevious_RG16F");srv.Format=DXGI_FORMAT_R16G16_FLOAT;m_device->CreateShaderResourceView(m_motion.Get(),&srv,SRVCPU(2));m_device->CreateRenderTargetView(m_motion.Get(),nullptr,RTV(FrameCount+1));

    // One depth resource, two views: D32_FLOAT DSV for real depth writes / ReShade
    // discovery and R32_FLOAT SRV for debug/NGX sampling. Passing this exact resource
    // to NGX avoids the old "R32 proxy + unrelated mirrored D32" ambiguity.
    D3D12_CLEAR_VALUE dcv{};dcv.Format=DXGI_FORMAT_D32_FLOAT;dcv.DepthStencil.Depth=1.0f;dcv.DepthStencil.Stencil=0;
    auto dep=Tex2D(DXGI_FORMAT_R32_TYPELESS,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&dep,D3D12_RESOURCE_STATE_DEPTH_WRITE,&dcv,IID_PPV_ARGS(&m_depth)),"Create unified DLSS depth"))return false;
    m_depth->SetName(L"DLSS_Depth_R32_TYPELESS_D32_DSV_R32_SRV");
    srv.Format=DXGI_FORMAT_R32_FLOAT;m_device->CreateShaderResourceView(m_depth.Get(),&srv,SRVCPU(3));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=DXGI_FORMAT_D32_FLOAT;dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;m_device->CreateDepthStencilView(m_depth.Get(),&dsv,DSV());

    auto bias=Tex2D(DXGI_FORMAT_R8_UNORM,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bias,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_biasCurrent)),"Create BiasCurrentColor mask"))return false;
    m_biasCurrent->SetName(L"DLSS_BiasCurrentColor_Disocclusion_R8");srv.Format=DXGI_FORMAT_R8_UNORM;m_device->CreateShaderResourceView(m_biasCurrent.Get(),&srv,SRVCPU(5));m_device->CreateRenderTargetView(m_biasCurrent.Get(),nullptr,RTV(FrameCount+2));
    auto grid=Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT,m_gridW,m_gridH,D3D12_RESOURCE_FLAG_NONE);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&grid,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_guideGrid)),"Create compact temporal guide grid"))return false;
    m_guideGrid->SetName(L"DLSS_CompactGuideGrid_RGBA32F");
    for(uint32_t i=0;i<FrameCount;++i) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; uint32_t rows=0; uint64_t rowBytes=0,total=0;
        if(!CreateUploadForTexture(grid,m_guideUpload[i],m_guideMapped[i],fp,rows,rowBytes,total,"Create compact guide upload"))return false;
        if(i==0){m_guideFootprint=fp;m_guideRows=rows;m_guideRowSize=rowBytes;m_guideUploadBytes=total;}
    }
    srv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;m_device->CreateShaderResourceView(m_guideGrid.Get(),&srv,SRVCPU(6));
    auto out=Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,m_outputW,m_outputH,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&m_dlssOutput)),"Create DLSS output"))return false;
    m_dlssOutput->SetName(L"DLSS_Output_Linear_FP16_UAV");
    srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;m_device->CreateShaderResourceView(m_dlssOutput.Get(),&srv,SRVCPU(1));
    LOG("DLSS resource contract ready: Color=R16G16B16A16_FLOAT " << m_renderW << "x" << m_renderH
        << ", MV=R16G16_FLOAT " << m_renderW << "x" << m_renderH
        << ", Depth=R32_TYPELESS resource / D32_FLOAT DSV / R32_FLOAT SRV " << m_renderW << "x" << m_renderH
        << ", BiasCurrentColor=R8_UNORM " << m_renderW << "x" << m_renderH
        << ", Output=R16G16B16A16_FLOAT UAV " << m_outputW << "x" << m_outputH
        << ", CompactGrid=R32G32B32A32_FLOAT " << m_gridW << "x" << m_gridH << " -> GPU MV/bias expansion + direct SV_Depth write");
    return true;
}

void D3D12Renderer::CopyMappedRows(uint8_t*mapped,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT&fp,const void*src,size_t tight,uint32_t rows){const uint8_t*s=static_cast<const uint8_t*>(src);for(uint32_t y=0;y<rows;++y)memcpy(mapped+fp.Offset+size_t(fp.Footprint.RowPitch)*y,s+tight*y,tight);}

float D3D12Renderer::Halton(uint32_t index,uint32_t base){float f=1.0f,r=0.0f;while(index){f/=float(base);r+=f*float(index%base);index/=base;}return r;}

bool D3D12Renderer::RenderFrame(const uint8_t*bgra,size_t bytes,const float*guideGridRGBA32F,size_t guideBytes,uint32_t gridW,uint32_t gridH,bool temporalReset,float frameTimeMs){
    if(m_gpuUnusable)return false;
    const size_t videoRow=size_t(m_sourceW)*4u,guideRow=size_t(m_gridW)*sizeof(float)*4u;
    if(!bgra||bytes<videoRow*m_sourceH||!guideGridRGBA32F||gridW!=m_gridW||gridH!=m_gridH||guideBytes<guideRow*m_gridH)return false;
    const uint32_t slot=m_frameSlot%FrameCount;
    if(!WaitForFrameSlot(slot)) return false;
    CopyMappedRows(m_uploadMapped[slot],m_uploadFootprint,bgra,videoRow,m_sourceH);
    CopyMappedRows(m_guideMapped[slot],m_guideFootprint,guideGridRGBA32F,guideRow,m_gridH);
    if(!HR(m_allocators[slot]->Reset(),"Reset frame allocator")) return false;
    auto* cmd=m_cmds[slot].Get();
    if(!HR(cmd->Reset(m_allocators[slot].Get(),nullptr),"Reset frame command list")) return false;
    ID3D12DescriptorHeap*heaps[]={m_srvHeap.Get()};cmd->SetDescriptorHeaps(1,heaps);

    if(!m_sourceInCopyDest)Barrier(cmd,m_decodedTexture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION d{};d.pResource=m_decodedTexture.Get();d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION s{};s.pResource=m_upload[slot].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;s.PlacedFootprint=m_uploadFootprint;cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);Barrier(cmd,m_decodedTexture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_sourceInCopyDest=false;

    if(!m_gridInCopyDest)Barrier(cmd,m_guideGrid.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    d.pResource=m_guideGrid.Get();s.pResource=m_guideUpload[slot].Get();s.PlacedFootprint=m_guideFootprint;cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);
    Barrier(cmd,m_guideGrid.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_gridInCopyDest=false;

    // One temporal jitter sample drives BOTH the color reconstruction input and the
    // spatial lookup of all guide buffers.  The motion-vector VALUES themselves remain
    // unjittered (hence no MVJittered create flag), matching the standard DLSS contract.
    const float jitterX=Halton(uint32_t(m_framesPresented%1024)+1,2)-0.5f;
    const float jitterY=Halton(uint32_t(m_framesPresented%1024)+1,3)-0.5f;
    const float jitterUVX=jitterX/float(m_renderW), jitterUVY=jitterY/float(m_renderH);

    // GPU-expand the compact CPU optical-flow/mask analysis to exact DLSS input
    // resolution. Depth is deliberately NOT mirrored through a color RT anymore:
    // it is written directly into the same typeless depth resource that NGX receives.
    if(!m_guidesInRT){Barrier(cmd,m_motion.Get(),GuideReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);Barrier(cmd,m_biasCurrent.Get(),GuideReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);}m_guidesInRT=true;
    D3D12_VIEWPORT gvp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT gsc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&gvp);cmd->RSSetScissorRects(1,&gsc);
    D3D12_CPU_DESCRIPTOR_HANDLE grt[2]={RTV(FrameCount+1),RTV(FrameCount+2)};cmd->OMSetRenderTargets(2,grt,FALSE,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoExpandGuides.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(6));float guideParams[4]={jitterUVX,jitterUVY,0,0};cmd->SetGraphicsRoot32BitConstants(1,4,guideParams,0);cmd->DrawInstanced(3,1,0,0);
    Barrier(cmd,m_motion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);Barrier(cmd,m_biasCurrent.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);m_guidesInRT=false;

    // Populate the exact depth resource passed to NGX. The resource is R32_TYPELESS,
    // viewed as D32_FLOAT while writing and R32_FLOAT while sampling/debugging.
    if(!m_depthInWrite)Barrier(cmd,m_depth.Get(),DepthGuideReadState,D3D12_RESOURCE_STATE_DEPTH_WRITE);m_depthInWrite=true;
    D3D12_VIEWPORT dvp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT dsc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&dvp);cmd->RSSetScissorRects(1,&dsc);
    auto dsvh=DSV();cmd->OMSetRenderTargets(0,nullptr,FALSE,&dsvh);cmd->ClearDepthStencilView(dsvh,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoDepthWrite.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(6));cmd->SetGraphicsRoot32BitConstants(1,4,guideParams,0);cmd->DrawInstanced(3,1,0,0);
    Barrier(cmd,m_depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,DepthGuideReadState);m_depthInWrite=false;

    if(!m_colorInRT)Barrier(cmd,m_dlssColor.Get(),GuideReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);m_colorInRT=true;
    D3D12_VIEWPORT vp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT sc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&vp);cmd->RSSetScissorRects(1,&sc);
    auto crt=RTV(FrameCount);cmd->OMSetRenderTargets(1,&crt,FALSE,nullptr);const float black[4]={0,0,0,1};cmd->ClearRenderTargetView(crt,black,0,nullptr);cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoConvert.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(0));
    float params[4]={jitterUVX,jitterUVY,0,0};cmd->SetGraphicsRoot32BitConstants(1,4,params,0);cmd->DrawInstanced(3,1,0,0);Barrier(cmd,m_dlssColor.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);m_colorInRT=false;

    ++m_framesPresented;

    // Create/recreate the NGX feature on an open command list, submit that list,
    // and only then evaluate on a fresh list. This mirrors the robust game-style
    // NGX lifetime instead of relying on CreateFeature and EvaluateFeature being
    // accepted back-to-back before the creation commands have reached the GPU.
    // Intentionally allow one complete Present before the first NGX CreateFeature.
    // ReShade add-ons finish their swapchain/runtime initialization on that first frame;
    // creating on frame 2 makes the raw CreateFeature much harder for RenoDX to miss.
    const auto featureSetup = ngx_session_detail::PrepareFeatureForFrame(
        DLSSEnabled(), m_dlss.FeatureCreated(), m_framesPresented,
        m_delayedRecreateDone, m_recreateRequested,
        [&] { return m_dlss.EnsureFeature(cmd); },
        [&] { return m_dlss.RecreateFeature(cmd); });
    const bool needFeatureFlush = featureSetup.needsFlush;
    if (featureSetup.selected) temporalReset = true;
    if (needFeatureFlush) {
        if (!HR(cmd->Close(), "Close command list after NGX CreateFeature")) return false;
        ID3D12CommandList* initLists[] = { cmd };
        m_queue->ExecuteCommandLists(1, initLists);
        if(!WaitGPUForContinuedUse())return false;
        if (!HR(m_allocators[slot]->Reset(), "Reset allocator after NGX CreateFeature")) return false;
        if (!HR(cmd->Reset(m_allocators[slot].Get(), nullptr), "Reset command list after NGX CreateFeature")) return false;
        ID3D12DescriptorHeap* postCreateHeaps[] = { m_srvHeap.Get() };
        cmd->SetDescriptorHeaps(1, postCreateHeaps);
        LOG("NGX feature creation flushed before EvaluateFeature; temporal history reset.");
    }

    bool used=false;if(DLSSEnabled() && m_dlss.FeatureCreated()){
        if(!m_outputInUAV)Barrier(cmd,m_dlssOutput.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);m_outputInUAV=true;
        used=m_dlss.Evaluate(cmd,m_dlssColor.Get(),m_dlssOutput.Get(),m_depth.Get(),m_motion.Get(),m_biasCurrent.Get(),temporalReset,frameTimeMs,jitterX,jitterY);
        if(used){Barrier(cmd,m_dlssOutput.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_outputInUAV=false;}
    }

    m_lastDLSSUsed=used;
    uint32_t bi=m_swapchain->GetCurrentBackBufferIndex();Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);D3D12_VIEWPORT ovp{0,0,float(m_outputW),float(m_outputH),0,1};D3D12_RECT osc{0,0,LONG(m_outputW),LONG(m_outputH)};cmd->RSSetViewports(1,&ovp);cmd->RSSetScissorRects(1,&osc);auto brt=RTV(bi);cmd->OMSetRenderTargets(1,&brt,FALSE,nullptr);cmd->ClearRenderTargetView(brt,black,0,nullptr);cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const bool applyColor=(m_debugView==DebugView::Final);
    const ColorSettings cs=applyColor?m_colorSettings:ColorSettings{};
    float presentParams[12]={0,0,0,0,cs.brightness,cs.contrast,cs.saturation,cs.gamma,cs.temperature,cs.tint,0,0};
    cmd->SetGraphicsRoot32BitConstants(1,12,presentParams,0);
    // DLSS inputs stay shader-readable for NGX. Only the texture selected for the
    // debug/fallback presentation pass is temporarily made pixel-shader readable.
    ID3D12Resource* debugPixelResource=nullptr;
    D3D12_RESOURCE_STATES debugBefore=GuideReadState;
    switch(m_debugView){
        case DebugView::MotionVectors:debugPixelResource=m_motion.Get();cmd->SetPipelineState(m_psoMotionDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(2));break;
        case DebugView::Depth:debugPixelResource=m_depth.Get();debugBefore=DepthGuideReadState;cmd->SetPipelineState(m_psoDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(3));break;
        case DebugView::BiasMask:debugPixelResource=m_biasCurrent.Get();cmd->SetPipelineState(m_psoDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(5));break;
        case DebugView::Input:debugPixelResource=m_dlssColor.Get();cmd->SetPipelineState(m_psoPresent.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));break;
        default:cmd->SetPipelineState(m_psoPresent.Get());if(used)cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(1));else{debugPixelResource=m_dlssColor.Get();cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));}break;
    }
    if(debugPixelResource)Barrier(cmd,debugPixelResource,debugBefore,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->DrawInstanced(3,1,0,0);
    if(debugPixelResource)Barrier(cmd,debugPixelResource,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,debugBefore);
    Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    if(!HR(cmd->Close(),"Close frame command list")) return false;
    ID3D12CommandList*ls[]={cmd};m_queue->ExecuteCommandLists(1,ls);
    HRESULT phr=m_swapchain->Present(0,m_allowTearing?DXGI_PRESENT_ALLOW_TEARING:0);
    if(FAILED(phr)){LOG("Present failed hr=0x"<<std::hex<<phr);return false;}
    return SignalFrameSlot(slot);
}

bool D3D12Renderer::PresentCurrent(){
    if(m_gpuUnusable||!m_swapchain||!m_queue||!m_rootSig)return false;
    const uint32_t slot=m_frameSlot%FrameCount;
    if(!WaitForFrameSlot(slot))return false;
    if(!HR(m_allocators[slot]->Reset(),"Reset static-present allocator"))return false;
    auto* cmd=m_cmds[slot].Get();
    if(!HR(cmd->Reset(m_allocators[slot].Get(),nullptr),"Reset static-present command list"))return false;
    ID3D12DescriptorHeap*heaps[]={m_srvHeap.Get()};cmd->SetDescriptorHeaps(1,heaps);

    const float black[4]={0,0,0,1};
    uint32_t bi=m_swapchain->GetCurrentBackBufferIndex();
    Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_VIEWPORT ovp{0,0,float(m_outputW),float(m_outputH),0,1};
    D3D12_RECT osc{0,0,LONG(m_outputW),LONG(m_outputH)};
    cmd->RSSetViewports(1,&ovp);cmd->RSSetScissorRects(1,&osc);
    auto brt=RTV(bi);cmd->OMSetRenderTargets(1,&brt,FALSE,nullptr);cmd->ClearRenderTargetView(brt,black,0,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const bool applyColor=(m_debugView==DebugView::Final);
    const ColorSettings cs=applyColor?m_colorSettings:ColorSettings{};
    float presentParams[12]={0,0,0,0,cs.brightness,cs.contrast,cs.saturation,cs.gamma,cs.temperature,cs.tint,0,0};
    cmd->SetGraphicsRoot32BitConstants(1,12,presentParams,0);

    ID3D12Resource* debugPixelResource=nullptr;
    D3D12_RESOURCE_STATES debugBefore=GuideReadState;
    switch(m_debugView){
        case DebugView::MotionVectors:debugPixelResource=m_motion.Get();cmd->SetPipelineState(m_psoMotionDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(2));break;
        case DebugView::Depth:debugPixelResource=m_depth.Get();debugBefore=DepthGuideReadState;cmd->SetPipelineState(m_psoDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(3));break;
        case DebugView::BiasMask:debugPixelResource=m_biasCurrent.Get();cmd->SetPipelineState(m_psoDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(5));break;
        case DebugView::Input:debugPixelResource=m_dlssColor.Get();cmd->SetPipelineState(m_psoPresent.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));break;
        default:
            cmd->SetPipelineState(m_psoPresent.Get());
            if(m_lastDLSSUsed&&DLSSEnabled())cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(1));
            else{debugPixelResource=m_dlssColor.Get();cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));}
            break;
    }
    if(debugPixelResource)Barrier(cmd,debugPixelResource,debugBefore,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->DrawInstanced(3,1,0,0);
    if(debugPixelResource)Barrier(cmd,debugPixelResource,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,debugBefore);
    Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    if(!HR(cmd->Close(),"Close static-present command list"))return false;
    ID3D12CommandList*ls[]={cmd};m_queue->ExecuteCommandLists(1,ls);
    HRESULT phr=m_swapchain->Present(0,m_allowTearing?DXGI_PRESENT_ALLOW_TEARING:0);
    if(FAILED(phr)){LOG("Static Present failed hr=0x"<<std::hex<<phr);return false;}
    return SignalFrameSlot(slot);
}

void D3D12Renderer::Barrier(ID3D12GraphicsCommandList*cmd,ID3D12Resource*res,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){if(a==b)return;auto x=Transition(res,a,b);cmd->ResourceBarrier(1,&x);}
bool D3D12Renderer::WaitForFrameSlot(uint32_t slot){
    if(slot>=FrameCount||!m_fence||!m_fenceEvent)return false;
    const uint64_t v=m_frameFence[slot];
    if(!v)return true;
    const auto waitResult=d3d12_renderer_detail::WaitForGPUFenceCompletion(
        v,
        GetTickCount64(),
        [&]{return m_fence->GetCompletedValue();},
        [&](uint64_t value){return m_fence->SetEventOnCompletion(value,m_fenceEvent);},
        [&](DWORD timeout){return WaitForSingleObject(m_fenceEvent,timeout);});
    const auto result=d3d12_renderer_detail::ClassifyFenceWaitFailure(
        waitResult,[&]{return m_device->GetDeviceRemovedReason();});
    if(result!=d3d12_renderer_detail::FenceWaitResult::Completed){m_gpuUnusable=true;m_lastFenceWaitResult=result;}
    return result==d3d12_renderer_detail::FenceWaitResult::Completed;
}
bool D3D12Renderer::SignalFrameSlot(uint32_t slot){
    if(m_gpuUnusable||slot>=FrameCount)return false;
    const uint64_t v=m_fenceValue+1;
    HRESULT signalResult=E_FAIL;
#if defined(D3D12_RENDERER_TESTING)
    if(m_testFrameSignal)signalResult=m_testFrameSignal(v);
    else
#endif
    {
        if(!m_queue||!m_fence)return false;
        signalResult=m_queue->Signal(m_fence.Get(),v);
    }
    if(FAILED(signalResult)){
        HRESULT removedReason=S_OK;
#if defined(D3D12_RENDERER_TESTING)
        if(m_testDeviceRemovedReason)removedReason=m_testDeviceRemovedReason();
        else
#endif
        if(m_device)removedReason=m_device->GetDeviceRemovedReason();
        m_lastFenceWaitResult=d3d12_renderer_detail::ClassifyFenceWaitFailure(
            d3d12_renderer_detail::FenceWaitResult::SignalFailed,
            [=]{return removedReason;});
        m_gpuUnusable=true;
        return false;
    }
    m_fenceValue=v;
    m_frameFence[slot]=v;
    m_frameSlot=(slot+1u)%FrameCount;
    return true;
}
d3d12_renderer_detail::FenceWaitResult D3D12Renderer::WaitGPU(){
#if defined(D3D12_RENDERER_TESTING)
    if(m_testWaitGPU){
        const auto result=m_testWaitGPU();m_lastFenceWaitResult=result;
        if(result!=d3d12_renderer_detail::FenceWaitResult::Completed)m_gpuUnusable=true;
        return result;
    }
#endif
    if(!m_queue||!m_fence||!m_fenceEvent)return d3d12_renderer_detail::FenceWaitResult::Completed;
    const uint64_t v=++m_fenceValue;
    const auto result=d3d12_renderer_detail::WaitForGPUFenceTeardown(
        v,
        [&](uint64_t value){return m_queue->Signal(m_fence.Get(),value);},
        [&]{return m_fence->GetCompletedValue();},
        [&](uint64_t value){return m_fence->SetEventOnCompletion(value,m_fenceEvent);},
        [&](DWORD timeout){return WaitForSingleObject(m_fenceEvent,timeout);},
        [&]{return m_device->GetDeviceRemovedReason();});
    if(result!=d3d12_renderer_detail::FenceWaitResult::Completed)
        LOG("GPU fence wait failed.");
    m_lastFenceWaitResult=result;
    if(result!=d3d12_renderer_detail::FenceWaitResult::Completed)m_gpuUnusable=true;
    return result;
}
bool D3D12Renderer::WaitGPUForContinuedUse(){
    const bool completed=WaitGPU()==d3d12_renderer_detail::FenceWaitResult::Completed;
    if(!completed)m_gpuUnusable=true;
    return completed;
}
d3d12_renderer_detail::FenceWaitResult D3D12Renderer::DrainForRetirement(){
    if(m_gpuUnusable)return m_lastFenceWaitResult;
    return WaitGPU();
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::RTV(uint32_t i)const{auto h=m_rtvHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*m_rtvInc;return h;}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::DSV()const{return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::SRVCPU(uint32_t i)const{auto h=m_srvHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*m_srvInc;return h;}
D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::SRVGPU(uint32_t i)const{auto h=m_srvHeap->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(i)*m_srvInc;return h;}
