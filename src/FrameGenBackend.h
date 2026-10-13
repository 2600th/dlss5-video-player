#pragma once
#include <cstdint>
#include <d3d12.h>
#include <dxgiformat.h>
#include <memory>
#include <string>

// A backend for the offline Frame Generation pass (FrameGenerationPass). The
// pass hands over two decoded backbuffer frames and asks for the frames between
// them; what the backend does with them is its own business.
//
// NVIDIA's path is NVidia NGX DLSS-G (DLSSGBackend): it owns the swapchain-adjacent
// generation and needs nvngx_dlssg.dll + an NGX session, which is exactly what is
// unavailable on AMD - that is why the capability query used to report
// "refused by the driver" there. The other implementation, AmdFrameGenBackend,
// needs no NGX: it estimates its own motion with a D3D12 compute optical-flow
// pass and warps+blends the two frames, so it runs on any D3D12 adapter.
//
// The interface is deliberately small and matches the surface FrameGenerationPass
// already used on DLSSGBackend, so the pass only has to switch from a concrete
// type to a std::unique_ptr<IFrameGenBackend> selected by CreateFrameGenBackend.
class IFrameGenBackend {
public:
    virtual ~IFrameGenBackend() = default;

    // Backend identity, for diagnostics and logs.
    virtual const char* Kind() const = 0;

    // Bring the backend up for the given backbuffer geometry. Returns false
    // (with a reason in LastErrorCode / LastDetail) when the runtime this
    // backend depends on is unavailable on this machine.
    virtual bool Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                            uint32_t width, uint32_t height, DXGI_FORMAT backbufferFormat) = 0;

    // Generate intermediate frame `multiFrameIndex` (1-based, <= multiFrameCount)
    // between the PREVIOUS evaluated backbuffer and `backbuffer`. `motion` and
    // `depth` are the guides the NVIDIA path consumes (zero motion / flat depth);
    // a backend that derives its own motion may ignore them. `reset` breaks
    // temporal continuity (first frame, scene cut): the backend should store
    // `backbuffer` as its history and produce no real in-between. `backbufferFrameId`
    // is a per-source-frame counter some runtimes want.
    virtual bool Evaluate(ID3D12GraphicsCommandList* cmd,
                          ID3D12Resource* backbuffer,
                          ID3D12Resource* motion,
                          ID3D12Resource* depth,
                          ID3D12Resource* outputInterpolated,
                          uint32_t multiFrameCount,
                          uint32_t multiFrameIndex,
                          bool reset,
                          uint64_t backbufferFrameId = 0) = 0;

    virtual uint64_t EvaluationCount() const = 0;
    // The runtime's own ceiling on generated frames per source pair; the largest
    // multiplier the pass may offer is one more than this. 0 until Initialize.
    virtual uint32_t MultiFrameCountMax() const = 0;

    // A code a caller can show when something failed (0 == success).
    virtual uint32_t LastErrorCode() const = 0;
    // A human-readable reason for the last failure, already composed.
    virtual std::wstring LastDetail() const { return {}; }

    // Whether the runtime admitted the feature. False until Initialize succeeds,
    // false again after Shutdown.
    virtual bool Available() const = 0;
    virtual bool FeatureCreated() const = 0;
    // True once the session/runtime the backend leans on came up, so a caller can
    // tell a runtime refusal (a verdict) from the backend never initializing
    // (not a verdict).
    virtual bool SessionEstablished() const = 0;

    virtual void Shutdown() = 0;
    // Forget the feature and its session WITHOUT releasing them, for a GPU that
    // may still be executing work that references them.
    virtual void Abandon() = 0;
};

// Selects the backend for the adapter the conversion runs on, mirroring
// SelectAdapter's preference: NVIDIA adapters keep the NGX path (measured on an
// RTX 5090); anything else gets the vendor-neutral optical-flow backend that
// needs no NGX.
std::unique_ptr<IFrameGenBackend> CreateFrameGenBackend(bool preferNvidia);
