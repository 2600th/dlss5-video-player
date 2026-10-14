#pragma once

// A local declaration of the slice of AMD FidelityFX's C ABI this backend uses.
//
// The SDK's own headers (Kits/FidelityFX/api/include, .../framegeneration/include)
// are NOT vendored here: FidelityFX's licence grants redistribution in binary
// form, so shipping its sources in this repository is not something the licence
// permits, and the DLL is loaded at runtime rather than linked, so nothing needs
// to be present at build time. What is declared below is the published API
// layout, written out here so the compiler agrees with the DLL about struct
// offsets without the SDK on disk.
//
// That makes this file a place where a FidelityFX version bump can silently
// break things: the descriptors are versioned (FFX_FRAMEGENERATION_VERSION) and
// CreateContext refuses a version it does not know, which is exactly the guard
// that turns a mismatch into a clean error instead of a corrupted frame. If the
// SDK is present, point FFX_SDK_ROOT at it and FSR_FRAMEGEN_USE_SDK_HEADERS picks
// the real headers up instead of these declarations - the two are checked against
// each other in FsrFrameGenBackend.cpp.
//
// Every field below is used by name and in the order the SDK declares it; none
// is a reinterpret_cast of a byte blob, because that is the shape of code that
// silently produces garbage when the vendor reorders a struct.

#include <cstdint>
#include <d3d12.h>

// FFX_API expands to __stdcall on x86 and to nothing on x64; get it from the SDK
// when one is configured, otherwise define the x64 form (which is what every
// 64-bit build of this player uses).
#if defined(FFX_SDK_ROOT)
#include "ffx_api.h"
#include "ffx_framegeneration.h"
#endif

#ifndef FFX_API
#if defined(_M_X64) || defined(__x86_64__)
#define FFX_API
#else
#define FFX_API __stdcall
#endif
#endif

namespace fsr_fg {

// Kits/FidelityFX/api/include/ffx_api_types.h: ffxReturnCode_t is the status
// every entry point returns (FFX_API_RETURN_OK is 0), and ffxContext is the
// opaque handle a created effect lives in.
typedef int32_t ffxReturnCode_t;
// ffx_api.h: typedef void* ffxContext - the runtime hands back an opaque
// implementation the caller only ever passes back in.
using ffxContext = void*;
// Distinguishes the two shapes a void* context appears in: a slot to write the
// handle into, and the handle itself.
using ffxContextSlot = void*;
constexpr ffxReturnCode_t kFfxReturnOk = 0;

// Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h
#define FFX_FRAMEGENERATION_MAKE_VERSION(major, minor, patch) ((major) << 22 | (minor) << 16 | (patch))
#define FFX_FRAMEGENERATION_VERSION FFX_FRAMEGENERATION_MAKE_VERSION(4, 0, 1)

// Kits/FidelityFX/api/include/ffx_api_types.h
#define FFX_API_SURFACE_FORMAT_UNKNOWN 0
// Values from FfxApiSurfaceFormat (ffx_api_types.h). The runtime appears not to
// reject out-of-enum values - the B8G8R8A8 path ran with a wrong number here
// before this was checked - but the authoritative values are what it documents.
#define FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT 4
#define FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM 14
#define FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM 17
#define FFX_API_SURFACE_FORMAT_R16G16_FLOAT 18
#define FFX_API_SURFACE_FORMAT_R32_FLOAT 28

// ffx_api_types.h enum FfxApiResourceState - BIT FLAGS, not ordinals. The
// first transcription used the FFX 2.x ordinal values (1 / 8 / 12); the v4 API
// reads them as bits and its barrier translation maps the wrong bits to the
// wrong D3D12 states - on the real driver that is an invalid barrier and a
// device removal (0x887A0001), while WARP tolerates it, which is why the
// offline smoke never caught it.
#define FFX_API_RESOURCE_STATE_COMMON (1 << 0)
#define FFX_API_RESOURCE_STATE_UNORDERED_ACCESS (1 << 1)
#define FFX_API_RESOURCE_STATE_COMPUTE_READ (1 << 2)
#define FFX_API_RESOURCE_STATE_PIXEL_READ (1 << 3)
#define FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ \
    (FFX_API_RESOURCE_STATE_PIXEL_READ | FFX_API_RESOURCE_STATE_COMPUTE_READ)
#define FFX_API_RESOURCE_STATE_COPY_SRC (1 << 4)
#define FFX_API_RESOURCE_STATE_COPY_DEST (1 << 5)
#define FFX_API_RESOURCE_STATE_PRESENT (1 << 7)
#define FFX_API_RESOURCE_STATE_RENDER_TARGET (1 << 8)
#define FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT (1 << 9)

// ffx_api_types.h enum FfxApiResourceUsage - also bit flags (UAV is 1<<1, not
// 1<<2; DEPTHTARGET is 1<<2, not 1<<0).
#define FFX_API_RESOURCE_USAGE_READ_ONLY 0
#define FFX_API_RESOURCE_USAGE_RENDERTARGET (1 << 0)
#define FFX_API_RESOURCE_USAGE_UAV (1 << 1)
#define FFX_API_RESOURCE_USAGE_DEPTHTARGET (1 << 2)

// ffx_api_types.h enum FfxApiResourceType.
#define FFX_API_RESOURCE_TYPE_BUFFER 0
#define FFX_API_RESOURCE_TYPE_TEXTURE1D 1
#define FFX_API_RESOURCE_TYPE_TEXTURE2D 2

#define FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB 1
#define FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ 2
#define FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB 3

// ffx_api.h defines these as MASK COMBINATIONS, not shifts: an effect id lives
// in bits 16-23 and a backend id in bits 24-31, so a sub-id is OR'd into the
// high half with everything else masked out. Transcribing them as shifts (which
// is the obvious mistake) yields descriptor types the runtime does not know,
// and CreateContext's failure then says nothing about which value was wrong.
#define FFX_API_EFFECT_MASK 0x00ff0000u
#define FFX_API_BACKEND_MASK 0xff000000u
#define FFX_API_MAKE_EFFECT_SUB_ID(effectId, subversion) \
    (((effectId) & FFX_API_EFFECT_MASK) | ((subversion) & ~FFX_API_EFFECT_MASK))
#define FFX_API_BACKEND_ID_DX12 0x00000000u
#define FFX_API_MAKE_BACKEND_SUB_ID(backendId, subversion) \
    (((backendId) & FFX_API_BACKEND_MASK) | ((subversion) & ~FFX_API_BACKEND_MASK))

// ffx_api.h: the effect id already carries its mask bits (0x0002 in bits 16-23).
#define FFX_API_EFFECT_ID_FRAMEGENERATION 0x00020000u
// ffx_api_dx12.h: the DX12 backend descriptor.
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 \
    FFX_API_MAKE_BACKEND_SUB_ID(FFX_API_BACKEND_ID_DX12, 0x02)
// ffx_framegeneration.h
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_FRAMEGENERATION, 0x01)
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_FRAMEGENERATION, 0x0e)
#define FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2 \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_FRAMEGENERATION, 0x0c)
#define FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_FRAMEGENERATION, 0x03)
// ffx_framegeneration.h: per-frame configuration - this is where frame
// generation is SWITCHED ON. The default state is disabled, and a context that
// never sees this descriptor dispatches nothing but passthrough.
#define FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_FRAMEGENERATION, 0x02)

// ffx_framegeneration.h FfxApiFrameGenerationCreateFlags / DispatchFlags.
// ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS tells the runtime to derive its own
// motion at display resolution - the mode that works with the zero vectors a
// video pipeline hands over. NO_SWAPCHAIN_CONTEXT_NOTIFY declares there is no
// swapchain to hook, which is the player's situation everywhere.
#define FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS (1 << 1)
#define FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING                    (1 << 6)
#define FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY         (1 << 3)

// FfxApiDimensions2D / FfxApiRect2D are shared across effects in
// ffx_api_types.h, hence the Ffx prefix here.
// FfxApiDimensions2D / FfxApiRect2D are shared across effects in
// ffx_api_types.h. Rect field NAMES in the SDK are left/top/width/height; the
// layout is four int32 either way.
struct FfxApiDimensions2D {
    uint32_t width;
    uint32_t height;
};
struct FfxApiRect2D {
    int32_t left;
    int32_t top;
    int32_t width;
    int32_t height;
};
// ffx_api_types.h struct FfxApiResourceDescription - the v4 layout is FLAT:
// eight uint32s. The first transcription used a nested description borrowed
// from the FFX 2.x ffx_resource API (dimension/mipCount/mipArraySize/...), 28
// bytes instead of 32. Every struct embedding FfxApiResource then shifted by 8
// bytes past its first resource member, so the runtime read a 1x1 texture with
// a garbage type and format where the real resource was declared, and garbage
// flags/frameID everywhere else. WARP ran it anyway; the real AMD driver
// removed the device on the first dispatch.
struct FfxApiResourceDescription {
    uint32_t type;    // FfxApiResourceType
    uint32_t format;  // FfxApiSurfaceFormat
    union {
        uint32_t width;
        uint32_t size;
    };
    union {
        uint32_t height;
        uint32_t stride;
    };
    union {
        uint32_t depth;
        uint32_t alignment;
    };
    uint32_t mipCount;
    uint32_t flags;
    uint32_t usage;
};
struct FfxApiResource {
    void* resource;
    FfxApiResourceDescription description;
    uint32_t state;
};

// ffx_api.h: the base descriptor every effect descriptor starts with. Two
// fields only - there is no apiVersion member; the version a context is created
// against travels in the FRAMEGENERATION_VERSION descriptor below instead.
struct ffxApiHeader {
    uint64_t type;
    struct ffxApiHeader* pNext;
};
using ffxCreateContextDescHeader = ffxApiHeader;
using ffxDispatchDescHeader = ffxApiHeader;

// dx12/ffx_api_dx12.h struct ffxCreateBackendDX12Desc - the v4 struct carries
// ONLY the header and the device. Allocation callbacks and diagnostics moved
// to their own descriptors; the extra fields the first transcription carried
// were dead weight the runtime never read (it walks the chain via pNext), but
// keeping them invited exactly the drift this file exists to prevent.
struct ffxCreateBackendDX12Desc {
    ffxCreateContextDescHeader header;
    ID3D12Device* device;
};

struct ffxCreateContextDescFrameGenerationVersion {
    ffxCreateContextDescHeader header;
    uint32_t version;
};
struct ffxCreateContextDescFrameGeneration {
    ffxCreateContextDescHeader header;
    uint32_t flags;
    FfxApiDimensions2D displaySize;
    FfxApiDimensions2D maxRenderSize;
    uint32_t backBufferFormat;
};

struct ffxDispatchDescFrameGenerationPrepareV2 {
    ffxDispatchDescHeader header;
    uint64_t frameID;
    uint32_t flags;
    void* commandList;
    FfxApiDimensions2D renderSize;
    float jitterOffset[2];
    float motionVectorScale[2];
    float frameTimeDelta;
    bool reset;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
    float viewSpaceToMetersFactor;
    FfxApiResource depth;
    FfxApiResource motionVectors;
    // ffx_framegeneration.h: the V2 prepare folded the deprecated
    // PrepareCameraInfo descriptor into these four trailing vectors; the first
    // transcription omitted cameraPosition. The vectors sit at the end so the
    // omission shifted nothing, but the runtime reads them all.
    float cameraPosition[3];
    float cameraUp[3];
    float cameraRight[3];
    float cameraForward[3];
};

struct ffxDispatchDescFrameGeneration {
    ffxDispatchDescHeader header;
    void* commandList;
    FfxApiResource presentColor;
    FfxApiResource outputs[4];
    uint32_t numGeneratedFrames;
    bool reset;
    uint32_t backbufferTransferFunction;
    float minMaxLuminance[2];
    FfxApiRect2D generationRect;
    uint64_t frameID;
};

// ffx_framegeneration.h, struct ffxConfigureDescFrameGeneration - fields in the
// SDK's order; the two bools before the pointer-bearing FfxApiResource let the
// compiler pad exactly as it pads the SDK's own definition (same compiler, same
// rules). Skipped fields (presentCallback, HUDLessColor, ...) stay zero.
struct ffxConfigureDescFrameGeneration {
    ffxDispatchDescHeader header;
    void* swapChain;
    void* presentCallback;
    void* presentCallbackUserContext;
    void* frameGenerationCallback;
    void* frameGenerationCallbackUserContext;
    bool frameGenerationEnabled;
    bool allowAsyncWorkloads;
    FfxApiResource HUDLessColor;
    uint32_t flags;
    bool onlyPresentGenerated;
    FfxApiRect2D generationRect;
    uint64_t frameID;
};

// ffx_api.h declares ALL five entry points taking `ffxContext* context` - the
// SLOT, not the handle. Create writes the handle into the slot; Configure,
// Dispatch, Query and Destroy read the handle back out of it. Passing the
// handle by value instead made the runtime dereference it as a pointer to a
// pointer: the first release crashed inside ffxDestroyContext (+0x10b9b2,
// both playback minidumps), the BGRA8 build crashed inside ffxConfigure
// reading 0xFFFFFFFFFFFFFFFF. MPCVR-FSR-FrameGen, the working reference,
// passes &m_context in every call.
using PfnFfxCreateContext = ffxReturnCode_t(FFX_API*)(ffxContextSlot* context,
                                                        ffxCreateContextDescHeader* desc,
                                                        const void* memCb);
using PfnFfxDestroyContext = ffxReturnCode_t(FFX_API*)(ffxContextSlot* context,
                                                        const void* memCb);
using PfnFfxDispatch = ffxReturnCode_t(FFX_API*)(ffxContextSlot* context,
                                                 const ffxDispatchDescHeader* desc);
using PfnFfxConfigure = ffxReturnCode_t(FFX_API*)(ffxContextSlot* context,
                                                  const ffxDispatchDescHeader* desc);

// The API version this declaration set was taken from; CreateContext refuses
// anything it does not recognise, so a newer DLL fails loudly here instead of
// misreading a descriptor.
constexpr uint32_t kApiVersion = FFX_FRAMEGENERATION_VERSION;

// Fills a resource descriptor by asking D3D12, mirroring ffxApiGetResourceDX12.
// The SDK's version also guards against an unknown dimension; this one reports
// the caller's state as given, which is all this backend needs because it always
// states a resource it just transitioned itself.
FfxApiResource GetResourceDX12(ID3D12Resource* resource, uint32_t state, uint32_t usages = 0);

} // namespace fsr_fg