# The camera-original corpus, and three verdicts re-checked on it - 14 September 2026, RTX 4080 SUPER

**Verdict: the corpus gap is closed.** Seven camera-original clips - publisher-released
footage, not the player's own output filmed off its own window - now exist with **17
frame-verified hard cuts and two genuine gradual transitions**, including the real
cross-dissolve this corpus has never had. Of the three quality verdicts that could be
re-checked on them, **one reproduces exactly** (Q7, the art defaults: `NRPreset` is still
byte-inert), **one becomes a coin flip** (Q3, the depth proxy: 14 of 28 per-clip metrics),
and **one could not be re-checked at all** (Q1, the round-trip gate, because its A/B is a
comparison of two builds and not of two profiles).

The corpus's first real result is separate from all three, and it reverses a recorded
conclusion: on the 22 labelled cuts that come from a camera the **shipped** cut criterion is
perfect and the candidate criterion misses two, so the criterion stays and the earlier
"switch to the failed-fraction candidate" conclusion is retired.

What this does **not** establish: nothing here is a human look judgement, no blind A/B was
run, and no timing, VRAM, PSNR/SSIM/delta-E, OCR or face number was measured for these
clips - the Q3 re-check is a temporal/motion-metric comparison only. Q1's quality win, the
only claimed quality win of the three, remains unmeasured on camera-original material.

## Environment

| Item | Value | Evidence |
|---|---|---|
| GPU | NVIDIA GeForce RTX 4080 SUPER, driver `32.0.16.1047` (= 610.47) | worker preflight receipt |
| CPU / OS | Intel Core i7-14700, Windows 11 Pro 10.0.26200 | session |
| Runtime | DLSS-NR `310.8.0`, ReShade `6.8.0.2155`, RenoDX `4.7` | preflight `runtime` |
| Render path | `tools/benchmark/run.py` through the real offline worker; scoring by `tools/benchmark/analyze.py`; cut criterion by `tools/benchmark/cutlab.py`; GUI session by `tools/verification/player_session.ps1` | commands below |
| Determinism | renders bit-identical across repeats within an arm, re-verified today by decoded-frame sequence digest | `analyze.py` `deterministic`, `frames.md5` |
| Driver scope | 610.47 is the neural floor, not the recommended pin (616.64) | nothing here speaks for the pinned driver |

## The clips

Built by `python tools/benchmark/corpus.py`; the seven builders are registered in
`tools/benchmark/corpus.py:487-490` and every clip's frame digest is recorded in
`manifest.json`, so `python tools/benchmark/corpus.py --check` proves whether a rebuild
produced the clips these labels were verified against.

The sources are **copyrighted trailers**. They are **not committed and not redistributed**:
`tools/benchmark/fetch_camera_original.ps1` fetches them
(`fetch_camera_original.ps1:11-22`), `corpus.py` cuts the labelled spans out of them, and
each `orig-*` builder skips itself with a warning when its source is absent
(`corpus.py:381-384`) - the same contract the `faces` fixture already had, so a checkout
without the sources still builds a complete corpus. Nothing in the fetch script is
SHA-pinned, because a streaming site re-encodes its own files; what is pinned is the video
id, the format id, and the geometry and frame rate the script verifies after download
(`fetch_camera_original.ps1:16-22`).

Two of the three sources are the documented upstreams of
`docs/media/neural-comparison-demo.mp4` itself (`docs/media/README.md:45-48`,
`fetch_camera_original.ps1:52-54`), which is the point: they are the publishers' own
releases of the same titles the demo capture filmed off the player's window.

| tag | source | `CAMERA_SOURCES` entry |
|---|---|---|
| `godfather` | THE GODFATHER 50th Anniversary Trailer (Paramount Pictures), 2560x1440 VP9, 23.976 fps, format 271 | `corpus.py:360-361` |
| `gtavi` | Grand Theft Auto VI: An Extended Look (Netflix / Now Playing), 2560x1440 VP9, 30 fps, format 271 | `corpus.py:362-363` |
| `lawrence` | Lawrence of Arabia - official HD trailer for the new restoration (Park Circus), **1920x1038** AV1 in mp4 (`ffprobe codec_name` = `av1`), 23.976 fps, 122 s, video id `HFAkWNiETrg`, format id `399` | `corpus.py:364-367` |

The third source was acquired for exactly one reason: the cross-dissolve.

Two corrections to this record, both applied after the first commit of it. The
description string in `CAMERA_SOURCES` said 1920x1080 where the file measures
1920x1038; the measured value is the right one and the string now says so. And
this source was originally selected by search text (`ytsearch1:...`), which is
not a pin - the top result can change. It is now pinned to video id
`HFAkWNiETrg` with format id `399`, and both halves were verified rather than
assumed: a format *selector* was still not a pin, because the selector's choice
moves when the site adds an encode, so the concrete format was resolved and the
source re-downloaded from scratch under it. Rebuilding the span from that download
reproduces `orig-dissolve` byte for byte, decoded-frame digest
`81aaa5a012891f6bfde1eb219f69fdf775987c7609ff756ec4c64e4456214e7f`. Format 399 is
AV1, not h264 as the description string first said - the string is prose the
builder never reads, but it is fixed.

**Geometry** (`corpus.py:352-356`, `camera_segment` at `corpus.py:370-376`): crop to the
active picture as measured by cropdetect - godfather `crop=2560:1384:0:28`, gtavi none,
lawrence `crop=1920:884:0:76` - scale to 1080 height with lanczos, then centre-crop to 1920
wide. **No padding**, so no synthetic black row enters the static-cell population that false
motion is divided by. Native frame rate is preserved: forcing 30 fps would duplicate one
frame in five, which is precisely what makes `real-film-cuts` 37 % motionless and its
false-motion *level* incomparable across clips (`docs/BENCHMARK.md:83-95`).

**That scale is not symmetric, and it matters for one clip.** 1384 active rows to
1080 is a downscale for both 1440p sources; 884 active rows to 1080 is a 1.22x
**upscale** for the letterboxed trailer. An upscale adds no information, so
`orig-dissolve` is the one clip here whose fine detail is partly resampled rather
than camera-native, and it should not be cited for sharpness, grain or any
per-pixel fidelity claim. It is kept that way deliberately: what the clip exists
to carry is a transition's temporal structure, which the resample does not touch,
and padding to 1080 instead would have put 196 static black rows into the
static-cell denominator that false motion is divided by - the exact artifact this
geometry exists to avoid. The alternative worth trying if anyone needs a
pixel-faithful dissolve is a 884-row corpus of its own, which nothing currently
asks for.

**The digests are tracked** (`tools/benchmark/camera-original.digests.json`).
Neither the sources nor the built clips nor the manifest beside them is in the
repository, so that file is the only thing a clean checkout can verify a rebuild
against: it carries the expected decoded-frame digest, frame count and cut list
per clip, and `corpus.py --check` compares a rebuild against it and fails on
drift. Verified in both directions - it reports every clip matching today, and a
deliberately corrupted entry produces `DRIFT AGAINST TRACKED DIGEST` and a
separate complaint when the pixels match but the labels do not.

| clip | frames | fps | labels |
|---|---:|---:|---|
| `orig-film-cuts-a` | 131 | 23.976 | 8 hard cuts: 4, 14, 24, 39, 51, 81, 105, 120 |
| `orig-film-cuts-b` | 70 | 23.976 | 3 hard cuts: 13, 41, 60 |
| `orig-film-fade` | 31 | 23.976 | soft 6-27, a real fade to black |
| `orig-faces` | 101 | 23.976 | 4 hard cuts: 19, 34, 56, 83; `faces` category |
| `orig-game-cuts` | 91 | 30 | 2 hard cuts: 20, 70 |
| `orig-game-motion` | 66 | 30 | no cuts, one continuous moving shot |
| `orig-dissolve` | 71 | 23.976 | soft 21-48, a real cross-dissolve |

Seven clips, 561 frames, 17 hard cuts, two soft spans. `orig-faces` is the only clip whose
category drives face metrics (`corpus.py:429-436`); face metrics were not measured in this
wave.

## How the labels were verified

Detector proposes, inspection decides - the rule this corpus already followed
(`docs/BENCHMARK.md:105-112`), applied unchanged here. Candidates came from mean |dY| >= 25
with 32-bin luma histogram overlap <= 0.55, computed **on the built 1920x1080 clip** so every
index is clip-local; then every candidate, and every other pair above |dY| 12, was inspected
as a frame pair. The frame at each labelled index is the first frame of the new shot.

The inspection is not ceremony, because a threshold alone would have missed cuts that are
labelled here. In `orig-film-cuts-a`, locals 4 and 51 have histogram overlap **0.65 and
0.75**; in `orig-faces`, locals 19, 34 and 83 have **0.67-0.71**. Those are exactly the
"similar cut" case, and a wrong ground-truth index silently corrupts every cut
precision/recall number computed afterwards.

**Two clips were rebuilt after inspection contradicted the first labelling.**

- `orig-film-cuts-b` starts one frame later than first cut (`corpus.py:406-413`): at source
  2270 the first frame was the tail of the previous shot, which would have put a one-frame
  shot at the head of the clip and given the cut test a boundary no reset policy can serve.
- The first `orig-game-motion` span was **discarded outright**: inspection found two hard
  cuts inside what the source-level scan had called continuous. The replacement span
  (`corpus.py:448-455`) is the one continuous span in the release that is also actually
  moving, and its strongest internal pair was inspected and is the same shot.

### The dissolve

Neither demo upstream contains one. Both were scanned end to end and every transition in
them is a hard cut, a fade through black, or - once, in the GTA source - a fade from white.
That is why the demo capture has none (`docs/BENCHMARK.md:114-119`) and why `real-dissolve`
had to synthesise its transition. The Lawrence span was verified as a dissolve rather than a
fade by **three properties together** (`corpus.py:458-469`):

1. across locals 21-48 each frame is a linear blend of the shots either side, with alpha
   sliding monotonically 1 -> 0 (residual **0.063** of the endpoint difference);
2. both endpoints are bright real shots rather than black, which a fade's would not be;
3. mid-transition gradient energy dips **below both endpoints** - the double-exposure
   signature.

Then confirmed by eye at locals 10/33/56: a title card over white desert dissolving into a
cliff shot, both visible at once in the middle. Recorded honestly: both sides of the
transition carry burned-in title text, so this clip is not clean of overlay.

### The detector's two false leads, because they are instructive

A candidate in the Apocalypse Now trailer had a textbook low-gradient midpoint and turned out
to be an "IN DOLBY VISION" title card over flat water - the low gradient was the water. Two
nature-documentary candidates were single shots whose low-gradient sky and spray fooled the
same test. **A blend test alone does not identify a dissolve**; the three properties above
are required jointly, and the eye check is what settled each one.

## Verdict re-check: Q7, the art defaults - reproduces exactly

`NRPreset` 0/1/2/3 produce **byte-identical decoded frames** on camera-original material,
checked on two clips:

| clip | `NRPreset` 0 / 1 / 2 / 3 output digest |
|---|---|
| `orig-faces` | `fe13589392d572a8c4561704` (all four) |
| `orig-film-cuts-b` | `6af4435732c20474db02b8a1` (all four) |


**The knob provably reached the runtime**, which is the half that makes byte-identity
mean anything: four identical outputs prove inertness only if the four requests
actually differed. Each run's `pass1-ReShade.log` carries the add-on echoing its own
state - `[DLSS 5 Neural Rendering] DLSS5 Generic: DLSS5 active settings: upscaling=OFF
intensity=1.000000 global_tone=1.000000 diffuse_white_nits=203.000000 preset=N style=0
enabled=ON` - with N matching the arm on both clips: `preset=0` in the
`shipped-depth-proxy` runs that serve as the preset-0 arm (all eight keys written,
mask on), and `preset=1/2/3` in the three preset arms. Same check the original Q7
used, and the reason its conclusion was citable.

**The four arms did not all come from one worker build, and that turns out to be
worth more than the tidy version.** The preset-0 arm is the `shipped-depth-proxy`
run, rendered at 23:37 against a `NeuralWorker.exe` built at 20:47
(sha256 `2d1fbfa07835d820...`); presets 1/2/3 ran at 00:15 against the
post-integration worker built at 23:44 (`8ad1ce45fa513a57...`), which carries the
five removed full-target clears and the persistently mapped timestamp readback.
The decoded frames are byte-identical across all four.

Two results fall out of that. The preset claim stands on same-build comparisons
between presets 1, 2 and 3, with preset 0 agreeing from the older build. And the
cross-build equality is the **bit-exactness proof the readback work only asserted**:
`GpuPath` argued from the shader code that a clear immediately before a full-target
draw cannot change a pixel, and here two builds either side of that change produce
identical output on 171 frames of camera-original material. Recorded in
[the readback report](../gpu-readback-20260914/REPORT.md) as well.

The earlier finding rested on four synthetic clips plus one real graded clip
(`docs/measurements/art-defaults-20260914/REPORT.md`). It now holds on publisher footage
too, which moves the conclusion's subject: **the knob being inert is a property of this
runtime, not of the test material.**

## Verdict re-check: Q3, the depth proxy - a coin flip on camera-original

Same two arms as the shipped-mask-state control, `shipped-depth-proxy` versus
`shipped-depth-constant`, differing **only** in the `--guides` string. 7 clips x 2 arms x 2
repeats = **28 renders, 0 failed**, every clip/arm pair bit-identical across repeats.
Artifact: `build-upscaling/benchmark-work/analysis-orig-depth/analysis.json` (52 runs, 28 of
them camera-original).

**The proxy takes 14 of 28 all-pairs temporal/motion metrics.** Per clip, proxy wins out of
4: `orig-film-cuts-a` 3, `orig-film-cuts-b` 2, `orig-film-fade` 3, `orig-faces` 0,
`orig-game-cuts` 1, `orig-game-motion` 4, `orig-dissolve` 1.

Medians over both repeats (identical, per the digests). **Lower is better for all four
metrics**: `flicker_added` and `temporal_sigma_added` are 8-bit luma levels of output minus
source with cut frames excluded, so more negative is smoother than the source;
`false_motion_rate` and `flip_rate_added` are fractions of cells on the generator's own
analysis grid. Delta is proxy minus constant.

| clip | metric | proxy | constant | delta | favours |
|---|---|---:|---:|---:|---|
| `orig-film-cuts-a` | flicker+ | -1.10991 | -1.10655 | -0.00336 | **proxy** |
| | sigma+ | -1.37073 | -1.36561 | -0.00512 | **proxy** |
| | false motion | 0.08438 | 0.08412 | +0.00026 | constant |
| | flips+ | -0.01699 | -0.01659 | -0.00040 | **proxy** |
| `orig-film-cuts-b` | flicker+ | -0.90955 | -0.90660 | -0.00295 | **proxy** |
| | sigma+ | -0.96879 | -0.96390 | -0.00489 | **proxy** |
| | false motion | 0.03208 | 0.03201 | +0.00007 | constant |
| | flips+ | -0.06123 | -0.06148 | +0.00025 | constant |
| `orig-film-fade` | flicker+ | -0.15680 | -0.15217 | -0.00463 | **proxy** |
| | sigma+ | -2.67828 | -2.66664 | -0.01164 | **proxy** |
| | false motion | 0.15112 | 0.14992 | +0.00120 | constant |
| | flips+ | -0.01145 | -0.01120 | -0.00025 | **proxy** |
| `orig-faces` | flicker+ | -1.85889 | -1.86216 | +0.00327 | constant |
| | sigma+ | -2.47033 | -2.47465 | +0.00432 | constant |
| | false motion | 0.03487 | 0.03443 | +0.00044 | constant |
| | flips+ | -0.09085 | -0.09102 | +0.00017 | constant |
| `orig-game-cuts` | flicker+ | -0.24559 | -0.24581 | +0.00022 | constant |
| | sigma+ | -0.80668 | -0.80813 | +0.00145 | constant |
| | false motion | 0.05601 | 0.05585 | +0.00016 | constant |
| | flips+ | -0.01400 | -0.01353 | -0.00047 | **proxy** |
| `orig-game-motion` | flicker+ | 0.10595 | 0.10607 | -0.00012 | **proxy** |
| | sigma+ | 0.62872 | 0.63528 | -0.00656 | **proxy** |
| | false motion | 0.07538 | 0.07561 | -0.00023 | **proxy** |
| | flips+ | -0.00711 | -0.00671 | -0.00040 | **proxy** |
| `orig-dissolve` | flicker+ | -0.21262 | -0.21285 | +0.00023 | constant |
| | sigma+ | -2.41077 | -2.42252 | +0.01175 | constant |
| | false motion | 0.04340 | 0.04212 | +0.00128 | constant |
| | flips+ | -0.01773 | -0.01700 | -0.00073 | **proxy** |

**The one durable signal is the continuous-motion clip.** `orig-game-motion` sweeps 4/4 for
the proxy on camera-original material - and that is the same clip family that swept 4/4 as
an NR-processed capture at mask-off and 3/4 at mask-on
(`docs/measurements/depth-ab-20260914/REPORT.md`). A continuous moving shot is the
adversarial case for the proxy's own vertical prior, and it is where the proxy keeps winning
across three provenances and two mask states.

Everywhere else the direction is **provenance-dependent**, which is what an effect this
small should be expected to do: the whole per-clip spread here is in the fourth decimal
place. This does not overturn the Q3 keep, which rests on cost and on the channel being
live; it removes the per-clip sweep from the list of things that can be cited for it.

## Verdict re-check: Q1, the round-trip gate - not re-checked, and why

**The gate is not a runtime option.** `GuideControls` (`src/GuideControls.h:11-18`) carries
exactly two switches, `motionVectors` and `depth`, and `--guides` expresses exactly
`mv=0|1,depth=0|1`. The gate A/B in
`docs/measurements/gate-real-footage-20260914/REPORT.md` was therefore a comparison of two
**builds** - the gate tree and a pre-gate tree - and not of two profiles. There is no
profile string, INI key or worker flag that disarms the gate in the shipped binary, and none
was invented to pretend otherwise.

Re-checking Q1 on camera-original material requires building the pre-gate tree again and
rendering these seven clips through it. **That is the single largest piece of owed evidence
on the quality side**, because Q1 is the only one of these three verdicts that claims a
quality *win*: the gate lowers false motion on all four NR-processed captures while synthetic
clips said it raises it on three of four. Direction already failed to transfer once for this
exact question, which is why it is the one that most needs camera-original footage.

## The cut criterion, settled

This is the corpus's first real result, and it **reverses a recorded conclusion**.
`python tools/benchmark/cutlab.py --sweep` over all 20 clips, at the shipped 0.3 s debounce;
artifact `build-upscaling/benchmark-work/cutlab-widened.json`:

| criterion | P | R | F1 | FP | missed | over-resets |
|---|---:|---:|---:|---:|---:|---:|
| shipped residual | 0.897 | 0.897 | 0.897 | 3 | 3 | 1 |
| failed-fraction candidate | 0.931 | 0.931 | 0.931 | 2 | 2 | 0 |

The aggregate still favours the candidate, as the earlier 13-clip sweep did (F1 0.923 versus
0.750 then). **Split by provenance it inverts:**

One instrument fix landed between the first sweep and this table, and it is worth
stating because it could have moved these numbers: `cutlab.py` scored the
over-reset window at a hardcoded 30 fps while running the criterion at each clip's
own rate, so the 23.976 fps camera-original clips were judged against a 9-frame
window where the mirror uses 7. It now uses `clip["fps"]`. The bug could only
over-count over-resets on those clips, they scored zero either way, and the sweep
was re-run after the fix to confirm every figure below is unchanged.

| criterion | camera-original (7 clips, 17 cuts) | NR-capture (4 clips, 5 cuts) | synthetic (9 clips, 7 cuts) |
|---|---|---|---|
| shipped residual | 17/17, 0 missed, 0 FP, 0 over-reset | 5/5, clean | 4/7, 3 missed, 3 FP, 1 over-reset |
| failed-fraction | 15/17, 2 missed on `orig-film-cuts-a` | 5/5, clean | 7/7, 0 missed, 2 FP |

**On all 22 real labelled cuts the shipped criterion is perfect, and the candidate misses two
of them. The candidate's entire aggregate advantage comes from synthetic clips** - the nine
clips that were built adversarial on purpose, where the shipped criterion scores 4/7 and the
candidate 7/7.

So **the criterion stays**, and the expired conclusion is retired rather than acted on. The
reason is this repository's standing rule about pooled numbers
(`docs/BENCHMARK.md:167-187`): synthetic clips can invert an effect's direction and always
inflate its magnitude, so an effect is never sized and a threshold is never tuned on numbers
that mix synthetic with real. This is now a worked example of that rule on the very question
that produced it.

For completeness, the best operating point each family can reach with no missed cut and no
over-reset - the comparison `cutlab.py` itself prints (`cutlab.py:302-306`):

| family | points with 0 missed and 0 over-reset | fewest false positives there | best F1 anywhere |
|---|---:|---:|---:|
| residual | 1 | 5 | 0.949 |
| failed-fraction | 10 | 2 | 0.967 |

Read together with the split table, that is the whole finding in two rows: **the candidate is
the better family on the pooled set and the worse one on footage from a camera.**

## Cross-check: the debounce fix is independently confirmed on real material

The debounce change shipped earlier this wave (0.6 s -> 0.3 s) is confirmed here without
being the subject of the experiment. In the shipped-mask depth runs, `history_resets` per
clip is **5/2/1/1** against the frozen record's **4/2/1/1**
(`docs/measurements/depth-ab-20260914/REPORT.md:59`). The extra reset is on `real-film-cuts`
at local **87** - the genuine cut 17 frames after its predecessor that the old window
suppressed. The fix adds a reset where the ground truth has a cut, and nowhere else.

## What was not measured

- No Q1 re-check: two builds, not two profiles (above).
- No timing, wall-clock, fps, GPU-ms or VRAM figure is reported for any camera-original
  render in this wave.
- No PSNR, SSIM, delta-E, OCR or face metric for the camera-original clips; the Q3 re-check
  is the four all-pairs temporal/motion metrics only.
- No human blind A/B. A look decision cannot be overturned by anything in this file.
- One machine, one driver (610.47), one guide configuration, one 1080p SDR path.
