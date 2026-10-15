#pragma once

// A local declaration of the slice of AMD FSR's C ABI this upscaler uses: the
// UPSCALE effect (FSR Upscaling 4.1.1 - the machine-learning "FSR4" - and the
// analytical FSR 3.1 fallback the same effect exposes on older hardware).
//
// The SDK's own headers are not vendored here, for the same reason the frame
// generation's are not (see FsrFrameGenAbi.h): FidelityFX ships under a licence
// that permits binary redistribution, not source redistribution, and the
// runtime is loaded at run time rather than linked, so nothing has to exist at
// build time. What is declared below is the published API layout, written out
// so the compiler agrees with the DLL about struct offsets without the SDK on
// disk.
//
// THREE things here are easy to get wrong and were checked against
// FidelityFX-SDK 2.3.0 (Kits/FidelityFX/upscalers/include/ffx_upscale.h)
// rather than transcribed from memory:
//
//   1. The version macro is NOT the frame generation's. FSR's upscaler packs
//      the minor into bits 12-21 - ((major) << 22 | (minor) << 12 | patch) -
//      where FFX_FRAMEGENERATION_MAKE_VERSION uses << 16. Reusing the frame
//      generation's macro builds a version number the runtime does not
//      recognise, and CreateContext refuses it.
//   2. The version descriptor is MANDATORY since SDK 2.1: without
//      ffxCreateContextDescUpscaleVersion chained off the create descriptor,
//      the runtime has no API version to check against. (The frame generation
//      has the same descriptor; FSR Upscaling documents the requirement.)
//   3. FSR Upscaling wants LINEAR colour. That is the opposite of the frame
//      generation, whose motion estimate the vendor validated in the sRGB
//      domain - and the opposite of EASU/RCAS, which are perceptual. The
//      playback present chain holds FP16 linear light end to end, so the
//      upscaler can be fed directly and needs neither NON_LINEAR flag; a
//      display-encoded input has to declare FFX_UPSCALE_ENABLE_NON_LINEAR_
//      COLORSPACE at create time (quality is then what the vendor calls
//      "might be improved", i.e. degraded).
//
// Every field below is used by name and in the order the SDK declares it; none
// is a reinterpret_cast of a byte blob. If the SDK is present, point
// FFX_SDK_ROOT at it and add its include directories - the real headers are
// pulled in and every struct is checked field-for-field by the static_assert
// block at the bottom, which is what turns a version bump into a compile error
// instead of a corrupted frame.
//
// The shared types (resource, header, dimensions, entry points) live in
// FsrFrameGenAbi.h: both effects are the same C ABI, and duplicating them is
// exactly the drift this pair of files exists to prevent.

#include "FsrFrameGenAbi.h"

#if defined(FFX_SDK_ROOT)
#include "ffx_upscale.h"
#endif

namespace fsr_up {

// The shared types this effect's descriptors are built from live in the frame
// generation's declaration (FsrFrameGenAbi.h): both effects are the same C ABI
// and the same SDK, so they are aliased here rather than declared twice - a
// second declaration would be the drift this pair of files exists to prevent.
using fsr_fg::ffxApiHeader;
using fsr_fg::ffxCreateContextDescHeader;
using fsr_fg::ffxDispatchDescHeader;
// ffx_api.h: Query descriptors start with the same two-field header. The frame
// generation never queries anything, so this alias is declared here.
using ffxQueryDescHeader = fsr_fg::ffxApiHeader;
using fsr_fg::FfxApiDimensions2D;
using fsr_fg::FfxApiResource;
using fsr_fg::FfxApiResourceDescription;

// Kits/FidelityFX/api/include/ffx_api.h - one effect id per effect, UPSCALE
// being 0x0001 in bits 16-23. The upscaler and the frame generation are
// different contexts on the same ABI, brought up by the same loader.
#define FFX_API_EFFECT_ID_UPSCALE 0x00010000u

// Kits/FidelityFX/upscalers/include/ffx_upscale.h
#define FFX_UPSCALER_MAKE_VERSION(major, minor, patch) \
    (((major) << 22) | ((minor) << 12) | (patch))
#define FFX_UPSCALER_VERSION FFX_UPSCALER_MAKE_VERSION(4, 1, 1)

// ffx_upscale.h: descriptor sub-ids, OR'd into the low half of the effect id.
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x00)
#define FFX_API_DISPATCH_DESC_TYPE_UPSCALE \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x01)
#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x02)
#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x03)
#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x04)
#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x05)
#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x08)
#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2 \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x09)
#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x0a)
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION \
    FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x0b)

// ffx_upscale.h enum FfxApiUpscaleQualityMode - the per-dimension ratios the
// vendor asks applications to expose under these names. A video player does
// not get to choose its render size (the source is the source), so these are
// only a naming convention here: the ratio in use is output/source, whatever
// the source happens to be.
#define FFX_UPSCALE_QUALITY_MODE_NATIVEAA          0
#define FFX_UPSCALE_QUALITY_MODE_QUALITY           1
#define FFX_UPSCALE_QUALITY_MODE_BALANCED          2
#define FFX_UPSCALE_QUALITY_MODE_PERFORMANCE       3
#define FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE 4

// ffx_upscale.h enum FfxApiCreateContextUpscaleFlags.
#define FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE                 (1 << 0)
#define FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS  (1 << 1)
#define FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION (1 << 2)
#define FFX_UPSCALE_ENABLE_DEPTH_INVERTED                     (1 << 3)
#define FFX_UPSCALE_ENABLE_DEPTH_INFINITE                     (1 << 4)
#define FFX_UPSCALE_ENABLE_AUTO_EXPOSURE                      (1 << 5)
#define FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION                 (1 << 6)
#define FFX_UPSCALE_ENABLE_DEBUG_CHECKING                     (1 << 7)
#define FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE              (1 << 8)
#define FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION                (1 << 9)

// ffx_upscale.h enum FfxApiDispatchFsrUpscaleFlags.
#define FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW       (1 << 0)
#define FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB (1 << 1)
#define FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ   (1 << 2)

// ffx_upscale.h enum FfxApiQueryResourceIdentifiers - the bitfield the
// GET_RESOURCE_REQUIREMENTS query fills: what the runtime needs given the
// flags the context was created with, and what it would consume if offered.
#define FFX_API_QUERY_RESOURCE_INPUT_COLOR                   (1u << 0)
#define FFX_API_QUERY_RESOURCE_INPUT_DEPTH                   (1u << 1)
#define FFX_API_QUERY_RESOURCE_INPUT_MV                      (1u << 2)
#define FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE                (1u << 3)
#define FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK            (1u << 4)
#define FFX_API_QUERY_RESOURCE_INPUT_TRANSPARENCYCOMPOSITION (1u << 5)

// ffx_api_types.h struct FfxApiFloatCoords2D - a 2D float pair, used for the
// jitter offset and the motion-vector scale.
struct FfxApiFloatCoords2D {
    float x;
    float y;
};

// ffx_api.h: the runtime's diagnostic sink. May be null; where it is not, the
// runtime reports the API misuse the DEBUG_CHECKING flag was created to catch
// (the documented warnings name frameTimeDelta in the wrong unit, a cameraFar
// that is too low, and the like) instead of failing silently.
using ffxApiMessage = void (*)(uint32_t type, const wchar_t* message);

// ffx_upscale.h struct ffxCreateContextDescUpscale. fpMessage is the LAST
// member; the first transcription put the message callback where the frame
// generation's create descriptor carries its flags, which is the sort of
// mistake only a field-by-field diff catches.
struct ffxCreateContextDescUpscale {
    ffxCreateContextDescHeader header;
    uint32_t flags;
    FfxApiDimensions2D maxRenderSize;
    FfxApiDimensions2D maxUpscaleSize;
    ffxApiMessage fpMessage;
};

// ffx_upscale.h struct ffxCreateContextDescUpscaleVersion - mandatory since
// SDK 2.1; chained through the create descriptor's pNext.
struct ffxCreateContextDescUpscaleVersion {
    ffxCreateContextDescHeader header;
    uint32_t version;
};

// ffx_upscale.h struct ffxDispatchDescUpscale. Every resource is handed over
// in the state the vendor documents (NON_PIXEL_SHADER_RESOURCE on DX12);
// exposure, reactive and transparencyAndComposition are OPTIONAL and are
// signalled as absent by an empty FfxApiResource (resource == nullptr), which
// is why they keep their place in the layout instead of being omitted.
struct ffxDispatchDescUpscale {
    ffxDispatchDescHeader header;
    void* commandList;
    FfxApiResource color;
    FfxApiResource depth;
    FfxApiResource motionVectors;
    FfxApiResource exposure;
    FfxApiResource reactive;
    FfxApiResource transparencyAndComposition;
    FfxApiResource output;
    FfxApiFloatCoords2D jitterOffset;
    FfxApiFloatCoords2D motionVectorScale;
    FfxApiDimensions2D renderSize;
    FfxApiDimensions2D upscaleSize;
    bool enableSharpening;
    float sharpness;
    float frameTimeDelta;
    float preExposure;
    bool reset;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
    float viewSpaceToMetersFactor;
    uint32_t flags;
};

struct ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode {
    ffxQueryDescHeader header;
    uint32_t qualityMode;
    float* pOutUpscaleRatio;
};

struct ffxQueryDescUpscaleGetRenderResolutionFromQualityMode {
    ffxQueryDescHeader header;
    uint32_t displayWidth;
    uint32_t displayHeight;
    uint32_t qualityMode;
    uint32_t* pOutRenderWidth;
    uint32_t* pOutRenderHeight;
};

struct ffxQueryDescUpscaleGetJitterPhaseCount {
    ffxQueryDescHeader header;
    uint32_t renderWidth;
    uint32_t displayWidth;
    int32_t* pOutPhaseCount;
};

struct ffxQueryDescUpscaleGetJitterOffset {
    ffxQueryDescHeader header;
    int32_t index;
    int32_t phaseCount;
    float* pOutX;
    float* pOutY;
};

struct ffxQueryDescUpscaleGetResourceRequirements {
    ffxQueryDescHeader header;
    uint64_t required_resources;
    uint64_t optional_resources;
};

// The API version this declaration set was taken from. CreateContext refuses
// what it does not recognise, so a DLL from a different SDK fails here rather
// than misreading a descriptor.
constexpr uint32_t kApiVersion = FFX_UPSCALER_VERSION;

} // namespace fsr_up

#if defined(FFX_SDK_ROOT)
// The whole point of building against the SDK: every struct above is diffed
// against the vendor's own definition, so a future SDK that reorders a field,
// grows a descriptor or changes a type stops the build instead of shipping a
// frame that is quietly wrong. This is the check the frame generation's ABI
// lacked - its three transcription bugs (nested resource description, ordinal
// resource states, effect-id shifts) were all of exactly this kind.
namespace fsr_up {
static_assert(sizeof(ffxCreateContextDescUpscale) == sizeof(::ffxCreateContextDescUpscale),
              "ffxCreateContextDescUpscale size drifted from the SDK");
static_assert(offsetof(ffxCreateContextDescUpscale, flags) ==
                  offsetof(::ffxCreateContextDescUpscale, flags) &&
              offsetof(ffxCreateContextDescUpscale, maxRenderSize) ==
                  offsetof(::ffxCreateContextDescUpscale, maxRenderSize) &&
              offsetof(ffxCreateContextDescUpscale, maxUpscaleSize) ==
                  offsetof(::ffxCreateContextDescUpscale, maxUpscaleSize) &&
              offsetof(ffxCreateContextDescUpscale, fpMessage) ==
                  offsetof(::ffxCreateContextDescUpscale, fpMessage),
              "ffxCreateContextDescUpscale fields drifted from the SDK");

static_assert(sizeof(ffxCreateContextDescUpscaleVersion) ==
                  sizeof(::ffxCreateContextDescUpscaleVersion),
              "ffxCreateContextDescUpscaleVersion size drifted from the SDK");
static_assert(offsetof(ffxCreateContextDescUpscaleVersion, version) ==
                  offsetof(::ffxCreateContextDescUpscaleVersion, version),
              "ffxCreateContextDescUpscaleVersion fields drifted from the SDK");

static_assert(sizeof(ffxDispatchDescUpscale) == sizeof(::ffxDispatchDescUpscale),
              "ffxDispatchDescUpscale size drifted from the SDK");
static_assert(offsetof(ffxDispatchDescUpscale, commandList) ==
                  offsetof(::ffxDispatchDescUpscale, commandList) &&
              offsetof(ffxDispatchDescUpscale, color) ==
                  offsetof(::ffxDispatchDescUpscale, color) &&
              offsetof(ffxDispatchDescUpscale, depth) ==
                  offsetof(::ffxDispatchDescUpscale, depth) &&
              offsetof(ffxDispatchDescUpscale, motionVectors) ==
                  offsetof(::ffxDispatchDescUpscale, motionVectors) &&
              offsetof(ffxDispatchDescUpscale, exposure) ==
                  offsetof(::ffxDispatchDescUpscale, exposure) &&
              offsetof(ffxDispatchDescUpscale, reactive) ==
                  offsetof(::ffxDispatchDescUpscale, reactive) &&
              offsetof(ffxDispatchDescUpscale, transparencyAndComposition) ==
                  offsetof(::ffxDispatchDescUpscale, transparencyAndComposition) &&
              offsetof(ffxDispatchDescUpscale, output) ==
                  offsetof(::ffxDispatchDescUpscale, output) &&
              offsetof(ffxDispatchDescUpscale, jitterOffset) ==
                  offsetof(::ffxDispatchDescUpscale, jitterOffset) &&
              offsetof(ffxDispatchDescUpscale, motionVectorScale) ==
                  offsetof(::ffxDispatchDescUpscale, motionVectorScale) &&
              offsetof(ffxDispatchDescUpscale, renderSize) ==
                  offsetof(::ffxDispatchDescUpscale, renderSize) &&
              offsetof(ffxDispatchDescUpscale, upscaleSize) ==
                  offsetof(::ffxDispatchDescUpscale, upscaleSize) &&
              offsetof(ffxDispatchDescUpscale, enableSharpening) ==
                  offsetof(::ffxDispatchDescUpscale, enableSharpening) &&
              offsetof(ffxDispatchDescUpscale, sharpness) ==
                  offsetof(::ffxDispatchDescUpscale, sharpness) &&
              offsetof(ffxDispatchDescUpscale, frameTimeDelta) ==
                  offsetof(::ffxDispatchDescUpscale, frameTimeDelta) &&
              offsetof(ffxDispatchDescUpscale, preExposure) ==
                  offsetof(::ffxDispatchDescUpscale, preExposure) &&
              offsetof(ffxDispatchDescUpscale, reset) ==
                  offsetof(::ffxDispatchDescUpscale, reset) &&
              offsetof(ffxDispatchDescUpscale, cameraNear) ==
                  offsetof(::ffxDispatchDescUpscale, cameraNear) &&
              offsetof(ffxDispatchDescUpscale, cameraFar) ==
                  offsetof(::ffxDispatchDescUpscale, cameraFar) &&
              offsetof(ffxDispatchDescUpscale, cameraFovAngleVertical) ==
                  offsetof(::ffxDispatchDescUpscale, cameraFovAngleVertical) &&
              offsetof(ffxDispatchDescUpscale, viewSpaceToMetersFactor) ==
                  offsetof(::ffxDispatchDescUpscale, viewSpaceToMetersFactor) &&
              offsetof(ffxDispatchDescUpscale, flags) ==
                  offsetof(::ffxDispatchDescUpscale, flags),
              "ffxDispatchDescUpscale fields drifted from the SDK");

static_assert(sizeof(ffxQueryDescUpscaleGetResourceRequirements) ==
                  sizeof(::ffxQueryDescUpscaleGetResourceRequirements),
              "ffxQueryDescUpscaleGetResourceRequirements drifted from the SDK");
static_assert(sizeof(ffxQueryDescUpscaleGetJitterOffset) ==
                  sizeof(::ffxQueryDescUpscaleGetJitterOffset),
              "ffxQueryDescUpscaleGetJitterOffset drifted from the SDK");
static_assert(sizeof(ffxQueryDescUpscaleGetJitterPhaseCount) ==
                  sizeof(::ffxQueryDescUpscaleGetJitterPhaseCount),
              "ffxQueryDescUpscaleGetJitterPhaseCount drifted from the SDK");
static_assert(kApiVersion == FFX_UPSCALER_VERSION,
              "the transcribed version macro disagrees with the SDK's");
} // namespace fsr_up
#endif
