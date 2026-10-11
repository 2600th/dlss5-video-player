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

// Names every status the ABI 3 enum defines, DEVICE_LOST included, so a
// rejected call reports which contract it broke instead of a bare number.
constexpr std::string_view LmxxfNrStatusName(int32_t status) noexcept
{
    switch (status) {
        case LMXXF_NR_OK: return "OK";
        case LMXXF_NR_UNSUPPORTED_ABI: return "UNSUPPORTED_ABI";
        case LMXXF_NR_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case LMXXF_NR_NOT_IMPLEMENTED: return "NOT_IMPLEMENTED";
        case LMXXF_NR_UNAVAILABLE: return "UNAVAILABLE";
        case LMXXF_NR_FAILED: return "FAILED";
        case LMXXF_NR_DEVICE_LOST: return "DEVICE_LOST";
    }
    return "UNKNOWN";
}

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

// The ABI 3 tail is deliberately excluded: GetTimings and the four recording
// functions (BeginRecordingExecution, EndRecordingExecution,
// InvalidateRecording, CollectRecording) were appended after the first ABI, so
// a table that predates them is still a valid table. Every operation needed to
// produce a frame must be present.
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
    // Size is exact, not a floor: the runtime hands back one specific table for
    // one specific abi_version, and the tail of a shorter one is not ours to
    // read. Accepting LMXXF_NR_API_V1_SIZE here would let a v1 table through
    // with GetTimings pointing at whatever follows it in memory.
    if (api.struct_size != sizeof(LmxxfNrApi))
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
    // The recording-lease half of the state machine. A lease session has no
    // Retire and no CancelUnsubmitted: a job is released by invalidating it and
    // then collecting it, and Collection may say "not yet" and have to be
    // retried, which is why those are phases of their own rather than a flag.
    RecordingBegun,
    RecordingEnded,
    Collected,
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
    return phase == CallPhase::Collected || phase == CallPhase::Retired ||
           phase == CallPhase::Cancelled;
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
    // ABI 3: required in the current package ABI and not optional in practice -
    // the codec encode/decode Record path scales by it. 1.0 is the documented
    // default and is what a non-HDR SDR source wants.
    info.paper_white = 1.0f;
    // temporal_flags, motion and depth are left zero, so this frame renders
    // spatially. That is the truthful answer for a host that has no guides
    // rather than a broken contract: the runtime reports history=off and
    // denoises every frame on its own. A caller that HAS guides applies them
    // with ApplyTemporalGuides below, and must also have created its session
    // with LMXXF_NR_CREATE_FLAG_RECORDING_LEASES - declaring the flags without
    // them is what fails, not leaving them unset.
    // See docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md section 5.3.
    return info;
}

// Everything the temporal model wants from the host, gathered into one struct
// so the frame builder stays a single call. ABI 3 grew LmxxfNrFrameInfo a
// motion and a depth texture plus the scalars that say how to read them, and
// the runtime's PrepareHistory refuses the whole feature if any one of them is
// missing, foreign to the device, or outside the format whitelist.
//
// The gates, in the order PrepareHistory applies them. The string it writes to
// historyStatus is in brackets - that string is the only place any of this is
// ever reported, so a run that reads history=off has exactly one of these to
// answer for:
//
//   * the session was created with LMXXF_NR_CREATE_FLAG_RECORDING_LEASES.
//     Without it MODEL_HISTORY is answered "unsupported-lifecycle" before any
//     texture check runs at all. The lease model is not an optimisation bolted
//     onto the temporal feature - it is the only lifecycle the history runs
//     under, which is why the ABI's own call order changes with the flag.
//   * LMXXF_NR_TEMPORAL_MODEL_HISTORY is set in flags                  [off]
//   * not a codec or HIP passthrough, and debug_view == 0    [diagnostic-view]
//   * the bridge has a post-auxiliary layout        [unsupported-post-layout]
//   * LMXXF_NR_TEMPORAL_INPUTS_VALID is set          [unknown-motion-contract]
//   * motion and depth are both non-null, are distinct, and neither is the
//     colour texture itself                                     [missing-guides]
//   * both are Texture2D, single-array, single-sample, SRV-readable, carrying
//     at least one mip, and in MotionFormat/DepthFormat's whitelist
//                                                  [unsupported-guide-format]
//   * motion_width/height are non-zero and inside the motion texture, and the
//     colour fits inside the depth texture                      [guide-extent]
//   * all four scalars are finite                      [invalid-motion-scalars]
//   * both guides report the session's own device                 [guide-device]
//
// Anything but "ready" means the frame renders spatially. It is never an error
// and it never throws, which is exactly why the status line has to be read.
struct TemporalGuides {
    // ID3D12Resource*. Motion is an (x, y) pair in whatever unit
    // motionScaleX/Y describe; depth is one channel the shader reads alone.
    void* motion{};
    void* depth{};
    // The D3D12_RESOURCE_STATES each is in when RecordInputs runs.
    uint32_t motionState{};
    uint32_t depthState{};
    // The motion FIELD's own extent, which is what the runtime normalises the
    // stored vectors against. Not necessarily the texture size: a field may
    // occupy a fraction of it, and the integration plan's analysis grid does.
    uint32_t motionWidth{};
    uint32_t motionHeight{};
    // What maps the stored vector onto the frame. The kernel computes
    // scaleX = motion_scale_x / motion_width, so a host whose motion texture
    // already stores frame pixels in x passes the frame's own width here and
    // gets scale 1. A field in normalised units passes 1.
    float motionScaleX{};
    float motionScaleY{};
    // Sub-pixel offsets of this frame's sampling grid. A decoded video frame
    // has none, and a host that says so with LMXXF_NR_TEMPORAL_MV_JITTERED must
    // leave these at zero: the runtime then skips the differenced reprojection
    // that exists for a game's per-frame camera jitter.
    float jitterX{};
    float jitterY{};
    // LMXXF_NR_TEMPORAL_MODEL_HISTORY | ..._INPUTS_VALID, plus _MV_JITTERED or
    // _DEPTH_INVERTED as the source requires. RESET is deliberately not part of
    // this: it describes one frame, so ApplyTemporalGuides takes it separately
    // and the caller has to pass it per frame rather than bake it in.
    uint32_t flags{};
    // True when this frame must not continue the previous one's history -
    // first frame, seek, cut, drop, source change, backend rebuild. The runtime
    // crosses its own resets in from the frame_id sequence and the frame
    // interval; this is the host's half of the same decision, and a host that
    // forgets it silently smears the previous shot into this one.
    bool reset{false};
};

// The guide flags a video host wants: history on, guides present, and the
// motion declared unjittered. A decoded frame has no engine jitter, and saying
// so is not cosmetic - it is what keeps the runtime from differencing
// jitter_x/y against the previous frame to reconstruct a reprojection offset
// the source never had, which would read the codec's own noise as camera
// motion.
inline constexpr uint32_t kTemporalVideoFlags =
    LMXXF_NR_TEMPORAL_MODEL_HISTORY | LMXXF_NR_TEMPORAL_INPUTS_VALID |
    LMXXF_NR_TEMPORAL_MV_JITTERED;

// Fills the guide half of `info`. Call it after MakeFrameInfo, which leaves all
// of these zero on purpose.
inline void ApplyTemporalGuides(LmxxfNrFrameInfo& info, const TemporalGuides& guides) noexcept
{
    info.motion = guides.motion;
    info.depth = guides.depth;
    info.motion_state = guides.motionState;
    info.depth_state = guides.depthState;
    info.motion_width = guides.motionWidth;
    info.motion_height = guides.motionHeight;
    info.motion_scale_x = guides.motionScaleX;
    info.motion_scale_y = guides.motionScaleY;
    info.jitter_x = guides.jitterX;
    info.jitter_y = guides.jitterY;
    info.temporal_flags =
        guides.flags | (guides.reset ? LMXXF_NR_TEMPORAL_RESET : 0u);
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
