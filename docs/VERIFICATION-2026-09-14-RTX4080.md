# RTX 4080 SUPER on 0.21.2+: the round-trip gate measured, and the cold stack timed

Verified 14 September 2026 on an RTX 4080 SUPER, on the machine that produced the
0.21.x work, to answer three questions that the two waves of changes landed that
day left open: does the forward/backward flow gate come up on real silicon and
does it improve anything measurable, does the scene-cut over-reset found by the
CPU sweep also happen in a real render, and what does the cold-start stack a
neural toggle pays for actually cost per phase.

Three results. The gate comes up at its top rung and changes the output by less
than this corpus can resolve - safe, unproven. The over-reset is real in a real
render: `history_resets = 6` on a clip with four hard cuts. And the helper side of
the cold start is **2.13 s**, of which NGX init and feature arm are 2.02 s, which
is the number the persistent-helper item exists to remove.

## Environment

| Item | Value | Source |
| --- | --- | --- |
| OS | `Windows 11 Pro` / `10.0.26200` | session |
| GPU | `NVIDIA GeForce RTX 4080 SUPER`, DXGI driver `32.0.16.1047` (610.47) | `Win32_VideoController`, worker preflight |
| GPU (second adapter) | `Intel(R) UHD Graphics 770`, driver `32.0.101.6129` | `Win32_VideoController` |
| Adapter used | LUID `0x1600b`, vendor `0x10de`, 16047 MiB, `m_gpuArch = 0x190` | device adapter log line, NGX log |
| Repo commit | `16b6f3c` (wave two integrated) | `git rev-parse HEAD` |
| Baseline commit | `1c41427` (before the gate) | second worktree |
| Build | Release x64, VS 17 2022 / MSVC 19.44, zero warnings | build log |
| Runtime | 12 locked files staged, `nvngx_dlssnr.dll` `310.8.SF-v2` | `tools/stage_runtime.ps1` |
| Encoder | `hevc_nvenc` | run results |

The driver is at the neural floor (610.47), not the recommended pin (616.64), so
nothing here speaks for the pinned driver; it speaks for the floor.

## Gates

- `ctest --test-dir build-upscaling -C Release`: **13/13 passed**, 29.43 s.
- `MediaGpuSmoke.exe external/ffmpeg/bin …/neural-runtime/NeuralWorker.exe …`:
  exit 0, `neural=1` with `verified` equal to `frames` on photo (1), animation
  (100) and video (24). Run twice, before and after wave two.
- `UpscalingGpuSmoke.exe <1080p clip> 1440`: 90 evaluations, 184.5 fps,
  `neuralGpuMs=0.674`, `peakLocalVramMiB=488`, `fenceWait=0`.

## The optical-flow engine comes up at its top rung

```
NVOFA ready: 1920x1080 on a 2x2 grid (960x540 vectors, S10.5 = 1/32 px),
perf=FAST, cost=on, direction=both, round-trip gate armed, global flow=on.
```

This is the first hardware observation of the capability ladder landed that day:
both prediction directions and the global flow estimate were accepted in one
`nvOFInit`, so no rung was given up, and the resolve pass has a reverse field to
gate on. Registration of the backward, backward-cost and global-flow surfaces,
and the global-flow readback's fence timing, are exercised by every frame of
every run below.

## What the gate is worth: nothing this corpus can see

Four labelled clips, 465 source frames, rendered twice through the real neural
path - once at `16b6f3c` with the gate armed, once at `1c41427` before it existed
- and scored by the same `analyze.py`. Identical corpus, identical profile
(`baseline`, `mv=1,depth=1`), identical runtime files.

| clip | false motion off → on | cell flips added off → on | temporal sigma added off → on |
| --- | --- | --- | --- |
| cuts-motion | 0.15080 → 0.16003 | 0.01993 → **0.01595** | −4.4955 → **−4.6029** |
| cuts-similar | 0.09968 → 0.10762 | 0.06173 → 0.06808 | −0.3871 → −0.3715 |
| flash-exposure | 0.10439 → 0.10726 | 0.03441 → 0.04114 | −0.7067 → **−0.8079** |
| pan-fast | 0.18074 → **0.17673** | 0.01230 → 0.01307 | −1.3496 → −1.2806 |

No metric moves consistently and no move exceeds one percentage point. The gate
wins the two numbers it was argued for on `cuts-motion` - the clip with real
disocclusion, where flips fall 20 % relative and sigma improves - and loses them
on clips built from patterns whose "motion" is a filter parameter. The honest
reading is that the effect is inside this corpus's noise: every run re-encodes
through `hevc_nvenc`, and the false-motion metric measures output pixels, not
vectors, so the encoder's own non-determinism sets a floor under it.

**Decision: the gate stays, unchanged and unflagged.** It is a refusal - a cell
the engine contradicts itself about emits no motion - so its risk is one-sided,
and the one clip with genuine occlusion is the one it helps. It is not evidence
of a quality win and must not be cited as one. What would settle it is real
footage: grain, motion blur, a real dissolve, and a camera that occludes. The
corpus's only real-footage clip (`faces`) needs a fixture that is not on this
machine.

## The scene-cut over-reset is real in a real render

`cuts-motion` concatenates five 1.5 s segments, so it has four hard cuts at
frames 45, 90, 135 and 180. The render reports:

- `history_resets: 6` - the first frame plus five accepted cuts.
- receipt scene-cut tally over the pipe: **4 strong, 1 weak, 0 suppressed**.
- the harness, replaying the criterion on the decoded source: detected
  `[45, 90, 91, 135, 180]`, precision 0.80, recall 1.00.

The extra reset is frame 91, one frame after the frame-90 cut, where the `life`
segment advances a whole automaton generation: residual 0.3739, overlap 0.1282.
It reaches the strong arm, and the strong arm is deliberately never debounced.
The same-day CPU sweep predicted exactly this from the decoded frames, and the
worker's own counters agree with it frame for frame - which is the first
end-to-end check that the harness mirror and the shipped C++ decide alike.

Two other classes confirmed on real renders: all three cuts in `cuts-similar`
(shots that share a luma histogram by construction) are **missed**, and
`flash-exposure` takes **two false positives** on a flash and an exposure step.
Neither threshold was changed; the sweep found no strictly better operating point
and seven labelled positives is not a mandate.

## The cold stack, per phase, measured

From the protocol v5 timeline message of the `cuts-motion` render. The player-side
phases are null because the harness drives `NeuralWorker.exe` directly, and
`firstOutput` is null because a single-file job never rotates a segment - both are
the absence ladder behaving as designed, not missing data.

| Phase | Measured | Handoff's estimate | What it covers |
| --- | --- | --- | --- |
| `helperStart` | **104.4 ms** | ~700 ms scanned | process creation → entry point: AV scan of the runtime tree, loader, ReShade proxy |
| `runtimeReady` | **10.2 ms** | ~410 ms | entry → add-on contract verified, adapter queried, MF up, window created |
| `neuralInit` | **1338.5 ms** | ~1510 ms | render entry → source open, D3D12 device, NGX init |
| `featureArm` | **680.5 ms** | ~630 ms | → feature 18 created, evaluated, inline interception armed |
| helper total | **2133.6 ms** | | |

Two corrections to the handoff's table fall out of this. The AV-scan window is
0.10 s here, not 0.7 s - this install is excluded, and the 98 MB tree is not the
cost anyone should optimise first. And ReShade proxy plus add-on load is 10 ms as
measured at the helper's own boundaries, because the proxy is resolved as the
helper's `dxgi` import *before* the entry point, so its cost is inside
`helperStart`, not beside it.

What survives intact is the item's premise: **NGX init plus feature arm is 2.02 s
of the 2.13 s the helper pays, 95 %, and every one of those milliseconds is
per-process.** A resident helper removes them from the second toggle onward. The
render itself, for comparison, is 5.85 ms/frame at 1080p.

## Render numbers

| clip | frames | wall s | first frame s | proc fps | neural GPU ms p50 / p95 / max | guide ms | peak VRAM MiB |
| --- | --- | --- | --- | --- | --- | --- | --- |
| cuts-motion | 225 | 8.46 | 4.54 | 99.2 | 5.850 / 6.506 / 7.679 | 2.739 | 993 |
| cuts-similar | 120 | 7.57 | - | 97.6 | 5.992 / - / - | - | 993 |
| pan-fast | 30 | 6.62 | - | 96.7 | 6.164 / - / - | - | 993 |
| flash-exposure | 90 | 7.01 | - | 98.2 | 5.865 / - / - | - | 993 |

Every run: `ok=true`, `failure=None`, `feature18_created`, `feature18_evaluated`,
`armed_before_capture`, `verified_neural_frames` equal to `frame_count`, zero
frame retries. The p50 of 5.85 ms at 1920x1080 is 4.8x the 1.223 ms receipt floor
for that geometry, so the floor is doing its job without being near the real
value.

## What this record does not establish

- **Not a pace prior.** `RenderPacePrior` for Ada is 1.22 from a 0.16.0 session;
  these are offline renders driven by the harness, not a live session with a
  playhead, so they do not re-measure it. The Ada row of the support matrix still
  wants a live 1080p30 session.
- **No player session.** The harness drives the helper directly, so nothing here
  exercises the player's toggle path, the attach, the receipt written to disk, the
  cold-start line in `DLSSVideoPlayer.log`, or the player-side phases. The
  toggle-to-picture total the persistent-helper item is judged on is therefore
  still unmeasured end to end.
- **Not the pinned driver.** 610.47, not 616.64.
- **No Optimus.** The adapter log line and its LUID comparison were observed on a
  desktop with a discrete card that was chosen correctly; the exports that matter
  on a hybrid laptop cannot be tested here.
- **Visual quality unjudged.** No face, OCR or blind comparison was run; the
  metrics above are temporal and motion-field statistics on synthetic content.
