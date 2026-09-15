#include "D3D12Renderer.h"
#include "D3D12FenceWait.h"
#include "TemporalGuides.h"
#include "Log.h"
#include "NvofResolveShader.h"
#include "RuntimePolicy.h"
#include "GpuPreference.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <limits>
#include <vector>

#include "ParallelFor.h"

using Microsoft::WRL::ComPtr;

namespace {

// Full-frame memory passes are bandwidth bound, so they are split only once they are
// large enough that the dispatch pays for itself. Roughly a quarter of a 1080p frame.
constexpr size_t kParallelCopyGrain = 2u * 1024u * 1024u;
constexpr size_t kParallelRowGrain = 64u;

// The readback copy runs on the offline export's resolve worker while the render loop is
// still building the next frame's guides, and both fan out. A pool carries one task slot,
// so sharing the default one would let the two dispatches overwrite each other. The
// workers park on semaphores, so the second pool costs its stacks and nothing else.
//
// Its width is deliberately a fraction of the machine. The copy no longer has to finish
// inside the loop, only before the next drain, so spare width buys nothing; meanwhile the
// export's real competition for cores is the ffmpeg children, which both decode the source
// in software and run the encoder. Taking every core for a copy that has a whole iteration
// to finish in starves them, and their back pressure lands on the render loop anyway.
// Whether that trade is real is visible in the stage table: the resolve wait row measures
// the copy failing to keep up, and the write row measures the encoder failing to.
constexpr size_t kCaptureCopyWidthDivisor = 4;

parallel_detail::WorkerPool& CaptureCopyPool()
{
    static parallel_detail::WorkerPool pool(
        std::max<size_t>(2u, parallel_detail::WorkerPool::DefaultWidth() / kCaptureCopyWidthDivisor));
    return pool;
}

// Local because every converter in this tree is private to the file that
// needs one, and a single log line does not justify a shared header the
// render loop would then have to carry.
std::string WideToUtf8(std::wstring_view text)
{
    if(text.empty())return {};
    const int length=static_cast<int>(text.size());
    const int size=WideCharToMultiByte(CP_UTF8,0,text.data(),length,nullptr,0,nullptr,nullptr);
    if(size<=0)return "<wide-string conversion failed>";
    std::string result(static_cast<size_t>(size),'\0');
    if(WideCharToMultiByte(CP_UTF8,0,text.data(),length,result.data(),size,nullptr,nullptr)!=size)
        return "<wide-string conversion failed>";
    return result;
}

// The adapter the device was actually created on, against the one the
// high-performance policy classified. A hybrid laptop can place this process
// on either GPU, and until this line existed nothing in the log told the two
// apart: the cache identity, the receipt's GPU label and the render-pace
// prior are all derived from the policy's pick, so a device on any other
// adapter leaves all three describing a part that rendered nothing. Both
// sides are printed every time and compared by LUID, never by description -
// two identical cards share a description, and a laptop report has to be
// readable without the machine in front of you. The policy's second
// enumeration costs one DXGI factory per device creation.
void LogDeviceAdapter(const DXGI_ADAPTER_DESC1& device)
{
    const DetectedGpu policy=DetectHighPerformanceGpu();
    const uint64_t deviceLuid=PackAdapterLuid(device.AdapterLuid.HighPart,device.AdapterLuid.LowPart);
    const char* verdict="cannot be compared with";
    switch(CompareAdapterLuids(deviceLuid,policy.adapterLuid)){
        case AdapterMatch::Same: verdict="is"; break;
        case AdapterMatch::Different: verdict="is NOT"; break;
        case AdapterMatch::Unknown: break;
    }
    LOG("D3D12 device adapter \""<<WideToUtf8(device.Description)<<"\" luid=0x"<<std::hex<<deviceLuid<<std::dec
        <<" vendor=0x"<<std::hex<<device.VendorId<<std::dec<<" vram="<<(device.DedicatedVideoMemory>>20)<<"MiB "
        <<verdict<<" the high-performance adapter \""<<WideToUtf8(policy.description)<<"\" luid=0x"
        <<std::hex<<policy.adapterLuid<<std::dec
        <<" that the cache identity, the receipt GPU label and the pace prior describe");
}

} // namespace

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
    if(m_testHooks)m_testHooks->ownedResource.reset();
    // The flow engine holds registered views of textures this object owns, so it has to
    // be torn down while the device and queue are still alive and only once the GPU has
    // stopped touching those surfaces. Member destruction order cannot express that:
    // m_nvof is declared ahead of the queue and the device, so it would be released
    // after both had gone - which is an access violation on the way out, not a leak.
    if (m_queue && m_fence && m_fenceEvent) WaitGPUForContinuedUse();
    m_nvof.Shutdown();
    for (uint32_t i=0;i<FrameCount;++i) {
        if (m_upload[i] && m_uploadMapped[i]) m_upload[i]->Unmap(0,nullptr);
        if (m_guideUpload[i] && m_guideMapped[i]) m_guideUpload[i]->Unmap(0,nullptr);
        if (m_referenceUpload[i] && m_referenceMapped[i]) m_referenceUpload[i]->Unmap(0,nullptr);
        m_uploadMapped[i]=nullptr;
        m_guideMapped[i]=nullptr;
        m_referenceMapped[i]=nullptr;
    }
    for (uint32_t i=0;i<CaptureSlots;++i) {
        if (m_cacheReadback[i] && m_cacheReadbackMapped[i]) m_cacheReadback[i]->Unmap(0,nullptr);
        m_cacheReadbackMapped[i]=nullptr;
    }
    if (m_timestampReadback && m_timestampMapped) m_timestampReadback->Unmap(0,nullptr);
    m_timestampMapped=nullptr;
    LOG("Renderer teardown: buffers unmapped");
    m_dlss.Shutdown();
    LOG("Renderer teardown: NGX released");
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
}

bool D3D12Renderer::Initialize(HWND hwnd,uint32_t sourceW,uint32_t sourceH,uint32_t outputW,uint32_t outputH,uint32_t gridW,uint32_t gridH,NVSDK_NGX_PerfQuality_Value quality,bool preserveSource) {
    m_preserveSource=preserveSource;
    m_hwnd=hwnd; m_sourceW=sourceW; m_sourceH=sourceH; m_outputW=outputW; m_outputH=outputH; m_gridW=gridW; m_gridH=gridH; m_quality=quality;
    if(!m_gridW||!m_gridH)return false;
    // NV12 planes need even dimensions, so an odd source keeps the BGRA upload
    // rather than losing a row or a column. Resolved here rather than with the
    // video resources it sizes, because CreatePipelines below only compiles the
    // source-conversion pass when the answer is NV12.
    m_sourceLayout=(m_requestedSourceLayout==PixelLayout::Nv12&&!(m_sourceW%2)&&!(m_sourceH%2))
        ?PixelLayout::Nv12:PixelLayout::Bgra;
    if(m_requestedSourceLayout==PixelLayout::Nv12&&m_sourceLayout==PixelLayout::Bgra)
        LOG("NV12 source needs even dimensions; taking BGRA at "<<m_sourceW<<"x"<<m_sourceH<<".");
    if(!CreateDeviceAndSwapchain(hwnd) || !CreateHeapsAndBackbuffers() || !CreatePipelines()) return false;
    bool gpuSynchronized=false;
    if(!InitializeDLSS(gpuSynchronized)) {
        if(!gpuSynchronized)return false;
        LOG("DLSS unavailable; using D3D12 scaler fallback.");
        m_renderW=sourceW; m_renderH=sourceH;
    }
    if(!CreateVideoResources()) return false;
    LOG("V11 guide contract: compact CPU optical-flow grid expanded on GPU into full R16G16_FLOAT MVs; depth is written directly into the same R32_TYPELESS/D32_FLOAT resource passed to NGX; temporal reset only on discontinuities.");
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
    if(FAILED(m_adapter.As(&m_adapter3)))m_adapter3.Reset();
    if(!HR(D3D12CreateDevice(m_adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&m_device)),"D3D12CreateDevice"))return false;
    DXGI_ADAPTER_DESC1 ad{};m_adapter->GetDesc1(&ad);LogDeviceAdapter(ad);
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(!HR(m_device->CreateCommandQueue(&q,IID_PPV_ARGS(&m_queue)),"CreateCommandQueue"))return false;
    if(FAILED(m_queue->GetTimestampFrequency(&m_timestampFrequency)))m_timestampFrequency=0;
    for(uint32_t i=0;i<FrameCount;++i) {
        if(!HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&m_allocators[i])),"CreateCommandAllocator"))return false;
        if(!HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&m_uploadAllocators[i])),"CreateCommandAllocator (upload)"))return false;
    }
    for(uint32_t i=0;i<FrameCount;++i) {
        if(!HR(m_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,m_allocators[i].Get(),nullptr,IID_PPV_ARGS(&m_cmds[i])),"CreateCommandList"))return false;
        m_cmds[i]->Close();
        if(!HR(m_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,m_uploadAllocators[i].Get(),nullptr,IID_PPV_ARGS(&m_uploadCmds[i])),"CreateCommandList (upload)"))return false;
        m_uploadCmds[i]->Close();
    }
    BOOL tearing=FALSE;if(m_requestedTearing&&SUCCEEDED(m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing))))m_allowTearing=tearing==TRUE;
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=m_outputW;sd.Height=m_outputH;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc={1,0};sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount=SwapchainBuffers;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sd.Scaling=DXGI_SCALING_STRETCH;sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE;sd.Flags=m_allowTearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
    ComPtr<IDXGISwapChain1>sc1;if(!HR(m_factory->CreateSwapChainForHwnd(m_queue.Get(),hwnd,&sd,nullptr,nullptr,&sc1),"CreateSwapChainForHwnd"))return false;
    m_factory->MakeWindowAssociation(hwnd,DXGI_MWA_NO_ALT_ENTER);sc1.As(&m_swapchain);
    if(m_swapchain) m_swapchain->SetMaximumFrameLatency(2);
    if(!HR(m_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&m_fence)),"CreateFence"))return false;
    m_fenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);return m_fenceEvent!=nullptr;
}

bool D3D12Renderer::CreateHeapsAndBackbuffers(){
    D3D12_DESCRIPTOR_HEAP_DESC rh{};rh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rh.NumDescriptors=RTVCount;
    if(!HR(m_device->CreateDescriptorHeap(&rh,IID_PPV_ARGS(&m_rtvHeap)),"Create RTV heap"))return false;m_rtvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for(uint32_t i=0;i<SwapchainBuffers;++i){if(!HR(m_swapchain->GetBuffer(i,IID_PPV_ARGS(&m_backbuffers[i])),"Get backbuffer"))return false;m_device->CreateRenderTargetView(m_backbuffers[i].Get(),nullptr,RTV(i));}
    D3D12_DESCRIPTOR_HEAP_DESC sh{};sh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;sh.NumDescriptors=SRVCount;sh.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(!HR(m_device->CreateDescriptorHeap(&sh,IID_PPV_ARGS(&m_srvHeap)),"Create SRV heap"))return false;
    m_srvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC dh{};dh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;dh.NumDescriptors=1;
    if(!HR(m_device->CreateDescriptorHeap(&dh,IID_PPV_ARGS(&m_dsvHeap)),"Create DSV heap"))return false;
    m_dsvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return true;
}

// Every pass but the optical-flow resolve, in one translation-unit-scope string so
// that the source conversion below can be compiled from it a second time - with a
// different set of -D constants - without a device.
inline constexpr char kPresentHlsl[]=R"(
Texture2D T:register(t0); Texture2D Ref:register(t1); SamplerState S:register(s0);
cbuffer Params:register(b0){
    float2 Reserved0;  // was sampling jitter; see the note above PSConvert
    float2 Misc;   // x = one output pixel in UV (divider half-width), y = zoom scale
    float4 ColorA; // brightness, contrast, saturation, gamma
    float4 ColorB; // temperature, tint, neural strength (1 = untouched), luminance-ratio guard
    float4 Compare; // mode (0 neural,1 original,2 blend,3 split,4 wipe), amount|splitX, zoomCenterX, zoomCenterY
    float4 Capture; // xy = one chroma texel in UV, zw reserved
}
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);V o;o.uv=uv;o.p=float4(uv.x*2-1,1-uv.y*2,0,1);return o;}
float3 SRGBToLinear(float3 c){float3 lo=c/12.92;float3 hi=pow(max((c+0.055)/1.055,0),2.4);return lerp(hi,lo,step(c,0.04045));}
float3 LinearToSRGB(float3 c){c=max(c,0);float3 lo=c*12.92;float3 hi=1.055*pow(c,1.0/2.4)-0.055;return saturate(lerp(hi,lo,step(c,0.0031308)));}
// A decoded video frame is an already-sampled, band-limited grid: there is no continuous
// scene behind it to re-sample at a new sub-pixel phase. Offsetting this fetch would only
// convolve the frame with a per-frame bilinear tent (Nyquist gain |1-2f|, so 1.0 at phase 0
// and 0.0 at phase 0.5) and hand the reconstruction a different amount of blur every frame.
// Feature 18 does not read a jitter offset at all, so nothing downstream undoes it either.
float4 PSConvert(V i):SV_Target{float3 c=T.SampleLevel(S,i.uv,0).rgb;return float4(SRGBToLinear(c),1);}
float Luma709(float3 c){return dot(c,float3(0.2126,0.7152,0.0722));}
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
    float l=Luma709(c);
    c=lerp(l.xxx,c,saturation);
    c*=float3(1.0+0.12*temperature,1.0,1.0-0.12*temperature);
    c*=float3(1.0+0.05*tint,1.0-0.10*tint,1.0+0.05*tint);
    c=pow(max(c,0.0),1.0/gamma);
    return c;
}
// The strength dial is a presentation composite, not a second neural pass: the add-on
// overwrites the NGX output in place, so T already holds the composed neural frame and
// Ref the original. Re-mixing those two is the whole dial, which is why moving it costs
// a present and not a re-render. Below 1 it mixes back toward the original; above 1 it
// extends the luminance RATIO the model produced, never an additive delta, because a
// ratio applied as one scalar to the whole triple keeps the hue where the model put it.
float3 ApplyNeuralStrength(float3 c,float3 ref,float s,float guard){
    if(s<1.0)return lerp(ref,c,max(s,0.0));
    // The 1/512 floor on both terms stops a near-black pixel from producing an
    // unbounded ratio, and the two-sided guard bounds how far one pixel may travel.
    float ratio=clamp((Luma709(c)+1.0/512.0)/(Luma709(ref)+1.0/512.0),1.0/guard,guard);
    c*=pow(ratio,s-1.0);
    // One scalar on the triple can push a saturated channel past 1. A luminance-only
    // knee would let that channel clip on encode and rotate the hue, so normalise by
    // the peak instead, which keeps the ratio between the channels intact.
    float peak=max(c.r,max(c.g,c.b));
    return peak>1.0?c/peak:c;
}
// Zoom first, then the strength composite, then choose the pair member, then the shared
// color adjustments. Ref is the source-size sRGB original; T is the linear neural output
// or converted input. The composite runs before the member selection so that Original
// (mode 1) still shows the untouched original and every comparison mode shows the dialled
// neural frame on its own side of the divider.
float4 PSPresent(V i):SV_Target{
    float zoom=max(Misc.y,0.01);
    float2 zc=Compare.zw;
    float2 uv=saturate((i.uv-zc)/zoom+zc);
    float3 c=T.SampleLevel(S,uv,0).rgb;
    int mode=int(Compare.x+0.5);
    float strength=ColorB.z;
    // The dial needs the original in the plain neural view too, so Ref is sampled when
    // the mode reads it OR the dial is off its default. At strength 1 nothing here runs:
    // the fetch is skipped and c reaches the adjustments as the untouched neural pixel.
    if(mode!=0||strength!=1.0){
        float3 ref=SRGBToLinear(Ref.SampleLevel(S,uv,0).rgb);
        if(strength!=1.0)c=ApplyNeuralStrength(c,ref,strength,max(ColorB.w,1.0));
        if(mode==1)c=ref;
        else if(mode==2)c=lerp(ref,c,saturate(Compare.y));
        else if(mode!=0)c=uv.x<Compare.y?ref:c;
    }
    c=ApplyVideoAdjustments(c);
    if(mode==4){
        float screenSplit=(Compare.y-zc.x)*zoom+zc.x;
        float d=abs(i.uv.x-screenSplit);
        // A one-pixel white line disappears into bright content - a divider down
        // a white shirt or a sky was invisible, which is the one thing this mode
        // exists to show. A white core inside a dark edge always leaves one of
        // the two with contrast against whatever it lands on.
        if(d<Misc.x*2.5)c=0.0;
        if(d<Misc.x)c=1.0;
    }
    return float4(LinearToSRGB(c),1);
}
// GPU colour conversion for the NV12 capture path. The picture is exactly what the
// cache-capture pass produces; only the encoding differs, from 8-bit BGRA to BT.709
// limited-range Y and interleaved UV, so ffmpeg never converts a frame on the CPU.
float3 CaptureRGB(float2 uv){return LinearToSRGB(ApplyVideoAdjustments(T.SampleLevel(S,uv,0).rgb));}
float CaptureY(float3 c){return (16.0+219.0*Luma709(c))/255.0;}
float2 CaptureChromaOf(float3 c){
    float u=128.0+224.0*dot(c,float3(-0.114572,-0.385428,0.5));
    float v=128.0+224.0*dot(c,float3(0.5,-0.454153,-0.045847));
    return float2(u,v)/255.0;
}
float PSCaptureLuma(V i):SV_Target{return CaptureY(CaptureRGB(i.uv));}
// One chroma sample covers a 2x2 luma block. The four samples are converted first and
// averaged after, which is what a CPU 4:2:0 conversion does; averaging the colours
// first would pull saturated edges towards grey.
float2 PSCaptureChroma(V i):SV_Target{
    float2 o=Capture.xy*0.25;
    float2 c=CaptureChromaOf(CaptureRGB(i.uv+float2(-o.x,-o.y)));
    c+=CaptureChromaOf(CaptureRGB(i.uv+float2(o.x,-o.y)));
    c+=CaptureChromaOf(CaptureRGB(i.uv+float2(-o.x,o.y)));
    c+=CaptureChromaOf(CaptureRGB(i.uv+float2(o.x,o.y)));
    return c*0.25;
}
// The exact inverse of the capture conversion above, for a source that arrives as NV12
// (Y at t0, interleaved UV at t1, sampled bilinearly at half size). It writes the same
// 8-bit sRGB-encoded BGRA the decoder used to upload, so every pass after the decoded
// texture is unchanged.
//
// SOURCE_* are supplied per renderer by CompileSourceNv12 from the colour description
// the source declared - never from a default. They are preprocessor tokens rather than
// constant-buffer values for one measured reason: fxc folds SOURCE_CHROMA_SCALE into
// each coefficient and SOURCE_LUMA_SCALE into each channel's multiply-add, so with the
// BT.709 limited-range numbers substituted this text is character for character the
// program that shipped, and compiles to byte-identical bytecode. A cbuffer value cannot
// be folded, so a parameterised version of this pass would move the last bits of every
// cached render on disk.
//
// The guard is not decoration: HLSL compiles this whole text for every entry point in
// it, and every other pass is compiled with no SOURCE_* defines at all. Without the
// guard they would all fail on the undeclared identifiers - and with it, a renderer
// that was handed no colour description has no program that could decode YUV under a
// guessed matrix, because this function does not exist in its build of the file.
#ifdef SOURCE_LUMA_OFFSET
float4 PSSourceNv12(V i):SV_Target{
    float y=(T.SampleLevel(S,i.uv,0).r*255.0-SOURCE_LUMA_OFFSET)/SOURCE_LUMA_SCALE;
    float2 c=(Ref.SampleLevel(S,i.uv,0).rg*255.0-128.0)/SOURCE_CHROMA_SCALE;
    float3 rgb=float3(y+SOURCE_RED_V*c.y,y-SOURCE_GREEN_U*c.x-SOURCE_GREEN_V*c.y,y+SOURCE_BLUE_U*c.x);
    return float4(saturate(rgb),1);
}
#endif
float3 hsv2rgb(float3 c){float4 K=float4(1,2.0/3.0,1.0/3.0,3);float3 p=abs(frac(c.xxx+K.xyz)*6-K.www);return c.z*lerp(K.xxx,saturate(p-K.xxx),c.y);}
float4 PSMotion(V i):SV_Target{float2 m=T.SampleLevel(S,i.uv,0).rg;float mag=length(m);float h=frac(atan2(-m.y,m.x)/6.2831853+1.0);float v=saturate(0.22+mag/24.0);float3 c=hsv2rgb(float3(h,saturate(mag/1.0),v));return float4(c,1);}
float4 PSDepth(V i):SV_Target{float d=saturate(T.SampleLevel(S,i.uv,0).r);d=pow(d,0.7);return float4(d,d,d,1);}
    // Depth comes directly from compact-guide B and is written through SV_Depth into
    // the exact typeless/D32 resource that NGX receives later in the frame.
    float PSWriteDepth(V i):SV_Depth{return saturate(T.SampleLevel(S,i.uv,0).b);}
    float2 PSExpandGuides(V i):SV_Target{return T.SampleLevel(S,i.uv,0).xy;}
)";

namespace d3d12_renderer_detail {
const SourceNv12Constants* SourceNv12ConstantsFor(SourceNv12Conversion conversion)
{
    // BT.709 limited range is the program every cached render on disk was made with,
    // so its seven tokens are frozen: 2(1-Kr) and 2(1-Kb) for Kr=0.2126, Kb=0.0722,
    // the two green terms, and the 8-bit studio-swing scalings. BT.601 is the same
    // algebra at Kr=0.299, Kb=0.114. Full range drops the 16 offset and spans the
    // whole byte in both planes.
    static constexpr SourceNv12Constants kBt709Limited{
        "16.0","219.0","224.0","1.5748","0.187324","0.468124","1.8556"};
    static constexpr SourceNv12Constants kBt709Full{
        "0.0","255.0","255.0","1.5748","0.187324","0.468124","1.8556"};
    static constexpr SourceNv12Constants kBt601Limited{
        "16.0","219.0","224.0","1.402","0.344136","0.714136","1.772"};
    static constexpr SourceNv12Constants kBt601Full{
        "0.0","255.0","255.0","1.402","0.344136","0.714136","1.772"};
    switch(conversion){
        case SourceNv12Conversion::Bt709Limited:return &kBt709Limited;
        case SourceNv12Conversion::Bt709Full:return &kBt709Full;
        case SourceNv12Conversion::Bt601Limited:return &kBt601Limited;
        case SourceNv12Conversion::Bt601Full:return &kBt601Full;
        case SourceNv12Conversion::Unsupported:break;
    }
    return nullptr;
}
} // namespace d3d12_renderer_detail

bool D3D12Renderer::CompileSourceNv12(SourceNv12Conversion conversion,ComPtr<ID3DBlob>& blob)
{
    const auto* constants=d3d12_renderer_detail::SourceNv12ConstantsFor(conversion);
    if(!constants){
        LOG("NV12 source conversion has no coefficients for the source's colour description; "
            "refusing to guess a matrix.");
        return false;
    }
    const D3D_SHADER_MACRO defines[]={
        {"SOURCE_LUMA_OFFSET",constants->lumaOffset},
        {"SOURCE_LUMA_SCALE",constants->lumaScale},
        {"SOURCE_CHROMA_SCALE",constants->chromaScale},
        {"SOURCE_RED_V",constants->redV},
        {"SOURCE_GREEN_U",constants->greenU},
        {"SOURCE_GREEN_V",constants->greenV},
        {"SOURCE_BLUE_U",constants->blueU},
        {nullptr,nullptr}};
    ComPtr<ID3DBlob>err;
    const HRESULT hr=D3DCompile(kPresentHlsl,sizeof(kPresentHlsl)-1,nullptr,defines,nullptr,
                                "PSSourceNv12","ps_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&err);
    if(FAILED(hr)){if(err)LOG((char*)err->GetBufferPointer());return false;}
    return true;
}

bool D3D12Renderer::CreatePipelines(){
    UINT flags=D3DCOMPILE_OPTIMIZATION_LEVEL3;ComPtr<ID3DBlob>vs,convert,present,motion,depth,depthWrite,expand,err;
    ComPtr<ID3DBlob>captureLuma,captureChroma,sourceNv12;
    auto C=[&](const char*entry,const char*target,ComPtr<ID3DBlob>&out)->bool{err.Reset();HRESULT hr=D3DCompile(kPresentHlsl,sizeof(kPresentHlsl)-1,nullptr,nullptr,nullptr,entry,target,flags,0,&out,&err);if(FAILED(hr)){if(err)LOG((char*)err->GetBufferPointer());return false;}return true;};
    if(!C("VS","vs_5_1",vs)||!C("PSConvert","ps_5_1",convert)||!C("PSPresent","ps_5_1",present)||!C("PSMotion","ps_5_1",motion)||!C("PSDepth","ps_5_1",depth)||!C("PSWriteDepth","ps_5_1",depthWrite)||!C("PSExpandGuides","ps_5_1",expand)||
       !C("PSCaptureLuma","ps_5_1",captureLuma)||!C("PSCaptureChroma","ps_5_1",captureChroma))return false;
    // Only a renderer that will actually be handed NV12 source frames compiles the
    // conversion, and it compiles exactly the one conversion the source's declared
    // description names. A BGRA source never reaches that draw, so it needs no program
    // and gets none - which is also why there is nowhere for a default matrix to live.
    const SourceNv12Conversion conversion=SourceNv12ConversionFor(m_sourceColor);
    if(m_sourceLayout==PixelLayout::Nv12){
        if(!CompileSourceNv12(conversion,sourceNv12))return false;
        // Third member of the "GPU source conversion" family the decoder's accepted
        // and refused lines belong to, and the only one that is evidence about the
        // program rather than the decision: it names the arm that actually compiled.
        LOG("GPU source conversion compiled: matrix="
            <<(conversion==SourceNv12Conversion::Bt709Limited||conversion==SourceNv12Conversion::Bt709Full?"bt709":"bt601")
            <<" range="
            <<(conversion==SourceNv12Conversion::Bt709Limited||conversion==SourceNv12Conversion::Bt601Limited?"limited":"full")
            <<".");
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    for(uint32_t r=0;r<2;++r){ranges[r].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[r].NumDescriptors=1;ranges[r].BaseShaderRegister=r;}
    // The reference table is two descriptors wide so the flow resolve can read the
    // backward field at t2 beside the cost at t1 without a root parameter nothing else
    // would use. Every other pass declares t1 alone and never touches the slot after it,
    // which is a written descriptor either way because the flow views below are created
    // whether or not the engine came up.
    ranges[1].NumDescriptors=2;
    // [0] t0 current view, [1] t1 comparison reference and t2 backward flow, [2]
    // PresentConstantCount root constants (Params).
    D3D12_ROOT_PARAMETER rp[3]{};
    for(uint32_t r=0;r<2;++r){rp[r].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[r].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[r].DescriptorTable.NumDescriptorRanges=1;rp[r].DescriptorTable.pDescriptorRanges=&ranges[r];}
    rp[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;rp[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[2].Constants.Num32BitValues=PresentConstantCount;rp[2].Constants.ShaderRegister=0;
    D3D12_STATIC_SAMPLER_DESC smp{};smp.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;smp.AddressU=smp.AddressV=smp.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;smp.ShaderRegister=0;smp.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;smp.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=3;rs.pParameters=rp;rs.NumStaticSamplers=1;rs.pStaticSamplers=&smp;rs.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob>sig;if(!HR(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err),"SerializeRootSignature"))return false;
    if(!HR(m_device->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&m_rootSig)),"CreateRootSignature"))return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=m_rootSig.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={convert->GetBufferPointer(),convert->GetBufferSize()};
    p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask=UINT_MAX;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
    p.DepthStencilState.DepthEnable=FALSE;p.DepthStencilState.StencilEnable=FALSE;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.SampleDesc={1,0};
    p.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoConvert)),"Create convert PSO"))return false;
    p.PS={present->GetBufferPointer(),present->GetBufferSize()};
    p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoPresent)),"Create present PSO"))return false;
    // Cache capture runs the same present shader into a BGRA8 target, so the
    // readback rows already carry the caller's byte order and need no CPU swizzle.
    p.RTVFormats[0]=DXGI_FORMAT_B8G8R8A8_UNORM;if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoCacheCapture)),"Create cache-capture PSO"))return false;
    p.PS={captureLuma->GetBufferPointer(),captureLuma->GetBufferSize()};
    p.RTVFormats[0]=DXGI_FORMAT_R8_UNORM;if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoCaptureLuma)),"Create NV12 luma PSO"))return false;
    p.PS={captureChroma->GetBufferPointer(),captureChroma->GetBufferSize()};
    p.RTVFormats[0]=DXGI_FORMAT_R8G8_UNORM;if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoCaptureChroma)),"Create NV12 chroma PSO"))return false;
    if(sourceNv12){
        p.PS={sourceNv12->GetBufferPointer(),sourceNv12->GetBufferSize()};
        p.RTVFormats[0]=DXGI_FORMAT_B8G8R8A8_UNORM;if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoSourceNv12)),"Create NV12 source PSO"))return false;
    }
    // The debug views draw into the backbuffer, so they take its format. They used to
    // inherit the cache target's B8G8R8A8 from the PSO created just above, which does
    // not match the R8G8B8A8 swapchain the present pass actually binds them to.
    p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
    p.PS={motion->GetBufferPointer(),motion->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoMotionDebug)),"Create MV debug PSO"))return false;
    p.PS={depth->GetBufferPointer(),depth->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoDepthDebug)),"Create depth debug PSO"))return false;
    p.PS={expand->GetBufferPointer(),expand->GetBufferSize()};p.NumRenderTargets=1;p.RTVFormats[0]=DXGI_FORMAT_R16G16_FLOAT;
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoExpandGuides)),"Create GPU guide expansion PSO"))return false;
    p.PS={depthWrite->GetBufferPointer(),depthWrite->GetBufferSize()};
    p.NumRenderTargets=0;p.RTVFormats[0]=DXGI_FORMAT_UNKNOWN;p.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    p.DepthStencilState.DepthEnable=TRUE;p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;p.DepthStencilState.StencilEnable=FALSE;
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoDepthWrite)),"Create real depth-buffer PSO"))return false;

    // The resolve pass's text lives in NvofResolveShader.h. It is compiled on its own
    // because it reads integer and unsigned textures where the shared source above
    // declares float ones at the same registers, and keeping the string in a header lets
    // a test compile it on a machine with no flow engine to run it on. Same root
    // signature, so it binds exactly like every other pass.
    ComPtr<ID3DBlob> nvofVs,nvofPs,nvofErr;
    auto CN=[&](const char*entry,const char*target,ComPtr<ID3DBlob>&out)->bool{
        nvofErr.Reset();
        const HRESULT hr=D3DCompile(kNvofResolveHlsl,sizeof(kNvofResolveHlsl)-1,"nvof",nullptr,nullptr,entry,target,flags,0,&out,&nvofErr);
        if(FAILED(hr)){if(nvofErr)LOG((char*)nvofErr->GetBufferPointer());return false;}
        return true;
    };
    if(!CN("VS","vs_5_1",nvofVs)||!CN("PSNvofMotion","ps_5_1",nvofPs))return false;
    p.VS={nvofVs->GetBufferPointer(),nvofVs->GetBufferSize()};
    p.PS={nvofPs->GetBufferPointer(),nvofPs->GetBufferSize()};
    p.NumRenderTargets=1;p.RTVFormats[0]=DXGI_FORMAT_R16G16_FLOAT;p.DSVFormat=DXGI_FORMAT_UNKNOWN;
    p.DepthStencilState.DepthEnable=FALSE;p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoNvofMotion)),"Create NVOFA motion resolve PSO"))return false;
    return true;
}

bool D3D12Renderer::InitializeDLSS(bool& gpuSynchronized){
    auto* cmd=m_cmds[0].Get();
    m_allocators[0]->Reset();cmd->Reset(m_allocators[0].Get(),nullptr);bool ok=m_dlss.Initialize(m_device.Get(),cmd,m_sourceW,m_sourceH,m_outputW,m_outputH,m_quality,m_preserveSource);
    if(ok){
        m_renderW=m_dlss.RenderWidth();m_renderH=m_dlss.RenderHeight();
        // The backend may have had to settle for a smaller output than the one
        // requested, when this source could not reach it. Video resources are
        // created after this point, so adopting it here keeps every consumer of
        // OutputW/OutputH on the size DLSS actually writes.
        m_outputW=m_dlss.OutputWidth();m_outputH=m_dlss.OutputHeight();
    }
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
    // m_sourceLayout was resolved in Initialize, before the pipelines that depend
    // on it.
    const bool nv12Source=m_sourceLayout==PixelLayout::Nv12;
    // The decoded texture is a render target only when the NV12 conversion draws into it.
    auto src=Tex2D(DXGI_FORMAT_B8G8R8A8_UNORM,m_sourceW,m_sourceH,nv12Source?D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET:D3D12_RESOURCE_FLAG_NONE);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&src,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_decodedTexture)),"Create decoded texture"))return false;
    m_decodedTexture->SetName(L"Video_Decoded_BGRA_sRGB");
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
    srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;m_device->CreateShaderResourceView(m_decodedTexture.Get(),&srv,SRVCPU(0));
    // The BGRA footprint is computed even for an NV12 source: the comparison reference
    // upload is BGRA and shares it.
    {uint32_t rows=0;uint64_t rowBytes=0,total=0;m_device->GetCopyableFootprints(&src,0,1,0,&m_uploadFootprint,&rows,&rowBytes,&total);m_numRows=rows;m_rowSize=rowBytes;m_uploadBytes=total;}
    if(nv12Source){
        m_device->CreateRenderTargetView(m_decodedTexture.Get(),nullptr,RTV(DecodedRTV));
        auto luma=Tex2D(DXGI_FORMAT_R8_UNORM,m_sourceW,m_sourceH,D3D12_RESOURCE_FLAG_NONE);
        if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&luma,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_sourceLuma)),"Create NV12 source luma"))return false;
        m_sourceLuma->SetName(L"Video_Source_Luma_R8");
        auto chroma=Tex2D(DXGI_FORMAT_R8G8_UNORM,m_sourceW/2u,m_sourceH/2u,D3D12_RESOURCE_FLAG_NONE);
        if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&chroma,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_sourceChroma)),"Create NV12 source chroma"))return false;
        m_sourceChroma->SetName(L"Video_Source_Chroma_R8G8");
        srv.Format=DXGI_FORMAT_R8_UNORM;m_device->CreateShaderResourceView(m_sourceLuma.Get(),&srv,SRVCPU(SourceLumaSRV));
        srv.Format=DXGI_FORMAT_R8G8_UNORM;m_device->CreateShaderResourceView(m_sourceChroma.Get(),&srv,SRVCPU(SourceChromaSRV));
        // Both planes of a frame share one upload buffer, mirroring the NV12 capture
        // readback: the chroma footprint sits at the placement alignment past the luma.
        uint32_t lumaRows=0,chromaRows=0;uint64_t lumaRowSize=0,chromaRowSize=0,lumaTotal=0,chromaTotal=0;
        m_device->GetCopyableFootprints(&luma,0,1,0,&m_sourceLumaFootprint,&lumaRows,&lumaRowSize,&lumaTotal);
        const uint64_t alignment=D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        const uint64_t chromaOffset=(lumaTotal+alignment-1u)&~(alignment-1u);
        m_device->GetCopyableFootprints(&chroma,0,1,chromaOffset,&m_sourceChromaFootprint,&chromaRows,&chromaRowSize,&chromaTotal);
        if(lumaRows!=m_sourceH||chromaRows!=m_sourceH/2u||lumaRowSize!=uint64_t{m_sourceW}||chromaRowSize!=uint64_t{m_sourceW})return false;
        const uint64_t total=chromaOffset+uint64_t{m_sourceChromaFootprint.Footprint.RowPitch}*chromaRows;
        D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=total;b.Height=1;b.DepthOrArraySize=1;b.MipLevels=1;b.SampleDesc={1,0};b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto up=HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        for(uint32_t i=0;i<FrameCount;++i){
            if(!HR(m_device->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&m_upload[i])),"Create NV12 video upload"))return false;
            D3D12_RANGE r{0,0};if(!HR(m_upload[i]->Map(0,&r,reinterpret_cast<void**>(&m_uploadMapped[i])),"Map NV12 video upload"))return false;
        }
    }else{
        // The reference table spans the slot after the one it is bound to, and the
        // present pass binds it one before the luma plane, so these two get defined
        // descriptors even on a source that has no NV12 planes.
        srv.Format=DXGI_FORMAT_R8_UNORM;m_device->CreateShaderResourceView(nullptr,&srv,SRVCPU(SourceLumaSRV));
        srv.Format=DXGI_FORMAT_R8G8_UNORM;m_device->CreateShaderResourceView(nullptr,&srv,SRVCPU(SourceChromaSRV));
        for(uint32_t i=0;i<FrameCount;++i){
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; uint32_t rows=0; uint64_t rowBytes=0,total=0;
            if(!CreateUploadForTexture(src,m_upload[i],m_uploadMapped[i],fp,rows,rowBytes,total,"Create video upload"))return false;
        }
    }

    // No optimized clear value: nothing clears this target. The conversion pass below
    // draws a full-screen triangle over the whole render-size viewport with blending
    // off and the full write mask, so every texel is written every frame and a clear
    // before it was a second full-target write of the same memory.
    auto col=Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&col,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_dlssColor)),"Create DLSS color"))return false;m_dlssColor->SetName(L"DLSS_Color_Input_Linear_FP16");m_device->CreateRenderTargetView(m_dlssColor.Get(),nullptr,RTV(FrameCount));
    srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;m_device->CreateShaderResourceView(m_dlssColor.Get(),&srv,SRVCPU(4));

    auto mot=Tex2D(DXGI_FORMAT_R16G16_FLOAT,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&mot,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_motion)),"Create motion guide"))return false;
    m_motion->SetName(L"DLSS_MotionVectors_CurrentToPrevious_RG16F");srv.Format=DXGI_FORMAT_R16G16_FLOAT;m_device->CreateShaderResourceView(m_motion.Get(),&srv,SRVCPU(2));m_device->CreateRenderTargetView(m_motion.Get(),nullptr,RTV(FrameCount+1));

    // Hardware optical flow replaces the CPU block matcher as the motion source when the
    // engine is present. It is asked for the decoded frame's own size, which is the
    // texture Capture() copies: on a neural-size path that is also the DLSS input size,
    // and on a runtime Super Resolution session it is not, so the resolve pass scales
    // the vectors into DLSS input pixels. See PlanHardwareFlow for why that conversion
    // is exact and why nothing else is refused here.
    m_nvofActive=false;
    // The three flow descriptors are written whether or not the engine comes up. A pass
    // that binds the reference table at the cost slot now spans the slot after it too,
    // and a heap slot nobody ever wrote is not a descriptor.
    D3D12_SHADER_RESOURCE_VIEW_DESC fsrv{};fsrv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    fsrv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;fsrv.Texture2D.MipLevels=1;
    fsrv.Format=DXGI_FORMAT_R16G16_SINT;
    m_device->CreateShaderResourceView(nullptr,&fsrv,SRVCPU(NvofFlowSRV));
    m_device->CreateShaderResourceView(nullptr,&fsrv,SRVCPU(NvofBackFlowSRV));
    fsrv.Format=DXGI_FORMAT_R8_UINT;m_device->CreateShaderResourceView(nullptr,&fsrv,SRVCPU(NvofCostSRV));
    const HardwareFlowPlan flowPlan=PlanHardwareFlow(m_sourceW,m_sourceH,m_renderW,m_renderH);
    m_nvofMotionScaleX=flowPlan.motionScaleX;m_nvofMotionScaleY=flowPlan.motionScaleY;
    if(flowPlan.attempt&&m_nvof.Initialize(m_device.Get(),flowPlan.width,flowPlan.height)){
        fsrv.Format=DXGI_FORMAT_R16G16_SINT;m_device->CreateShaderResourceView(m_nvof.Flow(),&fsrv,SRVCPU(NvofFlowSRV));
        // A device that offered no cost surface, or only the forward direction, keeps the
        // null descriptor written above: the resolve pass binds t1 and t2 either way, and
        // the gate that reads each is switched off by its own constant.
        if(m_nvof.Cost()){
            fsrv.Format=m_nvof.Cost()->GetDesc().Format;
            m_device->CreateShaderResourceView(m_nvof.Cost(),&fsrv,SRVCPU(NvofCostSRV));
        }
        if(m_nvof.BackwardFlow()){
            fsrv.Format=DXGI_FORMAT_R16G16_SINT;
            m_device->CreateShaderResourceView(m_nvof.BackwardFlow(),&fsrv,SRVCPU(NvofBackFlowSRV));
        }
        m_nvofActive=true;
    }
    // One line per session, because a log that does not name the estimator cannot tell
    // a session that ran on 1/32-pixel hardware vectors from one that ran on a 24x24
    // CPU block match, and those are different pictures.
    if(m_nvofActive)
        LOG("Motion guide backend: NVOFA hardware flow on the decoded "<<m_sourceW<<"x"<<m_sourceH
            <<" frame, vectors scaled by "<<m_nvofMotionScaleX<<","<<m_nvofMotionScaleY
            <<" into the "<<m_renderW<<"x"<<m_renderH<<" DLSS input.");
    else
        LOG("Motion guide backend: CPU block matcher, because "
            <<(flowPlan.attempt?"the flow engine did not come up for the decoded frame (see the NVOFA line above)":flowPlan.refusal)
            <<"; decoded "<<m_sourceW<<"x"<<m_sourceH<<", DLSS input "<<m_renderW<<"x"<<m_renderH<<".");

    // One depth resource, two views: D32_FLOAT DSV for real depth writes / ReShade
    // discovery and R32_FLOAT SRV for debug/NGX sampling. Passing this exact resource
    // to NGX avoids the old "R32 proxy + unrelated mirrored D32" ambiguity.
    D3D12_CLEAR_VALUE dcv{};dcv.Format=DXGI_FORMAT_D32_FLOAT;dcv.DepthStencil.Depth=1.0f;dcv.DepthStencil.Stencil=0;
    auto dep=Tex2D(DXGI_FORMAT_R32_TYPELESS,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&dep,D3D12_RESOURCE_STATE_DEPTH_WRITE,&dcv,IID_PPV_ARGS(&m_depth)),"Create unified DLSS depth"))return false;
    m_depth->SetName(L"DLSS_Depth_R32_TYPELESS_D32_DSV_R32_SRV");
    srv.Format=DXGI_FORMAT_R32_FLOAT;m_device->CreateShaderResourceView(m_depth.Get(),&srv,SRVCPU(3));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=DXGI_FORMAT_D32_FLOAT;dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;m_device->CreateDepthStencilView(m_depth.Get(),&dsv,DSV());

    auto grid=Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT,m_gridW,m_gridH,D3D12_RESOURCE_FLAG_NONE);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&grid,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_guideGrid)),"Create compact temporal guide grid"))return false;
    m_guideGrid->SetName(L"DLSS_CompactGuideGrid_RGBA32F");
    for(uint32_t i=0;i<FrameCount;++i) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; uint32_t rows=0; uint64_t rowBytes=0,total=0;
        if(!CreateUploadForTexture(grid,m_guideUpload[i],m_guideMapped[i],fp,rows,rowBytes,total,"Create compact guide upload"))return false;
        if(i==0){m_guideFootprint=fp;m_guideRows=rows;m_guideRowSize=rowBytes;m_guideUploadBytes=total;}
    }
    srv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;m_device->CreateShaderResourceView(m_guideGrid.Get(),&srv,SRVCPU(5));
    auto out=Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,m_outputW,m_outputH,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&m_dlssOutput)),"Create DLSS output"))return false;
    m_dlssOutput->SetName(L"DLSS_Output_Linear_FP16_UAV");
    srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;m_device->CreateShaderResourceView(m_dlssOutput.Get(),&srv,SRVCPU(1));

    auto cache=Tex2D(DXGI_FORMAT_B8G8R8A8_UNORM,m_outputW,m_outputH,
                     D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&cache,
        D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_cacheOutput)),
        "Create cache output"))return false;
    m_cacheOutput->SetName(L"Neural_Cache_Output_BGRA8_sRGB");
    m_device->CreateRenderTargetView(m_cacheOutput.Get(),nullptr,RTV(FrameCount+2));
    m_device->GetCopyableFootprints(&cache,0,1,0,&m_cacheFootprint,&m_cacheRows,
                                    &m_cacheRowSize,&m_cacheReadbackBytes);
    if(!m_cacheReadbackBytes||m_cacheRows!=m_outputH||m_cacheRowSize!=uint64_t{m_outputW}*4u)
        return false;
    // GPU colour conversion writes two planes instead of one BGRA target. NV12 is 4:2:0,
    // so an odd output keeps the BGRA capture rather than losing a row or a column.
    m_captureFormat=(m_requestedCaptureFormat==CaptureFormat::Nv12&&!(m_outputW%2)&&!(m_outputH%2))
        ?CaptureFormat::Nv12:CaptureFormat::Bgra;
    if(m_requestedCaptureFormat==CaptureFormat::Nv12&&m_captureFormat==CaptureFormat::Bgra)
        LOG("GPU colour conversion needs even output dimensions; capturing BGRA at "
            <<m_outputW<<"x"<<m_outputH<<".");
    m_captureLuma.Reset();m_captureChroma.Reset();
    m_lumaFootprint={};m_chromaFootprint={};
    if(m_captureFormat==CaptureFormat::Nv12){
        auto luma=Tex2D(DXGI_FORMAT_R8_UNORM,m_outputW,m_outputH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&luma,
            D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_captureLuma)),
            "Create NV12 luma plane"))return false;
        m_captureLuma->SetName(L"Neural_Capture_Luma_R8");
        m_device->CreateRenderTargetView(m_captureLuma.Get(),nullptr,RTV(FrameCount+3));
        auto chroma=Tex2D(DXGI_FORMAT_R8G8_UNORM,m_outputW/2u,m_outputH/2u,
                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&chroma,
            D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_captureChroma)),
            "Create NV12 chroma plane"))return false;
        m_captureChroma->SetName(L"Neural_Capture_Chroma_R8G8");
        m_device->CreateRenderTargetView(m_captureChroma.Get(),nullptr,RTV(FrameCount+4));
        // Both planes land in one readback buffer, the second at the offset D3D12
        // requires for a placed copy, so a capture is still a single fence and a single
        // mapped range.
        uint32_t lumaRows=0,chromaRows=0;uint64_t lumaRowSize=0,chromaRowSize=0,lumaTotal=0,chromaTotal=0;
        m_device->GetCopyableFootprints(&luma,0,1,0,&m_lumaFootprint,&lumaRows,&lumaRowSize,&lumaTotal);
        const uint64_t alignment=D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        const uint64_t chromaOffset=(lumaTotal+alignment-1u)&~(alignment-1u);
        m_device->GetCopyableFootprints(&chroma,0,1,chromaOffset,&m_chromaFootprint,&chromaRows,
                                        &chromaRowSize,&chromaTotal);
        if(lumaRows!=m_outputH||chromaRows!=m_outputH/2u||lumaRowSize!=uint64_t{m_outputW}||
           chromaRowSize!=uint64_t{m_outputW})return false;
        m_cacheReadbackBytes=chromaOffset+
            uint64_t{m_chromaFootprint.Footprint.RowPitch}*chromaRows;
    }
    auto readbackHeap=HeapProps(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC readback{};readback.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    readback.Width=m_cacheReadbackBytes;readback.Height=1;readback.DepthOrArraySize=1;
    readback.MipLevels=1;readback.SampleDesc={1,0};readback.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // One readback buffer per capture slot so frame N's copy target is not the buffer the
    // CPU is still reading for frame N-1. That is what lets the capture fence be waited on
    // per slot instead of draining the whole queue after every frame.
    //
    // The buffers stay mapped for the renderer's lifetime, which declares the CPU read
    // range exactly once. READBACK heap memory on a discrete PCIe adapter is write-back
    // cached and coherent, so that is safe here and this player already requires an RTX
    // GPU. A UMA or WARP adapter would need the range re-declared per read.
    for(uint32_t i=0;i<CaptureSlots;++i){
        if(!HR(m_device->CreateCommittedResource(&readbackHeap,D3D12_HEAP_FLAG_NONE,&readback,
            D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_cacheReadback[i])),
            "Create cache readback"))return false;
        m_cacheReadback[i]->SetName(L"Neural_Cache_Readback_RGBA8");
        const D3D12_RANGE readRange{0, static_cast<SIZE_T>(m_cacheReadbackBytes)};
        if(!HR(m_cacheReadback[i]->Map(0,&readRange,
            reinterpret_cast<void**>(&m_cacheReadbackMapped[i])),
            "Map persistent cache readback buffer"))return false;
    }

    // Comparison reference: the original member of the current pair at source size.
    // Same layout as the decoded texture, so the decoded upload footprint applies.
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&src,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_reference)),"Create comparison reference"))return false;
    m_reference->SetName(L"Comparison_Reference_BGRA_sRGB");
    for(uint32_t i=0;i<FrameCount;++i) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; uint32_t rows=0; uint64_t rowBytes=0,total=0;
        if(!CreateUploadForTexture(src,m_referenceUpload[i],m_referenceMapped[i],fp,rows,rowBytes,total,"Create comparison reference upload"))return false;
    }
    srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;m_device->CreateShaderResourceView(m_reference.Get(),&srv,SRVCPU(ReferenceSRV));

    // Two timestamps per frame slot bracket DLSS Evaluate; resolved into a readback
    // buffer and harvested once that slot's fence is known complete.
    D3D12_QUERY_HEAP_DESC qh{};qh.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qh.Count=FrameCount*2;
    if(!HR(m_device->CreateQueryHeap(&qh,IID_PPV_ARGS(&m_timestampHeap)),"Create timestamp query heap"))return false;
    readback.Width=uint64_t{FrameCount}*2u*sizeof(uint64_t);
    if(!HR(m_device->CreateCommittedResource(&readbackHeap,D3D12_HEAP_FLAG_NONE,&readback,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_timestampReadback)),
        "Create timestamp readback"))return false;
    m_timestampReadback->SetName(L"Neural_Timestamp_Readback");
    {
        // Mapped once here rather than around every harvest, which is the same trade the
        // capture readbacks above make: the buffer is written by ResolveQueryData and
        // read only after the fence that published it, so the mapping outliving the read
        // costs nothing and a Map/Unmap pair per frame is gone.
        void*stamps=nullptr;
        const D3D12_RANGE stampRange{0,static_cast<SIZE_T>(readback.Width)};
        if(!HR(m_timestampReadback->Map(0,&stampRange,&stamps),
            "Map persistent timestamp readback buffer"))return false;
        m_timestampMapped=static_cast<const uint64_t*>(stamps);
    }

    // Clear the reference to black before anything can sample it.
    memset(m_referenceMapped[0],0,size_t(m_uploadBytes));
    if(!HR(m_allocators[0]->Reset(),"Reset allocator for reference clear"))return false;
    auto*cmd=m_cmds[0].Get();
    if(!HR(cmd->Reset(m_allocators[0].Get(),nullptr),"Reset command list for reference clear"))return false;
    D3D12_TEXTURE_COPY_LOCATION rd{};rd.pResource=m_reference.Get();rd.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION rs{};rs.pResource=m_referenceUpload[0].Get();rs.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;rs.PlacedFootprint=m_uploadFootprint;
    cmd->CopyTextureRegion(&rd,0,0,0,&rs,nullptr);
    Barrier(cmd,m_reference.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_referenceInCopyDest=false;
    if(!HR(cmd->Close(),"Close reference clear command list"))return false;
    ID3D12CommandList*clearLists[]={cmd};m_queue->ExecuteCommandLists(1,clearLists);
    if(!WaitGPUForContinuedUse())return false;

    LOG("DLSS resource contract ready: Color=R16G16B16A16_FLOAT " << m_renderW << "x" << m_renderH
        << ", MV=R16G16_FLOAT " << m_renderW << "x" << m_renderH
        << ", Depth=R32_TYPELESS resource / D32_FLOAT DSV / R32_FLOAT SRV " << m_renderW << "x" << m_renderH
        << ", Output=R16G16B16A16_FLOAT UAV " << m_outputW << "x" << m_outputH
        << ", CompactGrid=R32G32B32A32_FLOAT " << m_gridW << "x" << m_gridH << " -> GPU MV expansion + direct SV_Depth write"
        << ", Reference=B8G8R8A8_UNORM " << m_sourceW << "x" << m_sourceH
        << ", Source=" << (nv12Source ? "NV12 -> GPU BT.709 conversion" : "BGRA upload")
        << ", NeuralTimestamps=" << FrameCount*2 << " @" << m_timestampFrequency << "Hz");
    return true;
}

void D3D12Renderer::CopyMappedRows(uint8_t*mapped,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT&fp,const void*src,size_t tight,uint32_t rows){
    const uint8_t*s=static_cast<const uint8_t*>(src);
    uint8_t*destination=mapped+fp.Offset;
    const size_t pitch=size_t(fp.Footprint.RowPitch);
    // A full 4K BGRA frame is over 30 MB per call and used to be copied one row at a
    // time on the calling thread. Row pitch is 256-aligned, which for the common widths
    // (1920 and 3840 give 7680 and 15360) already equals the tight row, so the whole
    // upload collapses to a single contiguous copy.
    if(pitch==tight){
        const size_t total=tight*size_t(rows);
        ParallelForRanges(total,kParallelCopyGrain,[&](size_t begin,size_t end){
            memcpy(destination+begin,s+begin,end-begin);
        });
        return;
    }
    ParallelForRanges(size_t(rows),kParallelRowGrain,[&](size_t begin,size_t end){
        for(size_t y=begin;y<end;++y)memcpy(destination+pitch*y,s+tight*y,tight);
    });
}

bool D3D12Renderer::RenderFrame(const uint8_t*bgra,size_t bytes,const float*guideGridRGBA32F,size_t guideBytes,uint32_t gridW,uint32_t gridH,bool temporalReset,bool motionGuides,float frameTimeMs){
    return RenderFrameInternal(bgra,bytes,guideGridRGBA32F,guideBytes,gridW,gridH,temporalReset,motionGuides,frameTimeMs,nullptr);
}

bool D3D12Renderer::RenderFrame(const uint8_t*bgra,size_t bytes,const FrameIdentity&frame,const GuideFrame&guide,float frameTimeMs){
    if(!guide.id.SameSource(frame)){
        LOG("Rejected frame/guide identity mismatch: frame#"<<frame.frameNumber<<" pts="<<frame.pts100ns
            <<" src="<<frame.sourceGeneration<<" job="<<frame.jobId
            <<" vs guide#"<<guide.id.frameNumber<<" pts="<<guide.id.pts100ns
            <<" src="<<guide.id.sourceGeneration<<" job="<<guide.id.jobId);
        return false;
    }
    return RenderFrameInternal(bgra,bytes,guide.guideGridRGBA32F.data(),guide.guideGridRGBA32F.size()*sizeof(float),
        guide.gridW,guide.gridH,guide.id.reset!=HistoryReset::None,guide.motionVectors,frameTimeMs,&guide.id);
}

bool D3D12Renderer::RenderFrameInternal(const uint8_t*bgra,size_t bytes,const float*guideGridRGBA32F,size_t guideBytes,uint32_t gridW,uint32_t gridH,bool temporalReset,bool motionGuides,float frameTimeMs,const FrameIdentity*identity){
    if(m_gpuUnusable)return false;
    const bool nv12Source=m_sourceLayout==PixelLayout::Nv12;
    const size_t videoRow=size_t(m_sourceW)*4u,guideRow=size_t(m_gridW)*sizeof(float)*4u;
    if(!bgra||bytes<SourceFrameBytes()||!guideGridRGBA32F||gridW!=m_gridW||gridH!=m_gridH||guideBytes<guideRow*m_gridH)return false;
    const uint32_t slot=m_frameSlot%FrameCount;
    if(!WaitForFrameSlot(slot, &m_renderSlotWaitNanos)) return false;
    HarvestNeuralTimings();
    SampleLocalVideoMemory();
    if(nv12Source){
        // Y plane, then the interleaved UV plane straight behind it; both rows are
        // m_sourceW bytes wide, the chroma plane has half the rows.
        CopyMappedRows(m_uploadMapped[slot],m_sourceLumaFootprint,bgra,size_t(m_sourceW),m_sourceH);
        CopyMappedRows(m_uploadMapped[slot],m_sourceChromaFootprint,bgra+size_t(m_sourceW)*m_sourceH,size_t(m_sourceW),m_sourceH/2u);
    }else CopyMappedRows(m_uploadMapped[slot],m_uploadFootprint,bgra,videoRow,m_sourceH);
    CopyMappedRows(m_guideMapped[slot],m_guideFootprint,guideGridRGBA32F,guideRow,m_gridH);
    if(!HR(m_allocators[slot]->Reset(),"Reset frame allocator")) return false;
    if(!HR(m_uploadAllocators[slot]->Reset(),"Reset frame upload allocator")) return false;
    auto* cmd=m_cmds[slot].Get();
    auto* pre=m_uploadCmds[slot].Get();
    if(!HR(cmd->Reset(m_allocators[slot].Get(),nullptr),"Reset frame command list")) return false;
    if(!HR(pre->Reset(m_uploadAllocators[slot].Get(),nullptr),"Reset frame upload command list")) return false;
    ID3D12DescriptorHeap*heaps[]={m_srvHeap.Get()};cmd->SetDescriptorHeaps(1,heaps);pre->SetDescriptorHeaps(1,heaps);
    RecordReferenceUpload(pre,slot);

    D3D12_TEXTURE_COPY_LOCATION d{};d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION s{};s.pResource=m_upload[slot].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    if(nv12Source){
        // Copy both planes, then one full-resolution draw converts them into the decoded
        // texture. The draw replaces the BGRA copy below; everything after it is shared.
        if(!m_sourcePlanesInCopyDest){Barrier(pre,m_sourceLuma.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);Barrier(pre,m_sourceChroma.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);}
        d.pResource=m_sourceLuma.Get();s.PlacedFootprint=m_sourceLumaFootprint;pre->CopyTextureRegion(&d,0,0,0,&s,nullptr);
        d.pResource=m_sourceChroma.Get();s.PlacedFootprint=m_sourceChromaFootprint;pre->CopyTextureRegion(&d,0,0,0,&s,nullptr);
        Barrier(pre,m_sourceLuma.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);Barrier(pre,m_sourceChroma.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_sourcePlanesInCopyDest=false;
        Barrier(pre,m_decodedTexture.Get(),m_sourceInCopyDest?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_VIEWPORT svp{0,0,float(m_sourceW),float(m_sourceH),0,1};D3D12_RECT ssc{0,0,LONG(m_sourceW),LONG(m_sourceH)};pre->RSSetViewports(1,&svp);pre->RSSetScissorRects(1,&ssc);
        auto srt=RTV(DecodedRTV);pre->OMSetRenderTargets(1,&srt,FALSE,nullptr);
        pre->SetGraphicsRootSignature(m_rootSig.Get());pre->SetPipelineState(m_psoSourceNv12.Get());pre->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        pre->SetGraphicsRootDescriptorTable(RootView,SRVGPU(SourceLumaSRV));pre->SetGraphicsRootDescriptorTable(RootReference,SRVGPU(SourceChromaSRV));
        const float none[4]={0,0,0,0};pre->SetGraphicsRoot32BitConstants(RootConstants,4,none,0);pre->DrawInstanced(3,1,0,0);
        Barrier(pre,m_decodedTexture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_sourceInCopyDest=false;
    }else{
        if(!m_sourceInCopyDest)Barrier(pre,m_decodedTexture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        d.pResource=m_decodedTexture.Get();s.PlacedFootprint=m_uploadFootprint;pre->CopyTextureRegion(&d,0,0,0,&s,nullptr);Barrier(pre,m_decodedTexture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_sourceInCopyDest=false;
    }

    if(!m_gridInCopyDest)Barrier(pre,m_guideGrid.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    d.pResource=m_guideGrid.Get();s.pResource=m_guideUpload[slot].Get();s.PlacedFootprint=m_guideFootprint;pre->CopyTextureRegion(&d,0,0,0,&s,nullptr);
    Barrier(pre,m_guideGrid.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_gridInCopyDest=false;

    // Hardware optical flow. The engine reads the decoded frame the list above just
    // produced, so the queue is signalled past that list and the engine waits on the
    // signal; the frame's own list then waits on the engine's fence before reading the
    // field. Both waits are on the GPU. Recording the uploads into their own list is
    // what makes that possible without resetting an allocator the rest of the frame is
    // still recording into, which is the only reason the split exists.
    // Motion guides off means no motion anywhere. The CPU grid already emits zero in
    // R and G, and skipping the engine is what keeps that true: the resolve pass below
    // does not read the guide controls, so leaving it on made the switch move the cache
    // key and change no pixel on any card where the engine comes up.
    const bool nvofFrame=motionGuides&&m_nvofActive&&DLSSEnabled();
    if(nvofFrame){
        if(temporalReset)m_nvof.Reset();
        m_nvof.Capture(pre,m_decodedTexture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if(!HR(pre->Close(),"Close frame upload command list")) return false;
    {ID3D12CommandList*uploadLists[]={pre};m_queue->ExecuteCommandLists(1,uploadLists);}
    // False on the first frame of a stream and on every cut: there is no previous frame
    // to compare against. The compact CPU grid is already all-zero on exactly those
    // frames, so falling back to it emits the zero motion the reset needs anyway.
    const bool nvofFlow=nvofFrame&&m_nvof.Submit(m_queue.Get());

    // Full-resolution motion for NGX: from the flow engine when it ran this frame,
    // otherwise by expanding the compact CPU analysis grid. Depth below always comes
    // from that grid - the engine estimates motion and nothing else.
    if(!m_guidesInRT)Barrier(cmd,m_motion.Get(),GuideReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);m_guidesInRT=true;
    D3D12_VIEWPORT gvp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT gsc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&gvp);cmd->RSSetScissorRects(1,&gsc);
    auto grt=RTV(FrameCount+1);cmd->OMSetRenderTargets(1,&grt,FALSE,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if(nvofFlow){
        m_nvof.BeginRead(cmd);
        cmd->SetPipelineState(m_psoNvofMotion.Get());
        cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(NvofFlowSRV));
        cmd->SetGraphicsRootDescriptorTable(RootReference,SRVGPU(NvofCostSRV));
        // S10.5: one stored unit is 1/32 of an engine-input pixel. The third and fourth
        // constants are the confidence gate, left equal so it stays off until its
        // thresholds are measured. The fifth and sixth carry the vector out of engine
        // pixels and into the DLSS input pixels the motion texture is in - 1,1 unless
        // this is a Super Resolution session. The seventh is the flow grid's cell pitch,
        // which the round-trip gate needs to find the cell a vector lands on; zero there
        // is what leaves that gate out of the pass entirely on a device that gave no
        // backward field.
        const float cells=m_nvof.BackwardFlow()?1.0f/float(m_nvof.Grid()):0.0f;
        const float resolve[7]={1.0f/32.0f,1.0f/32.0f,0.0f,0.0f,
                                m_nvofMotionScaleX,m_nvofMotionScaleY,cells};
        cmd->SetGraphicsRoot32BitConstants(RootConstants,7,resolve,0);
        cmd->DrawInstanced(3,1,0,0);
        m_nvof.EndRead(cmd);
    }else{
        cmd->SetPipelineState(m_psoExpandGuides.Get());
        cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(5));
        cmd->DrawInstanced(3,1,0,0);
    }
    Barrier(cmd,m_motion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);m_guidesInRT=false;

    // Populate the exact depth resource passed to NGX. The resource is R32_TYPELESS,
    // viewed as D32_FLOAT while writing and R32_FLOAT while sampling/debugging.
    if(!m_depthInWrite)Barrier(cmd,m_depth.Get(),DepthGuideReadState,D3D12_RESOURCE_STATE_DEPTH_WRITE);m_depthInWrite=true;
    D3D12_VIEWPORT dvp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT dsc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&dvp);cmd->RSSetScissorRects(1,&dsc);
    auto dsvh=DSV();cmd->OMSetRenderTargets(0,nullptr,FALSE,&dsvh);cmd->ClearDepthStencilView(dsvh,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoDepthWrite.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(5));cmd->DrawInstanced(3,1,0,0);
    Barrier(cmd,m_depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,DepthGuideReadState);m_depthInWrite=false;

    // No render target in this frame is cleared. Every pass here draws the same
    // full-screen triangle over a viewport the size of its whole target, with blending
    // off and the full write mask, so the draw writes each texel the clear would have -
    // and a clear in front of it is a second full-target write of the same memory,
    // which at 4K is tens of megabytes per pass per frame for no pixel that ends up
    // different. The depth pass above keeps its clear: it covers every texel too, but a
    // depth clear also resets the hierarchical-Z state a later reader may take, and NGX
    // is the reader of this one.
    if(!m_colorInRT)Barrier(cmd,m_dlssColor.Get(),GuideReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);m_colorInRT=true;
    D3D12_VIEWPORT vp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT sc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&vp);cmd->RSSetScissorRects(1,&sc);
    auto crt=RTV(FrameCount);cmd->OMSetRenderTargets(1,&crt,FALSE,nullptr);cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoConvert.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(0));
    cmd->DrawInstanced(3,1,0,0);Barrier(cmd,m_dlssColor.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);m_colorInRT=false;

    ++m_framesPresented;

    // Create the NGX feature on an open command list, submit that list, and only
    // then evaluate on a fresh list. This mirrors the robust game-style NGX
    // lifetime instead of relying on CreateFeature and EvaluateFeature being
    // accepted back-to-back before the creation commands have reached the GPU.
    // Intentionally allow one complete Present before the first NGX CreateFeature.
    // ReShade add-ons finish their swapchain/runtime initialization on that first frame;
    // creating on frame 2 makes the raw CreateFeature much harder for RenoDX to miss.
    // Nothing here releases a live feature on a frame count any more: the
    // add-on that hooks the release tears down its inline neural worksets on the
    // spot, and the offline job's receipt gate re-presents one source frame for
    // as long as it takes the add-on to publish a fresh evaluation, so a timed
    // release landed inside that gate and killed the pass it was waiting for.
    // A re-hook is now only ever requested explicitly, and the drain below still
    // honours the NGX rule that no command list referencing the feature may be
    // in flight when it is released.
    const auto featureSetup = ngx_session_detail::PrepareFeatureForFrame(
        DLSSEnabled(), m_dlss.FeatureCreated(), m_framesPresented,
        m_recreateRequested,
        [&] { return m_dlss.EnsureFeature(cmd); },
        [&] { return WaitGPUForContinuedUse() && m_dlss.RecreateFeature(cmd); },m_preserveSource);
    const bool needFeatureFlush = featureSetup.needsFlush;
    if (featureSetup.selected) temporalReset = true;
    if (temporalReset && identity) {
        const HistoryReset reason = identity->reset != HistoryReset::None ? identity->reset
            : featureSetup.selected ? HistoryReset::FeatureRecreate : HistoryReset::None;
        LOG("Temporal history reset: reason=" << HistoryResetName(reason) << " frame#" << identity->frameNumber
            << " pts=" << identity->pts100ns << " history=" << identity->historyGeneration << " job=" << identity->jobId);
    }
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
        const bool timed=m_timestampHeap&&m_timestampReadback&&m_timestampFrequency;
        if(timed)cmd->EndQuery(m_timestampHeap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*2u);
        used=m_dlss.Evaluate(cmd,m_dlssColor.Get(),m_dlssOutput.Get(),m_depth.Get(),m_motion.Get(),temporalReset,frameTimeMs);
        if(timed&&used){
            cmd->EndQuery(m_timestampHeap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*2u+1u);
            cmd->ResolveQueryData(m_timestampHeap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*2u,2,m_timestampReadback.Get(),uint64_t{slot}*2u*sizeof(uint64_t));
            m_neuralTimingPending[slot]=true;
        }
        if(used){Barrier(cmd,m_dlssOutput.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_outputInUAV=false;}
    }

    m_lastDLSSUsed=used;
    // The backbuffer pass and Present are not optional even for a window nobody sees:
    // the RenoDX add-on performs its feature-18 evaluation per present. An export that
    // skipped presents past the feature recreate rendered 900/900 "verified" frames with
    // DLAA only (0.46 ms neural GPU time against 5.7 ms), bit-for-bit non-neural.
    {
        uint32_t bi=m_swapchain->GetCurrentBackBufferIndex();Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);D3D12_VIEWPORT ovp{0,0,float(m_outputW),float(m_outputH),0,1};D3D12_RECT osc{0,0,LONG(m_outputW),LONG(m_outputH)};cmd->RSSetViewports(1,&ovp);cmd->RSSetScissorRects(1,&osc);auto brt=RTV(bi);cmd->OMSetRenderTargets(1,&brt,FALSE,nullptr);cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const bool finalView=(m_debugView==DebugView::Final);
        SetPresentConstants(cmd,finalView?m_colorSettings:ColorSettings{},finalView?m_comparison:ComparisonSettings{},finalView&&m_hasReference);
        // DLSS inputs stay shader-readable for NGX. Only the texture selected for the
        // debug/fallback presentation pass is temporarily made pixel-shader readable.
        ID3D12Resource* debugPixelResource=nullptr;
        D3D12_RESOURCE_STATES debugBefore=GuideReadState;
        switch(m_debugView){
            case DebugView::MotionVectors:debugPixelResource=m_motion.Get();cmd->SetPipelineState(m_psoMotionDebug.Get());cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(2));break;
            case DebugView::Depth:debugPixelResource=m_depth.Get();debugBefore=DepthGuideReadState;cmd->SetPipelineState(m_psoDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(3));break;
            case DebugView::Input:debugPixelResource=m_dlssColor.Get();cmd->SetPipelineState(m_psoPresent.Get());cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(4));break;
            default:cmd->SetPipelineState(m_psoPresent.Get());if(used)cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(1));else{debugPixelResource=m_dlssColor.Get();cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(4));}break;
        }
        if(debugPixelResource)Barrier(cmd,debugPixelResource,debugBefore,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->DrawInstanced(3,1,0,0);
        if(debugPixelResource)Barrier(cmd,debugPixelResource,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,debugBefore);
        Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    }
    if(!HR(cmd->Close(),"Close frame command list")) return false;
    ID3D12CommandList*ls[]={cmd};m_queue->ExecuteCommandLists(1,ls);
    {
        const auto presented=std::chrono::steady_clock::now();
        HRESULT phr=m_swapchain->Present(0,m_allowTearing?DXGI_PRESENT_ALLOW_TEARING:0);
        m_presentNanos+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-presented).count());
        if(FAILED(phr)){LOG("Present failed hr=0x"<<std::hex<<phr);return false;}
    }
    return SignalFrameSlot(slot);
}

bool D3D12Renderer::RenderFrameForCache(const uint8_t*bgra,size_t bytes,const FrameIdentity&frame,
                                        const GuideFrame&guide,float frameTimeMs,
                                        CapturedVideoFrame&capture){
    capture.pixels.clear();capture.width=0;capture.height=0;capture.id=guide.id;
    if(!RenderFrame(bgra,bytes,frame,guide,frameTimeMs))return false;
    return CaptureEvaluatedFrame(capture);
}

bool D3D12Renderer::UploadReferenceFrame(const uint8_t*bgra,size_t bytes){
    if(m_gpuUnusable||!m_reference)return false;
    const size_t row=size_t(m_sourceW)*4u;
    if(!bgra||bytes<row*m_sourceH)return false;
    // The next submission (RenderFrame/PresentCurrent/capture) reuses this same slot
    // and records the texture copy, so its fence also guards this upload buffer.
    const uint32_t slot=m_frameSlot%FrameCount;
    if(!WaitForFrameSlot(slot))return false;
    CopyMappedRows(m_referenceMapped[slot],m_uploadFootprint,bgra,row,m_sourceH);
    m_referenceUploadSlot=slot;m_referencePending=true;
    return true;
}

void D3D12Renderer::RecordReferenceUpload(ID3D12GraphicsCommandList*cmd,uint32_t slot){
    if(!m_referencePending||m_referenceUploadSlot!=slot)return;
    if(!m_referenceInCopyDest)Barrier(cmd,m_reference.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION d{};d.pResource=m_reference.Get();d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION s{};s.pResource=m_referenceUpload[slot].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;s.PlacedFootprint=m_uploadFootprint;
    cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);
    Barrier(cmd,m_reference.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_referenceInCopyDest=false;m_referencePending=false;m_hasReference=true;
}

// Root constants (PresentConstantCount floats): [0..1] JitterUV, [2] Misc.x = 1/outputW,
// [3] Misc.y = zoom, [4..7] ColorA, [8..9] ColorB.xy = temperature/tint, [10] ColorB.z =
// neural strength, [11] ColorB.w = luminance-ratio guard, [12..15] Compare{mode,
// amount|splitX, zoomCenterX, zoomCenterY}, [16..17] Capture.xy = one chroma texel in UV.
// Also binds the comparison reference at t1. Without an uploaded reference every
// comparison mode degrades to Neural so the shader never selects the black texture.
void D3D12Renderer::SetPresentConstants(ID3D12GraphicsCommandList*cmd,const ColorSettings&cs,const ComparisonSettings&cmp,bool useReference){
    const ComparisonMode mode=useReference?cmp.mode:ComparisonMode::Neural;
    const float select=(mode==ComparisonMode::Blend)?cmp.amount:cmp.splitX;
    // The dial composites against the reference, so without one it falls back to exactly
    // 1: the capture pass and the offline renderer never upload a reference, which is
    // what keeps every cached frame, export and settings digest bit-identical.
    const float strength=useReference?std::clamp(cmp.strength,0.0f,2.0f):1.0f;
    const float ratioGuard=std::max(cmp.ratioGuard,1.0f);
    const float params[PresentConstantCount]={
        0,0,m_outputW?1.0f/float(m_outputW):0.0f,std::max(cmp.zoomScale,0.01f),
        cs.brightness,cs.contrast,cs.saturation,cs.gamma,
        cs.temperature,cs.tint,strength,ratioGuard,
        float(static_cast<int>(mode)),select,cmp.zoomCenterX,cmp.zoomCenterY,
        m_outputW?2.0f/float(m_outputW):0.0f,m_outputH?2.0f/float(m_outputH):0.0f,0,0};
    cmd->SetGraphicsRoot32BitConstants(RootConstants,PresentConstantCount,params,0);
    cmd->SetGraphicsRootDescriptorTable(RootReference,SRVGPU(ReferenceSRV));
}

bool D3D12Renderer::CaptureEvaluatedFrame(CapturedVideoFrame&capture){
    capture.pixels.clear();capture.width=0;capture.height=0;
    if(!m_lastDLSSUsed||!m_outputW||!m_outputH)return false;
    // Whatever the active capture format produces, not four bytes per pixel: an NV12
    // capture is 1.5, and the size is the contract the caller checks the frame against.
    const uint64_t pixels64=uint64_t{m_outputW}*m_outputH;
    const uint64_t tightBytes64=m_captureFormat==CaptureFormat::Nv12
        ?pixels64+pixels64/2u:pixels64*4u;
    if(tightBytes64>std::numeric_limits<size_t>::max())return false;
    if(m_testHooks&&m_testHooks->cacheCapture){
        const size_t tightBytes=static_cast<size_t>(tightBytes64);
        std::vector<uint8_t> bytes;
        if(!m_testHooks->cacheCapture(bytes)||bytes.size()!=tightBytes)return false;
        capture.pixels=std::move(bytes);capture.width=m_outputW;capture.height=m_outputH;
        return true;
    }
    // The synchronous form owns the whole ring, so it may only be used while nothing is
    // in flight. The offline job uses it for the first frame, whose evidence receipt loop
    // has to read a capture back before deciding whether to resubmit the same frame.
    if(m_capturePending)return false;
    if(!EnqueueEvaluatedFrameCapture())return false;
    return ResolveOldestCapture(capture);
}

bool D3D12Renderer::EnqueueEvaluatedFrameCapture(){
    if(!m_lastDLSSUsed||!m_outputW||!m_outputH)return false;
    if(m_capturePending>=CaptureSlots)return false;
    const bool nv12=m_captureFormat==CaptureFormat::Nv12;
    if(m_gpuUnusable||!m_cacheOutput||!m_dlssOutput||!m_queue||!m_rootSig||!m_psoCacheCapture)
        return false;
    if(nv12&&(!m_captureLuma||!m_captureChroma||!m_psoCaptureLuma||!m_psoCaptureChroma))return false;
    const uint32_t readbackSlot=m_captureWrite;
    if(!m_cacheReadback[readbackSlot])return false;
    const uint32_t slot=m_frameSlot%FrameCount;
    if(!WaitForFrameSlot(slot, &m_captureSubmitSlotWaitNanos))return false;
    if(!HR(m_allocators[slot]->Reset(),"Reset cache-capture allocator"))return false;
    auto*cmd=m_cmds[slot].Get();
    if(!HR(cmd->Reset(m_allocators[slot].Get(),nullptr),"Reset cache-capture command list"))
        return false;
    ID3D12DescriptorHeap*heaps[]={m_srvHeap.Get()};cmd->SetDescriptorHeaps(1,heaps);
    RecordReferenceUpload(cmd,slot);
    D3D12_VIEWPORT viewport{0,0,float(m_outputW),float(m_outputH),0,1};
    D3D12_RECT scissor{0,0,LONG(m_outputW),LONG(m_outputH)};
    cmd->RSSetViewports(1,&viewport);cmd->RSSetScissorRects(1,&scissor);
    // No clear on either capture plane, for the reason the frame pass gives: the
    // full-screen triangle below writes every texel of the plane it is drawing into, so
    // a clear in front of it is an extra full-plane write on the path whose cost is
    // proportional to the frame.
    auto target=RTV(nv12?FrameCount+3:FrameCount+2);cmd->OMSetRenderTargets(1,&target,FALSE,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(nv12?m_psoCaptureLuma.Get():m_psoCacheCapture.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(1));
    // Cache frames are always the bare neural output: no comparison, no color/zoom.
    SetPresentConstants(cmd,ColorSettings{},ComparisonSettings{},false);
    cmd->DrawInstanced(3,1,0,0);
    // A single set of capture targets is enough even with several captures in flight:
    // the next frame's draw into them and this frame's copy out of them are recorded on
    // the same queue, so the GPU already runs them in order.
    auto copyPlane=[&](ID3D12Resource*plane,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT&footprint){
        Barrier(cmd,plane,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource=m_cacheReadback[readbackSlot].Get();
        destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint=footprint;
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=plane;
        source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        Barrier(cmd,plane,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    };
    if(nv12){
        // The chroma pass reads the same neural output at half resolution, averaging each
        // 2x2 block after conversion.
        D3D12_VIEWPORT chromaViewport{0,0,float(m_outputW/2u),float(m_outputH/2u),0,1};
        D3D12_RECT chromaScissor{0,0,LONG(m_outputW/2u),LONG(m_outputH/2u)};
        cmd->RSSetViewports(1,&chromaViewport);cmd->RSSetScissorRects(1,&chromaScissor);
        auto chromaTarget=RTV(FrameCount+4);cmd->OMSetRenderTargets(1,&chromaTarget,FALSE,nullptr);
        cmd->SetPipelineState(m_psoCaptureChroma.Get());
        cmd->DrawInstanced(3,1,0,0);
        copyPlane(m_captureLuma.Get(),m_lumaFootprint);
        copyPlane(m_captureChroma.Get(),m_chromaFootprint);
    }else{
        copyPlane(m_cacheOutput.Get(),m_cacheFootprint);
    }
    if(!HR(cmd->Close(),"Close cache-capture command list"))return false;
    ID3D12CommandList*lists[]={cmd};m_queue->ExecuteCommandLists(1,lists);
    // Signal only. The old code drained the entire queue here, which idled the GPU for
    // the full CPU copy and encode stage of every frame.
    if(!SignalFrameSlot(slot))return false;
    m_captureFence[readbackSlot]=m_fenceValue;
    m_captureWrite=(readbackSlot+1u)%CaptureSlots;
    ++m_capturePending;
    return true;
}

bool D3D12Renderer::BeginResolveOldestCapture(CaptureReadbackView&view){
    view=CaptureReadbackView{};
    if(!m_capturePending)return false;
    const uint32_t readbackSlot=m_captureRead;
    const bool nv12=m_captureFormat==CaptureFormat::Nv12;
    const uint64_t pixels64=uint64_t{m_outputW}*m_outputH;
    const uint64_t tightBytes64=nv12?pixels64+pixels64/2u:pixels64*4u;
    if(!m_outputW||!m_outputH||tightBytes64>std::numeric_limits<size_t>::max()){
        EndResolveOldestCapture();return false;
    }
    if(!WaitForFenceValue(m_captureFence[readbackSlot], &m_captureResolveWaitNanos)){
        EndResolveOldestCapture();return false;
    }
    const uint8_t*base=m_cacheReadbackMapped[readbackSlot];
    if(!base){EndResolveOldestCapture();return false;}
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT&plane=nv12?m_lumaFootprint:m_cacheFootprint;
    view.base=base+plane.Offset;
    view.rowPitch=size_t(plane.Footprint.RowPitch);
    view.bytes=static_cast<size_t>(tightBytes64);
    view.width=m_outputW;view.height=m_outputH;
    view.format=m_captureFormat;
    if(nv12){
        view.chromaBase=base+m_chromaFootprint.Offset;
        view.chromaRowPitch=size_t(m_chromaFootprint.Footprint.RowPitch);
    }
    return true;
}

void D3D12Renderer::EndResolveOldestCapture(){
    if(!m_capturePending)return;
    m_captureRead=(m_captureRead+1u)%CaptureSlots;
    --m_capturePending;
}

// static
void D3D12Renderer::CopyCaptureView(const CaptureReadbackView&view,std::vector<uint8_t>&pixels){
    // Only resize when the caller handed back a differently sized buffer. Constructing a
    // fresh vector here value-initialised a whole frame, over 30 MB of pointless memset
    // per frame at 4K, immediately before overwriting every byte of it.
    if(pixels.size()!=view.bytes)pixels.resize(view.bytes);
    uint8_t*out=pixels.data();
    auto&pool=CaptureCopyPool();
    if(view.format==CaptureFormat::Nv12){
        // Y rows, then the interleaved UV rows. Both planes are one byte per sample and
        // the chroma plane is half as wide with two samples per texel, so the tight row
        // is the frame width for each of them.
        const size_t row=size_t(view.width);
        const size_t lumaHeight=size_t(view.height);
        uint8_t*const chromaOut=out+row*lumaHeight;
        ParallelForRangesIn(pool,lumaHeight,kParallelRowGrain,[&](size_t begin,size_t end){
            for(size_t y=begin;y<end;++y)memcpy(out+row*y,view.base+view.rowPitch*y,row);
        });
        ParallelForRangesIn(pool,lumaHeight/2u,kParallelRowGrain,[&](size_t begin,size_t end){
            for(size_t y=begin;y<end;++y)
                memcpy(chromaOut+row*y,view.chromaBase+view.chromaRowPitch*y,row);
        });
        return;
    }
    const size_t tightRow=size_t(view.width)*4u;
    // No channel swizzle: the cache target is B8G8R8A8 and the encoder is started with
    // EncoderPixelFormat::Bgra, so ffmpeg consumes this layout directly.
    if(view.rowPitch==tightRow){
        ParallelForRangesIn(pool,view.bytes,kParallelCopyGrain,[&](size_t begin,size_t end){
            memcpy(out+begin,view.base+begin,end-begin);
        });
    }else{
        ParallelForRangesIn(pool,size_t(view.height),kParallelRowGrain,[&](size_t begin,size_t end){
            for(size_t y=begin;y<end;++y)memcpy(out+tightRow*y,view.base+view.rowPitch*y,tightRow);
        });
    }
}

bool D3D12Renderer::ResolveOldestCapture(CapturedVideoFrame&capture){
    capture.width=0;capture.height=0;
    CaptureReadbackView view;
    if(!BeginResolveOldestCapture(view)){capture.pixels.clear();return false;}
    CopyCaptureView(view,capture.pixels);
    EndResolveOldestCapture();
    capture.width=view.width;capture.height=view.height;
    return true;
}

bool D3D12Renderer::PresentCurrent(){
    if(m_gpuUnusable||!m_swapchain||!m_queue||!m_rootSig)return false;
    const uint32_t slot=m_frameSlot%FrameCount;
    if(!WaitForFrameSlot(slot, &m_presentSlotWaitNanos))return false;
    if(!HR(m_allocators[slot]->Reset(),"Reset static-present allocator"))return false;
    auto* cmd=m_cmds[slot].Get();
    if(!HR(cmd->Reset(m_allocators[slot].Get(),nullptr),"Reset static-present command list"))return false;
    ID3D12DescriptorHeap*heaps[]={m_srvHeap.Get()};cmd->SetDescriptorHeaps(1,heaps);
    HarvestNeuralTimings();
    RecordReferenceUpload(cmd,slot);

    uint32_t bi=m_swapchain->GetCurrentBackBufferIndex();
    Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_VIEWPORT ovp{0,0,float(m_outputW),float(m_outputH),0,1};
    D3D12_RECT osc{0,0,LONG(m_outputW),LONG(m_outputH)};
    cmd->RSSetViewports(1,&ovp);cmd->RSSetScissorRects(1,&osc);
    auto brt=RTV(bi);cmd->OMSetRenderTargets(1,&brt,FALSE,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const bool finalView=(m_debugView==DebugView::Final);
    SetPresentConstants(cmd,finalView?m_colorSettings:ColorSettings{},finalView?m_comparison:ComparisonSettings{},finalView&&m_hasReference);

    ID3D12Resource* debugPixelResource=nullptr;
    D3D12_RESOURCE_STATES debugBefore=GuideReadState;
    switch(m_debugView){
        case DebugView::MotionVectors:debugPixelResource=m_motion.Get();cmd->SetPipelineState(m_psoMotionDebug.Get());cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(2));break;
        case DebugView::Depth:debugPixelResource=m_depth.Get();debugBefore=DepthGuideReadState;cmd->SetPipelineState(m_psoDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(3));break;
        case DebugView::Input:debugPixelResource=m_dlssColor.Get();cmd->SetPipelineState(m_psoPresent.Get());cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(4));break;
        default:
            cmd->SetPipelineState(m_psoPresent.Get());
            if(m_lastDLSSUsed&&DLSSEnabled())cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(1));
            else{debugPixelResource=m_dlssColor.Get();cmd->SetGraphicsRootDescriptorTable(RootView,SRVGPU(4));}
            break;
    }
    if(debugPixelResource)Barrier(cmd,debugPixelResource,debugBefore,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->DrawInstanced(3,1,0,0);
    if(debugPixelResource)Barrier(cmd,debugPixelResource,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,debugBefore);
    Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    if(!HR(cmd->Close(),"Close static-present command list"))return false;
    ID3D12CommandList*ls[]={cmd};m_queue->ExecuteCommandLists(1,ls);
    const auto presented=std::chrono::steady_clock::now();
    HRESULT phr=m_swapchain->Present(0,m_allowTearing?DXGI_PRESENT_ALLOW_TEARING:0);
    m_presentNanos+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-presented).count());
    if(FAILED(phr)){LOG("Static Present failed hr=0x"<<std::hex<<phr);return false;}
    return SignalFrameSlot(slot);
}

// Reads back every slot whose DLSS timestamps were resolved by a fence value the
// GPU has already passed. Slots are ordered by fence so the newest complete
// evaluation wins; slots still in flight are left for a later call.
void D3D12Renderer::HarvestNeuralTimings(){
    if(!m_timestampMapped||!m_fence||!m_timestampFrequency)return;
    const uint64_t completed=m_fence->GetCompletedValue();
    if(completed==UINT64_MAX)return;
    uint64_t bestFence=0;uint32_t bestSlot=FrameCount;
    for(uint32_t slot=0;slot<FrameCount;++slot){
        if(!m_neuralTimingPending[slot]||m_frameFence[slot]>completed)continue;
        if(m_frameFence[slot]>=bestFence){bestFence=m_frameFence[slot];bestSlot=slot;}
    }
    if(bestSlot==FrameCount)return;
    const uint64_t begin=m_timestampMapped[bestSlot*2u],end=m_timestampMapped[bestSlot*2u+1u];
    if(end>begin)m_lastNeuralGpuMs=double(end-begin)*1000.0/double(m_timestampFrequency);
    for(uint32_t slot=0;slot<FrameCount;++slot)
        if(m_neuralTimingPending[slot]&&m_frameFence[slot]<=completed)m_neuralTimingPending[slot]=false;
}

void D3D12Renderer::SampleLocalVideoMemory(){
    if(!m_adapter3)return;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if(FAILED(m_adapter3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&info)))return;
    const uint64_t mib=info.CurrentUsage>>20;
    if(mib>m_peakLocalVideoMemoryMiB)m_peakLocalVideoMemoryMiB=mib;
}

uint64_t D3D12Renderer::CurrentLocalVideoMemoryMiB()const{
    if(!m_adapter3)return 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if(FAILED(m_adapter3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&info)))return 0;
    return info.CurrentUsage>>20;
}

bool D3D12Renderer::ReleaseDLSSFeatureForIdle(){
    if(!m_dlss.FeatureCreated())return false;
    if(!WaitGPUForContinuedUse())return false;
    return m_dlss.ReleaseFeatureFreeingMemory();
}

void D3D12Renderer::Barrier(ID3D12GraphicsCommandList*cmd,ID3D12Resource*res,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){if(a==b)return;auto x=Transition(res,a,b);cmd->ResourceBarrier(1,&x);}
bool D3D12Renderer::WaitForFenceValue(uint64_t value,uint64_t* stageWaitNanos){
    if(!value)return true;
    if(!m_fence||!m_fenceEvent)return false;
    const auto waited=std::chrono::steady_clock::now();
    const auto waitResult=d3d12_renderer_detail::WaitForGPUFenceCompletion(
        value,
        GetTickCount64(),
        d3d12_renderer_detail::RenderFenceWaitMilliseconds,
        [&]{return m_fence->GetCompletedValue();},
        [&](uint64_t v){return m_fence->SetEventOnCompletion(v,m_fenceEvent);},
        [&](DWORD timeout){return WaitForSingleObject(m_fenceEvent,timeout);});
    const uint64_t elapsed=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-waited).count());
    m_fenceWaitNanos+=elapsed;
    if(stageWaitNanos)*stageWaitNanos+=elapsed;
    const auto result=d3d12_renderer_detail::ClassifyFenceWaitFailure(
        waitResult,[&]{return m_device->GetDeviceRemovedReason();});
    if(result!=d3d12_renderer_detail::FenceWaitResult::Completed){m_gpuUnusable=true;m_lastFenceWaitResult=result;}
    return result==d3d12_renderer_detail::FenceWaitResult::Completed;
}
bool D3D12Renderer::WaitForFrameSlot(uint32_t slot,uint64_t* stageWaitNanos){
    if(slot>=FrameCount||!m_fence||!m_fenceEvent)return false;
    return WaitForFenceValue(m_frameFence[slot],stageWaitNanos);
}
bool D3D12Renderer::SignalFrameSlot(uint32_t slot){
    if(m_gpuUnusable||slot>=FrameCount)return false;
    const uint64_t v=m_fenceValue+1;
    HRESULT signalResult=E_FAIL;
    if(m_testHooks&&m_testHooks->frameSignal)signalResult=m_testHooks->frameSignal(v);
    else
    {
        if(!m_queue||!m_fence)return false;
        signalResult=m_queue->Signal(m_fence.Get(),v);
    }
    if(FAILED(signalResult)){
        HRESULT removedReason=S_OK;
        if(m_testHooks&&m_testHooks->deviceRemovedReason)removedReason=m_testHooks->deviceRemovedReason();
        else if(m_device)removedReason=m_device->GetDeviceRemovedReason();
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
d3d12_renderer_detail::FenceWaitResult D3D12Renderer::WaitGPU(DWORD budgetMilliseconds){
    if(m_testHooks&&m_testHooks->waitGPU){
        const auto result=m_testHooks->waitGPU();m_lastFenceWaitResult=result;
        if(result!=d3d12_renderer_detail::FenceWaitResult::Completed)m_gpuUnusable=true;
        return result;
    }
    if(!m_queue||!m_fence||!m_fenceEvent)return d3d12_renderer_detail::FenceWaitResult::Completed;
    const uint64_t v=++m_fenceValue;
    const auto result=d3d12_renderer_detail::WaitForGPUFenceDrain(
        v,
        budgetMilliseconds,
        [&](uint64_t value){return m_queue->Signal(m_fence.Get(),value);},
        [&]{return m_fence->GetCompletedValue();},
        [&](uint64_t value){return m_fence->SetEventOnCompletion(value,m_fenceEvent);},
        [&](DWORD timeout){return WaitForSingleObject(m_fenceEvent,timeout);},
        [&]{return m_device->GetDeviceRemovedReason();});
    if(result!=d3d12_renderer_detail::FenceWaitResult::Completed)
        LOG("GPU fence wait failed after "<<budgetMilliseconds<<" ms.");
    m_lastFenceWaitResult=result;
    if(result!=d3d12_renderer_detail::FenceWaitResult::Completed)m_gpuUnusable=true;
    return result;
}
bool D3D12Renderer::WaitGPUForContinuedUse(){
    const bool completed=WaitGPU(d3d12_renderer_detail::RenderFenceWaitMilliseconds)==d3d12_renderer_detail::FenceWaitResult::Completed;
    if(!completed)m_gpuUnusable=true;
    return completed;
}
d3d12_renderer_detail::FenceWaitResult D3D12Renderer::DrainForRetirement(){
    if(m_gpuUnusable)return m_lastFenceWaitResult;
    return WaitGPU(d3d12_renderer_detail::TeardownFenceWaitMilliseconds);
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::RTV(uint32_t i)const{auto h=m_rtvHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*m_rtvInc;return h;}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::DSV()const{return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::SRVCPU(uint32_t i)const{auto h=m_srvHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*m_srvInc;return h;}
D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::SRVGPU(uint32_t i)const{auto h=m_srvHeap->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(i)*m_srvInc;return h;}
