# RTX VSR against DLSS Super Resolution and plain scalers on video — 2 October 2026, RTX 5090

**Verdict: RTX VSR is the better video upscaler by every quality measure taken here,
and DLSS Super Resolution is the worst.** At the player's default quality (High, q3),
over six clips upscaled from 960x540 to 1920x1080, RTX VSR scores 87.5 VMAF against
bicubic's 80.5 and DLSS SR's 74.9 on clean input, and 78.5 against 69.5 and 66.1 on
H.264-compressed input. It wins on VMAF NEG too, the model that does not reward
sharpening, and on luma PSNR, so the gain is not sharpening alone. It costs 0.45 ms a
frame. The one thing it does worse is temporal: its error changes more from frame to
frame than a plain scaler's (error flicker 0.83 against 0.46), which is the shimmer a
per-frame network can add to moving detail. On a held frame it adds none.

This closes the question `sr-quality-20260924` left open ("the brief's target, clearly
above bicubic, is not reachable in this pipeline"): it is not reachable with DLSS SR,
and RTX VSR reaches it.

## What was already known

Nobody has published an objective comparison of RTX VSR against bicubic or Lanczos:
not NVIDIA, not the players that expose it, not the reviews, not the projects that
ship it. What exists, so that none of it was measured again:

- [Merserk/dlss5-visual-enhancer#25](https://github.com/Merserk/dlss5-visual-enhancer/issues/25)
  (the request behind this report, from issue #14's reporter): screenshots only.
  DLSS output "a bit blurry", RTX VSR Ultra "restores details better" but "slightly
  increases the color saturation". v7.0 then built its Upscale mode on RTX VSR. In
  v14.0 every pipeline stage is off by default and RTX VSR is one of two upscaling
  stages, at Ultra when on; only its CLI `upscale` command defaults to it
  ([README](https://github.com/Merserk/dlss5-visual-enhancer)).
- NVIDIA describes VSR as upscaling and compression-artifact reduction in one pass,
  trained on content at various compression levels
  ([blog](https://blogs.nvidia.com/blog/rtx-video-super-resolution)). Its SDK guide
  tunes quality levels 1-4 for compressed video, recommends High (3) when unsure, and
  has separate High-Bitrate modes (16-19) for clean downscaled input
  ([VFX SDK 1.3 guide](https://docs.nvidia.com/maxine/vfx/1.3.0/nvidia-vfx-sdk-user-guide.pdf), §8.2).
  The pinned `nvngx_vsr.dll` (RTX Video SDK) exposes 1-4 only, so 16-19 are not here.
- Subjective reviews: transformative from good 1080p to 4K, blurrier than native from
  480p, "overly crisp and smoothed out" on stills
  ([PCWorld](https://www.pcworld.com/article/1525299/nvidia-rtx-video-super-resolution-tested.html)).
  Forum reports split between a "waxy" look on clean sources and praise on Blu-ray remuxes
  ([NVIDIA forum](https://forums.developer.nvidia.com/t/feedback-and-questions-regarding-rtx-vsr-improvements-and-future-rtx-video-features/383633)).
- Players: VLC, mpv and MPC Video Renderer ship it off by default; Chrome asks for it
  by default and lets the driver decide. None published a quality evaluation.

So the numbers below are new: VMAF, VMAF NEG, PSNR and a temporal measure for RTX VSR
against the plain scalers and DLSS SR, on clean and on compressed input.

## Setup

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 5090, driver 617.14 |
| RTX VSR | `nvngx_vsr.dll` from the pinned RTX Video SDK, through the player's renderer (`UpscalingGpuSmoke vsr-capture`): the clip is the reference the view reads, the window is the output's size so VSR maps texel for pixel, tags off |
| DLSS SR | the player's playback path: `UpscalingGpuSmoke <clip> 1080 <raw> mv=1,depth=1 <frames> per-frame` (Per-frame is the viewer's default history) |
| Scalers | FFmpeg `bicubic` and `lanczos` of the decoder's BGRA frames |
| Scoring | every arm through 8-bit RGB → BT.709 limited YUV 4:2:0, against the 1080p original; `libvmaf` with `vmaf_v0.6.1` and `vmaf_v0.6.1neg`, luma PSNR; error flicker = mean \|e_t − e_(t−1)\| with e the luma error (as in `sr-history-20260924`) |
| Clips | 60 frames each: `demo`, `ncd`, `still`, `pan`, `mpan`, `ts`, as in `sr-quality-20260924`; recipes in `run.py` |
| Inputs | *clean*: area-reduced to 960x540, lossless. *coded*: the same, then H.264 CRF 30 |

## Results

Averages over the six clips (`results.csv` has every row):

| input | arm | VMAF | VMAF NEG | PSNR-Y | error flicker | GPU ms |
|---|---|---|---|---|---|---|
| clean | bicubic | 80.46 | 77.43 | 35.16 | 0.460 | |
| clean | lanczos | 81.50 | 78.36 | 35.29 | 0.469 | |
| clean | DLSS SR | 74.89 | 70.87 | 34.08 | 0.494 | |
| clean | VSR Low (q1) | 84.24 | 80.30 | 35.77 | 0.659 | 0.22 |
| clean | VSR Medium (q2) | 85.79 | 82.23 | 35.73 | 0.794 | 0.26 |
| clean | **VSR High (q3)** | **87.47** | **83.43** | 35.92 | 0.826 | 0.45 |
| clean | VSR Ultra (q4) | 87.16 | 83.05 | **36.09** | 0.860 | 0.60 |
| coded | bicubic | 69.51 | 66.64 | 34.41 | 0.634 | |
| coded | lanczos | 70.50 | 67.58 | 34.52 | 0.639 | |
| coded | DLSS SR | 66.06 | 62.30 | 33.71 | 0.652 | |
| coded | VSR Low (q1) | 74.79 | 71.43 | 35.37 | 0.761 | 0.23 |
| coded | VSR Medium (q2) | 75.73 | 72.24 | 35.33 | 0.947 | 0.27 |
| coded | **VSR High (q3)** | **78.53** | **74.58** | 35.56 | 0.953 | 0.46 |
| coded | VSR Ultra (q4) | 78.38 | 74.55 | **35.66** | 0.988 | 0.61 |

Per clip, VMAF (clean / coded):

| clip | bicubic | lanczos | DLSS SR | VSR High |
|---|---|---|---|---|
| demo | 81.39 / 69.22 | 82.31 / 70.09 | 76.74 / 66.64 | **92.80 / 81.36** |
| ncd | 81.62 / 65.90 | 82.93 / 67.17 | 76.60 / 62.38 | **91.20 / 76.45** |
| still | 81.14 / 75.05 | 81.83 / 75.65 | 79.23 / 74.32 | **90.04 / 84.66** |
| pan | 79.60 / 69.71 | 81.72 / 71.67 | 78.39 / 68.92 | **88.30 / 80.52** |
| mpan | 71.61 / 62.48 | **72.77** / 63.71 | 62.43 / 56.68 | 72.34 / **68.31** |
| ts | 87.40 / 74.71 | 87.47 / 74.72 | 75.94 / 67.44 | **90.15 / 79.87** |

Reading it:

- **Real footage gains 9 to 11 VMAF clean and 10 to 12 compressed** at High (`demo`,
  `ncd`, `still`, `pan`), and 7 to 11 by VMAF NEG. PSNR rises with it, by 1.0 to 2.4 dB
  on the same clips, so this is detail put back, not contrast added.
- **Synthetic worst cases**: on `mpan` (a Mandelbrot pan, detail at every scale) VSR
  only ties the scalers on clean input, and its PSNR is 0.7 dB lower; on `ts` (a test
  pattern) Low and Medium fall below bicubic and High is above it.
- **High is the level to use.** Ultra is no better on average and costs a third more;
  Low and Medium give up 2 to 3 VMAF.
- **DLSS SR is below bicubic on every clip and both inputs**, as `sr-quality-20260924`
  found on another GPU.
- **Temporal cost.** Error flicker is 0 on `still` for every arm, so the network is
  deterministic on a held frame. On moving clips VSR's is 1.4 to 3.8 times
  bicubic's (`demo` clean 0.64 against 0.17). VMAF does not score that. It is the
  case for keeping a plain scaler one click away, not against the default.

## Limits

- One GPU (RTX 5090), one scale (2x, 540p to 1080p), 60 frames a clip.
- No LPIPS; VMAF NEG and PSNR stand in for a check that the gain is not sharpening.
- The High-Bitrate modes NVIDIA recommends for clean input are not in the pinned DLL,
  so the clean-input numbers are, if anything, VSR's lower bound.
- RTX VSR here upscales the original. On a neural render (P2.39 in
  `docs/IMPROVEMENT-TASKS.md`) it has not been measured.

## Reproduce

```sh
cmake --build --preset vs2022 --target UpscalingGpuSmoke
python docs/measurements/vsr-quality-20261002/run.py <work dir> --frames 60
```

`run.py` builds every clip from the two committed MP4s and FFmpeg's lavfi sources,
runs each arm, scores it, and writes `results.csv`. It needs a build with the RTX
Video SDK staged (`VsrGpuSmoke` registered), Python 3 and NumPy.
