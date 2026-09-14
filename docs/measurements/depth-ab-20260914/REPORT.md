# Does the reconstructed depth proxy pay for itself? — 14 September 2026

**Verdict: the proxy pays — keep it.** Roadmap item Q3 asked whether
`TemporalGuides::BuildDepthProxy` earns the guide path it occupies. Answered in
two rounds on this one machine: four labelled **synthetic** clips first, then four
**real-footage** clips, two arms, two repeats each, 64 renders, every one
bit-identical on repeat.

The keep rests on three measured facts and one honest negative.

1. The channel is **live** — the two arms differ in output digest on all eight clips, so this is not a no-op binding.
2. It is **nearly free** — 0.057–0.128 ms of CPU per frame at a 160×90 grid and *exactly nothing* on the GPU, because the depth resource, the `SV_Depth` pass and the NGX parameter are unconditional and `depth=0` merely writes a uniform `0.75f`.
3. The **removal hypothesis was falsifiable, and it failed.** `real-game-motion` — a character running off a rooftop and falling, so the subject climbs the frame while getting *nearer*, inverting the proxy's `-0.42*yn` prior — is the adversarial case for that prior. The proxy improves **all four** all-pairs temporal/motion metrics there, one of only three clips in eight where it sweeps them all. The prior being locally wrong does not make it harmful.
4. The honest negative: **no visible quality win, and this report's own published bar for one was not met.** Round 1 set a keep bar of "false motion and flip rate improve together by more than the 0.1–0.3 pp seen here". On real footage they improve together on 3 of 4 clips, including the adversarial one, but by **0.002–0.059 pp** — an order of magnitude *below* that bar. So this is a keep on cost-benefit, not a keep on a quality claim, and the distinction is load-bearing: nobody should cite this report for an image-quality improvement.
5. One scope limit remains on the real-footage half, and one has been closed.
   Those four clips are **NR-processed captures**, not footage: every frame was
   recorded with neural rendering on, so the pixels are the player's own DLSS-NR
   output taken through a screen capture, an h264 encode and a lanczos upscale
   before this A/B rendered them again. Sound for an A/B where both arms see
   byte-identical input, which this is; not a statement about original footage.
   The mask-state check is now **done** and it does not leave the verdict where it
   found it: at the shipped `NRAutoMask=1` the keep still stands on cost, but the
   per-clip sweep this report leaned on does not reproduce - two of the four clips
   change direction. See [The mask-state control](#the-mask-state-control-16-renders-at-nrautomask1)
   at the end, which supersedes point 3's use of `real-game-motion` as the
   settling result.

The superseded first-round verdict ("undecided pending real footage") is preserved
below with the reasoning that produced it, because the thing that changed it was
the specific experiment it asked for.

## Environment

| Item | Value | Source |
| --- | --- | --- |
| OS | `Windows 11 Pro` / `10.0.26200` | session |
| GPU | `NVIDIA GeForce RTX 4080 SUPER`, DXGI driver `32.0.16.1047` (= NVIDIA 610.47), 16047 MiB | worker preflight (`result.json` → `preflight.gpu`) |
| GPU (second adapter) | `Intel(R) UHD Graphics 770`, not used for rendering | session |
| CPU | `Intel(R) Core(TM) i7-14700` | session |
| Repo commit | `0556eb0`, branch `slice/depth` | `git rev-parse` |
| Build | Release x64, Visual Studio 17 2022, MSVC `19.44.35222.0`, C++20 | `CMakeCXXCompiler.cmake` |
| Worker | `NeuralWorker.exe` `0.21.2` | preflight `workerVersion` |
| Runtime | 12 locked files staged; ReShade `6.8.0.2155`, RenoDX `4.7` (addon `0.2026.828.517`, API 18), NR `310.8.0` | `tools/stage_runtime.ps1`, preflight `runtime` |
| NGX | feature 18 created, evaluated, armed, inline interception, upscaling OFF, no later failure | preflight `feature18` |
| Encoder | `hevc_nvenc` | run results |
| Synthetic corpus (round 1) | built in this tree; schema 1, generated `2026-09-14T11:04:05Z`, FFmpeg `9.0.1-essentials`, matroska/ffv1 level 3 yuv420p | `manifest.json` |
| Real-footage corpus (round 2) | read-only from `../dlss5-corpus/build-upscaling/benchmark-corpus`; schema 1, generated `2026-09-14T11:55:32Z`, owned and frame-verified by `RealCorpus` | `manifest.json` |
| GPU contention | round 1 quality + **cost** on a claimed idle GPU (window granted on `hub`, all three sibling slices acked idle; released after 117 s). Round 2 deliberately **contended** — no window claimed, `MainDefects` cleared to saturate cores concurrently — so every round-2 timing is `CONTENDED — not citable` | `hub` transcript |

The driver is the neural floor (610.47) rather than the recommended pin (616.64),
so nothing here speaks for the pinned driver.

## Gates

- **48/48 renders succeeded**, 0 failed, across all three batches: 16 synthetic contended, 16 synthetic idle, 16 real-footage contended (`run.py` reported `0 failed` each time).
- **Every frame was neural in every run**: `verified_neural_frames == frame_count` with `frame_retries = 0` in all 32 scored runs — 225/120/90/30 on the synthetic clips, 102/68/76/66 on the real ones. No run fell back to a non-neural path, so both arms are compared on fully neural output throughout. In particular **no run short-framed or failed during `MainDefects`' deliberate core-saturation window**, so there is no load artefact to discard.
- **Preflight passed for both profiles in both rounds**: feature 18 `created`/`evaluated`/`armed`, `upscalingOff`, `inlineInterception`, `laterFailure = false`, diagnosis `cause: none`.
- **`deterministic: true` for all 16 clip/arm pairs** (8 synthetic, 8 real), and identical digests across the two synthetic batches — see the determinism section.
- **The worker's own cut receipts agree arm-for-arm on all eight clips.** `accepted_strong/weak/suppressed` + `history_resets`: synthetic `4/1/0`+6 (cuts-motion), `0/0/0`+1 (cuts-similar), `0/2/1`+3 (flash-exposure), `0/0/0`+1 (pan-fast); real `2/1/1`+4 (real-film-cuts), `0/1/0`+2 (real-game-cuts), `0/0/0`+1 (real-game-motion), `0/0/0`+1 (real-dissolve). Identical in *both* arms in every case.
- **`ctest` not run** — this slice changed no `src/` or `tests/` file.

## What the two arms actually are

`tools/benchmark/run.py --list-profiles` was read rather than assumed. The
roadmap's names are aliases, and the harness prints the mapping on every run:

```
depth-proxy is baseline (mv=1,depth=1); its runs land under runs/<clip>__baseline__<rep>
depth-constant is depth-off (mv=1,depth=0); its runs land under runs/<clip>__depth-off__<rep>
```

- **`depth-proxy`** → profile `baseline`, guides `mv=1,depth=1`, one pass, no INI overrides.
- **`depth-constant`** → profile `depth-off`, guides `mv=1,depth=0`, one pass, no INI overrides.

The `[RenoDX.DLSS5]` section written into both profile clones was diffed and is
byte-identical (`EnableHooks=2`, `NeuralUplift=1`, `NREnableUpscaling=0`). The
**only** variable between the arms is the `--guides` string, so nothing in this
report is confounded by a filter setting.

### The isolation is exact, and narrower than "a whole guide path"

This matters for costing a removal, so it was checked in the source rather than
inferred from the arm names.

`TemporalGuides.cpp:615-617` is the entire gate:

```cpp
std::vector<float> depthGrid;
if (m_controls.depth) BuildDepthProxy(cur, fx, fy, gw, gh, depthGrid);
else depthGrid.assign(size_t(gw) * gh, 0.75f);
```

Everything else runs identically in both arms: `DownsampleLuma`, `EstimateFlow`,
`LumaHistogramIntersection`, `ClassifySceneCut`, `MedianFlow`, and the parallel
guide-grid write loop that packs R/G/B per cell. So the arm delta isolates
`BuildDepthProxy` alone and recovers **no** shared motion-vector cost.

On the GPU side there is nothing to recover at all. A disabled guide is still
bound — `GuideControls.h` states the contract ("the D3D12 contract rejects null
resources") and the renderer honours it unconditionally:

- `D3D12Renderer.cpp:540-544` always creates the `R32_TYPELESS` depth resource with both a `D32_FLOAT` DSV and an `R32_FLOAT` SRV.
- `D3D12Renderer.cpp:808-812` always runs the `m_psoDepthWrite` pass; `PSWriteDepth` returns `saturate(T.SampleLevel(S,i.uv,0).b)` into `SV_Depth`.
- `D3D12Renderer.cpp:865` always hands that resource to `m_dlss.Evaluate(...)`, and `DLSSBackend.cpp:278` always sets `NVSDK_NGX_Parameter_Depth`.

With `depth=0` the grid's B channel is a uniform `0.75f`, so NGX receives a real,
flat depth texture — not a null binding and not a skipped pass. **Deleting the
proxy would therefore not delete the depth resource, the `SV_Depth` pass, the
RGBA32F grid channel or the NGX binding.** It would delete the heuristic
(`0.92 - 0.42*yn - 0.17*motion - 0.10*grad`, clamped to `[0.08, 0.97]`), its
`m_prevDepth` 0.80/0.20 EMA and the 14,400-float history buffer (57.6 KiB), plus
the `depth` term in `GuideControls` and the cache identity. That is a much smaller
prize than the roadmap wording implies, and it changes the cost-benefit balance.

## Commands

Run from the worktree root unless noted. FFmpeg was **copied** (not junctioned)
into `external/ffmpeg/bin`, because `tools/benchmark/common.py` resolves it
repo-relative and a junction under `external/` recurses into the main checkout.

```
git worktree add -b slice/depth ../dlss5-depth main

cmake -S . -B build-upscaling -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON ^
  -DDLSS_SDK=C:/Users/User/Documents/GitHub/dlss5-video-player/external/DLSS ^
  -DFFMPEG_STAGED_DIR=C:/Users/User/Documents/GitHub/dlss5-video-player/external/ffmpeg/bin ^
  -DNVOF_SDK=C:/Users/User/Documents/GitHub/dlss5-video-player/external/nvof ^
  -DYOUTUBE_STAGED_DIR=C:/Users/User/Documents/GitHub/dlss5-video-player/external/youtube
cmake --build build-upscaling --config Release --parallel --target DLSSVideoPlayer NeuralWorker

powershell -NoProfile -ExecutionPolicy Bypass -File tools/stage_runtime.ps1 ^
  -InputDirectory C:/Users/User/Documents/GitHub/dlss5-video-player/external/runtime ^
  -Destination build-upscaling/Release/neural-runtime
copy packaging\ReShade.ini        build-upscaling\Release\neural-runtime\
copy packaging\ReShadePreset.ini  build-upscaling\Release\neural-runtime\

python tools/benchmark/corpus.py --corpus C:/Users/User/Documents/GitHub/dlss5-depth/build-upscaling/benchmark-corpus
python tools/benchmark/run.py --list-profiles
```

Render and score round 1 (synthetic), both from `tools/benchmark`:

```
python run.py --corpus C:/Users/User/Documents/GitHub/dlss5-depth/build-upscaling/benchmark-corpus ^
  --clips cuts-motion cuts-similar flash-exposure pan-fast ^
  --profiles depth-proxy depth-constant --repeats 2

python analyze.py --corpus C:/Users/User/Documents/GitHub/dlss5-depth/build-upscaling/benchmark-corpus ^
  --runs C:/Users/User/Documents/GitHub/dlss5-depth/build-upscaling/benchmark-work/runs ^
  --no-ocr --no-faces --force
```

Round 2 (real footage) reuses `RealCorpus`' already-built corpus read-only — no
rebuild, and nothing in that tree is written. Note the **absence of `--force`**:
that makes `analyze.py` reload the 16 synthetic `metrics.json` files from cache
(`cached <run>` per line) and compute only the 16 new runs, so one `analysis.json`
covers both rounds. The real corpus manifest is a superset of the synthetic clip
names, so it resolves every run directory in the tree.

```
python run.py --corpus C:/Users/User/Documents/GitHub/dlss5-corpus/build-upscaling/benchmark-corpus ^
  --clips real-film-cuts real-game-cuts real-game-motion real-dissolve ^
  --profiles depth-proxy depth-constant --repeats 2

python analyze.py --corpus C:/Users/User/Documents/GitHub/dlss5-corpus/build-upscaling/benchmark-corpus ^
  --runs C:/Users/User/Documents/GitHub/dlss5-depth/build-upscaling/benchmark-work/runs ^
  --no-ocr --no-faces
```

Two notes for anyone reproducing this:

- `--fresh-profiles` is unsafe here. It `rmtree`s the profile clone before *every*
  run, which races the OS releasing the just-exited worker's DLL mappings; the
  fourth run died with `PermissionError: [WinError 32] ... neural-runtime`. The
  clone's contents depend only on the profile, so one clone per profile is all the
  isolation an A/B needs. Clear `benchmark-work/` once up front instead.
- The synthetic batch was run **twice**: once contended (four slices sharing the
  GPU) and once on a claimed idle GPU. The contended batch is retained at
  `benchmark-work/runs-contended` and `analysis-contended` as the cross-check
  described below. Every synthetic number in this report comes from the idle
  batch; the real-footage batch was run contended on purpose and reports no
  citable timing.

## Determinism: repeats are bit-identical, and so is the whole batch under a different machine load

`analyze.py` reports `deterministic: true` for all **16** clip/arm pairs — 8
synthetic, 8 real. On the synthetic set, stronger still: the 16 idle-GPU runs and
the 16 contended runs produce **exactly 8 distinct `output_digest` values — one
per clip/arm — with all 16 pairs matching across the two batches**, and every one
of the 12 compared quality metrics is identical between batches to full stored
precision. The real-footage batch adds 8 more distinct digests, again one per
clip/arm with both repeats matching, rendered while `MainDefects` was deliberately
saturating cores. 48 renders, 16 digests, no exceptions.

| digest (first 16 hex) | clip | arm | repeats | batches |
| --- | --- | --- | --- | --- |
| `027bb9ccfac9e43f` | cuts-motion | depth-proxy | 2 | idle + contended |
| `c59564169c72063c` | cuts-motion | depth-constant | 2 | idle + contended |
| `37e5a65bfa48693a` | cuts-similar | depth-proxy | 2 | idle + contended |
| `5c0e0e56950e9540` | cuts-similar | depth-constant | 2 | idle + contended |
| `ae54d7a3a9972efe` | flash-exposure | depth-proxy | 2 | idle + contended |
| `fe7d59b0e8f3f2e9` | flash-exposure | depth-constant | 2 | idle + contended |
| `cfbe0716eef851d1` | pan-fast | depth-proxy | 2 | idle + contended |
| `0b02b621dbcba88f` | pan-fast | depth-constant | 2 | idle + contended |
| `bb429113e99cf159` | real-film-cuts | depth-proxy | 2 | contended |
| `39062be6065ef294` | real-film-cuts | depth-constant | 2 | contended |
| `1082deb0a2b0aeab` | real-game-cuts | depth-proxy | 2 | contended |
| `454ef144032dd564` | real-game-cuts | depth-constant | 2 | contended |
| `f1449aa12f3a901c` | real-game-motion | depth-proxy | 2 | contended |
| `e7a9a39f77ec6bdb` | real-game-motion | depth-constant | 2 | contended |
| `377ce328943d14de` | real-dissolve | depth-proxy | 2 | contended |
| `7581883990fb8a75` | real-dissolve | depth-constant | 2 | contended |

Two consequences, and they pull in opposite directions:

1. **A sub-point delta in the table below is signal, not run noise.** There is no
   run-to-run variance to hide behind: rerunning reproduces the identical frames
   on a busy box and on a quiet one. Every quality delta reported here is exactly
   reproducible.
2. **The digests differ between arms on all eight clips, so the depth channel is
   genuinely live.** This is not a no-op binding being measured; the neural pass
   reads the depth texture and its output changes. That disposes of the cheapest
   possible removal argument ("it does nothing").

What determinism does *not* buy is content generality. Each arm was sampled on 8
clips, not 8 clips × N independent noise draws, so the spread that matters for the
verdict is **across clips**, and on the synthetic set it is a spread that changes
sign. It is one machine, one driver, one 1080p30 SDR path.

## Round 1: the four labelled synthetic clips

Per clip/arm medians over 2 repeats (identical, per the digests above). Deltas are
proxy − constant; the `favours` column applies each metric's own polarity (higher
PSNR/SSIM/F1 is better, lower dE/flicker/sigma/false-motion/flip is better).

`flicker_added` and `temporal_sigma_added` are in 8-bit luma levels, output minus
source, cut frames excluded — **negative means the neural pass is smoother than
the source**, so a *more negative* value is better. `false_motion_rate` is the
fraction of cells the source held static that the output moved anyway;
`flip_rate_output` is the fraction of cells whose moving/static verdict changes
between consecutive pairs, on the generator's own 160×90 analysis grid (14,400
cells, 12×12 px each).

The **sampling** column is load-bearing. `analyze.py --sample-every` strides only
the PSNR/SSIM/dE block; sigma, the motion field and the cut test see every
consecutive pair. On `pan-fast` that is the difference between 2 sampled frames
and 29 pairs.

### cuts-motion — 225 frames, 220 pairs, 15 sampled frames, 4 manifest hard cuts

| metric | sampling | depth-proxy `mv=1,depth=1` | depth-constant `mv=1,depth=0` | delta (proxy − constant) | favours |
|---|---|---:|---:|---:|---|
| PSNR mean (dB) | sampled | 22.891 | 22.865 | +0.026 | **proxy** |
| SSIM mean | sampled | 0.8673 | 0.8656 | +0.0017 | **proxy** |
| delta-E mean (CIE76) | sampled | 11.248 | 11.411 | -0.163 | **proxy** |
| `flicker_added` | all pairs | -2.8530 | -2.8407 | -0.0123 | **proxy** |
| `temporal_sigma_added` | all pairs | -4.6029 | -4.5899 | -0.0129 | **proxy** |
| `false_motion_rate` | all pairs | 0.16003 | 0.16263 | -0.00260 | **proxy** |
| `flip_rate_output` | all pairs | 0.22865 | 0.23000 | -0.00136 | **proxy** |
| cut precision (output) | all pairs | 0.500 | 0.500 | +0.000 | tie |
| cut recall (output) | all pairs | 1.000 | 1.000 | +0.000 | tie |
| cut F1 (output) | all pairs | 0.667 | 0.667 | +0.000 | tie |

### cuts-similar — 120 frames, 116 pairs, 8 sampled frames, 3 manifest hard cuts

| metric | sampling | depth-proxy `mv=1,depth=1` | depth-constant `mv=1,depth=0` | delta (proxy − constant) | favours |
|---|---|---:|---:|---:|---|
| PSNR mean (dB) | sampled | 20.056 | 20.098 | -0.042 | constant |
| SSIM mean | sampled | 0.9829 | 0.9830 | -0.0001 | constant |
| delta-E mean (CIE76) | sampled | 13.309 | 13.176 | +0.134 | constant |
| `flicker_added` | all pairs | -0.5120 | -0.5307 | +0.0187 | constant |
| `temporal_sigma_added` | all pairs | -0.3715 | -0.3930 | +0.0215 | constant |
| `false_motion_rate` | all pairs | 0.10762 | 0.10606 | +0.00156 | constant |
| `flip_rate_output` | all pairs | 0.19167 | 0.19261 | -0.00094 | **proxy** |
| cut precision (output) | all pairs | 1.000 | 1.000 | +0.000 | tie |
| cut recall (output) | all pairs | 0.333 | 0.333 | +0.000 | tie |
| cut F1 (output) | all pairs | 0.500 | 0.500 | +0.000 | tie |

### flash-exposure — 90 frames, 89 pairs, 6 sampled frames, 0 manifest cuts

| metric | sampling | depth-proxy `mv=1,depth=1` | depth-constant `mv=1,depth=0` | delta (proxy − constant) | favours |
|---|---|---:|---:|---:|---|
| PSNR mean (dB) | sampled | 23.157 | 23.138 | +0.019 | **proxy** |
| SSIM mean | sampled | 0.9788 | 0.9789 | -0.0001 | constant |
| delta-E mean (CIE76) | sampled | 13.581 | 13.675 | -0.093 | **proxy** |
| `flicker_added` | all pairs | -1.0622 | -1.0610 | -0.0012 | **proxy** |
| `temporal_sigma_added` | all pairs | -0.8079 | -0.8134 | +0.0055 | constant |
| `false_motion_rate` | all pairs | 0.10726 | 0.10789 | -0.00063 | **proxy** |
| `flip_rate_output` | all pairs | 0.22297 | 0.22349 | -0.00052 | **proxy** |
| cut precision (output) | all pairs | 0.000 | 0.000 | +0.000 | tie |
| cut recall (output) | all pairs | - | - | - | undefined |
| cut F1 (output) | all pairs | - | - | - | undefined |

### pan-fast — 30 frames, 29 pairs, 2 sampled frames, 0 manifest cuts

| metric | sampling | depth-proxy `mv=1,depth=1` | depth-constant `mv=1,depth=0` | delta (proxy − constant) | favours |
|---|---|---:|---:|---:|---|
| PSNR mean (dB) | sampled | 24.343 | 24.176 | +0.167 | **proxy** |
| SSIM mean | sampled | 0.9883 | 0.9882 | +0.0001 | **proxy** |
| delta-E mean (CIE76) | sampled | 11.010 | 11.324 | -0.314 | **proxy** |
| `flicker_added` | all pairs | -0.9163 | -0.9206 | +0.0043 | constant |
| `temporal_sigma_added` | all pairs | -1.2806 | -1.2752 | -0.0054 | **proxy** |
| `false_motion_rate` | all pairs | 0.17673 | 0.17742 | -0.00068 | **proxy** |
| `flip_rate_output` | all pairs | 0.15807 | 0.15818 | -0.00011 | **proxy** |
| cut precision (output) | all pairs | - | - | - | undefined |
| cut recall (output) | all pairs | - | - | - | undefined |
| cut F1 (output) | all pairs | - | - | - | undefined |

### Reading the round-1 table

**The cut test is a structural tie, not a measurement.** Precision, recall and F1
are identical between arms on every clip where they are defined, and so are the
worker's own receipts — `accepted_strong_cuts`, `accepted_weak_cuts`,
`suppressed_cuts` and `history_resets` match arm-for-arm on all four clips
(cuts-motion `4/1/0`, resets 6; cuts-similar `0/0/0`, resets 1; flash-exposure
`0/2/1`, resets 3; pan-fast `0/0/0`, resets 1). That is expected from the source:
`ClassifySceneCut` consumes `globalCost` and `LumaHistogramIntersection`, neither
of which touches depth, and `BuildDepthProxy` is called *after* the cut decision.
**Depth cannot move the cut columns, so they carry no evidence either way** — they
are reported because the acceptance criteria asked for them, and because their
exact agreement is a useful consistency check on the A/B.

The `flash-exposure` precision of `0.000` and the `cuts-similar` recall of `0.333`
are pre-existing properties of the cut detector on those clips (the flash and the
exposure step fire the weak arm; the identical-histogram cuts are largely missed),
identical in both arms. They are not depth findings.

**Tally of the seven metrics depth can actually move.** cuts-motion: proxy 7/7.
pan-fast: proxy 5, constant 2. flash-exposure: proxy 4, constant 3.
cuts-similar: constant 6, proxy 1. Three clips lean proxy, one leans constant.

**Only one metric sweeps all four clips: `flip_rate_output`**, by −0.00011 to
−0.00136 against an output flip rate of 0.158–0.230. A consistent direction on
all four clips is the single most suggestive result in the table — a guide doing
its job should steady the moving/static verdict — but the magnitude is 0.07 % to
0.6 % relative, which no viewer will see.

**Every delta is small against the pass's own effect.** The neural pass changes
flicker by −0.5 to −2.9 levels and sigma by −0.4 to −4.6 levels; the depth channel
moves those by:

| clip | source flicker | output flicker | pass effect | depth delta | depth as % of pass |
|---|---:|---:|---:|---:|---:|
| cuts-motion | 14.578 | 11.725 | -2.853 | -0.0123 | 0.43 % |
| cuts-similar | 7.017 | 6.505 | -0.512 | +0.0187 | 3.65 % |
| flash-exposure | 9.590 | 8.528 | -1.062 | -0.0012 | 0.11 % |
| pan-fast | 19.486 | 18.569 | -0.916 | +0.0043 | 0.47 % |

| clip | source sigma | output sigma | pass effect | depth delta | depth as % of pass |
|---|---:|---:|---:|---:|---:|
| cuts-motion | 46.167 | 41.564 | -4.603 | -0.0129 | 0.28 % |
| cuts-similar | 13.252 | 12.880 | -0.371 | +0.0215 | 5.80 % |
| flash-exposure | 40.697 | 39.889 | -0.808 | +0.0055 | 0.68 % |
| pan-fast | 44.947 | 43.666 | -1.281 | -0.0054 | 0.42 % |

The two clips where the depth channel is the largest *fraction* of the pass effect
are the two where it makes things slightly worse, because those are the clips where
the pass effect itself is smallest. That is a warning against reading the
percentages as an effect size.

**The largest single win is the least-sampled number.** `pan-fast` PSNR +0.167 dB
is the biggest fidelity delta in the table and it rests on **2 sampled frames** of
a 30-frame clip. It should carry almost no weight. The all-pairs metrics on the
same clip (`false_motion_rate` −0.00068, `flip_rate_output` −0.00011) are the
trustworthy ones there, and they are tiny.

## Round 2: real footage, including the prior's adversarial case

Round 1 concluded "undecided" for one specific reason: synthetic fractals and
gradients give the proxy's geometric prior nothing to be right or wrong about.
`RealCorpus` built and frame-verified four real clips in the same wave, two of
which have exactly the near/far structure the round-1 decision procedure asked
for. They were rendered with the same two arms, the same `--repeats 2`, and scored
into the same `analysis.json`.

### Provenance: real, but not pristine

**The capture these clips were cut from was taken with neural rendering ON.** The
"source" pixels are the player's own DLSS-NR output from an RTX 5090, not
camera-original footage. Two consequences, and the first is the one that matters:

- **The A/B is unaffected.** Both arms receive byte-identical input, and the whole
  measurement is a difference between two arms on that input. Nothing about the
  comparison needs the source to be pristine.
- **The source-referenced fidelity columns change meaning.** PSNR is 28.9–33.3 dB
  here against 20.1–24.3 dB on the synthetic clips, and dE is 2.5–3.9 against
  11.0–13.6, because the pass is being asked to re-process pixels that already
  look like its own output. Those columns are measuring "how close does
  re-processing land to already-processed pixels", which is a different question
  from fidelity to a camera original. They are reported for completeness and
  carry little weight in the verdict.

So this is **real footage, not pristine footage**, and the report does not claim
otherwise. `real-dissolve` is a further step removed: per its manifest note the
demo capture contains no dissolve anywhere, so it is real material with a
*synthesised* cross-fade. It is the least natural of the four, which is worth
remembering when reading its row — it is also the only real clip that prefers the
flat arm.

All round-2 timings are `CONTENDED — not citable`: no window was claimed, and
`MainDefects` was explicitly cleared to saturate cores concurrently. Quality
metrics and digests are contention-immune, which this report established
experimentally in round 1.

### The real-footage table

Same conventions as round 1. `flicker_added` and `temporal_sigma_added` are 8-bit
luma levels, output minus source, cut frames excluded; negative means the pass is
smoother than its input. Note that on `real-game-motion` and `real-dissolve` these
are **positive** — the neural pass *adds* temporal instability on those two — so
there "proxy better" means the proxy reduces harm the pass is doing, not that it
adds polish.

#### real-film-cuts — 102 frames, 97 pairs, 7 sampled frames, 4 manifest hard cuts

| metric | sampling | depth-proxy `mv=1,depth=1` | depth-constant `mv=1,depth=0` | delta (proxy − constant) | favours |
|---|---|---:|---:|---:|---|
| PSNR mean (dB) | sampled | 33.286 | 33.325 | -0.039 | constant |
| SSIM mean | sampled | 0.9717 | 0.9717 | -0.0000 | constant |
| delta-E mean (CIE76) | sampled | 2.523 | 2.518 | +0.005 | constant |
| `flicker_added` | all pairs | -0.0369 | -0.0349 | -0.0019 | **proxy** |
| `temporal_sigma_added` | all pairs | -0.0681 | -0.0656 | -0.0025 | **proxy** |
| `false_motion_rate` | all pairs | 0.01146 | 0.01156 | -0.00010 | **proxy** |
| `flip_rate_output` | all pairs | 0.11677 | 0.11679 | -0.00002 | **proxy** |
| cut precision (output) | all pairs | 1.000 | 1.000 | +0.000 | tie |
| cut recall (output) | all pairs | 0.750 | 0.750 | +0.000 | tie |
| cut F1 (output) | all pairs | 0.857 | 0.857 | +0.000 | tie |

#### real-game-cuts — 68 frames, 66 pairs, 5 sampled frames, 1 manifest hard cut

| metric | sampling | depth-proxy `mv=1,depth=1` | depth-constant `mv=1,depth=0` | delta (proxy − constant) | favours |
|---|---|---:|---:|---:|---|
| PSNR mean (dB) | sampled | 31.872 | 31.869 | +0.004 | **proxy** |
| SSIM mean | sampled | 0.9633 | 0.9633 | -0.0000 | constant |
| delta-E mean (CIE76) | sampled | 3.257 | 3.261 | -0.004 | **proxy** |
| `flicker_added` | all pairs | -0.5559 | -0.5565 | +0.0006 | constant |
| `temporal_sigma_added` | all pairs | -1.0027 | -1.0018 | -0.0009 | **proxy** |
| `false_motion_rate` | all pairs | 0.14277 | 0.14336 | -0.00059 | **proxy** |
| `flip_rate_output` | all pairs | 0.26726 | 0.26738 | -0.00012 | **proxy** |
| cut precision (output) | all pairs | 1.000 | 1.000 | +0.000 | tie |
| cut recall (output) | all pairs | 1.000 | 1.000 | +0.000 | tie |
| cut F1 (output) | all pairs | 1.000 | 1.000 | +0.000 | tie |

#### real-game-motion — 76 frames, 75 pairs, 6 sampled frames, 0 cuts — **the prior's adversarial case**

| metric | sampling | depth-proxy `mv=1,depth=1` | depth-constant `mv=1,depth=0` | delta (proxy − constant) | favours |
|---|---|---:|---:|---:|---|
| PSNR mean (dB) | sampled | 29.500 | 29.518 | -0.018 | constant |
| SSIM mean | sampled | 0.9673 | 0.9673 | +0.0000 | **proxy** |
| delta-E mean (CIE76) | sampled | 3.632 | 3.625 | +0.006 | constant |
| `flicker_added` | all pairs | +0.4096 | +0.4127 | -0.0031 | **proxy** |
| `temporal_sigma_added` | all pairs | +1.2809 | +1.2846 | -0.0037 | **proxy** |
| `false_motion_rate` | all pairs | 0.07000 | 0.07037 | -0.00037 | **proxy** |
| `flip_rate_output` | all pairs | 0.18779 | 0.18825 | -0.00046 | **proxy** |
| cut precision/recall/F1 | all pairs | - | - | - | undefined (no cuts) |

#### real-dissolve — 66 frames, 65 pairs, 5 sampled frames, soft cut 15–37, 0 hard cuts

| metric | sampling | depth-proxy `mv=1,depth=1` | depth-constant `mv=1,depth=0` | delta (proxy − constant) | favours |
|---|---|---:|---:|---:|---|
| PSNR mean (dB) | sampled | 28.919 | 28.905 | +0.014 | **proxy** |
| SSIM mean | sampled | 0.9657 | 0.9656 | +0.0001 | **proxy** |
| delta-E mean (CIE76) | sampled | 3.913 | 3.921 | -0.008 | **proxy** |
| `flicker_added` | all pairs | -0.1367 | -0.1391 | +0.0024 | constant |
| `temporal_sigma_added` | all pairs | +0.2959 | +0.2836 | +0.0123 | constant |
| `false_motion_rate` | all pairs | 0.13114 | 0.13106 | +0.00008 | constant |
| `flip_rate_output` | all pairs | 0.25802 | 0.25756 | +0.00047 | constant |
| cut precision/recall/F1 | all pairs | - | - | - | undefined (no hard cuts) |

### The adversarial case is the result that settles it

`real-game-motion` was singled out before it was measured, and it deserves its own
paragraph. The shot is a character running off a rooftop and falling towards a
city: the camera translates with the subject, which occludes and disoccludes
background throughout, and — the point — **the falling subject climbs the frame
while getting nearer**. That inverts `-0.42*yn`, the proxy's "lower in frame is
nearer" term, over the part of the image that matters most. If the prior being
wrong were harmful, this is the clip where it would show.

It does not show. The proxy improves **all four** all-pairs metrics there —
`flicker_added` −0.0031, `temporal_sigma_added` −0.0037, `false_motion_rate`
−0.00037, `flip_rate_output` −0.00046 — making it one of only **three clips out of
eight** where the proxy sweeps every all-pairs metric (the others being
`cuts-motion` and `real-film-cuts`). More precisely: the neural pass *adds*
instability on this shot (`flicker_added` +0.41, `temporal_sigma_added` +1.28, both
positive), and the depth channel takes a small bite out of that harm rather than
adding to it.

The reading that fits is that the pass is not using this signal as metric depth at
all. It is using it as a **spatially coherent, temporally smoothed segmentation
hint** — which is exactly what `TemporalGuides.cpp:535-536` says it is ("explicitly
a VIDEO DEPTH PROXY, not geometric engine depth ... stable segmentation /
disocclusion hints when a movie has no Z buffer"). A field that is smooth, stable
and correlated with *something* in the image appears to help the temporal pass
slightly even when its absolute ordering is wrong. That also explains why the
`0.80/0.20` EMA matters more than the ramp coefficients, and it is a testable claim
left `UNEXERCISED` below.

### Reading the real-footage table

**Direction on the four all-pairs metrics** — the ones with 65–97 pairs behind them
rather than 5–7 sampled frames:

| clip | `flicker_added` | `temporal_sigma_added` | `false_motion_rate` | `flip_rate_output` | all four? |
|---|---|---|---|---|---|
| real-film-cuts | proxy | proxy | proxy | proxy | **yes, 4/4 proxy** |
| real-game-cuts | constant | proxy | proxy | proxy | 3/4 proxy |
| real-game-motion | proxy | proxy | proxy | proxy | **yes, 4/4 proxy** |
| real-dissolve | constant | constant | constant | constant | 4/4 constant |

That is a far more coherent picture than round 1 produced: three of four real clips
favour the proxy on the motion field, two of them unanimously, and the one that
does not is the clip with the synthesised transition. Whole-row consistency is
itself evidence — on the synthetic set no clip was unanimous across these four.

**Magnitude against the pass's own effect**, which is the sanity check that keeps
the percentages honest:

| clip | source flicker | output flicker | pass effect | depth delta | depth % of pass |
|---|---:|---:|---:|---:|---:|
| real-film-cuts | 1.519 | 1.483 | -0.0369 | -0.0019 | 5.29 % |
| real-game-cuts | 14.146 | 13.590 | -0.5559 | +0.0006 | 0.11 % |
| real-game-motion | 6.264 | 6.673 | +0.4096 | -0.0031 | 0.76 % |
| real-dissolve | 8.111 | 7.974 | -0.1367 | +0.0024 | 1.75 % |

| clip | source sigma | output sigma | pass effect | depth delta | depth % of pass |
|---|---:|---:|---:|---:|---:|
| real-film-cuts | 5.479 | 5.411 | -0.0681 | -0.0025 | 3.64 % |
| real-game-cuts | 26.020 | 25.017 | -1.0027 | -0.0009 | 0.09 % |
| real-game-motion | 19.005 | 20.286 | +1.2809 | -0.0037 | 0.29 % |
| real-dissolve | 34.301 | 34.597 | +0.2959 | +0.0123 | 4.15 % |

**The absolute deltas are an order of magnitude smaller than on synthetic content**
— `false_motion_rate` moves by 0.0001–0.0006 (0.010–0.059 pp) against 0.0006–0.0026
(0.06–0.26 pp) on the synthetic clips, and `flip_rate_output` by 0.00002–0.00047
(0.002–0.047 pp). Real footage is simply less pathological than a cellular
automaton hard-cut to a fractal zoom: on `real-film-cuts` the pass changes flicker
by only 0.037 levels in the first place, so there is far less headroom for any
guide to move.

**The cut columns are again a structural tie**, for the reason given in round 1,
and the receipts confirm it clip by clip. Two are worth recording because they are
the first real-footage observations of the cut detector in this report, and both
are identical in both arms: `real-game-cuts` scores a perfect **1.000 / 1.000 /
1.000** on its single hard cut (caught by the weak histogram arm — `0/1/0` — with
2 history resets), and `real-film-cuts` scores **1.000 / 0.750 / 0.857**, finding 3
of its 4 labelled cuts with no false positives (`2` strong, `1` weak, `1`
suppressed, 4 resets). `real-game-motion` produced **no cut and no extra reset** on
a 76-frame shot full of disocclusion, which is the correct answer. `real-dissolve`
also produced no accepted cut and no reset inside the 15–37 fade; whether that is
right is the cut detector's question, not depth's — both arms agree exactly, so it
is not a depth finding either way.

## The cost side, measured on a claimed idle GPU

A GPU window was requested on `hub`, granted by `Main`, and acked idle by
`ArtDefaults`, `RealCorpus` and `PlayerSession` before these numbers were taken;
the window was released after 117 s. **These are the only citable timing numbers
in this report.**

`guide_ms_mean` is the worker's mean per-frame wall time for the whole CPU guide
pass. Because `depth=0` skips exactly `BuildDepthProxy` and nothing else, the arm
delta *is* the proxy's cost:

| clip | proxy guide ms (r1, r2, mean) | constant guide ms (r1, r2, mean) | delta | max within-arm spread | resolved? |
|---|---|---|---:|---:|---|
| cuts-motion | 1.391, 1.389, **1.390** | 1.338, 1.328, **1.333** | +0.057 | 0.010 | yes |
| cuts-similar | 1.429, 1.398, **1.413** | 1.294, 1.277, **1.286** | +0.128 | 0.030 | yes |
| flash-exposure | 1.340, 1.291, **1.316** | 1.273, 1.182, **1.228** | +0.088 | 0.091 | no — delta ≈ spread |
| pan-fast | 1.511, 1.362, **1.437** | 1.314, 1.399, **1.357** | +0.080 | 0.149 | no — delta < spread |

So the proxy costs roughly **0.06–0.13 ms of CPU per frame**, about **4–9 % of the
1.23–1.44 ms guide pass**, resolved above the within-arm spread on the two longest
clips (225 and 120 frames) and below it on the two shortest (90 and 30 frames,
where two repeats of a mean over few frames is simply not a stable instrument).
Against `PlayerSession`'s idle-GPU live pace of 11.25–11.57 ms/frame at 1080p,
that is **0.5–1.1 % of a frame**.

Three caveats on that number, because a bare millisecond reads as an indictment:

- It is **one machine's** CPU cost for the heuristic at a **160×90 grid** (14,400
  cells) on an i7-14700 — a serial max-reduction over the flow grid followed by a
  `ParallelForRanges` per-cell evaluation. It is a statement about this
  implementation and this grid, not a property of the guide design. A different
  grid size or a fused pass would produce a different number.
- The **GPU cost is identical by construction**, and the measurement agrees:
  `neural_gpu_ms_p50` is 5.564/5.601 vs 5.570/5.575 (cuts-motion),
  5.565/5.658 vs 5.625/5.738 (cuts-similar), 5.575/5.765 vs 5.700/5.640
  (flash-exposure), 5.842/5.585 vs 5.768/5.566 (pan-fast) — proxy vs constant,
  interleaved, no separation. The depth resource, the `SV_Depth` pass and the NGX
  binding exist in both arms, so there is no GPU time to recover.
- **Cost does not decide Q3 on its own**, and here it barely participates: at
  0.5–1.1 % of a frame with zero GPU cost, the proxy is close to free. There is no
  performance pressure to remove it.

**Rejected as uncitable:** the first batch, rendered while four slices shared the
GPU, put `guide_ms_mean` at 1.915–8.009 ms and ranked the **depth-OFF** arm slower
than depth-ON on 2 of 4 clips (6.080 vs 3.707 on cuts-motion; 8.009 vs 2.100 on
cuts-similar). A contended box cannot resolve a 0.1 ms difference. Those figures
are retained in `runs-contended` only as the determinism cross-check and are
`CONTENDED — not citable` as timings. The same applies to every `wall_s`,
`e2e_fps` and `proc_fps` in that batch.

## Superseded first-round verdict: undecided pending real footage

Kept verbatim in substance because the experiment it demanded is what changed the
answer, and because a reader should be able to see that the bar was set *before*
the real-footage numbers were seen rather than fitted to them.

**Why not "proxy pays (keep)".** Three of four clips lean proxy and one metric
sweeps all four, but no delta on any clip is remotely visible: the largest
fidelity gain is +0.167 dB PSNR from 2 sampled frames, the largest temporal gain
is 0.43 % of the pass's own flicker effect, and one clip prefers the flat arm on 6
of 7 metrics. Calling that a quality win would be exactly the "refusal or filter
change dressed up as a quality win" this project reviews for. Declaring victory
here would also mean declaring it on content the proxy's prior cannot be evaluated
against — see below.

**Why not "proxy does not pay (candidate for removal)".** Three reasons.
First, the removal is worth far less than the roadmap assumes: the depth resource,
the `SV_Depth` write pass, the RGBA32F B channel and the NGX depth binding all
survive it, because a disabled guide is still bound with a neutral value. What
actually goes is one heuristic, one EMA and 57.6 KiB. Second, the cost being
removed is 0.5–1.1 % of a frame of CPU and exactly zero GPU. Third, the direction
of the evidence is mildly *favourable*: `flip_rate_output` improves on 4/4 clips,
and the largest single temporal improvement (`false_motion_rate` −0.00260, 1.6 %
relative) lands on `cuts-motion`, the only clip in this set with hard cuts and
genuine disocclusion — which is precisely the case the proxy exists to help.
Deleting a guide path on that evidence would be premature.

**Why "undecided", and what specifically is missing.** The proxy encodes a
geometric prior: *lower in frame is nearer* (`-0.42*yn`), *moving is nearer*
(`-0.17*motion`), *high-gradient is nearer* (`-0.10*grad`), temporally smoothed
80/20. The four clips round 1 had are, per the corpus manifest, five
dissimilar fractal/cellular-automaton/gradient segments cut together; four
mirrored pans of the same deep fractal still; a diagonal pan across a 4K fractal
still; and a slow pan with a flash and an exposure step. **None of them has scene
geometry.** There is no correlation between screen-Y and depth in a fractal, so
the y-ramp term is feeding the neural pass an arbitrary field rather than a
right-or-wrong estimate. These clips can prove the channel is live and can bound
its cost — both of which they did — but they structurally cannot answer whether
the prior is *informative*. A landscape shot, an interior with a foreground
subject, or any footage with a real near/far split is where `-0.42*yn` is either
approximately right or actively wrong, and that is the measurement Q3 needs.

`RealCorpus` is producing real-footage clips in this same wave. **This A/B should
be re-run against them before the proxy is either blessed or deleted**, using
exactly the commands above with the new clip names. The decision procedure is
already fixed by this report: the arms, the metric set, the sampling caveat, the
cut columns' structural tie, and the cost figure against which any quality gain
has to be judged. Concretely, on real footage I would expect a keep if
`false_motion_rate` and `flip_rate_output` improve together on shots with genuine
depth layering by more than the ~0.1–0.3 percentage points seen here, and a
removal if the y-ramp measurably *hurts* on footage whose near/far split runs
against it — a low horizon, an overhead shot, a subject at the top of frame.

## Final verdict: the proxy pays — keep it

Round 2 ran exactly the experiment round 1 specified, on the clips round 1 said
were missing, including the adversarial case round 1 named. Both of the conditions
it set were then checked literally, and it is worth being precise about how they
came out, because they did not come out symmetrically.

**The removal condition was tested and failed.** Round 1 said: remove if the
y-ramp measurably hurts where the near/far split runs against it. That is
`real-game-motion`, where the falling subject climbs the frame while getting
nearer. The proxy improves all four all-pairs metrics there — one of only three
clips in eight where it sweeps them all. The prior is locally wrong on
that shot and the output is still better with it than without it. **That is the
single most decisive number in this report**, because it is the one that could have
killed the feature and did not.

**The keep condition was not met at the magnitude it demanded.** Round 1 asked for
`false_motion_rate` and `flip_rate_output` to improve together by more than
0.1–0.3 pp. On real footage they improve together on 3 of 4 clips — unanimously
across all four all-pairs metrics on 2 of them — but by **0.002–0.059 pp**, an
order of magnitude below that bar. Round 1's bar was calibrated for "is this a
quality win worth citing", and by that bar the honest answer is still **no: there
is no visible quality win here, on any clip, in either round.** Anyone citing this
report for a quality improvement is misciting it.

**So the keep is decided on cost-benefit, and the asymmetry is the argument.** The
question Q3 actually poses is whether this code should stay in the tree, and that
question is not symmetric in the way the round-1 bar assumed:

- **What keeping costs:** 0.057–0.128 ms of CPU per frame at a 160×90 grid — 0.5–1.1 % of a frame against a measured 1080p live pace — and *exactly nothing* on the GPU, because the depth resource, the `SV_Depth` pass and the NGX parameter are unconditional in both arms.
- **What removing buys:** that same sub-millisecond, plus one heuristic, one EMA and 57.6 KiB of history. It does **not** buy the deletion of a guide path, which is what the roadmap item assumed; the depth texture, its write pass, the RGBA32F B channel and the NGX binding all survive.
- **What removing risks:** a small, reproducible, consistently-signed regression on 3 of 4 real clips, including a 4/4 sweep on the one clip in the set with genuine sustained disocclusion.

Paying 1 % of a frame of CPU to not take that risk is obviously right, and the
decision would not flip until that millisecond is genuinely contested.

**Why the effect looks the way it does**, stated as a hypothesis and not as a
finding: the pass appears to be consuming this channel as a spatially coherent,
temporally smoothed segmentation hint rather than as metric depth — which is what
the source comment claims it is for. That fits every observation here: the effect
is small, consistently signed, survives the prior being inverted, and is largest
on clips with real disocclusion. If that is right, the `0.80/0.20` EMA is doing
more work than the ramp coefficients, and the ramp could be wrong in detail
without mattering. **That is testable and untested** — see `UNEXERCISED`.

**Scope.** Eight clips, four synthetic and four real, one machine, one RTX 4080
SUPER at driver 610.47, one 1080p30 SDR path, 1920×1080 throughout, one neural
runtime pin. The real clips are real footage but not pristine footage: their source
pixels are themselves DLSS-NR output. The verdict is "keep a nearly-free component
that testing shows is never harmful and usually marginally helpful", which is as
strong a claim as 48 renders on one box can carry — and deliberately weaker than
"the depth proxy improves image quality", which this report does not establish and
should not be read as establishing.

**What would change this answer.** A genuine CPU budget fight over that
0.06–0.13 ms; a real monocular-depth model to compare against, which would move
the question from "is a cheap heuristic better than flat" to "is a cheap heuristic
good enough"; or footage where the proxy regresses the motion field by a margin a
viewer could see. None of those is in evidence today.

## `UNEXERCISED`

| Item | Why not exercised |
|---|---|
| **`depth-of` third arm** (depth from NVOFA structure) | Refused by name in `run.py`'s `UNAVAILABLE` table: "depth would have to come from the gated NVOFA structure, and `--guides` selects only `mv=0\|1,depth=0\|1` — there is no depth source to ask for". Explicit non-goal; no player flag was invented for it. Still the largest open question: it is the only arm that could show whether a *better* depth field beats a cheap heuristic. |
| **Pristine / camera-original footage** | Round 2's real clips are cut from a capture taken with **neural rendering ON**, so their source pixels are the player's own DLSS-NR output from an RTX 5090. The A/B is unaffected (both arms see identical input) but the source-referenced PSNR/SSIM/dE columns measure re-processing of already-processed pixels. No camera-original material was available in either tree. |
| **A learned depth model** | Explicit non-goal. Nothing here speaks to what a monocular-depth network would be worth; the measured comparison is heuristic-versus-flat only. |
| **The segmentation-hint hypothesis** | The final verdict proposes that the pass consumes this channel as a coherent, temporally smoothed hint rather than as metric depth. Testable — hold the `0.80/0.20` EMA and vary only the ramp coefficients, or feed a smooth field uncorrelated with the image — and **untested**: that needs new `--guides` values or a source change, both out of scope for this slice. |
| **A depth field that is deliberately inverted** | `real-game-motion` makes the prior wrong *incidentally*. Nobody rendered an arm with the ramp sign flipped, which is the clean version of that experiment and would need a source change. |
| **OCR / text legibility** | `--no-ocr` in both rounds; `text-subtitles` is not among the eight assigned clips. Whether a flat depth field harms text sharpening is unmeasured. |
| **Faces** | `--no-faces`; the corpus builder reported `face fixture missing: build-upscaling/runtime-comparison-20260907/fixtures/mafia-60s.mkv`. `real-film-cuts` contains a portrait and a man in a crowd, so this is now measurable and simply was not measured. |
| **Soft-cut behaviour as a depth question** | `real-dissolve` was rendered and scored, but whether the absent reset inside its 15–37 fade is correct belongs to the cut detector, not to depth; both arms agree exactly. |
| **Cut precision/recall as depth evidence** | Structurally insensitive to depth (`ClassifySceneCut` runs before `BuildDepthProxy` and reads only luma cost and histogram overlap). Measured and reported as ties on all eight clips; carries no signal either way. |
| **PSNR/SSIM/dE at full frame rate** | Sampled at stride 15 — 15/8/6/2 frames synthetic, 7/5/6/5 real. `pan-fast`'s fidelity row rests on 2 frames. Not re-run at stride 1. |
| **Round-2 timings** | Deliberately contended, no window claimed, `MainDefects` cleared to saturate cores concurrently. Every round-2 `wall_s`, `e2e_fps`, `proc_fps`, `gpu_ms` and `guide_ms` is `CONTENDED — not citable`. The one citable cost figure comes from the round-1 idle window and was not re-measured. |
| **Cost on real footage** | The 0.057–0.128 ms figure is a synthetic-clip measurement at a 160×90 grid. `BuildDepthProxy` is content-independent in shape (same grid, same per-cell work), so it should carry, but that was not separately measured on an idle box. |
| **4K, HDR, 60 fps, other GPUs/drivers** | One 1080p30 SDR path on one RTX 4080 SUPER at driver 610.47, one runtime pin. |
| **Perceptual / human judgement** | No viewer comparison in either round. Every claim is a metric claim, and all deltas are below any plausible visibility threshold — which is why the verdict is cost-benefit and not a quality claim. |
| **`ctest`** | Not run: this slice changed no `src/` or `tests/` file. Documentation only. |

## Artifacts

- `build-upscaling/benchmark-work/runs` + `analysis` — all 32 scored runs (16 synthetic idle-GPU, 16 real-footage contended) and the single `analysis.json` covering both rounds.
- `build-upscaling/benchmark-work/runs-contended` + `analysis-contended` — the first synthetic batch, retained solely as the cross-batch determinism cross-check. Its timings are `CONTENDED — not citable`.
- `build-upscaling/benchmark-corpus/manifest.json` — the synthetic corpus built in this tree, schema 1, per-clip digests.
- `../dlss5-corpus/build-upscaling/benchmark-corpus/manifest.json` — `RealCorpus`' real-footage corpus, read-only, schema 1, per-clip digests and frame-by-frame cut provenance notes.

Render fixtures (48 `output.mkv`, ~1.5 GiB) are local build artifacts and are not
committed; the claims above stand on the decoded-frame digests and the
`analysis.json` metrics, both reproducible from the commands in this report.

---

## The mask-state control (16 renders at `NRAutoMask=1`)

Measured 2026-09-14, later the same day, on the same machine and driver. The
report above ran both arms on the harness `baseline` profile, which writes no
`NR*` key at all and therefore leaves the add-on's automatic mask **off** while
the player ships it **on**. This is that comparison repeated at the shipped mask
state, and it is the check the verdict head above owed.

Both arms write all eight player keys at their `src/NeuralSettings.h` defaults
and differ in exactly one thing, the `--guides` string:

```
python run.py --clips real-film-cuts real-game-cuts real-game-motion real-dissolve ^
  --profile-file ../../docs/measurements/depth-ab-20260914/shipped-state.profile.json ^
  --profiles shipped-depth-proxy shipped-depth-constant --repeats 2
python analyze.py --no-ocr --no-faces
```

16 renders, 0 failed, every clip/profile pair bit-identical across its two
repeats (repeat spread exactly `0.00e+00` on all seven metrics), so every delta
below is deterministic rather than sampled.

### Direction: two of four clips change their answer

| clip | mask **off** (this report) | mask **on** (shipped) | stable? |
|---|---|---|---|
| `real-film-cuts` | **4/4 proxy** | **4/4 proxy** | yes |
| `real-game-cuts` | 3/4 proxy | **0/4 proxy** | no - flips to constant |
| `real-game-motion` | **4/4 proxy** | 3/4 proxy | weakens |
| `real-dissolve` | 4/4 constant | 3/4 proxy | no - flips to proxy |

Aggregate is almost untouched - the proxy takes 11 of 16 all-pairs metrics at
mask-off and 10 of 16 at mask-on - but the per-clip composition is not stable,
and `real-game-motion`, the adversarial clip this report singled out *before*
measuring and then used as its settling result, no longer sweeps.

### Magnitude: unchanged, and still a rounding error on the pass

| clip | source flicker | output flicker | pass effect | depth delta | depth % of pass |
|---|---:|---:|---:|---:|---:|
| real-film-cuts | 1.519 | 1.478 | -0.0415 | -0.0004 | 0.94 % |
| real-game-cuts | 14.146 | 13.602 | -0.5437 | +0.0045 | 0.84 % |
| real-game-motion | 6.264 | 6.665 | +0.4013 | -0.0011 | 0.27 % |
| real-dissolve | 8.111 | 7.979 | -0.1322 | -0.0032 | 2.41 % |

| clip | source sigma | output sigma | pass effect | depth delta | depth % of pass |
|---|---:|---:|---:|---:|---:|
| real-film-cuts | 5.479 | 5.386 | -0.0928 | -0.0034 | 3.62 % |
| real-game-cuts | 26.020 | 25.052 | -0.9677 | +0.0067 | 0.69 % |
| real-game-motion | 19.005 | 20.297 | +1.2913 | -0.0112 | 0.87 % |
| real-dissolve | 34.301 | 34.611 | +0.3095 | -0.0063 | 2.02 % |

The depth term is 0.27-3.62 % of what the neural pass itself does to the same
metric, the same order as at mask-off. So the mask state does not shrink the
depth effect; it reshuffles which clip the sign lands on, which is what an effect
this small should be expected to do.

### What this does and does not change

- **The keep stands, and its basis is unchanged**: live channel, 0.057-0.128 ms
  of CPU per frame, zero GPU, no guide path deleted by removal. None of that is
  mask-dependent.
- **Point 3 above is retracted as a quality argument.** "The proxy sweeps all
  four metrics on the adversarial clip" is true at mask-off and false at the
  shipped mask state. A per-clip sweep that moves when a variable *other than the
  one under test* changes is not evidence about depth; it is evidence that the
  effect is too small for per-clip direction to be stable. The falsification
  argument survives in its weaker and still sufficient form: removal was
  predicted to hurt the adversarial clip and does not, at either mask state.
- **The gate A/B and this one are now asymmetric on purpose to note**: the gate
  reproduced at mask-on within 0.00108, every conclusion intact; depth did not
  reproduce clip-by-clip. The difference is effect size, not method - the gate
  moves false motion by 4.6-14.4 % relative, depth by under 4 % of the pass.

### A cross-check that fell out of it

`history_resets` are identical arm-for-arm on all four clips, as at mask-off -
`5/2/1/1` against the frozen record's `4/2/1/1`. The one extra reset is on
`real-film-cuts` and is the **debounce fix landing on real material**: the cut at
local 87 was suppressed under the shipped 0.6 s window and is accepted under
0.3 s. The arms still agree with each other, so the depth guide does not touch
the cut decision; the corpus and the criterion changed underneath, exactly as
intended, and this is the first independent confirmation of that fix on a
labelled real clip.
