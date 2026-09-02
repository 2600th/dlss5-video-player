# Simple Runtime Upscaling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Independent optional DLSS SR on cached neural/original playback.
**Architecture:** Existing offline NR in a hidden helper directory; hook-free player uses its existing NGX backend. Only job metadata crosses processes, and cached videos remain the media handoff.
**Tech Stack:** C++20, Win32, D3D12/NGX, existing FFmpeg, CMake/CTest.
**Spec:** `docs/superpowers/specs/2026-09-02-simple-runtime-upscaling-design.md`

## Global Constraints

- NR defaults on; SR off; FG off/unavailable. 1440p default output, 2160p selectable.
- Preserve original source dimensions, aspect ratio, caches, seek/EOF fixes and padded controls.
- No new runtime downloads, optical-flow dependency, Streamline integration, cache-schema change, commit, tag or publication.
- Work in the current feature checkout; preserve the existing uncommitted implementation. Do not discard or move it to a clean checkout.
- Keep previously delivered packages; use a separate local build directory for this implementation.
- No antivirus scans or security configuration changes.

### Task 1: Isolated offline helper

**Files:** New `src/NeuralWorker.h`, `src/NeuralWorker.cpp`, `src/NeuralWorkerMain.cpp`, `tests/NeuralWorkerTests.cpp`; targeted helper-location changes in `VideoDecoder.cpp` and `MediaPipeline.cpp`.
**Interfaces:** `NeuralRenderResult RunNeuralWorker(const std::filesystem::path& executable, const NeuralRenderRequest&, OfflineNeuralRenderer::ProgressCallback, std::stop_token)`; separate `wmain` helper invokes existing `OfflineNeuralRenderer::Run`.

- [x] Write failing tests: nonexistent helper fails; cancellation of a running test child is bounded; malformed/truncated results cannot be accepted; valid result preserves all verification fields.
- [x] Observe failures, then implement a versioned metadata pipe, quoted arguments, hidden suspended launch, kill-on-close job ownership, and strict result validation. Reuse the real offline renderer; only the helper creates its render window. Keep ReShade configuration local to the helper directory.
- [x] Resolve shared FFmpeg tools from the explicit helper-package parent when running in `neural-runtime`; do not search arbitrary user paths.
- [x] Build and run the new helper tests; review process lifetime and error paths.

### Task 2: Playback SR and UI

**Files:** `src/DLSSBackend.*`, `src/D3D12Renderer.*`, `src/main.cpp`, `src/AppMenu.*`, `src/Localization.h`, focused tests.
**Interfaces:** Add optional source-preserving SR mode to renderer/backend initialization. Add a pure target-selection helper with literal tests: 1920x1080 to 2560x1440; 1920x1080 to 3840x2160; no downscale for 3840x2160 to 1440p.

- [x] Write failing size/availability/menu/default tests, run them, then implement the source-preserving mode without changing the offline DLAA carrier.
- [x] Route cache misses through `RunNeuralWorker`; compute the unchanged runtime digest in the helper directory; remove player-side neural hooking. Reject old root-level proxy layouts clearly.
- [x] Add independent SR toggle and two output targets. Recreate a candidate renderer on a separate child window and swap it only after successful first SR evaluation. Preserve audio/position/pause and reset guides at discontinuities. On failure leave or restore ordinary playback and explain it.
- [x] Handle original and cached playback, view switches, seeks, EOF and source replacement. Guard commands during seeks/jobs; keep all menus/toolbars synchronized.
- [x] Run policy/UI/cache tests and review the integrated path.

### Task 3: Build, package and hardware verification

**Files:** `CMakeLists.txt`, `tools/package_release.ps1`, `tools/verify_package.ps1`, `src/ReleasePackagePolicy.h`, focused packaging assertions, README/architecture/changelog.

- [x] Build helper into `neural-runtime`, keep player root hook-free; stage existing locked NR files in helper directory and ordinary SR DLL in root. Update allowlist tests before package policy.
- [x] Build Release in `build-upscaling`; run all CTest suites, then repeat after final fixes. Run `git diff --check`.
- [x] Verify real 1080p-to-1440p SR, module isolation, cache reuse, toggles and seeking using the Resident Evil sample on the local RTX 5090; measure dropped frames and inspect output against ordinary scaling. Try 2160p separately.
- [x] Produce a distinct private local package only after applicable gates pass. Report any GPU/visual checks not run; do not claim RTX 40 testing.

## Progress

- Plan reviewed against approved spec: each task's interfaces match; Task 1 and 2 share only the worker function; Task 3 owns CMake/package wiring. All preserve the NR cache schema and current source-quality policy.
- Baseline build/tests passed before implementation. Final verification and
  measured hardware results: `2026-09-02-upscaling-verification.md`.
