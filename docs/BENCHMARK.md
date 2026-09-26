# Neural quality benchmark

_Verified against 0.26.2 (b965b53) on 2026-09-26._

The benchmark exists to give quality claims a repeatable test set: measured
neural time / FPS / VRAM / flicker / OCR / face consistency / colour shift /
determinism, blind one-pass versus two-pass comparison, and an ablation of
every guide and control. The scripts under
[`tools/benchmark/`](../tools/benchmark/README.md) implement that against the
isolated `NeuralWorker.exe`; this page is the operator's summary.

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

Every measurement on this page was taken against RenoDX 4.70 and DLSS-NR
310.8.0, except the settings table at the end, which was re-measured on the pinned
RenoDX 6.5.3 on 2026-09-24. The rest are the last known state rather than the
current one; the pages below say per section which of them a rerun would be
expected to move.

Prerequisites: Python 3.12 with `tools/benchmark/requirements.txt`,
`external/ffmpeg/bin`, a built `build-upscaling/Release/neural-runtime/`
(worker plus RenoDX/ReShade runtime), an NVIDIA GPU. Copy `Release/neural-runtime`
to `build-upscaling/benchmark-work/runtime-snapshot/` first when a build may
run concurrently; the runner prefers the snapshot.

### The corpus, and where it stops being synthetic

`corpus.py` carries three kinds of builder, and `manifest.json` - not this page -
is the count that is true for a given checkout. Synthetic clips (seeded FFmpeg
sources, pinned colours, no camera) carry `"synthetic": true` and build
everywhere. Camera-original clips are downloaded by video id through
`fetch_camera_original.ps1`, and each skips itself with a warning when its
source is absent, as `faces` - which needs an external fixture - always has.
The `"category": "real"`, `"synthetic": false` clips are cut from this
repository's own demo capture,
`tools/benchmark/fixtures/demo-capture-20260912.mp4` (1920×1080, 30 fps, h264, 22.6 s, tracked in
git). Until 22 September 2026 that file was `docs/media/neural-comparison-demo.mp4`; the
README demonstration was then replaced, and the capture moved here byte-identical (git
blob `dc9527b7`, SHA-256 `ac46f10a…`), so reports that name the old path mean this file. They exist because every quality conclusion here used to rest on mandelbrot
zooms and cellular automata, and because they need no fixture to fetch and add
nothing to redistribute that the tree does not already carry.

| clip | frames | labelled | what it is |
|---|---:|---|---|
| `real-film-cuts` | 102 | cuts 20, 47, 70, 87 | four trailer shots — hands over a bedspread, a car on a road, a man in a crowd, a revolver firing — with grain, motion blur and a two-frame muzzle flash inside one shot, then a **static paused player frame** from local 87 on. The cut at 87 is the demo composition's scene boundary, not a film edit |
| `real-game-cuts` | 68 | cut 32 | a race exterior hard-cut to a store interior, both with the game's own static HUD over fast camera motion |
| `real-game-motion` | 76 | none | one continuous shot: a character runs off a rooftop and falls, so the camera translates while the subject occludes and disoccludes background throughout |
| `real-dissolve` | 66 | soft cut 15–37 | the two shots above cross-faded over 0.7 s, held 0.5 s before and 1.0 s after |

The demo is a screen capture of the player, so only half of each frame is footage.
The clips are cropped to the player's video surface — `crop=1362:766:502:126`,
measured as the only region whose temporal standard deviation is nonzero while the
capture plays video — and upscaled to 1920×1080 with lanczos so the corpus stays one
resolution. The crop is not cosmetic: the static chrome is half the frame, and left
in it would dominate the residual the cut test reads while inflating the static-cell
denominator false motion is divided by. The upscale is one concession — grain and
h264 texture survive it softened, not sharpened — and it is why these clips are
never called pristine footage.

**These are NR-processed captures, not real footage, and the difference is
load-bearing.** Every frame of the capture was taken with neural rendering enabled -
the toggle reads `Neural Rendering · On` and the status bar `Neural rendered · Source
2560×1440` - so the video surface is the player's own DLSS-NR output on an RTX 5090.
The full chain a `real` clip's pixels have been through is: source video → DLSS-NR →
the player's 1442×932 window → `gdigrab` screen capture → an x264 re-encode at crf 17
(by the since-replaced `prepare-inputs.ps1`) → Remotion's own h264 encode → `crop` →
lanczos upscale to 1920×1080 → FFV1 → **and then the neural pass again** when the
benchmark renders it. That is two h264 generations before the corpus encode, not one.
Re-rendering therefore measures the pass on pixels it has already touched, at a
different resolution and on different silicon, with the grain being post-pass grain
softened twice by resampling.

For an A/B where both trees see byte-identical input this is sound, and it is what
the gate and depth comparisons rest on. What it does not support is the phrase "real
footage": say **NR-processed captures**. In particular the `intensity-0` control
measures the carrier against a source that already carries NR relighting, so the
"share attributable to the neural pass" it computes is a share of a second pass over
a first one. The camera-original sources - 2560×1440 downloads named in
`docs/media/README.md` - are not in the repository and the `faces` fixture is not on
this machine, so nothing here closes that gap.

**One clip is 37 % motionless, for two separate reasons.** `real-film-cuts` ends
where the capture pauses before its magnify/wipe demonstration: source frames
102-116, local 87-101, are a single still frame repeated (consecutive mean |ΔY| ≤
0.02 against 67.5 at the cut into it), so its last 15 frames are a static image. The
labelled cut at local 87 is therefore the demo composition's scene boundary rather
than a film edit, and the fifth "shot" is a paused player frame. On top of that the
Godfather source is a 23.976 fps trailer captured at 30 fps, so roughly one frame in
five is a capture duplicate. Measured over the clip's 101 consecutive pairs, **37 %
carry no motion at all** (<0.10 mean |ΔY|): 14 from the frozen tail and about 23 from
frame-rate duplication. Both feed the static-cell population that false motion is
divided by, which makes this clip's false-motion figures the least comparable of the
four - it affects both trees identically, so the A/B stays valid, but the *level* is
not comparable across clips.

No split-screen, divider or UI chrome is inside any measured clip, and that was
settled from the composition rather than inferred: `tools/demo-video/src/index.tsx`
puts uninterrupted playback in frames 0-101 and 438-581 and the paused compare
scenes in 102-269 and 270-437, at 30 fps. `real-film-cuts` stops at 116, one frame
before the magnification (116→117 jumps by 25.0); both game ranges sit wholly inside
the second playback scene, checked by eye at frames 450 and 540. The overlay chips,
headings and progress bar all fall outside `crop=1362:766:502:126`.

**Every cut index was verified, not proposed.** FFmpeg scene detection on the cropped
surface proposed the boundaries; each one was then confirmed by extracting every frame
of the clip and inspecting it, and each `notes` field records how. The four
`real-film-cuts` indices scored 0.72/0.66/0.52/0.53 and are the only pairs in 102
frames where the shot changes; the eight smaller detector flags inside that clip
(0.032–0.076) are hand motion, a pan or the muzzle flash, and are deliberately not
labelled. A wrong ground-truth index silently corrupts every cut precision/recall
number computed afterwards, which is why "the detector said so" is not accepted here.

**The capture contains no dissolve.** All 678 frames were differenced: every
transition in it is a single-frame jump. `real-dissolve` therefore has real material
and a synthesised transition, and says so in its `notes` — the fade is in `soft_cuts`,
never in `cuts`, because no single frame of a dissolve is the one where history stops
being valid. Real grain, real motion blur and a camera that occludes are now in the
corpus; a real dissolve still is not.

## What is measured

| Group | Metric | Source |
|---|---|---|
| Runtime | preflight receipt: GPU, driver, ReShade/RenoDX/DLSS-NR versions, locked module hashes, Feature 18 creation/evaluation, RenoDX active settings | `--neural-preflight` probe, once per profile |
| Speed | end-to-end fps (frames / wall), processing fps (Rendering-phase progress records), neural GPU ms p50/p95/max, guide ms, capture ms | worker result over the metadata pipe (protocol v6) |
| Memory | worker peak local VRAM (receipt) and NVML whole-GPU used/util/power/temperature at 2 Hz | `result.json`, `gpu.csv` |
| Determinism | sha256 of the rgb24 per-frame MD5 sequence across repeats | `frames.md5` |
| Temporal | added flicker = mean |ΔY| between consecutive output frames minus the same for the source, cut frames excluded; added per-pixel temporal σ and its p99 over the same shots | `analyze.py` |
| Motion field | false-motion rate and cell flip rate on the guide generator's own analysis grid | `analyze.py` |
| Cuts | precision/recall/F1 of the generator's cut test against the manifest's hard-cut indices at ±1 frame, source and output | `analyze.py`, `manifest.json` |
| Colour | mean CIE76 ΔE in CIELAB and per-channel RGB shift vs source | `analyze.py` |
| Fidelity | PSNR and SSIM vs the lossless source | `analyze.py` |
| Text | rapidocr character-level ratio against the manifest's ground-truth strings, output and source side by side | text clip |
| Faces | Haar-box crops; resnet18 cosine of output crop vs source crop and frame-to-frame drift | faces clip |
| Two-pass | metric deltas vs `baseline`; `blind.py` sealed A/B stills and excerpts, each clipped to the shot it starts in (`--seconds` is a cap, not a length) | `report.md`, `blind/` |

Ablation profiles (`run.py --ablation`) change one factor each: motion
vectors, depth, RenoDX automatic mask, local structure, local
tone, intensity (control), presets 1-3, styles natural/cinematic, two-pass.
Every profile uses the same corpus, the same worker priming/preroll and the same
runtime files, so differences attribute to the changed factor.

**`baseline` is not the shipped configuration, and art-knob ablations must be read
against the shipped one.** `baseline` writes no `NR*` key at all, so the add-on
applies its own defaults - and its automatic mask defaults OFF, while the player
writes `NRAutoMask=1` on every render (`src/main.cpp`, from `NeuralSettings{}`).
Measured 2026-09-14 on four clips, both repeats, bit-identical by output digest:
writing all eight keys with the mask off reproduces `baseline` exactly, and
writing the single key `NRAutoMask=1` reproduces the shipped state exactly. So the
`automask-off` ablation row is a no-op against `baseline`, and every absolute
`baseline` number in this document - including the reference run below - describes
a configuration the player never ships. Relative guide comparisons are structurally
unaffected, because both sides carry the same mask state - and on the real clips
that was checked rather than assumed: the whole gate A/B re-rendered at
`NRAutoMask=1` reproduces every conclusion it reached at mask-off, with false
motion shifting by at most 0.00108 between the two states.

The sign can invert on the difference: `structure-0` improves delta-E by 2.46 on
`cuts-similar` measured against `baseline` and worsens it by 2.94 measured against
the shipped state. Run art-knob ablations from
`docs/measurements/art-defaults-20260914/shipped-state.profile.json`, which writes
all eight keys and varies one. `NRAutoMask` is also the only one of the eight whose
explicit write changes a pixel; the other seven at shipped values are bit-identical
to writing nothing, which proves only that the add-on agrees with us about them.

**Synthetic clips inflate an effect's magnitude, and can point the wrong way on its
direction.** Three independent measurements on 2026-09-14 converged on the magnitude
half. `NRLocalTone`'s full range moves delta-E by 0.77 on the NR-processed capture
and 4.02-8.11 on fractals, 5-11x. The automatic mask shifts false motion by at most
0.00108 on the real clips against ±0.0093 synthetic, one to two orders. And cut
precision/recall goes 0.571/0.571 synthetic to 1.000/1.000 real, the same bias from
the other end, because those synthetic clips were built adversarial on purpose. Part
of it is the carrier: 1.26 delta-E of a shipped 2.48 is the NVENC floor before the
model contributes anything, so effects shrink toward that floor on footage.

Direction survived in the art-knob and mask cases - same sign, smaller size. It did
**not** survive for the round-trip gate, which is the exception that sets the rule's
strength: synthetic clips said the gate raises false motion on three of four, the
captures said it lowers it on four of four. So a synthetic result is evidence about
direction, not proof of it, and it is never evidence about size.

The practical consequence is how to read this document. Synthetic patterns are sound
for regression-gating a change and for forming a hypothesis about direction; they are
unsound for **sizing** an effect, and unsound for tuning a threshold on pooled
numbers. That is why the synthetic and real cut tables here are never pooled, and it
is the argument against retuning the 0.30 / 0.10 / 0.85 criterion on a pooled set.

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
stratified samples in normalized Rec.709 luma, and the cut thresholds and the 0.3 s
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
  On RenoDX 6.5.3 that holds only with `NRNormGovernor` 0 or 1: the add-on's
  default governor (2, stable) makes repeats differ wherever it moves (see the
  settings table below and `docs/measurements/governor-20260924/REPORT.md`, after
  which the player pins 1).
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
  (That SR reading predates hardware optical flow. From 0.20.0 the resolve pass
  wrote the motion texture whenever the engine came up, and it did not read the
  guide switch, so `mv=0` stopped changing anything on that path until the switch
  was wired into the pass - see the CHANGELOG. Re-measured after that fix on an
  RTX 4080 SUPER, 12 frames at 1440p: 5.9 % of bytes.)
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
python tools/benchmark/corpus.py --corpus <dir>          # 13 clips, ~2 min
python tools/benchmark/cutlab.py --corpus <dir> --sweep  # 1212 pairs, ~30 s cached
```

The labelled set grew for this: five clips exist only to be got right. `cuts-similar`
hard-cuts between four mirrorings of one fractal still, so the shots share a luma
histogram by construction and only correspondence can see the cut; `pan-fast` travels
6.1 analysis cells per frame and `zoom-fast` goes 1.0× → 2.2× in 1.5 s, neither of
which is a cut; `dissolve` cross-fades over 0.7 s, where one reset is right and two
are not; `flash-exposure` has a four-frame flash and a sustained exposure step, and
the scene never changes.

**What the shipped 0.30 / 0.10 / 0.85 criterion does** (the nine synthetic clips, 1212
consecutive pairs, 7 labelled hard cuts — the four `real` clips were added later and
are scored separately below):

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

**On the nine synthetic clips at the 0.6 s window the candidate was not better, so
nothing in `src/` changed.** Both families reached the identical best operating point
— every labelled cut found, no over-reset, and the same two false positives on the
same clip — so the extra per-cell state a fraction needs in `EstimateFlow` would have
bought nothing. Neither score even orders that set correctly: the weakest true cut is
0.1853 residual against a 0.3739 non-cut, and 0.5661 failed fraction against a 0.7297
non-cut. Both families were carried by the two-arm split and the debounce, not by the
score.

**That conclusion no longer holds on the corpus as it stands, and it is recorded here
rather than acted on.** Re-scored 2026-09-14 over all thirteen clips at the shipped
0.3 s window, the failed-fraction family strictly dominates: **P 0.857 / R 1.000 /
F1 0.923** (12 true, 2 false, none missed) against the shipped residual criterion's
**0.750 / 0.750 / 0.750** (9 true, 3 false, 3 missed). It suppresses the
`cuts-motion` frame-91 over-reset - under that criterion 91 is a *weak*-arm fire, so
the debounce reaches it - and it catches all three `cuts-similar` cuts the residual
arm misses entirely. Its only remaining errors are `flash-exposure`'s two, which both
families share.

**Settled 2026-09-14 on a corpus with more real boundaries, and the answer is that
the criterion stays.** The corpus grew from thirteen clips to twenty: seven
camera-original clips carrying 17 frame-verified hard cuts joined the four
NR-processed captures, which is what this paragraph asked for. (Two further
camera-original clips, `orig-film-motion-a` and `orig-film-motion-b`, landed after
this sweep, so `corpus.py --check` now verifies nine.) Re-swept over all
twenty at the shipped window, the aggregate still favours the candidate - F1 0.931
against 0.897 - and split by provenance it inverts:

| criterion | camera-original (7 clips, 17 cuts) | NR-capture (4 clips, 5 cuts) | synthetic (9 clips, 7 cuts) |
|---|---|---|---|
| shipped residual | 17/17, no miss, no false positive, no over-reset | 5/5, clean | 4/7, 3 missed, 3 false positives, 1 over-reset |
| failed fraction | 15/17, two missed on `orig-film-cuts-a` | 5/5, clean | 7/7, no miss, 2 false positives |

On all 22 real labelled cuts the shipped criterion is perfect and the candidate
misses two of them; the candidate's aggregate advantage comes entirely from the
synthetic half. That is this document's own rule about pooled numbers, arriving as
a worked example on the very question that produced it, so the expired conclusion
is retired rather than acted on. For completeness, with no missed cut and no
over-reset the residual family reaches exactly one operating point (5 false
positives; best F1 anywhere 0.949) and the failed-fraction family reaches ten
(fewest 2 false positives; best F1 anywhere 0.967) - it is the better family on
the pooled set and the worse one on real footage. Full tables:
[camera-original report](measurements/camera-original-20260914/REPORT.md).

The shipped thresholds were left alone for the same reason. The residual sweep's best
point is `residual > 0.40`, or `> 0.13` with **no** histogram gate — and the gate is
what protects the corpus's own fast pan, which reaches residual 0.1252 on
`cuts-motion` frame 39, a 3.8 % margin under that 0.13. Raising the strong arm to 0.40
would also demote all four `cuts-motion` cuts (0.242–0.397) to the debounced weak arm,
trading one documented over-reset for an unmeasured missed-cut risk on fast-cut
footage. Seven labelled cuts over nine synthetic clips is not enough evidence to do
either.

### The same criterion on real footage (2026-09-14, CPU only)

The four `real` clips were added afterwards and scored separately, five labelled hard
cuts over 312 frames. Both rows below were measured at the **0.6 s** weak-arm window
that shipped at the time; the window is now **0.3 s**, and the re-measurement follows.

| criterion | P | R | F1 | false pos | missed |
|---|---:|---:|---:|---:|---:|
| residual (shipped shape), 0.6 s window | 1.000 | 0.800 | 0.889 | 0 | 1 |
| failed fraction, 0.6 s window | 1.000 | 0.800 | 0.889 | 0 | 1 |

| clip | truth | accepted resets | missed | false positives |
|---|---|---|---:|---|
| `real-film-cuts` | 20, 47, 70, 87 | 20, 47, 70 (**87 suppressed**) | 1 | – |
| `real-game-cuts` | 32 | 32 | 0 | – |
| `real-game-motion`, `real-dissolve` | none / soft 15–37 | none | 0 | – |

**Re-measured at the 0.3 s window, which is what ships now.** `real-film-cuts` takes
all four: 20 residual 0.3363, 47 residual 0.3114, 70 histogram 0.2504, 87 histogram
0.2711 — accepted. Residual and overlap are bit-identical to the 0.6 s run and only
frame 87's verdict changed, so the attribution is exact. The real four become
**P 1.000 / R 1.000 / F1 1.000**, five of five with no false positive. The synthetic
nine are **unchanged at 0.571/0.571/0.571**, so the shorter window costs the synthetic
set nothing and buys the one real cut. `flash-exposure` keeps its frame-34 suppression
(4 frames after 30, inside 9 as it was inside 18) and its two false positives at 30
and 60; `cuts-motion` is unchanged because frame 91 fires the strong arm, which is
never debounced at any window length.

Two findings the synthetic set could not produce:

- **The debounce, not a threshold, lost that cut.** Local 87 fires the weak arm at
  residual 0.2711 with overlap 0.5294 — a clear detection — and was suppressed for
  being 17 frames after the accepted cut at 70, inside the old 0.6 s (18-frame)
  window. The shortest synthetic segment here is 1.0 s, so no synthetic clip can
  exercise a gap that short. Note what the 17 frames actually are: the span runs
  from the cut at 70 to the cut at 87, and 87 is where the capture's own scene
  changes into the paused frame - so it is bounded above by the demo composition,
  not by a trailer edit. It is therefore evidence that a reset must follow a
  discontinuity 17 frames after its predecessor, and not evidence about how fast
  film is cut. The labelled corpus brackets the replacement from both sides:
  `flash-exposure`'s transient returns 4 frames after the cut that opened it, so the
  window must exceed 4, and that labelled span is 17 frames, so the window must not
  exceed 17 — suppression is `since_cut < min_frames`, so a 17-frame window still
  accepts a cut 17 frames out. The usable range is 5–17 frames inclusive and the
  shipped 9 sits in it, asymmetrically: 5.7x of margin at the flash end against
  1.9x at the other, so the next clip with a shorter labelled span is what would
  squeeze it. A label audit found no two labelled cuts anywhere in
  the corpus closer than 9 frames (tightest gaps 17, then 23) and both soft spans 22
  frames wide, so the new window discards no labelled cut.
  **The lower bound is corpus-bound, not physical.** It rests on one synthetic
  4-frame transient; the corpus contains no 6–15-frame transient, and a lightning
  strike, a camera-flash bloom or a short exposure ramp at 30 fps is typically that
  long. The old 18-frame window protected those cases and the shipped 9 would
  double-reset on them. That is an accepted trade, not a measured safe margin, and a
  `flash-exposure` variant with an 8–12-frame transient is what would close it.
  Note also what raising the strong arm to the
  sweep's "best" 0.40 would do to this set: `real-game-cuts` fires at 0.2174 and every
  `real-film-cuts` cut at 0.2504–0.3363, so all five would move to the debounced weak
  arm and the miss would get worse, not better.
- **Precision is 1.000 on real material, dissolve included.** Grain, motion blur, a
  muzzle flash inside a shot, a HUD over fast motion and a 0.7 s cross-fade produced
  no false positive at all — including zero resets inside the fade, which is inside
  the "at most one" tolerance. The two false positives in the synthetic table are both
  `flash-exposure`, a deliberately adversarial clip.

What is still missing is a real dissolve — the demo capture contains none, every
transition in it is a single-frame jump — and a shot-boundary set large enough that a
three-parameter grid search is not fitting twelve positives. The gate A/B these clips
were built for is in
[`docs/measurements/gate-real-footage-20260914/REPORT.md`](measurements/gate-real-footage-20260914/REPORT.md).

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
invented threshold; `IsHDR` on the linear FP16 input is an A/B nobody has run on the
neural pass (Super Resolution's is in `docs/measurements/sr-quality-20260924/`, the
supplied exposure's in `docs/measurements/exposure-ab-20260923/`); and depth is now answerable as
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

## Which neural settings change the image (2026-09-24, RenoDX 6.5.3)

Re-measured on the pinned runtime (ReShade 6.8.0.2155, RenoDX 6.5.3, DLSS-NR 310.8.0,
RTX 4080 SUPER) with `tools/benchmark/knobs.py`, which replaced the 4.70 method of
pairing the player's single-frame previews: every row renders a whole clip through the
worker at the player's shipped state - the ten keys it writes - changed in exactly one
key or one guide, and is compared with the reference as decoded rgb24 bytes over every
frame. Two clips: `real-film-cuts` (102 frames, faces on 12 of 21 sampled frames) and
`real-game-cuts` (68 frames, a static game HUD). The reference also writes
`NRNormGovernor=0` and was rendered twice to 0 differing bytes; why is the next
subsection. Full table, face shares and method:
[knobs-653 report](measurements/knobs-653-20260924/REPORT.md).

| control | change | bytes differing (film / game) | mean abs delta (film / game) |
|---|---|---:|---:|
| (reference, rendered again) | none | 0 % / 0 % | 0 / 0 |
| Style | 0 → 1 (Natural) | 76.37 % / 93.64 % | 5.79 / 6.36 |
| Style | 0 → 2 (Cinematic) | 76.32 % / 92.20 % | 4.34 / 5.87 |
| Passes | 1 → 2 | 69.97 % / 89.29 % | 2.31 / 3.63 |
| **Color strength** | 1.00 → 0.20 | **68.28 % / 88.69 %** | **2.06 / 3.47** |
| **Color strength** | 1.00 → 0.60 | **59.38 % / 81.66 %** | **1.22 / 2.09** |
| Intensity | 1.00 → 0.40 | 64.74 % / 85.94 % | 1.63 / 2.76 |
| Local tone | 1.00 → 0.30 | 63.46 % / 85.29 % | 1.51 / 2.55 |
| Local structure | 1.00 → 0.30 | 53.69 % / 78.65 % | 1.12 / 2.04 |
| Chained history (2 passes, vs 2 passes chained) | on → off | 49.41 % / 77.41 % | 0.89 / 2.08 |
| Motion-vector guide | on → off | 46.89 % / 81.94 % | 1.02 / 2.72 |
| Automatic mask | on → off | 43.80 % / 67.14 % | 0.68 / 1.28 |
| Skin structure | −1.00 → 0.00 | 43.13 % / 66.34 % | 0.78 / 1.27 |
| Skin structure | −1.00 → +0.25 / +0.50 / +0.99 | 38.7-42.5 % / 65.2-66.0 % | 0.59-0.76 / 1.22-1.26 |
| Depth guide | on → off (motion on) | 35.91 % / 63.23 % | 0.54 / 1.18 |
| **Skin structure** | −1.00 → −0.50, −0.01, **+1.00** | **0 % / 0 %** | 0 / 0 |
| **Skin structure, mask off** | any value | **0 % against mask off** | 0 |
| **Render preset** | 0 → 1, 0 → 3 | **0 % / 0 %** | 0 / 0 |
| **Global tone** (`NRGlobalTone`, not written by the player) | 1.00 → 0.00, 0.30, 1.05, 2.00 | **0 % / 0 %** | 0 / 0 |
| **UI correction** (`NRUICorrection`, not written by the player) | 0 → 1 | **0 % / 0 %** | 0 / 0 |

What moved since 4.70, and what the UI does about it:

- **Color strength now changes the image**: 0.20 moves more of the picture than
  Intensity at 0.40. On 4.70 it was 0 bytes on every pair, which is why the dialog hid
  it. It is back in the dialog as a 0.00-1.00 slider (default 1.00, unchanged); it
  was never out of `NeuralSettings`, the INI or the render identity, so no cache
  entry changes meaning.
- **The render preset is still inert**, now on a third runtime, and stays hidden.
- **Skin structure only has a range of 0.00 to 0.99.** Every negative value and
  exactly +1.00 render the shipped picture, byte for byte; 0.00, 0.25, 0.50 and 0.99
  each change it, and by less as they rise. So the shipped −1.00 means *off*, half of
  the dialog's −1..+1 slider is inert, and the add-on's own overlay text ("negative
  smooths, positive enhances, 0 = neutral") does not describe what the runtime does.
  The dialog's slider now offers exactly that: **Off** at its left end, then 0.00 to
  0.99, greyed out with "Mask off" while Automatic mask is off. Off is still
  written as −1.00, so the default render and its cache key are unchanged, and a
  value an older build saved loads as what it rendered as (a negative value or +1.00
  as Off).
- **Skin structure needs the automatic mask**: with `NRAutoMask=0` its value changes
  nothing, which matches the add-on's description of the mask ("so the
  Character/Skin Structure response applies to them"). The mask's own tooltip said it
  "chooses regions to leave untouched"; it now says what the add-on and this table say.
- **Neither gives face protection**. The change a skin value makes lands on
  Haar-detected face boxes 2.1× as densely as their area, against 1.8× for Intensity
  and 1.9× for Color strength on the same frames: skin is barely more face-local than
  a global control, and the mask toggle less (1.35×). There is nothing here worth
  wiring into the compare mask as a "Protect skin" option, so nothing was.
- **Global tone and UI correction are inert** on this path at every value tried, so
  the player does not write them and the dialog does not show them.
  `NRUICorrection` is read as an integer (0.5 is stored back as 0).
- **Chained history works.** A first pass of this measurement said it did nothing;
  that was a harness bug, below.

### Two harness corrections this table depends on

**The normalization governor.** The add-on's `NRNormGovernor` (0 off, 1 slew, 2
stable; the player did not write it then, so 2 shipped) settles its brightness divisor at
rates per second. On the two synthetic near/far clips a repeat of one configuration
at the default is not byte-identical: 12.5 % of bytes from frame 69 of `depth-pan`
and 26 % from frame 37 of `depth-subject`, mean |Δ| 0.41-0.46, worst frame 0.9-2.2
levels. At `NRNormGovernor=0` every repeat of every clip tried was identical, and on
the three NR-processed captures the governor changes nothing at all (0 bytes between
0 and 2, repeats identical). So the shipped render is reproducible on the captures
and not on saturated synthetic material, and every byte-for-byte comparison in the
harness now writes `NRNormGovernor=0`. Whether the player should write 0 too is a
flicker trade this table could not decide. `docs/measurements/governor-20260924`
did: off brings frame-to-frame pumping back (2.7-6× the governed mean-luma jitter on
three clips), stable's repeats differ on five of nine clips including real footage
with lighting changes (50.7 % of bytes), and slew (1) is reproducible in every render
and damps as stable does - so the player now writes `NRNormGovernor=1`.

**Schema migration.** `run.py` wrote 4.70's `NREnableUpscaling=0` and no
`ConfigVersion`, so 6.5.3 read every fresh profile as config schema v0 and migrated
it on load ("inherited NRCodecMode/NRChainedHistory defaults adopted", a `.bak`
beside the add-on) - which set `NRChainedHistory` back to 1 and made a chained-off
profile render the chained picture. `run.py` now writes the player's own managed keys
plus `ConfigVersion=6`, the stamp the add-on writes on first load and so the state of
every player runtime after its first launch. At the defaults this changes no pixel
(identical digests with and without the migration on three clips).

The same migration could reach the player once: `packaging/ReShade.ini` carries no
`[RenoDX.DLSS5]` section, so the first launch's section had no `ConfigVersion` and a
chained-off setting in `DLSSVideoPlayer.ini` was rendered chained on that first
launch only. The player now writes `ConfigVersion=6` into a section it creates
(`src/ReShadeConfig.cpp`); a preflight on a fresh runtime logs no migration and leaves
no `.bak`.

Cost of a change is unchanged in kind: settings and guides are part of the render
identity, so every distinct combination renders from scratch (16 cold 1080p
single-frame previews took a median of **10.6 s** each on 4.70, and repeats came back
from cache in **1.03 s**). Changing the render preset therefore still costs a full
re-render and produces byte-identical output.

### The 4.70 table it replaces (2026-09-09)

Measured through the player's paused previews, one frame per pair: Intensity 84.75 %,
Local tone 66.45 %, Local structure 53.41 %, Style 2 → 0 49.31 %, motion guide
39.79 %, depth guide 37.92 %, Skin structure +1.00 → −0.40 36.50 %, Automatic mask
36.08 %, and **Color strength 0 %** and **render preset 0 %** on every pair. The skin
pair would measure 0 % on 6.5.3, where both of its values are off: the runtime
changed, not the method.
