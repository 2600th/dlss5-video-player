# Neural quality benchmark

Repeatable corpus, worker driver, metric analysis and blind A/B tooling for the
isolated `NeuralWorker.exe` (roadmap P0 items 2 and 3). Everything is written
under `build-upscaling/` (already gitignored); nothing here touches the player,
its cache or the committed runtime.

```
python tools/benchmark/corpus.py                       # build-upscaling/benchmark-corpus/*.mkv + manifest.json
python tools/benchmark/corpus.py --clips pan-fast zoom-fast           # a subset, into its own --corpus dir
python tools/benchmark/cutlab.py --sweep               # scores the cut criterion against the labelled cuts
python tools/benchmark/run.py --clips text-subtitles cuts-motion --profiles baseline mv-off --repeats 3
python tools/benchmark/run.py --ablation --repeats 2   # every profile in run.ABLATION
python tools/benchmark/run.py --profiles depth-constant depth-proxy  # the depth A/B
python tools/benchmark/analyze.py                      # build-upscaling/benchmark-work/analysis/report.md
python tools/benchmark/blind.py --pairs-per-clip 3     # sealed A/B pairs for one-pass vs two-pass
```

Requirements: Python 3.12 with `requirements.txt`, `external/ffmpeg/bin/ffmpeg.exe`
and `ffprobe.exe`, a built `build-upscaling/Release/neural-runtime/NeuralWorker.exe`
with the RenoDX/ReShade runtime beside it, and an NVIDIA GPU with `nvml.dll`
(NVML sampling degrades gracefully when it is missing).

## Layout

| Path | Contents |
|---|---|
| `corpus.py` | Deterministic FFV1 corpus generator; `--clips` builds a subset, `--check` re-hashes an existing corpus |
| `run.py` | Profiles, ablation matrix, two-pass, preflight receipts, worker launches, NVML sampling |
| `analyze.py` | Per-run metrics, medians per clip/profile, guide and two-pass deltas, cut scores, `report.md` |
| `cutmirror.py` | The mirror of `src/TemporalGuides.cpp`'s cut path (analysis grid, cell luma, global search, histogram overlap, per-cell match costs, both criteria, the debounce), shared by `analyze.py` and `cutlab.py` |
| `cutlab.py` | Scores the cut criterion itself against the manifest's labelled cuts and sweeps it; needs no GPU, no worker and no render |
| `blind.py` | Randomized A/B stills + 3 s excerpts with a sealed `key.json`; `--score` tallies a ballot |
| `common.py` | Paths, `ffprobe`/`framemd5` helpers, NWR1 protocol v4 decoder (progress, result, preflight, segment) |
| `build-upscaling/benchmark-corpus/` | Generated clips and `manifest.json` |
| `build-upscaling/benchmark-work/runtime-snapshot/` | Optional frozen copy of `Release/neural-runtime`; used in preference to `Release/` so a concurrent rebuild cannot change the worker mid-benchmark |
| `build-upscaling/benchmark-work/profiles/<profile>/` | Isolated runtime clone, `ffmpeg.exe`/`ffprobe.exe` hard links, `profile.json`, `preflight.json` |
| `build-upscaling/benchmark-work/runs/<clip>__<profile>__<rep>/` | `output.mkv`, `result.json`, `pass1.bin` (raw metadata pipe), `gpu.csv`, copied `ReShade.log`/`.ini`, `frames.md5`, `metrics.json` |
| `build-upscaling/benchmark-work/analysis/` | `analysis.json`, `report.md` |
| `build-upscaling/benchmark-work/blind/` | `pairs/`, `key.json`, `ballot.csv` |

## Corpus

1920x1080, 30 fps, FFV1 level 3 (lossless, `yuv420p`), 1-8 s each. Every
synthetic clip comes from seeded FFmpeg sources with pinned colours; the
manifest records an rgb24 `framemd5` sequence digest per clip, and
`corpus.py --check` proves a regenerated corpus is bit-identical.

Cut ground truth lives in two manifest keys. `cuts` is the list of frames at which a
history reset must happen; `soft_cuts` is a list of `[first, last]` spans, used by the
dissolve, inside which the first reset is tolerated and counted as neither a hit nor a
false positive while a second one is still a false positive. A clip with both empty
must never reset.

| Clip | Category | Content |
|---|---|---|
| `text-subtitles` | text | 14-28 px drifting mono/UI text plus three burned SRT cues; ground-truth strings and cue times in the manifest |
| `fine-detail` | detail | Mandelbrot zoom, sub-pixel-drifting 16 px grid, `zoompan` test-pattern inset |
| `highlights-gradients` | tone | Animated linear gradients with two radial highlight sweeps that clip to white |
| `cuts-motion` | cuts | Five dissimilar 1.5 s segments hard-cut at frames 45/90/135/180 (recorded in `cuts`) |
| `cuts-similar` | cuts | Four 1.0 s mirrorings of one deep fractal still, hard-cut at 30/60/90. The shots share a luma histogram by construction, so only a correspondence test can see the cut |
| `pan-fast` | cuts | 1.0 s diagonal pan across a 4K fractal still, 64×36 px per frame (6.1 analysis cells). No cut |
| `zoom-fast` | cuts | 1.5 s centre zoom 1.0× → 2.2× on a fractal still, which no global translation can model. No cut |
| `dissolve` | cuts | 0.7 s cross-fade between two shots. `cuts` is empty and `soft_cuts` marks the fade: one reset inside it is tolerated, a second is a false positive |
| `flash-exposure` | cuts | 3 s slow pan with a 4-frame flash and a sustained exposure step. Neither is a cut; both collapse the luma histogram |
| `faces` | faces | **Not synthetic**: seconds 12-20 of `build-upscaling/runtime-comparison-20260907/fixtures/mafia-60s.mkv` (frontal/three-quarter faces, skin, hair). Skipped when the fixture is absent |
| `real-film-cuts` | real | **NR-processed capture** (see below): 102 frames, four trailer shots cut at 20/47/70, grain and motion blur, a two-frame muzzle flash *inside* one shot that is deliberately unlabelled — then a static paused player frame from local 87, which is why the cut at 87 is the demo's scene boundary and not a film edit. 37 % of its consecutive pairs carry no motion (frozen tail plus 23.976→30 fps capture duplicates) |
| `real-game-cuts` | real | **NR-processed capture**: 68 frames, one hard cut at 32 from a race exterior to a store interior, with the game's own static HUD over fast camera motion |
| `real-game-motion` | real | **NR-processed capture**: 76 frames, one continuous shot, camera translating while the subject occludes and disoccludes the background. No cut |
| `real-dissolve` | real | NR-processed capture, **synthesised transition**: the source contains no dissolve, so two shots are cross-faded over 0.7 s. `cuts` is empty and `soft_cuts` marks the fade |

The four `real` clips are **not camera-original footage.** They are cut from
`docs/media/neural-comparison-demo.mp4`, a screen capture of this player recorded
with neural rendering *on*, so their pixels have been through: source → DLSS-NR →
the player's window → `gdigrab` → an h264 encode → `crop` → a lanczos upscale — and
then the neural pass again when the benchmark renders them. That is sound for an A/B
where both arms see byte-identical input, and it is not a claim about original
footage; `docs/BENCHMARK.md` carries the consequences, including what it does to the
`intensity-0` carrier control. No split-screen, divider or UI chrome is inside any
clip: the capture's magnify/wipe demonstration starts one frame after
`real-film-cuts` ends, checked frame by frame.

## Scene-cut lab (`cutlab.py`)

Scores the criterion rather than a render, so it needs nothing but the corpus and
FFmpeg. It replays `cutmirror` over every clip's cell grids, matches each accepted
history reset against `cuts` / `soft_cuts`, and prints where the labelled cuts sit in
each score's ordering, every firing with its residual/overlap/failed fraction, the
per-clip verdicts, and - with `--sweep` - both threshold families ranked. Cell
features are cached under `<corpus>/cutlab-cache` on the clip's own digest, so the
first run costs a decode pass and later sweeps cost seconds.

`--sweep` compares the shipped residual criterion against roadmap survey item 3, the
scale-free candidate in `cutmirror.FailedFractionCriterion`: a cell's match failed
when its winning displacement costs more than `ratio` x its standing-still cost, and
the decision is the fraction of failed cells rather than a mean residual. As of
2026-09-14 the two families reach the identical best operating point, so nothing in
`src/` uses the candidate; see `docs/BENCHMARK.md` for the measurement and why the
shipped thresholds were left alone.

## Profiles and ablation

A profile is `{guides, overrides, passes}`:

- `guides` is the canonical `--guides` string (`mv=1,depth=1`). A disabled guide is still uploaded with neutral values (motion 0, depth 0.75) so the DLSS input contract is unchanged.
- `overrides` are exact-case `[RenoDX.DLSS5]` keys written into the profile's `ReShade.ini` after the three managed keys (`EnableHooks=2`, `NeuralUplift=1`, `NREnableUpscaling=0`). Known RenoDX 4.70 keys: `NRIntensity`, `NRLocalTone`, `NRLocalStructure`, `NRSkinStructure`, `NRColorStrength`, `NRPreset` (0-3), `NRStyle` (0 default, 1 natural, 2 cinematic), `NRAutoMask`.
- `passes` 1 or 2.

`run.ABLATION` changes one factor per profile: `baseline`, `mv-off`, `depth-off`,
`automask-off`, `structure-0`, `tone-0`, `intensity-0` (control),
`preset-1..3`, `style-natural`, `style-cinematic`, `two-pass`. Custom sets:
`--profile-file profiles.json` with `{name: {guides, overrides, passes, description}}`.

The roadmap's depth A/B is `--profiles depth-constant depth-proxy`; both name guide
strings the matrix already carries (`depth=0` *is* the constant 0.75 field), so they
resolve to `depth-off` and `baseline` and reuse their run directories rather than
rendering the same configuration twice. `depth-of` is refused by name: `--guides`
expresses only `mv=0|1,depth=0|1`, so depth from the NVOFA structure cannot be
requested. `--list-profiles` prints the matrix, the two aliases and that refusal.

Every profile runs `--neural-preflight` once; the Feature 18 probe receipt (GPU,
driver, ReShade/RenoDX/DLSS-NR versions, locked module hashes, feature18
creation/evaluation evidence, RenoDX "active settings" line) is stored in
`profiles/<profile>/preflight.json` and copied into each `result.json`.

Worker exit code 75 means the helper repaired its `ReShade.ini`; the runner
relaunches once with `--configuration-restarted`, exactly like the player.

### Two-pass

Pass 1 renders the corpus clip with the profile's settings. Pass 2 renders the
**pass-1 output MKV** (NVENC HEVC, or software H.264 when NVENC is unavailable)
in a second runtime clone with `NRIntensity=0.750000` added. `result.json`
carries `pass2_reencoded_input: true` and both pass results; the second pass
never sees lossless frames, so every two-pass metric includes one lossy
generation on top of the model's effect.

## Metrics (`analyze.py`)

All comparisons decode both files to rgb24 with FFmpeg and stream frame pairs;
"sampled" metrics use every `--sample-every` (default 15) frames, while `flicker+`,
`sigma+`, `false mv`, `flips+` and the cut test always see every consecutive pair.
`sigma+`, `false mv`, `flips+` and the cut test use the guide generator's own analysis
grid and thresholds (`src/TemporalGuides.cpp`: at 30 fps 160×90 cells of 12×12 source
pixels at 1080p, normalized Rec.709 cell luma), and are withheld whole - `null` plus a
`temporal_withheld` reason - when the output frame count does not match the source.

| Metric | Definition |
|---|---|
| determinism | sha256 of the rgb24 `framemd5` sequence; `det` is true when every repeat of a clip/profile produced the same digest |
| e2e fps | `frame_count / wall seconds` of the final pass (includes process start, decode, priming, encode finish) |
| proc fps | frames per second between the first and last `Rendering` progress records |
| gpu ms p50/p95/max, guide ms, capture ms, VRAM local | copied from the worker result (GPU timestamp queries around DLSS evaluate, CPU guide generation, readback/capture, `IDXGIAdapter3` local-budget peak). `0` means the worker build does not report it |
| VRAM total | NVML `nvmlDeviceGetMemoryInfo.used` peak across the whole GPU during the run (every process, including the desktop) |
| flicker+ | mean over frames of mean `|Y_t - Y_{t-1}|` for the output minus the same for the source, skipping manifest cut frames; positive = the neural pass added temporal change, negative = it smoothed |
| sigma+ | output minus source per-pixel temporal standard deviation of luma inside a shot (frames between manifest cuts), meaned over pixels and frame-weighted over shots, in 8-bit luma levels; `temporal_sigma_p99_*` in `metrics.json` is the p99 of the same map. Localized shimmer a frame-global mean averages away moves this |
| false mv | fraction of the cells the source held static across a consecutive pair (cell luma change ≤ 2/255) whose output changed by more than the same tolerance: motion the pass invented. The NVENC carrier sets a floor; `intensity-0` measures it |
| flips+ | output minus source fraction of cells whose moving/static verdict changes from one consecutive pair to the next - instability of the motion field rather than of the pixels |
| cut P/R/F1 | the generator's own cut test (residual > 0.30, or residual > 0.10 with histogram overlap < 0.85, debounced over 0.6 s) run over each file's cell grids and matched to the manifest's hard cuts at ±1 frame, source and output. `cuts.*_evidence` in `metrics.json` records every firing frame with residual, overlap, arm and suppression; `cutlab.py` scores the same test against the labelled corpus without needing a render |
| dE | mean CIE76 ΔE*ab between output and source (OpenCV Lab, L rescaled to 0-100) |
| RGB shift | mean per-channel (output − source) in `metrics.json` |
| PSNR / SSIM | RGB PSNR and grayscale Gaussian SSIM against the lossless source. A relighting model is expected to move these; use them as a change magnitude, not a pass/fail |
| OCR out/src | rapidocr on sampled frames; character-level `SequenceMatcher` ratio of the manifest strings active at that timestamp against the recognized text, for output and for the source (the source figure is the OCR engine's own ceiling) |
| face cos | resnet18 (ImageNet, penultimate layer) cosine between the source face crop and the same crop of the output; boxes from OpenCV Haar frontal+profile cascades on the source. `faces.output_drift_mean` in `metrics.json` is the frame-to-frame embedding drift of the output versus the source's own drift |
| resets | `history_resets` from the worker result |

Two-pass rows in `report.md` list metric deltas against `baseline` on the same clip,
and any profile whose guide string differs from `baseline`'s gets the same deltas in a
guide A/B table. `report.md` also carries a per-run cut table and lists any run whose
temporal metrics were withheld.

## Blind A/B (`blind.py`)

For each clip with both a `baseline` and a `two-pass` run, `blind.py` emits
`pairs/<id>-A.png/.mp4` and `-B`, with A/B shuffled per pair from `--seed`,
excerpt start frames chosen away from recorded cuts. The mapping is sealed in
`key.json`; fill `ballot.csv` (`preferred` = A/B/tie, `confidence` 1-5) without
opening it, then `blind.py --score ballot.csv`.

## Caveats

- **Worker output is lossy.** The cache carrier is NVENC HEVC (software H.264 fallback), so PSNR/SSIM/ΔE against the lossless source include encoder loss. Compare profiles against each other and against `intensity-0`, which isolates the carrier's own loss.
- **VRAM numbers are different things.** `peak_local_vram_mib` in the receipt is the worker's own local-budget peak from `IDXGIAdapter3::QueryVideoMemoryInfo`; NVML `total_used_mib` is device-wide and includes the desktop, the benchmark's own decoders and anything else running.
- **Guides only affect output once the worker applies them.** Bit-identical `baseline` and `mv-off` digests mean the worker build ignored `--guides`; check `deterministic` and the digests before attributing an ablation result.
- **OCR is engine-limited.** The source ratio is the ceiling; judge the output by its distance from the source figure.
- **Face embeddings are generic.** resnet18 ImageNet features measure crop similarity, not identity; low cosine indicates a large visual change of the face region, not necessarily a worse face.
- Timing fields (`gpu ms`, `guide ms`, `capture ms`, local VRAM) are zero for worker builds that predate the timing instrumentation.
- Runs are skipped when `runs/<clip>__<profile>__<rep>/result.json` exists; delete the directory to rerun. `--fresh-profiles` rebuilds the runtime clones (needed after a worker rebuild when no snapshot is used).
