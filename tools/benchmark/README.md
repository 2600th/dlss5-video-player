# Neural quality benchmark

Repeatable corpus, worker driver, metric analysis and blind A/B tooling for the
isolated `NeuralWorker.exe` (roadmap P0 items 2 and 3). Everything is written
under `build-upscaling/` (already gitignored); nothing here touches the player,
its cache or the committed runtime.

```
python tools/benchmark/corpus.py                       # build-upscaling/benchmark-corpus/*.mkv + manifest.json
python tools/benchmark/run.py --clips text-subtitles cuts-motion --profiles baseline mv-off --repeats 3
python tools/benchmark/run.py --ablation --repeats 2   # every profile in run.ABLATION
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
| `corpus.py` | Deterministic FFV1 corpus generator; `--check` re-hashes an existing corpus |
| `run.py` | Profiles, ablation matrix, two-pass, preflight receipts, worker launches, NVML sampling |
| `analyze.py` | Per-run metrics, medians per clip/profile, two-pass deltas, `report.md` |
| `blind.py` | Randomized A/B stills + 3 s excerpts with a sealed `key.json`; `--score` tallies a ballot |
| `common.py` | Paths, `ffprobe`/`framemd5` helpers, NWR1 protocol v2 decoder |
| `build-upscaling/benchmark-corpus/` | Generated clips and `manifest.json` |
| `build-upscaling/benchmark-work/runtime-snapshot/` | Optional frozen copy of `Release/neural-runtime`; used in preference to `Release/` so a concurrent rebuild cannot change the worker mid-benchmark |
| `build-upscaling/benchmark-work/profiles/<profile>/` | Isolated runtime clone, `ffmpeg.exe`/`ffprobe.exe` hard links, `profile.json`, `preflight.json` |
| `build-upscaling/benchmark-work/runs/<clip>__<profile>__<rep>/` | `output.mkv`, `result.json`, `pass1.bin` (raw metadata pipe), `gpu.csv`, copied `ReShade.log`/`.ini`, `frames.md5`, `metrics.json` |
| `build-upscaling/benchmark-work/analysis/` | `analysis.json`, `report.md` |
| `build-upscaling/benchmark-work/blind/` | `pairs/`, `key.json`, `ballot.csv` |

## Corpus

1920x1080, 30 fps, FFV1 level 3 (lossless, `yuv420p`), 7-8 s each. Every
synthetic clip comes from seeded FFmpeg sources with pinned colours; the
manifest records an rgb24 `framemd5` sequence digest per clip, and
`corpus.py --check` proves a regenerated corpus is bit-identical.

| Clip | Category | Content |
|---|---|---|
| `text-subtitles` | text | 14-28 px drifting mono/UI text plus three burned SRT cues; ground-truth strings and cue times in the manifest |
| `fine-detail` | detail | Mandelbrot zoom, sub-pixel-drifting 16 px grid, `zoompan` test-pattern inset |
| `highlights-gradients` | tone | Animated linear gradients with two radial highlight sweeps that clip to white |
| `cuts-motion` | cuts | Five dissimilar 1.5 s segments hard-cut at frames 45/90/135/180 (recorded in `cuts`) |
| `faces` | faces | **Not synthetic**: seconds 12-20 of `build-upscaling/runtime-comparison-20260907/fixtures/mafia-60s.mkv` (frontal/three-quarter faces, skin, hair). Skipped when the fixture is absent |

## Profiles and ablation

A profile is `{guides, overrides, passes}`:

- `guides` is the canonical `--guides` string (`mv=1,depth=1,mask=1`). A disabled guide is still uploaded with neutral values (motion 0, depth 0.75, mask 0) so the DLSS input contract is unchanged.
- `overrides` are exact-case `[RenoDX.DLSS5]` keys written into the profile's `ReShade.ini` after the three managed keys (`EnableHooks=2`, `NeuralUplift=1`, `NREnableUpscaling=0`). Known RenoDX 4.70 keys: `NRIntensity`, `NRLocalTone`, `NRLocalStructure`, `NRSkinStructure`, `NRColorStrength`, `NRPreset` (0-3), `NRStyle` (0 default, 1 natural, 2 cinematic), `NRAutoMask`.
- `passes` 1 or 2.

`run.ABLATION` changes one factor per profile: `baseline`, `mv-off`, `depth-off`,
`mask-off`, `automask-off`, `structure-0`, `tone-0`, `intensity-0` (control),
`preset-1..3`, `style-natural`, `style-cinematic`, `two-pass`. Custom sets:
`--profile-file profiles.json` with `{name: {guides, overrides, passes, description}}`.

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
"sampled" metrics use every `--sample-every` (default 15) frames.

| Metric | Definition |
|---|---|
| determinism | sha256 of the rgb24 `framemd5` sequence; `det` is true when every repeat of a clip/profile produced the same digest |
| e2e fps | `frame_count / wall seconds` of the final pass (includes process start, decode, priming, encode finish) |
| proc fps | frames per second between the first and last `Rendering` progress records |
| gpu ms p50/p95/max, guide ms, capture ms, VRAM local | copied from the worker result (GPU timestamp queries around DLSS evaluate, CPU guide generation, readback/capture, `IDXGIAdapter3` local-budget peak). `0` means the worker build does not report it |
| VRAM total | NVML `nvmlDeviceGetMemoryInfo.used` peak across the whole GPU during the run (every process, including the desktop) |
| flicker+ | mean over frames of mean `|Y_t - Y_{t-1}|` for the output minus the same for the source, skipping manifest cut frames; positive = the neural pass added temporal change, negative = it smoothed |
| dE | mean CIE76 ΔE*ab between output and source (OpenCV Lab, L rescaled to 0-100) |
| RGB shift | mean per-channel (output − source) in `metrics.json` |
| PSNR / SSIM | RGB PSNR and grayscale Gaussian SSIM against the lossless source. A relighting model is expected to move these; use them as a change magnitude, not a pass/fail |
| OCR out/src | rapidocr on sampled frames; character-level `SequenceMatcher` ratio of the manifest strings active at that timestamp against the recognized text, for output and for the source (the source figure is the OCR engine's own ceiling) |
| face cos | resnet18 (ImageNet, penultimate layer) cosine between the source face crop and the same crop of the output; boxes from OpenCV Haar frontal+profile cascades on the source. `faces.output_drift_mean` in `metrics.json` is the frame-to-frame embedding drift of the output versus the source's own drift |
| resets | `history_resets` from the worker result |

Two-pass rows in `report.md` list metric deltas against `baseline` on the same clip.

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
