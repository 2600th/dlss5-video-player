# Super Resolution history: Temporal against Per-frame — 24 September 2026, RTX 4080 SUPER

**What shipped.** DLSS Upscaling stays off by default, as before. It now has a
**History** choice: **Temporal (steadier)**, the default and what it always did, or
**Per-frame (sharper on some clips)**, which resets DLSS's history on every frame so
that each output is DLSS's single-frame upscale of that frame. The choice is
**DLSS > Upscaling history** for playback and a **History** row under Super Resolution
in **Export with DLSS stages**. The two are one setting (`[Playback] UpscalingHistory`),
and `--render` reads it too. A pass that also runs the neural model keeps Temporal,
because the model runs on the same carrier. That includes every cached render, so no
cache key moves.

**What was measured, plainly.** Neither history beats a plain bicubic upscale on
video, on any clip. Per-frame scored higher than Temporal on 5 of 6 clips, by up to
19 VMAF, and 1.3 lower on a held frame. Temporal's output changes less from frame
to frame than the source does, which is the sense in which it is "steadier": it
calms grain and trails motion. By the error-flicker measure below, though,
Per-frame was no less stable on any clip. The lead ruled that the default stays
Temporal; this report gives the numbers to revisit that ruling.

## Method

- **Clips**: the six of [sr-quality-20260924](../sr-quality-20260924/REPORT.md),
  rebuilt the same way. Each is a 1080p30 BT.709 original stored as FFV1, area-reduced
  to 960x540 for the upscale. `still` is `demo` frame 0 held for 60 frames. `demo` and
  `ncd` are the first 180 frames of `tools/benchmark/fixtures/demo-capture-20260912.mp4`
  and `docs/media/neural-comparison-demo.mp4`. `pan` is a 1920x1080 window sliding
  1 px/frame across a 2400x1350 lanczos enlargement of `ncd` frame 0, which is exactly
  0.5 input px per frame. `mpan` is the same pan over a Mandelbrot still. `ts` is
  `testsrc2`.
- **Upscale**: `UpscalingGpuSmoke <clip_540> 1080 <raw> mv=1,depth=1 <frames> [per-frame]`.
  That is the playback renderer in preserve-source SR, with the capture ring drained
  and BGRA written raw. DLSS 310.7.0 beside the test, preset K, MaxQuality, NVOFA flow
  with the zero-motion test.
- **Scoring** (`vmaf.sh` here): BGRA → BT.709 limited 4:2:0 → `libvmaf` (default model)
  against the 1080p original, the fair-bicubic baseline of the earlier report
  (`fairbic.sh`), and two temporal measures on luma:
  - *flicker added*: mean |Y_t − Y_(t−1)| of the upscale minus the original's.
    Negative means the output changes less from frame to frame than the source.
  - *error flicker*: mean |e_t − e_(t−1)|, where e is upscale minus original. This is
    the part of the frame-to-frame change that is error rather than content: shimmer,
    crawl, and history re-sampled out of place. It is 0 for a steady error.

## Results

| clip | bicubic | **Temporal** | **Per-frame** | frame 0 | flicker added T / P | error flicker bicubic / T / P |
|---|---|---|---|---|---|---|
| still | 80.99 | **80.36** | 79.10 | 79.10 | +0.006 / 0.000 | 0.000 / 0.006 / 0.000 |
| demo | 81.03 | 75.25 | **76.04** | 79.10 | −0.045 / −0.004 | 0.117 / 0.215 / 0.143 |
| ncd | 84.92 | 61.61 | **79.52** | 90.80 | −0.156 / −0.005 | 0.454 / 0.619 / 0.472 |
| pan | 96.16 | 74.96 | **93.54** | 91.89 | −0.095 / −0.015 | 0.506 / 0.711 / 0.528 |
| mpan | 69.66 | 48.60 | **62.55** | 61.83 | −1.227 / −0.882 | 1.820 / 1.866 / 1.860 |
| ts | 84.02 | 66.61 | **72.92** | 69.02 | −0.644 / −0.281 | 0.735 / 0.974 / 0.850 |

(VMAF means; bicubic is scored through the same BGRA path. `frame 0` is identical in
both arms, because the first frame has no history.)

Through the **export path** (`NeuralWorker --require-neural 0 --output-width 1920
--output-height 1080 [--sr-history per-frame]`, the export's SR stage on its own,
NVENC HEVC carrier):

| clip | Temporal | Per-frame |
|---|---|---|
| still | 80.02 (err. flicker 0.012) | 78.80 (0.007) |
| pan | 74.89 (0.754) | 92.74 (0.580) |

The choice reaches the export: the pan moves by 18 VMAF, as it does on the playback
path. Both files keep the NVENC carrier's own loss.

What it says:

1. **Bicubic wins on video, whichever history is chosen.** This holds on every clip,
   by 0.6 (Temporal, `still`) to 23 VMAF (Temporal, `ncd`). The reasons are in the
   earlier report: DLSS SR is built for jittered, aliased, noise-free rendered samples.
   The feature is off by default and the UI now says what it measured.
2. **Per-frame scores higher on 5 of 6 clips.** The margin is +0.8 on `demo`, +6.3 on
   `ts`, +14 on `mpan`, +18 on `ncd` and +19 on `pan`. It scores 1.3 lower on the held
   frame, where Temporal's history has real samples to add. On moving video,
   accumulation costs more than it adds.
3. **"Steadier" is true only in the narrow sense.** Temporal's output changes less
   from frame to frame than the source, by up to 1.2 luma levels on `mpan`. That
   calms grain and noise, and it also trails real motion. Per-frame changes as much
   as the source (within 0.02 on the real clips). By error flicker, Per-frame is
   lower than Temporal on every clip, and equal on `mpan`. The labels the lead
   specified are kept, and the tooltip gives both numbers.

## Ruling and follow-up

- Default Temporal and labels "Temporal (steadier)" / "Per-frame (sharper on some
  clips)": the lead's ruling, kept. The measurement above argues for revisiting the
  default. It is a one-line change (`kDefaultUpscalingHistory`) and moves no cache key.
- With Neural rendering on the same pass, History is greyed in the export dialog and
  the job keeps Temporal (`CarrierUpscalingHistory`). The helper refuses a per-frame
  request that carries the model. Whether a per-frame carrier would change the model's
  output was not measured. Keeping Temporal there keeps every cached render and its
  key exactly as they were.

## Reproduce

```
bash fairbic.sh <clip>                                  # bicubic baseline
UpscalingGpuSmoke <clip>_540.mkv 1080 sr_t.raw mv=1,depth=1 <frames>
UpscalingGpuSmoke <clip>_540.mkv 1080 sr_p.raw mv=1,depth=1 <frames> per-frame
bash vmaf.sh sr_t.raw <clip>_ref.mkv sr_t.json          # likewise sr_p
```

`UpscalingSrQualitySmoke` (label `gpu`) now also renders its held frame with
Per-frame history and fails (exit 10) unless all 60 frames are byte-identical. That
is what a reset on every frame of identical input produces, so it proves the choice
reaches the evaluate.
