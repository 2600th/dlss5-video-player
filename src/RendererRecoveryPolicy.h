#pragma once

#include <windows.h>

#include <utility>

// Rebuilding the renderer after the GPU became unusable.
//
// The recovery path used to rebuild into the SAME HWND. DXGI allows one
// flip-model swapchain per window, and the renderer being retired may still
// own one: D3D12RendererDeleter deliberately keeps a renderer alive when its
// bounded GPU drain does not complete, because deleting it would release
// resources its command lists are still reading and the NGX feature
// underneath them. DrainForRetirement also short-circuits on an already
// latched failure and answers TimedOut / WaitFailed / EventRegistrationFailed
// - none of which is Completed or DeviceRemoved - so the old swapchain
// routinely survives.
//
// So CreateSwapChainForHwnd answered DXGI_ERROR_INVALID_CALL, the media
// unloaded, and the user was shown "the GPU has been lost" by the one code
// path whose whole purpose is to survive that. Every other renderer-swap path
// in the player - EnableUpscaling, CreateRendererCandidate - already creates a
// fresh child window first. Only recovery reused, and it is the path nobody
// exercises by hand.
//
// The four operations are injected so the ordering can be asserted without a
// device, a display or a message loop, the same way WaitForGPUFenceTeardown is
// tested in D3D12FenceWait.h.
namespace renderer_recovery {

enum class Outcome {
    Rebuilt,
    WindowCreationFailed,
    RendererInitFailed,
};

struct Result {
    Outcome outcome{};
    // The window the caller now owns: the new one once it exists, otherwise
    // the one it came in with. Callers tear this down on failure, so handing
    // back the wrong one leaks a window or destroys a live swapchain's.
    HWND window{};
    bool oldWindowDestroyed{};
};

// `createWindow`   -> HWND, null when it could not be created.
// `destroyWindow`  (HWND)
// `releaseRetiring`-> bool: true when the retiring renderer was actually
//                    destroyed, false when it was retained alive. A retained
//                    renderer still holds its swapchain, so its window must
//                    outlive it.
// `initialize`     (HWND) -> bool
template <class CreateWindow, class DestroyWindow, class ReleaseRetiring, class Initialize>
Result Rebuild(HWND retiringWindow,
               CreateWindow&& createWindow,
               DestroyWindow&& destroyWindow,
               ReleaseRetiring&& releaseRetiring,
               Initialize&& initialize)
{
    const HWND fresh = createWindow();
    if (!fresh) {
        // Nothing has been retired yet, so the caller still has a usable old
        // window to report the failure through.
        return Result{Outcome::WindowCreationFailed, retiringWindow, false};
    }

    // Released before the new device is created rather than after: the old
    // renderer is already unusable, and holding both means holding two full
    // sets of render targets at once on a GPU that just failed a wait.
    const bool released = releaseRetiring();
    bool destroyed = false;
    if (released && retiringWindow) {
        destroyWindow(retiringWindow);
        destroyed = true;
    }

    if (!initialize(fresh)) return Result{Outcome::RendererInitFailed, fresh, destroyed};
    return Result{Outcome::Rebuilt, fresh, destroyed};
}

} // namespace renderer_recovery
