# Correction 1 report — atomic activation and bounded GPU teardown

## Result

DONE

- Base: `c30dfb0573a993278087898a6ca239c9b84e5edf`
- Branch: `codex/release-ui-youtube-impl`
- Required commit message: `fix: bound YouTube renderer handoff`

## Root causes

1. `InstallPreparedYouTube` used a three-stage helper that installed the prepared state, stopped only the old audio, and activated prepared audio. The swapped-out decoder, renderer, and render window were retired only after activation, while the candidate render window was not shown until still later. A renderer destructor blocked in `WaitGPU` with new audio active and the validated frame hidden.
2. `D3D12Renderer::WaitGPU` ignored the results of queue `Signal`, fence `SetEventOnCompletion`, and `WaitForSingleObject`, and used `INFINITE`. Signal/event/wait failure or a fence that never completed could therefore block candidate rollback or old-renderer destruction indefinitely.

## TDD evidence

### RED

- Ordering test first failed to compile with MSVC `C2672`: the production helper accepted only three callbacks while the required production path needed four explicit phases.
- After the smallest compile-enabling wrong-order overload, direct `PolicyTests` produced the intended behavioral RED:
  - candidate visibility was false at activation;
  - all four old-owner retirement counts were still zero at activation;
  - prepared audio had already been unpaused;
  - the handoff event order differed from `install -> visible -> retire all -> clocks/audio activation`.
- The same RED run exercised every required fence failure result:
  - signal failure returned the wrong result;
  - event-registration failure returned the wrong result and did not register the event;
  - wait failure returned the wrong result and observed `INFINITE`;
  - timeout returned the wrong result and observed `INFINITE`.
- The first attempted RED command did not count as evidence because `cmake` was absent from `PATH`; all recorded RED/GREEN runs used the Visual Studio-bundled CMake executable explicitly.

### GREEN

- Focused Release `PolicyTests` target rebuild succeeded.
- Direct focused execution reported `PolicyTests: all assertions passed` and `Resolver symlink coverage: exercised`.

## Implementation

- Expanded `CommitPreparedAudioHandoff` to enforce four phases: install prepared ownership, make the candidate visible, retire all old ownership, then activate.
- Reordered `InstallPreparedYouTube` so prepared decoder/audio/renderer/window ownership is installed while audio remains paused; the already-rendered candidate window is shown; old audio, swapped-out decoder, old renderer, and old window are each retired once; playback state and clocks are then established before prepared audio is unpaused.
- Added the internal `D3D12FenceWait.h` helper. It exposes no player setting and uses one fixed `2000 ms` teardown bound.
- `D3D12Renderer::WaitGPU` now checks queue signal, event registration, and wait outcomes through that helper. Failure or timeout produces only `GPU teardown wait failed.` and returns to teardown safely.
- Added deterministic policy coverage for handoff ordering/exact retirement and for signal failure, event-registration failure, wait failure, and timeout.

## Fresh verification

- Clean Release configure/build: PASS using Visual Studio 2022 Build Tools and the repository's shared DLSS SDK checkout.
- Full CTest from the clean build: PASS, `2/2` tests and `0` failures.
  - `PolicyTests`: PASS in `6.31 sec`.
  - `ReleaseApiCompileTests`: PASS in `0.04 sec`.
- Direct clean `PolicyTests`: PASS; all assertions passed.
- Deterministic candidate-handoff smoke: PASS under an outer `30000 ms` process bound, exit `0` in approximately `6.6 sec`.
- `git diff --check`: PASS (exit `0`; Git emitted only its existing LF-to-CRLF working-copy notice for `CMakeLists.txt`).
- Process cleanup scan after tests: `DLSSVideoPlayer=0`, `PolicyTests=0`, `ffmpeg=0`, `ffprobe=0`, `yt-dlp=0`, `deno=0`.
- Log scan: no `GPU teardown wait failed.` entries and no URL-like text in workspace log files.
- Scope check: no resolver, UI, examples, packaging, localization, helper/cache policy, or unrelated renderer behavior changed.
- Prohibited runs: did not run the Impeccable detector or 4K/re-hook stress.

## Workspace note

Pre-existing untracked build directories were left untouched. Verification created `build-correction-1-clean2`; an earlier configure-only `build-correction-1-clean` directory remains untracked after its default SDK lookup failed before compilation. Neither directory is included in the commit.
