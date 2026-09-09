# RTX 4080 SUPER verification and universal-runtime cutover

Verified 9 September 2026 on a Windows 11 Pro 26200 machine with an
NVIDIA GeForce RTX 4080 SUPER (device 0x2702, driver 610.47 / DXGI 32.0.16.1047),
Visual Studio 2022 Build Tools 17.14 (MSVC 19.44), CMake from the Build Tools.

## Inputs

Every build input was fetched fresh and every locked runtime file reproduced
byte-for-byte from public releases; the lock's `provenance` names each source
and [Building](BUILDING.md) lists them. The ReShade proxy `dxgi.dll`
(6.8.0.2155, SHA-256 `0CEE63F9…94F7`) is `ReShade64.dll` extracted from the
official `ReShade_Setup_6.8.0_Addon.exe`, which is a ZIP container.
`tools/stage_runtime.ps1 -ValidateOnly` accepted all 12 files against the
v0.15.0 lock before any change was made.

## Baseline: v0.15.0 source, previous RTX 40-targeted runtime

- Release build, 12/12 CTest suites passed in 30.16 s.
- `MediaGpuSmoke` (strict evidence chain, photo/GIF/video, five export
  formats each): `overall=PASS`. The add-on log reports
  `Running on NVIDIA GeForce RTX 4080 SUPER Driver 610.47`,
  `signed DLSSNR 310.8.0 D3D12 runtime initialized`, feature 18 created via
  the signed snippet and `inline feature 18 evaluation succeeded (count=60)`.
- Player live session on the 30 s 1080p30 demo clip: **failed twice in two
  attempts** with `retry-exhausted frames=0/0 verified=0 resets=1 retries=3`.
  The worker log shows the one-frame hook probe succeeding, the 60-frame
  preroll evaluating at about 15 ms/frame, then
  `Recreating RAW NGX DLSS feature for ReShade/RenoDX hook capture` on the
  first captured frame and `GPU fence wait failed` exactly 2.00 s later, with
  no further add-on log lines. The 640x360 smoke passes because its job starts
  at frame 0 and has no preroll in flight when the recreate fires.

## Cause and fixes

1. `DLSSBackend::RecreateFeature` released the NGX feature while up to two
   earlier frames' evaluations (three pipelined slots) were still executing.
   The RenoDX add-on hooks `ReleaseFeature` and tears down its NR worksets;
   the queue never completed. The renderer now drains the queue
   (`WaitGPUForContinuedUse`) before any feature release.
2. The 2 s teardown fence budget also governed every per-frame wait and the
   post-`CreateFeature` flush. Render-path waits now have a 20 s budget
   (`RenderFenceWaitMilliseconds`); teardown keeps 2 s. Device removal is
   detected through the fence's `UINT64_MAX` sentinel regardless of budget.

## After the fix, universal runtime `310.8.SF-v2`

- Clean rebuild, no warnings; 12/12 CTest suites (25.77 s) including the new
  pace, forecast and fence-budget tests; `PolicyTests: all assertions passed`.
- `MediaGpuSmoke` with the previous RTX 40 build and with the SF-v2 build,
  same machine, same session: both `overall=PASS`; render+capture stage mean
  4.00 vs 3.97 ms (video), 3.52 vs 3.22 ms (GIF). The universal build costs
  nothing on Ada.
- Player live session, same demo clip, `D` pressed 3.4 s in: `798/798`
  frames, `verified=798`, `feature18=armed lock=ok failure=none`, 14 segments
  published at a steady 60 frames per 0.93 s, playback attached with 4.0 s
  buffered, `presented=798 dropped=0`, session concatenated into the ordinary
  cache entry. Toggling off logged
  `Measured neural render pace: 1920x1080 at 15.3055 ms/frame over 738 frames
  (1.22112x the reference GPU)` and wrote `[NeuralPace]` to the INI.
- Final smoke on the shipped configuration: `overall=PASS`, add-on log
  `signed runtime sha256 6EB209E7…3927 (custom runtime accepted)`.

## Limits

- Blackwell (RTX 50) was verified in v0.15.0 on the previous runtime and has
  not been re-verified on the universal build.
- Turing (RTX 20) and Ampere (RTX 30) are enabled by policy and by the
  runtime's published support, but no such hardware was available here. They
  lack native FP8 tensor math and are reported several times slower; the
  policy makes no keep-up forecast for them until a machine has measured
  itself.
- The 4080 numbers come from one 30 s 1080p30 source; 1440p and 4K were not
  measured on this machine. At the measured 1.22x, the model forecasts 4K30
  at 0.97x real time.
