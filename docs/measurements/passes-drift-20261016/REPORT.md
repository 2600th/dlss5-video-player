# Brightness across stacked neural passes (P1.41) — 16 October 2026

**Verdict: stacking does move mean brightness beyond run-to-run noise, but not in one
direction, so there is nothing to correct.** Every repeat rendered byte-identical (noise
floor 0), and from 1 to 4 passes the mean luma moved +0.87, +1.74, −0.01, −1.10 and −0.73
codes (+1.3 %, +2.1 %, 0.0 %, −2.3 %, −0.7 %) on the five clips. NeuralScreen v2.2.0's
finding was a systematic darkening (0.988 → 0.970 over three passes); here three passes
brighten two clips, leave one flat and darken two. What stacking does is compound the
model's own tonal and colour change, roughly linearly per pass - which is what passes
are for - and a global gain term would push as many clips the wrong way as the right
one. No cache-key term is proposed.

Two things the numbers do show:

- **Faces cool down with passes.** On `orig-faces` R falls 2.64 codes (−6 % of its mean)
  from 1 to 4 passes while B rises 0.43; on `orig-game-motion` B falls 1.90. The colour
  change compounds the same way the luma does.
- **Stacking adds flicker.** The worker's own temporal metric (`flicker_added`, 8-bit
  codes) goes from −0.75…+0.06 at one pass to −0.76…+1.39 at four (+0.13 to +1.39 on
  four of five clips; `orig-faces` unchanged).

## Method

`tools/benchmark/m1.py` renders through `run.py`'s worker driver at the **Lossless** rung
(`--cache-quality lossless`, FFV1 10-bit from the P010 capture), so no encoder loss enters
the means. Profiles write exactly what `NeuralAddonOverridesFor(NeuralSettings{})` writes
at 0.28.3 (the eleven keys plus `NRPass{2..4}Intensity/Color` = 1.00), with `NRPasses`
1-4 and `NRChainedHistory=1`, at the shipped governor (`NRNormGovernor=1`) and, as a
check, at 0 (knobs.py's reference). Passes 1 and 4 were rendered twice.

Means are taken over every second frame straight from the Y, Cb and Cr planes (no colour
conversion; 10-bit codes divided by 4), R/G/B from those with BT.709 limited range. The
"distance" rows use the knobs method (rgb24, every fourth frame).

## Environment

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 5090, driver 617.14 (32.0.16.1714, worker preflight) |
| Worker | `NeuralWorker.exe` 0.28.2 (Release build of 2026-10-04), SHA-256 `2244361e…2832`, run from a frozen `benchmark-work/runtime-snapshot` |
| Runtime | ReShade 6.8.0.2155, RenoDX 6.5.3 (add-on 0.2026.917.1432), DLSS-NR 310.8.SF-v2 (310.8.2.0), DLSS 310.9.1, Streamline 2.13.0.0 - byte-identical to `external/runtime` |
| FFmpeg | n9.0.2-14-gebafaee10a-20260929 |
| Clips | `real-film-cuts` (102 frames), `real-game-cuts` (68), `real-game-motion` (76), `orig-faces` (101), `orig-game-motion` (66); 1920x1080. The `orig-*` clips were built locally from the camera-original sources and match `camera-original.digests.json`; nothing from them is committed |

## Results

Mean Y (8-bit codes) and the change from one pass:

| clip | source | 1 pass | 2 passes | 3 passes | 4 passes | 4 vs 1 | ΔR / ΔG / ΔB, 4 vs 1 |
|---|---:|---:|---:|---:|---:|---:|---|
| `real-film-cuts` | 68.67 | 67.81 | +0.30 | +0.59 | +0.87 | +1.28 % | +0.30 / +1.18 / +1.38 |
| `real-game-cuts` | 83.74 | 83.18 | +0.57 | +1.16 | +1.74 | +2.09 % | +1.81 / +2.12 / +1.68 |
| `real-game-motion` | 104.28 | 102.90 | −0.01 | −0.02 | −0.01 | −0.01 % | +0.13 / +0.05 / −1.01 |
| `orig-faces` | 49.23 | 47.78 | −0.42 | −0.79 | −1.10 | −2.29 % | −2.64 / −1.04 / +0.43 |
| `orig-game-motion` | 106.53 | 104.99 | −0.22 | −0.47 | −0.73 | −0.69 % | −0.82 / −0.75 / −1.90 |

Noise: 1 pass and 4 passes each rendered twice - identical frame digests on all five
clips (0.000). Governor 0 renders the same bytes as governor 1 at 1 and 4 passes on all
five (these are real captures; see governor-20260924).

How far each stack moves the picture (knobs method):

| | change to 1 pass | distance from source (1 pass = 100 %) |
|---|---|---|
| 2 passes | 3.67 (2.16-5.71) | 167 % (155-180) |
| 3 passes | 6.91 (4.01-11.02) | 229 % (203-257) |
| 4 passes | 9.84 (5.70-15.87) | 285 % (246-327) |

Cost per render (median over the five clips; model GPU time p50, the worker's peak local
VRAM): 1 pass 3.32 ms / 1184 MiB, 2 passes 6.23 / 1703, 3 passes 9.18 / 2206, 4 passes
12.14 / 2709 - about +2.95 ms and +505 MiB per pass.

Raw numbers: `drift.csv`, `looks.csv`, `timing.csv` beside this report.

## Caveats

- The absolute offset against the source (one pass reads 0.6-1.5 codes darker than the
  source on every clip here) disagrees with the worker's own `luma_shift` metric (sampled
  cells), which reads about 1.2 codes brighter on each clip. The pass-to-pass drift agrees
  between the two instruments within 0.2-0.3 codes (worker: +1.04, +2.03, −0.03, −1.22,
  −0.89 from 1 to 4 passes). Treat the "vs source" column as unverified; the conclusion
  rests on the pass-to-pass column.
- Five clips, three of them screen captures of an earlier render. The direction is
  content-dependent, so another clip can go either way.
- Chained history on only; unchained stacks were not measured.
