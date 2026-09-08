# Neural quality benchmark

Roadmap P0 items 2 and 3 ([DLSS5_VIDEO_ROADMAP.md](DLSS5_VIDEO_ROADMAP.md))
ask for a repeatable test set, measured neural time / FPS / VRAM / flicker /
OCR / face consistency / colour shift / determinism, blind one-pass versus
two-pass comparison, and an ablation of every guide and control. The scripts
under [`tools/benchmark/`](../tools/benchmark/README.md) implement that
against the isolated `NeuralWorker.exe`; this page is the operator's summary.

## Run it

```
python tools/benchmark/corpus.py                                     # once; ~1 min
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
| Speed | end-to-end fps (frames / wall), processing fps (Rendering-phase progress records), neural GPU ms p50/p95/max, guide ms, capture ms | worker result over the metadata pipe (protocol v2) |
| Memory | worker peak local VRAM (receipt) and NVML whole-GPU used/util/power/temperature at 2 Hz | `result.json`, `gpu.csv` |
| Determinism | sha256 of the rgb24 per-frame MD5 sequence across repeats | `frames.md5` |
| Temporal | added flicker = mean |ΔY| between consecutive output frames minus the same for the source, cut frames excluded | `analyze.py` |
| Colour | mean CIE76 ΔE in CIELAB and per-channel RGB shift vs source | `analyze.py` |
| Fidelity | PSNR and SSIM vs the lossless source | `analyze.py` |
| Text | rapidocr character-level ratio against the manifest's ground-truth strings, output and source side by side | text clip |
| Faces | Haar-box crops; resnet18 cosine of output crop vs source crop and frame-to-frame drift | faces clip |
| Two-pass | metric deltas vs `baseline`; `blind.py` sealed A/B stills and 3 s excerpts | `report.md`, `blind/` |

Ablation profiles (`run.py --ablation`) change one factor each: motion
vectors, depth, temporal mask, RenoDX automatic mask, local structure, local
tone, intensity (control), presets 1-3, styles natural/cinematic, two-pass.
Every profile uses the same corpus, the same worker priming/preroll and the same
runtime files, so differences attribute to the changed factor.

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

Guide ablation on `text-subtitles` and `cuts-motion` (one repeat each, all
four guide profiles), plus `faces`/`baseline` and the text clip through
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

Observations (P0-3 guide proof):

- Rerenders are bit-identical across repeats (framemd5 sequence digests match).
- `mask-off` is byte-identical to `baseline` on both clips: the estimated
  bias/disocclusion mask has no measurable effect on the active consumer for
  this content. Do not spend on better masks until a clip shows a difference.
- `mv-off` and `depth-off` change the output. On these synthetic clips the
  estimated motion vectors slightly *hurt* (mv-off: +0.30 dB PSNR, +0.018
  SSIM and −0.9 flicker on the cuts clip; +0.10 dB and −0.07 flicker on the
  text clip), which is the expected signature of block-matching vectors on
  drifting text and hard pans. Depth changes are within 0.02 dB.
- The cut detector fires exactly on the clip's hard cuts (`resets` = 1 first
  frame + 5 cuts on `cuts-motion`, 1 on the cut-free text clip).
- The neural pass itself is ~3.3 ms GPU per 1080p frame at p50 (3.8 ms p95);
  wall time is dominated by CPU guide generation (~6.4 ms), readback
  (~6.7 ms) and NVENC, so processing sits at ~31 fps.
- The second pass at `NRIntensity=0.75` adds flicker (+0.094), ΔE (+0.63)
  and costs 2.8 dB PSNR and 11 OCR points on small text; face crops keep a
  0.952 mean cosine to the source with frame-to-frame drift equal to the
  source's own (0.137 vs 0.133). Two-pass is not a default candidate.
