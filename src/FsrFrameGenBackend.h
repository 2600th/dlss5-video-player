#pragma once
#include <cstdint>
#include <d3d12.h>
#include <dxgiformat.h>
#include <string>
#include <windows.h>
#include <wrl/client.h>

#include "FrameGenBackend.h"
#include "FsrFrameGenAbi.h"

// Frame generation through AMD FidelityFX, by loading AMD's own signed runtime
// (amd_fidelityfx_framegeneration_dx12.dll) and calling its C API.
//
// Why this exists, and why it is the preferred path on AMD:
//
//   AmdFrameGenBackend estimates motion itself - a D3D12 compute block matcher
//   feeding a warp+blend. It runs anywhere D3D12 does, but its output is only as
//   good as a single-scale 8x8 matcher, and measured on a known-answer clip it
//   did not land the intermediate frame on the content at all. FidelityFX is
//   AMD's own frame generator, built and validated by the vendor for exactly
//   this GPU class; it infers motion and disocclusion from the colour buffers
//   themselves, so it needs none of the guides the NVIDIA path asks for.
//
// Licensing is why this is a separate backend rather than a patch of the other
// one. FidelityFX's licence (FidelityFX-SDK/docs/license.md) grants
// redistribution in binary form, with two obligations this file and the build
// satisfy: the copyright and permission notice must travel with the
// distribution (see docs/THIRD_PARTY-NOTICES.md), and the binary may not be
// reverse engineered - which is why nothing here reaches inside the DLL, and
// why every field goes through the published descriptor structs. The player
// itself stays under its own MIT terms; linking a vendor's binary at runtime is
// not a derivative work of it. (The MPCVR-FSR-FrameGen project that pointed at
// this DLL is GPL v3, but that covers its DirectShow video renderer, NOT the
// frame generation - the DLL is AMD's, not theirs. No code was taken from it.)
//
// The DLL is discovered next to the executable (and in a couple of standard SDK
// locations) rather than being vendored, so a build without the SDK still
// compiles and every other feature still works; this backend simply reports
// itself unavailable and CreateFrameGenBackend falls back to the self-contained
// matcher.
class FsrFrameGenBackend final : public IFrameGenBackend {
public:
    ~FsrFrameGenBackend() override;

    const char* Kind() const override { return "amd-fidelityfx"; }

    // Loads the runtime and creates the feature context for the given geometry.
    // `cmd` is used only for the copy that uploads the flat-depth / zero-motion
    // guides the API declares; ownership stays with the caller.
    bool Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                    uint32_t width, uint32_t height, DXGI_FORMAT backbufferFormat) override;

    // ISOLATED-ISLAND mode, for the playback renderer. The difference from
    // Initialize is WHERE the runtime lives, and it is the whole point:
    //
    //   Initialize runs the context on the CALLER's device, recorded into the
    //   caller's command list beside everything else that frame does. Measured
    //   on the RX 9070 XT (2026-10-14), that arrangement removes the renderer's
    //   device within ~45 ms of the context being created - before a single
    //   evaluate - and the player dies with it ("Create decoded texture failed
    //   hr=0x887a0005"). The offline conversion, which owns its device, and
    //   probe-video, which owns its device, both run the same backend, the same
    //   DLL and the same GPU without a removal. The runtime's internal
    //   initialization evidently does not tolerate the company it keeps there.
    //
    //   InitializeIsolated gives the runtime a device of its own - created on
    //   `adapterLuid` so it is the same physical GPU - with its own queue,
    //   allocator, list and fence. Every EvaluateIsolated submission is
    //   executed and waited out before it returns, exactly like MPCVR-FSR-
    //   FrameGen's FsrFrameGenerator (the reference this architecture is taken
    //   from) and probe-video. A fault inside the runtime then removes the
    //   ISLAND's device and nothing else: the caller is told (false return),
    //   drops the backend and falls back to its own generator.
    //
    // The frames themselves cross the device boundary through shared textures
    // the CALLER created and keeps owning: `sharedInput` (the converted source
    // frame, written by the caller) and `sharedOutput` (the generated frame,
    // read by the caller). `sharedFence` carries the one direction that needs
    // GPU-side ordering - the island waits on the value the caller signals
    // after writing the input - and the island's own completion wait covers
    // the other. `sharedFence` must be created with D3D12_FENCE_FLAG_SHARED.
    bool InitializeIsolated(const LUID& adapterLuid,
                            uint32_t width, uint32_t height, DXGI_FORMAT format,
                            HANDLE sharedInput, HANDLE sharedOutput, HANDLE sharedFence);

    // One generation on the island. The caller must have signalled the shared
    // fence at `waitValue` after writing `backbuffer`; this waits on it
    // (GPU-side), dispatches, and returns only after the GPU work has retired
    // (bounded) - which also means the caller's input write has retired. A
    // false return means the bridge or the island is finished: the caller
    // falls back and never calls this again.
    bool EvaluateIsolated(ID3D12Resource* backbuffer, ID3D12Resource* outputInterpolated,
                          uint64_t waitValue, bool reset, uint64_t frameId, float frameTimeMs);

    // False once the island has been abandoned (its device was removed, or a
    // wait timed out). The renderer asks so it can tell "the runtime refused
    // this machine" from "the runtime took the island down mid-playback".
    bool IslandAlive() const { return m_island; }

    bool Evaluate(ID3D12GraphicsCommandList* cmd,
                  ID3D12Resource* backbuffer,
                  ID3D12Resource* motion,
                  ID3D12Resource* depth,
                  ID3D12Resource* outputInterpolated,
                  uint32_t multiFrameCount,
                  uint32_t multiFrameIndex,
                  bool reset,
                  uint64_t backbufferFrameId = 0, float frameTimeMs = 0.0f) override;

    uint64_t EvaluationCount() const override { return m_evaluations; }
    // FFX_FRAMEGENERATION dispatches one interpolated frame per call, so 2x is
    // what this backend offers, matching what the self-contained matcher claims.
    uint32_t MultiFrameCountMax() const override { return m_multiFrameCountMax; }

    uint32_t LastErrorCode() const override { return m_lastError; }
    std::wstring LastDetail() const override { return m_lastDetail; }

    bool Available() const override { return m_context != nullptr; }
    bool FeatureCreated() const override { return m_context != nullptr; }
    bool SessionEstablished() const override { return m_module != nullptr; }

    void Shutdown() override;
    void Abandon() override;

    // Where the runtime was found, or the paths that were tried. For the log:
    // "this machine has no AMD FidelityFX" and "the DLL is right there" are very
    // different bugs to the reader.
    static bool RuntimePresent(std::wstring* foundPath = nullptr);
    // The directory searched, exposed so a caller can name it in a message.
    static std::wstring RuntimeSearchDescription();

private:
    bool LoadRuntime();
    bool CreateContext(ID3D12Device* device);
    // Copies the constant guides to their device textures. Split out of
    // Initialize because the playback renderer has no command list at
    // construction time - it calls this on the first Evaluate instead.
    bool UploadGuides(ID3D12GraphicsCommandList* cmd);
    // Island plumbing: a device of the runtime's own, and the bounded
    // execute-and-wait every EvaluateIsolated submission goes through.
    bool CreateIsland(const LUID& adapterLuid);
    bool ExecuteAndWaitIsland();
    void AbandonIsland();

    HMODULE m_module = nullptr;
    fsr_fg::ffxContext m_context = nullptr;
    ID3D12Device* m_device = nullptr;
    // The island: owned D3D12 objects the runtime lives on in island mode.
    // m_device points at m_islandDevice while that mode is active, so every
    // device-scoped call in this class (guides, RTV heap) lands on the right
    // one without a second member.
    Microsoft::WRL::ComPtr<ID3D12Device> m_islandDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_islandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_islandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_islandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_islandFence;
    HANDLE m_islandFenceEvent = nullptr;
    uint64_t m_islandFenceValue = 0;
    // Opened copies of the caller's shared resources. The island reads
    // m_sharedInput and writes m_sharedOutput; m_sharedFence is the one the
    // caller signals and the island waits on.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sharedInput;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sharedOutput;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_sharedFence;
    bool m_island = false;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    // The API declares depth and motion-vector inputs even though it derives its
    // own motion, so they must be real textures holding something sane. Both are
    // flat and static, allocated once.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_flatDepth;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_zeroMotion;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_guideRtvHeap;
    uint64_t m_frameId = 0;
    uint64_t m_evaluations = 0;
    uint32_t m_multiFrameCountMax = 0;
    uint32_t m_lastError = 0;
    std::wstring m_lastDetail;
    std::wstring m_runtimePath;
    bool m_uploaded = false;
    // Destroying a context that never dispatched crashes inside the runtime
    // (see Shutdown); this records whether this one ever did.
    bool m_dispatched = false;
};