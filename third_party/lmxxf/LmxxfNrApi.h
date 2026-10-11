#pragma once

/*
 * Vendored from TheAutomatic/dlss-5-amd-project, tag v1.10.5-alpha,
 * OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h.
 *
 * This is the LmxxfNrRuntime C ABI that the staged -gai runtime speaks: ABI 3,
 * a 176-byte table (8-byte header + 21 function pointers). It supersedes the
 * upstream lmxxf/dlss5-on-amd-9070xt-porting revision a9b7a521, which stops at
 * ABI 1 / 136 bytes / 16 pointers and cannot be negotiated with the staged DLL.
 *
 * The v1..v3 table is strictly append-only, so the ABI 1 prefix keeps its
 * meaning; only the size and the appended tail change. Keep this file
 * byte-compatible with the runtime build: any change to the table or to the
 * struct sizes requires a new upstream ABI revision and a new lock identity.
 *
 * MSVC host and MinGW runtime must not share a C++ ABI. No STL, exceptions, or
 * CRT-allocated objects cross this boundary. x64 stdcall is the Windows default.
 */

#include <stdint.h>
#include "NrPerformance.h" /* LmxxfNrTimingApi, for LmxxfNrGetTimingApi below */
#include <stddef.h>        /* wchar_t in C hosts */

#ifdef __cplusplus
extern "C" {
#endif

#define LMXXF_NR_ABI_VERSION 3u

/* Legacy table sizes, kept for negotiation sweeps and for reading a runtime's
 * own contract out of its rejection codes. Neither is the compiled-in size. */
#define LMXXF_NR_API_V1_SIZE 136u /* 8 + 16*8: upstream a9b7a521 */
#define LMXXF_NR_API_V2_SIZE 144u /* 8 + 17*8: v1 + GetTimings */
#define LMXXF_NR_API_V3_SIZE 176u /* 8 + 21*8: this header */

/* Optional recovery when HIP enqueue or the session queue contract fails.
 * On recovery, EnqueueHip returns OK only after the private neural output was fully zeroed;
 * GetLastError then contains a recovery diagnostic. Submit input and output work
 * on the queue passed to EnqueueHip, and submit the output reader before Retire,
 * Drain, or Destroy. The runtime waits for that supplied queue before output reuse
 * or destruction. It cannot discover output readers on other queues; callers must
 * synchronize those queues themselves before reuse or destruction. A failed or
 * uncertain clear returns FAILED and the session must be rebuilt. */
#define LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK (1u << 0)
/* v2: stable recording jobs. The caller serializes recording/execution calls across sessions and keeps
 * each job until recording invalidation AND CollectRecording confirms completion.
 * This mode does not support the blocking legacy zero-output recovery path. */
#define LMXXF_NR_CREATE_FLAG_RECORDING_LEASES (1u << 1)
#define LMXXF_NR_SUBMITTED_PRODUCER (1u << 0)
#define LMXXF_NR_SUBMITTED_CONSUMER (1u << 1)

enum LmxxfNrStatus
{
    LMXXF_NR_OK = 0,
    LMXXF_NR_UNSUPPORTED_ABI = 1,
    LMXXF_NR_INVALID_ARGUMENT = 2,
    LMXXF_NR_NOT_IMPLEMENTED = 3,
    LMXXF_NR_UNAVAILABLE = 4,
    LMXXF_NR_FAILED = 5,
    LMXXF_NR_DEVICE_LOST = 6 /* Collect released an invalidated job after confirmed device removal */
};

enum LmxxfNrJobState
{
    LMXXF_NR_JOB_NONE = 0,
    LMXXF_NR_JOB_PREPARED = 1,           /* Set by PrepareFrame; ready for RecordInputs */
    LMXXF_NR_JOB_PRODUCER_SUBMITTED = 2, /* Set by RecordInputs; producer recorded/submitted */
    LMXXF_NR_JOB_NR_ENQUEUED = 3,        /* EnqueueHip scheduled on queue */
    LMXXF_NR_JOB_NR_COMPLETE = 4,        /* EnqueueHip executed or completed */
    LMXXF_NR_JOB_CONSUMER_COMPLETE = 5,  /* Set by RecordOutputs; consumer recorded */
    LMXXF_NR_JOB_RETIRED = 6             /* Set by Retire or CancelUnsubmitted */
};

/* 32 bytes in ABI 3. The ABI 1 hip_ready field is gone: readiness is read from
 * GetStatus(context) after Create, and it was never meaningful before Create. */
typedef struct LmxxfNrCapabilities
{
    uint32_t struct_size;
    uint32_t abi_version;
    /* The always-admitted box. Admission is by pixel budget, so a wider input (up to 2560, height
     * still within max_input_height) is also accepted while width*height stays within
     * max_input_width*max_input_height (ultrawide). */
    uint32_t max_input_width;
    uint32_t max_input_height;
    uint32_t history_supported; /* native final-pass history; query session for active/fallback status */
    uint32_t overlap_supported; /* first product version: 0 */
    uint32_t graph_supported;   /* first product version: 0; EnqueueHip must not graph-wait */
    /* [DEPRECATED] 1 = legacy single-target indicator; does not reflect active runtime GPU arch.
     * Query session via GetStatus(context) for active architecture. */
    uint32_t gfx1201_target;
} LmxxfNrCapabilities;

typedef struct LmxxfNrCreateInfo
{
    uint32_t struct_size;
    void *device; /* ID3D12Device*; not dereferenced until HIP is wired */
    void *queue;  /* ID3D12CommandQueue*; must match device when HIP is wired */
    const wchar_t *assets_directory;
    uint32_t flags; /* LMXXF_NR_CREATE_FLAG_*; unknown bits are rejected */
} LmxxfNrCreateInfo;

#define LMXXF_NR_FRAME_FLAG_STRENGTH          (1u << 0)
#define LMXXF_NR_FRAME_FLAG_DEBUG_VIEW        (1u << 1)
#define LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH (1u << 2)
/* No usable exposure texture in this frame: the runtime meters the colour input itself (mean
 * luminance, log-domain smoothing) and binds the result as the codec exposure. Ignored when a
 * usable exposure is supplied; pre_exposure and exposure_scale are then not applied. */
#define LMXXF_NR_FRAME_FLAG_AUTO_EXPOSURE     (1u << 3)
/* Diagnostic: normal RGB conversion, shared-memory fences and HIP copies, no per-frame
 * network inference. Mutually exclusive with CODEC_PASSTHROUGH. Restart the host to change. */
#define LMXXF_NR_FRAME_FLAG_HIP_PASSTHROUGH   (1u << 4)

#define LMXXF_NR_TEMPORAL_MODEL_HISTORY (1u << 0)
#define LMXXF_NR_TEMPORAL_INPUTS_VALID (1u << 1)
#define LMXXF_NR_TEMPORAL_RESET (1u << 2)
#define LMXXF_NR_TEMPORAL_MV_JITTERED (1u << 3)
#define LMXXF_NR_TEMPORAL_DEPTH_INVERTED (1u << 4)

/* 160 bytes in ABI 3. The ABI 1 frame info stopped at model_scale (80 bytes); the
 * exposure block grew the struct to 104 and everything from paper_white on is new. */
typedef struct LmxxfNrFrameInfo
{
    uint32_t struct_size;
    uint64_t session_id;
    uint64_t frame_id;
    uint64_t list_generation;
    void *command_list; /* ID3D12GraphicsCommandList*; Record* do not Execute */
    uint32_t color_width;
    uint32_t color_height;
    void *color; /* ID3D12Resource*; required for RecordInputs */
    uint32_t color_state; /* D3D12_RESOURCE_STATES at RecordInputs */
    uint32_t flags; /* LMXXF_NR_FRAME_FLAG_* */
    float transfer_strength; /* Detail strength: 0..3, default 1. Above 1 extrapolates past the network result. */
    float color_strength;    /* Colour strength: 0..3, default 1. 0 keeps hue. Above 1 extrapolates. */
    uint32_t debug_view;     /* 0=normal, 1=proxy, 2=neural solo, 3=diff 20x, 4=tint */
    float model_scale;       /* 0.25..1.0, default 1.0 */
    /* Optional texture; all fields belong to the current package ABI. */
    void *exposure;    /* ID3D12Resource* 1x1 R16_FLOAT/R32_FLOAT, shader-readable; NULL = none */
    uint32_t exposure_state; /* D3D12_RESOURCE_STATES of exposure at RecordInputs */
    float pre_exposure;   /* game pre-exposure; finite and > 0, default 1 */
    float exposure_scale; /* exposure scale; finite and > 0, default 1 */
    /* Occupies the tail padding of the 104-byte exposure struct. Leave 0. */
    uint32_t reserved_after_exposure;
    /* Codec paper white passed to encode and decode Record. Finite and in (0, 64], default 1.
     * Not the HDR Paper White anchor. Required in the current package ABI. */
    float paper_white;
    uint32_t temporal_flags;
    void *motion, *depth;
    uint32_t motion_state, depth_state, motion_width, motion_height;
    float motion_scale_x, motion_scale_y, jitter_x, jitter_y;
} LmxxfNrFrameInfo;

/* struct_size must be sizeof(LmxxfNrJob) or PrepareFrame rejects the call.
 *
 * The job argument changes meaning after PrepareFrame, and the header cannot
 * express it - it is the one behavioural difference that survives a clean ABI
 * negotiation and still fails at run time.
 *
 *   PrepareFrame(context, info, &job)   takes the struct address; the runtime
 *                                       writes handle and private_output back
 *   everything else                     takes job.handle
 *
 * The runtime resolves the later calls through its job registry, which in the
 * default non-recording mode accepts only NULL or its own handle. Passing &job
 * again returns INVALID_ARGUMENT with "need job and command list" while both
 * pointers are demonstrably non-null. */
typedef struct LmxxfNrJob
{
    uint32_t struct_size;
    void *handle;
    void *private_output; /* ID3D12Resource* for SR; null until PrepareFrame succeeds */
} LmxxfNrJob;

/* Upstream fe4d1d73 timing payload. Non-blocking, latest completed frame.
 * First call enables measurement; serialize with frame calls. */
typedef struct LmxxfNrTimings
{
    uint32_t struct_size, valid;
    float network_ms;
    uint32_t reserved;
    uint64_t frame_id;
} LmxxfNrTimings;

typedef struct LmxxfNrApi
{
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t (*QueryCapabilities)(LmxxfNrCapabilities *out);
    int32_t (*Create)(const LmxxfNrCreateInfo *info, void **context);
    int32_t (*Destroy)(void *context);
    int32_t (*PrepareSession)(void *context);
    int32_t (*PrepareFrame)(void *context, const LmxxfNrFrameInfo *info, LmxxfNrJob *job);
    int32_t (*RecordInputs)(void *context, void *job, void *command_list);
    int32_t (*EnqueueHip)(void *context, void *job, void *command_queue);
    int32_t (*RecordOutputs)(void *context, void *job, void *command_list);
    int32_t (*ExecuteAfterProducer)(void *context, void *job, void *command_queue);
    int32_t (*CancelUnsubmitted)(void *context, void *job);
    int32_t (*Poll)(void *context, void *job, uint32_t *state);
    int32_t (*Retire)(void *context, void *job);
    int32_t (*ResetHistory)(void *context);
    int32_t (*Drain)(void *context);
    int32_t (*GetStatus)(void *context, char *buf, uint32_t buf_chars);
    int32_t (*GetLastError)(char *buf, uint32_t buf_chars);
    /* ABI v2 append-only extension. Begin runs BEFORE producer submission; End
     * runs exactly once afterward, including failure/no-submission outcomes.
     * fence is borrowed by End (the runtime AddRefs it); signal_status is the
     * actual tail Signal HRESULT, not the HIP result or a guessed completion. */
    int32_t (*BeginRecordingExecution)(void *context, void *job, void *actual_queue);
    int32_t (*EndRecordingExecution)(void *context, void *job, void *actual_queue,
                                    uint32_t submitted_flags, void *fence, uint64_t value,
                                    int32_t signal_status);
    int32_t (*InvalidateRecording)(void *context, void *job);
    /* UNAVAILABLE means retain and retry; OK or DEVICE_LOST consumes the handle.
     * Unknown/stale handles return INVALID_ARGUMENT without dereferencing them. */
    int32_t (*CollectRecording)(void *context, void *job);
    /* Product v2 has recording extensions at upstream's append offset.
     * Timing is appended after them; never reinterpret the upstream v1 table. */
    int32_t (*GetTimings)(void *context, LmxxfNrTimings *out);
} LmxxfNrApi;

/* The sizes the runtime actually negotiates are contract, not commentary: this
 * file is only valid while they hold. */
#if defined(__cplusplus)
static_assert(sizeof(LmxxfNrApi) == LMXXF_NR_API_V3_SIZE,
              "ABI 3 table must be 8 + 21*8 = 176 bytes; the runtime rejects any other size");
static_assert(sizeof(LmxxfNrCapabilities) == 32u, "ABI 3 capabilities struct is 8 uint32 fields");
#endif

#ifdef _WIN32
#ifdef LMXXF_NR_RUNTIME_EXPORTS
#define LMXXF_NR_EXPORT __declspec(dllexport)
#else
#define LMXXF_NR_EXPORT __declspec(dllimport)
#endif
#else
#define LMXXF_NR_EXPORT
#endif

/* Sole export. Caller sets out->struct_size = sizeof(LmxxfNrApi) before the call. */
LMXXF_NR_EXPORT int32_t LmxxfNrGetApi(uint32_t abi_version, LmxxfNrApi *out);
/* Optional telemetry extension; its absence does not invalidate the rendering ABI. */
LMXXF_NR_EXPORT int32_t LmxxfNrGetTimingApi(uint32_t version, LmxxfNrTimingApi *out);

#ifdef __cplusplus
}
#endif
