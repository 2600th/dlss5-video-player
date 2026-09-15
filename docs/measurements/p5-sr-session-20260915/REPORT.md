# P5 — live SuperResolution session evidence

**Date:** 2026-09-15 · **Machine:** `-PRANSHUL-`, Windows 11 Pro 10.0.26200, RTX 4080 SUPER
driver `32.0.16.1047` (610.47) plus Intel UHD 770 · **Tree:** `62491c6` in the worktree
`C:/Users/User/Documents/GitHub/dlss5-p5sr` (left in place; see [Artefacts](#artefacts)).

Every `file:line` citation below is against `62491c6`. Concurrent work was editing `src/` in
the main checkout while this was measured, so line numbers may have moved there; the numbers
quoted are the ones in the worktree the sessions were built from.

## Verdict

**The SR branch is proven live. Nothing was missing, and nothing had to be staged.** Two real
player sessions on a 1080p clip with `[Playback] SuperResolution=1 / UpscaleHeight=1440`
created and evaluated a DLSS SuperResolution feature at `input=1920x1080 output=2560x1440`
for 600+ frames, and NVOFA hardware flow was the motion guide in that SR renderer.

**The per-axis ratio the handoff asked for does not exist in the shipped player, by
construction:** the SR path passes `preserveSource=true` (`src/main.cpp:2286`), so the DLSS
backend sets `m_renderW=sourceW; m_renderH=sourceH` (`src/DLSSBackend.cpp:169-170`), so a
real SR session's DLSS input *is* the decoded frame and the ratio is exactly `1,1`. The
honest closure is therefore: **SR session proven live; the per-axis scaling path is exercised
only where the decoded size differs from the DLSS input, which no shipped configuration
produces** (all six renderer call sites enumerated in
[Why the ratio is 1,1](#why-the-ratio-is-11)).

**The previously reported blocker was a misread log line.** `NGXLoadLibrary: 126` is NGX's
app-local probe for the NGX *core*, which it always tries first and which no application is
supposed to ship; the same log file then resolves the core from the driver store and reports
success. It is an expected, benign step, not a failure.

**Measured, not argued:** the pre-P5 parent build (`4bfd287~1`) drove the same SR session and
initialised NVOFA in the same SR renderer, so P5 changed no observable live-SR behaviour on
this geometry — the pre-P5 equality gate already admitted this session
([Pre-P5 arm](#pre-p5-arm-measured-not-inferred)).

## 1. What the SR loader wants, and from where

`DLSSBackend::Initialize` calls `NVSDK_NGX_D3D12_Init_with_ProjectID`
(`src/DLSSBackend.cpp:67-71`). That call is serviced by the statically linked NGX loader,
which needs three different things:

| # | File | Directory it is wanted in | Who provides it |
|---|---|---|---|
| 1 | `_nvngx.dll`, then `nvngx.dll` (the **NGX core**) | first beside the executable (`build-upscaling/Release/`), then wherever `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore` → `FullPath` points | the **NVIDIA driver**, never the application |
| 2 | `nvngx_dlss.dll` (the **SR feature snippet**) | beside the executable | already staged by the build: `CMakeLists.txt:186-189` copies `external/DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll` to `$<TARGET_FILE_DIR:DLSSVideoPlayer>/nvngx_dlss.dll` |
| 3 | `neural-runtime/` (unrelated to SR, but gates startup) | `neural-runtime/` beside the executable | `tools/stage_runtime.ps1` + `packaging/runtime-lock.json`; the player refuses to start with an incomplete layout (`src/main.cpp:784-799`) |

**The error reported when the core is absent beside the executable, and what happens next.**
Verbatim from this session's `ngx_logs/nvsdk_ngx.log`
(archived as `ngx-core-load-direct.log`):

```
[2026-09-15 10:29:28] [NGXLoadLibrary:326] error: failed to load NGXCore: 126 (C:\Users\User\Documents\GitHub\dlss5-p5sr\build-upscaling\Release\_nvngx.dll)
[2026-09-15 10:29:28] [NGXLoadLibrary:326] error: failed to load NGXCore: 126 (C:\Users\User\Documents\GitHub\dlss5-p5sr\build-upscaling\Release\nvngx.dll)
[2026-09-15 10:29:28] [NGXGetPathUsingQAI:147] Attempting to read from Parameters\NGXCore\NGXPath
[2026-09-15 10:29:28] [NGXGetPathUsingQAI:171] Path to driverStore found using QAI: C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispsi.inf_amd64_7cc7738d8e428783
[2026-09-15 10:29:28] [NGXLoadCoreLibrary:279] Loading C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispsi.inf_amd64_7cc7738d8e428783\_nvngx.dll succeeded
```

Line 3 is `ERROR_MOD_NOT_FOUND` for a file the application is not meant to have; line 7 is the
load that matters. The player's own log copies lines 3-4 because the NGX logging callback
forwards any message containing "error" (`src/DLSSBackend.cpp:49-61`), which is why they are
visible in `DLSSVideoPlayer.log` while the success line is not.

### Where the NGX core exists on this machine

| Location searched | Result |
|---|---|
| `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore` | `FullPath = C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispsi.inf_amd64_7cc7738d8e428783`, `Installed = 0x1` |
| driver store | `_nvngx.dll`, `nvngx.dll`, `nvngx_dlssg.dll` present in that folder; `_nvngx.dll` is the one the loader takes |
| pinned SDK `external/DLSS/lib/Windows_x86_64/{rel,dev}` | `nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssg.dll` only — no NGX core, as expected |
| `external/runtime/` (12 staged files) | no NGX core |
| `packaging/runtime-lock.json` | 12 entries: `dxgi.dll`, `nvngx_dlss.dll`, `nvngx_dlssnr.dll`, `renodx-dlss5.addon64`, `sl.common.dll`, `sl.dlss.dll`, `sl.dlss_g.dll`, `sl.dlss_nr.dll`, `sl.interposer.dll`, `sl.nis.dll`, `sl.pcl.dll`, `sl.reflex.dll` — no NGX core, and none is needed |

**No staging step was performed or required**, and `tools/stage_runtime.ps1` was not modified.
It staged its 12 locked files unchanged (digests in the console output match the lock).

One incidental find, because it explains a second error code seen in older logs: the main
checkout's built `neural-runtime/` directories contain a 102,912-byte `nvngx.dll`
(sha256 `49a7248…`) that is **not** in the lock and is **not** produced by
`tools/stage_runtime.ps1` — my freshly staged `neural-runtime/` has exactly the 12 locked
files plus `NeuralWorker.exe` and the two ReShade inis, with no `nvngx.dll`. It is not the
driver's `nvngx.dll` (488,816 bytes, sha256 `3f59858…`). Where it comes from is not
established here; its effect is that NGX reports `-2146885623` (a trust failure) for the
app-local probe in those directories instead of `126` (not found), before resolving the core
from the driver store exactly as above.

## 2. The live SR session

Built in the worktree, Release, no tests:

```
cmake -S . -B build-upscaling -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF \
  -DDLSS_SDK=.../dlss5-p5sr/external/DLSS -DFFMPEG_STAGED_DIR=.../external/ffmpeg/bin \
  -DNVOF_SDK=.../external/nvof -DYOUTUBE_STAGED_DIR=.../external/youtube
cmake --build build-upscaling --config Release --parallel
powershell -File tools/stage_runtime.ps1 -InputDirectory external/runtime -Destination build-upscaling/Release/neural-runtime
cp packaging/ReShade.ini packaging/ReShadePreset.ini build-upscaling/Release/neural-runtime/
```

Configure reported `NVIDIA Optical Flow SDK headers found at …/external/nvof; hardware
optical flow enabled.` — the arm assertion that this binary can use NVOFA at all.
`external/` payloads were **copied** with `robocopy` (1.404 GB), never junctioned.

Clip: `docs/media/neural-comparison-demo.mp4` looped ×6 with `-stream_loop 5 -c copy` into
`work/demo-loop.mp4` — 1920x1080, 30 fps, 135.6 s, so a 28-second session never reaches EOF.
Profile: `player-profile-sr-on.ini` (`SuperResolution=1.000000`, `UpscaleHeight=1440.000000`).

**Post-P5 arm assertion:** `sha256(build-upscaling/Release/DLSSVideoPlayer.exe) =
02bcb0d018ef8494721366f045103e309eddfc11c8b768eac69893f526da2ffd`, 4,506,624 bytes, and the
binary contains the compiled string `Motion guide backend: NVOFA hardware flow on the
decoded` (1 match) — i.e. the P5 code is in the executable that was launched.

### Run A — direct launch, `--safe-mode` (SR only, no neural job)

`session-direct-sr-safe-mode.log`, 10:29:27-10:29:55. SR needs no keystroke: it is restored
from the ini at load (`src/main.cpp:2246,2321-2323`). Lines, in order, with the second
renderer being the SR one:

```
[10:29:28.983] NGX capability: SuperSampling.Available=1
[10:29:28.983] SR range query quality=2 result=1 optimal=1707x960 min=1280x720 max=2560x1440
[10:29:28.983] NGX input policy: source=1920x1080 optimal=1707x960 range=1280x720..2560x1440 selected=1920x1080 output=2560x1440
[10:29:28.983] NGX initialized. Deferred raw CreateFeature armed: input=1920x1080 output=2560x1440
[10:29:29.040] NVOFA ready: 1920x1080 on a 2x2 grid (960x540 vectors, S10.5 = 1/32 px), perf=FAST, cost=on, direction=both, round-trip gate armed, global flow=on.
[10:29:29.040] Motion guide backend: NVOFA hardware flow on the decoded 1920x1080 frame, vectors scaled by 1,1 into the 1920x1080 DLSS input.
[10:29:29.049] DLSS resource contract ready: … Output=R16G16B16A16_FLOAT UAV 2560x1440, …
[10:29:29.099] RAW NGX D3D12 CreateFeature SUCCESS: feature=SuperSampling input=1920x1080 output=2560x1440 flags=MVLowRes|AutoExposure; direct hook-visible contract
[10:29:29.101] RAW NGX EvaluateFeature_C SUCCESS #1 color=1920x1080 depth=1920x1080 mv=1920x1080 output=2560x1440 jitter=(0,0) reset=1
[10:29:29.128] Playback SR enabled: 1920x1080 -> 2560x1440
[10:29:39.096] RAW NGX EvaluateFeature_C SUCCESS #300 … output=2560x1440 … reset=0
[10:29:49.096] RAW NGX EvaluateFeature_C SUCCESS #600 … output=2560x1440 … reset=0
```

Three independent facts make this an SR session rather than a relabelled 1080p one: the
`output=2560x1440` in `CreateFeature`/`EvaluateFeature`, the SR renderer's own
`DLSS resource contract … Output=… UAV 2560x1440`, and `Playback SR enabled`, which the
player only logs after the candidate renderer has rendered a frame **and**
`LastFrameUsedDLSS()` returned true (`src/main.cpp:2294-2306`). The earlier
`Motion guide backend:` line at 10:29:28.834 belongs to the pre-SR 1920x1080 renderer, whose
resource contract shows `Output=… UAV 1920x1080`.

### Run B — `tools/verification/player_session.ps1`, shipped defaults (neural addon on)

```
powershell -File tools/verification/player_session.ps1 \
  -Player build-upscaling/Release/DLSSVideoPlayer.exe -Media work/demo-loop.mp4 \
  -Sessions 1 -FreshProfile Never -DropRenderCache -RunSeconds 15 \
  -OutJson …/session-harness.json -Note "…"
```

`-FreshProfile Never` is load-bearing: `First`/`Each` delete `DLSSVideoPlayer.ini`, which is
where `SuperResolution=1` lives. The session (`session-harness.session1.log`, 10:31:01) is
byte-for-byte equivalent in its SR lines: `SR range query quality=2 result=1 optimal=1707x960
min=1280x720 max=2560x1440`, `NVOFA ready: 1920x1080 …`, `Motion guide backend: NVOFA
hardware flow on the decoded 1920x1080 frame, vectors scaled by 1,1 into the 1920x1080 DLSS
input.`, `RAW NGX D3D12 CreateFeature SUCCESS … output=2560x1440`, `Playback SR enabled:
1920x1080 -> 2560x1440`, and `EvaluateFeature_C SUCCESS #300` ten seconds later — with the
neural pre-render path active rather than safe mode, so this is the shipped default
configuration.

**The harness itself failed at its own gate, and that is a real blocker for anything
input-driven.** `session-harness.json`, `outcome.exitCode = 14`:

```
"failure": "the neural toggle could not be injected: SendInput refused the chord, Win32 error 5;
 the foreground window is none - the session manager reports this session locked
 (WTSSessionInfoEx SessionFlags = WTS_SESSIONSTATE_LOCK), so injected input goes nowhere"
```

I read the same source before launching anything: `WTSQuerySessionInformationW`
(`WTSSessionInfoEx`, class 25) returned `level=1 sessionFlags=0` — `WTS_SESSIONSTATE_LOCK`.
**The desktop was locked for the whole of this work.** It did not block the SR evidence,
because SR is restored from the ini at load and needs no injected input; it does block the
neural toggle, so no neural-frame or pace number is claimed here.

## 3. Why the ratio is `1,1`

`PlanHardwareFlow` (`src/OpticalFlowNvof.h:174-188`) asks the engine for the decoded size and
returns `motionScaleX = renderW/sourceW`, `motionScaleY = renderH/sourceH`, where
`renderW/renderH` is the DLSS input chosen by `DLSSBackend` and pushed into the resolve pass
(`src/D3D12Renderer.cpp:527-528`, `828-830`). A ratio other than `1,1` therefore requires a
renderer whose DLSS input differs from its decoded frame. Every call site:

| Call site | Output passed | Quality / `preserveSource` | DLSS input | Ratio |
|---|---|---|---|---|
| `src/main.cpp:2241` `LoadOriginal` | decoder size (`:2236`) | DLAA (`:2237`), `preserveSource=false` | `renderW=outputW` (`DLSSBackend.cpp:171-172`) = source | `1,1` |
| `src/main.cpp:2285-2286` `EnableUpscaling` — **the SR path** | 2560x1440 / 3840x2160 | MaxQuality, **`preserveSource=true`** | `renderW=sourceW` (`DLSSBackend.cpp:169-170`) | `1,1` |
| `src/main.cpp:3877` neural/live playback load | decoder size | DLAA | `= source` | `1,1` |
| `src/main.cpp:4065` network attach | `configuration.outputWidth/Height`, built at `:3986` from `ow,oh` = decoder size (`:3981`) with DLAA (`:3982`) | DLAA, `preserveSource=false` | `= source` | `1,1` |
| `src/NeuralPreflightProbe.cpp:65-66` preflight probe | probe size = probe size | DLAA | `= source` | `1,1` |
| `src/OfflineNeuralRenderer.cpp:1823` worker | `(w,h,w,h)` | DLAA | `= source` | `1,1` |
| `tests/UpscalingGpuSmoke.cpp:40-41,103-104` | SR target | MaxQuality, `preserveSource=true` | `= source` | `1,1` |

There is also no user-reachable route around that table:

- `--quality` was removed and now fails argument parsing with an explanatory message
  (`src/main.cpp:696-697`), so nothing can select a non-DLAA quality on a non-`preserveSource`
  renderer.
- `--output` survives only as `AppOptions::maxW/maxH`, which reach
  `ResolveNeuralRenderDefaults` (`src/main.cpp:4720-4722`) and then
  `PrepareYouTubeMedia`, where both parameters are `[[maybe_unused]]`
  (`src/main.cpp:3976`); the helper that once turned them into a renderer output,
  `OutputForAspect` (`src/main.cpp:2413-2416`), has **no callers**.
- The `preserveSource` search in `DLSSBackend` can shrink the *output* until the source fits
  the runtime's advertised input range (`DLSSBackend.cpp:118-149`), but it never changes the
  input: `m_renderW=sourceW` regardless.

This is consistent with the design comment in `src/OpticalFlowNvof.h:147-157` and with
`src/D3D12Renderer.h:368-369` ("Both are 1 unless this is a Super Resolution session") — the
scaling exists for the case where the DLSS input is a resample of the decoded frame. What the
session shows is that the shipped SR path is not that case: it hands the decoded frame to NGX
unresampled, which is the whole point of `preserveSource`.

**Consequence for the handoff.** The acceptance evidence it named — "grep the same
`Motion guide backend:` line for a scale other than `1,1`"
(`docs/measurements/gpu-readback-20260914/REPORT.md:461-464`) — was unobtainable on any
machine with this source tree, not just on this one. The live evidence that *is* obtainable,
and now exists, is that a real SR session runs NVOFA on the decoded frame and reports its
conversion factor explicitly.

## 4. Coverage of the per-axis scaling, and the part nothing reaches

| Layer | Coverage |
|---|---|
| the numeric plan, `PlanHardwareFlow` | `tests/RenderSettingsTests.cpp:555-597`, `hardware_flow_runs_at_the_decoded_size_and_scales_only_for_super_resolution` (registered at `:632`, ctest target `RenderSettingsTests`, `CMakeLists.txt:250`). It asserts identity `1,1`; a downscaled DLSS input `1280/1920, 720/1080`; an upscaled one `1.5,1.5`; an **anisotropic** one `1280/1920, 1.0`; and that a zero dimension refuses with the scale left at 1. |
| the engine bound that keeps the CPU matcher | `tests/RenderSettingsTests.cpp:599-612` (`FlowGeometrySupported`) |
| the GPU consumer — `motion*MotionScale` in the resolve pass (`src/NvofResolveShader.h:32,72`), constants at `src/D3D12Renderer.cpp:828-830` | **compile-only.** `tests/UpscalingTests.cpp:60-72` compiles `PSNvofMotion` and asserts nothing about its arithmetic. |

**Finding, stated rather than smoothed:** the shader-side multiply by a *non-unit*
`MotionScale` has never executed — not in a test (no test runs that pixel shader), and not in
a session (every live configuration supplies `1,1`, per the table above). The numeric factor
that would be handed to it is well tested; the multiply that would consume it is exercised
only at the identity, where it is a no-op. Residual risk is therefore confined to the shader
path and is exactly as large as the first future configuration that resamples the decoded
frame into a differently sized DLSS input; the honest way to retire it is a GPU test that
drives `RenderFrame` with `renderW != sourceW` and checks the emitted motion texture, which
does not exist today.

## 5. Pre-P5 arm, measured not inferred

P5's stated premise was that "runtime SR silently uses the CPU estimator". The pre-P5 gate was
`if(m_sourceW==m_renderW&&m_sourceH==m_renderH&&m_nvof.Initialize(...))`
(`src/D3D12Renderer.cpp:520` at `4bfd287~1`), and the session above logs those two sizes as
equal — so the gate should have admitted it. That was checked rather than assumed.

Arm assertions (compiled strings prove which binary ran; the log line proves what it did):

| Arm | exe sha256 | `Motion guide backend:` in exe | `NVOFA ready:` in exe |
|---|---|---|---|
| post-P5 `62491c6`, `build-upscaling/Release` | `02bcb0d0…2ffd` | 1 | 1 |
| pre-P5 `4bfd287~1` (`eab9bc2`), `build-pre-p5/Release` | `e1990fe6…4fbc` | **0** | 1 |

The pre-P5 arm was configured with the same flags, staged with the same 12 locked files, given
the same ini and the same clip, and driven the same way (`session-pre-p5-arm.log`, 10:34:49):

```
[10:34:50.170] SR range query quality=2 result=1 optimal=1707x960 min=1280x720 max=2560x1440
[10:34:50.170] NGX input policy: source=1920x1080 optimal=1707x960 range=1280x720..2560x1440 selected=1920x1080 output=2560x1440
[10:34:50.228] NVOFA ready: 1920x1080 on a 2x2 grid (960x540 vectors, S10.5 = 1/32 px), perf=FAST, cost=on, direction=both, round-trip gate armed, global flow=on.
[10:34:50.237] DLSS resource contract ready: … Output=R16G16B16A16_FLOAT UAV 2560x1440, …
[10:34:50.285] RAW NGX D3D12 CreateFeature SUCCESS: feature=SuperSampling input=1920x1080 output=2560x1440 …
[10:34:50.311] Playback SR enabled: 1920x1080 -> 2560x1440
[10:35:10.280] RAW NGX EvaluateFeature_C SUCCESS #600 … output=2560x1440 … reset=0
```

**Observation:** both arms initialise hardware flow in the SR renderer, at the same geometry,
with the same engine parameters. The pre-P5 build has no `Motion guide backend:` line at all
(it did not exist yet), so the observable is the `NVOFA ready:` line emitted by
`OpticalFlowNvof::Initialize` (`src/OpticalFlowNvof.cpp:385`) only when the engine actually
came up — and it appears in the SR renderer of both arms, bracketed by the same
`NGX input policy … output=2560x1440` and `DLSS resource contract … UAV 2560x1440`.

So on this geometry P5 changed no live SR behaviour: what it added is the generalisation (any
decoded-vs-input size), the per-session log line that names the estimator, and the refusal
reason when the engine does not come up. The perf/quality cliff the item was written against
is not present in this build's SR path, because that path never resampled the decoded frame
in the first place.

## Artefacts

All paths relative to this directory unless stated.

| File | What it is |
|---|---|
| `session-direct-sr-safe-mode.log` | Run A, full player log (77 lines), SR on, `--safe-mode` |
| `ngx-core-load-direct.log` | Run A `ngx_logs/nvsdk_ngx.log` — the 126 probes and the driver-store load |
| `session-harness.json` | Run B `player_session.ps1` record, `outcome.exitCode = 14` (desktop locked) |
| `session-harness.session1.log` | Run B player log (74 lines), shipped defaults, SR on |
| `session-pre-p5-arm.log` | Pre-P5 arm player log, SR on |
| `ngx-core-load-pre-p5.log` | Pre-P5 arm `nvsdk_ngx.log` |
| `player-profile-sr-on.ini` | the profile the sessions ran with, as the player rewrote it on exit |

**Worktree left behind:** `C:/Users/User/Documents/GitHub/dlss5-p5sr` at `62491c6`
(detached). It holds `build-upscaling/` (post-P5 arm, staged and runnable), `build-pre-p5/`
(pre-P5 arm), `work/demo-loop.mp4`, and a copied `external/`. Tracked files are unmodified;
`git status` shows only the two untracked build/work directories. No `src/`, `tests/`,
`tools/` or `docs/` file outside this directory was touched, and nothing was committed.

## What this contradicts elsewhere in the docs (for the integrator, not edited here)

1. `docs/IMPLEMENTATION-HANDOFF-quality-and-performance.md:48` and `:18` — "the player-root SR
   runtime is unstaged, so `[Playback] SuperResolution=1` fails at `NGXLoadLibrary`". The
   player-root SR runtime (`nvngx_dlss.dll`) is staged by `CMakeLists.txt:186-189`; the
   `NGXLoadLibrary: 126` lines are the NGX core's app-local probe, and the core then loads
   from the driver store (excerpt in section 1). SR runs.
2. `docs/DLSS5_VIDEO_ROADMAP.md:625-627` and
   `docs/measurements/gpu-readback-20260914/REPORT.md:453-464` — same misreading, plus the
   prediction that a live SR session would show "a scale other than `1,1`". It shows `1,1`,
   and section 3 shows why no shipped configuration can show anything else.
