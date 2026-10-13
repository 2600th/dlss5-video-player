#include "AmdFrameGenBackend.h"

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

// Optical flow as coarse block matching. One 8x8 THREAD GROUP per BLOCKxBLOCK
// tile: the group splits the +/-SEARCH window across its 64 threads, each
// measures a strided subset of the candidate displacements, and a groupshared
// reduction picks the winner. A thread per tile (what this started as) left the
// machine 98% idle - one thread cannot cover a texture fetch's latency, and
// this shader is nothing but texture fetches.
//
// The output is one R16G16_FLOAT vector per block, in pixels:
// curr(origin + mv) ~= prev(origin), i.e. curr(p) = prev(p - mv).
constexpr char kFlowHlsl[] = R"hlsl(
cbuffer Params : register(b0) { uint gW; uint gH; uint gBlock; uint gSearch; };
Texture2D<float4> Prev : register(t0);
Texture2D<float4> Curr : register(t1);
RWTexture2D<float2> Flow : register(u0);

// The reduction's scratch, one slot per thread in the group.
groupshared float sSad[64];
groupshared int   sDx[64];
groupshared int   sDy[64];

[numthreads(8, 8, 1)]
void main(uint3 groupId : SV_GroupID, uint3 lid : SV_GroupThreadID) {
    int bw = (int)gW, bh = (int)gH;
    int originX = (int)groupId.x * (int)gBlock;
    int originY = (int)groupId.y * (int)gBlock;
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

// Warp + blend. The previous frame's content advances by mv per frame, so at
// time t the point now at p was at p - t*mv in the previous frame and at
// p + (1-t)*mv in the current one. We sample both and average. Occlusions (a
// coordinate landing outside the frame) are clamped by the bilinear helper, so
// they fade toward the nearer frame rather than sampling garbage.
constexpr char kBlendHlsl[] = R"hlsl(
cbuffer Params : register(b0) { uint gW; uint gH; uint gBlock; uint gTBits; };
Texture2D<float4> Prev : register(t0);
Texture2D<float4> Curr : register(t1);
Texture2D<float2> Flow : register(t2);
RWTexture2D<float4> Out : register(u0);
float4 sampleClamped(Texture2D<float4> tex, int x, int y, int w, int h) {
    x = clamp(x, 0, w - 1); y = clamp(y, 0, h - 1);
    return tex[int2(x, y)];
}
float4 bilinear(Texture2D<float4> tex, float2 p, int w, int h) {
    float2 f = p - 0.5f;
    float2 fl = floor(f);
    float2 t = f - fl;
    int x0 = (int)fl.x, y0 = (int)fl.y;
    int x1 = x0 + 1, y1 = y0 + 1;
    float4 s00 = sampleClamped(tex, x0, y0, w, h);
    float4 s10 = sampleClamped(tex, x1, y0, w, h);
    float4 s01 = sampleClamped(tex, x0, y1, w, h);
    float4 s11 = sampleClamped(tex, x1, y1, w, h);
    return lerp(lerp(s00, s10, t.x), lerp(s01, s11, t.x), t.y);
}
[numthreads(8, 8, 1)]
void main(uint3 gid : SV_DispatchThreadID) {
    int x = (int)gid.x, y = (int)gid.y;
    if (x >= (int)gW || y >= (int)gH) return;
    int bx = x / (int)gBlock, by = y / (int)gBlock;
    float2 mv = Flow[int2(bx, by)];
    float t = asfloat(gTBits);
    float3 pW = bilinear(Prev, float2((float)x, (float)y) - t * mv, (int)gW, (int)gH).rgb;
    float3 cW = bilinear(Curr, float2((float)x, (float)y) + (1.0f - t) * mv, (int)gW, (int)gH).rgb;
    Out[int2(x, y)] = float4(0.5f * (pW + cW), 1.0f);
}
)hlsl";

// ASCII (the only thing shader compilers and D3DCompile diagnostics ever emit)
// to a wide diagnostic string without dragging <locale> or a code-page guess in.
std::wstring ToWide(const std::string& s)
{
    std::wstring w;
    w.reserve(s.size());
    for (char c : s) w.push_back(static_cast<wchar_t>(c));
    return w;
}

} // namespace

AmdFrameGenBackend::~AmdFrameGenBackend() { Shutdown(); }

void AmdFrameGenBackend::Fail(uint32_t code, std::wstring detail)
{
    m_lastError = code;
    m_lastDetail = std::move(detail);
    m_initialized = false;
}

void AmdFrameGenBackend::Shutdown()
{
    m_flowPso.Reset();
    m_blendPso.Reset();
    m_rootSig.Reset();
    m_heap.Reset();
    m_prevFrame.Reset();
    m_flow.Reset();
    m_initialized = false;
}

void AmdFrameGenBackend::Abandon()
{
    // Nothing to leak deliberately: this backend holds no session the GPU may be
    // mid-execution on beyond the textures above, and a stuck context in
    // FrameGenerationPass already retains every resource it references. Just drop
    // our own handles.
    Shutdown();
}

bool AmdFrameGenBackend::CompileShaders()
{
    ComPtr<ID3DBlob> flowBlob, blendBlob, err;
    auto compile = [&](const char* src, const char* entry, const char* name, ComPtr<ID3DBlob>& out) -> bool {
        err.Reset();
        HRESULT hr = D3DCompile(src, std::strlen(src), name, nullptr, nullptr, entry, "cs_5_1",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
        if (FAILED(hr)) {
            std::string msg = err ? static_cast<const char*>(err->GetBufferPointer()) : "unknown shader compile error";
            Fail(0x8000'0001u, L"Optical-flow shader \"" + ToWide(name) + L"\" failed to compile: " + ToWide(msg));
            return false;
        }
        return true;
    };
    if (!compile(kFlowHlsl, "main", "amd-framegen-flow", flowBlob)) return false;
    if (!compile(kBlendHlsl, "main", "amd-framegen-blend", blendBlob)) return false;

    D3D12_DESCRIPTOR_RANGE1 ranges[2];
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3; // t0 prev, t1 curr, t2 flow
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1; // u0 output
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    ranges[1].OffsetInDescriptorsFromTableStart = 3;

    D3D12_ROOT_PARAMETER1 params[2];
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].Constants.Num32BitValues = 4;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.RegisterSpace = 0;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
    rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rsd.Desc_1_1.NumParameters = 2;
    rsd.Desc_1_1.pParameters = params;
    rsd.Desc_1_1.NumStaticSamplers = 0;
    rsd.Desc_1_1.pStaticSamplers = nullptr;
    rsd.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rsd, &sigBlob, &err))) {
        Fail(0x8000'0002u, L"Optical-flow root signature serialization failed.");
        return false;
    }
    if (FAILED(m_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_rootSig)))) {
        Fail(0x8000'0003u, L"Optical-flow root signature creation failed.");
        return false;
    }

    auto pso = [&](ComPtr<ID3DBlob>& blob, ComPtr<ID3D12PipelineState>& psoOut) -> bool {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = m_rootSig.Get();
        desc.CS.pShaderBytecode = blob->GetBufferPointer();
        desc.CS.BytecodeLength = blob->GetBufferSize();
        if (FAILED(m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&psoOut)))) {
            Fail(0x8000'0004u, L"Optical-flow compute pipeline creation failed.");
            return false;
        }
        return true;
    };
    if (!pso(flowBlob, m_flowPso)) return false;
    if (!pso(blendBlob, m_blendPso)) return false;
    return true;
}

bool AmdFrameGenBackend::CreatePipelines() { return CompileShaders(); }

void AmdFrameGenBackend::WriteSrv(uint32_t slot, ID3D12Resource* res, DXGI_FORMAT format)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += slot * m_heapInc;
    m_device->CreateShaderResourceView(res, &srv, h);
}

void AmdFrameGenBackend::WriteUav(uint32_t slot, ID3D12Resource* res, DXGI_FORMAT format)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += slot * m_heapInc;
    m_device->CreateUnorderedAccessView(res, nullptr, &uav, h);
}

bool AmdFrameGenBackend::Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, uint32_t width,
                                    uint32_t height, DXGI_FORMAT backbufferFormat)
{
    (void)cmd; // history is established lazily on the first reset Evaluate
    m_device = device;
    m_width = width;
    m_height = height;
    m_format = backbufferFormat;
    m_lastError = 0;
    m_lastDetail.clear();

    // Previous frame: copy of the prior backbuffer, read as an SRV by the passes.
    D3D12_RESOURCE_DESC prevDesc{};
    prevDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    prevDesc.Width = width;
    prevDesc.Height = height;
    prevDesc.DepthOrArraySize = 1;
    prevDesc.MipLevels = 1;
    prevDesc.Format = backbufferFormat;
    prevDesc.SampleDesc.Count = 1;
    prevDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &prevDesc,
                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                   IID_PPV_ARGS(&m_prevFrame)))) {
            Fail(0x8000'0010u, L"Optical-flow backend could not allocate the history texture.");
            return false;
        }
    }

    // Per-block flow, R16G16_FLOAT.
    const uint32_t blocksX = (width + kBlock - 1) / kBlock;
    const uint32_t blocksY = (height + kBlock - 1) / kBlock;
    D3D12_RESOURCE_DESC flowDesc = prevDesc;
    flowDesc.Width = blocksX;
    flowDesc.Height = blocksY;
    flowDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    flowDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &flowDesc,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                   IID_PPV_ARGS(&m_flow)))) {
            Fail(0x8000'0011u, L"Optical-flow backend could not allocate the flow texture.");
            return false;
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 8;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap)))) {
        Fail(0x8000'0012u, L"Optical-flow backend could not allocate its descriptor heap.");
        return false;
    }
    m_heapInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (!CreatePipelines()) return false;

    m_initialized = true;
    return true;
}

bool AmdFrameGenBackend::Evaluate(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backbuffer,
                                  ID3D12Resource* /*motion*/, ID3D12Resource* /*depth*/,
                                  ID3D12Resource* outputInterpolated, uint32_t multiFrameCount,
                                  uint32_t multiFrameIndex, bool reset, uint64_t /*backbufferFrameId*/)
{
    if (!m_initialized) {
        Fail(0x8000'0020u, L"Optical-flow backend used before Initialize.");
        return false;
    }
    const uint32_t blocksX = (m_width + kBlock - 1) / kBlock;
    const uint32_t blocksY = (m_height + kBlock - 1) / kBlock;

    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        cmd->ResourceBarrier(1, &b);
    };
    auto copy = [&](ID3D12Resource* dst, ID3D12Resource* src) { cmd->CopyResource(dst, src); };

    // reset: store the current frame as history, emit no in-between.
    if (reset) {
        barrier(m_prevFrame.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        barrier(backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        copy(m_prevFrame.Get(), backbuffer);
        barrier(m_prevFrame.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ++m_evaluations;
        return true;
    }

    // Flow: read history (prev) and current (backbuffer), write flow. m_flow's
    // before-state is whichever we last left it in - UNORDERED_ACCESS from
    // Initialize, or NON_PIXEL_SHADER_RESOURCE from the prior blend read.
    // Only issue the transition when it actually changes the state: a barrier
    // whose before- and after-state are equal is rejected by the runtime
    // (cmd->Close() returns E_INVALIDARG), and on the very first pair m_flow is
    // still UNORDERED_ACCESS from Initialize, so an unguarded transition here
    // would be exactly that illegal same-state barrier.
    if (m_flowState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        barrier(m_flow.Get(), m_flowState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_flowState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    WriteSrv(0, m_prevFrame.Get(), m_format);
    WriteSrv(1, backbuffer, m_format);
    // Slot 2 (t2) is unused by the flow shader but part of its descriptor table,
    // so it must still hold a valid SRV; reuse curr to keep the heap fully valid.
    WriteSrv(2, backbuffer, m_format);
    WriteUav(3, m_flow.Get(), DXGI_FORMAT_R16G16_FLOAT);
    {
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
        cmd->SetDescriptorHeaps(1, m_heap.GetAddressOf());
        cmd->SetComputeRootSignature(m_rootSig.Get());
        cmd->SetPipelineState(m_flowPso.Get());
        cmd->SetComputeRootDescriptorTable(0, gpu);
        uint32_t c[4] = {m_width, m_height, kBlock, kSearch};
        cmd->SetComputeRoot32BitConstants(1, 4, c, 0);
        cmd->Dispatch(blocksX, blocksY, 1);
    }
    barrier(m_flow.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_flowState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Blend: warp+blend prev/curr with the flow into the interpolated output.
    const float t = static_cast<float>(multiFrameIndex) / static_cast<float>(multiFrameCount + 1);
    uint32_t tBits = 0;
    std::memcpy(&tBits, &t, sizeof(tBits));
    WriteSrv(0, m_prevFrame.Get(), m_format);
    WriteSrv(1, backbuffer, m_format);
    WriteSrv(2, m_flow.Get(), DXGI_FORMAT_R16G16_FLOAT);
    WriteUav(3, outputInterpolated, m_format);
    {
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
        cmd->SetComputeRootSignature(m_rootSig.Get());
        cmd->SetPipelineState(m_blendPso.Get());
        cmd->SetComputeRootDescriptorTable(0, gpu);
        uint32_t c[4] = {m_width, m_height, kBlock, tBits};
        cmd->SetComputeRoot32BitConstants(1, 4, c, 0);
        cmd->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);
    }

    // Advance history to the current frame for the next pair.
    barrier(m_prevFrame.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy(m_prevFrame.Get(), backbuffer);
    barrier(m_prevFrame.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ++m_evaluations;
    return true;
}
