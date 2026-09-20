#pragma once

#include <windows.h>

#include <chrono>
#include <thread>

// Windows SDK 10.0.17134 (Win10 1803) introduced the flag; older headers lack the
// name but the kernel still honours (or ignores) the value, and the plain-timer
// fallback below covers the rest.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// std::this_thread::sleep_for rounds up to the ~15.6ms default Windows
// scheduler tick unless a high-resolution timer backs the wait; nothing in
// this process calls the process-wide timeBeginPeriod, so a 1-2ms poll sleep
// was measuring p95 13.7ms instead. A waitable timer with the high-resolution
// flag (Win10 1803+) gets sub-ms accuracy without touching global timer
// resolution. Creating the handle is measurable, so an owner keeps one rather
// than making it per wait.
//
// The player's message loop uses this too. It is deliberately NOT the
// swapchain's frame-latency waitable: asking DXGI for that means creating the
// swapchain with DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, which the
// neural runtime's add-on does not survive - see D3D12Renderer::CreateDevice.
class PrecisionSleeper {
public:
    PrecisionSleeper() {
        m_timer = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!m_timer) m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ~PrecisionSleeper() { if (m_timer) CloseHandle(m_timer); }
    PrecisionSleeper(const PrecisionSleeper&) = delete;
    PrecisionSleeper& operator=(const PrecisionSleeper&) = delete;

    void SleepMs(int ms) {
        if (!m_timer) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); return; }
        // Relative due time in 100ns units; negative means relative-to-now.
        LARGE_INTEGER due; due.QuadPart = -(static_cast<LONGLONG>(ms) * 10000);
        if (!SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            return;
        }
        WaitForSingleObject(m_timer, INFINITE);
    }

private:
    HANDLE m_timer = nullptr;
};

// One handle per thread, paid once per thread rather than once per wait.
inline void SleepPreciseMs(int ms) {
    thread_local PrecisionSleeper sleeper;
    sleeper.SleepMs(ms);
}
