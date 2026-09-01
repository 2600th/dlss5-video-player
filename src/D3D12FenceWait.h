#pragma once

#include <windows.h>

#include <cstdint>
#include <utility>

namespace d3d12_renderer_detail {

inline constexpr DWORD TeardownFenceWaitMilliseconds=2000;

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
        if(elapsed>=TeardownFenceWaitMilliseconds)return FenceWaitResult::TimedOut;
        const DWORD remaining=static_cast<DWORD>(TeardownFenceWaitMilliseconds-elapsed);
        const DWORD waitResult=wait(remaining);
        if(waitResult!=WAIT_OBJECT_0&&waitResult!=WAIT_TIMEOUT)return FenceWaitResult::WaitFailed;
        const uint64_t completed=completedValue();
        if(completed==UINT64_MAX)return FenceWaitResult::DeviceRemoved;
        if(completed>=value)return FenceWaitResult::Completed;
        if(waitResult==WAIT_TIMEOUT)return FenceWaitResult::TimedOut;
    }
}

template<class Signal, class CompletedValue, class RegisterEvent, class Wait>
FenceWaitResult WaitForGPUFenceTeardown(uint64_t value,
                                        Signal&& signal,
                                        CompletedValue&& completedValue,
                                        RegisterEvent&& registerEvent,
                                        Wait&& wait)
{
    const ULONGLONG started=GetTickCount64();
    if(FAILED(signal(value)))return FenceWaitResult::SignalFailed;
    return WaitForGPUFenceCompletion(value,started,
        std::forward<CompletedValue>(completedValue),
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
