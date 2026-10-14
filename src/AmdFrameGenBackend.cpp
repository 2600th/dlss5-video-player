#include "AmdFrameGenBackend.h"

#include "FrameGenBlendShader.h"
#include "FrameGenFlowShader.h"

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

// The matcher and the blend are both shared with the playback renderer now: one
// implementation each, two consumers. This build's blend is the per-tile one,
// because the matcher writes one displacement per tile.
constexpr const char* const kFlowHlsl = frame_gen_flow::kHlsl;
constexpr const char* const kBlendHlsl = frame_gen_blend::kHlsl;

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
                                  uint32_t multiFrameIndex, bool reset, uint64_t /*backbufferFrameId*/,
                                  float /*frameTimeMs*/)
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
