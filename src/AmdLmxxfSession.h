#pragma once

#include "AmdLmxxfApi.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace amd_lmxxf {

// Serializes one runtime session's frame state machine. The API deliberately
// has no thread-local context, so the caller must also serialize calls across
// all jobs belonging to this Session.
class Session
{
public:
    Session(const LmxxfNrApi& api, void* context) noexcept
        : api_(api), context_(context) {}

    void* Context() const noexcept { return context_; }
    CallPhase Phase() const noexcept { return phase_; }
    const LmxxfNrJob& Job() const noexcept { return job_; }
    const std::string& LastError() const noexcept { return lastError_; }

    bool PrepareFrame(const LmxxfNrFrameInfo& info)
    {
        if (!CanPrepare()) return Refuse("PrepareFrame is out of order");
        job_ = MakeJob();
        const int32_t status = api_.PrepareFrame(context_, &info, &job_);
        if (status != static_cast<int32_t>(LMXXF_NR_OK)) return Fail(status, "PrepareFrame");
        if (job_.handle == nullptr) return Refuse("PrepareFrame returned a null job");
        phase_ = CallPhase::Prepared;
        lastError_.clear();
        return true;
    }

    bool RecordInputs(void* commandList)
    {
        if (!CanRecordInputs(phase_)) return Refuse("RecordInputs is out of order");
        const int32_t status = api_.RecordInputs(context_, &job_, commandList);
        if (status != static_cast<int32_t>(LMXXF_NR_OK)) return Fail(status, "RecordInputs");
        phase_ = CallPhase::ProducerRecorded;
        lastError_.clear();
        return true;
    }

    // Called after the producer command list was submitted on the session's
    // queue. It is host state, not an API call.
    bool ProducerSubmitted()
    {
        if (phase_ != CallPhase::ProducerRecorded)
            return Refuse("producer submission marker is out of order");
        phase_ = CallPhase::ProducerSubmitted;
        return true;
    }

    bool EnqueueHip(void* commandQueue)
    {
        if (!CanEnqueueHip(phase_)) return Refuse("EnqueueHip is out of order");
        const int32_t status = api_.EnqueueHip(context_, &job_, commandQueue);
        if (status != static_cast<int32_t>(LMXXF_NR_OK)) return Fail(status, "EnqueueHip");
        phase_ = CallPhase::HipEnqueued;
        lastError_.clear();
        return true;
    }

    bool RecordOutputs(void* commandList)
    {
        if (!CanRecordOutputs(phase_)) return Refuse("RecordOutputs is out of order");
        const int32_t status = api_.RecordOutputs(context_, &job_, commandList);
        if (status != static_cast<int32_t>(LMXXF_NR_OK)) return Fail(status, "RecordOutputs");
        phase_ = CallPhase::ConsumerRecorded;
        lastError_.clear();
        return true;
    }

    bool Retire()
    {
        if (!CanRetire(phase_)) return Refuse("Retire is out of order");
        const int32_t status = api_.Retire(context_, &job_);
        if (status != static_cast<int32_t>(LMXXF_NR_OK)) return Fail(status, "Retire");
        phase_ = CallPhase::Retired;
        lastError_.clear();
        return true;
    }

    bool Cancel()
    {
        if (IsTerminal(phase_)) return Refuse("Cancel is out of order");
        const int32_t status = api_.CancelUnsubmitted(context_, &job_);
        if (status != static_cast<int32_t>(LMXXF_NR_OK)) return Fail(status, "CancelUnsubmitted");
        phase_ = CallPhase::Cancelled;
        lastError_.clear();
        return true;
    }

    bool ResetHistory()
    {
        const int32_t status = api_.ResetHistory(context_);
        if (status != static_cast<int32_t>(LMXXF_NR_OK)) return Fail(status, "ResetHistory");
        lastError_.clear();
        return true;
    }

private:
    bool CanPrepare() const noexcept
    {
        return phase_ == CallPhase::Idle || phase_ == CallPhase::Retired ||
               phase_ == CallPhase::Cancelled;
    }

    bool Refuse(std::string_view detail)
    {
        lastError_.assign(detail);
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
        return false;
    }

    const LmxxfNrApi& api_;
    void* context_{};
    CallPhase phase_{CallPhase::Idle};
    LmxxfNrJob job_{};
    std::string lastError_;
};

} // namespace amd_lmxxf
