# Neural quality benchmark

Roadmap P0 items 2 and 3 ([DLSS5_VIDEO_ROADMAP.md](DLSS5_VIDEO_ROADMAP.md))
ask for a repeatable test set, measured neural time / FPS / VRAM / flicker /
OCR / face consistency / colour shift / determinism, blind one-pass versus
two-pass comparison, and an ablation of every guide and control. The scripts
under [`tools/benchmark/`](../tools/benchmark/README.md) implement that
against the isolated `NeuralWorker.exe`; this page is the operator's summary.

## Run it

```
python tools/benchmark/corpus.py                                     # once; ~2 min
python tools/benchmark/cutlab.py --sweep                             # scores the cut criterion itself
python tools/benchmark/run.py --profiles baseline mv-off --repeats 3 # renders
python tools/benchmark/analyze.py                                    # metrics + report.md
python tools/benchmark/blind.py                                      # A/B pairs (needs a two-pass run)
```

Outputs live under `build-upscaling/benchmark-corpus/` and
`build-upscaling/benchmark-work/` (both inside the gitignored build tree):
runs in `runs/<clip>__<profile>__<repeat>/`, the report in
`analysis/report.md`, blind pairs in `blind/pairs/` with a sealed
`blind/key.json`.

Prerequisites: Python 3.12 with `tools/benchmark/requirements.txt`,
`external/ffmpeg/bin`, a built `build-upscaling/Release/neural-runtime/`
(worker plus RenoDX/ReShade runtime), an NVIDIA GPU. Copy `Release/neural-runtime`
to `build-upscaling/benchmark-work/runtime-snapshot/` first when a build may
run concurrently; the runner prefers the snapshot.

## What is measured

| Group | Metric | Source |
|---|---|---|
| Runtime | preflight receipt: GPU, driver, ReShade/RenoDX/DLSS-NR versions, locked module hashes, Feature 18 creation/evaluation, RenoDX active settings | `--neural-preflight` probe, once per profile |
| Speed | end-to-end fps (frames / wall), processing fps (Rendering-phase progress records), neural GPU ms p50/p95/max, guide ms, capture ms | worker result over the metadata pipe (protocol v4) |
| Memory | worker peak local VRAM (receipt) and NVML whole-GPU used/util/power/temperature at 2 Hz | `result.json`, `gpu.csv` |
| Determinism | sha256 of the rgb24 per-frame MD5 sequence across repeats | `frames.md5` |
| Temporal | added flicker = mean |ΔY| between consecutive output frames minus the same for the source, cut frames excluded; added per-pixel temporal σ and its p99 over the same shots | `analyze.py` |
| Motion field | false-motion rate and cell flip rate on the guide generator's own analysis grid | `analyze.py` |
| Cuts | precision/recall/F1 of the generator's cut test against the manifest's hard-cut indices at ±1 frame, source and output | `analyze.py`, `manifest.json` |
| Colour | mean CIE76 ΔE in CIELAB and per-channel RGB shift vs source | `analyze.py` |
| Fidelity | PSNR and SSIM vs the lossless source | `analyze.py` |
| Text | rapidocr character-level ratio against the manifest's ground-truth strings, output and source side by side | text clip |
| Faces | Haar-box crops; resnet18 cosine of output crop vs source crop and frame-to-frame drift | faces clip |
| Two-pass | metric deltas vs `baseline`; `blind.py` sealed A/B stills and 3 s excerpts | `report.md`, `blind/` |

Ablation profiles (`run.py --ablation`) change one factor each: motion
vectors, depth, RenoDX automatic mask, local structure, local
tone, intensity (control), presets 1-3, styles natural/cinematic, two-pass.
Every profile uses the same corpus, the same worker priming/preroll and the same
runtime files, so differences attribute to the changed factor.

The depth A/B is `run.py --profiles depth-constant depth-proxy`. Both are names for
guide strings the matrix already carries — a disabled depth guide *is* the constant
0.75 field — so they resolve to `depth-off` and `baseline` and share their run
directories instead of rendering the same configuration twice. The third profile the
roadmap names, `depth-of`, is refused by name: `--guides` expresses exactly
`mv=0|1,depth=0|1`, so there is no way to ask the worker for depth derived from the
NVOFA structure, and no flag was invented to pretend otherwise.

### The temporal metrics

These exist to make the motion-vector and depth claims falsifiable, so each one is
defined by the script rather than by prose. All of them mirror
`src/TemporalGuides.cpp`: the cell grid is `AnalysisGrid` (width/10 cells clamped to
96–160 below 45 fps, width/14 clamped to 96–128 above it, so 160×90 cells of 12×12
source pixels for the 30-fps 1080p corpus), a cell's value is `DownsampleLuma`'s four
stratified samples in normalized Rec.709 luma, and the cut thresholds and the 0.6 s
weak-arm debounce are the generator's own. A threshold swept here therefore transfers
to the runtime unchanged.

- **Per-pixel temporal σ** (`temporal_sigma_source/output/added`, 8-bit luma levels).
  The temporal standard deviation of each pixel's luma inside a shot — the frames
  between two manifest cuts, because across a cut a pixel's spread is the edit and not
  the pass — meaned over pixels and then frame-weighted over the shots; `added` is
  output minus source, and the p99 of the same map is reported beside it. Shots shorter
  than 3 frames are dropped rather than reported as suspiciously low σ. This is the
  number the "2.23 → 1.41" claim refers to. What moves it is localized shimmer that a
  frame-global mean hides: jitter, breathing fine detail, a guide flickering on and off.
  A pass that only shifts the picture's level does not move it at all.
- **False-motion rate** (`motion_field.false_motion_rate`, fraction of cells).
  Staticness comes from the source itself: a cell is static across a consecutive pair
  when its cell luma changed by at most 2/255 — two 8-bit levels — and the rate is the
  fraction of those cells whose output changed by more than the same tolerance. That is
  motion the pass invented rather than carried. The carrier sets a floor, since an NVENC
  re-encode of a still region is not bit-exact; `intensity-0` measures that floor for
  the clip, and the rate is only meaningful against it.
- **Cell flip rate** (`motion_field.flip_rate_source/output/added`, fraction of cells).
  The fraction of cells whose moving/static verdict changes between one consecutive pair
  and the next, on the same grid and the same tolerance, source and output side by side.
  It is the instability of the motion field rather than of the pixels: a threshold only
  becomes visible once it oscillates, and a field that drops and re-acquires the same
  vector on alternate frames scores badly here while the pixel metrics look calm.
- **Cut precision/recall/F1** (`cuts.source`, `cuts.output`). The generator's own test
  run over each file's cell grids and matched to the manifest's hard-cut indices, at
  most one detection per cut within ±1 frame. The source column is a threshold check
  and is the same for every profile of a clip; the output column is what a consumer of
  the rendered file would detect. `cuts.*_evidence` records every firing frame with its
  residual, its histogram overlap, the arm that fired and whether the debounce withheld
  it. A cut-free clip has no recall to report and its detection count is its
  false-positive count. A clip may also carry `soft_cuts` spans — a dissolve, where no
  single frame is the right one — inside which the first reset is neither a hit nor a
  false positive and a second one still is.

`--sample-every` strides ΔE/PSNR/SSIM/OCR/faces alone. Every metric above sees every
consecutive frame pair whatever the stride is, and all of them are withheld whole —
`null` fields plus a `temporal_withheld` reason in `metrics.json` and a section in
`report.md` — when the output frame count does not match the source, because frame *i*
of one file is then not frame *i* of the other.

## Reading the numbers

- The worker's carrier is NVENC HEVC (software H.264 fallback). PSNR/SSIM/ΔE
  therefore include encoder loss; `intensity-0` isolates that loss.
- `peak_local_vram_mib` (worker, `IDXGIAdapter3` local budget) and NVML
  `total_used_mib` (device-wide, every process) are not comparable; report both.
- Bit-identical output between `baseline` and a guide ablation (same
  `output_digest` in `metrics.json`) means that guide has no effect on the
  active consumer for that clip; the worker reports the guide controls it
  applied in the receipt. Worker builds without timing instrumentation
  report the ms/VRAM fields as `0`.
- OCR and face scores are engine-limited: compare the output figure with the
  source figure produced by the same engine.
- Pass 2 of a two-pass profile renders the lossy pass-1 file, so two-pass
  deltas mix the model's second application with one extra encode.

## Reference run (2026-09-08, RTX 5090, driver 32.0.16.1664 / 616.64, ReShade 6.8.0.2155, RenoDX 4.7, DLSS-NR 310.8.0, worker 0.14.1 protocol v2)

Guide ablation on `text-subtitles` and `cuts-motion` (one repeat each, the three
guide profiles plus the since-removed `mask-off`), plus `faces`/`baseline` and the text clip through
`two-pass`. `resets` is the worker's `historyResets` (first frame plus every
detected cut); `digest` is the first 8 hex characters of the output framemd5
sequence so byte-identical outputs are visible directly.

| clip | profile | resets | digest | wall s | e2e fps | proc fps | neural GPU ms p50 / p95 | guide ms | capture ms | local VRAM MiB | flicker+ | ΔE | PSNR | SSIM |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| cuts-motion | baseline | 6 | 902a7347 | 13.8 | 16.32 | 32.02 | 3.27 / 3.80 | 6.4 | 6.7 | 1041 | −2.074 | 11.15 | 22.61 | 0.8153 |
| cuts-motion | mv-off | 6 | 65821a03 | 13.8 | 16.33 | 31.71 | 3.27 / 3.80 | 6.4 | 6.7 | 1041 | −2.969 | 11.14 | 22.91 | 0.8335 |
| cuts-motion | depth-off | 6 | e26be8e1 | 13.8 | 16.31 | 31.66 | 3.27 / 3.80 | 6.4 | 6.7 | 1041 | −2.091 | 11.12 | 22.63 | 0.8162 |
| cuts-motion | mask-off | 6 | 902a7347 | 13.8 | 16.33 | 31.59 | 3.27 / 3.80 | 6.4 | 6.7 | 1041 | −2.074 | 11.15 | 22.61 | 0.8153 |
| faces | baseline | 0 | fca59b4d | 14.7 | 16.34 | 31.41 | – | – | – | – | −0.702 | 3.01 | 31.94 | 0.9594 |
| text-subtitles | baseline | 1 | 4d412db9 | 13.3 | 15.82 | 31.33 | 3.26 / 3.80 | 6.4 | 6.7 | 1041 | +0.078 | 2.44 | 29.49 | 0.9875 |
| text-subtitles | mv-off | 1 | df999e3a | 13.2 | 15.93 | 31.63 | 3.26 / 3.80 | 6.4 | 6.7 | 1041 | +0.009 | 2.49 | 29.59 | 0.9880 |
| text-subtitles | depth-off | 1 | 55c6e660 | 13.3 | 15.80 | 31.35 | 3.26 / 3.80 | 6.4 | 6.7 | 1041 | +0.079 | 2.44 | 29.48 | 0.9875 |
| text-subtitles | mask-off | 1 | 4d412db9 | 13.4 | 15.69 | 31.24 | 3.26 / 3.80 | 6.4 | 6.7 | 1041 | +0.078 | 2.44 | 29.49 | 0.9875 |
| text-subtitles | two-pass | 0 | 8b82158f | 27.4 | 15.35 | 31.37 | – | – | – | – | +0.172 | 3.07 | 26.73 | 0.9772 |

Observations (P0-3 guide proof). **The table above predates the guide, capture,
segment, decode and mask work described below, it predates v0.20.0's hardware optical
flow, and it predates every temporal metric this page documents; its rates, its
`mv-off` comparison and its absent σ / false-motion / flip / cut columns are all
historical.** Rerun it before quoting a number from it: `analyze.py` produces those
columns now, so the rerun is one command and the comparison it then allows is the
whole reason the staleness matters.

- Rerenders are bit-identical across repeats (framemd5 sequence digests match).
- The mask guide is gone, and the `mask-off` rows are why. `mask-off` was
  byte-identical to `baseline` on both clips, and the mask was not missing: the
  generator produced a mask with 5–24 % of cells non-zero on `cuts-motion` (0 %
  on the static text clip, which is correct), reaching NGX as the R8 texture
  bound to `DLSS_Input_Bias_Current_Color_Mask`, `DLSS_DisocclusionMask` and
  `DLSS_ResponsivityMask`. Three later experiments closed the question: forcing
  the mask to **all ones** is also pixel-identical; **render preset Default
  versus preset K** is pixel-identical too, so the preset hints are inert for
  this feature as well; and on the **DLSS-SR upscaling path** — a different NGX
  feature — mask on versus off is 0 differing bytes over 24 frames at 1440p
  while motion vectors on versus off differ in 6.85 % of bytes at 43.1 dB. Two
  of the three parameter names were never SuperSampling inputs at all: they
  belong to Ray Reconstruction. The guide was deleted.
- `mv-off` beating `baseline` in this table was real, and it was the motivation
  for rewriting the flow estimator. The vectors were coarse (160×90 grid, 12 px
  steps) and 60.8 % of cells on `cuts-motion` carried a vector that did not
  reduce the warp residual. With acceptance by evidence margin, a banded reverse
  check and a confidence-weighted vector median, that false-motion rate is 3.7 %,
  temporal stability on the static text clip is 0.3292 against `mv-off`'s 0.3342
  and the old 0.4101, and PSNR-Y recovers +0.297 dB on `cuts-motion`. Motion
  vectors now help; re-measure this table to see it.
- Depth changes the output but only within 0.02 dB, and an incidental finding
  during the flow work is that changing the flow field does not change the
  render at all through the depth channel — the proxy is dominated by its
  vertical ramp. Depth is the next guide worth questioning.
- The cut detector does **not** fire exactly on the clip's hard cuts, and the sentence
  that used to stand here was wrong twice over: `cuts-motion` has **four** hard cuts
  (frames 45/90/135/180), not five, and `resets` = 6 is one first-frame reset plus
  **five** accepted cuts — an over-reset at frame 91, one frame after the frame-90 cut.
  Measured with `cutlab.py`, below.
- Per-frame cost has changed substantially since these runs: the neural pass is
  still ~3.3 ms GPU per 1080p frame, but guide generation is 3.1 ms (was 6.4),
  capture is 5.7 ms (was 6.7), and steady-state throughput is 12.50 ms/frame at
  1080p, 16.60 at 1440p and 28.07 at 4K. See `docs/ARCHITECTURE.md`.
- The second pass at `NRIntensity=0.75` adds flicker (+0.094), ΔE (+0.63)
  and costs 2.8 dB PSNR and 11 OCR points on small text; face crops keep a
  0.952 mean cosine to the source with frame-to-frame drift equal to the
  source's own (0.137 vs 0.133). Two-pass is not a default candidate.

## The scene-cut criterion, measured (2026-09-14, CPU only)

`cutlab.py` scores the criterion itself rather than a render: no GPU, no worker, no
neural pass. It replays `cutmirror` — the shared Python mirror of
`src/TemporalGuides.cpp` that `analyze.py` also uses — over the corpus's cell grids
and matches every accepted history reset against the manifest.

```
python tools/benchmark/corpus.py --corpus <dir>          # 9 clips, ~2 min
python tools/benchmark/cutlab.py --corpus <dir> --sweep  # 1212 pairs, ~30 s cached
```

The labelled set grew for this: five clips exist only to be got right. `cuts-similar`
hard-cuts between four mirrorings of one fractal still, so the shots share a luma
histogram by construction and only correspondence can see the cut; `pan-fast` travels
6.1 analysis cells per frame and `zoom-fast` goes 1.0× → 2.2× in 1.5 s, neither of
which is a cut; `dissolve` cross-fades over 0.7 s, where one reset is right and two
are not; `flash-exposure` has a four-frame flash and a sustained exposure step, and
the scene never changes.

**What the shipped 0.30 / 0.10 / 0.85 criterion does** (9 clips, 1212 consecutive
pairs, 7 labelled hard cuts):

| clip | truth | accepted resets | missed | false positives |
|---|---|---|---:|---|
| cuts-motion | 45, 90, 135, 180 | 45, 90, **91**, 135, 180 | 0 | 91 |
| cuts-similar | 30, 60, 90 | none | 3 | – |
| flash-exposure | none | 30, 60 (34 suppressed) | 0 | 30, 60 |
| text-subtitles, fine-detail, highlights-gradients, pan-fast, zoom-fast, dissolve | none | none | 0 | – |

Pooled precision 0.571, recall 0.571, F1 0.571. Three findings:

- **The frame-91 over-reset is real.** Frame 90 cuts into the `life` automaton, and
  frame 91 is that automaton's first generation step: residual 0.3739, overlap 0.1282.
  The strong arm fires and, by design, is never debounced, so DLSS gets `Reset=1` twice
  in two frames. It is the sixth reset the reference table reports.
- **A cut between similar shots is invisible.** All three `cuts-similar` cuts land at
  residual 0.185–0.213 with overlap 0.88–0.98: under the 0.30 strong arm and over the
  0.85 histogram gate, so neither arm fires.
- **A flash is indistinguishable from a cut.** Residual 0.25 with overlap 0.44 is
  exactly the weak arm's shape. The 0.6 s debounce catches the frame that ends the
  flash (34) but not the one that starts it (30), nor the exposure step at 60.

**Roadmap survey item 3, the scale-free candidate.** A cell's match failed when its
winning displacement cost exceeds `ratio` × its standing-still cost — the
no-prediction baseline `EstimateFlow` computes and discards — among cells whose
standing-still cost clears `floor`; the decision is the *fraction* of failed cells
(x265 `scenecut-bias`, mvtools `thSCD2`) rather than a mean residual. Implemented in
`cutmirror.FailedFractionCriterion`, swept over 1350 points against the residual
family's 198:

| family | best F1 | points with no missed cut and no over-reset | fewest false positives there | clips still wrong |
|---|---:|---:|---:|---|
| residual (shipped shape) | 0.875 | 2 | 2 | flash-exposure |
| failed fraction | 0.875 | 89 | 2 | flash-exposure |

**The candidate is not better, so nothing in `src/` changed.** Both families reach the
identical best operating point — every labelled cut found, no over-reset, and the same
two false positives on the same clip — so the extra per-cell state a fraction needs in
`EstimateFlow` would buy nothing. Neither score even orders the set correctly: the
weakest true cut is 0.1853 residual against a 0.3739 non-cut, and 0.5661 failed
fraction against a 0.7297 non-cut. Both families are carried by the two-arm split and
the debounce, not by the score.

The shipped thresholds were left alone for the same reason. The residual sweep's best
point is `residual > 0.40`, or `> 0.13` with **no** histogram gate — and the gate is
what protects the corpus's own fast pan, which reaches residual 0.1252 on
`cuts-motion` frame 39, a 3.8 % margin under that 0.13. Raising the strong arm to 0.40
would also demote all four `cuts-motion` cuts (0.242–0.397) to the debounced weak arm,
trading one documented over-reset for an unmeasured missed-cut risk on fast-cut
footage. Seven labelled cuts over nine synthetic clips is not enough evidence to do
either.

What would settle it is footage this corpus cannot synthesize: real grain, real motion
blur, real dissolves, and a shot-boundary set large enough that a three-parameter grid
search is not fitting seven positives.

## What the harness measures now, and what it still cannot

The four numbers this project used to quote from ad-hoc runs are scripted: false-motion
rate, per-pixel temporal σ with its p99, cell flip rate, and cut precision/recall
against the manifest's ground truth. `analyze.py` computes all four inside the single
streaming pass it already made over each run, and `report.md` carries them per
clip/profile, as deltas against `baseline` for every profile whose guide string
differs, and as a per-run cut table. The 60.8 % → 3.7 % false-motion figure and the
2.23 → 1.41 σ figure quoted above and elsewhere in these docs predate that script and
were measured differently; they stand as history until a rerun replaces them with the
script's own numbers.

What still cannot be decided by measurement here: the NVOFA cost gate has no published
scale, so calibrating it wants the forward/backward agreement mask rather than another
invented threshold; `IsHDR` on the linear FP16 input, and a supplied 1×1 exposure
texture against auto-exposure, are A/Bs nobody has run; and depth is now answerable as
`depth-constant` versus `depth-proxy` but the OF-structure candidate has no argument
behind it to run. The VSR literature's warp-error metric remains the shape to copy for
a stronger false-motion number than a luma tolerance can give: the flow rejection
thresholds were chosen by warping the previous frame's full-resolution pixels and
scoring the residual, which works just as well pointed at DLSS output.

To A/B a guide against the **upscaling** feature rather than neural rendering,
`UpscalingGpuSmoke` takes a probe form:
`UpscalingGpuSmoke.exe <clip> <targetHeight> <out.raw> mv=1,depth=1 <frames>`
writes every captured output frame as raw BGRA, so two runs can be compared byte
for byte. That is how the mask question was settled for DLSS-SR.

## Which neural settings change the image (2026-09-09, same stack)

Measured through the player, not the benchmark driver: with playback paused, each
control was changed one at a time and the debounced single-frame preview it
triggers was rendered. Every preview is an ordinary range render, so each
distinct combination has its own cache entry; the entries were paired from the
worker's `Checking neural cache key=… settings=…` log lines and compared as
decoded rgb24 bytes, so every row below differs in exactly one field at the same
source frame.

| control | change | bytes differing | mean abs delta |
|---|---|---:|---:|
| Intensity | 1.00 → 0.40 | 84.75 % | 2.40 |
| Local tone | 1.00 → 0.30 | 66.45 % | 1.14 |
| Local structure | 1.00 → 0.30 | 53.41 % | 0.87 |
| Style | 2 → 0 | 49.31 % | 0.79 |
| Motion-vector guide | on → off | 39.79 % | 0.63 |
| Depth guide | on → off (motion on) | 37.92 % | 0.57 |
| Skin structure | +1.00 → −0.40 | 36.50 % | 0.56 |
| Automatic mask | on → off | 36.08 % | 0.54 |
| **Color strength** | 1.00 → 0.20 | **0 %** | 0 |
| **Render preset** | 0→1, 0→2, 0→3, 1→3 | **0 %** | 0 |

Six of the eight model parameters and both guides reach the output. **Color
strength and render preset do not**, on any of the four preset pairs tried and
from two independent baselines. This is not a plumbing fault on our side, and
the add-on is not dropping the value either: driving the runtime `ReShade.ini`
directly and rendering one frame per value through
`NeuralWorkerTests --real-worker`, the add-on's own log reports back
`preset=0`, `preset=1` and `preset=3` to match, and every pair still decodes to
0 differing bytes. So the hint reaches NGX and the model ignores it — the same
shape of result as the deleted mask guide and the inert SR preset hints.

What the preset is meant to be: the dialog's `Default, 1, 2, 3` are passed as
`NRPreset` in `[RenoDX.DLSS5]`, which the add-on maps to the NGX parameter
`DLSSNR.Hint.Render.Preset` — the neural-rendering analogue of DLSS-SR's render
presets, a request for a particular trained variant. NVIDIA publishes no meaning
for the NR values, which is why the combo is labelled with bare numbers.
`matiasLombo/neural-upstream` reports the same parameter "turned out to be
inert", independently of this measurement. Note also that the add-on exposes a
`DLSSNR.UICorrection` control this player does not, which has not been tested.

The depth guide only matters while motion vectors are on: with `mv=0`, toggling
depth changes nothing (0 %), which is what a temporal consumer with no
reprojection to perform should do. An earlier reading that called depth inert had
motion vectors already off.

Cost of a change: settings and guides are part of the render identity, so every
distinct combination is rendered from scratch — 16 cold single-frame previews
took a median of **10.6 s** each, and the 4 repeats of an already-rendered
combination came back in **1.03 s** from cache. Changing color strength or the
preset therefore costs a full re-render and produces byte-identical output.
