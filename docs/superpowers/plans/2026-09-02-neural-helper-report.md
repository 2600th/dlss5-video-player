# Neural helper implementation report

Date: 2026-09-02

## Scope delivered

Implemented Task 1's isolated neural-worker interface in `src/NeuralWorker.h`
and `src/NeuralWorker.cpp`, plus the helper entry point in
`src/NeuralWorkerMain.cpp` and focused tests in `tests/NeuralWorkerTests.cpp`.
The player-side wrapper:

- validates its executable/request before launch;
- creates a restricted inheritable metadata pipe, launches the helper hidden
  and suspended, assigns it to a `KILL_ON_JOB_CLOSE` job, then resumes it;
- consumes versioned binary progress/result metadata with fixed maximum sizes;
- accepts a successful result only when every neural-evidence, frame-count,
  duration and evaluation invariant is present and valid; and
- kills the job and waits up to two seconds on cancellation. It has no cache
  promotion dependency, so malformed, failed, truncated and cancelled results
  cannot publish a cache entry.

The helper configures only `neural-runtime/ReShade.ini`, creates its own hidden
render window, pumps Win32 messages while the real `OfflineNeuralRenderer`
runs, and owns COM/Media Foundation initialization. If repairing the local
ReShade configuration requires a new proxy load, it exits with code 75 so its
hook-free parent can relaunch it once after full exit, before GPU initialization.
This prevents overlapping ReShade log ownership. Child FFmpeg encoder processes remain descendants
of the parent-owned job.

`MediaPipeline.cpp` and `VideoDecoder.cpp` have the requested narrow lookup:
when their executable directory is exactly `neural-runtime`, FFmpeg/FFprobe
come only from its explicit package parent and do not fall back to PATH.

## TDD record

1. Wrote tests first: missing helper failure, bounded cancellation of a real
   child process, rejection of a truncated metadata record, complete
   preservation of a valid result/progress record, and direct normal/restarted
   helper command-envelope coverage.
2. RED observed with the new CMake target: configuration failed because
   `src/NeuralWorker.cpp` was absent (`Cannot find source file`). This was the
   intended missing implementation failure.
3. Added the minimal worker wrapper/protocol implementation. The first GREEN
   build completed `NeuralWorkerTests`, and CTest passed all 1/1 focused tests
   in 0.46 seconds.
4. Corrected the production helper parser to its actual 16-argument launch
   contract and bounded the restarted form's loop at its final option/value
   pair. The shared parser envelope is directly tested for both normal and
   restarted forms. Fresh focused verification passed: `NeuralWorkerTests`
   built and CTest reported 1/1 passed in 0.43 seconds.
5. Added opt-in `--real-worker <workerexe> <sourcevideo> <outputvideo>
   <width> <height> <fps> <seconds>` mode. It invokes the packaged helper,
   prints throttled progress plus full result/evidence, and returns success
   only for a valid helper render. It is intentionally not part of regular
   CTest and has not been run here because it needs the supplied GPU fixture.

## Required CMake helper target

Use these sources for the helper executable:

`src/NeuralWorkerMain.cpp`, `src/NeuralWorker.cpp`,
`src/OfflineNeuralRenderer.cpp`, `src/MediaPipeline.cpp`,
`src/VideoDecoder.cpp`, `src/D3D12Renderer.cpp`, `src/DLSSBackend.cpp`,
`src/TemporalGuides.cpp`, `src/RuntimePolicy.cpp`, `src/ReShadeConfig.cpp`,
and `src/UiLayout.cpp`.

Use the player target's normal include paths and link libraries, including
`d3d12 dxgi dxguid d3dcompiler mfplat mfreadwrite mfuuid ole32 shell32 shlwapi
comdlg32 comctl32 user32 gdi32 winmm dwmapi winhttp bcrypt nvsdk_ngx_d`, and
definitions `UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
DLSS_VIDEO_PLAYER_VERSION="${PROJECT_VERSION}"`. Package the executable to
`neural-runtime`; package policy/runtime staging remains owned by Task 3.

## Verification boundary

The focused wrapper tests never invoke ReShade, DXGI, GPU rendering, FFmpeg or
the actual packaged helper. A successful build/test therefore proves process
ownership/protocol validation only; real helper launch, ReShade isolation,
FFmpeg lookup and visual neural output still require the Task 3 package and
hardware gate. An attempted direct helper-target rebuild after the focused test
was blocked only by a concurrent shared-build lock on `DLSSBackend.obj`; the
parent build owns the final helper/link verification.
