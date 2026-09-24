#include "HdrToneMapGpu.h"

#include "Log.h"
#include "ParallelFor.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

using Microsoft::WRL::ComPtr;

namespace hdr_tonemap {
namespace {

// HdrToneMap.h's ToneMapP010Pixel and Interpolate, line for line, in the same
// 32-bit integers: any difference between the two is a cache key that no longer
// describes its render, so the test holds them to identical bytes.
constexpr char kShader[] = R"(
ByteAddressBuffer Src : register(t0);
Buffer<uint> Lut : register(t1);
RWByteAddressBuffer Dst : register(u0);
cbuffer Constants : register(b0) {
    uint W; uint H; int YOffset; int KY; int KCrR; int KCbG; int KCrG; int KCbB;
};
uint Sample16(uint offset) {
    uint word = Src.Load(offset & ~3u);
    return (offset & 2u) ? (word >> 16) : (word & 0xFFFFu);
}
int Chroma(uint col, uint row, uint component) {
    return int(Sample16(W * H * 2u + ((row * (W / 2u) + col) * 2u + component) * 2u) >> 6);
}
int ClampCode(int v) { return v < 0 ? 0 : (v > 65535 ? 65535 : v); }
uint LutAt(int r, int g, int b, uint channel) {
    return Lut[(uint(r) * 65u + uint(g)) * 65u * 3u + uint(b) * 3u + channel];
}
uint EncodeLinear(int lin) {
    if (lin <= 0) return 0u;
    uint code = 0u;
    [unroll] for (uint step = 128u; step != 0u; step >>= 1)
        if (code + step <= 255u && uint(lin) >= Lut[274625u * 3u + code + step]) code += step;
    return code;
}
int High(uint node) { return int(node >> 12); }
int Low(uint node) { return int(node & 0xFFFu); }
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint x = id.x, y = id.y;
    if (x >= W || y >= H) return;
    uint cw = W / 2u, ch = H / 2u, j = x >> 1, k = y >> 1;
    uint jn = (x & 1u) ? min(j + 1u, cw - 1u) : j;
    uint kn = (y & 1u) ? min(k + 1u, ch - 1u) : (k != 0u ? k - 1u : 0u);
    int cb = 3 * (Chroma(j, k, 0u) + Chroma(jn, k, 0u)) + (Chroma(j, kn, 0u) + Chroma(jn, kn, 0u)) - 4096;
    int cr = 3 * (Chroma(j, k, 1u) + Chroma(jn, k, 1u)) + (Chroma(j, kn, 1u) + Chroma(jn, kn, 1u)) - 4096;
    int yTerm = (int(Sample16((y * W + x) * 2u) >> 6) * 8 - YOffset) * KY;
    int r = ClampCode((yTerm + KCrR * cr + 4096) >> 13);
    int g = ClampCode((yTerm - KCbG * cb - KCrG * cr + 4096) >> 13);
    int b = ClampCode((yTerm + KCbB * cb + 4096) >> 13);
    int xr = r * 64, xg = g * 64, xb = b * 64;
    int ir = xr >> 16, ig = xg >> 16, ib = xb >> 16;
    int fr = (xr & 0xFFFF) >> 4, fg = (xg & 0xFFFF) >> 4, fb = (xb & 0xFFFF) >> 4;
    int3 d1, d2;
    int w0, w1, w2, w3;
    if (fr >= fg) {
        if (fg >= fb) { d1 = int3(1, 0, 0); d2 = int3(1, 1, 0); w0 = 4096 - fr; w1 = fr - fg; w2 = fg - fb; w3 = fb; }
        else if (fr >= fb) { d1 = int3(1, 0, 0); d2 = int3(1, 0, 1); w0 = 4096 - fr; w1 = fr - fb; w2 = fb - fg; w3 = fg; }
        else { d1 = int3(0, 0, 1); d2 = int3(1, 0, 1); w0 = 4096 - fb; w1 = fb - fr; w2 = fr - fg; w3 = fg; }
    } else {
        if (fb >= fg) { d1 = int3(0, 0, 1); d2 = int3(0, 1, 1); w0 = 4096 - fb; w1 = fb - fg; w2 = fg - fr; w3 = fr; }
        else if (fb >= fr) { d1 = int3(0, 1, 0); d2 = int3(0, 1, 1); w0 = 4096 - fg; w1 = fg - fb; w2 = fb - fr; w3 = fr; }
        else { d1 = int3(0, 1, 0); d2 = int3(1, 1, 0); w0 = 4096 - fg; w1 = fg - fr; w2 = fr - fb; w3 = fb; }
    }
    uint pixel = 0xFF000000u;
    [unroll] for (uint c = 0u; c < 3u; ++c) {
        uint n0 = LutAt(ir, ig, ib, c), n1 = LutAt(ir + d1.x, ig + d1.y, ib + d1.z, c);
        uint n2 = LutAt(ir + d2.x, ig + d2.y, ib + d2.z, c), n3 = LutAt(ir + 1, ig + 1, ib + 1, c);
        int highs = w0 * High(n0) + w1 * High(n1) + w2 * High(n2) + w3 * High(n3);
        int lows = w0 * Low(n0) + w1 * Low(n1) + w2 * Low(n2) + w3 * Low(n3);
        int lin = highs + ((lows + 2048) >> 12) - 8388608;
        pixel |= EncodeLinear(lin) << (16u - 8u * c);
    }
    Dst.Store((y * W + x) * 4u, pixel);
}
)";

struct Constants {
    uint32_t w, h;
    int32_t yOffset, ky, crR, cbG, crG, cbB;
};
static_assert(sizeof(Constants) == 32);

class GpuToneMapper {
public:
    bool Run(const Table& table, const uint8_t* p010, uint32_t w, uint32_t h, uint8_t* bgra)
    {
        std::lock_guard lock(m_mutex);
        if (m_failed) return false;
        if (!m_device && !Initialize()) return Fail("initialisation");
        if (m_lutId != table.id && !UploadLut(table)) return Fail("table upload");
        if ((w != m_w || h != m_h) && !Resize(w, h)) return Fail("buffer allocation");
        const Constants constants{w, h, table.matrix.yOffset, table.matrix.y, table.matrix.crR,
                                  table.matrix.cbG, table.matrix.crG, table.matrix.cbB};
        m_context->UpdateSubresource(m_constants.Get(), 0, nullptr, &constants, 0, 0);
        // The P010 bytes as they came off the pipe; the source buffer is rounded up
        // to whole words and the tail is never read.
        m_context->UpdateSubresource(m_source.Get(), 0, nullptr, p010, 0, 0);
        ID3D11ShaderResourceView* views[] = {m_sourceView.Get(), m_lutView.Get()};
        ID3D11UnorderedAccessView* output = m_outputView.Get();
        ID3D11Buffer* constantBuffer = m_constants.Get();
        m_context->CSSetShader(m_shader.Get(), nullptr, 0);
        m_context->CSSetShaderResources(0, 2, views);
        m_context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        m_context->CSSetConstantBuffers(0, 1, &constantBuffer);
        m_context->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
        ID3D11UnorderedAccessView* none = nullptr;
        m_context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
        m_context->CopyResource(m_readback.Get(), m_output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = m_context->Map(m_readback.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return Fail("readback", hr);
        std::memcpy(bgra, mapped.pData, size_t(w) * h * 4u);
        m_context->Unmap(m_readback.Get(), 0);
        return true;
    }

private:
    bool Fail(const char* step, HRESULT hr = S_OK)
    {
        // A device that failed once is not retried per frame: the CPU path gives the
        // same bytes, so the only cost of staying there is time.
        if (!m_failed) {
            std::ostringstream code;
            if (FAILED(hr)) code << " (hr=0x" << std::hex << static_cast<unsigned long>(hr) << ")";
            LOG("HDR tone map: the GPU pass failed at " << step << code.str()
                << "; tone mapping on the CPU from here on (identical output).");
        }
        m_failed = true;
        return false;
    }

    bool Initialize()
    {
        static constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                       UINT(std::size(levels)), D3D11_SDK_VERSION, &m_device, nullptr, &m_context);
        if (FAILED(hr)) { m_device.Reset(); return false; }
        ComPtr<ID3DBlob> code, errors;
        hr = D3DCompile(kShader, sizeof(kShader) - 1, "HdrToneMap", nullptr, nullptr, "main", "cs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr)) {
            if (errors) LOG("HDR tone map shader: " << static_cast<const char*>(errors->GetBufferPointer()));
            m_device.Reset();
            return false;
        }
        hr = m_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_shader);
        if (FAILED(hr)) { m_device.Reset(); return false; }
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(Constants);
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = m_device->CreateBuffer(&cb, nullptr, &m_constants);
        if (FAILED(hr)) { m_device.Reset(); return false; }
        LOG("HDR tone map: D3D11 compute pass ready (feature level 0x" << std::hex << m_device->GetFeatureLevel()
            << std::dec << ").");
        return true;
    }

    bool UploadLut(const Table& table)
    {
        const std::vector<uint32_t>& wide = table.lut;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = UINT(wide.size() * sizeof(uint32_t));
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{wide.data(), 0, 0};
        m_lut.Reset(); m_lutView.Reset();
        if (FAILED(m_device->CreateBuffer(&desc, &data, &m_lut))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_UINT;
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        view.Buffer.NumElements = UINT(wide.size());
        if (FAILED(m_device->CreateShaderResourceView(m_lut.Get(), &view, &m_lutView))) return false;
        m_lutId = table.id;
        return true;
    }

    bool Resize(uint32_t w, uint32_t h)
    {
        m_source.Reset(); m_sourceView.Reset(); m_output.Reset(); m_outputView.Reset(); m_readback.Reset();
        m_w = m_h = 0;
        const UINT sourceBytes = UINT((P010FrameBytes(w, h) + 3) & ~size_t(3));
        const UINT outputBytes = UINT(size_t(w) * h * 4u);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sourceBytes;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(m_device->CreateBuffer(&desc, nullptr, &m_source))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        view.BufferEx.NumElements = sourceBytes / 4;
        view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(m_device->CreateShaderResourceView(m_source.Get(), &view, &m_sourceView))) return false;
        desc.ByteWidth = outputBytes;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(m_device->CreateBuffer(&desc, nullptr, &m_output))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = outputBytes / 4;
        uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(m_device->CreateUnorderedAccessView(m_output.Get(), &uav, &m_outputView))) return false;
        D3D11_BUFFER_DESC staging{};
        staging.ByteWidth = outputBytes;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_device->CreateBuffer(&staging, nullptr, &m_readback))) return false;
        m_w = w; m_h = h;
        return true;
    }

    std::mutex m_mutex;
    bool m_failed = false;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11ComputeShader> m_shader;
    ComPtr<ID3D11Buffer> m_constants, m_lut, m_source, m_output, m_readback;
    ComPtr<ID3D11ShaderResourceView> m_lutView, m_sourceView;
    ComPtr<ID3D11UnorderedAccessView> m_outputView;
    uint64_t m_lutId = 0;
    uint32_t m_w = 0, m_h = 0;
};

GpuToneMapper& Gpu()
{
    // Never destroyed: a decoder on another thread may still be converting at
    // process exit, and the driver tears the device down with the process.
    static GpuToneMapper* mapper = new GpuToneMapper;
    return *mapper;
}

} // namespace

Table MakeTable(hdr_policy::HdrSignal signal, double peakNits, bool fullRange)
{
    static std::atomic<uint64_t> next{1};
    Table table;
    table.id = next.fetch_add(1);
    table.signal = signal;
    table.peakNits = peakNits;
    table.fullRange = fullRange;
    table.lut = BuildLut(signal, peakNits);
    table.matrix = MatrixFor(fullRange);
    return table;
}

void ToneMapFrameCpu(const Table& table, const uint8_t* p010, uint32_t w, uint32_t h, uint8_t* bgra)
{
    ParallelForRanges(size_t(h), 16, [&](size_t begin, size_t end) {
        ToneMapP010Rows(p010, w, h, table.matrix, table.lut.data(), bgra, uint32_t(begin), uint32_t(end));
    });
}

bool ToneMapFrameGpu(const Table& table, const uint8_t* p010, uint32_t w, uint32_t h, uint8_t* bgra)
{
    return Gpu().Run(table, p010, w, h, bgra);
}

Path ToneMapFrame(const Table& table, const uint8_t* p010, uint32_t w, uint32_t h, uint8_t* bgra)
{
    if (ToneMapFrameGpu(table, p010, w, h, bgra)) return Path::Gpu;
    ToneMapFrameCpu(table, p010, w, h, bgra);
    return Path::Cpu;
}

} // namespace hdr_tonemap
