# Does the reconstructed depth proxy pay for itself? — 14 September 2026

**Verdict: undecided pending real footage. Keep the proxy in place meanwhile.**
Roadmap item Q3 asked whether `TemporalGuides::BuildDepthProxy` earns the guide
path it occupies. Two arms, four labelled synthetic clips, two repeats each, all
32 renders bit-identical: the depth channel is unambiguously *live* — it changes
output pixels on every clip — and it is unambiguously *cheap*, costing 0.057–0.128
ms of CPU per frame and exactly nothing on the GPU. What four synthetic clips
cannot establish is whether its geometric prior is *informative*, because none of
them has scene geometry for the prior to be right or wrong about. Three clips lean
proxy, one leans constant, and every delta is well under a percentage point. That
is not a keep and it is not a removal; it is a decision that needs footage, which
`RealCorpus` is producing in this same wave.

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
| Corpus | schema 1, generated `2026-09-14T11:04:05Z`, FFmpeg `9.0.1-essentials`, matroska/ffv1 level 3 yuv420p | `manifest.json` |
| GPU contention | **the citable batch was rendered on a claimed idle GPU** (window granted on `hub`, all three sibling slices acked idle; released after 117 s) | `hub` transcript |

The driver is the neural floor (610.47) rather than the recommended pin (616.64),
so nothing here speaks for the pinned driver.

## Gates

- **32/32 renders succeeded**, 0 failed, across both batches (`run.py` reported `0 failed` each time).
- **Every frame was neural in every run**: `verified_neural_frames == frame_count` for all 16 idle runs (225, 120, 90, 30 per clip) with `frame_retries = 0`. No run fell back to a non-neural path, so both arms are compared on fully neural output.
- **Preflight passed for both profiles**: feature 18 `created`/`evaluated`/`armed`, `upscalingOff`, `inlineInterception`, `laterFailure = false`, diagnosis `cause: none`.
- **`deterministic: true` for all 8 clip/arm pairs**, and identical digests across the two batches — see the determinism section.
- **The worker's own cut receipts agree arm-for-arm on all four clips**: `accepted_strong/weak/suppressed` and `history_resets` are `4/1/0` + 6 (cuts-motion), `0/0/0` + 1 (cuts-similar), `0/2/1` + 3 (flash-exposure), `0/0/0` + 1 (pan-fast) in *both* arms.
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

Render and score, both from `tools/benchmark`:

```
python run.py --corpus C:/Users/User/Documents/GitHub/dlss5-depth/build-upscaling/benchmark-corpus ^
  --clips cuts-motion cuts-similar flash-exposure pan-fast ^
  --profiles depth-proxy depth-constant --repeats 2

python analyze.py --corpus C:/Users/User/Documents/GitHub/dlss5-depth/build-upscaling/benchmark-corpus ^
  --runs C:/Users/User/Documents/GitHub/dlss5-depth/build-upscaling/benchmark-work/runs ^
  --no-ocr --no-faces --force
```

Two notes for anyone reproducing this:

- `--fresh-profiles` is unsafe here. It `rmtree`s the profile clone before *every*
  run, which races the OS releasing the just-exited worker's DLL mappings; the
  fourth run died with `PermissionError: [WinError 32] ... neural-runtime`. The
  clone's contents depend only on the profile, so one clone per profile is all the
  isolation an A/B needs. Clear `benchmark-work/` once up front instead.
- The render batch was run **twice**: once contended (four slices sharing the GPU)
  and once on a claimed idle GPU. The contended batch is retained at
  `benchmark-work/runs-contended` and `analysis-contended` as the cross-check
  described below. Every number in this report comes from the idle batch.

## Determinism: repeats are bit-identical, and so is the whole batch under a different machine load

`analyze.py` reports `deterministic: true` for all **8** clip/arm pairs. Stronger
than that, the 16 idle-GPU runs and the 16 contended runs produce **exactly 8
distinct `output_digest` values — one per clip/arm — with all 16 pairs matching
across the two batches**, and every one of the 12 compared quality metrics is
identical between batches to full stored precision. 32 renders, 8 digests.

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

Two consequences, and they pull in opposite directions:

1. **A sub-point delta in the table below is signal, not run noise.** There is no
   run-to-run variance to hide behind: rerunning reproduces the identical frames
   on a busy box and on a quiet one. Every quality delta reported here is exactly
   reproducible.
2. **The digests differ between arms on all four clips, so the depth channel is
   genuinely live.** This is not a no-op binding being measured; the neural pass
   reads the depth texture and its output changes. That disposes of the cheapest
   possible removal argument ("it does nothing").

What determinism does *not* buy is content generality. Each arm was sampled on 4
clips, not 4 clips × N independent noise draws, so the spread that matters for the
verdict is **across clips**, and it is a spread that changes sign.

## The table

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

### Reading the table

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

## Verdict: undecided pending real footage; keep the proxy meanwhile

Of the three honest verdicts, this is the one the evidence supports.

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
80/20. The four clips this wave can offer are, per the corpus manifest, five
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

## `UNEXERCISED`

| Item | Why not exercised |
|---|---|
| **`depth-of` third arm** (depth from NVOFA structure) | Refused by name in `run.py`'s `UNAVAILABLE` table: "depth would have to come from the gated NVOFA structure, and `--guides` selects only `mv=0\|1,depth=0\|1` — there is no depth source to ask for". Explicit non-goal; no player flag was invented for it. |
| **Real footage** | Not available in this tree's corpus this wave; `RealCorpus` owns it. This is the gap the verdict turns on, not a nice-to-have. |
| **A learned depth model** | Explicit non-goal. Nothing here speaks to what a real monocular-depth network would be worth. |
| **Scene geometry of any kind** | All four clips are synthetic fractals, gradients, cellular automata and mirrored stills. The proxy's `yn` prior has no ground truth here. |
| **OCR / text legibility** | `--no-ocr`; also `text-subtitles` is not one of the four labelled clips. Whether a flat depth field harms text sharpening is unmeasured. |
| **Faces** | `--no-faces`; the corpus builder reported `face fixture missing: build-upscaling/runtime-comparison-20260907/fixtures/mafia-60s.mkv`. Face identity/drift under either arm is unmeasured. |
| **Soft cuts / dissolves** | `dissolve` carries `soft_cuts` labels and is outside the four labelled clips assigned here. |
| **Cut precision/recall as depth evidence** | Structurally insensitive to depth (`ClassifySceneCut` runs before `BuildDepthProxy` and reads only luma cost and histogram overlap). Measured and reported as ties; carries no signal. |
| **PSNR/SSIM/dE at full frame rate** | Sampled at stride 15 — 15/8/6/2 frames per clip. `pan-fast`'s fidelity row in particular rests on 2 frames. Not re-run at stride 1. |
| **4K, HDR, 60 fps, other GPUs/drivers** | One 1080p30 SDR path on one RTX 4080 SUPER at driver 610.47. |
| **Perceptual / human judgement** | No viewer comparison was made. Every claim is a metric claim, and all the deltas here are below any plausible visibility threshold. |
| **`ctest`** | Not run: this slice changed no `src/` or `tests/` file. Documentation only. |

## Artifacts

- `build-upscaling/benchmark-work/runs` + `analysis` — the idle-GPU batch every number above comes from.
- `build-upscaling/benchmark-work/runs-contended` + `analysis-contended` — the contended batch, retained solely as the cross-batch determinism cross-check. Its timings are `CONTENDED — not citable`.
- `build-upscaling/benchmark-corpus/manifest.json` — corpus schema 1, per-clip digests.

Render fixtures (16 + 16 `output.mkv`, ~1 GiB) are local build artifacts and are
not committed; the claims above stand on the decoded-frame digests and the
`analysis.json` metrics, both reproducible from the commands in this report.
