#pragma once

#include <cstdint>
#include <d3d12.h>
#include <dxgiformat.h>
#include <string>
#include <wrl/client.h>

// AMD FSR Upscaling 4.1.1 ("FSR4") as this player's playback upscaler, loaded
// from AMD's own signed runtime the same way the frame generation is: the
// loader DLL next to the executable, and the effect's C API through the
// published descriptor structs (see FsrUpscaleAbi.h for why nothing here is
// transcribed from memory).
//
// WHERE IT SITS: it replaces the stretch half of the present chain's own
// stretch-and-sharpen (AmdUpscaleShader.h). The CAS sharpen after it, the
// descriptor slot present samples and every pass above it are untouched, so
// "the upscale is FSR4 now" is a one-pass change and the filter it replaced
// stays as the fallback.
//
// WHAT VIDEO DOES NOT HAVE: FSR expects a rendered frame - jittered colour,
// depth, motion vectors, an exposure - and a camera. None of that survives
// encoding, so it is supplied the way the vendor allows rather than invented:
//
//   depth          flat 1.0 everywhere (no disocclusion can be inferred, which
//                  is the honest answer: there is no geometry)
//   motion vectors zero (the reprojection is then the identity; FSR's own
//                  shading-change term is what keeps history off moving pixels)
//   exposure       1x1, constant 1.0 (no auto-exposure, so a bright scene
//                  cannot be read as an exposure change and flushed)
//   jitter         zero, reported as zero. Jitter is what lets a renderer
//                  supersample over time; a decoded frame is one fixed sample
//                  grid, and moving it per frame is a visible wobble unless
//                  the reprojection compensates - which, with zero motion,
//                  it does not.
//
// Every one of those is a constant texture written once, because the trap the
// frame generation hit (Map/Unmap of a staging buffer while an FFX context is
// alive -> TDR on the next execute) applies to any upload made after the
// context exists.
class FsrUpscaleBackend {
public:
    ~FsrUpscaleBackend();

    // Whether the runtime is on this machine at all: the loader, and the
    // upscaler implementation the loader brings up for the UPSCALE effect.
    // `detail` gets the paths that were tried when it is not.
    static bool RuntimePresent(std::wstring* foundPath = nullptr, std::wstring* detail = nullptr);
    static std::wstring RuntimeSearchDescription();

    // Creates the context for this geometry. `device` is the renderer's own.
    // Guides are uploaded with a command list of this backend's own, because
    // the renderer has none at the point these resources are built.
    bool Initialize(ID3D12Device* device, uint32_t renderW, uint32_t renderH,
                    uint32_t outputW, uint32_t outputH, DXGI_FORMAT colorFormat,
                    DXGI_FORMAT outputFormat);

    bool Available() const { return m_context != nullptr; }
    // Set once a dispatch has failed: the caller must not try again, because
    // the failure is a machine-level one (device removed, runtime refused).
    bool Failed() const { return m_failed; }

    // One upscale. `color` must be at renderSize in NON_PIXEL_SHADER_RESOURCE
    // and `output` at upscaleSize in UNORDERED_ACCESS; the caller owns both
    // barriers, because the present chain already tracks those states.
    // `reset` clears the accumulated history (seek, switch, first frame).
    bool Dispatch(ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* output,
                  bool reset, float frameTimeMs);

    void Shutdown();

    uint32_t LastErrorCode() const { return m_lastError; }
    std::wstring LastDetail() const { return m_lastDetail; }
    uint64_t DispatchCount() const { return m_dispatches; }

private:
    bool LoadRuntime();
    bool CreateContext(ID3D12Device* device);
    // The three constant inputs, filled on the GPU (see the class comment).
    bool CreateGuides(ID3D12Device* device);

    void* m_context = nullptr;      // fsr_fg::ffxContext - the handle, not the slot
    HMODULE m_module = nullptr;
    uint32_t m_lastError = 0;
    std::wstring m_lastDetail;
    uint64_t m_dispatches = 0;
    bool m_failed = false;

    uint32_t m_renderW = 0, m_renderH = 0;
    uint32_t m_outputW = 0, m_outputH = 0;
    DXGI_FORMAT m_colorFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT m_outputFormat = DXGI_FORMAT_UNKNOWN;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_motion;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_exposure;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_guideRtvHeap;
};
