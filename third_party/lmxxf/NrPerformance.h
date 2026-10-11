#pragma once

/*
 * Vendored from TheAutomatic/dlss-5-amd-project, tag v1.10.5-alpha,
 * OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/NrPerformance.h.
 *
 * Pulled in because the ABI 3 LmxxfNrApi.h declares the optional
 * LmxxfNrGetTimingApi export with a LmxxfNrTimingApi* out parameter, and that
 * type lives here. Only the type declarations are needed; nothing in this file
 * is called by the host today.
 */

#include <stdint.h>

/* Independent telemetry ABI. Durations are milliseconds; timestamps use GetTickCount64.
 * GPU fields stay invalid (samples == 0) until actual completion is confirmed.
 * CPU and GPU durations are different measurements, not additive frame time. */
#define NR_TIMING_VERSION 1u
#define NR_TIMING_STAGE_COUNT 9u
enum NrTimingStage
{
    NR_CPU_PREPARE, NR_CPU_ENQUEUE, NR_CPU_REBUILD, NR_CPU_DRAIN,
    NR_GPU_ENCODE, NR_GPU_NETWORK, NR_GPU_DECODE, NR_GPU_BLEND, NR_GPU_STABILIZER
};
typedef struct NrTimingValue
{
    uint64_t samples;
    double last_ms, mean_ms, max_ms;
    uint64_t last_tick_ms, frame_id, execution_id;
} NrTimingValue;
typedef struct NrTimingSnapshot
{
    uint32_t struct_size, version, enabled, reserved;
    uint64_t dropped;
    NrTimingValue stages[NR_TIMING_STAGE_COUNT];
} NrTimingSnapshot;

typedef struct LmxxfNrTimingApi
{
    uint32_t struct_size, version;
    /* Callers retain the session until the query returns. No GPU waits in these calls. */
    int32_t (*SetEnabled)(void* context, uint32_t enabled);
    int32_t (*GetSnapshot)(void* context, NrTimingSnapshot* out);
} LmxxfNrTimingApi;
