#pragma once

/*
 * Vendored from dlss5-on-amd-9070xt-porting commit
 * a9b7a52165800cc4f8f98e1d5497638196413ffd.
 *
 * This is the standalone LmxxfNrRuntime C ABI. Keep this file byte-compatible
 * with the runtime build; changes to the table or struct sizes require a new
 * upstream ABI revision and a new lock identity here.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LMXXF_NR_ABI_VERSION 1u
#define LMXXF_NR_FRAME_INFO_V1_SIZE 80u
#define LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK (1u << 0)

enum LmxxfNrStatus
{
    LMXXF_NR_OK = 0,
    LMXXF_NR_UNSUPPORTED_ABI = 1,
    LMXXF_NR_INVALID_ARGUMENT = 2,
    LMXXF_NR_NOT_IMPLEMENTED = 3,
    LMXXF_NR_UNAVAILABLE = 4,
    LMXXF_NR_FAILED = 5
};

enum LmxxfNrJobState
{
    LMXXF_NR_JOB_NONE = 0,
    LMXXF_NR_JOB_PREPARED = 1,
    LMXXF_NR_JOB_PRODUCER_SUBMITTED = 2,
    LMXXF_NR_JOB_NR_ENQUEUED = 3,
    LMXXF_NR_JOB_NR_COMPLETE = 4,
    LMXXF_NR_JOB_CONSUMER_COMPLETE = 5,
    LMXXF_NR_JOB_RETIRED = 6
};

typedef struct LmxxfNrCapabilities
{
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t max_input_width;
    uint32_t max_input_height;
    uint32_t history_supported;
    uint32_t overlap_supported;
    uint32_t graph_supported;
    uint32_t hip_ready;
    uint32_t gfx1201_target;
} LmxxfNrCapabilities;

typedef struct LmxxfNrCreateInfo
{
    uint32_t struct_size;
    void *device;
    void *queue;
    const wchar_t *assets_directory;
    uint32_t flags;
} LmxxfNrCreateInfo;

#define LMXXF_NR_FRAME_FLAG_STRENGTH (1u << 0)
#define LMXXF_NR_FRAME_FLAG_DEBUG_VIEW (1u << 1)
#define LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH (1u << 2)

typedef struct LmxxfNrFrameInfo
{
    uint32_t struct_size;
    uint64_t session_id;
    uint64_t frame_id;
    uint64_t list_generation;
    void *command_list;
    uint32_t color_width;
    uint32_t color_height;
    void *color;
    uint32_t color_state;
    uint32_t flags;
    float transfer_strength;
    float color_strength;
    uint32_t debug_view;
    float model_scale;
    void *exposure;
    uint32_t exposure_state;
    float pre_exposure;
    float exposure_scale;
} LmxxfNrFrameInfo;

typedef struct LmxxfNrJob
{
    uint32_t struct_size;
    void *handle;
    void *private_output;
} LmxxfNrJob;

typedef struct LmxxfNrTimings
{
    uint32_t struct_size;
    uint32_t valid;
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
    int32_t (*GetTimings)(void *context, LmxxfNrTimings *out);
} LmxxfNrApi;

#define LMXXF_NR_API_V1_SIZE 136u

#ifdef _WIN32
#ifdef LMXXF_NR_RUNTIME_EXPORTS
#define LMXXF_NR_EXPORT __declspec(dllexport)
#else
#define LMXXF_NR_EXPORT __declspec(dllimport)
#endif
#else
#define LMXXF_NR_EXPORT
#endif

LMXXF_NR_EXPORT int32_t LmxxfNrGetApi(uint32_t abi_version, LmxxfNrApi *out);

#ifdef __cplusplus
}
#endif
