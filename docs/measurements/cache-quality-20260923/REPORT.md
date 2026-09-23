# Cache quality ladder and capture dither (P2.3, P2.2) - 23 September 2026, RTX 4080 SUPER

**Verdict: the Standard rung is not the constant-quality encode its name says, and the
High rung is what fixes it.** hevc_nvenc with `-rc vbr -cq 16 -b:v 0` and no `-maxrate`
holds a hidden VBR ceiling of about 10.7 Mbit/s at 1080p (about 32 at 4K): on the
detailed fractal clip every CQ from 10 to 20 produced the same 43.7 KiB a frame, and the
render scored VMAF 81.7 against its own lossless capture. Lifting the ceiling and
capturing 10 bits (High, HEVC Main10 CQ 14) scores 98.7 on that clip and 97.6 on
average against 93.7, and removes the banding Standard's 8-bit path leaves in smooth
gradients (CAMBI 6.0 -> 0.9 on average, 10.6 -> 0.9 on the gradient clip), at 2.2x the
bytes on average (1x on easy content, 5x on the detailed clip). Render time did not
change between rungs. Standard stays the default, as the brief requires; **whether High
should become the default is the product decision this measurement is for** - the
visible blocking reported on the GTA VI trailer (issue #13) is what a 10.7 Mbit/s
ceiling on detailed 1080p footage looks like. **Review ruling (section "Ruling A"
below): the ceiling is now lifted on Standard too**, so Standard is constant quality
at CQ 16 (VMAF 93.5 -> 96.9 on the rebased tree) and stays the default.

Before that ruling the Standard rung's own bytes did not move: `real-film-cuts` rendered
at Standard before the ladder existed (21:13) and after every change in the package
(22:20) decoded to the same frame digest. The worker's own High renders, run after the ceiling was lifted,
match the re-encoded `main10u` CQ 14 arm below to within 0.03 VMAF and 2 % in size.

The capture dither is off by default on a strict reading of the brief's bar, with a
recommendation to turn it on; see the P2.2 section.

## Environment and instruments

| Item | Value |
|---|---|
| GPU / driver | NVIDIA GeForce RTX 4080 SUPER, driver `32.0.16.1047` |
| CPU / OS | 28 logical processors, Windows 11 Pro 10.0.26200 |
| Runtime | DLSS-NR 310.8.0, ReShade 6.8.0.2155, RenoDX 6.5.3, worker 0.25.0 |
| Tree | branch `wip/w2-quality` from `8afb573`, this change |
| Offline renders | `tools/benchmark/run.py` (the real offline worker), profile file [`quality.profile.json`](quality.profile.json) |
| Clips | `highlights-gradients`, `fine-detail`, `text-subtitles`, `real-film-cuts`, `real-game-motion` from `tools/benchmark/corpus.py`, 1920x1080, 30 fps |
| Scoring | FFmpeg 9.0.1 `libvmaf` (model `vmaf_v0.6.1`, features `psnr` and `cambi`), both sides at `yuv420p10le`; an 8-bit arm is shifted up exactly (`x << 2`, checked: 235 -> 940) |

Every profile writes all ten player keys at their `src/NeuralSettings.h` defaults. The
Lossless render is the reference for every score: the neural frames are bit-identical
run to run (two Lossless repeats of `real-game-motion` and `highlights-gradients` gave
identical decoded-frame digests), so a score against it measures the capture and the
encode and nothing else.

`temporal error` below is the mean over consecutive frame pairs of mean |e(t) - e(t-1)|,
with e the luma error against the reference, in 8-bit code values: the flicker an encode
or a dither adds on top of what the neural frames already do.

## P2.3: the ladder

The three rungs, through the worker (`--cache-quality high|lossless`), and the arms that
separate the two things High changes. `main10u` is the High rung's exact encoder
arguments (`-maxrate 800M -bufsize 800M`, Main10, P010) fed the Lossless render's own
P010 samples, which are the High rung's capture; `main10` is the same with NVENC's
ceiling left in place; `8bit` and `8bitu` are the Standard arguments (NV12) with and
without the ceiling, fed FFmpeg's 10-to-8-bit conversion of the same samples.

| clip | arm | VMAF | VMAF p1 | PSNR-Y | CAMBI | temporal error | KiB/frame |
|---|---|---:|---:|---:|---:|---:|---:|
| highlights-gradients | **Standard** (worker) | 93.70 | 92.74 | 53.88 | 10.57 | 0.397 | 36.6 |
| highlights-gradients | main10, capped, CQ 16 | 94.38 | 93.28 | 56.55 | 0.98 | 0.253 | 26.3 |
| highlights-gradients | main10u CQ 16 | 94.42 | 93.36 | 56.59 | 0.96 | 0.252 | 26.7 |
| highlights-gradients | **High** = main10u CQ 14 | 94.93 | 94.10 | 57.13 | 0.91 | 0.243 | 33.2 |
| highlights-gradients | 8bit, capped, CQ 16 | 93.56 | 92.54 | 53.15 | 2.94 | 0.404 | 37.6 |
| highlights-gradients | 8bitu CQ 16 | 93.80 | 92.75 | 53.26 | 2.82 | 0.400 | 41.7 |
| highlights-gradients | **Lossless** (worker) | 100 | 100 | - | 0.23 | 0 | 717.6 |
| fine-detail | **Standard** (worker) | 81.72 | 72.03 | 34.89 | 0.87 | 3.546 | 43.7 |
| fine-detail | main10, capped, CQ 16 | 81.48 | 71.27 | 34.75 | 0.27 | 3.578 | 43.7 |
| fine-detail | main10u CQ 16 | 97.41 | 93.23 | 41.39 | 0.33 | 1.883 | 174.7 |
| fine-detail | **High** = main10u CQ 14 | 98.74 | 95.35 | 42.90 | 0.34 | 1.620 | 219.0 |
| fine-detail | 8bit, capped, CQ 16 | 81.40 | 71.73 | 34.81 | 0.70 | 3.571 | 43.8 |
| fine-detail | 8bitu CQ 16 | 97.29 | 93.08 | 41.32 | 0.40 | 1.920 | 174.5 |
| fine-detail | **Lossless** (worker) | 100 | 100 | - | 0.25 | 0 | 1813.4 |
| text-subtitles | **Standard** (worker) | 97.26 | 97.09 | 58.05 | 7.78 | 0.140 | 15.7 |
| text-subtitles | main10, capped, CQ 16 | 97.40 | 97.27 | 60.57 | 1.62 | 0.110 | 13.5 |
| text-subtitles | main10u CQ 16 | 97.41 | 97.27 | 60.66 | 1.59 | 0.109 | 13.7 |
| text-subtitles | **High** = main10u CQ 14 | 97.45 | 97.35 | 61.21 | 1.58 | 0.107 | 15.0 |
| text-subtitles | 8bit, capped, CQ 16 | 97.35 | 97.17 | 56.22 | 1.57 | 0.150 | 16.2 |
| text-subtitles | 8bitu CQ 16 | 97.36 | 97.18 | 56.23 | 1.50 | 0.149 | 16.3 |
| text-subtitles | **Lossless** (worker) | 100 | 100 | - | 0.61 | 0 | 510.6 |
| real-film-cuts | **Standard** (worker) | 96.86 | 94.06 | 53.15 | 8.83 | 0.309 | 32.9 |
| real-film-cuts | main10, capped, CQ 16 | 97.09 | 94.41 | 55.31 | 1.59 | 0.225 | 24.4 |
| real-film-cuts | main10u CQ 16 | 97.17 | 95.54 | 55.54 | 1.59 | 0.218 | 26.6 |
| real-film-cuts | **High** = main10u CQ 14 | 97.27 | 95.69 | 56.08 | 1.53 | 0.210 | 31.6 |
| real-film-cuts | 8bit, capped, CQ 16 | 96.76 | 93.67 | 52.49 | 2.95 | 0.308 | 32.9 |
| real-film-cuts | 8bitu CQ 16 | 96.91 | 95.05 | 52.74 | 2.87 | 0.303 | 39.4 |
| real-film-cuts | **Lossless** (worker) | 100 | 100 | - | 1.90 | 0 | 756.5 |
| real-game-motion | **Standard** (worker) | 98.97 | 94.94 | 49.39 | 2.04 | 0.731 | 46.0 |
| real-game-motion | main10, capped, CQ 16 | 99.23 | 95.36 | 50.75 | 0.08 | 0.586 | 45.8 |
| real-game-motion | main10u CQ 16 | 99.57 | 96.46 | 52.44 | 0.06 | 0.495 | 66.0 |
| real-game-motion | **High** = main10u CQ 14 | 99.66 | 96.74 | 53.62 | 0.06 | 0.441 | 84.8 |
| real-game-motion | 8bit, capped, CQ 16 | 98.86 | 94.66 | 49.12 | 1.16 | 0.742 | 46.6 |
| real-game-motion | 8bitu CQ 16 | 99.39 | 96.11 | 50.56 | 0.95 | 0.654 | 77.6 |
| real-game-motion | **Lossless** (worker) | 100 | 100 | - | 0.00 | 0 | 1121.2 |

Means over the five clips:

| rung | VMAF mean (worst clip) | KiB/frame mean (max) | Mbit/s at 30 fps, mean | CAMBI mean |
|---|---:|---:|---:|---:|
| Standard | 93.70 (81.72) | 35.0 (46.0) | 8.6 | 6.02 |
| High at CQ 16 (not shipped) | 97.19 (94.42) | 61.5 (174.7) | 15.1 | 0.91 |
| **High, CQ 14** | 97.61 (94.93) | 76.7 (219.0) | 18.8 | 0.88 |
| Lossless | 100 | 983.9 (1813.4) | 241.8 | 0.60 (the reference) |

What the separating arms say:

- **The ceiling.** Capped and uncapped arms are the same size wherever the ceiling does
  not bind (text, gradients) and 1.5-4x apart where it does (game motion, fine detail).
  On `fine-detail` the ceiling alone costs 16 VMAF (`8bit` 81.4 against `8bitu` 97.3);
  on the others it costs 0.2-0.5.
- **Ten bits.** At the same ceiling and CQ, Main10 against 8-bit is +0.1 to +0.8 VMAF,
  +2 to +3 dB PSNR-Y, and the banding difference: CAMBI 0.1-1.6 against 0.7-2.9 even
  when the 8-bit arm is dithered by FFmpeg's 10-to-8 conversion, and against 7.8-10.6
  for Standard's real capture, which is not.
- **CQ 14 over CQ 16** is +0.5 VMAF on the gradients and +1.3 on the detailed clip,
  under 0.1 elsewhere, for 25 % more bytes. CQ 14 was taken because the rung exists for
  quality and Lossless exists above it; CQ 16 would be a defensible, cheaper choice.
- **The ceiling value.** `-maxrate 1000M -bufsize 2000M` was refused by NVENC
  (`InitializeEncoder failed: invalid param (8)`); 800M/800M opens at 1080p and 4K. At
  4K a 30-frame upscale of `fine-detail` wrote 32 Mbit/s capped and 161 uncapped.

Cost:

| | Standard | High | Lossless |
|---|---|---|---|
| Worker throughput, 1080p, `proc_fps` (5 clips) | 90-103 fps | 98-103 fps | 91-111 fps |
| Encoder alone, 1080p | NVENC 3.4-5.6 ms/frame | NVENC 3.4-5.3 ms/frame | FFV1 110-178 fps (5.6-9.1 ms/frame), CPU |
| Encoder alone, 4K (upscaled film clip) | - | - | FFV1 70 fps encode, 72 fps decode to NV12 |
| Size, 1080p30 | ~8.6 Mbit/s | ~19 Mbit/s (54 on the detailed clip) | ~240 Mbit/s, 1.8 GB a minute |

At 1080p the neural pass is the long pole for every rung. FFV1 at 4K runs at about 70
frames a second on this 28-thread CPU, which keeps up with a 4K neural pass but leaves
little headroom for real-time playback of a 4K Lossless cache.

Playback and export of the 10-bit rungs go through the existing decoders:
`tests/CachedExportTests.cpp` (`QualityLadderRoundTripTest`) encodes P010 frames of known
codes through the shipped Lossless and High (x264 fallback) arguments, checks FFV1 hands
them back bit for bit, plays FFV1, x264 High 10 and libx265 Main10 files through
`VideoDecoder` in both NV12 and BGRA, exports FFV1 to MP4 as lossless 10-bit H.264 (bit
for bit again) and carries Main10 into MP4 as `hvc1` without re-encoding.

## P2.2: the capture dither

The window present dithers against a 64x64 blue-noise map; that is presentation-only and
always on. For the capture the question was different, because its frames go on to HEVC.
Cutting each clip's Lossless render to 8 bits four ways, written losslessly and then
through the Standard rung's NVENC arguments (`hevc_nvenc -cq 16`, capped), CAMBI (scored
at 8 bits here, so not comparable with the 10-bit-scored tables):

| clip | round to nearest | blue noise | Bayer 8x8 | FFmpeg's default 10-to-8 |
|---|---|---|---|---|
| highlights-gradients, lossless 8-bit | 2.461 | **0.048** | 0.504 | 0.525 |
| highlights-gradients, after NVENC | 6.459 | 5.727 | **1.618** | 1.613 |
| real-film-cuts, lossless 8-bit | 4.331 | **0.222** | 1.841 | 1.859 |
| real-film-cuts, after NVENC | 5.953 | 4.939 | **2.331** | 2.329 |

Blue noise is the best 8-bit picture and the encoder erases it: its energy is at exactly
the high spatial frequencies the quantiser spends least on. An 8x8 ordered (Bayer) map
survives, because a period-8 tile lands on a handful of transform coefficients (FFmpeg's
own 10-to-8 dither is the same matrix and scores the same). So the capture dithers with
the ordered map, and the window keeps blue noise.

The first implementation put blue noise at the capture too. Through the worker, against
Standard with no dither, it cut CAMBI by 5-9 % and cost 0.84 VMAF on `fine-detail`, where
the bitrate ceiling binds and dither noise competes for bits it has already rationed:

| clip | VMAF | CAMBI | temporal error |
|---|---:|---:|---:|
| highlights-gradients | 93.70 -> 93.66 | 10.57 -> 9.78 | 0.397 -> 0.403 |
| fine-detail | 81.72 -> 80.88 | 0.87 -> 1.02 | 3.546 -> 3.559 |
| text-subtitles | 97.26 -> 97.26 | 7.78 -> 7.33 | 0.140 -> 0.139 |
| real-film-cuts | 96.86 -> 96.86 | 8.83 -> 8.44 | 0.309 -> 0.309 |
| real-game-motion | 98.97 -> 98.96 | 2.04 -> 2.03 | 0.731 -> 0.732 |

The shipped ordered map, through the worker (`--capture-dither 1`), on the default BGRA
capture and on the GPU NV12 capture (`--gpu-color-conversion 1`), each against the same
path without dither (VMAF, CAMBI and temporal error against the Lossless render):

| clip | path | VMAF | CAMBI | temporal error | KiB/frame |
|---|---|---:|---:|---:|---:|
| highlights-gradients | BGRA | 93.70 -> 93.71 | 10.57 -> 4.52 | 0.397 -> 0.404 | 36.6 -> 36.7 |
| highlights-gradients | NV12 | 93.78 -> 93.62 | 10.57 -> 3.03 | 0.384 -> 0.403 | 35.9 -> 37.0 |
| fine-detail | BGRA | 81.72 -> 81.74 | 0.87 -> 0.89 | 3.546 -> 3.548 | 43.7 -> 43.7 |
| fine-detail | NV12 | 81.48 -> 81.52 | 0.71 -> 0.71 | 3.563 -> 3.563 | 43.8 -> 43.8 |
| text-subtitles | BGRA | 97.26 -> 97.29 | 7.78 -> 2.88 | 0.140 -> 0.147 | 15.7 -> 15.2 |
| text-subtitles | NV12 | 97.30 -> 97.33 | 7.74 -> 1.67 | 0.135 -> 0.149 | 15.1 -> 15.5 |
| real-film-cuts | BGRA | 96.86 -> 96.84 | 8.83 -> 4.91 | 0.309 -> 0.309 | 32.9 -> 32.4 |
| real-film-cuts | NV12 | 96.87 -> 96.80 | 9.08 -> 3.20 | 0.305 -> 0.306 | 31.8 -> 32.3 |
| real-game-motion | BGRA | 98.97 -> 98.93 | 2.04 -> 1.40 | 0.731 -> 0.741 | 46.0 -> 45.9 |
| real-game-motion | NV12 | 98.98 -> 98.91 | 2.06 -> 1.12 | 0.725 -> 0.743 | 45.9 -> 46.3 |

Plain output flicker (mean |Y(t) - Y(t-1)| in 8-bit codes, cut frames skipped) on the
BGRA path, no dither -> ordered dither: 1.225 -> 1.224, 6.879 -> 6.882, 0.203 -> 0.210,
1.267 -> 1.259, 5.570 -> 5.568.

**Default: off.** The ordered dither removes 44-63 % of the Standard rung's banding on
the three clips that have any (and 66-78 % on the NV12 path) at an unchanged size, but the
brief's bar is no VMAF or flicker regression, and the deltas are not all zero: VMAF moves
-0.04 to +0.03 on the BGRA path and -0.16 to +0.04 on NV12, the temporal error rises by
up to 0.010 codes (0.019 on NV12), and plain flicker on the text clip by 0.007 codes. All
of it is two orders of magnitude below a visible difference and mixed in sign, so the
recommendation is to turn it on; that call is left to review rather than taken against
the stated bar. The High rung removes the banding at the source instead (CAMBI 0.9),
which is why the dither matters only to Standard.

## Ruling A: the Standard rung made constant quality

The review ruled that Standard should be what its name says. `BuildEncoderArguments`
now passes `-maxrate 800M -bufsize 800M` to NVENC on every rung, so `-cq 16` alone sets
Standard's quality; the libx264 fallback (CRF 16) had no ceiling to lift. Standard's bytes
changed, so its key term is `standard-cq16-uncapped-v1`, and every render made under the
ceiling is retired.

Measured on the rebased tree (integration branch `07730d7` plus this package), through the
worker, on the same five clips. **The reference was re-rendered for this section**: the
rebased tree shows the model different input on these untagged corpus clips (the
integration branch's untagged-colour and temporal changes), so a render from before the
rebase is not comparable with one from after it, and the tables above stay against their
own reference. "Capped" is the Standard rung from a worker built at `8b22363` (the commit
before the ruling); "uncapped" is the ruling's worker; both render bit-identical neural
frames, so the encode is the only difference. Mbit/s at 30 fps.

| clip | arm | VMAF | VMAF p1 | PSNR-Y | CAMBI | temporal error | Mbit/s |
|---|---|---:|---:|---:|---:|---:|---:|
| highlights-gradients | capped | 93.78 | 92.79 | 54.11 | 11.06 | 0.385 | 9.0 |
| highlights-gradients | uncapped | 93.88 | 92.98 | 54.16 | 11.02 | 0.383 | 9.4 |
| fine-detail | capped | 80.32 | 71.09 | 34.83 | 1.45 | 3.634 | 10.7 |
| fine-detail | uncapped | 96.81 | 92.79 | 41.39 | 1.57 | 1.934 | 42.8 |
| text-subtitles | capped | 97.32 | 97.16 | 55.27 | 10.26 | 0.169 | 4.0 |
| text-subtitles | uncapped | 97.32 | 97.18 | 55.29 | 10.25 | 0.169 | 4.0 |
| real-film-cuts | capped | 96.87 | 94.07 | 53.17 | 8.76 | 0.310 | 8.1 |
| real-film-cuts | uncapped | 96.98 | 94.96 | 53.37 | 8.76 | 0.304 | 9.1 |
| real-game-motion | capped | 98.99 | 95.06 | 49.36 | 2.00 | 0.734 | 11.3 |
| real-game-motion | uncapped | 99.44 | 96.20 | 50.82 | 1.96 | 0.647 | 17.9 |

Means: VMAF 93.46 -> 96.89 (worst clip 80.32 -> 93.88), 8.6 -> 16.7 Mbit/s (largest 11.3 ->
42.8). Where the ceiling never bound (text) nothing moved; where it did, the encode spent
what CQ 16 asks for and the error that flickered frame to frame fell with it (temporal error
3.63 -> 1.93 on `fine-detail`). Worker throughput was unchanged: 101.6-104.4 frames a second
capped, 101.8-104.7 uncapped. Frame generation's encoder shares these arguments, so its
conversions are constant quality now too; they are not cached, so no key moves for them.

## Reproducing

```
python tools/benchmark/corpus.py
python tools/benchmark/run.py --profile-file docs/measurements/cache-quality-20260923/quality.profile.json \
    --profiles q-lossless q-high q-standard q-dither q-nv12 q-nv12-dither \
    --clips highlights-gradients fine-detail text-subtitles real-film-cuts real-game-motion
```

then score each run's `output.mkv` against the same clip's `q-lossless` output with
`ffmpeg -i <arm> -i <lossless> -lavfi "[0:v]format=yuv420p10le[d];[1:v]format=yuv420p10le[r];[d][r]libvmaf=feature=name=psnr|name=cambi:log_fmt=json:log_path=v.json" -f null -`.
The `main10u`/`8bit` arms re-encode the Lossless render's decoded P010 through the
arguments `BuildEncoderArguments` writes, from raw input, so the timed step is the
encode alone.
