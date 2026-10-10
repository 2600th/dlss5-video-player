#pragma once

#include "../third_party/lmxxf/LmxxfNrApi.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace amd_lmxxf {

enum class ApiProblem
{
    None,
    NullTable,
    StructSizeMismatch,
    AbiMismatch,
    MissingFunction,
    InvalidCapabilities,
};

constexpr std::string_view ApiProblemName(ApiProblem problem) noexcept
{
    switch (problem) {
        case ApiProblem::None: return "none";
        case ApiProblem::NullTable: return "null-table";
        case ApiProblem::StructSizeMismatch: return "struct-size-mismatch";
        case ApiProblem::AbiMismatch: return "abi-mismatch";
        case ApiProblem::MissingFunction: return "missing-function";
        case ApiProblem::InvalidCapabilities: return "invalid-capabilities";
    }
    return "unknown";
}

// GetTimings is deliberately excluded: it was appended after the first ABI and
// an older runtime returns a valid v1 table without it. Every operation needed
// to produce a frame must be present.
inline bool HasRequiredFunctions(const LmxxfNrApi& api) noexcept
{
    return api.QueryCapabilities != nullptr &&
           api.Create != nullptr &&
           api.Destroy != nullptr &&
           api.PrepareSession != nullptr &&
           api.PrepareFrame != nullptr &&
           api.RecordInputs != nullptr &&
           api.EnqueueHip != nullptr &&
           api.RecordOutputs != nullptr &&
           api.ExecuteAfterProducer != nullptr &&
           api.CancelUnsubmitted != nullptr &&
           api.Poll != nullptr &&
           api.Retire != nullptr &&
           api.ResetHistory != nullptr &&
           api.Drain != nullptr &&
           api.GetStatus != nullptr &&
           api.GetLastError != nullptr;
}

inline ApiProblem ValidateApiTable(const LmxxfNrApi& api) noexcept
{
    if (api.struct_size != LMXXF_NR_API_V1_SIZE && api.struct_size != sizeof(LmxxfNrApi))
        return ApiProblem::StructSizeMismatch;
    if (api.abi_version != LMXXF_NR_ABI_VERSION)
        return ApiProblem::AbiMismatch;
    if (!HasRequiredFunctions(api))
        return ApiProblem::MissingFunction;
    return ApiProblem::None;
}

inline ApiProblem ValidateCapabilities(const LmxxfNrCapabilities& capabilities) noexcept
{
    if (capabilities.struct_size != sizeof(LmxxfNrCapabilities) ||
        capabilities.abi_version != LMXXF_NR_ABI_VERSION ||
        capabilities.max_input_width == 0 ||
        capabilities.max_input_height == 0)
        return ApiProblem::InvalidCapabilities;
    return ApiProblem::None;
}

enum class CallPhase
{
    Idle,
    Prepared,
    ProducerRecorded,
    ProducerSubmitted,
    HipEnqueued,
    ConsumerRecorded,
    Retired,
    Cancelled,
};

constexpr bool CanRecordInputs(CallPhase phase) noexcept
{
    return phase == CallPhase::Prepared;
}

constexpr bool CanEnqueueHip(CallPhase phase) noexcept
{
    return phase == CallPhase::ProducerSubmitted || phase == CallPhase::ConsumerRecorded;
}

constexpr bool CanRecordOutputs(CallPhase phase) noexcept
{
    return phase == CallPhase::ProducerSubmitted || phase == CallPhase::HipEnqueued;
}

constexpr bool CanRetire(CallPhase phase) noexcept
{
    return phase == CallPhase::ConsumerRecorded || phase == CallPhase::HipEnqueued;
}

constexpr bool IsTerminal(CallPhase phase) noexcept
{
    return phase == CallPhase::Retired || phase == CallPhase::Cancelled;
}

inline LmxxfNrCreateInfo MakeCreateInfo(void* device, void* queue,
                                        const wchar_t* assetsDirectory,
                                        uint32_t flags = 0) noexcept
{
    LmxxfNrCreateInfo info{};
    info.struct_size = sizeof(info);
    info.device = device;
    info.queue = queue;
    info.assets_directory = assetsDirectory;
    info.flags = flags;
    return info;
}

inline LmxxfNrFrameInfo MakeFrameInfo(void* color, uint32_t width, uint32_t height,
                                      uint32_t colorState, void* commandList,
                                      uint64_t sessionId, uint64_t frameId,
                                      uint64_t listGeneration,
                                      float transferStrength = 1.0f,
                                      float colorStrength = 1.0f,
                                      float modelScale = 1.0f) noexcept
{
    LmxxfNrFrameInfo info{};
    info.struct_size = sizeof(info);
    info.session_id = sessionId;
    info.frame_id = frameId;
    info.list_generation = listGeneration;
    info.command_list = commandList;
    info.color_width = width;
    info.color_height = height;
    info.color = color;
    info.color_state = colorState;
    info.flags = LMXXF_NR_FRAME_FLAG_STRENGTH;
    info.transfer_strength = transferStrength;
    info.color_strength = colorStrength;
    info.model_scale = modelScale;
    info.pre_exposure = 1.0f;
    info.exposure_scale = 1.0f;
    return info;
}

inline LmxxfNrJob MakeJob() noexcept
{
    LmxxfNrJob job{};
    job.struct_size = sizeof(job);
    return job;
}

inline LmxxfNrTimings MakeTimings() noexcept
{
    LmxxfNrTimings timings{};
    timings.struct_size = sizeof(timings);
    return timings;
}

} // namespace amd_lmxxf
