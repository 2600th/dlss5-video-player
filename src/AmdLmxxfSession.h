#pragma once

#include "AmdLmxxfApi.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace amd_lmxxf {

// Serializes one runtime session's frame state machine. The API deliberately
// has no thread-local context, so the caller must also serialize calls across
// all jobs belonging to this Session.
//
// ABI 3 changed what the `job` argument of every call after PrepareFrame IS.
// The v1 runtime read it as a pointer to the caller's LmxxfNrJob; the ABI 3
// runtime resolves it through Session::FindJob(handle), which in the default
// (non recording-lease) mode accepts only nullptr or the address of the
// session's OWN job - the value PrepareFrame writes back into job->handle.
// Handing it &job_ therefore fails with "need job and command list" while every
// pointer involved is non-null. Only PrepareFrame takes the struct address;
// everything after it takes the handle.
//
// A session created with LMXXF_NR_CREATE_FLAG_RECORDING_LEASES is a different
// machine, not a variant of this one. Two steps swap places and two disappear:
//
//   * RecordOutputs is recorded BEFORE BeginRecordingExecution, because the
//     runtime refuses it once the job has started and Begin is what starts it.
//     Both command lists are therefore closed before either is submitted, and
//     producer and consumer reach the queue as two separate submissions with
//     the HIP work the runtime enqueues between them.
//   * Retire and CancelUnsubmitted both answer INVALID_ARGUMENT - "Use recording
//     invalidation and collection in lease mode". The only release path is
//     InvalidateRecording then CollectRecording, and Collection may answer
//     UNAVAILABLE, meaning retain the job and try again. No other call in this
//     class has a "not yet".
//
// That lifecycle is also the only one under which the runtime engages its
// temporal model, so it is not an optional extra: a host that wants history
// runs this machine, not the default one.
class Session
{
public:
    Session(const LmxxfNrApi& api, void* context, bool leases = false) noexcept
        : api_(api), context_(context), leases_(leases) {}

    bool Leases() const noexcept { return leases_; }
    void* Context() const noexcept { return context_; }
    CallPhase Phase() const noexcept { return phase_; }
    const LmxxfNrJob& Job() const noexcept { return job_; }
    const std::string& LastError() const noexcept { return lastError_; }

    // The raw LmxxfNrStatus of the most recent step, so callers can still tell
    // DEVICE_LOST apart from a plain failure now that they no longer call the
    // table entries directly. A refusal - an out-of-order host call, where the
    // ABI has no code for "the caller skipped a step" - reports FAILED and puts
    // the real story in LastError().
    int32_t LastStatus() const noexcept { return lastStatus_; }

    bool PrepareFrame(LmxxfNrFrameInfo info)
    {
        if (!CanPrepare()) return Refuse("PrepareFrame is out of order");
        // struct_size is part of the contract and it moved between the revisions
        // the host serves (ABI 6: frame 168, job 32; ABI 3: 160, 24). This
        // session is the one object that knows which revision it was built
        // against, so the stamp happens here once instead of at every caller -
        // a caller cannot forget it and cannot disagree with the runtime.
        StampFrameInfoForAbi(info, api_.abi_version);
        job_ = MakeJob();
        StampJobForAbi(job_, api_.abi_version);
        const int32_t status = api_.PrepareFrame(context_, &info, &job_);
        if (status != static_cast<int32_t>(LMXXF_NR_OK)) return Fail(status, "PrepareFrame");
        if (job_.handle == nullptr) return Refuse("PrepareFrame returned a null job");
        phase_ = CallPhase::Prepared;
        return Succeed();
    }

    bool RecordInputs(void* commandList)
    {
        if (!CanRecordInputs(phase_)) return Refuse("RecordInputs is out of order");
        const int32_t status = api_.RecordInputs(context_, JobHandle(), commandList);
        if (status != static_cast<int32_t>(LMXXF_NR_OK)) return Fail(status, "RecordInputs");
        phase_ = CallPhase::ProducerRecorded;
        return Succeed();
    }

    // Called after the producer command list was submitted on the session's
    // queue. It is host state, not an API call.
    bool ProducerSubmitted()
    {
        if (phase_ != CallPhase::ProducerRecorded)
            return Refuse("producer submission marker is out of order");
        phase_ = CallPhase::ProducerSubmitted;
        return Succeed();
    }

    bool EnqueueHip(void* commandQueue)
    {
        if (!CanEnqueueHipNow()) return Refuse("EnqueueHip is out of order");
        const int32_t status = api_.EnqueueHip(context_, JobHandle(), commandQueue);
        if (status != static_cast<int32_t>(LMXXF_NR_OK)) return Fail(status, "EnqueueHip");
        // In lease mode Begin already moved the machine to RecordingBegun and
        // EndRecording is what closes it, so this must not advance past it. The
        // default order's HipEnqueued phase describes a sequence that lease mode
        // does not have - and advancing to it here is exactly how EndRecording
        // ends up refusing as out of order.
        if (!leases_) phase_ = CallPhase::HipEnqueued;
        return Succeed();
    }

    bool RecordOutputs(void* commandList)
    {
        if (!CanRecordOutputsNow()) return Refuse("RecordOutputs is out of order");
        const int32_t status = api_.RecordOutputs(context_, JobHandle(), commandList);
        if (status != static_cast<int32_t>(LMXXF_NR_OK)) return Fail(status, "RecordOutputs");
        phase_ = CallPhase::ConsumerRecorded;
        return Succeed();
    }

    bool Retire()
    {
        if (leases_) return Refuse("a lease session has no Retire; invalidate and collect instead");
        if (!CanRetire(phase_)) return Refuse("Retire is out of order");
        const int32_t status = api_.Retire(context_, JobHandle());
        if (status != static_cast<int32_t>(LMXXF_NR_OK)) return Fail(status, "Retire");
        phase_ = CallPhase::Retired;
        return Succeed();
    }

    bool Cancel()
    {
        if (leases_) return Refuse("a lease session has no Cancel; invalidate and collect instead");
        if (IsTerminal(phase_)) return Refuse("Cancel is out of order");
        const int32_t status = api_.CancelUnsubmitted(context_, JobHandle());
        if (status != static_cast<int32_t>(LMXXF_NR_OK)) return Fail(status, "CancelUnsubmitted");
        phase_ = CallPhase::Cancelled;
        return Succeed();
    }

    bool ResetHistory()
    {
        const int32_t status = api_.ResetHistory(context_);
        if (status != static_cast<int32_t>(LMXXF_NR_OK)) return Fail(status, "ResetHistory");
        return Succeed();
    }

    // ---- the recording-lease half ----------------------------------------
    //
    // Begin runs after both lists are closed and before either is submitted.
    // The runtime uses it to set up this frame's history chain, which is why it
    // refuses a queue belonging to another device, and why there is no separate
    // marking step here the way the default machine has one: the phase simply
    // does not leave ConsumerRecorded until Begin accepts.
    bool BeginRecording(void* queue)
    {
        if (!leases_) return Refuse("BeginRecording needs a lease session");
        if (phase_ != CallPhase::ConsumerRecorded)
            return Refuse("BeginRecording is out of order");
        const int32_t status = api_.BeginRecordingExecution(context_, JobHandle(), queue);
        if (status != static_cast<int32_t>(LMXXF_NR_OK))
            return Fail(status, "BeginRecordingExecution");
        phase_ = CallPhase::RecordingBegun;
        return Succeed();
    }

    // submittedFlags is LMXXF_NR_SUBMITTED_PRODUCER | ..._CONSUMER once both
    // lists are on the queue. fence/value must be the host's own tail Signal
    // that actually covered them: the runtime stores that pair as this job's
    // completion certificate and refuses a zero value, an UINT64_MAX value or a
    // failed HRESULT rather than guessing whether the work landed.
    bool EndRecording(void* queue, uint32_t submittedFlags, void* fence, uint64_t value,
                      int32_t signalStatus)
    {
        if (!leases_) return Refuse("EndRecording needs a lease session");
        if (phase_ != CallPhase::RecordingBegun)
            return Refuse("EndRecording is out of order");
        const int32_t status = api_.EndRecordingExecution(context_, JobHandle(), queue,
                                                          submittedFlags, fence, value,
                                                          signalStatus);
        if (status != static_cast<int32_t>(LMXXF_NR_OK))
            return Fail(status, "EndRecordingExecution");
        phase_ = CallPhase::RecordingEnded;
        return Succeed();
    }

    // The host's statement that it will submit nothing more against this job.
    // It advances no phase of ours, because it says nothing about where the
    // caller is - Collection is what moves. On the runtime's side it is what
    // releases a job that was still merely prepared, so it is also the right
    // cleanup after a failure that never reached Begin.
    bool InvalidateRecording()
    {
        if (!leases_) return Refuse("InvalidateRecording needs a lease session");
        if (phase_ != CallPhase::RecordingEnded)
            return Refuse("InvalidateRecording is out of order");
        const int32_t status = api_.InvalidateRecording(context_, JobHandle());
        if (status != static_cast<int32_t>(LMXXF_NR_OK))
            return Fail(status, "InvalidateRecording");
        return Succeed();
    }

    // The one call in this class where "not yet" is not a failure. UNAVAILABLE
    // means the job is retained and the caller must ask again, which is why
    // this returns the raw status rather than a bool: everything else here can
    // be read as yes or no, and this cannot.
    int32_t CollectRecording()
    {
        if (!leases_) {
            (void)Refuse("CollectRecording needs a lease session");
            return lastStatus_;
        }
        if (phase_ != CallPhase::RecordingEnded) {
            (void)Refuse("CollectRecording is out of order");
            return lastStatus_;
        }
        const int32_t status = api_.CollectRecording(context_, JobHandle());
        // OK and DEVICE_LOST both consume the handle. Only UNAVAILABLE retains
        // it, and only UNAVAILABLE is worth asking again.
        if (status == static_cast<int32_t>(LMXXF_NR_OK) ||
            status == static_cast<int32_t>(LMXXF_NR_DEVICE_LOST)) {
            phase_ = CallPhase::Collected;
            Succeed();
            return status;
        }
        lastStatus_ = status;
        lastError_.assign("CollectRecording status=");
        lastError_ += std::to_string(status);
        if (api_.GetLastError != nullptr) {
            char buffer[512]{};
            if (api_.GetLastError(buffer, static_cast<uint32_t>(sizeof(buffer))) ==
                    static_cast<int32_t>(LMXXF_NR_OK) &&
                buffer[0] != '\0') {
                lastError_ += ": ";
                lastError_ += buffer;
            }
        }
        return status;
    }

private:
    // The handle PrepareFrame wrote back into job_.handle. Never hand the
    // runtime &job_ after PrepareFrame: see the class comment.
    void* JobHandle() const noexcept { return job_.handle; }

    bool CanPrepare() const noexcept
    {
        return phase_ == CallPhase::Idle || phase_ == CallPhase::Collected ||
               phase_ == CallPhase::Retired || phase_ == CallPhase::Cancelled;
    }

    // In lease mode both lists are recorded before either is submitted, so the
    // output recording follows the input recording directly instead of
    // following the producer's submission. Past Begin the runtime owns the
    // ordering, and EnqueueHip is the very next call on that same queue.
    bool CanRecordOutputsNow() const noexcept
    {
        return leases_ ? phase_ == CallPhase::ProducerRecorded
                       : CanRecordOutputs(phase_);
    }

    bool CanEnqueueHipNow() const noexcept
    {
        return leases_ ? phase_ == CallPhase::RecordingBegun
                       : CanEnqueueHip(phase_);
    }

    bool Succeed()
    {
        lastError_.clear();
        lastStatus_ = static_cast<int32_t>(LMXXF_NR_OK);
        return true;
    }

    bool Refuse(std::string_view detail)
    {
        lastError_.assign(detail);
        lastStatus_ = static_cast<int32_t>(LMXXF_NR_FAILED);
        return false;
    }

    bool Fail(int32_t status, std::string_view operation)
    {
        lastError_.assign(operation);
        lastError_ += " status=";
        lastError_ += std::to_string(status);
        if (api_.GetLastError != nullptr) {
            char buffer[512]{};
            if (api_.GetLastError(buffer, static_cast<uint32_t>(sizeof(buffer))) ==
                static_cast<int32_t>(LMXXF_NR_OK) && buffer[0] != '\0') {
                lastError_ += ": ";
                lastError_ += buffer;
            }
        }
        lastStatus_ = status;
        return false;
    }

    const LmxxfNrApi& api_;
    void* context_{};
    // Set from LMXXF_NR_CREATE_FLAG_RECORDING_LEASES at construction. It
    // changes two orderings and removes two calls, so it is not a hint.
    bool leases_{};
    CallPhase phase_{CallPhase::Idle};
    LmxxfNrJob job_{};
    std::string lastError_;
    int32_t lastStatus_{static_cast<int32_t>(LMXXF_NR_OK)};
};

} // namespace amd_lmxxf
