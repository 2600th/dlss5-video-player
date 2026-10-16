# A sharper yardstick before the temporal work (P1.42) — 16 October 2026

**Verdict.** `analyze.py` can now score a render with the VMAF v1 models, with
ColorVideoVDP, and with a frozen-output guard beside every stability number. All
three are opt-in; without the flags the analysis is byte-identical to before.

1. **The guard catches what the stability numbers reward.** A copy frozen after
   frame 10 reads as the *calmest* render in the table: `flicker+` −5.64 and
   `sigma+` −15.3 on `real-game-motion`, `false mv` 0. Its `lost` is 0.90 and
   `lost mv` 0.89, against 0.09–0.19 for every real render. A temporally smoothed
   copy (EMA, α 0.25) reads smoother than the source (`flicker+` −4.14) and has
   `lost` 0.74; a copy held on twos has `lost` 0.49–0.62. The source scored
   against itself is 0 on every term.
2. **The flicker side still works.** Gaussian luma noise (σ 2 levels, new every
   frame) raises `flicker+` by 1.1–1.8 and `added` to 1.6–1.9, and leaves `lost`
   at 0.07.
3. **The earlier "distance from the source" conclusions hold under both
   perceptual scores** ([knobs-upper-20261003](../knobs-upper-20261003/REPORT.md)),
   on the three clips re-scored. Intensity 2.00 matches the default render on
   every new number. Intensity 0.00 and Gentle score closer to the source than the
   default. Local tone 2.00, Natural, Cinematic and two passes score further.
   Local structure 2.00 costs only a little.
4. **VMAF v0.6.1 gets two of those the wrong way round.** It scores Local tone
   2.00 and "all at 2.00" *higher* than the default (93.0–94.0 against 89.0 on
   `real-film-cuts`). v1, ColorVideoVDP and the byte-level distance all score them
   as further from the source. Do not use v0.6.1 alone to judge enhancement.
5. **Cinematic's calm is partly lost change.** On `real-game-cuts`, Style
   Cinematic has the lowest `flicker+` of any real render (−1.71) and the highest
   `lost` (0.186, against 0.104 at the default); Natural shows the same, less.
   Before this guard that pattern would have read as a gain in stability.

## Method

`tools/benchmark/analyze.py`, with three opt-in flags (`--yardstick` sets all
three):

```
python tools/benchmark/quality.py fetch                  # once: the pinned VMAF v1 models
python tools/benchmark/analyze.py --yardstick            # every run, all three
python tools/benchmark/analyze.py --match "real-*__knob-*" --vmaf-v1 --cvvdp   # a subset
```

- **`--lost-change`** splits `flicker+` (mean |dY_out| − mean |dY_src|) into
  halves that cannot cancel, per consecutive pair, on the 8-bit luma `flicker+`
  already uses:
  - `added` is the mean over all pixels of max(|dY_out| − |dY_src|, 0);
  - `lost` is Σ max(|dY_src| − |dY_out|, 0) / Σ |dY_src|, over pixels where the
    source moved by more than 2 levels, pooled over pairs as a ratio of sums.
    0 means every change was carried; 1 means frozen.

  Manifest `cuts` and `soft_cuts` spans are excluded, and so are held source
  pairs (under 0.1 % of pixels moving), which drops 17 of `real-film-cuts`' 97
  in-shot pairs, so no ratio runs through a near-zero denominator. `lost mv`
  mirrors `false mv` on the guide generator's own grid and tolerance: of the
  cells the source moved, the fraction the output held static. Neither term is
  motion-compensated; both compare co-located change, which a render that follows
  the source's motion reproduces.
- **`--vmaf-v1`** runs one FFmpeg pass with the v1 model and v0.6.1 side by side.
  The model is `vmaf_v1.0.16_3d0h` up to 1080 lines, `_1d5h_2160` above, and the
  `_hfr` variant above 45 fps. Both inputs are converted to 10-bit 4:2:0, as
  `models_v1.md` asks. Recorded: the v1 mean, p5 and minimum; `CAMBI+`, the
  model's built-in CAMBI on the output minus CAMBI on the source (CAMBI is
  no-reference, so the source gets its own pass); and `chroma`, the model's
  `speed_chroma_uv` feature. The models come from Netflix/vmaf at the v3.2.1 tag,
  fetched into `build-upscaling/benchmark-work/vmaf-models/` and checked against
  the SHA-256 values pinned in `quality.py`; nothing is committed.
- **`--cvvdp`** runs ColorVideoVDP 0.5.7 on the GPU, output against source, with
  the `standard_fhd` display (24-inch 1080p, 200 cd/m², sRGB SDR) up to 1080 lines
  and `standard_4k` above, at the clip's true fps. Both files are decoded in
  lockstep and streamed into the metric, so memory does not grow with clip
  length; GPU memory is capped at 8 GB so a concurrent render keeps its VRAM.
  Reported: the overall JOD, plus the metric's own pooling applied to one channel
  at a time with the others zeroed — `transient` (visible temporal difference),
  `rg` and `yv` (the two chroma channels).

**Validation set.** Ten synthetic outputs, two clips × five variants, built from
the lossless corpus and written as FFV1 RGB: `real-game-motion` (one continuous
shot) and `real-film-cuts` (four cuts, 17 held pairs), each as identity (the
source), hold2 (each frame held for two), freeze (frame 10 held to the end), ema
(out = 0.25·src + 0.75·out_prev) and noise (independent N(0, 2) luma noise per
frame, seed 1042). Then the 3 October knob set re-scored: twelve settings on the
three `real-*` clips, 36 existing renders, nothing re-rendered, read from copies
of their `result.json` so nothing was written into `benchmark-work/runs`.

**Byte-identity of the default.** The original `analyze.py` scored all 46 runs
with no flags; the new one re-scored the 17 `real-game-motion` runs with
`--force` and all 17 `metrics.json` files were byte-identical. Run again over all
46, `report.md` and `analysis.json` were byte-identical to the original's.

## Environment

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 5090, driver 617.14 (shared with a concurrent render job) |
| FFmpeg | `external/ffmpeg`, BtbN n9.0.2-14-gebafaee10a (2026-09-29); libvmaf `86da14d` (v3.2.1 + 8 commits) loads the v1 JSON models and their `cambi`, `speed_chroma`, `adm3`/`aim` and `motion3` extractors unmodified |
| VMAF models | Netflix/vmaf `f85a853692a8c730d0270cd733c8bb30b5b93b7c` (tag v3.2.1), `model/vmaf_v1.0.16{,_hfr}/*.json`, BSD-2-Clause-Patent; SHA-256 pinned in `tools/benchmark/quality.py` |
| ColorVideoVDP | `cvvdp` 0.5.7 (pip; MIT), PyTorch 2.11.0+cu128, Python 3.12.10 |
| Renders | the 3 October knob renders: ReShade 6.8.0.2155, RenoDX 6.5.3, DLSS-NR 310.8.SF-v2, NVENC HEVC 8-bit 4:2:0 output |
| Cost | about 30 s per 100-frame 1080p run for VMAF v1 and ColorVideoVDP together, beside the existing decode |

## Results

### Synthetic: the guard fires where it should

| clip | variant | flicker+ | sigma+ | added | lost | false mv | lost mv | VMAF v1 (p5) | v0.6.1 | JOD | JOD trans |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `real-game-motion` | identity | 0.000 | 0.000 | 0.000 | 0.000 | 0.0000 | 0.0000 | 99.91 (99.56) | 99.73 | 10.00 | 10.00 |
| `real-game-motion` | noise | 1.142 | 0.133 | 1.595 | 0.066 | 0.2082 | 0.0912 | 98.60 (97.27) | 99.16 | 9.85 | 9.90 |
| `real-game-motion` | hold2 | −2.050 | −0.433 | 1.791 | 0.615 | 0.1000 | 0.5558 | 64.72 (0.00) | 68.73 | 7.01 | 7.77 |
| `real-game-motion` | ema | −4.140 | −6.113 | 0.400 | 0.739 | 0.0722 | 0.4421 | 29.30 (0.00) | 20.69 | 6.01 | 6.54 |
| `real-game-motion` | freeze | −5.639 | −15.259 | 0.000 | 0.903 | 0.0000 | 0.8885 | 15.58 (0.00) | 17.89 | 3.03 | 5.69 |
| `real-film-cuts` | identity | 0.000 | 0.000 | 0.000 | 0.000 | 0.0000 | 0.0000 | 96.23 (91.89) | 97.51 | 10.00 | 10.00 |
| `real-film-cuts` | noise | 1.817 | 1.030 | 1.894 | 0.074 | 0.1563 | 0.1102 | 95.38 (93.58) | 96.08 | 9.86 | 9.91 |
| `real-film-cuts` | hold2 | 1.323 | 5.616 | 1.631 | 0.494 | 0.0611 | 0.4964 | 80.56 (0.00) | 84.48 | 1.77 | 1.79 |
| `real-film-cuts` | freeze | −0.936 | −3.077 | 0.000 | 0.574 | 0.0000 | 0.5870 | 11.01 (0.00) | 11.51 | −4.68 | −2.92 |

On `real-game-motion` every stability number says the frozen copy is the best
render, and only the guard and the full-reference scores say it is the worst. On
`real-film-cuts` the freeze reads 0.57 rather than 1 because the first ten pairs
carry much of the clip's in-shot motion; the frozen frame also crosses the cuts,
which is why CVVDP drops below zero there. Noise at σ 2 is near the visibility
floor (JOD 9.85, `transient` 9.90) while `flicker+` and `added` flag it clearly:
the pixel terms are the more sensitive flicker detector, and the JOD says how
much of it a viewer would see.

The identity row of `real-film-cuts` scores 96.2 in VMAF and 10.00 in JOD. That
floor is the synthetic carrier (RGB FFV1, converted back to YUV for libvmaf), not
the metric: on `real-game-motion` the same path scores 99.9. The pixel terms read
RGB directly and are exactly 0.

### Re-scoring the 3 October knob set

Values per clip as `real-film-cuts` / `real-game-cuts` / `real-game-motion`.
Intensity 2.00 is byte-identical to the default render, and every new number
agrees to the last digit.

| Setting | VMAF v1 | v0.6.1 | JOD | `lost` | `flicker+` | Earlier finding | Holds? |
|---|---|---|---|---|---|---|---|
| Default (shipped) | 75.6 / 80.4 / 80.9 | 89.0 / 87.8 / 90.6 | 9.00 / 8.87 / 8.96 | 0.125 / 0.104 / 0.108 | −0.05 / −0.38 / 0.26 | — | — |
| Intensity 2.00 | = default | = default | = default | = default | = default | inert, = 1.00 | yes |
| Intensity 0.00 | 83.5 / 87.3 / 87.1 | 89.0 / 89.8 / 85.6 | 9.54 / 9.39 / 9.28 | 0.118 / 0.087 / 0.123 | −0.13 / −0.55 / −0.37 | closest to the source (47 %) | yes |
| Intensity 0.40 | 82.8 / 86.1 / 87.5 | 90.1 / 89.9 / 89.1 | 9.40 / 9.31 / 9.22 | 0.117 / 0.091 / 0.111 | −0.11 / −0.50 / −0.14 | closer (60 %) | yes; v1 puts it level with 0.00 on `real-game-motion` |
| Preset Gentle | 82.8 / 86.5 / 87.7 | 90.8 / 89.9 / 88.8 | 9.35 / 9.32 / 9.24 | 0.116 / 0.090 / 0.111 | −0.11 / −0.54 / −0.16 | closer (61 %) | yes |
| Local structure 2.00 | 73.7 / 79.1 / 78.2 | 90.3 / 88.2 / 91.5 | 8.93 / 8.83 / 8.81 | 0.120 / 0.102 / 0.106 | −0.01 / −0.21 / 0.38 | no further (99 %) | yes; it costs a little (≤ 0.15 JOD, ≤ 2.7 VMAF) |
| Local tone 2.00 | 74.2 / 79.6 / 78.6 | 93.0 / 89.5 / 93.4 | 8.72 / 8.61 / 8.93 | 0.123 / 0.105 / 0.105 | 0.00 / −0.25 / 0.42 | further (118 %) | yes in v1 and JOD; **v0.6.1 says closer** |
| Intensity + tone + structure at 2.00 | 72.3 / 78.1 / 75.9 | 94.0 / 89.7 / 94.0 | 8.68 / 8.53 / 8.78 | 0.120 / 0.103 / 0.104 | 0.05 / −0.06 / 0.53 | further (122 %) | yes in v1 and JOD; **v0.6.1 says closer** |
| Style Natural | 63.7 / 70.5 / 69.8 | 82.9 / 79.1 / 74.2 | 7.45 / 8.28 / 8.57 | 0.151 / 0.138 / 0.146 | −0.03 / −0.68 / 0.00 | much further (168 %) | yes |
| Style Cinematic | 65.7 / 66.1 / 73.7 | 72.8 / 68.3 / 79.6 | 8.48 / 8.07 / 8.59 | 0.182 / 0.186 / 0.138 | −0.11 / −1.71 / 0.00 | further (134 %) | yes |
| 2 passes | 61.0 / 69.2 / 65.7 | 84.6 / 84.2 / 88.8 | 8.19 / 8.04 / 8.05 | 0.141 / 0.120 / 0.111 | 0.04 / 0.07 / 0.97 | much further (169 %) | yes |

The 3 October report averaged six clips. Here the order among Natural, Cinematic
and two passes changes from clip to clip, so that ordering is not a finding on
three clips; that all three sit at least 0.3 JOD and 7 VMAF points further from
the source than the default is.

The `lost` floor of a real render is about 0.09–0.13, even at Intensity 0.00:
the NVENC carrier drops low-amplitude temporal detail, and the model's residual
change adds to it. Judge `lost` against that floor and the shipped state, not
against zero. `lost mv` has the same floor (0.11–0.21) for the same reason.
`chroma` rises with every setting that moves colour (3.4 at Intensity 0.00 to
12.9 at two passes), and the chroma-channel JODs stay at 9.5 or above
everywhere: the colour change on these clips is measurable but small next to the
achromatic change. `CAMBI+` stays within ±0.7 on every real render, so none of
these settings adds visible banding.

## Limitations

- **Not motion-compensated.** `lost` and `added` compare co-located change, so a
  render that shifts content by a pixel scores it as change added and change
  lost. The guide grid's 12×12 cells absorb most of that, which is why `lost mv`
  is reported beside `lost`.
- **Contrast scales `lost`.** A setting that lowers local contrast moves the same
  motion through fewer levels, which raises `lost` a little without freezing
  anything; part of Cinematic's 0.18 is that. A freeze reads 0.5–0.9; a relight
  reads tenths.
- **CAMBI+ depends on the carrier.** Both sides go to 10 bits through the same
  conversion, so 8-bit renders against the 8-bit corpus compare fairly, as here;
  the synthetic RGB carrier reads −1 to −8 on identical content. Compare CAMBI+
  only between outputs that share an encode path.
- **One display model per resolution.** JOD depends on the display and viewing
  distance; `standard_fhd` and `standard_4k` are fixed. Compare JODs only within
  one display model.
- **The 4K and HFR selection is untested on footage.** The corpus is 1080p30. The
  model choice follows `models_v1.md`, and the HFR and 2160p files load in
  libvmaf, but no 2160p or 60 fps clip was scored for this report.
