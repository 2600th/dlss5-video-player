# RTX 4080 SUPER on 0.21.2+: the round-trip gate measured, and the cold stack timed

Verified 14 September 2026 on an RTX 4080 SUPER, on the machine that produced the
0.21.x work, to answer three questions that the two waves of changes landed that
day left open: does the forward/backward flow gate come up on real silicon and
does it improve anything measurable, does the scene-cut over-reset found by the
CPU sweep also happen in a real render, and what does the cold-start stack a
neural toggle pays for actually cost per phase.

Three results. The gate comes up at its top rung, and against the pre-gate tree no
metric moves by a percentage point - reproducibly, since the renders are
bit-identical between repeats, so the small deltas are signal. They do not favour
it: on the one clip with real disocclusion it steadies the motion field but raises
invented appearance by 0.92 points, and false motion improves on the fast pan
alone. The over-reset is real in a real render: `history_resets = 6` on a clip with
four hard cuts. And the helper side of the cold start is **2.1-2.6 s** over two
renders, of which NGX init and feature arm are 95 % in both, which is the cost the
persistent-helper item exists to remove.

## Environment

| Item | Value | Source |
| --- | --- | --- |
| OS | `Windows 11 Pro` / `10.0.26200` | session |
| GPU | `NVIDIA GeForce RTX 4080 SUPER`, DXGI driver `32.0.16.1047` (610.47) | `Win32_VideoController`, worker preflight |
| GPU (second adapter) | `Intel(R) UHD Graphics 770`, driver `32.0.101.6129` | `Win32_VideoController` |
| Adapter used | LUID `0x1600b`, vendor `0x10de`, 16047 MiB, `m_gpuArch = 0x190` | device adapter log line, NGX log |
| Repo commit | `16b6f3c` (wave two integrated) | `git rev-parse HEAD` |
| Baseline commit | `1c41427` (before the gate) | second worktree |
| Build | Release x64, VS 17 2022 / MSVC 19.44, `--clean-first`; zero lines matching `warning` or `error` in the full rebuild log | build log |
| Runtime | 12 locked files staged, `nvngx_dlssnr.dll` `310.8.SF-v2` | `tools/stage_runtime.ps1` |
| Encoder | `hevc_nvenc` | run results |

The driver is at the neural floor (610.47), not the recommended pin (616.64), so
nothing here speaks for the pinned driver; it speaks for the floor.

## Gates

- `ctest --test-dir build-upscaling -C Release`: **13/13 passed**, 30.26 s after a
  clean rebuild.
- Each measured clip rendered **twice**; the harness reports `deterministic: true`
  per clip and every metric is identical between repeats.
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

## What the gate is worth: a real effect, smaller than a percentage point

Four labelled clips, 465 source frames, rendered through the real neural path on
two trees and scored by the same `analyze.py`: `16b6f3c` with the gate armed and
`1c41427` before it existed. Identical corpus, identical profile (`baseline`,
`mv=1,depth=1`), identical runtime files.

**This is a tree comparison, not a gate toggle.** `1c41427..16b6f3c` is two waves
of work, and four of those commits touch files in the render path, so the
attribution rests on reading them: `6528eb5` is the gate itself (both flow
directions, global flow, the round-trip rejection in the resolve pass);
`25952ef` adds scene-cut tally increments guarded on `!repeat` and changes no
threshold and no decision; `8b9897a` adds a log line and one DXGI enumeration to
device creation and leaves the selection loop alone; `df50c3a` stamps cold-start
phases. Only the first can move a pixel, but a reader should know the others were
in the build.

**There is no noise floor to hide in.** Each clip was rendered twice on the gate
side: `deterministic: true` per clip, and every metric below is identical to full
float precision between repeat 1 and repeat 2 (spread exactly 0.00000 on all
three metrics, all four clips). The pipeline is bit-reproducible, which is what
the harness's determinism digest exists to assert, so the deltas are signal.

| clip | false motion off → on | cell flips added off → on | temporal sigma added off → on |
| --- | --- | --- | --- |
| cuts-motion | 0.15080 → 0.16003 | 0.01993 → **0.01595** | −4.4955 → **−4.6029** |
| cuts-similar | 0.09968 → 0.10762 | 0.06173 → 0.06808 | −0.3871 → −0.3715 |
| flash-exposure | 0.10439 → 0.10726 | 0.03441 → 0.04114 | −0.7067 → **−0.8079** |
| pan-fast | 0.18074 → **0.17673** | 0.01230 → 0.01307 | −1.3496 → −1.2806 |

Every delta is reproducible and every delta is small: 0.29 to 0.92 percentage
points on false motion, 0.08 to 0.67 on flips, 0.016 to 0.107 on sigma. No clip
improves on all three, and no metric improves on all clips. Per clip: on
`cuts-motion` - the only one with genuine disocclusion - flips fall 20 % relative
and sigma improves, while false motion *rises* by 0.92 points; on `pan-fast` false
motion is the one that falls and the other two drift the wrong way;
`flash-exposure` gains sigma and loses the other two; `cuts-similar` loses all
three. The single clip where the gate improves false motion is the fast pan, and
the clip it was argued for improves the two motion-field numbers instead. Note
also what the false-motion metric is: output pixels on cells the source held
static, so it answers "did the pass invent appearance" and not "did the vector
field improve", and a zeroed vector changes what NR is given as much as a wrong
one does.

**The scale to read those against is the carrier, not repeat variance.** The
`intensity-0` control - the same tree and the same gate, with the model's
relighting at zero, so decode, guides, the feature-18 pass and the NVENC
re-encode all still happen - reports false motion **0.09153** on `cuts-motion`,
flips added **−0.00228** and sigma added **−3.4116** (PSNR 32.41 against
baseline's 22.89). So of baseline's 0.16003, about 9.2 points are the carrier
inventing appearance on static cells and 6.85 are the neural pass itself. The
gate's 0.92-point rise on that clip is 13.5 % of the share attributable to NR - so
it is neither noise nor plumbing, and on the metric the item was argued for it
moves the wrong way on the clip the item was argued for.

**Decision: the gate stays, unchanged and unflagged.** It is a refusal - a cell
the engine contradicts itself about emits no motion instead of a confident wrong
one - so it cannot introduce a vector that was not measured, and on the
disocclusion clip it buys the two motion-field numbers (flips −20 % relative,
sigma −0.107) that describe the field's stability. What it does not buy is less
invented appearance: false motion rises on three of four clips, including that
one. That is a real result and an uncomfortable one, and it is why this record
refuses to call the gate a quality win. Settling it needs real footage - grain,
motion blur, a real dissolve, a camera that occludes - and the corpus's only
real-footage clip (`faces`) needs a fixture that is not on this machine. If it
does not pay there either, the case for removing it is the honest next move.

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

From the protocol v5 timeline messages of two renders on this machine: **A** is
the `cuts-motion` render above at `16b6f3c`; **B** is a segmented 1080p30 render
taken in the slice worktree while the instrument was being written, so it is the
same instrumentation from a different build and a different job. The player-side
phases are null in both because
the harness drives `NeuralWorker.exe` directly, and `firstOutput` is null in A
because a single-file job never rotates a segment - both are the absence ladder
behaving as designed, not missing data.

| Phase | A | B | Handoff's estimate | What it covers |
| --- | --- | --- | --- | --- |
| `helperStart` | **104.4 ms** | 99 ms | ~700 ms scanned | process creation → entry point: AV scan of the runtime tree, the loader, and the ReShade proxy with them |
| `runtimeReady` | **10.2 ms** | 10 ms | ~410 ms | entry → add-on contract verified, adapter queried, MF up, window created |
| `neuralInit` | **1338.5 ms** | 1847 ms | ~1510 ms | render entry → source open, D3D12 device, NGX init |
| `featureArm` | **680.5 ms** | 641 ms | ~630 ms | → feature 18 created, evaluated, inline interception armed |
| `firstOutput` | absent (single file) | 2726 ms | ~1780 ms | → first finalized segment file |
| helper total to armed | **2133.6 ms** | 2597 ms | | |

Unlike the render metrics, these are not reproducible, and the two samples are not
a controlled pair: **B** came from the slice worktree's own build of the same
instrumentation, on a different, segmented job, and `neuralInit` brackets the
source open as well as NGX. So the 509 ms gap is not attributable to run-to-run
variance alone - but neither sample can stand as an acceptance number, and a check
written against 2 s needs several samples from one build on one clip.

Two things the handoff's table gets structurally wrong, independent of the
spread. ReShade proxy plus add-on load is not 0.41 s beside the loader, because
the proxy *is* the helper's `dxgi` import and resolves before the entry point:
its cost is inside `helperStart`, which is 0.10 s in total. And the antivirus
figure: `helperStart` is ~0.10 s in both runs, but this machine's exclusion state
could not be read (`Get-MpPreference` needs administrator; real-time protection
is reported **enabled**), and both samples were taken after the runtime tree had
already been executed repeatedly in the same session. So 0.10 s is a warm-cache
number, not evidence that a scanned first-touch install is cheap - the 3 s
scanned acceptance budget stands unchallenged by this record.

What survives intact, and is the reason the item exists: **NGX init plus feature
arm is 2.02 s of A's 2.13 s and 2.49 s of B's 2.60 s - 95 % in both - and every
one of those milliseconds is per-process.** A resident helper removes them from
the second toggle onward. The render itself, for comparison, is 5.85 ms/frame at
1080p.

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
