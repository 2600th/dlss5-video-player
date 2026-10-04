# What the Neural settings controls do on DLSS-NR 310.8 (issue #15) — 3 October 2026

**Verdict.** The dialog's controls are live with three exceptions, and two of its
labels describe something the runtime does not do.

1. **Intensity above 1.00 does nothing.** 1.30, 1.50 and 2.00 render byte-identical
   to 1.00 on all six clips, at Style Default and at Cinematic. The add-on passes the
   value through (its log reads `intensity=2.000000`); the cap is in NVIDIA's
   runtime. Half of the slider is inert.
2. **Intensity is a second Color strength.** Intensity 0.00 renders byte-identical
   to Color strength 0.00 on all six clips; at 0.50 the two land within 0.45-0.85 of
   each other (mean |a-b|) against a change of 1.4-3.1 from the default; Intensity
   0.50 with Color strength 0.50 lands where Color strength 0.25 does. Both fade the
   model's change back towards the source, and neither reaches it: at 0.00 the
   picture is still about half as far from the source as the default render (47 %).
   The tooltip's "0.00 leaves the decoded frame alone" is wrong.
3. **Skin structure's "Off" means "same as Local structure".** At Local structure
   0.50, skin "Off" (−1) renders byte-identical to skin 0.50 on all six clips, and
   skin 1.00 differs from both. The 24 September table found every negative value
   equal to the default only because structure was 1.00 there.
4. **The Strong preset gets nothing from its Intensity 1.50.** It renders
   byte-identical to the same preset at Intensity 1.00 on all six clips, and lands 6 %
   further from the source than the default. Two passes land 69 % further, Style
   Natural 68 %, Cinematic 34 %.
5. **Local tone and Local structure above 1.00 are live.** Tone at 2.00 moves the
   picture 18 % further from the source; structure at 2.00 changes the frame by about
   as much as structure 0.50 does, but in a direction that does not move it further
   from the source (99 %). 1.50 and 2.00 differ from each other on every clip.

## Method

`tools/benchmark/knobs.py` renders one change at a time from the player's shipped
settings (`NRNormGovernor=0`, as in every byte-for-byte table since
[knobs-653-20260924](../knobs-653-20260924/REPORT.md); the reference rendered twice
differs in 0 bytes on every clip). The rows added for this report are the dialog's
upper halves and 0.00 ends, the Intensity × Color strength products, skin at
structure 0.50, Intensity under Natural, Cinematic and two passes, and the Strong
and Gentle presets as `NeuralPresets.h` writes them.

Each output is compared, every fourth frame as decoded rgb24, with two pictures:

- **the default render** — mean |row − default|, in 8-bit codes;
- **the source** — mean |row − source| as a share of mean |default − source|. 100 %
  is as far from the source as the default render; below 100 % is closer to the
  source, above is further. This is the "is it weaker or stronger" a viewer judges.

"Byte-identical" means the SHA-256 of the sampled decoded frames is equal.

Clips: the three `real-*` captures (screen recordings of an earlier render, see
[BENCHMARK.md](../../BENCHMARK.md)) and three camera-original clips cut from the
publishers' releases by `tools/benchmark/corpus.py`: `orig-faces`,
`orig-film-cuts-a` and `orig-game-motion`, all three matching the digests in
`tools/benchmark/camera-original.digests.json`. The camera-original sources are
copyrighted and were downloaded locally for this measurement only; nothing from them
is committed.

## Environment

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 5090, driver 32.0.16.1714 (worker preflight) |
| Runtime | ReShade 6.8.0.2155, RenoDX 6.5.3 (add-on 0.2026.917.1432), DLSS-NR 310.8.SF-v2 (ShortFuse's community build of the leaked 310.8.0; file version 310.8.2.0) — all byte-identical to the files in the 0.28.1 package |
| Worker | `build-upscaling/Release/neural-runtime/NeuralWorker.exe`, identical to 0.28.1's |
| Renders | 222, all `ok` |

## Results

Mean over the six clips, with the range across them:

| Setting (one change from the shipped state) | change to the default render | distance from the source (default = 100 %) |
|---|---|---|
| Intensity 0.00 | 4.03 (2.45-6.01) | 47 % (33-60) |
| Intensity 0.40 | 2.54 (1.60-3.69) | 60 % (53-66) |
| Intensity 0.50 | 2.17 (1.39-3.13) | 66 % (60-71) |
| Intensity 1.30 | 0.00 | 100 % |
| Intensity 1.50 | 0.00 | 100 % |
| Intensity 2.00 | 0.00 | 100 % |
| Color strength 0.00 | 4.03 (2.45-6.01) | 47 % (33-60) |
| Color strength 0.20 | 3.29 (2.04-4.85) | 51 % (40-61) |
| Color strength 0.50 | 2.17 (1.39-3.12) | 66 % (60-71) |
| Local tone 0.00 | 2.96 (1.92-4.27) | 64 % (53-75) |
| Local tone 0.30 | 2.28 (1.48-3.24) | 71 % (64-78) |
| Local tone 1.50 | 1.37 (1.03-1.64) | 113 % (109-115) |
| Local tone 2.00 | 2.15 (1.78-2.68) | 118 % (108-130) |
| Local structure 0.00 | 2.09 (1.31-2.88) | 77 % (72-84) |
| Local structure 0.30 | 1.70 (1.09-2.27) | 83 % (78-89) |
| Local structure 0.50 | 1.34 (0.89-1.74) | 88 % (84-92) |
| Local structure 1.50 | 0.98 (0.75-1.23) | 100 % (98-102) |
| Local structure 2.00 | 1.37 (1.07-1.76) | 99 % (94-102) |
| Intensity + tone + structure at 2.00 | 2.68 (1.95-3.40) | 122 % (106-142) |
| Style Natural | 6.09 (3.73-8.24) | 168 % (117-229) |
| Style Cinematic | 5.00 (3.51-5.84) | 134 % (100-175) |
| 2 passes | 3.66 (2.28-5.77) | 169 % (156-183) |
| Preset Strong (1.50 / 1.25 / 1.50) | 1.18 (0.95-1.41) | 106 % (103-110) |
| Preset Gentle (0.50 / 1.00 / 0.50, skin 0.00) | 2.54 (1.60-3.61) | 61 % (55-67) |
| Strong redefined: Local tone 2.00, Local structure 1.50 | 2.51 (1.92-3.30) | 122 % (108-141) |

The model's own change, mean |default − source|: `real-film-cuts` 2.79,
`real-game-cuts` 4.61, `real-game-motion` 5.42, `orig-faces` 3.90,
`orig-film-cuts-a` 4.03, `orig-game-motion` 6.58.

The claims, pair by pair:

| Claim | Result on each of the six clips |
|---|---|
| Intensity 2.00 = 1.00 | byte-identical on all six |
| Intensity 1.50 = 1.00 | byte-identical on all six |
| Intensity 0.00 = Color strength 0.00 | byte-identical on all six |
| Intensity 0.50 vs Color strength 0.50 | differ: 0.45, 0.77, 0.81, 0.60, 0.57, 0.85 |
| Skin "Off" = skin 0.50, at structure 0.50 | byte-identical on all six |
| Skin "Off" vs skin 1.00, at structure 0.50 | differ: 0.60, 0.87, 0.88, 0.72, 0.71, 0.91 |
| Cinematic: Intensity 2.00 = 1.00 | byte-identical on all six |
| Cinematic: Intensity 0.40 vs 1.00 | differ: 2.82, 4.34, 3.34, 2.63, 3.34, 3.83 |
| Natural: Intensity 0.40 vs 1.00 | differ: 3.68, 4.20, 5.83, 2.47, 2.87, 6.65 |
| 2 passes: Intensity 0.40 vs 1.00 | differ: 1.47, 2.28, 2.75, 1.77, 1.94, 3.39 |
| Strong = Strong with Intensity 1.00 | byte-identical on all six |
| Local tone 1.50 vs 2.00 | differ: 1.20, 1.69, 1.68, 1.18, 1.18, 1.66 |
| Local structure 1.50 vs 2.00 | differ: 0.83, 1.24, 1.26, 0.87, 0.94, 1.29 |

(Clip order: `real-film-cuts`, `real-game-cuts`, `real-game-motion`, `orig-faces`,
`orig-film-cuts-a`, `orig-game-motion`; values are mean |a − b| in 8-bit codes.)

## What a setting costs

Every render records the model's own GPU time per frame (`neural_gpu_ms_p50`) and the
time to its first frame, which includes building the neural feature. Over the same six
clips:

| Setting | model GPU time per frame | change from the default | first frame (median) |
|---|---|---|---|
| Default (shipped) | 3.57 ms (3.56-3.59) | — | 5.64 s |
| Style Natural | 3.60 ms (3.56-3.71) | +0.6 % | 5.49 s |
| Style Cinematic | 3.60 ms (3.59-3.62) | +0.8 % | 5.74 s |
| Preset Strong (redefined) | 3.61 ms (3.57-3.68) | +1.1 % | 5.45 s |
| Preset Gentle | 3.54 ms (3.52-3.56) | −0.9 % | 5.39 s |
| Local tone 2.00 | 3.60 ms (3.56-3.66) | +0.7 % | 5.69 s |
| 2 passes | 6.76 ms (6.63-6.81) | **+89 %** | 5.64 s |

The default rendered again differs from itself by −0.7 to +1.3 % per clip, so no Style,
preset or Look value costs measurable render time; `neural.tip.style`'s "rebuilds the
neural feature" is a fresh temporal history, not a slower render. Neural passes are the
one Look-adjacent control with a cost: each pass is another model evaluation.

## What other sources say

The same cap was found independently before this measurement, and nothing found
contradicts it:

- **The shipped add-on** (strings and disassembly of `renodx-dlss5.addon64`, 6.5.3):
  clamps Intensity, Local tone and Local structure to 0-2 and Skin structure to −1…1
  at load and passes them unchanged to `DLSSNR.Intensity`,
  `DLSSNR.LocalToneStrength`, `DLSSNR.LocalStructureStrength` and
  `DLSSNR.SkinStructureStrength`. Color strength is not an NGX parameter.
  `nvngx_dlssnr.dll` 310.8.SF-v2 contains no `GlobalToneStrength` string (why Global tone
  measured inert) and falls back on presets it does not have (why Preset did).
- **NeuralScreen** measured identical frame hashes at Intensity 1, 1.25, 1.5, 2 and
  2.5 on 310.8.0 and capped its slider at 1 (`tests/test_param_effect.py`), after its
  issue #40, "low effect strength". A ComfyUI node (Konohamaru04 #9) found 1.5 and 2.0
  byte-identical to 1.0, also at Cinematic.
- **NVIDIA's public integrations** (dxvk-remix, nvpro vk_gltf_renderer) document
  Intensity, tone, structure and skin as 0-1. **OpenDLSS-NR**, a bit-exact
  reimplementation of the network, applies Intensity only below 1.
- **RenoDX 7.5 source** says a negative skin value "asks runtime 310.8 to make Skin
  follow Structure", which the skin rows above confirm on 6.5.3. The add-on's 6.5.3
  overlay text ("negative smooths, positive enhances") does not describe what it does.

## Decisions this supports

| Control | Today | Measured | Change |
|---|---|---|---|
| Intensity | 0.00-2.00, "2.00 is the maximum", "0.00 leaves the decoded frame alone" | 1.00-2.00 inert; 0.00 is not the source; near-duplicate of Color strength | Range 0.00-1.00 with 1.00 the default and the maximum; tooltip says what 0.00 does. A saved value above 1.00 is read as 1.00 — the same picture, and one cache identity instead of several |
| Color strength | "the model's colour term" | live; same effect family as Intensity | Tooltip: fades the model's change back towards the source; 0.00 keeps about half of it |
| Local tone | 0.00-2.00 | live across the whole range; above 1.00 moves further from the source | Keep; say 1.00 is NVIDIA's documented maximum and above it is beyond that |
| Local structure | 0.00-2.00 | live; above 1.00 changes detail without moving further from the source | Keep, with the same note |
| Skin structure | "Off" at the left end | "Off" = follow Local structure | Label the left end "Same as Local structure"; the stored −1 is unchanged |
| Preset Strong | "Raised intensity and local structure" | 6 % further from the source than the default; Intensity part inert | Redefined as Local tone 2.00 + Local structure 1.50: 22 % further (8-41 %), no extra render time. Two passes go further (+69 %) but cost one more model evaluation per frame |
| Footnote | "apply to the paused preview and to the next conversion" | Apply also restarts an active session (issue #15) | Rewrite to say what Apply does while playing, paused and converting |

Style is the largest single lever the dialog offers; its labels (Default / Natural /
Cinematic) do not say that Natural is the one that moves furthest from the source,
and the preset called "Natural (recommended)" uses Style Default.

## Not covered

- The player itself renders with `NRNormGovernor=1`, under which repeats of the
  default differ by about 0.45 on a paused frame (issue #15's face test). That is
  noise around these results, not a change to them; every row here was rendered at 0
  so that byte identity means something.
- Whether Intensity reaches passes 2 and up only through the add-on's per-pass array:
  the 2-pass rows show Intensity 0.40 still acts there, which is all the dialog needs.
- How each setting looks: these numbers say how much and in which direction a
  setting moves the picture, not whether a viewer prefers it.
