# NVENC adaptive quantisation and lookahead (P2.50) — 16 October 2026

**Verdict: AQ and lookahead lose at matched size; do not adopt.** Against the cache
encode's own settings, `-spatial-aq 1` needs **+15 % to +77 % more bits** for the same
VMAF (BD-rate). `-spatial-aq 1 -temporal-aq 1 -rc-lookahead 20` needs **+20 % to +72 %**.
Both lose on every input, both rungs, and on VMAF NEG too. At the shipped encode's own
size, VMAF falls 0.03-0.53 and its 5th percentile 0.2-1.6. The one thing they buy is a
little less banding on the 8-bit Standard rung (CAMBI −0.1 to −0.6 at matched size on the
neural renders, from a base of 0.25-1.84); the High rung's 10 bits already remove more
of it. Encode-speed cost is within the noise of this run (median −5 % to −14 % on
Standard, none on High).

## The encode reproduced

`src/MediaPipeline.cpp` `BuildEncoderArguments`, which `NvencDirectPolicy.h`
`ApplyHevcNvencOptions` copies field by field:

- `-c:v hevc_nvenc -preset p5 -tune hq -rc vbr -cq Q -b:v 0 -maxrate 800M -bufsize 800M -split_encode_mode auto`
- the BT.709 limited tags, GOP 250, the preset's B-frames (no `-bf` / `-b_ref_mode` override)
- no AQ, no lookahead, multipass off
- Standard: `-pix_fmt nv12`, CQ 16
- High: `-profile:v main10 -pix_fmt p010le`, CQ 14 (`kHighRungCq`)

Rate control is VBR with `-cq`, not constqp, so AQ and lookahead are honoured.

## Method

`tools/benchmark/nvenc_aq.py`. Inputs:

- the five Lossless neural renders of passes-drift-20261016 (FFV1 10-bit, 1 pass, shipped settings), at both rungs
- the five source clips, at Standard only

Each input is decoded once to a raw file: NV12 via FFmpeg's 10-to-8-bit conversion for
Standard, P010 for High. Each arm is encoded at four CQs (Standard 16/19/22/25, High
14/17/20/23) and scored against exactly the frames it was given, 8-bit against 8-bit and
10-bit against 10-bit. Scoring: libvmaf `vmaf_v0.6.1`, `vmaf_v0.6.1neg` and CAMBI on the
encode, frames paired by index (`settb=1/30,setpts=N`). BD-rate uses log rate over VMAF
with piecewise-cubic (PCHIP) interpolation, over the overlapping VMAF range. Encode fps is
taken at the shipped CQ with the input looped ten times, from page cache.

## Environment

RTX 5090 (NVENC), driver 617.14; FFmpeg n9.0.2-14-gebafaee10a-20260929 with built-in
libvmaf models; 16 logical CPUs.

## Results

BD-rate on VMAF (positive = more bits for the same score), arm vs today's encode:

| input | Standard, spatial AQ | Standard, S+T AQ + LA20 | High, spatial AQ | High, S+T AQ + LA20 |
|---|---:|---:|---:|---:|
| neural `real-film-cuts` | +77 % | +72 % | +47 % | +46 % |
| neural `real-game-cuts` | +18 % | +37 % | +15 % | +28 % |
| neural `real-game-motion` | +32 % | +42 % | +20 % | +30 % |
| neural `orig-faces` | +43 % | +39 % | +31 % | +24 % |
| neural `orig-game-motion` | +29 % | +28 % | +19 % | +20 % |
| source `real-film-cuts` | +60 % | +55 % | - | - |
| source `real-game-cuts` | +18 % | +50 % | - | - |
| source `real-game-motion` | +30 % | +39 % | - | - |
| source `orig-faces` | +36 % | +48 % | - | - |
| source `orig-game-motion` | +21 % | +22 % | - | - |

VMAF NEG gives the same picture (`nvenc_aq_bdrate.csv`).

At the shipped encode's bitrate, interpolated (`nvenc_aq_matched_size.csv`), the neural
renders on Standard:

| | ΔVMAF | ΔVMAF p5 | ΔCAMBI (base) |
|---|---|---|---|
| spatial AQ | −0.03 to −0.49 | −0.23 to −0.78 | −0.13 to −0.60 (0.25-1.84) |
| S+T AQ + LA20 | −0.07 to −0.45 | −0.35 to −1.20 | −0.13 to −0.50 |

On High, ΔCAMBI is −0.21 to +0.04 (base 0.05-1.69).

At the shipped CQ the AQ arms are also bigger, by 0.97-1.84x (`size_ratio_at_shipped_cq`
in `nvenc_aq_points.csv`). So turning them on as a default would grow the cache and lower
its VMAF.

## Caveats

- Encode fps was measured while another CPU-heavy scoring job ran, so treat it as ±30 %.
  The neural Standard rung's median ratio was 0.86 for spatial AQ and 0.95 for the full arm.
- VMAF and VMAF NEG are known to undervalue AQ's psychovisual redistribution; a viewer test
  could favour AQ where these do not. CAMBI is the one metric here that sees what AQ
  targets, and its gain is small.
- 1080p, 66-102 frames per clip; the bitrate complaints (#13, VE #103) are about 4K.
