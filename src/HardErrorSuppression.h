#pragma once

#include <windows.h>

// CreateProcessW on a corrupt, truncated or non-PE image raises a modal Windows
// hard-error dialog on the calling thread ("cannot start or run due to
// incompatibility with 64-bit versions of Windows") and does not return until
// somebody dismisses it. Every executable this player launches - ffmpeg,
// ffprobe, yt-dlp, deno, NeuralWorker - is a file beside the exe that a partial
// download or a half-extracted archive can leave invalid, and the spawns happen
// on worker threads, so the dialog would hang acquisition or a render with no
// diagnostic anywhere. Suppressing the hard error does not hide the failure:
// CreateProcessW still returns FALSE with ERROR_BAD_EXE_FORMAT, which every
// call site already logs and fails closed on.
//
// The mode is per thread and restored on scope exit, so nothing else in the
// process loses its own error handling.
class ScopedHardErrorSuppression {
public:
    ScopedHardErrorSuppression()
    {
        m_restore = SetThreadErrorMode(SEM_FAILCRITICALERRORS, &m_previous) != FALSE;
    }

    ~ScopedHardErrorSuppression()
    {
        if (m_restore) SetThreadErrorMode(m_previous, nullptr);
    }

    ScopedHardErrorSuppression(const ScopedHardErrorSuppression&) = delete;
    ScopedHardErrorSuppression& operator=(const ScopedHardErrorSuppression&) = delete;

private:
    DWORD m_previous{};
    bool m_restore{false};
};
