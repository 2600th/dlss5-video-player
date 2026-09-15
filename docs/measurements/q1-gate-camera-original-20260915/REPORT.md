# Q1, the round-trip gate, on camera-original material — 15 September 2026, RTX 4080 SUPER

**Verdict: Q1's WIN survives on camera-original material — won.** With the gate isolated
to its rejection alone, the shipped build lowers false motion on **6 of 7** publisher-cut
clips (−2.1 % to −46.9 % relative), lowers the cell-flip rate on **7 of 7**, and lowers
added temporal σ on **4 of 7**; three clips improve all three at once. The one clip that
goes the other way is `orig-game-cuts`, at **+0.43 %** false motion (+0.00024 absolute).
Measured against the `shipped-intensity-0` carrier floor, the gate removes **10.9 % to
77.3 %** of the false motion the neural pass itself contributes on the six clips it wins,
and adds 1.8 % of it on the one it loses. Every number here was measured today; both arms
are bit-reproducible within themselves and provably different from each other.

This closes the item the previous wave called "the largest owed" one: Q1 was the only
quality verdict claiming a WIN, and it rested on NR-processed captures — the player's own
DLSS-NR output through a screen capture, two h264 generations and a lanczos upscale
(`docs/measurements/gate-real-footage-20260914/REPORT.md`). It had never been scored on
footage cut from publishers' own releases, because the gate is not a runtime option:
`GuideControls` (`src/GuideControls.h`) carries only `mv` and `depth`, so the A/B is two
**builds**. Two builds is what this is.

One difference from the earlier record worth stating up front: that measurement compared
the gate tree against a **pre-gate tree** — `1c41427..0556eb0`, two waves of commits, so
the attribution rested on reading them. This one changes the *rejection* and nothing else
(diff below), keeps `predDirection = BOTH`, `enableGlobalFlow` and the backward-flow
allocation exactly as shipped, and proves per arm that the arm is the arm it claims to be.

## Environment

| Item | Value | Evidence |
|---|---|---|
| GPU | NVIDIA GeForce RTX 4080 SUPER, 16047 MiB, driver `32.0.16.1047` (610.47) | preflight receipt in every `result.json` |
| CPU / OS | Intel Core i7-14700, Windows 11 Pro 10.0.26200 | session |
| Both trees | `62491c6`, detached, `git worktree add` from the main checkout | `git worktree list` |
| Build | Release x64, Visual Studio 17 2022, MSVC toolset `14.44.35207`, target `NeuralWorker`, `-DBUILD_TESTING=OFF`, `-DDLSS_SDK=<main>/external/DLSS` | configure + build logs |
| Runtime | the same 12 locked files staged into both trees from the main tree's `external/runtime`: ReShade `6.8.0.2155`, RenoDX `4.7` (build Sep 2 2026), add-on `0.2026.828.517` API 18, DLSS-NR `310.8.0` (`nvngx_dlssnr.dll` `310.8.2.0`), worker `0.21.2` | `tools/stage_runtime.ps1` ("Verified and staged 12 locked runtime files"), preflight receipts |
| Model store | digest `bb8af7a367236735…`, 188 files, 128 content-hashed, 1341985582 bytes — **identical on both arms** | preflight `modelStore` |
| Encoder | `hevc_nvenc` on all 50 runs | `result.json` |
| FFmpeg | the main tree's `external/ffmpeg/bin/{ffmpeg,ffprobe}.exe`, **copied** into each worktree (never junctioned — see `docs/IMPLEMENTATION-HANDOFF-quality-and-performance.md`, "Process note, learned the hard way") | `cp`, `corpus.py` / `analyze.py` resolve FFmpeg from their own tree |
| Timing | the GPU was shared with two other slices for this whole session | `wall_s`, `e2e_fps`, `proc_fps`, `gpu_ms_*` from these runs are CONTENDED and are not cited anywhere below |

## 1. Which estimator path actually ran, and which half of the gate that puts under test

**NVOFA hardware flow supplied the motion guide on every clip.** Each arm logs its
estimator choice once per session, and the two arms' estimator lines are identical:

```
q1on  Motion guide backend: NVOFA hardware flow on the decoded 1920x1080 frame,
      vectors scaled by 1,1 into the 1920x1080 DLSS input.
      NVOFA ready: 1920x1080 on a 2x2 grid (960x540 vectors, S10.5 = 1/32 px),
      perf=FAST, cost=on, direction=both, round-trip gate armed, global flow=on.

q1off Motion guide backend: NVOFA hardware flow on the decoded 1920x1080 frame,
      vectors scaled by 1,1 into the 1920x1080 DLSS input.
      NVOFA ready: 1920x1080 on a 2x2 grid (960x540 vectors, S10.5 = 1/32 px),
      perf=FAST, cost=on, direction=both, round-trip gate armed, global flow=on.
      Q1 measurement arm: round-trip rejection REMOVED in both the NVOFA resolve pass
      and the CPU estimator; estimation (BOTH directions, global flow, backward field)
      is the shipped one. Gate-off build for docs/measurements/q1-gate-camera-original-20260915.
```
(`build-upscaling/benchmark-work/profiles/shipped-defaults/neural-runtime/DLSSVideoPlayer.log`
in each worktree.)

Three things follow, and the second is a correction to this measurement's own brief.

1. **The gate under test on this material is the resolve-shader half**,
   `src/NvofResolveShader.h:66-71` at HEAD, armed by `CellsPerPixel > 0`, which
   `src/D3D12Renderer.cpp:827` sets to `1/grid` whenever `m_nvof.BackwardFlow()` exists.
   The decoded frame is 1920×1080 and the DLSS input is 1920×1080, so `PlanHardwareFlow`
   (`src/OpticalFlowNvof.h:174-188`) returns `attempt=true` with `motionScale 1,1` and the
   engine came up; the CPU block matcher's `kRoundTripCells = 0.8` decision
   (`src/TemporalGuides.cpp:440`) never decides the motion texture NGX reads.
2. **`OpticalFlowNvof.cpp:391` cannot tell the two arms apart.** It prints
   `both, round-trip gate armed` from `m_backFlow != nullptr`, and this A/B deliberately
   keeps the backward field allocated in both arms — so that line reports the *field*, not
   the *rejection*, and it says "armed" in the gate-off build too (quoted above). That is
   why the gate-off arm carries a log line of its own. Anyone reaching for line 391 as an
   arm assertion in future should read it as "a backward field exists", nothing more.
3. **The CPU half is not idle, it feeds depth.** `BuildDepthProxy`
   (`src/TemporalGuides.cpp:536-556`) reads the CPU `flowX`/`flowY` field to build the
   guide grid's B channel, and these renders ran at the shipped `mv=1,depth=1`. So with
   both halves disabled together — which is what the arm definition asks for — the
   arm-to-arm difference is the shader gate acting on the motion texture **plus** the CPU
   gate acting on the depth proxy. Section 7 probes that: with `depth=0` (uniform 0.75, no
   CPU flow in the output) the two arms still differ, so the shader half alone moves the
   picture. Splitting the two halves' *magnitudes* would need a third build and is named as
   unexercised in section 9.

## 2. The exact diff that defines the gate-off arm

`q1on` is unmodified `62491c6` (`git status` clean). `q1off` is `62491c6` plus exactly this,
in `../dlss5-q1off` (`git diff -U2 HEAD -- src/`, 3 files, +23 −14):

```diff
--- a/src/NvofResolveShader.h
+++ b/src/NvofResolveShader.h
@@ -57,17 +57,12 @@ float2 PSNvofMotion(V i):SV_Target{
         motion*=saturate((Gate.y-cost)/(Gate.y-Gate.x));
     }
-    // The round trip. The backward field is estimated on the reference frame, so it is
-    // read where the forward vector lands rather than where it starts, and the landing
-    // cell is clamped because content that left the frame has nowhere to come back from
-    // and the border cell is the closest honest answer. The test judges the measured
-    // pair, not the cost-faded vector above, and rejects by zeroing rather than fading:
-    // an occluded cell has no motion to scale down, and zero is already what the rest of
-    // this path means by nothing moved here.
-    if(CellsPerPixel>0){
-        int2 dest=clamp(cell+int2(round(flow*CellsPerPixel)),int2(0,0),int2(dim)-1);
-        float2 back=float2(BackFlow.Load(int3(dest,0)))*FlowScale;
-        float2 residual=flow+back;
-        if(dot(residual,residual)>GateAlpha*(dot(flow,flow)+dot(back,back))+GateBeta)motion=float2(0,0);
-    }
+    // Q1 MEASUREMENT ARM - ROUND-TRIP REJECTION REMOVED. [comment, 8 lines]
     return motion*MotionScale;
 }

--- a/src/TemporalGuides.cpp
+++ b/src/TemporalGuides.cpp
@@ -438,5 +438,12 @@ void TemporalGuideGenerator::EstimateFlow(...)
                         const float rtx = fbx + float(rx), rty = fby + float(ry);
-                        consistent = std::sqrt(rtx * rtx + rty * rty) <= kRoundTripCells;
+                        // Q1 MEASUREMENT ARM - ROUND-TRIP REJECTION REMOVED. [comment, 6 lines]
+                        (void)rtx; (void)rty;
+                        consistent = true;
                     }

--- a/src/D3D12Renderer.cpp
+++ b/src/D3D12Renderer.cpp
@@ -553,4 +553,11 @@ bool D3D12Renderer::CreateVideoResources(){
+    // Q1 MEASUREMENT ARM. [comment, 4 lines]
+    LOG("Q1 measurement arm: round-trip rejection REMOVED in both the NVOFA resolve pass "
+        "and the CPU estimator; estimation (BOTH directions, global flow, backward field) "
+        "is the shipped one. Gate-off build for docs/measurements/q1-gate-camera-original-20260915.");
```

What that is and is not:

- **Only the rejection changes.** `NV_OF_INIT_PARAMS` is untouched, so `predDirection`
  stays `NV_OF_PRED_DIRECTION_BOTH`, `enableGlobalFlow` stays `NV_OF_TRUE`, the backward
  flow and cost surfaces are still allocated and registered, and the backward SRV is still
  written at `t2` (`src/D3D12Renderer.cpp:538-541`). The CPU estimator still runs its
  reverse ±2-cell search and still pays for it; only the verdict is forced true. Turning
  BOTH-direction flow off instead would have changed vector *estimation* as well as the
  gate, which would not answer this question.
- **The third hunk is a log line and nothing else.** It writes no pixel and is there
  because of finding 2 above: the shipped log cannot distinguish the arms.
- `FlowGate.h` is deliberately untouched: its `Disagrees()` has no runtime caller
  (`tests/UpscalingTests.cpp` is the only one), the criterion reaching the GPU is the HLSL
  text above.

## 3. Per-arm assertion that each arm took effect

Four, of which the first three are independent of any rendering.

**(a) Compiled-constant probe.** The resolve pass's HLSL is a string literal in the
worker, so the rejection is greppable in the binary. Byte counts in
`build-upscaling/Release/neural-runtime/NeuralWorker.exe`:

| needle | q1on | q1off |
|---|---:|---:|
| `GateAlpha*(dot(flow,flow)` — the rejection itself | **1** | **0** |
| `BackFlow.Load(int3(dest,0))` — the backward fetch it judges | **1** | **0** |
| `static const float GateAlpha=` — the constant's declaration | 1 | 1 |
| `Q1 measurement arm: round-trip rejection REMOVED` | **0** | **1** |
| `both, round-trip gate armed` — the line that can't tell them apart | 1 | 1 |

`sha256(NeuralWorker.exe)`: q1on `da1794f0da1e124b49487403213f00dc4e7ee35327c331fbb5240bd45585c372`
(717824 bytes), q1off `2b2de1e19eef3a704969b7bb85a49ef2069e86d6ea5d179b34903c4e34840ebc`
(718336 bytes).

**(b) The runtime's own identity receipt.** Each preflight receipt hashes 13 runtime files.
Flattened and compared key by key across 37 fields, the two arms' receipts differ only in
`elapsedMilliseconds` (2718 vs 2719), the timestamps inside the Feature-18 observation
lines, and **`NeuralWorker.exe`'s `sha256`** — which matches (a) exactly. All 12 vendor
modules, their versions and hashes, and the model-store digest are identical. The arms
differ in the worker and in nothing else the runtime can see.

**(c) The gate-off arm says so at session start**, and the shipped arm prints no such line
(section 1). Read from each arm's own `DLSSVideoPlayer.log`.

**(d) The rendered pictures are not bit-identical**, which is the assertion that matters:
the gate engages on this material. Frames that differ between the arms, `shipped-defaults`,
repeat 1 (rgb24 per-frame MD5):

| clip | frames | differ | frames where the arms agree |
|---|---:|---:|---|
| `orig-film-cuts-a` | 131 | **129** | 0, 4 |
| `orig-film-cuts-b` | 70 | **59** | 0, then 60–69 (the static end shot after the last cut) |
| `orig-film-fade` | 31 | **30** | 0 |
| `orig-faces` | 101 | **101** | none |
| `orig-game-cuts` | 91 | **91** | none |
| `orig-game-motion` | 66 | **65** | 0 |
| `orig-dissolve` | 71 | **71** | none |

Frame 0 has no predecessor, so no flow exists to gate — agreement there is the expected
shape, not a null result.

**Within each arm the renders are bit-reproducible**, which is what makes the deltas
signal. Both repeats of every clip in both arms produced identical rgb24 frame-sequence
digests, and across all **25** clip×profile×arm cells measured today (7 clips × 2 arms at
`shipped-defaults`, 7 at `shipped-intensity-0`, 2 × 2 at `shipped-depth-constant`) the
maximum |repeat 1 − repeat 2| over `temporal_sigma_added`, `temporal_sigma_output`,
`temporal_sigma_source`, `flicker_added`, `psnr_mean`, `ssim_mean`, `delta_e_mean`,
`false_motion_rate`, `flip_rate_output`, `flip_rate_added` and `flip_rate_source` is
exactly `0.0000000000`. There is no noise floor for a sub-point delta to hide in.
(`tables.py` also refuses to print a number whose two repeats disagree; it printed every
one.)

| clip | gate-on digest (both repeats) | gate-off digest (both repeats) |
|---|---|---|
| `orig-film-cuts-a` | `b73f2d214fcd3ca9` | `722fc62efb47be58` |
| `orig-film-cuts-b` | `aad153f77c74830b` | `cf36d866cb509ab7` |
| `orig-film-fade` | `b8ae06f59f86be9a` | `09ad6f82e0822b0a` |
| `orig-faces` | `71ca84c3db1aab69` | `e8ce6e65a03a9f2a` |
| `orig-game-cuts` | `ebb44bd990a5000f` | `2498da2526346618` |
| `orig-game-motion` | `d2d0079d0501d3da` | `c4d5a398207afac3` |
| `orig-dissolve` | `ead6be3c37ad0fe8` | `6a335fa123b587d6` |

## 4. The corpus, rebuilt and verified

`python tools/benchmark/corpus.py` in `../dlss5-q1on`, with the three fetched sources
copied in from the main tree's `build-upscaling/camera-original/` (they are not committed;
`tools/benchmark/fetch_camera_original.ps1` is what fetches them, and nothing needed
re-fetching today). 20 clips built; `python tools/benchmark/corpus.py --check` then
re-hashed all of them and compared the camera-original seven against the tracked digests
in `tools/benchmark/camera-original.digests.json`:

```
orig-film-cuts-a: matches the tracked digest and labels
orig-film-cuts-b: matches the tracked digest and labels
orig-film-fade:   matches the tracked digest and labels
orig-faces:       matches the tracked digest and labels
orig-game-cuts:   matches the tracked digest and labels
orig-game-motion: matches the tracked digest and labels
orig-dissolve:    matches the tracked digest and labels
```

So all seven `orig-*` clips are the clips whose labels were verified frame by frame on
14 September, and nothing was skipped: **no source failed to fetch.** (The unrelated
synthetic `faces` clip skipped itself as it always does here — its `mafia-60s.mkv`
fixture is not on this machine — and it is not part of this A/B.)

| clip | frames | fps | labels |
|---|---:|---:|---|
| `orig-film-cuts-a` | 131 | 23.976 | 8 hard cuts: 4, 14, 24, 39, 51, 81, 105, 120 |
| `orig-film-cuts-b` | 70 | 23.976 | 3 hard cuts: 13, 41, 60 |
| `orig-film-fade` | 31 | 23.976 | soft 6–27, a real fade to black |
| `orig-faces` | 101 | 23.976 | 4 hard cuts: 19, 34, 56, 83 |
| `orig-game-cuts` | 91 | 30 | 2 hard cuts: 20, 70 |
| `orig-game-motion` | 66 | 30 | no cuts, one continuous moving shot |
| `orig-dissolve` | 71 | 23.976 | soft 21–48, a real cross-dissolve |

## 5. Commands

```
# (shell mixture: the .ps1 invocation below is PowerShell, the rest is copy-paste shape
#  rather than one runnable script - each arm was driven from its own worktree)
# trees (from the main checkout, HEAD 62491c6)
git worktree add ../dlss5-q1on HEAD
git worktree add ../dlss5-q1off HEAD
# FFmpeg copied, never junctioned; the DLSS SDK is read in place via -DDLSS_SDK
cp external/ffmpeg/bin/ffmpeg.exe  ../dlss5-q1{on,off}/external/ffmpeg/bin/
cp external/ffmpeg/bin/ffprobe.exe ../dlss5-q1{on,off}/external/ffmpeg/bin/
cp build-upscaling/camera-original/{godfather-50th.webm,gtavi-extended.webm,cand-lawrence-arabia.mp4} \
   ../dlss5-q1on/build-upscaling/camera-original/

# per tree
cmake -S . -B build-upscaling -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF \
      -DDLSS_SDK=<main>/external/DLSS
cmake --build build-upscaling --config Release --target NeuralWorker --parallel
./tools/stage_runtime.ps1 -InputDirectory <main>/external/runtime `
      -Destination build-upscaling/Release/neural-runtime
cp packaging/ReShade.ini packaging/ReShadePreset.ini build-upscaling/Release/neural-runtime/

# corpus, once, in q1on; both arms render the same bytes
cd ../dlss5-q1on && python tools/benchmark/corpus.py && python tools/benchmark/corpus.py --check

# renders: 7 clips x 2 arms x 2 repeats at the shipped mask state
cd <arm>/tools/benchmark
python run.py --corpus <q1on>/build-upscaling/benchmark-corpus \
  --clips orig-film-cuts-a orig-film-cuts-b orig-film-fade orig-faces orig-game-cuts \
          orig-game-motion orig-dissolve \
  --profiles shipped-defaults \
  --profile-file <main>/docs/measurements/art-defaults-20260914/shipped-defaults.profile.json \
  --repeats 2
# carrier floor, gate-on arm only
python run.py ... --profiles shipped-intensity-0 \
  --profile-file <main>/docs/measurements/art-defaults-20260914/shipped-state.profile.json --repeats 2
# depth-off probe (section 7), two clips
python run.py ... --profiles shipped-depth-constant \
  --profile-file <main>/docs/measurements/depth-ab-20260914/shipped-state.profile.json --repeats 2

# scoring: each arm's own runs directory, quality metrics only
python analyze.py --corpus <q1on>/build-upscaling/benchmark-corpus \
  --runs <arm>/build-upscaling/benchmark-work/runs --no-ocr --no-faces
```

**Run directories never crossed**: each arm is a separate worktree, so `common.py`'s
`RUNS` resolves to that arm's own `build-upscaling/benchmark-work/runs` and each arm's
per-run `metrics.json` and `analysis/` stay in its own tree. **50 runs, 0 failed** — 14
`shipped-defaults` + 14 `shipped-intensity-0` + 4 `shipped-depth-constant` in `q1on`, 14
`shipped-defaults` + 4 `shipped-depth-constant` in `q1off`.

**On the mask state:** the profile used is the tracked `shipped-defaults` — all eight
player `NR*` keys at their `src/NeuralSettings.h` values with `NRAutoMask=1`, byte-identical
in its overrides to `shipped-depth-proxy` in `depth-ab-20260914/shipped-state.profile.json`.
`art-defaults-20260914/shipped-state.profile.json` contains only arms that deviate in one
key from that state (it is where `shipped-intensity-0` comes from), so the pure shipped
state is its sibling file. Harness `baseline` was deliberately *not* used: it writes no
`NR*` key at all, which leaves the add-on's auto mask OFF while the player ships
`NRAutoMask=1` — the Q3 lesson.

`tables.py` beside this report re-derives every number below from the run directories;
`tables.txt` is its output from this session.

## 6. The A/B

Median of 2 bit-identical repeats per cell, `shipped-defaults` (`mv=1,depth=1`,
`NRAutoMask=1`). **Lower is better in the first three metric pairs**: false motion is the
fraction of source-static cells the output moved anyway, flips is the fraction of cells
whose moving/static verdict changes between consecutive pairs, added σ is the per-pixel
temporal standard deviation of the output minus the source's inside a shot. Δ is
**gate-on − gate-off**, so **negative is the gate paying**.

| clip | false motion on → off | Δ | relative | flips on → off | Δ | σ+ on → off | Δ |
|---|---|---:|---:|---|---:|---|---:|
| `orig-film-cuts-a` | **0.08397** → 0.09437 | −0.01041 | **−11.03 %** | **0.22218** → 0.22664 | −0.00446 | **−1.3767** → −1.2102 | −0.1664 |
| `orig-film-cuts-b` | **0.03187** → 0.04378 | −0.01191 | **−27.20 %** | **0.10385** → 0.12147 | −0.01762 | **−0.9554** → −0.7964 | −0.1590 |
| `orig-film-fade` | **0.14923** → 0.15245 | −0.00321 | **−2.11 %** | **0.19369** → 0.21843 | −0.02474 | −2.6463 → **−3.1530** | +0.5067 |
| `orig-faces` | **0.03478** → 0.06552 | −0.03074 | **−46.91 %** | **0.12110** → 0.15884 | −0.03774 | **−2.4812** → −2.0286 | −0.4527 |
| `orig-game-cuts` | 0.05563 → **0.05539** | +0.00024 | **+0.43 %** | **0.16617** → 0.16917 | −0.00300 | **−0.8195** → −0.8094 | −0.0101 |
| `orig-game-motion` | **0.07461** → 0.08223 | −0.00762 | **−9.27 %** | **0.18374** → 0.18763 | −0.00388 | 0.6395 → **0.5850** | +0.0545 |
| `orig-dissolve` | **0.04383** → 0.04497 | −0.00114 | **−2.54 %** | **0.12112** → 0.12539 | −0.00427 | −2.4117 → **−2.4395** | +0.0278 |

Score, per clip and never pooled: **false motion 6 of 7 for the gate, flips 7 of 7,
added σ 4 of 7.** All three at once on `orig-film-cuts-a`, `orig-film-cuts-b` and
`orig-faces`. Two of three on the remaining four.

Secondary metrics, same runs, same convention (Δ = on − off):

| clip | flicker+ on → off | Δ | flips *added* on → off | flips source | PSNR Δ | SSIM Δ |
|---|---|---:|---|---:|---:|---:|
| `orig-film-cuts-a` | −1.1128 → −0.9507 | −0.1621 | −0.01773 → −0.01327 | 0.23991 | −0.063 | −0.00112 |
| `orig-film-cuts-b` | −0.9014 → −0.7641 | −0.1374 | −0.06144 → −0.04382 | 0.16529 | +0.057 | +0.00037 |
| `orig-film-fade` | −0.1514 → −0.1171 | −0.0343 | **−0.01270 → +0.01204** | 0.20639 | **+1.820** | **+0.07745** |
| `orig-faces` | −1.8539 → −1.4449 | −0.4091 | −0.09098 → −0.05324 | 0.21208 | −0.246 | −0.00660 |
| `orig-game-cuts` | −0.2395 → −0.2214 | −0.0180 | −0.01315 → −0.01016 | 0.17933 | −0.290 | −0.00185 |
| `orig-game-motion` | 0.1065 → 0.1454 | −0.0389 | −0.00719 → −0.00330 | 0.19093 | −0.103 | +0.00059 |
| `orig-dissolve` | −0.2073 → −0.1967 | −0.0106 | −0.01823 → −0.01395 | 0.13934 | −0.009 | +0.00056 |

Three things in that table are worth naming:

- **Added flicker improves on 7 of 7** (−0.0106 to −0.4091 eight-bit luma levels).
- **`orig-film-fade` is the one clip where the gate-off arm adds flips above the source**
  (+0.01204) where the shipped arm removes them (−0.01270); it is also where the shipped
  arm is 1.82 dB and 0.077 SSIM *closer to the source*. Its added σ still favours gate-off
  by 0.507, so σ and fidelity disagree on this clip and the report does not pretend
  otherwise: on a fade to black the gate-off arm is smoother than the shipped one and
  further from the source.
- **PSNR is a wash elsewhere** (−0.290 to +0.057 dB) and is the weakest metric here anyway —
  the `intensity-0` carrier below reads 34.3–51.5 dB against the arms' 28.1–44.3, i.e. most
  of the distance to the source is the neural relighting the pass is supposed to apply, not
  error. No fidelity claim rests on PSNR in this report.

## 7. Where the difference lives: not at the cuts, and not only in the depth channel

**Cut decisions are identical between arms on all seven clips** — `history_resets`,
`accepted_strong_cuts`, `accepted_weak_cuts`, `suppressed_cuts` from each `result.json`:
9/2/6/0, 4/1/2/0, 1/0/0/0, 5/0/4/0, 3/0/2/0, 1/0/0/0, 1/0/0/0 in both arms. Both arms'
outputs also score **1.00/1.00/1.00** cut precision/recall/F1 against the manifest on all
four labelled-cut clips, detecting exactly the labelled indices — `[4, 14, 24, 39, 51, 81,
105, 120]`, `[13, 41, 60]`, `[19, 34, 56, 83]`, `[20, 70]` — with no false positive, and
zero detections on the three unlabelled clips. So the gate changed the flow field, not the
cut logic.

**The win is not a cut artifact.** `analyze.py` already excludes pairs that span a labelled
cut, but the frames just after a cut have no history for the gate to test, so the false
motion was recomputed with the same grid, the same 2/255 cell tolerance and an added
distance filter:

| clip | ≥0 frames from a cut | ≥3 frames | ≥6 frames |
|---|---|---|---|
| `orig-film-cuts-a` | −11.03 % (122 pairs) | −10.09 % (90) | −8.27 % (46) |
| `orig-film-cuts-b` | −27.20 % (66) | −27.59 % (54) | −30.16 % (36) |
| `orig-faces` | −46.91 % (96) | −45.79 % (80) | −46.55 % (56) |
| `orig-game-cuts` | +0.43 % (88) | +0.71 % (80) | +0.74 % (68) |

Every sign and nearly every magnitude survives dropping the cut neighbourhoods, and the
three clips with no cuts at all (`orig-film-fade`, `orig-game-motion`, `orig-dissolve`)
move in the same direction as the rest. The adverse clip stays adverse.

**The shader half alone changes the picture.** Because `BuildDepthProxy` reads the CPU flow
field (section 1, finding 3), both arms were also rendered at `shipped-depth-constant`
(`mv=1,depth=0`, uniform 0.75 depth, so no CPU flow reaches the output at all), two repeats
each on `orig-faces` and `orig-game-cuts`:

| clip, `mv=1,depth=0` | false motion on → off | Δ | relative | at `depth=1` for comparison |
|---|---|---:|---:|---:|
| `orig-faces` | **0.03451** → 0.06538 | −0.03087 | **−47.21 %** | −46.91 % |
| `orig-game-cuts` | 0.05613 → **0.05531** | +0.00082 | **+1.48 %** | +0.43 % |

Both depth-off arms are bit-reproducible across their two repeats, and the two arms differ
on **101/101** and **91/91** frames respectively (digests `aec010c189a077e1` vs
`7f80d8c97e6caecc`, `b5209df0ef2fcd72` vs `64ee29089adcf22e`). With the depth channel
pinned to a constant the whole arm-to-arm difference has to come from the resolve pass, and
it reproduces the `depth=1` result to within 0.3 pp relative on the clip with the large
effect and 1 pp on the clip with the tiny one — including its sign on both. So the gate's
behaviour on this material is the shader half's; the CPU half's route through the depth
proxy is present but small. Flips (−0.03736, −0.00274) and added σ (−0.4448, −0.0101) also
keep their `depth=1` signs and magnitudes. This is a two-clip probe, not the full
attribution — see the first item under UNEXERCISED.

## 8. The scale to read the deltas against

Same construction as the earlier gate record: `shipped-intensity-0` is the **gate-on** tree
at the shipped mask state with the model's relighting at zero, so decode, guides, the
Feature-18 evaluate and the NVENC re-encode all still happen. It measures the plumbing's
own false motion; the distance from it to an arm is what the neural pass contributes.

| clip | carrier floor | gate-on | gate-off | NR's share, on | NR's share, off | share of NR's false motion the gate removes |
|---|---:|---:|---:|---:|---:|---:|
| `orig-film-cuts-a` | 0.05605 | 0.08397 | 0.09437 | +0.02792 | +0.03832 | **27.2 %** |
| `orig-film-cuts-b` | 0.02420 | 0.03187 | 0.04378 | +0.00767 | +0.01958 | **60.8 %** |
| `orig-film-fade` | 0.13385 | 0.14923 | 0.15245 | +0.01538 | +0.01859 | **17.3 %** |
| `orig-faces` | 0.02575 | 0.03478 | 0.06552 | +0.00903 | +0.03977 | **77.3 %** |
| `orig-game-cuts` | 0.04204 | 0.05563 | 0.05539 | +0.01359 | +0.01335 | −1.8 % |
| `orig-game-motion` | 0.05017 | 0.07461 | 0.08223 | +0.02444 | +0.03206 | **23.8 %** |
| `orig-dissolve` | 0.03453 | 0.04383 | 0.04497 | +0.00929 | +0.01043 | **10.9 %** |

Both arms stay above the floor on every clip, so the gate is not pushing false motion below
what the plumbing alone produces; it is removing between a tenth and three quarters of what
the neural pass adds on top of it — on `orig-faces` the pass's own false-motion share is
**4.4×** larger without the gate than with it. This is a bigger effect than the captures
showed (11.8–74.4 % of NR's share there, on four clips, with a −4.6 to −14.4 % relative
spread against −2.1 to −46.9 % here), and it is the relevant comparison because the
carriers differ: the captures' baseline was itself DLSS-NR output, this one is a publisher's
release.

## 9. Verdict

**Won.** On camera-original material the round-trip gate lowers false motion on 6 of 7
clips (−2.1 % to −46.9 % relative, removing 10.9–77.3 % of the neural pass's own
contribution to that metric), lowers cell flips on 7 of 7 and added flicker on 7 of 7,
improves added σ on 4 of 7, and loses on exactly one clip by one part in 230
(`orig-game-cuts`, false motion +0.00024 = +0.43 %, while that clip's flips and σ still
favour the gate). With renders bit-identical across repeats inside each arm there is no
noise floor for those deltas to hide in, so the small adverse number is real and small
rather than uncertain — and it is the only one.

Q1's WIN therefore **reproduces on footage cut from publishers' own releases**, and does so
under a stricter A/B than the one that first claimed it: the arms here differ by the
rejection alone rather than by two waves of commits, and each arm is proven to be itself by
a compiled-constant probe, the runtime's own identity receipt and its own log line. The
wave-two line "if real footage does not pay either, removing it is the honest move" is
answered: it pays.

Scope, stated rather than implied: one machine, one Ada card, driver 610.47, one guide
configuration (`mv=1,depth=1`) at one mask state (`NRAutoMask=1`), 561 source frames across
7 clips, 17 labelled hard cuts and two real gradual transitions, 1080p SDR, `hevc_nvenc`.

## What this does not settle (UNEXERCISED)

- **Which half of the gate pays how much, on all seven clips.** Both halves were disabled
  together, as this measurement's brief specified. Section 1 establishes that the motion
  texture comes from NVOFA, and section 7's `depth=0` probe bounds the CPU half's share at
  0.3 pp relative on `orig-faces` and 1 pp on `orig-game-cuts` — but that is two clips and
  a bound, not an attribution. Splitting the two magnitudes clip by clip needs a third
  build (shader gate off, CPU gate on) over the same seven clips. Not built today.
- **The gate's two constants.** `FLOW_GATE_ALPHA 0.01` / `FLOW_GATE_BETA_PX2 0.5` are still
  Sundaram/Brox literature defaults; nothing here ran the backward field against a labelled
  occlusion mask, and nothing here swept them. A verdict that the gate pays is not a verdict
  that these are the right thresholds.
- **`kRoundTripCells = 0.8`** was never the deciding constant on this material, because the
  CPU block matcher did not supply motion on any of these clips. Its own A/B needs a session
  where NVOFA is absent or refused.
- **Human judgement.** No blind ballot, no face-identity metric (`--no-faces`), no OCR
  (`--no-ocr`); `orig-faces` is scored on temporal/motion metrics only.
- **Timing, VRAM and throughput.** The GPU was shared for the whole session; every number
  above is a deterministic quality metric, and no timing, pace or VRAM claim is made.
- **Other hardware.** One Ada card. Nothing here speaks for Turing, Ampere, Blackwell, or
  for the pinned 616.64 driver.

## Artifacts and cleanup

| What | Where |
|---|---|
| This report, its table script and that script's output | `docs/measurements/q1-gate-camera-original-20260915/{REPORT.md,tables.py,tables.txt}` |
| Gate-on arm (unmodified `62491c6`), runs, corpus | worktree **`../dlss5-q1on`** — `build-upscaling/benchmark-work/runs` (32 runs), `build-upscaling/benchmark-corpus`, `build-upscaling/camera-original` (copied sources) |
| Gate-off arm (`62491c6` + the section-2 diff), runs | worktree **`../dlss5-q1off`** — `build-upscaling/benchmark-work/runs` (18 runs) |

Both worktrees are **detached at `62491c6`** and left in place for inspection; the gate-off
diff is uncommitted in `../dlss5-q1off` working tree. Neither has a junction anywhere —
FFmpeg was copied in and the DLSS SDK was read in place through `-DDLSS_SDK`, so
`git worktree remove --force` on either is safe, and the main tree's `external/` is
untouched. `../dlss5-q1on` is 2.1 GiB on disk and `../dlss5-q1off` 984 MiB (`du -sh`);
removing them deletes the builds, the staged runtimes, the copied FFmpeg and camera
sources, the rebuilt corpus and both arms' run directories. Everything cited above is in
this directory.
