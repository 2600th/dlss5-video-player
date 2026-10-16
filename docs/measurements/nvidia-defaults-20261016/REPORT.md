# NVIDIA's RTX Remix NR defaults as a preset (P2.58) — 16 October 2026

**Verdict: a distinct look, at about the Gentle preset's strength.** NVIDIA's RTX Remix
defaults (Intensity 1.00, Local structure 0.70, Local tone 0.30, Skin 0.50, automatic
mask on) move the picture 2.46 codes from Standard (1.50-3.49 per clip) and land at
**67 %** of Standard's distance from the source (59-76 %). Gentle lands at 64 %
(58-72 %), 1.01 codes from Remix. Most of Remix is its tone term: Local tone 0.30 alone
is 0.64 from Remix and lands at 73 %. It is a measurably different mix from Gentle
(Remix keeps Intensity at 1.00 and takes it out of tone; Gentle halves Intensity and
structure), but on strength alone it would be a second Gentle. Worth a named preset only
as "NVIDIA's reference look"; Standard is the owner's decision and is not touched here.

**The values reach NGX unchanged.** RenoDX 6.5.3 does not log tone, structure or skin; its
disassembly (knobs-upper-20261003) clamps them to 0-2 / −1…1 and passes them through, and
0.30 / 0.70 / 0.50 are inside that. RenoDX 8.5.0-rc10 logs what it sends, and for this
profile it logged `local_tone=0.3 structure=0.7 skin=0.5 ... sent_structure=0.7
sent_skin=0.5`.

## Method

The knobs method of knobs-upper-20261003 on Lossless renders (`m1.py looks`): mean
|candidate − reference| over every fourth frame decoded to rgb24, and mean |candidate −
source| as a share of mean |reference − source|. Profiles are the player's eleven keys
plus `NRPass{2..4}Intensity/Color`, governor 1 (governor 0 gave identical numbers). Remix
writes `NRIntensity=1.000000 NRLocalStructure=0.700000 NRLocalTone=0.300000
NRSkinStructure=0.500000`; Gentle is `NeuralPresets.h`'s (0.50 / tone 1.00 / structure
0.50 / skin 0.00).

## Environment

As [passes-drift-20261016](../passes-drift-20261016/REPORT.md): RTX 5090, driver 617.14,
worker 0.28.2, ReShade 6.8.0.2155, RenoDX 6.5.3, DLSS-NR 310.8.SF-v2; five 1080p clips.

## Results

Per clip, change to the reference / distance from source as a share of the reference's
(`real-film-cuts`, `real-game-cuts`, `real-game-motion`, `orig-faces`, `orig-game-motion`):

| row | per clip | mean |
|---|---|---|
| Remix vs Standard | 1.50/76 % · 2.51/67 % · 2.93/61 % · 1.86/72 % · 3.49/59 % | 2.46 / 67 % |
| Gentle vs Standard | 1.47/72 % · 2.56/62 % · 2.86/61 % · 1.92/67 % · 3.50/58 % | 2.46 / 64 % |
| Local tone 0.30 alone vs Standard | 1.34/83 % · 2.24/73 % · 2.64/67 % · 1.68/79 % · 3.11/66 % | 2.20 / 73 % |
| Remix vs Gentle | 0.67/107 % · 0.94/107 % · 1.20/99 % · 0.81/108 % · 1.45/102 % | 1.01 / 104 % |
| Remix vs tone 0.30 alone | 0.45 · 0.65 · 0.69 · 0.55 · 0.87 | 0.64 |
| Standard rendered again | 0 on all five (identical digests) | 0 |
| Remix on RenoDX 8.5 vs on 6.5.3 | 0.13 · 0.24 · 0.26 · 0.22 · 0.28 | 0.23 |

The model's own change, mean |Standard − source|: 2.92, 4.66, 5.63, 4.02, 6.76.
No setting costs render time: model GPU time 3.32 ms for Standard and Remix alike.

Raw numbers: `looks.csv` beside this report.

## Caveats

- How the two looks differ to a viewer (Remix keeps colour intensity, Gentle keeps local
  tone) is not something these numbers say; a blind A/B would.
- Face-region concentration was not scored; Remix's skin 0.50 is explicit where Gentle's
  is 0.00.
