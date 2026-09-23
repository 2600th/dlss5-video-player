# Supplied exposure A/B (P2.1) - 23 September 2026, RTX 4080 SUPER

**Verdict: supplying a smoothed, metered exposure does not improve the render, so it ships
off.** On all six clips whose output is real footage or plain text the supplied-exposure
render is **bit-identical** to the AutoExposure render (same decoded-frame digest): on
those the neural pass does not read the exposure at all. On the seven synthetic clips where
it does change pixels, the change is small and goes the wrong way on average: added flicker
+0.0023 codes, added temporal sigma +0.0048, colour distance to the source +0.025 dE, false
motion +0.0007, flips +0.0004. The clip built to show exposure pumping, `flash-exposure`,
moves by 0.0025 codes of flicker (worse) and 0.009 of sigma (better) - noise-scale either
way. The brief's bar was "ON only if flicker/added-sigma improves without a colour/tone
regression"; neither improves.

The implementation stays in the tree, off by default, keyed and tested, because the answer
is a property of this runtime (DLSS-NR 310.8.0 through RenoDX 6.5.3), not of the idea: a
runtime that does consume the exposure would change the verdict, and the A/B below is two
commands to re-run.

## Environment and instruments

| Item | Value |
|---|---|
| GPU / driver | NVIDIA GeForce RTX 4080 SUPER, driver `32.0.16.1047` |
| Runtime | DLSS-NR 310.8.0, ReShade 6.8.0.2155, RenoDX 6.5.3, worker 0.25.0 |
| Tree | branch `wip/w2-quality` (`f2a793a` and later) |
| Renders | `tools/benchmark/run.py`, profiles `x-auto` and `x-supplied` in [`exposure.profile.json`](exposure.profile.json): the shipped state (all ten player keys at their defaults, Standard rung), the second adding `--supplied-exposure 1` |
| Clips | all 13 corpus clips `tools/benchmark/corpus.py` builds on a clean checkout (the `faces` and `orig-*` sources were absent) |
| Metrics | `tools/benchmark/analyze.py --no-ocr --no-faces` (torch is not installed and was not used): `flicker+`, `sigma+`, `dE`, `RGB shift`, `false motion`, `flips+` against the lossless source |

The worker log confirms the supplied arm ran as intended: every job created the feature
with `flags=MVLowRes, supplied exposure` and logged `Supplied exposure armed`; the auto arm
created it with `flags=MVLowRes|AutoExposure`.

## What was supplied

`src/ExposurePolicy.h`: the log-average linear luminance of the temporal guide
generator's analysis grid (the cells the scene-cut test reads), smoothed by a one-pole
low-pass in log units with libplacebo's `peak_smoothing_period` default of 20 frames,
restarted on every history reset the generator declares (a classified cut, a seek, a first
frame), and turned into middle grey over the meter, bounded to four stops either way. The
value is uploaded per frame into a 1x1 `R32_FLOAT` texture bound as
`NVSDK_NGX_Parameter_ExposureTexture`, and the feature is created without
`NVSDK_NGX_DLSS_Feature_Flags_AutoExposure`.

## Results

`auto -> supplied`; one value where the two renders are bit-identical.

| clip | bit-identical | flicker+ | sigma+ | dE | RGB shift (R/G/B) | false motion | flips+ |
|---|---|---:|---:|---:|---:|---:|---:|
| cuts-motion | no | -2.8285 -> -2.7983 | -4.6186 -> -4.5356 | 8.219 -> 8.270 | +0.78/-1.67/+1.26 -> +0.57/-1.75/+1.11 | 0.1401 -> 0.1438 | +0.0136 -> +0.0134 |
| cuts-similar | no | -0.7059 -> -0.7046 | -0.5130 -> -0.5144 | 5.699 -> 5.707 | +5.10/-6.50/+2.37 -> +5.27/-6.47/+2.39 | 0.1012 -> 0.1028 | +0.0672 -> +0.0700 |
| dissolve | no | -0.8520 -> -0.8558 | -2.6831 -> -2.6574 | 9.579 -> 9.598 | +4.66/-7.70/+5.16 -> +4.73/-7.71/+5.20 | 0.0663 -> 0.0662 | +0.0388 -> +0.0394 |
| fine-detail | no | -4.8217 -> -4.8230 | -6.6718 -> -6.6982 | 11.204 -> 11.262 | +4.17/-1.09/+11.18 -> +4.24/-1.03/+11.38 | 0.3362 -> 0.3379 | -0.0476 -> -0.0463 |
| flash-exposure | no | -0.8376 -> -0.8351 | +0.4713 -> +0.4621 | 8.749 -> 8.721 | +2.11/-4.04/+9.29 -> +2.03/-4.02/+9.25 | 0.1113 -> 0.1118 | +0.0409 -> +0.0404 |
| highlights-gradients | yes | -0.0977 | -1.6604 | 3.196 | +0.62/+0.08/-0.54 | 0.0327 | +0.0123 |
| pan-fast | no | -0.2562 -> -0.2653 | +0.5636 -> +0.5744 | 7.236 -> 7.371 | +3.43/-5.54/+6.20 -> +3.64/-5.76/+6.25 | 0.1971 -> 0.1939 | +0.0110 -> +0.0100 |
| real-dissolve | yes | -0.0593 | +0.8227 | 3.458 | -1.01/-1.13/-1.46 | 0.1153 | +0.0023 |
| real-film-cuts | yes | -0.0196 | +0.0397 | 2.153 | -1.26/-0.80/-0.32 | 0.0110 | -0.0136 |
| real-game-cuts | yes | -0.3603 | -0.3232 | 3.103 | -0.45/-0.64/-0.61 | 0.1336 | -0.0077 |
| real-game-motion | yes | +0.2728 | +1.1911 | 3.156 | -1.36/-1.86/-2.19 | 0.0632 | -0.0142 |
| text-subtitles | yes | +0.0130 | -0.1208 | 2.039 | -6.43/+1.15/+0.26 | 0.0034 | +0.0021 |
| zoom-fast | no | -0.7802 -> -0.7696 | -0.2234 -> -0.2435 | 8.571 -> 8.654 | +4.99/-5.77/+7.57 -> +5.23/-5.89/+7.59 | 0.1096 -> 0.1139 | +0.0257 -> +0.0274 |

Means over the 13 clips, auto -> supplied (delta): flicker+ -0.8718 -> -0.8695 (+0.0023),
sigma+ -1.0558 -> -1.0510 (+0.0048), dE 5.874 -> 5.899 (+0.025), false motion 0.1093 ->
0.1100 (+0.0007), flips+ 0.0101 -> 0.0104 (+0.0004). Lower is better for every one.

How far the supplied arm moves the picture where it moves it at all (VMAF and PSNR-Y of
supplied against auto; mean and frame-to-frame spread of the difference in frame-mean luma,
8-bit codes): `fine-detail` 88.5 / 44.0 dB / +0.025 +- 0.098, `cuts-motion` 91.6 / 48.2 dB /
+0.012 +- 0.193, `dissolve` 97.6 / 56.9 dB, and 99.6-99.9 on the other four. There is no
tone shift to speak of - the largest mean luma difference is 0.1 of a code (`pan-fast`) -
so no brightness pumping that the supplied value could have removed was there to remove.

## Reproducing

```
python tools/benchmark/corpus.py
python tools/benchmark/run.py --profile-file docs/measurements/exposure-ab-20260923/exposure.profile.json \
    --profiles x-auto x-supplied
python tools/benchmark/analyze.py --no-ocr --no-faces
```

`run.py` renders every clip in the manifest when `--clips` is omitted; `analyze.py` writes
each run's `metrics.json`, from which the table above is read.
