#pragma once

#include <windows.h>

// Keeps Windows from sleeping while a render, an export or a conversion runs
// (P1.39). A power request rather than SetThreadExecutionState: the request
// belongs to its handle, not to the calling thread, and `powercfg /requests`
// names it with the reason. System only - the display may still turn off.
// Construct one for the life of the job; a failure to create it costs the job
// nothing but the guarantee.
class KeepAwake {
public:
    explicit KeepAwake(const wchar_t* reason) noexcept
    {
        REASON_CONTEXT context{};
        context.Version = POWER_REQUEST_CONTEXT_VERSION;
        context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        context.Reason.SimpleReasonString = const_cast<wchar_t*>(reason);
        m_request = PowerCreateRequest(&context);
        if (m_request == INVALID_HANDLE_VALUE) m_request = nullptr;
        if (m_request && !PowerSetRequest(m_request, PowerRequestSystemRequired)) {
            CloseHandle(m_request);
            m_request = nullptr;
        }
    }
    ~KeepAwake()
    {
        if (!m_request) return;
        PowerClearRequest(m_request, PowerRequestSystemRequired);
        CloseHandle(m_request);
    }
    KeepAwake(const KeepAwake&) = delete;
    KeepAwake& operator=(const KeepAwake&) = delete;

    bool Active() const noexcept { return m_request != nullptr; }

private:
    HANDLE m_request{};
};
