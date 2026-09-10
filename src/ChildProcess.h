#pragma once

#include <windows.h>

// Keeps a failed child launch silent.
//
// CreateProcessW on a file that is not a valid Windows executable - a helper that was
// truncated mid-download, replaced, or is simply the wrong file - does not just fail.
// Windows raises a hard error first, and the shell puts up a modal "unsupported 16-bit
// application" box that waits for a click. In the player that lands in front of the
// user for something the launch site already reports properly; in the tests, which
// deliberately stage an invalid helper, it stops the run until somebody dismisses it.
//
// Inside this scope the call still fails, with ERROR_BAD_EXE_FORMAT, and every launch
// site here already handles that. Only the dialog is gone. The mode is per thread, so a
// launch on another thread keeps whatever mode it had.
class ChildProcessErrorModeScope {
public:
    ChildProcessErrorModeScope()
    {
        restore_ = SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previous_) != FALSE;
    }
    ~ChildProcessErrorModeScope()
    {
        if (restore_) SetThreadErrorMode(previous_, nullptr);
    }
    ChildProcessErrorModeScope(const ChildProcessErrorModeScope&) = delete;
    ChildProcessErrorModeScope& operator=(const ChildProcessErrorModeScope&) = delete;

private:
    DWORD previous_ = 0;
    bool restore_ = false;
};

// True unless the file is one Windows refuses to launch as an image.
//
// The error-mode scope above is not enough on its own: the invalid-image hard error is
// raised before the launching thread's mode is consulted, so the modal box still
// appeared for a staged helper that is not a PE file at all. Refusing the launch here
// keeps that dialog off the screen for good, and reports the same failure the launch
// sites already handle - CreateProcessW would have failed on this file anyway.
//
// Only a definitively bad image is refused. Any other reason the check cannot answer
// (the file is locked, or access is denied) falls through to CreateProcessW, which
// reports it exactly as before. A DOS or 16-bit image is refused too: those are the
// images that raise the dialog on 64-bit Windows.
inline bool ChildProcessImageIsLaunchable(const std::wstring& executable)
{
    DWORD binaryType = 0;
    if (!GetBinaryTypeW(executable.c_str(), &binaryType))
        return GetLastError() != ERROR_BAD_EXE_FORMAT;
    return binaryType == SCS_32BIT_BINARY || binaryType == SCS_64BIT_BINARY;
}
