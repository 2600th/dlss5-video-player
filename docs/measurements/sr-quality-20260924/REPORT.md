# DLSS Super Resolution against bicubic on video — 24 September 2026, RTX 4080 SUPER

**Verdict: one real bug, fixed; the rest is the model.** The flow engine (NVOFA)
reports a fixed sub-pixel motion field for two *identical* frames. Super Resolution
took that field as motion and re-sampled its history by it every frame, which is why
a held frame lost 17 VMAF in 60 frames and why moving clips scored about 11 VMAF
lower than they had to. The resolve pass now zeroes every vector that explains the
frame pair no better than zero motion does, and only in Super Resolution sessions.
With that fixed, a held frame no longer decays (79.2 → 80.7) and `demo-capture` goes
from 63.9 to 75.2.

**DLSS SR still scores below bicubic, and the fix cannot change that.** On every clip
tried, DLSS SR's *first frame* already scores 2 to 6 VMAF below a bicubic upscale of
the same 960x540 frames. The first frame has no history and no motion vectors, so the
guides cannot be the cause. Every colour contract, preset and quality mode the pinned
runtime offers was tried, and none closes the gap. A synthetic pan with *exact* motion
vectors shows the same thing: temporal accumulation scores below DLSS's own
single-frame output (78.5 against 93.1, with bicubic at 95.8). DLSS SR is built for
rendered frames: sub-pixel jittered, aliased, noise-free point samples. Decoded video
has no jitter, is band-limited by its own downscale and carries codec noise. On that
input, accumulation adds blur and ringing rather than detail. The brief's target
("clearly above bicubic") is not reachable in this pipeline, and this report shows why.

## Environment

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 4080 SUPER, 16047 MiB, driver 610.47 |
| DLSS | `nvngx_dlss.dll` 310.7.0 staged beside the test executable, SDK headers from `external/DLSS`; preset K (the shipped hint), MaxQuality candidate, 960x540 → 1920x1080 (`range=960x540..1920x1080`) |
| Flow | NVOFA 960x540, 2x2 grid, perf FAST, both directions, global flow on (the shipped mode) |
| FFmpeg | `external/ffmpeg/bin` 9.0.1 (gyan essentials), `libvmaf` default model, luma PSNR |
| Harness | `UpscalingGpuSmoke <clip> 1080 <raw> mv=1,depth=1 <frames>`: the guide probe. It runs the real renderer in preserve-source SR, drains the capture ring (`RenderFrameForCache`, the export's own capture) and writes BGRA frames |
| Scoring | `vmaf.sh` in this folder: BGRA → BT.709 limited YUV 4:2:0 → `libvmaf` against the 1080p original, per frame |

## Clips

Each clip is the first 180 frames (60 for the still and the pans) of a 1080p30
original, tagged BT.709, stored lossless (ffv1). It is area-reduced to 960x540, which
is the input SR and bicubic both receive.

- `demo`: `tools/benchmark/fixtures/demo-capture-20260912.mp4`. A dark interior with small bright highlights.
- `ncd`: `docs/media/neural-comparison-demo.mp4`. A near-static frame with a wipe and overlays fading in, from lossy footage.
- `still`: `demo` frame 0, held for 60 frames.
- `pan`: a 1920x1080 window sliding 1 px per frame across a 2400x1350 lanczos enlargement of `ncd` frame 0. That is exactly 0.5 input px per frame, so the exact motion vector is (-0.5, 0).
- `mpan`: the same pan over a `mandelbrot` still, for detail at every scale.
- `ts`: `testsrc2` at 1080p.

## The baseline has to go through BGRA too

SR's output reaches the scorer as 8-bit BGRA, because that is what the capture reads
back. A bicubic upscale done in YUV skips that round trip. The round trip costs
**2.9 VMAF** on `still` by itself: the same bicubic frames score 84.09 straight from
YUV and 81.23 once decoded to BGRA and converted back. So the fair baseline is bicubic
of the decoder's BGRA frames, scored through the same conversion (`fairbic.sh`). The
YUV numbers are given too, because they are the ones the wave-3 table used.

| clip | bicubic, YUV | **bicubic, fair (BGRA)** |
|---|---|---|
| demo | 82.92 | **80.91** |
| ncd | 86.07 | **84.93** |
| still | 84.09 | **81.14** |
| pan | 96.36 | **95.83** |
| mpan | 64.91 | **63.26** |
| ts | 88.45 | **88.07** |

## 1. Reproduction and the bisection

SR as shipped (VMAF mean; `first10` → `last10` where the trend matters):

| clip | SR as shipped | frame 0 |
|---|---|---|
| still | 66.65 (76.98 → **62.35**) | 79.24 |
| demo | 63.86 (72.44 → 56.58) | 79.24 |
| ncd | 58.43 | 90.97 |

The still's decay reproduces the wave-3 finding (74.8 → 62). Each suspect in the brief
was switched on its own, on `still`. Switches that needed code went through
environment variables in an uncommitted build.

| arm (still) | VMAF | first10 → last10 | reading |
|---|---|---|---|
| shipped | 66.65 | 76.98 → 62.35 | decays |
| motion vectors off (`mv=0`) | **80.47** | 79.66 → 80.65 | **no decay** |
| CPU estimator instead of NVOFA | **80.47** | 79.66 → 80.65 | **no decay** |
| flat depth (`depth=0`) | 66.82 | 77.04 → 62.49 | depth is not it |
| `IsHDR` (linear input, HDR mode, auto-exposure) | 57.63 | 64.67 → 55.01 | worse: auto-exposure on a dark clip |
| `IsHDR`, no auto-exposure | 67.55 | 77.55 → 63.47 | colour is not the decay |
| sRGB-encoded input in LDR mode, linearised after | 66.06 | 75.91 → 61.61 | colour is not the decay |
| reset every frame (spatial only) | 79.24 | flat | the ceiling without history |

The decay follows the NVOFA vectors and nothing else. Reading the motion texture NGX
receives back, on `still` (identical frames):

```
EXP MV frame=2  nvof=1 mean|mv|=0.0571161 max=0.349386 nonzero=0.474583
...                                (identical on every frame)
EXP MV frame=60 nvof=1 mean|mv|=0.0571161 max=0.349386 nonzero=0.474583
```

The engine returns the same field for every identical pair: 47 % of pixels move, by
0.057 px on average and up to 0.35 px. It is not noise, because it repeats frame after
frame, and so the error accumulates. Each frame DLSS re-samples its history by that
field. That is a sub-pixel blur applied 60 times.

The other suspects in the brief were read and ruled out:

- **Motion-vector units and sign.** The vectors are current→previous, in input pixels, with `MVLowRes` and scale 1,1. That is correct: a sweep of exact constant vectors on `pan` scores the true (-0.5, 0) above the wrong sign (+0.5, 0): 78.5 against 72.3.
- **Jitter** is pinned to (0, 0).
- **Reset** fires on the first frame and on cuts only.
- **Depth**: flat depth changes nothing (above).
- **Exposure**: `AutoExposure` on or off makes no difference in LDR mode. The two runs are bit-identical.
- **Resource states and history lifetime**: one feature for the whole job, and history is kept, as the `mv=0` arm shows.

## 2. The fix and what it buys

In the resolve pass (`src/NvofResolveShader.h`), a vector is kept only where it matches
the current frame to the previous one better than zero motion does. The comparison is a
3x3 luma sum of absolute differences over the engine's own two input copies, and a tie
goes to zero. It is the same null hypothesis the CPU estimator already applies to every
cell. It runs only in Super Resolution sessions (`m_preserveSource`). A neural render at
source size keeps exactly the field it was cached with, so no default cache key moves.
The reduced processing-scale rungs do run on an SR carrier, so their cache term moves
from `-v1` to `-v2`.

Read back after the fix, on `still`: `mean|mv|=0 max=0 nonzero=0` on every frame.

| clip | fair bicubic | SR before | **SR after** | SR reset every frame |
|---|---|---|---|---|
| still | 81.14 | 66.65 (last10 62.35) | **80.47 (last10 80.65)** | 79.24 |
| demo | 80.91 | 63.86 | **75.18** | 75.97 |
| ncd | 84.93 | 58.43 | **61.18** | 79.26 |
| pan | 95.83 | 73.91 | **75.95** | 93.11 |
| ts | 88.07 | – | **72.42** | 76.73 |

Requiring the vector to beat zero motion by a margin (1, 2 or 4 code values per pixel)
moved `demo` and `ncd` by at most 0.3 VMAF and `pan` by +1.6 / −0.6, so the fix ships
without a margin.

## 3. Why SR stays below bicubic: the evidence it is the model on this input

1. **Frame 0 is already below bicubic.** Frame 0 has no history and no vectors. DLSS SR scores 79.24 on `still` and `demo`, 90.97 on `ncd`, 92.08 on `pan`, 57.04 on `mpan` and 72.88 on `ts`, against fair bicubic's frame 0 of 81.14, 81.14, 94.79, 94.73, 62.94 and 83.76. The output is registered to the reference: a sub-pixel search of ±0.75 px peaks at (0, 0).
2. **No input contract closes the gap.** Frame 0 on `still`, by variant: linear LDR (shipped) 79.24; linear LDR without auto-exposure 79.24; sRGB-encoded LDR, as NVIDIA's guide asks for LDR input, 77.14; HDR without auto-exposure 79.82; HDR with auto-exposure 66.31. Presets: K 79.24, J 78.75, M 76.31, L 74.22, default (M) 76.31. The quality mode (Performance, Balanced, Ultra Performance) makes no difference with the preset held fixed.
3. **Exact vectors do not make accumulation help.** On `pan`, with the exact (-0.5, 0) and flow off, DLSS scores 78.53 (sRGB-encoded input 84.95). Spatial-only DLSS on the same clip scores 93.11, and bicubic 95.83. Wrong vectors that are *larger* score *better* (-1.0: 85.6; -2.0: 88.0) because DLSS then discards more of its history. Accumulation is what costs quality on this input.
4. **With the fix, colour encoding is a wash.** sRGB-encoded against linear input, both with the zero-motion test: `still` 79.16 vs 80.47, `demo` 74.01 vs 75.18, `ncd` 62.77 vs 61.18, `pan` 78.90 vs 75.95. The shipped linear contract was kept: it breaks the letter of NVIDIA's LDR guidance, but measures no worse on real footage. Changing it would move every cached SR-carrier render.

Two things were measured but not changed:

- **Resetting every frame beats temporal SR on three of five clips** (`ncd` by 18 VMAF). That trades away temporal stability, which VMAF does not score, so it is a product decision rather than a fix.
- **Generated zoom content is the worst case.** In the new GPU gate's Mandelbrot zoom, SR scores 40.2 against bicubic's 72.3.

## 4. The regression gate

`UpscalingSrQualitySmoke` (label `gpu`) runs `UpscalingGpuSmoke <ffmpeg> sr-quality <dir>`.
It generates a Mandelbrot clip and its first frame held for 60 frames, upscales both
through the SR path, scores them against the 1080p originals, and fails when the held
frame's last ten frames score more than 1 VMAF below its first ten. It prints bicubic
beside SR, scored through the same BGRA path, but does not assert an order between
them, for the reasons in section 3.

| build | still first10 → last10 | exit |
|---|---|---|
| with the fix | 63.17 → 64.62 | 0 |
| zero-motion test switched off | 62.73 → **56.04** | **9** |
