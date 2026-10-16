# RenoDX 8.5.0-rc10 against the pinned 6.5.3 (P3.14, RenoDX half) — 16 October 2026

**Verdict: adopt later, with three config changes; not blocked.** 8.5.0-rc10 loads on the
pinned ReShade 6.8.0.2155 (API 18) and DLSS-NR 310.8.SF-v2, renders every clip, repeats
byte-identically, and costs the same render time. With the player's config written
unchanged it is **not** the same picture:

1. **Detail stability is on by default.** `NRDetailStability=0` is Auto, and Auto engages
   in the player's after-upscale path: a temporal filter on the model's edit along the
   motion vectors. It moves the render 0.33 codes from 6.5.3 (0.19-0.39), at 98-100 % of
   its distance from the source, and changes the worker's `flicker_added` by −0.01 to
   −0.04. With `NRDetailStability=1` (off), 8.5 lands within 0.10 code of 6.5.3:
   byte-identical on `orig-game-motion`, 0.000-0.016 on three clips, 0.10 on
   `orig-faces`.
2. **Independent skin is on by default and caps Local structure at 1.** With
   `NRSkinIndependent=1`, 8.5 logs `structure=1.5 ... sent_structure=1 sent_skin=1`. The
   Strong preset's structure 1.50 is silently sent as 1.00 (it renders 0.10 from
   Standard, against 0.52 on 6.5.3), and the player's skin "Same as Local structure" (−1)
   is sent as skin 1.00. With `NRSkinIndependent=0` it sends `structure=1.5 skin=-1`, as
   6.5.3 does, and lands 0.12 from 6.5.3's render.
3. **It migrates the player's config.** The player stamps `ConfigVersion=6`; 8.5 migrates
   it to 7, writes `ReShade.ini.renodx-dlss5-pre-v7-migration-*.bak`, and fills every new
   key with its default. `NRNormGovernor=1` stays Slew after the migration (logged
   `governor=slew`). A player shipping 8.5 should stamp 7.

So moving to 8.5 means `kNeuralConfigVersion` "7", `NRDetailStability=1` and
`NRSkinIndependent=0` written explicitly (or a measured decision to keep 8.5's
defaults, which re-keys every cache entry). Control mask: not exposed
([control-mask-20261016](../control-mask-20261016/REPORT.md)).

## What changed in the `[RenoDX.DLSS5]` contract

`config-contract.txt` lists every key. In short: 43 `NR*` names in 6.5.3 and 126 in 8.5.
None the player writes was removed. New groups:

- per-pass controls for passes 2-4 (`NRPass{n}LocalTone/Structure/Skin/Style/AutoMask/UICorrection/Transfer/FollowPass1`)
- a post-model look stage (`NRLook*`, "Adjust NR's look", off by default: `NRLookMode=0`)
- `NRDetailStability`, `NRSkinIndependent`, `NRStyleHighlightGuard`, `NRNeuralFloorGuard`, `NRNormCutAware`
- diagnostics (`NRCostMeter`, `NREditTrace`, `NRScreenshot*`), present-path and Vulkan/D3D11 keys

On the NGX side the `DLSSNR.*` keys are identical, except `DLSSNR.GlobalToneStrength`,
which 8.5 no longer sets (6.5.3's Global tone measured inert). Natural and Cinematic
styles are still passed through `DLSSNR.Style`; 8.5 adds an optional highlight guard
for them. 8.5 also writes a `RenoDX-DLSS5-crash.log` session log beside the worker
(not a crash; harmless to the unlocked-module check).

## Method

`m1.py render` clones the runtime snapshot per profile and replaces only the clone's
`renodx-dlss5.addon64` with 8.5's. `external/runtime` and the pinned files are untouched.
Profiles: the player's keys as written (governor 1, and 0 as a check),
`NRDetailStability=1`, the Remix defaults, and Local structure 1.50 with and without
`NRSkinIndependent=0`. All at the Lossless rung, repeated twice. Pace was measured in a
separate pass that interleaved 6.5.3, 8.5 and 8.5-with-stability-off on each clip, twice,
on an otherwise idle GPU. VRAM is the worker's own peak local budget and NVML device
memory over the first (pre-load) sample.

## Environment

| Item | Value |
|---|---|
| GPU / driver | RTX 5090, 617.14 |
| Add-on under test | `renodx-dlss5_8.5.0-rc10.zip` from RankFTW/rhi-repo (zip SHA-256 `a745040cb2e93e3a1500a6c5a322972acc4c5c667108fcea31c748285d6364a9`, matches GitHub's digest); `renodx-dlss5.addon64` SHA-256 `dcd93881e976ad033d83c2bb01f4bc3e4ddc59c15fe0dd4ca165bc5fc7d1ac68`, version 0.2026.926.210, "v8.5.0-rc10 (build Sep 26 2026 16:19:46)" |
| Everything else | as passes-drift-20261016 (worker 0.28.2, ReShade 6.8.0.2155, DLSS-NR 310.8.SF-v2, DLSS 310.9.1, Streamline 2.13) |

## Results

| row | change (mean, range) | distance from source | identical |
|---|---|---|---|
| 8.5 as written vs 6.5.3 | 0.33 (0.19-0.39) | 99 % (98-100) | 0/5 |
| 8.5 rendered again | 0 | 100 % | 5/5 |
| 8.5, Detail stability off, vs 6.5.3 | 0.02 (0.00-0.10) | 100 % | 1/5 |
| 8.5 Detail stability Auto vs Off | 0.31 (0.17-0.39) | 99 % | 0/5 |
| 8.5 Remix vs 6.5.3 Remix | 0.23 (0.13-0.28) | 99 % | 0/5 |
| 8.5 governor 0 vs 6.5.3 governor 0 | 0.33 (same as governor 1) | 99 % | 0/5 |
| `orig-faces`, structure 1.50: 8.5 default vs 6.5.3 | 0.61 | - | - |
| `orig-faces`, structure 1.50: 8.5 `NRSkinIndependent=0` vs 6.5.3 | 0.12 | - | - |

Pace and memory (interleaved runs, 5 clips × 2, medians):

| | model GPU ms p50 | processing fps | worker peak local VRAM | NVML device delta |
|---|---|---|---|---|
| 6.5.3 | 3.325 | 125.6 | 1184 MiB | 1202 MiB |
| 8.5 as written | 3.404 (+2.4 %) | 126.9 | 1232 MiB | 1251 MiB |
| 8.5, Detail stability off | 3.321 | 124.1 | 1168 MiB | 1186 MiB |

Processing fps at the Lossless rung is bounded by the CPU FFV1 encode, so it shows no
pace difference; the model's GPU time is the comparable number. First frame 5.6-5.8 s
for all three.

Raw numbers: `looks.csv`, `timing.csv` and `config-contract.txt` beside this report.

## Caveats

- One release candidate; the strings mention rc11 behaviour, so a final 8.5 may differ.
- Detail stability's effect on flicker was read from the worker's own metric only;
  whether it is worth keeping on is a P1.42-yardstick question.
- NVIDIA's October model is still unobtainable; this is the RenoDX half only.
