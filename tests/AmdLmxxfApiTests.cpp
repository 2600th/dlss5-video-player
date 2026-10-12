#include "AmdLmxxfApi.h"
#include "AmdLmxxfPolicy.h"
#include "AmdLmxxfSession.h"
#include "NeuralStabilitySettings.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
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

// Whether the accumulation rung is on by default on AMD, and whether Off keeps
// the key this backend published before the pass existed.
//
// Both halves are load-bearing and both are cheap to get wrong silently. If the
// default were Off, this backend would ship the arm that flickers. If Off grew a
// line in the document, every AMD entry already on disk - made by a build whose
// picture is exactly what Off produces - would be retired and re-rendered for a
// byte.
void amd_accumulation_rung_is_on_by_default_and_off_keeps_the_old_key_test()
{
    // Off is not the neutral setting here: the runtime relights every frame on
    // its own, so Off is the picture with the model's own frame-to-frame jitter
    // left in place, and the pass is what takes it out. Low rather than a
    // stronger rung because this is a default and not a choice - the rungs trade
    // steadiness for detail monotonically, and Low moves the picture least
    // (AmdLmxxfPolicy.h).
    CHECK_EQ(TemporalStability::Low, amd_lmxxf::kAmdDefaultStability);

    // The pre-pass document, spelled out. Any added or renamed line here is a
    // change to every AMD key on disk at the defaults, so the check is against
    // the literal text and not against `AmdNeuralSettingsDocument(Off)`.
    const std::string before = "backend=amd-lmxxf\n"
                               "temporal=off\n"
                               "motion=constant(0,0)\n"
                               "depth=constant(1)\n";
    CHECK_EQ(before, AmdNeuralSettingsDocument(TemporalStability::Off));

    // Every other rung adds exactly its own line, and no two rungs share a
    // document: two rungs blend differently, so one key for both would serve a
    // render made under the other.
    std::string seen[3];
    size_t distinct = 0;
    for (const TemporalStability rung :
         {TemporalStability::Low, TemporalStability::Medium, TemporalStability::High}) {
        const std::string document = AmdNeuralSettingsDocument(rung);
        const std::string line = "accumulation=" + std::string(TemporalStabilityName(rung)) + "\n";
        CHECK_EQ(before.size() + line.size(), document.size());
        CHECK(document.compare(0, before.size(), before) == 0);
        CHECK(document.compare(before.size(), line.size(), line) == 0);
        for (size_t other = 0; other < distinct; ++other) CHECK(document != seen[other]);
        seen[distinct++] = document;
    }

    // The experimental pass count keys the same way and for the same reason:
    // the default 1 writes nothing, so every render before the experiment keys
    // as it keyed, and each non-default count is its own line because N passes
    // is N runs of the model and must never serve another count's entry.
    CHECK_EQ(before, AmdNeuralSettingsDocument(TemporalStability::Off, 1));
    for (const uint32_t passes : {2u, 3u, 4u}) {
        const std::string document = AmdNeuralSettingsDocument(TemporalStability::Off, passes);
        const std::string line = "passes=" + std::to_string(passes) + "\n";
        CHECK_EQ(before.size() + line.size(), document.size());
        CHECK(document.compare(before.size(), line.size(), line) == 0);
        // And the count composes with a rung rather than replacing it.
        const std::string both =
            AmdNeuralSettingsDocument(TemporalStability::Low, passes);
        CHECK(both.find("accumulation=low\n") != std::string::npos);
        CHECK(both.find(line) != std::string::npos);
    }
}

// Which key a rung is remembered under, and what a machine takes when its key
// says nothing.
//
// The two backends' switches are different features - NVIDIA's is the add-on's
// own ladder in its capture path, opt-in; AMD's is this player's accumulation
// over the runtime's output, on by default - so one shared key would hand a
// machine the other backend's answer, and on AMD that answer is the arm that
// flickers.
void stability_rung_is_remembered_under_a_key_per_backend_test()
{
    CHECK(std::wstring_view(StabilitySettingsKey(NeuralBackend::Nvidia)) == L"Stability");
    CHECK(std::wstring_view(StabilitySettingsKey(NeuralBackend::Amd)) == L"AmdStability");

    CHECK_EQ(TemporalStability::Off, StabilitySettingsDefault(NeuralBackend::Nvidia));
    CHECK_EQ(amd_lmxxf::kAmdDefaultStability, StabilitySettingsDefault(NeuralBackend::Amd));

    // A rung that was saved is honoured, whatever it is and on either backend.
    // Off on AMD is the case that matters: it is reachable, so it has to be
    // readable back rather than being replaced by the default.
    CHECK_EQ(TemporalStability::High, SavedStability(NeuralBackend::Nvidia, "high"));
    CHECK_EQ(TemporalStability::Medium, SavedStability(NeuralBackend::Amd, "medium"));
    CHECK_EQ(TemporalStability::Off, SavedStability(NeuralBackend::Amd, "off"));

    // Nothing saved, or something a hand edit or a future build left behind:
    // this backend's own default and never the other's. The file is written in
    // the lowercase spelling, so anything else is malformed rather than a rung.
    CHECK_EQ(StabilitySettingsDefault(NeuralBackend::Nvidia),
             SavedStability(NeuralBackend::Nvidia, ""));
    CHECK_EQ(StabilitySettingsDefault(NeuralBackend::Amd), SavedStability(NeuralBackend::Amd, ""));
    CHECK_EQ(StabilitySettingsDefault(NeuralBackend::Amd),
             SavedStability(NeuralBackend::Amd, "Low"));
    CHECK_EQ(StabilitySettingsDefault(NeuralBackend::Amd), SavedStability(NeuralBackend::Amd, "2"));
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
        {"amd_accumulation_rung_is_on_by_default_and_off_keeps_the_old_key_test",
         amd_accumulation_rung_is_on_by_default_and_off_keeps_the_old_key_test},
        {"stability_rung_is_remembered_under_a_key_per_backend_test",
         stability_rung_is_remembered_under_a_key_per_backend_test},
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
