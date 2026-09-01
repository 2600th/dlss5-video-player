#pragma once

#include <windows.h>

#include <cstdint>
#include <utility>

namespace d3d12_renderer_detail {

inline constexpr DWORD TeardownFenceWaitMilliseconds=2000;

enum class FenceWaitResult {
    Completed,
    SignalFailed,
    EventRegistrationFailed,
    WaitFailed,
    TimedOut,
};

template<class Signal, class CompletedValue, class RegisterEvent, class Wait>
FenceWaitResult WaitForGPUFenceTeardown(uint64_t value,
                                        Signal&& signal,
                                        CompletedValue&& completedValue,
                                        RegisterEvent&& registerEvent,
                                        Wait&& wait)
{
    if(FAILED(std::forward<Signal>(signal)(value)))return FenceWaitResult::SignalFailed;
    if(std::forward<CompletedValue>(completedValue)()>=value)return FenceWaitResult::Completed;
    if(FAILED(std::forward<RegisterEvent>(registerEvent)(value)))return FenceWaitResult::EventRegistrationFailed;
    const DWORD waitResult=std::forward<Wait>(wait)(TeardownFenceWaitMilliseconds);
    if(waitResult==WAIT_OBJECT_0)return FenceWaitResult::Completed;
    if(waitResult==WAIT_TIMEOUT)return FenceWaitResult::TimedOut;
    return FenceWaitResult::WaitFailed;
}

} // namespace d3d12_renderer_detail
