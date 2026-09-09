#pragma once

#include <windows.h>

#include <cstdint>
#include <utility>

namespace d3d12_renderer_detail {

// Two budgets: teardown stays short so a hung device cannot hold the helper
// open, while frame waits during rendering must tolerate the slowest supported
// GPU. The whole DLSS/NR pipeline for one frame can take well over a second on
// a Turing or Ampere part, and up to three frames are in flight. A genuine hang
// is still reported promptly: device removal signals the fence event and
// surfaces as the UINT64_MAX sentinel below, independent of the budget.
inline constexpr DWORD TeardownFenceWaitMilliseconds=2000;
inline constexpr DWORD RenderFenceWaitMilliseconds=20000;

enum class FenceWaitResult {
    Completed,
    DeviceRemoved,
    SignalFailed,
    EventRegistrationFailed,
    WaitFailed,
    TimedOut,
};

template<class DeviceRemovedReason>
FenceWaitResult ClassifyFenceWaitFailure(FenceWaitResult result,
                                         DeviceRemovedReason&& deviceRemovedReason)
{
    if(result==FenceWaitResult::Completed||result==FenceWaitResult::DeviceRemoved)return result;
    return FAILED(deviceRemovedReason())?FenceWaitResult::DeviceRemoved:result;
}

template<class CompletedValue, class RegisterEvent, class Wait>
FenceWaitResult WaitForGPUFenceCompletion(uint64_t value,
                                          ULONGLONG started,
                                          DWORD budgetMilliseconds,
                                          CompletedValue&& completedValue,
                                          RegisterEvent&& registerEvent,
                                          Wait&& wait)
{
    const uint64_t beforeRegistration=completedValue();
    if(beforeRegistration==UINT64_MAX)return FenceWaitResult::DeviceRemoved;
    if(beforeRegistration>=value)return FenceWaitResult::Completed;
    if(FAILED(registerEvent(value)))return FenceWaitResult::EventRegistrationFailed;
    for(;;){
        const ULONGLONG elapsed=GetTickCount64()-started;
        if(elapsed>=budgetMilliseconds)return FenceWaitResult::TimedOut;
        const DWORD remaining=static_cast<DWORD>(budgetMilliseconds-elapsed);
        const DWORD waitResult=wait(remaining);
        if(waitResult!=WAIT_OBJECT_0&&waitResult!=WAIT_TIMEOUT)return FenceWaitResult::WaitFailed;
        const uint64_t completed=completedValue();
        if(completed==UINT64_MAX)return FenceWaitResult::DeviceRemoved;
        if(completed>=value)return FenceWaitResult::Completed;
        if(waitResult==WAIT_TIMEOUT)return FenceWaitResult::TimedOut;
    }
}

// Signals `value` on the queue and waits for it within `budgetMilliseconds`.
template<class Signal, class CompletedValue, class RegisterEvent, class Wait>
FenceWaitResult WaitForGPUFenceDrain(uint64_t value,
                                     DWORD budgetMilliseconds,
                                     Signal&& signal,
                                     CompletedValue&& completedValue,
                                     RegisterEvent&& registerEvent,
                                     Wait&& wait)
{
    const ULONGLONG started=GetTickCount64();
    if(FAILED(signal(value)))return FenceWaitResult::SignalFailed;
    return WaitForGPUFenceCompletion(value,started,budgetMilliseconds,
        std::forward<CompletedValue>(completedValue),
        std::forward<RegisterEvent>(registerEvent),std::forward<Wait>(wait));
}

template<class Signal, class CompletedValue, class RegisterEvent, class Wait,
         class DeviceRemovedReason>
FenceWaitResult WaitForGPUFenceDrain(uint64_t value,
                                     DWORD budgetMilliseconds,
                                     Signal&& signal,
                                     CompletedValue&& completedValue,
                                     RegisterEvent&& registerEvent,
                                     Wait&& wait,
                                     DeviceRemovedReason&& deviceRemovedReason)
{
    const auto result=WaitForGPUFenceDrain(
        value,budgetMilliseconds,std::forward<Signal>(signal),std::forward<CompletedValue>(completedValue),
        std::forward<RegisterEvent>(registerEvent),std::forward<Wait>(wait));
    return ClassifyFenceWaitFailure(
        result,std::forward<DeviceRemovedReason>(deviceRemovedReason));
}

template<class Signal, class CompletedValue, class RegisterEvent, class Wait>
FenceWaitResult WaitForGPUFenceTeardown(uint64_t value,
                                        Signal&& signal,
                                        CompletedValue&& completedValue,
                                        RegisterEvent&& registerEvent,
                                        Wait&& wait)
{
    return WaitForGPUFenceDrain(value,TeardownFenceWaitMilliseconds,
        std::forward<Signal>(signal),std::forward<CompletedValue>(completedValue),
        std::forward<RegisterEvent>(registerEvent),std::forward<Wait>(wait));
}

template<class Signal, class CompletedValue, class RegisterEvent, class Wait,
         class DeviceRemovedReason>
FenceWaitResult WaitForGPUFenceTeardown(uint64_t value,
                                        Signal&& signal,
                                        CompletedValue&& completedValue,
                                        RegisterEvent&& registerEvent,
                                        Wait&& wait,
                                        DeviceRemovedReason&& deviceRemovedReason)
{
    const auto result=WaitForGPUFenceTeardown(
        value,std::forward<Signal>(signal),std::forward<CompletedValue>(completedValue),
        std::forward<RegisterEvent>(registerEvent),std::forward<Wait>(wait));
    return ClassifyFenceWaitFailure(
        result,std::forward<DeviceRemovedReason>(deviceRemovedReason));
}

} // namespace d3d12_renderer_detail
