# Which settings change the image on RenoDX 6.5.3 (P2.13) — 24 September 2026

**Verdict.** The 4.70 table that decided which controls the dialog hides was wrong
about one of the two it hid. On the pinned runtime **Color strength changes the
image** - 68 % and 89 % of bytes at 1.00 → 0.20 on the two clips, more than Intensity
at 0.40 - so it is back in the neural settings dialog. **The render preset is still
inert** (0 bytes, 0 → 1 and 0 → 3) and stays hidden. Of the controls community tools
expose, **`NRGlobalTone` and `NRUICorrection` are inert** at every value tried, and
**`NRSkinStructure` and `NRAutoMask` are live but give no face protection**: the skin
term lands on detected faces 2.1× as densely as their area against 1.8× for
Intensity, so nothing was wired into the P2.6 mask.

Three findings matter more than any single row:

1. **Skin structure only honours 0.00-0.99.** Every negative value tried (−1.00,
   −0.50, −0.01) and exactly +1.00 render the shipped picture byte for byte; 0.00,
   0.25, 0.50 and 0.99 each change 39-43 % / 65-66 % of bytes, less as they rise. The
   shipped default −1.00 therefore means *off*, and half the dialog's slider is inert.
   The add-on's own overlay says "negative smooths, positive enhances (0 = neutral)";
   the runtime does not do that. The slider and its default stay (moving the default
   moves every render); its tooltip now says what was measured.
2. **The add-on's default normalization governor makes a render irreproducible on
   some material.** At `NRNormGovernor=2` (the add-on default; the player does not
   write the key) a repeat of one configuration differs from itself on both synthetic
   near/far clips - 12.5 % of bytes from frame 69 of `depth-pan`, 26 % from frame 37
   of `depth-subject`. At 0 every repeat is identical. On the three NR-processed
   captures the governor makes no difference at all. The harness now renders
   byte-for-byte comparisons at 0; the player is unchanged.
3. **The harness had been rendering a migrated configuration.** `run.py` wrote 4.70's
   `NREnableUpscaling=0` and no `ConfigVersion`, so 6.5.3 migrated every fresh
   profile as schema v0 on load and reset `NRChainedHistory` to 1. A first pass of
   this very table reported chained history as inert for that reason. Fixed in
   `run.py` (the player's managed keys plus `ConfigVersion=6`); chained history
   changes 49 % / 77 % of bytes.

## Method

`tools/benchmark/knobs.py` renders every row of its `KNOBS` list through `run.py`'s
worker driver - whole clips, not single previews - and compares each output with the
reference as decoded rgb24 bytes over every frame: the share of bytes that differ, the
mean and the maximum absolute difference. The reference writes the ten keys the player
writes on every render (`NeuralAddonOverridesFor(NeuralSettings{})`: intensity, local
tone and local structure 1.00, skin −1.00, colour strength 1.00, preset 0, style 0,
automatic mask on, one pass, chained history on) plus `NRNormGovernor=0`, and every
row changes exactly one key or one guide. The reference was rendered twice; the
repeat differs in 0 bytes on both clips.

"Face delta / area" is where the change landed: OpenCV's Haar frontal-face cascade
run on every fifth source frame (faces on 12 of 21 sampled frames of `real-film-cuts`,
7 of 14 of `real-game-cuts`, where they cover only 0.9 % of the area and every ratio
sits near 1), the share of the row's absolute difference inside those boxes, against
the share of the area the boxes cover. A control aimed at skin should concentrate
there well above the global controls; none does.

Clips: `real-film-cuts` (102 frames of trailer footage with faces: hands, a man in a
crowd, a revolver, a portrait) and `real-game-cuts` (68 frames, a race exterior cut to
a store interior, both under the game's static HUD, which is where a UI-correction
control would show). Both are NR-processed captures (`docs/BENCHMARK.md` explains
what that does and does not support); the camera-original clips need a download this
machine may not make.

## Environment

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 4080 SUPER, driver 32.0.16.1047 (worker preflight) |
| Runtime | ReShade 6.8.0.2155, RenoDX 6.5.3 (add-on 0.2026.917.1432, built Sep 19 2026), DLSS-NR 310.8.0; the locked files from the main checkout's staged runtime |
| Worker | 0.25.0, built from this branch |
| Corpus | `corpus.py --clips depth-pan depth-subject real-film-cuts real-game-cuts real-game-motion`, FFmpeg 9.0.1-essentials |
| Renders | 68 knob renders, 20 governor renders, all `ok`, every frame neural; no migration `.bak` in any profile after the fix |

## Which keys the pinned add-on reads

From the add-on binary's own key table and overlay strings (`renodx-dlss5.addon64`),
the user-facing controls and what each maps to:

| INI key | overlay label | NGX parameter | player |
|---|---|---|---|
| `NRIntensity` | Overall Intensity | `DLSSNR.Intensity` | writes, dialog |
| `NRGlobalTone` | Global Tone Intensity ("community sweet spot 1.00-1.05") | `DLSSNR.GlobalToneStrength` | not written; inert |
| `NRLocalTone` | Local Tone Intensity | `DLSSNR.LocalToneStrength` | writes, dialog |
| `NRLocalStructure` | Structure Intensity | `DLSSNR.LocalStructureStrength` | writes, dialog |
| `NRSkinStructure` | Character/Skin Structure | `DLSSNR.SkinStructureStrength` | writes, dialog |
| `NRAutoMask` | Automatic / Character Mask | `DLSSNR.UseAutoMask` | writes, dialog |
| `NRUICorrection` | NR UI Correction | `DLSSNR.UICorrection` | not written; inert |
| `NRColorStrength` | Color Strength | - (the add-on's colour bridge) | writes; **dialog again** |
| `NRPreset` | NR Preset | `DLSSNR.Hint.Render.Preset` | writes; hidden, inert |
| `NRStyle` | NR Style | `DLSSNR.Style` | writes, dialog |
| `NRPasses`, `NRChainedHistory` | NR Passes, Chained temporal history | - | write, dialog |
| `NRNormGovernor` | Normalization Governor (Off / Slew / Stable) | - | not written; see finding 2 |

## The table

All rows against the reference; the digest is the first 12 hex characters of the sha256
over the rgb24 `framemd5` sequence of `real-film-cuts`, so every inert row is visibly
the reference's `54a580778d0e`.

| row | control | change | film: bytes / mean / max | film: face delta / area (x) | game: bytes / mean / max | film digest |
|---|---|---|---:|---:|---:|---|
| `shipped-repeat` | (reference, rendered again) | none | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `governor-2` | Normalization governor (hidden) | 0 -> 2 (the add-on default the player ships) | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `governor-2-repeat` | Normalization governor (hidden) | 2, rendered again (its own noise) | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `governor-1` | Normalization governor (hidden) | 0 -> 1 (slew) | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `intensity-040` | Intensity | 1.00 -> 0.40 | 64.74 % / 1.625 / 50 | 13.6 / 7.5 % (1.82x) | 85.94 % / 2.757 / 55 | `26353b90a5f6` |
| `tone-030` | Local tone | 1.00 -> 0.30 | 63.46 % / 1.509 / 94 | 13.4 / 7.5 % (1.80x) | 85.29 % / 2.553 / 67 | `9ac7416d1cfe` |
| `structure-030` | Local structure | 1.00 -> 0.30 | 53.69 % / 1.123 / 82 | 11.2 / 7.5 % (1.50x) | 78.65 % / 2.036 / 58 | `5e469c0b0178` |
| `globaltone-030` | Global tone (hidden) | 1.00 -> 0.30 | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `globaltone-105` | Global tone (hidden) | 1.00 -> 1.05 | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `globaltone-0` | Global tone (hidden) | 1.00 -> 0.00 | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `globaltone-2` | Global tone (hidden) | 1.00 -> 2.00 | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `style-1` | Style | 0 -> 1 (Natural) | 76.37 % / 5.788 / 86 | 8.8 / 7.5 % (1.18x) | 93.64 % / 6.356 / 72 | `8fdb50ad7a19` |
| `style-2` | Style | 0 -> 2 (Cinematic) | 76.32 % / 4.342 / 65 | 9.4 / 7.5 % (1.25x) | 92.20 % / 5.873 / 59 | `fe26b25b9f98` |
| `mv-off` | Motion-vector guide | on -> off | 46.89 % / 1.017 / 116 | 8.2 / 7.5 % (1.10x) | 81.94 % / 2.722 / 144 | `bd7d7ef741e3` |
| `depth-off` | Depth guide | on -> off (motion on) | 35.91 % / 0.542 / 69 | 10.8 / 7.5 % (1.44x) | 63.23 % / 1.180 / 85 | `65b8397faab8` |
| `skin-0` | Skin structure | -1.00 -> 0.00 | 43.13 % / 0.780 / 72 | 15.7 / 7.5 % (2.10x) | 66.34 % / 1.273 / 45 | `b6668e82e0b4` |
| `skin-plus1` | Skin structure | -1.00 -> +1.00 | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `skin-m050` | Skin structure | -1.00 -> -0.50 | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `skin-m001` | Skin structure | -1.00 -> -0.01 | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `skin-p025` | Skin structure | -1.00 -> +0.25 | 42.52 % / 0.759 / 71 | 14.6 / 7.5 % (1.96x) | 66.01 % / 1.259 / 51 | `864ed5cafdf3` |
| `skin-p050` | Skin structure | -1.00 -> +0.50 | 41.33 % / 0.687 / 51 | 12.5 / 7.5 % (1.68x) | 65.94 % / 1.248 / 56 | `3bdeb97b74d3` |
| `skin-p099` | Skin structure | -1.00 -> +0.99 | 38.68 % / 0.589 / 18 | 10.7 / 7.5 % (1.44x) | 65.21 % / 1.222 / 42 | `405fb194660f` |
| `automask-off` | Automatic mask | on -> off | 43.80 % / 0.682 / 34 | 10.1 / 7.5 % (1.36x) | 67.14 % / 1.276 / 43 | `cff6c2db6919` |
| `skin-0-nomask` | Skin structure, mask off | -1.00 -> 0.00 with NRAutoMask=0 | 43.80 % / 0.682 / 34 | 10.1 / 7.5 % (1.36x) | 67.14 % / 1.276 / 43 | `cff6c2db6919` |
| `skin-plus1-nomask` | Skin structure, mask off | -1.00 -> +1.00 with NRAutoMask=0 | 43.80 % / 0.682 / 34 | 10.1 / 7.5 % (1.36x) | 67.14 % / 1.276 / 43 | `cff6c2db6919` |
| `uicorrection-1` | UI correction (hidden) | 0 -> 1 | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `uicorrection-05` | UI correction (hidden) | 0 -> 0.5 | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `colorstrength-020` | Color strength | 1.00 -> 0.20 | 68.28 % / 2.058 / 57 | 14.0 / 7.5 % (1.87x) | 88.69 % / 3.473 / 61 | `72e627670309` |
| `colorstrength-060` | Color strength | 1.00 -> 0.60 | 59.38 % / 1.217 / 33 | 13.4 / 7.5 % (1.80x) | 81.66 % / 2.091 / 37 | `489615bff3c0` |
| `preset-1` | Render preset (hidden) | 0 -> 1 | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `preset-3` | Render preset (hidden) | 0 -> 3 | 0.00 % / 0.000 / 0 | - | 0.00 % / 0.000 / 0 | `54a580778d0e` |
| `passes-2` | Passes | 1 -> 2 | 69.97 % / 2.306 / 74 | 12.2 / 7.5 % (1.63x) | 89.29 % / 3.630 / 71 | `442219bce9f4` |
| `passes-2-unchained` | Chained history | on -> off (2 passes) | 69.76 % / 2.260 / 69 | 12.2 / 7.5 % (1.63x) | 87.76 % / 3.353 / 69 | `ecfa2cbbb4c4` |

Two rows need a note. `passes-2-unchained` is scored against the one-pass reference,
like every row; against its real counterpart - two passes, chained - it differs in
49.41 % / 77.41 % of bytes (mean 0.89 / 2.08), so chained history is live.
`uicorrection-05` is not a half-strength test: the add-on reads the key as an integer
and stored 0.5 back as 0.

The skin rows against each other: mask off, the skin value changes nothing
(`skin-0-nomask` and `skin-plus1-nomask` are both `automask-off`'s digest); mask on,
0.00 against the mask-off picture differs in 44.65 % / 67.23 % of bytes. So the mask
is what lets the skin term act, which is the add-on's own description of the mask
("Let the runtime detect characters automatically so the Character/Skin Structure
response applies to them") and not the "regions to leave untouched" the dialog's
tooltip used to claim.

## The normalization governor

`NRNormGovernor` 0 against 2, each rendered twice, on five clips:

| clip | repeat at 0 | repeat at 2 (the default) | 0 against 2 |
|---|---|---|---|
| `depth-pan` (synthetic) | identical | **21 of 90 frames differ** from frame 69; 12.53 % of bytes, mean 0.41, worst frame 2.18 | 61.79 % of bytes, mean 2.28 |
| `depth-subject` (synthetic) | identical | **53 of 90 frames differ** from frame 37; 26.05 % of bytes, mean 0.46, worst frame 0.90 | 29.32 %, mean 0.56 |
| `real-game-motion` | identical | identical | identical |
| `real-film-cuts` | identical | identical | identical |
| `real-game-cuts` | identical | identical | identical |

The overlay describes the stable governor as a divisor that "settles at fixed rates per
second"; an offline render's frames are not evenly spaced in time, so where the
divisor moves at all its trajectory depends on how fast the render ran. On these
captures it never moves. On saturated synthetic material it does, and there the
shipped render is not reproducible - so every byte-for-byte comparison in the harness
now writes `NRNormGovernor=0`, and every other number in this report was taken at 0.
Whether the player should write 0 is a flicker question (the governor exists to damp
exposure pumping) that byte counts cannot answer; it is left for a measurement that
can, with `analyze.py`'s flicker and sigma on camera-original footage.

## Decisions

| control | 4.70 | 6.5.3 | dialog |
|---|---|---|---|
| Color strength | 0 bytes | **68-89 %** | **shown again** (0.00-1.00 slider, default 1.00) |
| Render preset | 0 bytes | 0 bytes | hidden |
| Skin structure | live | live on 0.00-0.99 only | shown; tooltip corrected |
| Automatic mask | live | live; gates skin | shown; tooltip corrected |
| Global tone, UI correction | not tested | 0 bytes at every value | not written, not shown |
| Normalization governor | not tested | 0 bytes on captures, irreproducible on synthetic | not written; harness writes 0 |
| Chained history | new in 6.x | live | shown |

Colour strength needed no identity work: it never left `NeuralSettings`, the
`[NeuralSettings]` INI section, `NeuralAddonOverridesFor` or
`CanonicalNeuralSettings`, which the render cache key hashes, so a cached render made
at 0.20 was always keyed as 0.20 and none changes meaning now. The dialog gained the
slider (control id 7305, the one it had before it was hidden), its read-back and a
tooltip; `PlayerUiRegressionTests` checks the slider maps 0..100 onto 0.00-1.00 and
that the value reaches the add-on override and the canonical settings string.

## Not changed, and why

- **The skin slider's range and default.** Restricting it to 0.00-0.99 with an "off"
  state would be the honest control, but the default −1.00 is today's picture and
  must stay, and remapping a persisted value is a migration of every user's INI.
  Recorded here and in the tooltip; a follow-up can do it deliberately.
- **The first-launch migration in the player.** `packaging/ReShade.ini` has no
  `[RenoDX.DLSS5]` section, so the section the player writes on its first launch has
  no `ConfigVersion`, and the add-on migrates it on that load - adopting its own
  `NRChainedHistory` and `NRCodecMode`. The player's default is chained history on,
  so this only bites a user whose saved settings turned it off before the runtime
  ever loaded, and only once. Writing `ConfigVersion=6` into a section the player
  creates would close it; it is a runtime-contract change and was left to the owner.
- **The governor default.** See above.

## Reproduce

```
set DLSS_BENCHMARK_BUILD=<scratch dir>        (optional; replaces build-upscaling/)
python tools/benchmark/corpus.py --corpus <c> --clips real-film-cuts real-game-cuts
python tools/benchmark/knobs.py --corpus <c> --clips real-film-cuts real-game-cuts --render-only
python tools/benchmark/knobs.py --corpus <c> --clips real-film-cuts real-game-cuts --compare-only
```

`knobs.py --list` prints every row; `--rows` renders and scores a subset.
