#include "AmdLmxxfApi.h"
#include "AmdLmxxfPolicy.h"
#include "AmdLmxxfSession.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <type_traits>

namespace {

int failures = 0;

void Check(bool condition, std::string_view expression, int line)
{
    if (condition) return;
    ++failures;
    std::cerr << "line " << line << ": CHECK failed: " << expression << '\n';
}

template <typename Expected, typename Actual>
void CheckEqual(const Expected& expected, const Actual& actual,
                std::string_view expectedExpression, std::string_view actualExpression,
                int line)
{
    if (expected == actual) return;
    ++failures;
    std::cerr << "line " << line << ": CHECK_EQ failed: "
              << expectedExpression << " != " << actualExpression;
    if constexpr (requires(std::ostream& out, const Expected& value) { out << value; }) {
        if constexpr (requires(std::ostream& out, const Actual& value) { out << value; })
            std::cerr << " (expected " << expected << ", actual " << actual << ')';
    }
    std::cerr << '\n';
}

#define CHECK(expression) Check((expression), #expression, __LINE__)
#define CHECK_EQ(expected, actual) \
    CheckEqual((expected), (actual), #expected, #actual, __LINE__)

LmxxfNrApi CompleteApi()
{
    LmxxfNrApi api{};
    api.struct_size = sizeof(api);
    api.abi_version = LMXXF_NR_ABI_VERSION;
    api.QueryCapabilities = [](LmxxfNrCapabilities*) { return int32_t{LMXXF_NR_OK}; };
    api.Create = [](const LmxxfNrCreateInfo*, void**) { return int32_t{LMXXF_NR_OK}; };
    api.Destroy = [](void*) { return int32_t{LMXXF_NR_OK}; };
    api.PrepareSession = [](void*) { return int32_t{LMXXF_NR_OK}; };
    api.PrepareFrame = [](void*, const LmxxfNrFrameInfo*, LmxxfNrJob*) {
        return int32_t{LMXXF_NR_OK};
    };
    api.RecordInputs = [](void*, void*, void*) { return int32_t{LMXXF_NR_OK}; };
    api.EnqueueHip = [](void*, void*, void*) { return int32_t{LMXXF_NR_OK}; };
    api.RecordOutputs = [](void*, void*, void*) { return int32_t{LMXXF_NR_OK}; };
    api.ExecuteAfterProducer = [](void*, void*, void*) { return int32_t{LMXXF_NR_OK}; };
    api.CancelUnsubmitted = [](void*, void*) { return int32_t{LMXXF_NR_OK}; };
    api.Poll = [](void*, void*, uint32_t*) { return int32_t{LMXXF_NR_OK}; };
    api.Retire = [](void*, void*) { return int32_t{LMXXF_NR_OK}; };
    api.ResetHistory = [](void*) { return int32_t{LMXXF_NR_OK}; };
    api.Drain = [](void*) { return int32_t{LMXXF_NR_OK}; };
    api.GetStatus = [](void*, char*, uint32_t) { return int32_t{LMXXF_NR_OK}; };
    api.GetLastError = [](char*, uint32_t) { return int32_t{LMXXF_NR_OK}; };
    return api;
}

void complete_table_is_accepted_without_the_abi3_appendix_test()
{
    LmxxfNrApi api = CompleteApi();
    // GetTimings and the four recording functions are the ABI 3 appendix. A
    // table without them is still complete enough to produce a frame.
    api.GetTimings = nullptr;
    api.BeginRecordingExecution = nullptr;
    api.EndRecordingExecution = nullptr;
    api.InvalidateRecording = nullptr;
    api.CollectRecording = nullptr;
    CHECK_EQ(amd_lmxxf::ApiProblem::None, amd_lmxxf::ValidateApiTable(api));

    // A legacy-sized table is not a substitute. Its tail was never written, so
    // reading GetTimings out of it would be reading whatever follows in memory.
    api.struct_size = LMXXF_NR_API_V1_SIZE;
    CHECK_EQ(amd_lmxxf::ApiProblem::StructSizeMismatch, amd_lmxxf::ValidateApiTable(api));

    api.struct_size = LMXXF_NR_API_V2_SIZE;
    CHECK_EQ(amd_lmxxf::ApiProblem::StructSizeMismatch, amd_lmxxf::ValidateApiTable(api));

    api = CompleteApi();
    api.struct_size = LMXXF_NR_API_V3_SIZE;
    CHECK_EQ(amd_lmxxf::ApiProblem::None, amd_lmxxf::ValidateApiTable(api));
}

void malformed_table_is_rejected_before_use_test()
{
    LmxxfNrApi api = CompleteApi();
    api.struct_size = 1;
    CHECK_EQ(amd_lmxxf::ApiProblem::StructSizeMismatch, amd_lmxxf::ValidateApiTable(api));

    api = CompleteApi();
    api.abi_version = 2;
    CHECK_EQ(amd_lmxxf::ApiProblem::AbiMismatch, amd_lmxxf::ValidateApiTable(api));

    api = CompleteApi();
    api.RecordOutputs = nullptr;
    CHECK_EQ(amd_lmxxf::ApiProblem::MissingFunction, amd_lmxxf::ValidateApiTable(api));

    api = CompleteApi();
    api.GetTimings = nullptr;
    CHECK_EQ(amd_lmxxf::ApiProblem::None, amd_lmxxf::ValidateApiTable(api));
}

void capabilities_are_validated_against_their_declared_size_test()
{
    LmxxfNrCapabilities capabilities{};
    capabilities.struct_size = sizeof(capabilities);
    capabilities.abi_version = LMXXF_NR_ABI_VERSION;
    capabilities.max_input_width = 1920;
    capabilities.max_input_height = 1080;
    // ABI 3 removed hip_ready; readiness comes from GetStatus after Create.
    CHECK_EQ(uint32_t{32}, uint32_t{sizeof(LmxxfNrCapabilities)});
    CHECK_EQ(amd_lmxxf::ApiProblem::None, amd_lmxxf::ValidateCapabilities(capabilities));

    capabilities.max_input_width = 0;
    CHECK_EQ(amd_lmxxf::ApiProblem::InvalidCapabilities,
             amd_lmxxf::ValidateCapabilities(capabilities));

    capabilities = {};
    capabilities.struct_size = sizeof(capabilities);
    capabilities.abi_version = LMXXF_NR_ABI_VERSION;
    capabilities.max_input_width = 1920;
    capabilities.max_input_height = 1080;
    capabilities.struct_size = 36u; // the ABI 1 size, which had hip_ready
    CHECK_EQ(amd_lmxxf::ApiProblem::InvalidCapabilities,
             amd_lmxxf::ValidateCapabilities(capabilities));
}

void call_order_accepts_only_the_documented_phase_transitions_test()
{
    using Phase = amd_lmxxf::CallPhase;
    CHECK(amd_lmxxf::CanRecordInputs(Phase::Prepared));
    CHECK(!amd_lmxxf::CanRecordInputs(Phase::Idle));
    CHECK(!amd_lmxxf::CanRecordInputs(Phase::ProducerSubmitted));

    CHECK(amd_lmxxf::CanEnqueueHip(Phase::ProducerSubmitted));
    CHECK(amd_lmxxf::CanEnqueueHip(Phase::ConsumerRecorded));
    CHECK(!amd_lmxxf::CanEnqueueHip(Phase::Prepared));

    CHECK(amd_lmxxf::CanRecordOutputs(Phase::ProducerSubmitted));
    CHECK(amd_lmxxf::CanRecordOutputs(Phase::HipEnqueued));
    CHECK(!amd_lmxxf::CanRecordOutputs(Phase::Prepared));

    CHECK(amd_lmxxf::CanRetire(Phase::ConsumerRecorded));
    CHECK(amd_lmxxf::CanRetire(Phase::HipEnqueued));
    CHECK(!amd_lmxxf::CanRetire(Phase::ProducerRecorded));
    CHECK(amd_lmxxf::IsTerminal(Phase::Retired));
    CHECK(amd_lmxxf::IsTerminal(Phase::Cancelled));
}

void frame_builder_sets_the_sizes_the_runtime_negotiates_test()
{
    uint32_t color = 0;
    uint32_t list = 0;
    const LmxxfNrFrameInfo frame = amd_lmxxf::MakeFrameInfo(
        &color, 1920, 1080, 4u, &list, 7u, 11u, 2u, 0.75f, 0.5f, 1.0f);
    CHECK_EQ(uint32_t{sizeof(LmxxfNrFrameInfo)}, frame.struct_size);
    CHECK_EQ(uint64_t{7}, frame.session_id);
    CHECK_EQ(uint64_t{11}, frame.frame_id);
    CHECK_EQ(uint64_t{2}, frame.list_generation);
    CHECK_EQ(static_cast<void*>(&color), frame.color);
    CHECK_EQ(static_cast<void*>(&list), frame.command_list);
    CHECK_EQ(uint32_t{1920}, frame.color_width);
    CHECK_EQ(uint32_t{1080}, frame.color_height);
    CHECK_EQ(uint32_t{4}, frame.color_state);
    CHECK_EQ(uint32_t{LMXXF_NR_FRAME_FLAG_STRENGTH}, frame.flags);
    CHECK_EQ(0.75f, frame.transfer_strength);
    CHECK_EQ(0.5f, frame.color_strength);
    CHECK_EQ(1.0f, frame.model_scale);
    CHECK_EQ(1.0f, frame.pre_exposure);
    CHECK_EQ(1.0f, frame.exposure_scale);
    // ABI 3 grew the frame info from 80 to 160 bytes, and paper_white is
    // required by the codec Record path rather than optional.
    CHECK_EQ(uint32_t{160}, uint32_t{sizeof(LmxxfNrFrameInfo)});
    CHECK_EQ(1.0f, frame.paper_white);
    CHECK_EQ(uint32_t{0}, frame.reserved_after_exposure);
    CHECK_EQ(uint32_t{0}, frame.temporal_flags);
    CHECK(frame.exposure == nullptr);
    CHECK(frame.motion == nullptr);
    CHECK(frame.depth == nullptr);

    const LmxxfNrJob job = amd_lmxxf::MakeJob();
    CHECK_EQ(uint32_t{sizeof(LmxxfNrJob)}, job.struct_size);
    CHECK(job.handle == nullptr);
    CHECK(job.private_output == nullptr);

    const LmxxfNrTimings timings = amd_lmxxf::MakeTimings();
    CHECK_EQ(uint32_t{sizeof(LmxxfNrTimings)}, timings.struct_size);
    CHECK_EQ(uint32_t{0}, timings.valid);
}

struct FakeContext
{
    int prepare = 0;
    int inputs = 0;
    int enqueue = 0;
    int outputs = 0;
    int retire = 0;
    int cancel = 0;
    int reset = 0;
    // ABI 3 job plumbing. PrepareFrame is handed the caller's struct address;
    // every call after it must be handed the handle the runtime wrote back.
    // Recording both is what makes a regression to &job visible.
    void* preparedStructAddress = nullptr;
    void* lastJobArgument = nullptr;
    int jobArgumentsSeen = 0;
    std::string error;
};

LmxxfNrApi FakeApi(FakeContext& fake)
{
    LmxxfNrApi api{};
    api.struct_size = sizeof(api);
    api.abi_version = LMXXF_NR_ABI_VERSION;
    api.PrepareFrame = [](void* context, const LmxxfNrFrameInfo*, LmxxfNrJob* job) {
        auto* state = static_cast<FakeContext*>(context);
        ++state->prepare;
        state->preparedStructAddress = job;
        // The real runtime publishes its own job identity here, not the
        // caller's address - which is exactly why the address cannot be reused.
        job->handle = state;
        job->private_output = state;
        return int32_t{LMXXF_NR_OK};
    };
    api.RecordInputs = [](void* context, void* job, void*) {
        auto* state = static_cast<FakeContext*>(context);
        ++state->inputs;
        state->lastJobArgument = job;
        ++state->jobArgumentsSeen;
        return int32_t{LMXXF_NR_OK};
    };
    api.EnqueueHip = [](void* context, void* job, void*) {
        auto* state = static_cast<FakeContext*>(context);
        ++state->enqueue;
        state->lastJobArgument = job;
        ++state->jobArgumentsSeen;
        return int32_t{LMXXF_NR_OK};
    };
    api.RecordOutputs = [](void* context, void* job, void*) {
        auto* state = static_cast<FakeContext*>(context);
        ++state->outputs;
        state->lastJobArgument = job;
        ++state->jobArgumentsSeen;
        return int32_t{LMXXF_NR_OK};
    };
    api.Retire = [](void* context, void* job) {
        auto* state = static_cast<FakeContext*>(context);
        ++state->retire;
        state->lastJobArgument = job;
        ++state->jobArgumentsSeen;
        return int32_t{LMXXF_NR_OK};
    };
    api.CancelUnsubmitted = [](void* context, void* job) {
        auto* state = static_cast<FakeContext*>(context);
        ++state->cancel;
        state->lastJobArgument = job;
        ++state->jobArgumentsSeen;
        return int32_t{LMXXF_NR_OK};
    };
    api.ResetHistory = [](void* context) {
        ++static_cast<FakeContext*>(context)->reset;
        return int32_t{LMXXF_NR_OK};
    };
    api.GetLastError = [](char* buffer, uint32_t chars) {
        if (buffer != nullptr && chars != 0) buffer[0] = '\0';
        return int32_t{LMXXF_NR_OK};
    };
    (void)fake;
    return api;
}

void session_rejects_out_of_order_calls_before_reaching_the_runtime_test()
{
    FakeContext fake;
    const LmxxfNrApi api = FakeApi(fake);
    amd_lmxxf::Session session(api, &fake);

    CHECK(!session.RecordInputs(&fake));
    CHECK(!session.EnqueueHip(&fake));
    CHECK(!session.RecordOutputs(&fake));
    CHECK(!session.Retire());
    CHECK_EQ(0, fake.inputs);
    CHECK_EQ(0, fake.enqueue);
    CHECK_EQ(0, fake.outputs);
    CHECK_EQ(0, fake.retire);

    const LmxxfNrFrameInfo frame = amd_lmxxf::MakeFrameInfo(
        &fake, 1920, 1080, 4u, &fake, 1u, 1u, 1u);
    CHECK(session.PrepareFrame(frame));
    CHECK_EQ(amd_lmxxf::CallPhase::Prepared, session.Phase());
    CHECK(session.RecordInputs(&fake));
    CHECK(!session.RecordInputs(&fake));
    CHECK_EQ(amd_lmxxf::CallPhase::ProducerRecorded, session.Phase());
    CHECK(session.ProducerSubmitted());
    CHECK(session.EnqueueHip(&fake));
    CHECK(session.RecordOutputs(&fake));
    CHECK(session.Retire());
    CHECK_EQ(amd_lmxxf::CallPhase::Retired, session.Phase());
    CHECK_EQ(1, fake.prepare);
    CHECK_EQ(1, fake.inputs);
    CHECK_EQ(1, fake.enqueue);
    CHECK_EQ(1, fake.outputs);
    CHECK_EQ(1, fake.retire);
}

void session_accepts_recording_outputs_before_enqueue_and_call_order_doc_test()
{
    FakeContext fake;
    const LmxxfNrApi api = FakeApi(fake);
    amd_lmxxf::Session session(api, &fake);
    const LmxxfNrFrameInfo frame = amd_lmxxf::MakeFrameInfo(
        &fake, 1280, 720, 4u, &fake, 1u, 2u, 1u);

    CHECK(session.PrepareFrame(frame));
    CHECK(session.RecordInputs(&fake));
    CHECK(session.ProducerSubmitted());
    CHECK(session.RecordOutputs(&fake));
    CHECK(session.EnqueueHip(&fake));
    CHECK(session.Retire());
    CHECK_EQ(1, fake.outputs);
    CHECK_EQ(1, fake.enqueue);
}

void session_hands_the_runtime_the_job_handle_after_prepare_frame_test()
{
    FakeContext fake;
    const LmxxfNrApi api = FakeApi(fake);
    amd_lmxxf::Session session(api, &fake);
    const LmxxfNrFrameInfo frame = amd_lmxxf::MakeFrameInfo(
        &fake, 1920, 1080, 4u, &fake, 1u, 1u, 1u);

    CHECK(session.PrepareFrame(frame));
    const void* structAddress = fake.preparedStructAddress;
    const void* handle = session.Job().handle;
    CHECK(structAddress != nullptr);
    CHECK(handle != nullptr);
    // The runtime publishes its own job identity, not the caller's address.
    // Handing the address back is what ABI 3 rejects at run time with
    // "need job and command list" even though nothing is null.
    CHECK(handle != structAddress);

    CHECK(session.RecordInputs(&fake));
    CHECK_EQ(handle, fake.lastJobArgument);
    CHECK(session.ProducerSubmitted());
    CHECK(session.EnqueueHip(&fake));
    CHECK_EQ(handle, fake.lastJobArgument);
    CHECK(session.RecordOutputs(&fake));
    CHECK_EQ(handle, fake.lastJobArgument);
    CHECK(session.Retire());
    CHECK_EQ(handle, fake.lastJobArgument);
    // RecordInputs, EnqueueHip, RecordOutputs and Retire all took the handle.
    CHECK_EQ(4, fake.jobArgumentsSeen);
}

void session_cancel_is_terminal_and_reset_is_explicit_test()
{
    FakeContext fake;
    const LmxxfNrApi api = FakeApi(fake);
    amd_lmxxf::Session session(api, &fake);
    const LmxxfNrFrameInfo frame = amd_lmxxf::MakeFrameInfo(
        &fake, 1280, 720, 4u, &fake, 1u, 3u, 1u);

    CHECK(session.PrepareFrame(frame));
    CHECK(session.Cancel());
    CHECK_EQ(amd_lmxxf::CallPhase::Cancelled, session.Phase());
    CHECK(!session.Cancel());
    CHECK(session.ResetHistory());
    CHECK_EQ(1, fake.cancel);
    CHECK_EQ(1, fake.reset);
}

void amd_gpu_policy_only_admits_the_targets_the_runtime_has_modules_for_test()
{
    CHECK_EQ(amd_lmxxf::GpuFamily::Rdna4,
             amd_lmxxf::ClassifyGpu(0x1002, L"AMD Radeon RX 9070 XT"));
    CHECK_EQ(amd_lmxxf::KernelTarget::Gfx1201,
             amd_lmxxf::SelectKernelTarget(amd_lmxxf::GpuFamily::Rdna4,
                                           L"AMD Radeon RX 9070 XT"));
    CHECK_EQ(amd_lmxxf::KernelTarget::Gfx1200,
             amd_lmxxf::SelectKernelTarget(amd_lmxxf::GpuFamily::Rdna4,
                                           L"AMD Radeon RX 9060 XT"));
    CHECK_EQ(amd_lmxxf::GpuFamily::Unsupported,
             amd_lmxxf::ClassifyGpu(0x1002, L"AMD Radeon RX 7900 XTX"));
    CHECK_EQ(amd_lmxxf::GpuFamily::Unsupported,
             amd_lmxxf::ClassifyGpu(0x10DE, L"NVIDIA GeForce RTX 5090"));
}

void amd_runtime_layout_requires_every_checked_component_test()
{
    amd_lmxxf::RuntimeLayout layout{};
    CHECK_EQ(amd_lmxxf::LayoutState::Absent, amd_lmxxf::ClassifyLayout(layout));

    layout.runtimeDll = true;
    CHECK_EQ(amd_lmxxf::LayoutState::Incomplete, amd_lmxxf::ClassifyLayout(layout));

    layout.assetsDirectory = true;
    layout.shaders = true;
    layout.modules = true;
    layout.weights = true;
    CHECK_EQ(amd_lmxxf::LayoutState::Incomplete, amd_lmxxf::ClassifyLayout(layout));

    layout.lockFile = true;
    CHECK(layout.Complete());
    CHECK_EQ(amd_lmxxf::LayoutState::Complete, amd_lmxxf::ClassifyLayout(layout));
}

void Run(std::string_view only)
{
    const struct {
        const char* name;
        void (*run)();
    } cases[]{
        {"complete_table_is_accepted_without_the_abi3_appendix_test",
         complete_table_is_accepted_without_the_abi3_appendix_test},
        {"malformed_table_is_rejected_before_use_test",
         malformed_table_is_rejected_before_use_test},
        {"capabilities_are_validated_against_their_declared_size_test",
         capabilities_are_validated_against_their_declared_size_test},
        {"call_order_accepts_only_the_documented_phase_transitions_test",
         call_order_accepts_only_the_documented_phase_transitions_test},
        {"frame_builder_sets_the_sizes_the_runtime_negotiates_test",
         frame_builder_sets_the_sizes_the_runtime_negotiates_test},
        {"session_rejects_out_of_order_calls_before_reaching_the_runtime_test",
         session_rejects_out_of_order_calls_before_reaching_the_runtime_test},
        {"session_accepts_recording_outputs_before_enqueue_and_call_order_doc_test",
         session_accepts_recording_outputs_before_enqueue_and_call_order_doc_test},
        {"session_hands_the_runtime_the_job_handle_after_prepare_frame_test",
         session_hands_the_runtime_the_job_handle_after_prepare_frame_test},
        {"session_cancel_is_terminal_and_reset_is_explicit_test",
         session_cancel_is_terminal_and_reset_is_explicit_test},
        {"amd_gpu_policy_only_admits_the_targets_the_runtime_has_modules_for_test",
         amd_gpu_policy_only_admits_the_targets_the_runtime_has_modules_for_test},
        {"amd_runtime_layout_requires_every_checked_component_test",
         amd_runtime_layout_requires_every_checked_component_test},
    };
    for (const auto& test : cases) {
        if (!only.empty() && std::string_view(test.name).find(only) == std::string_view::npos)
            continue;
        test.run();
    }
}

} // namespace

int main(int argc, char** argv)
{
    Run(argc > 1 ? argv[1] : std::string_view{});
    std::cout << "AmdLmxxfApiTests: " << (failures == 0 ? "PASS" : "FAIL") << '\n';
    return failures == 0 ? 0 : 1;
}
