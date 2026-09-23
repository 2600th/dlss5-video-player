# Guide files: an A/B harness for offline depth and flow (P2.4) — 24 September 2026

**Verdict.** The harness P2.4 asks for exists and is transparent. `NeuralWorker.exe`
now takes `depth=file:<dir>` and `mv=file:<dir>` guide modes (per-frame Portable Float
Maps, layout in `src/GuideFiles.h` and `tools/benchmark/README.md`), plus `mv=cpu` and
`--guide-dump <dir>`. Feeding the **current estimator's own guides** back through files
renders **byte-identically** to the built-in path on all three clips tried - two new
near/far clips and one NR-processed capture - for both the CPU estimator's motion and
depth together, and for the shipped depth under hardware optical flow.

The first thing the harness measured is the ceiling a depth model could reach here,
and it is low. With the **exact** depth of the two near/far clips fed in, the output
moves by 0.00-0.03 dB PSNR and at most 0.004 in false-motion rate against the shipped
depth proxy, and by no more against constant depth. Depth reaches NGX at the
analysis grid's 12-pixel cell pitch, the same as the proxy, so this is the ceiling for
depth *on this pipeline*; Video Depth Anything cannot beat the truth. The owner steps
to run it anyway are in `tools/benchmark/README.md` ("Evaluating Video Depth
Anything"); torch and the model download are not available on this machine.

## What was built

| Piece | Where |
|---|---|
| `mv=cpu`, `mv=file:<dir>`, `depth=file:<dir>`, `--guide-dump <dir>`; parsed off the helper's line before the shared parser, so the player cannot send them and the cache key never sees them | `src/GuideFiles.h`, `src/NeuralWorkerMain.cpp` |
| Substitution inside the guide stage, after the generator ran (scene-cut decisions stay the estimator's); a missing or malformed file fails the frame rather than falling back | `src/OfflineNeuralRenderer.cpp` (`ProductionEvaluatorAdapter::ApplyGuideFiles`) |
| Near/far clips `depth-pan` (landscape pan, three layers at depth 0.9/0.5/0.15 moving 3/9/24 px per frame) and `depth-subject` (interior, a person-sized subject at 0.2 crossing a room at 0.85) | `tools/benchmark/corpus.py` |
| True depth and motion of both, written from the same constants the clips are built from; clip-wide conversion of offline depth maps; PFM read/write; the round-trip proof | `tools/benchmark/guidefiles.py`, `tools/benchmark/roundtrip.profile.json` |
| `{clip}`/`{work}` in a profile's guides and worker flags | `tools/benchmark/run.py` |
| Tests: the argument extraction (canonical line untouched; every mode; malformed specs refused) and the grid round trip (bit for bit, bottom-up rows, big-endian files, area average) | `tests/NeuralWorkerTests.cpp` |

**Motion files replace hardware flow.** On a card with the optical-flow engine the
shipped motion is NVOFA's full-resolution field, written by a GPU pass that never goes
through the CPU grid, so no grid file can reproduce it. Any `mv=` other than `0`/`1`
switches the engine off for that render. That is why the proof below has two arms:
`mv=cpu` against its own dump (motion and depth), and the shipped `mv=1` against a
dump of its depth alone.

## Environment

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 4080 SUPER, driver 32.0.16.1047 (worker preflight) |
| Runtime | ReShade 6.8.0.2155, RenoDX 6.5.3 (add-on 0.2026.917.1432, built Sep 19 2026), DLSS-NR 310.8.0 |
| Worker | 0.25.0, built from this branch |
| Corpus | `corpus.py --clips depth-pan depth-subject real-film-cuts real-game-cuts real-game-motion`, FFmpeg 9.0.1-essentials, FFV1 |
| Settings | the ten keys the player writes, plus `NRNormGovernor=0` and `ConfigVersion=6` (below) |

Two harness corrections came out of this work and both apply here; the details are in
[the settings report](../knobs-653-20260924/REPORT.md). `run.py` wrote 4.70's
`NREnableUpscaling` key and no `ConfigVersion`, so 6.5.3 migrated every profile's
section as schema v0 on load, which resets `NRChainedHistory`; it now writes the
player's contract and `ConfigVersion=6`. And the add-on's default normalization
governor makes repeats of one configuration differ on these two synthetic clips (21
and 53 frames of 90), so every arm here writes `NRNormGovernor=0`, under which every
repeat tried was byte-identical. The round-trip digests below were identical with and
without the migration, which is also the evidence that `ConfigVersion=6` changes no
pixel at the defaults.

## The round trip: byte-identical

`python tools/benchmark/guidefiles.py prove` (four arms per clip, rendered in order;
the dump arms write every submitted frame's grid, the replay arms read it back):

| clip | `mv=cpu` dump → `mv=file,depth=file` replay | shipped `mv=1` dump → `mv=1,depth=file` replay | CPU vs hardware motion |
|---|---|---|---|
| `depth-pan` (90 frames) | `002d8ae5c14718ab` = `002d8ae5c14718ab` | `d3bae25a2112b9e1` = `d3bae25a2112b9e1` | different |
| `depth-subject` (90) | `caf0a0e99c02c811` = `caf0a0e99c02c811` | `4a34743c7281de2b` = `4a34743c7281de2b` | different |
| `real-game-motion` (76) | `1246b597cd3420bd` = `1246b597cd3420bd` | `3c74153c0c718ff7` = `3c74153c0c718ff7` | different |

Digests are the first 16 hex characters of the sha256 over the rgb24 `framemd5`
sequence. Six of six pairs identical; the command exits 0. Writing the dump costs
about 1.3 ms per frame of guide time (160×90 grid, two files); reading files costs
less than the estimator it stands in for (guide ms 2.7-2.9 on the replay arms against
4.0-4.4 on the dumping arms, which run the estimator and write).

The last column is a finding of its own: **the CPU block matcher and NVOFA give
different pictures on every clip**, which the harness can now measure directly (next
section) where before the CPU path only ran on cards without the engine.

## True guides against the estimator

`docs/measurements/guide-files-20260924/truth.profile.json`, one render per arm,
scored by `analyze.py --no-ocr --no-faces`. `flicker+` and `sigma+` are output minus
source (negative = smoother than the source, which on these clips means motion was
smeared rather than carried); `false mv` is the fraction of source-static cells the
output moved.

| clip | arm | motion | depth | flicker+ | sigma+ | false mv | dE | PSNR | SSIM |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| depth-pan | `truth-cpu` | CPU matcher | proxy | -4.833 | -8.164 | 0.2129 | 12.62 | 16.23 | 0.8527 |
| depth-pan | `truth-shipped` | NVOFA | proxy | -1.891 | -5.030 | 0.1773 | 10.86 | 19.34 | 0.9300 |
| depth-pan | `truth-depth-off` | NVOFA | constant 0.75 | -1.887 | -5.006 | 0.1753 | 10.86 | 19.33 | 0.9301 |
| depth-pan | `truth-depth` | NVOFA | **true** | -1.905 | -5.075 | 0.1814 | 10.88 | 19.31 | 0.9281 |
| depth-pan | `truth-both` | **true** | **true** | -0.303 | -4.105 | 0.2334 | 10.83 | 19.47 | 0.9303 |
| depth-subject | `truth-cpu` | CPU matcher | proxy | -1.477 | -2.122 | 0.0578 | 12.77 | 23.83 | 0.9516 |
| depth-subject | `truth-shipped` | NVOFA | proxy | -0.809 | -1.665 | 0.0605 | 12.94 | 24.20 | 0.9636 |
| depth-subject | `truth-depth-off` | NVOFA | constant 0.75 | -0.752 | -1.661 | 0.0759 | 12.93 | 24.20 | 0.9636 |
| depth-subject | `truth-depth` | NVOFA | **true** | -0.804 | -1.677 | 0.0612 | 12.94 | 24.20 | 0.9637 |
| depth-subject | `truth-both` | **true** | **true** | -0.721 | -1.688 | 0.0594 | 12.89 | 24.27 | 0.9655 |

What it says, in order of size:

1. **Motion matters and NVOFA is close to the truth.** On the pan the CPU matcher
   costs 3.1 dB and 0.078 SSIM against NVOFA and smears the layers (flicker+ -4.8
   against -1.9); true motion adds another 0.13 dB and removes most of the remaining
   smear (-0.30). On the interior the ordering is the same at a smaller size
   (23.83 → 24.20 → 24.27 dB). NVOFA recovers most of what exact motion is worth.
   One metric disagrees: false motion on the pan is highest with true motion
   (0.2334). True vectors carry the posts' 24 px/frame exactly, and the model then
   moves cells the cell-luma test reads as static inside a moving layer; read that
   column only within an arm family.
2. **Depth barely matters, even exact depth.** Proxy, constant and true depth sit
   within 0.03 dB, 0.002 SSIM and 0.07 sigma of each other on both clips. The one
   signal is on the interior, where constant depth raises false motion from 0.0605 to
   0.0759 and true depth sits with the proxy (0.0612): some depth structure helps
   there, and the proxy already has it.
3. **So a depth model's upside on this pipeline is bounded by the `truth-depth` arm,
   and that arm is a wash.** Video Depth Anything cannot do better than exact depth.
   The case for P2.4's model half therefore rests on two things this harness cannot
   yet test: depth delivered at full resolution rather than through the 12-pixel
   analysis grid (a renderer change in `D3D12Renderer.cpp`), and camera-original
   footage, where the clips here are synthetic.

Caveats: two synthetic clips, one render per arm (every arm at `NRNormGovernor=0`
repeated byte-identically wherever it was repeated), and the synthetic-magnitude rule
in `docs/BENCHMARK.md` applies - these numbers are evidence about direction, not size.
The true motion field is exact per pixel but area-averaged onto the 160×90 grid, so
cells on a layer boundary carry a blend, which is also the best the estimator's grid
could carry.

## Reproduce

```
set DLSS_BENCHMARK_BUILD=<scratch dir>        (optional; replaces build-upscaling/)
python tools/benchmark/corpus.py --corpus <c> --clips depth-pan depth-subject real-game-motion
python tools/benchmark/guidefiles.py --corpus <c> truth
python tools/benchmark/guidefiles.py --corpus <c> prove
python tools/benchmark/run.py --corpus <c> --clips depth-pan depth-subject \
    --profile-file docs/measurements/guide-files-20260924/truth.profile.json \
    --profiles truth-shipped truth-cpu truth-depth truth-both truth-depth-off
python tools/benchmark/analyze.py --corpus <c> --no-ocr --no-faces
```
