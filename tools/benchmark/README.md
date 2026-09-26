# Neural quality benchmark

_Verified against 0.26.2 (b965b53) on 2026-09-26._

Repeatable corpus, worker driver, metric analysis and blind A/B tooling for the
isolated `NeuralWorker.exe`. Everything is written
under `build-upscaling/` (already gitignored); nothing here touches the player,
its cache or the committed runtime.

```
python tools/benchmark/corpus.py                       # build-upscaling/benchmark-corpus/*.mkv + manifest.json
python tools/benchmark/corpus.py --clips pan-fast zoom-fast           # a subset, into its own --corpus dir
python tools/benchmark/cutlab.py --sweep               # scores the cut criterion against the labelled cuts
python tools/benchmark/cutlab.py --ladder              # the player's Scene cuts rungs on the same labels
python tools/benchmark/duplab.py --grid                # frame generation's duplicate test on clips re-timed onto twos
python tools/benchmark/run.py --clips text-subtitles cuts-motion --profiles baseline mv-off --repeats 3
python tools/benchmark/run.py --ablation --repeats 2   # every profile in run.ABLATION
python tools/benchmark/run.py --profiles depth-constant depth-proxy  # the depth A/B
python tools/benchmark/analyze.py                      # build-upscaling/benchmark-work/analysis/report.md
python tools/benchmark/blind.py --pairs-per-clip 3     # sealed A/B pairs for one-pass vs two-pass
python tools/benchmark/knobs.py --clips real-film-cuts real-game-cuts  # which settings change the image
python tools/benchmark/guidefiles.py truth             # true depth/motion of the near/far clips
python tools/benchmark/guidefiles.py prove             # guide files render byte-identically to the estimator
```

Requirements: Python 3.12 with `requirements.txt`, `external/ffmpeg/bin/ffmpeg.exe`
and `ffprobe.exe`, a built `build-upscaling/Release/neural-runtime/NeuralWorker.exe`
with the RenoDX/ReShade runtime beside it, and an NVIDIA GPU with `nvml.dll`
(NVML sampling degrades gracefully when it is missing). In a checkout without the
staged `external/` binaries, set `DLSS_BENCHMARK_BUILD` to a scratch directory (it
replaces `build-upscaling/` for everything below) and `DLSS_BENCHMARK_FFMPEG` to a
directory holding `ffmpeg.exe` and `ffprobe.exe`.

## Layout

| Path | Contents |
|---|---|
| `corpus.py` | Deterministic FFV1 corpus generator; `--clips` builds a subset, `--check` re-hashes an existing corpus |
| `run.py` | Profiles, ablation matrix, two-pass, preflight receipts, worker launches, NVML sampling |
| `analyze.py` | Per-run metrics, medians per clip/profile, guide and two-pass deltas, cut scores, `report.md` |
| `cutmirror.py` | The mirror of `src/TemporalGuides.cpp`'s cut path (analysis grid, cell luma, global search, histogram overlap, per-cell match costs, both criteria, the debounce), shared by `analyze.py` and `cutlab.py` |
| `cutlab.py` | Scores the cut criterion itself against the manifest's labelled cuts and sweeps it; `--ladder` scores the four Scene cuts rungs the player offers; needs no GPU, no worker and no render |
| `duplab.py` | Scores `scene_cut::IsDuplicateDecodedPair` - frame generation's hold-a-repeat test - on corpus clips re-timed onto twos through libx264, against the real pairs of the same files and a small object crossing a still frame; needs no GPU |
| `knobs.py` | Renders one control at a time from the player's shipped state and reports bytes differing, mean and max absolute delta, and how much of the change lands on detected faces; the table in `docs/BENCHMARK.md` is its output |
| `governor.py` | Renders the shipped settings at each `NRNormGovernor` value, repeated, and scores reproducibility, flicker+, sigma+ and whole-frame brightness pumping, with a mean-luma-over-time plot per clip; builds `real-lighting` (real footage under an exposure schedule) for it |
| `guidefiles.py` | The `depth=file:` / `mv=file:` layout (PFM read/write, area average), `truth` for the near/far clips, `convert` for offline depth maps, `compare`, and `prove` - the byte-identical round trip |
| `roundtrip.profile.json` | The four arms `guidefiles.py prove` renders |
| `blind.py` | Randomized A/B stills + excerpts with a sealed `key.json`; every candidate frame is provably inside a manifest shot and each excerpt is clipped to that shot, so `--seconds` caps a length it does not guarantee; `--score` tallies a ballot |
| `common.py` | Paths, `ffprobe`/`framemd5` helpers, NWR1 protocol v6 decoder (progress, result, preflight, segment, timeline, and the render's own temporal metrics - the in-app counterpart of `analyze.py`'s flicker and sigma; unknown kinds are skipped by payload length, so a helper that adds one stays readable) |
| `build-upscaling/benchmark-corpus/` | Generated clips and `manifest.json` |
| `build-upscaling/benchmark-work/runtime-snapshot/` | Optional frozen copy of `Release/neural-runtime`; used in preference to `Release/` so a concurrent rebuild cannot change the worker mid-benchmark |
| `build-upscaling/benchmark-work/profiles/<profile>/` | Isolated runtime clone, `ffmpeg.exe`/`ffprobe.exe` hard links, `profile.json`, `preflight.json` |
| `build-upscaling/benchmark-work/runs/<clip>__<profile>__<rep>/` | `output.mkv`, `result.json`, `pass1.bin` (raw metadata pipe), `gpu.csv`, copied `ReShade.log`/`.ini`, `frames.md5`, `metrics.json` |
| `build-upscaling/benchmark-work/analysis/` | `analysis.json`, `report.md` |
| `build-upscaling/benchmark-work/blind/` | `pairs/`, `key.json`, `ballot.csv` |

## Corpus

1920x1080, 30 fps, FFV1 level 3 (lossless, `yuv420p`), 1-8 s each. Every
synthetic clip comes from seeded FFmpeg sources with pinned colours; the
manifest records an rgb24 `framemd5` sequence digest per clip, and
`corpus.py --check` proves a regenerated corpus is bit-identical.

Cut ground truth lives in two manifest keys. `cuts` is the list of frames at which a
history reset must happen; `soft_cuts` is a list of `[first, last]` spans, used by the
dissolve, inside which the first reset is tolerated and counted as neither a hit nor a
false positive while a second one is still a false positive. A clip with both empty
must never reset.

| Clip | Category | Content |
|---|---|---|
| `text-subtitles` | text | 14-28 px drifting mono/UI text plus three burned SRT cues; ground-truth strings and cue times in the manifest |
| `fine-detail` | detail | Mandelbrot zoom, sub-pixel-drifting 16 px grid, `zoompan` test-pattern inset |
| `highlights-gradients` | tone | Animated linear gradients with two radial highlight sweeps that clip to white |
| `cuts-motion` | cuts | Five dissimilar 1.5 s segments hard-cut at frames 45/90/135/180 (recorded in `cuts`) |
| `cuts-similar` | cuts | Four 1.0 s mirrorings of one deep fractal still, hard-cut at 30/60/90. The shots share a luma histogram by construction, so only a correspondence test can see the cut |
| `pan-fast` | cuts | 1.0 s diagonal pan across a 4K fractal still, 64×36 px per frame (6.1 analysis cells). No cut |
| `zoom-fast` | cuts | 1.5 s centre zoom 1.0× → 2.2× on a fractal still, which no global translation can model. No cut |
| `dissolve` | cuts | 0.7 s cross-fade between two shots. `cuts` is empty and `soft_cuts` marks the fade: one reset inside it is tolerated, a second is a false positive |
| `flash-exposure` | cuts | 3 s slow pan with a 4-frame flash and a sustained exposure step. Neither is a cut; both collapse the luma histogram |
| `depth-pan` | depth | Near/far **landscape pan** from three layers at known depth and whole-pixel speed: fractal sky at 0.9 moving 3 px/frame, hills at 0.5 moving 9, roadside posts at 0.15 moving 24. True guides from `guidefiles.py truth`. No cut |
| `depth-subject` | depth | Near/far **interior**: a fractal room at 0.85 drifting 1 px/frame behind a person-sized textured ellipse at 0.2 crossing 14 px/frame and bobbing ±18 px. True guides from `guidefiles.py truth`. No cut |
| `still-hold` | still | NR-processed capture: one film frame of the demo (a revolver on a patterned bedspread, grain) **held for 60 frames**. Every pair is identical, so any motion a guide reports is invented; the clip the flow zero-motion test is judged on. No cut |
| `pan-slow` | still | NR-processed capture: one demo frame (a city skyline) panned right at **exactly 0.5 px/frame** for 90 frames - a 1 px/frame crop of a 2x enlargement, area-reduced. True motion (-0.5, 0) everywhere. No cut |
| `pan-slow-fractal` | still | The same 0.5 px/frame pan over a fractal still: detail at every scale, no noise. No cut |
| `faces` | faces | **Not synthetic**: seconds 12-20 of `build-upscaling/runtime-comparison-20260907/fixtures/mafia-60s.mkv` (frontal/three-quarter faces, skin, hair). Skipped when the fixture is absent |
| `real-film-cuts` | real | **NR-processed capture** (see below): 102 frames, four trailer shots cut at 20/47/70, grain and motion blur, a two-frame muzzle flash *inside* one shot that is deliberately unlabelled — then a static paused player frame from local 87, which is why the cut at 87 is the demo's scene boundary and not a film edit. 37 % of its consecutive pairs carry no motion (frozen tail plus 23.976→30 fps capture duplicates) |
| `real-game-cuts` | real | **NR-processed capture**: 68 frames, one hard cut at 32 from a race exterior to a store interior, with the game's own static HUD over fast camera motion |
| `real-game-motion` | real | **NR-processed capture**: 76 frames, one continuous shot, camera translating while the subject occludes and disoccludes the background. No cut |
| `real-dissolve` | real | NR-processed capture, **synthesised transition**: the source contains no dissolve, so two shots are cross-faded over 0.7 s. `cuts` is empty and `soft_cuts` marks the fade |
| `orig-film-cuts-a` | camera-original | **Camera-original** (see below): 131 frames at 23.976 fps, **eight** hard cuts at 4/14/24/39/51/81/105/120 — the densest real editing rhythm here. Locals 4 and 51 have histogram overlap 0.65/0.75, so a threshold alone misses them |
| `orig-film-cuts-b` | camera-original | **Camera-original**: 70 frames, three hard cuts at 13/41/60. Starts one frame later than first cut, so the clip does not open with a one-frame shot |
| `orig-film-fade` | camera-original | **Camera-original**, and a **real** gradual transition: the trailer's own fade to black, luma 77 → 0 across locals 6-27, no pair above \|dY\| 6. `soft_cuts` marks it |
| `orig-faces` | faces | **Camera-original** faces: 101 frames, four hard cuts at 19/34/56/83, a face clearly visible on at least one side of each. Shares the `faces` category with the `faces` clip above, which is deliberate - they are alternatives, not a pair. On a machine that still has `mafia-60s.mkv` both are built and `--faces` scores both under one category name, so drop one from `--clips` if you want a single face row |
| `orig-game-cuts` | camera-original | **Camera-original** game footage: 91 frames, hard cuts at 20 and 70 (street chase → jet skis → armoured truck) |
| `orig-game-motion` | camera-original | **Camera-original**: 66 frames, one continuous moving shot — the only span in its 26 s source that is both cut-free and actually moving (median \|dY\| 3.9); every other cut-free span is the static end card |
| `orig-dissolve` | camera-original | **Camera-original**, and the **real cross-dissolve** this corpus lacked: locals 21-48 are a linear blend of the shots either side, alpha sliding 1 → 0, residual 0.063 of the endpoint difference, mid-transition gradient below both ends. Both sides carry burned-in title text |
| `orig-film-motion-a` | camera-original | **Camera-original**: 272 frames, 11.3 s, one continuous interior shot with faces and skin and a slow camera — the longest cut-free moving span in its source. Gentle motion (median \|dY\| 1.81); 33 of its 271 pairs are near-static, so read its false-motion level only against itself |
| `orig-film-motion-b` | camera-original | **Camera-original**: 258 frames, 10.8 s, one continuous exterior tracking shot and the **strongest sustained motion** of any clip here (median \|dY\| 4.72 against 3.9 for `orig-game-motion`). Foliage streaming past with real motion blur. Two caveats: the letterbox geometry of its source (884 active rows scaled UP to 1080) and a burned-in title card over the first half |

The four `real` clips are **not camera-original footage.** They are cut from
`fixtures/demo-capture-20260912.mp4` (formerly `docs/media/neural-comparison-demo.mp4`), a screen capture of this player recorded
with neural rendering *on*, so their pixels have been through: source → DLSS-NR →
the player's window → `gdigrab` → an h264 encode → `crop` → a lanczos upscale — and
then the neural pass again when the benchmark renders them. That is sound for an A/B
where both arms see byte-identical input, and it is not a claim about original
footage; `docs/BENCHMARK.md` carries the consequences, including what it does to the
`intensity-0` carrier control. No split-screen, divider or UI chrome is inside any
clip: the capture's magnify/wipe demonstration starts one frame after
`real-film-cuts` ends, checked frame by frame.

The nine `orig-*` clips exist because of that paragraph. They are cut straight
from the publisher's own releases - two of them the documented upstreams of the
demo capture itself (`docs/media/README.md`), the third a restored trailer
acquired for the cross-dissolve neither upstream contains - so nothing in them
has been through the player. Geometry: crop to the active picture, scale to 1080
height with lanczos, centre-crop to 1920 wide, so no padded black row joins the
static-cell population that false motion is divided by. Native frame rate is
kept: re-timing 23.976 to 30 would duplicate one frame in five, and a duplicate
pair is motionless, which is exactly what makes `real-film-cuts` 37 % motionless.

Those sources are copyrighted trailers, so they are neither committed nor
redistributed. `tools/benchmark/fetch_camera_original.ps1` fetches them by video
id and format id into `build-upscaling/camera-original/`, verifies the geometry
and frame rate the labels were verified against, and each `orig-*` builder skips
itself when its source is absent - the same contract `faces` has always had.
Nothing is SHA-pinned, because a streaming site re-encodes its own files. The
integrity check is downstream and it is tracked in git:
`tools/benchmark/camera-original.digests.json` carries the expected decoded-frame
digest, frame count and cut list for each `orig-*` clip, and
`python tools/benchmark/corpus.py --check` compares a rebuild against it - which
is what a clean checkout needs, since neither the sources nor the built clips nor
the manifest beside them are in the repository. Drift there means the upstream
re-encoded or a span moved, not that the labels are wrong; re-verify before citing
them.

One geometry caveat belongs with that promise. The scale to 1080 height is a
*down*scale for the two 1440p sources and an *up*scale for the letterboxed
1920x1038 trailer (884 active rows, 1.22x), so `orig-dissolve` is the one clip
here whose fine detail is partly resampled rather than camera-native. It is kept
that way because what the clip exists to carry is a transition's temporal
structure, and padding to 1080 instead would put 196 static black rows into the
static-cell denominator - but do not cite it for sharpness, grain or any
per-pixel fidelity claim.

Every `orig-*` cut index was verified the same way as the `real-*` ones: a
detector proposes (mean |dY| ≥ 25 with histogram overlap ≤ 0.55, computed on the
built 1920x1080 clip so indices are clip-local), then every candidate and every
other pair above |dY| 12 is inspected as a frame pair. Two clips were rebuilt
when inspection contradicted the first labelling, and one whole span was
discarded for containing two cuts a source-level scan had called continuous.
`docs/measurements/camera-original-20260914/REPORT.md` has the method and what
these clips settled.

## Scene-cut lab (`cutlab.py`)

Scores the criterion rather than a render, so it needs nothing but the corpus and
FFmpeg. It replays `cutmirror` over every clip's cell grids, matches each accepted
history reset against `cuts` / `soft_cuts`, and prints where the labelled cuts sit in
each score's ordering, every firing with its residual/overlap/failed fraction, the
per-clip verdicts, and - with `--sweep` - both threshold families ranked. Cell
features are cached under `<corpus>/cutlab-cache` on the clip's own digest, so the
first run costs a decode pass and later sweeps cost seconds.

`--sweep` compares the shipped residual criterion against the
scale-free candidate in `cutmirror.FailedFractionCriterion`: a cell's match failed
when its winning displacement costs more than `ratio` x its standing-still cost, and
the decision is the fraction of failed cells rather than a mean residual. As of
2026-09-14 the two families reach the identical best operating point, so nothing in
`src/` uses the candidate; see `docs/BENCHMARK.md` for the measurement and why the
shipped thresholds were left alone.

`--ladder` scores the rungs of the player's **Scene cuts** setting
(`cutmirror.LADDER`, the mirror of `ThresholdsFor` in `src/SceneCut.h`). Each rung
moves one number of the default, and the table it prints - precision, recall,
false positives, missed cuts, over-resets and every accepted reset per clip - is
the evidence that rung was chosen on.

## Profiles and ablation

A profile is `{guides, overrides, passes}`:

- `guides` is the `--guides` string: the canonical `mv=0|1,depth=0|1`, or the benchmark-only guide sources `mv=cpu`, `mv=file:<dir>` and `depth=file:<dir>` (see *Guide files* below). A disabled guide is still uploaded with neutral values (motion 0, depth 0.75) so the DLSS input contract is unchanged. `{clip}` and `{work}` are replaced per run.
- `overrides` are exact-case `[RenoDX.DLSS5]` keys written into the profile's `ReShade.ini` after the managed keys: the player's four (`EnableHooks=2`, `NeuralUplift=1`, `NRFollowInputRes=0`, `NRResolutionScale=1`) and `ConfigVersion=6`, the schema stamp the add-on writes on first load - without it 6.5.3 migrates the section as schema v0 and resets `NRChainedHistory` to 1. The resolution pair replaced 4.70's single `NREnableUpscaling=0` when 6.x split the working resolution into a mode and a scale; the scale is a multiplier, so `1` is native. Known RenoDX 6.5.3 keys: `NRIntensity`, `NRLocalTone`, `NRLocalStructure`, `NRSkinStructure`, `NRColorStrength`, `NRPreset` (0-3), `NRStyle` (0 default, 1 natural, 2 cinematic), `NRAutoMask`, `NRPasses` (1-4), `NRChainedHistory` and `NRNormGovernor` (0 off, 1 slew, 2 stable; the add-on's default is 2, the player pins 1), which the player writes; and `NRGlobalTone` (default 1) and `NRUICorrection` (default 0), which it does not. **The add-on's default governor makes a repeat of one configuration differ from itself** on this runtime, so any byte-for-byte comparison writes `NRNormGovernor` 0 or 1; see `docs/measurements/knobs-653-20260924/REPORT.md` and, for what each value costs, `governor.py` and `docs/measurements/governor-20260924/REPORT.md`.
- `passes` 1 or 2.

`run.ABLATION` changes one factor per profile: `baseline`, `mv-off`, `depth-off`,
`automask-off`, `structure-0`, `tone-0`, `intensity-0` (control),
`preset-1..3`, `style-natural`, `style-cinematic`, `two-pass`. Custom sets:
`--profile-file profiles.json` with `{name: {guides, overrides, passes, description,
worker_flags}}`.

`worker_flags` is passed to the worker command line unchanged, and exists for
switches that are a player policy rather than a render parameter - so an A/B over
one becomes two profiles instead of two builds. The case it was added for is the
NV12 conversion pair, `["--gpu-source-conversion", "1", "--gpu-color-conversion",
"1"]`, whose defaults decide how many bytes cross the decoder pipe and the capture
readback; `docs/measurements/gpu-readback-20260914/` carries that profile file and
the measurement, which is why those defaults have not changed.

**For the shipped-state depth comparison, do not use these two.** `depth-constant`/`depth-proxy`
run at RenoDX defaults, which leaves the automatic mask OFF while the player ships
`NRAutoMask=1` - that mask-off run is the flaw the Q3 control existed to close. The
shipped-state arms are `--profile-file docs/measurements/depth-ab-20260914/shipped-state.profile.json
--profiles shipped-depth-proxy shipped-depth-constant`, which write all eight player keys
and differ only in `guides`.

The depth A/B is `--profiles depth-constant depth-proxy`; both name guide
strings the matrix already carries (`depth=0` *is* the constant 0.75 field), so they
resolve to `depth-off` and `baseline` and reuse their run directories rather than
rendering the same configuration twice. `depth-of` is refused by name: no estimator
in the worker derives depth from the NVOFA structure; compute it offline and pass it
as `depth=file:` instead. `--list-profiles` prints the matrix, the two aliases and
that refusal.

Every profile runs `--neural-preflight` once; the Feature 18 probe receipt (GPU,
driver, ReShade/RenoDX/DLSS-NR versions, locked module hashes, feature18
creation/evaluation evidence, RenoDX "active settings" line) is stored in
`profiles/<profile>/preflight.json` and copied into each `result.json`.

Worker exit code 75 means the helper repaired its `ReShade.ini`; the runner
relaunches once with `--configuration-restarted`, exactly like the player.

### Two-pass

Pass 1 renders the corpus clip with the profile's settings. Pass 2 renders the
**pass-1 output MKV** (NVENC HEVC, or software H.264 when NVENC is unavailable)
in a second runtime clone with `NRIntensity=0.750000` added. `result.json`
carries `pass2_reencoded_input: true` and both pass results; the second pass
never sees lossless frames, so every two-pass metric includes one lossy
generation on top of the model's effect.

## Guide files (`guidefiles.py`, P2.4)

`depth=file:` and `mv=file:` let a guide computed offline - by a model this player
does not ship - be rendered through the unchanged neural pass and scored against the
built-in guides by the rest of this harness. They exist only on the helper's own
command line: `NeuralWorker.exe` takes them off before the shared worker parser runs
(`src/GuideFiles.h`), so the player cannot send them, and no cached render can be
made from a file.

| `--guides` value | motion | depth |
|---|---|---|
| `mv=1,depth=1` | built-in: hardware optical flow (NVOFA) where the engine comes up, else the CPU block matcher | built-in depth proxy |
| `mv=cpu,depth=1` | the CPU block matcher even where NVOFA is present | built-in |
| `mv=file:<dir>,depth=1` | `<dir>/NNNNNN.pfm` | built-in |
| `mv=1,depth=file:<dir>` | built-in | `<dir>/NNNNNN.pfm` |
| `--guide-dump <dir>` (worker flag) | writes what each frame was rendered with to `<dir>/mv` and `<dir>/depth` | |
| `--zero-motion-test 0\|1` (worker flag) | the flow resolve pass's zero-motion test forced off or on for a render at source size, whatever the build ships (`src/NeuralMotionPolicy.h`); a Super Resolution carrier keeps it on | |

Profiles may use `{clip}` and `{work}` in `guides` and `worker_flags`; `run.py`
replaces them per run, so one profile names every clip's own directory
(`depth=file:{work}/truth/{clip}/depth`).

**Layout.** One directory per guide, one Portable Float Map per source frame, named by
the frame's number - `round(pts × fps)`, the zero-based decoded index for a file that
starts at pts 0 - as `000000.pfm`, `000001.pfm`, ... Depth is a one-channel `Pf` map in
[0, 1] with **0 = near, 1 = far** (DLSSBackend's convention; outside values are clamped
on the GPU). Motion is a three-channel `PF` map of (x, y, 0) in **DLSS input pixels**
(the source size at 100 % processing scale), pointing **from the current frame to where
the content was in the previous one**: content that moved 3 px left carries +3. PFM
rows are stored bottom-up, as the format specifies; `guidefiles.read_pfm` /
`write_pfm` hand you top-down arrays. Any size is accepted: a file at the analysis
grid's size (160×90 for a 30-fps 1080p clip) is taken exactly, anything else is
area-averaged onto that grid, because the grid is what the renderer's expansion pass
reads - a full-resolution depth map therefore reaches NGX at the same 12-pixel cell
pitch the built-in proxy does. A missing or malformed file fails the render; it never
falls back to the estimator for a frame.

**File motion replaces hardware flow.** On a card with NVOFA the built-in `mv=1`
motion is the engine's full-resolution field, which never passes through the grid, so
it cannot be dumped or replayed as a grid file; any `mv=` other than `0`/`1` switches
the engine off for that render and uses the grid. `mv=cpu` is therefore the reference a
motion file is compared with, and the arm that says how much the engine itself is
worth.

**The proof that the file path is transparent** is `guidefiles.py prove`, which renders
`roundtrip.profile.json` in order: `rt-cpu-dump` (CPU motion, built-in depth, dumped)
then `rt-cpu-replay` (both read back), and `rt-shipped-dump` (the shipped guides,
dumped) then `rt-shipped-depth-replay` (hardware motion, depth read back). Each pair
must be byte-identical by output digest, and the command fails if either is not.

**Near/far clips.** `depth-pan` (a landscape pan: fractal sky at depth 0.9 moving
3 px/frame, hills at 0.5 moving 9, roadside posts at 0.15 moving 24) and
`depth-subject` (an interior: a fractal room at 0.85 drifting 1 px/frame behind a
person-sized ellipse at 0.2 crossing 14 px/frame and bobbing ±18 px) are built by
`corpus.py` from layers at known depth and whole-pixel speeds, so their **true** depth
and motion are known exactly: `guidefiles.py truth` writes them from the same constants
(`corpus.DEPTH_PAN`, `corpus.DEPTH_SUBJECT`) into `benchmark-work/truth/<clip>/`.
`docs/measurements/guide-files-20260924/truth.profile.json` runs the estimator against
the truth on them.

### Evaluating Video Depth Anything (owner steps)

This machine has no torch and must not download a model, so the evaluation itself is
yours to run. Everything from step 4 on is this harness.

1. Environment: a separate venv with `torch` (CUDA build), `torchvision`, `opencv-python`,
   `matplotlib`, `imageio` per the upstream `requirements.txt`, and a clone of
   <https://github.com/DepthAnything/Video-Depth-Anything>. Use **Small only**: its
   weights are Apache-2.0, Base and Large are CC-BY-NC-4.0 and cannot ship. Fetch
   `video_depth_anything_vits.pth` into `checkpoints/` as its README says.
2. Run it over the whole clip in one call - its temporal head needs the sequence, and
   one call keeps one scale - with the Small encoder (`--encoder vits`), on the corpus
   file itself (`--input_video <corpus>/<clip>.mkv`), saving the **raw** output
   (relative inverse depth, float; `--save_npz`), not the colourised video. Keep every
   frame at its own index: if the script resamples the frame rate or caps the length,
   turn that off, because frame *i* of the maps must be frame *i* of the render.
   `convert` refuses a map count that differs from the clip's frame count.
3. Split the stacked array into one file per frame in frame order:
   `for i, d in enumerate(np.load("depths.npz")["depths"]): np.save(f"vda/{i:06d}.npy", d)`
   (use whatever key the saved archive holds). Resolution does not matter.
4. `python tools/benchmark/guidefiles.py convert --clip <clip> --depth-dir vda --inverse`
   normalises **over the whole clip** (the 1st-99th percentile of every frame together,
   never per frame, which would make a static wall's depth pump when something nearer
   walks in), flips it so 0 = near, and writes `benchmark-work/offline-depth/<clip>/depth/`.
5. Render the A/B at the shipped state with a profile file whose arms differ only in
   `guides`: `mv=1,depth=1` against `mv=1,depth=file:{work}/offline-depth/{clip}/depth`,
   plus `mv=1,depth=0` as the constant-depth control, e.g.
   `python tools/benchmark/run.py --profile-file vda.profile.json --profiles vda-proxy vda-file vda-off --clips depth-pan depth-subject orig-film-motion-a orig-game-motion --repeats 2`.
6. `python tools/benchmark/analyze.py --no-ocr --no-faces` and read the guide A/B table:
   sigma+, false mv and flicker+ against the proxy and against constant depth. On the
   two near/far clips the `truth-depth` arm of `truth.profile.json` is the ceiling a
   depth model can reach on this pipeline; a model is worth shipping (ONNX Runtime's
   TensorRT-RTX provider, P2.4's deployment plan) only if it closes part of the gap
   between the proxy and that ceiling on the camera-original clips as well.

## Metrics (`analyze.py`)

All comparisons decode both files to rgb24 with FFmpeg and stream frame pairs;
"sampled" metrics use every `--sample-every` (default 15) frames, while `flicker+`,
`sigma+`, `false mv`, `flips+` and the cut test always see every consecutive pair.
`sigma+`, `false mv`, `flips+` and the cut test use the guide generator's own analysis
grid and thresholds (`src/TemporalGuides.cpp`: at 30 fps 160×90 cells of 12×12 source
pixels at 1080p, normalized Rec.709 cell luma), and are withheld whole - `null` plus a
`temporal_withheld` reason - when the output frame count does not match the source.

| Metric | Definition |
|---|---|
| determinism | sha256 of the rgb24 `framemd5` sequence; `det` is true when every repeat of a clip/profile produced the same digest |
| e2e fps | `frame_count / wall seconds` of the final pass (includes process start, decode, priming, encode finish) |
| proc fps | frames per second between the first and last `Rendering` progress records |
| gpu ms p50/p95/max, guide ms, capture ms, VRAM local | copied from the worker result (GPU timestamp queries around DLSS evaluate, CPU guide generation, readback/capture, `IDXGIAdapter3` local-budget peak). `0` means the worker build does not report it |
| VRAM total | NVML `nvmlDeviceGetMemoryInfo.used` peak across the whole GPU during the run (every process, including the desktop) |
| flicker+ | mean over frames of mean `|Y_t - Y_{t-1}|` for the output minus the same for the source, skipping manifest cut frames; positive = the neural pass added temporal change, negative = it smoothed |
| sigma+ | output minus source per-pixel temporal standard deviation of luma inside a shot (frames between manifest cuts), meaned over pixels and frame-weighted over shots, in 8-bit luma levels; `temporal_sigma_p99_*` in `metrics.json` is the p99 of the same map. Localized shimmer a frame-global mean averages away moves this |
| false mv | fraction of the cells the source held static across a consecutive pair (cell luma change ≤ 2/255) whose output changed by more than the same tolerance: motion the pass invented. The NVENC carrier sets a floor; `intensity-0` measures it |
| flips+ | output minus source fraction of cells whose moving/static verdict changes from one consecutive pair to the next - instability of the motion field rather than of the pixels |
| cut P/R/F1 | the generator's own cut test (residual > 0.30, or residual > 0.10 with histogram overlap < 0.85, debounced over 0.3 s) run over each file's cell grids and matched to the manifest's hard cuts at ±1 frame, source and output. `cuts.*_evidence` in `metrics.json` records every firing frame with residual, overlap, arm and suppression; `cutlab.py` scores the same test against the labelled corpus without needing a render |
| dE | mean CIE76 ΔE*ab between output and source (OpenCV Lab, L rescaled to 0-100) |
| RGB shift | mean per-channel (output − source) in `metrics.json` |
| PSNR / SSIM | RGB PSNR and grayscale Gaussian SSIM against the lossless source. A relighting model is expected to move these; use them as a change magnitude, not a pass/fail |
| OCR out/src | rapidocr on sampled frames; character-level `SequenceMatcher` ratio of the manifest strings active at that timestamp against the recognized text, for output and for the source (the source figure is the OCR engine's own ceiling) |
| face cos | resnet18 (ImageNet, penultimate layer) cosine between the source face crop and the same crop of the output; boxes from OpenCV Haar frontal+profile cascades on the source. `faces.output_drift_mean` in `metrics.json` is the frame-to-frame embedding drift of the output versus the source's own drift |
| resets | `history_resets` from the worker result |

Two-pass rows in `report.md` list metric deltas against `baseline` on the same clip,
and any profile whose guide string differs from `baseline`'s gets the same deltas in a
guide A/B table. `report.md` also carries a per-run cut table and lists any run whose
temporal metrics were withheld.

## Blind A/B (`blind.py`)

For each clip with both a `baseline` and a `two-pass` run, `blind.py` emits
`pairs/<id>-A.png/.mp4` and `-B`, with A/B shuffled per pair from `--seed`.
Candidate frames come from the manifest's own ground truth: shots are derived
from `cuts` and `soft_cuts`, a frame must sit past a 0.1 s guard after the cut
that opened its shot and leave at least `MIN_EXCERPT_SECONDS` (0.5 s) of that
shot behind it, and the excerpt is then clipped to the shot so it never spans an
edit. Shots too short to serve are named in `key.json` as `dropped_shots`, clips
with no usable shot as `skipped_clips`, and a run with nothing left to judge
fails rather than falling back to frame 0 - which is what it used to do on every
cut-bearing clip. The mapping is sealed in
`key.json`; fill `ballot.csv` (`preferred` = A/B/tie, `confidence` 1-5) without
opening it, then `blind.py --score ballot.csv`.

## Caveats

- **Worker output is lossy.** The cache carrier is NVENC HEVC (software H.264 fallback), so PSNR/SSIM/ΔE against the lossless source include encoder loss. Compare profiles against each other and against `intensity-0`, which isolates the carrier's own loss.
- **VRAM numbers are different things.** `peak_local_vram_mib` in the receipt is the worker's own local-budget peak from `IDXGIAdapter3::QueryVideoMemoryInfo`; NVML `total_used_mib` is device-wide and includes the desktop, the benchmark's own decoders and anything else running.
- **Guides only affect output once the worker applies them.** Bit-identical `baseline` and `mv-off` digests mean the worker build ignored `--guides`; check `deterministic` and the digests before attributing an ablation result.
- **OCR is engine-limited.** The source ratio is the ceiling; judge the output by its distance from the source figure.
- **Face embeddings are generic.** resnet18 ImageNet features measure crop similarity, not identity; low cosine indicates a large visual change of the face region, not necessarily a worse face.
- Timing fields (`gpu ms`, `guide ms`, `capture ms`, local VRAM) are zero for worker builds that predate the timing instrumentation.
- Runs are skipped when `runs/<clip>__<profile>__<rep>/result.json` exists; delete the directory to rerun. `--fresh-profiles` rebuilds the runtime clones (needed after a worker rebuild when no snapshot is used).
