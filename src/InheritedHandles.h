#pragma once

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <vector>

// The handles a child process inherits, and no others (P1.28).
//
// CreateProcess with bInheritHandles=TRUE and no list hands the child every
// inheritable handle alive in the player at that instant, not only the pipe
// ends it was meant to get. A seek during an export spawned the playback
// ffmpeg while the export's probe had its stdout pipe open, so that ffmpeg
// held the probe's write end too: the probe's drain waited for end-of-file
// until the playback child died, and a cancel joined that drain on the UI
// thread. The milder form was a ten-second audio stall.
//
// PROC_THREAD_ATTRIBUTE_HANDLE_LIST is the documented fix. Every spawn names
// its own standard handles here; duplicates and nulls are dropped, because
// the attribute refuses a list that repeats a handle, and NUL often stands in
// for two of the three. An empty list means nothing is inherited at all.
//
//     InheritedHandles inherit{si.hStdInput, si.hStdOutput, si.hStdError};
//     STARTUPINFOEXW startup{}; startup.StartupInfo = si;
//     startup.StartupInfo.cb = sizeof(startup);
//     startup.lpAttributeList = inherit.AttributeList();
//     CreateProcessW(..., inherit.InheritHandles(), flags | inherit.CreationFlags(),
//                    ..., &startup.StartupInfo, &info);
//
// PolicyTests reads every CreateProcessW in src/ and refuses one that
// inherits without a list.
class InheritedHandles {
public:
    explicit InheritedHandles(std::initializer_list<HANDLE> handles)
    {
        for (const HANDLE handle : handles) {
            if (!handle || handle == INVALID_HANDLE_VALUE) continue;
            if (std::find(m_handles.begin(), m_handles.end(), handle) == m_handles.end()) m_handles.push_back(handle);
        }
        if (m_handles.empty()) { m_ready = true; return; }
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (!bytes) return;
        m_storage.resize(bytes);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_storage.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) return;
        m_list = list;
        m_ready = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, m_handles.data(),
                                            m_handles.size() * sizeof(HANDLE), nullptr, nullptr) != FALSE;
    }
    ~InheritedHandles() { if (m_list) DeleteProcThreadAttributeList(m_list); }
    InheritedHandles(const InheritedHandles&) = delete;
    InheritedHandles& operator=(const InheritedHandles&) = delete;

    // False when the list could not be built. The spawn is refused rather than
    // made without one: that is the leak this exists to close.
    bool Ready() const { return m_ready; }
    BOOL InheritHandles() const { return m_handles.empty() ? FALSE : TRUE; }
    DWORD CreationFlags() const { return m_list ? EXTENDED_STARTUPINFO_PRESENT : 0; }
    LPPROC_THREAD_ATTRIBUTE_LIST AttributeList() const { return m_list; }

private:
    std::vector<HANDLE> m_handles;
    std::vector<std::byte> m_storage;
    LPPROC_THREAD_ATTRIBUTE_LIST m_list = nullptr;
    bool m_ready = false;
};
