# The round-trip gate on real footage — 14 September 2026, RTX 4080 SUPER

**Verdict: real footage reverses the synthetic result.** On all four new
real-footage clips the forward/backward round-trip gate *lowers* false motion, by
4.6 % to 14.4 % relative, and on two of the four it improves all three of false
motion, cell flips and added temporal σ at once — the outcome no synthetic clip
produced. The whole A/B was then repeated at the shipped auto-mask state and every
conclusion survived (−4.9 % to −13.9 %). The synthetic finding this measurement was
built to test was that the gate raises false motion on `cuts-motion` by 13.5 % of the
share attributable to the neural pass while improving flips and σ, with no clip
improving on all three. That finding still stands on synthetic patterns; it does not
generalize. Scope below is narrow and stated: one machine, one driver, one guide
configuration, 312 source frames, five labelled hard cuts.

This is the measurement [`docs/VERIFICATION-2026-09-14-RTX4080.md`](../../VERIFICATION-2026-09-14-RTX4080.md)
asked for in as many words — "real footage: grain, motion blur, a real dissolve, a
camera that occludes". Three of those four are now in the corpus. A real dissolve is
not, and that is called out rather than papered over.

## Environment

| Item | Value | Evidence |
|---|---|---|
| GPU | NVIDIA GeForce RTX 4080 SUPER, 16047 MiB, driver `32.0.16.1047` (610.47) | preflight receipt in every `result.json` |
| CPU / OS | Intel Core i7-14700, Windows 11 Pro 10.0.26200 | — |
| Gate tree | binaries built from `0556eb0` (gate armed) in worktree `../dlss5-corpus`, branch `slice/corpus`; this slice touched only `corpus.py` and docs, which the worker never loads | `git rev-parse HEAD` at build time |
| Pre-gate tree | `1c41427` (detached, the commit before `6528eb5` landed the gate), worktree `../dlss5-pregate-corpus` | `git rev-parse HEAD` |
| Build | Release x64, VS 17 2022 / MSVC 19.44, targets `DLSSVideoPlayer NeuralWorker` | build logs |
| Runtime | the same 12 locked files staged into both trees from `external/runtime`; ReShade 6.8.0.2155, RenoDX 4.7, DLSS-NR 310.8.0, worker 0.21.2 on both sides | `tools/stage_runtime.ps1`, preflight receipts |
| Encoder | `hevc_nvenc` on all 40 runs | `result.json` |
| Profiles | `baseline` (`mv=1,depth=1`, RenoDX defaults) on both trees; `shipped-defaults` (all eight player `NR*` keys, `NRAutoMask=1`) on both trees; `intensity-0` as the scale control, gate tree only | `run.py --list-profiles`, `--profile-file` |
| FFmpeg | 9.0.1-essentials (gyan.dev), the same binary for corpus and analysis on both sides | `manifest.json` |

The gate is observable in the two trees' own logs, which is what makes this a gate
comparison rather than a version comparison:

```
gate     NVOFA ready: 1920x1080 on a 2x2 grid (960x540 vectors, S10.5 = 1/32 px),
         perf=FAST, cost=on, direction=both, round-trip gate armed, global flow=on.
pre-gate NVOFA ready: 1920x1080 on a 2x2 grid (960x540 vectors, S10.5 = 1/32 px),
         perf=FAST, cost=on.
```

**This is still a tree comparison, not a runtime toggle**, exactly as the earlier
record said: `1c41427..0556eb0` is two waves of work. The attribution rests on
reading those commits, and on the log lines above — `6528eb5` is the gate itself
(both flow directions, global flow, the round-trip rejection in the resolve pass);
`25952ef` only increments counters; `8b9897a` adds a log line and a DXGI enumeration;
`df50c3a` stamps cold-start timings; the rest are docs, tests and benchmark scripts
that the worker never loads. One consequence is visible in the tables: the pre-gate
worker predates the scene-cut counters, so its `worker` column is empty where the
gate side reports strong/weak/suppressed tallies.

## The clips

Four new clips, `"category": "real"`, `"synthetic": false`, cut from this
repository's own demo capture `docs/media/neural-comparison-demo.mp4` (1920×1080,
30 fps, h264, 22.6 s, tracked in git — no fixture to fetch, nothing to redistribute
that the tree does not already carry). Cropped to the player's video surface
(`crop=1362:766:502:126`) and upscaled to 1920×1080 with lanczos so the corpus stays
one resolution; see [`docs/BENCHMARK.md`](../../BENCHMARK.md) for why the crop is
load-bearing rather than cosmetic.

| clip | frames | labelled | content | digest |
|---|---:|---|---|---|
| `real-film-cuts` | 102 | cuts 20, 47, 70, 87 | five film shots, grain, motion blur, a two-frame muzzle flash inside one shot | `8cfd7d5666bcd057` |
| `real-game-cuts` | 68 | cut 32 | race exterior hard-cut to store interior, static HUD over fast motion | `59626fdfd98e0c1d` |
| `real-game-motion` | 76 | none | one continuous shot, camera translating, subject occluding and disoccluding background | `bd5c372bbf162c8b` |
| `real-dissolve` | 66 | soft cut 15–37 | the two shots above cross-faded over 0.7 s | `2f9c40a41a9ea53b` |

**Every cut index was verified, not proposed.** FFmpeg scene detection on the cropped
surface proposed boundaries; each was then confirmed by extracting every frame of the
clip and inspecting it, and each clip's `notes` field in `manifest.json` records how.
The four `real-film-cuts` boundaries scored 0.72 / 0.66 / 0.52 / 0.53 and are the
only pairs in 102 frames where the shot changes; the eight smaller flags inside that
clip (0.032–0.076) are hand motion, a pan, or the muzzle flash, and are deliberately
unlabelled. Independently of the labelling, the built clips' own frame-difference
curves peak exactly at the labelled indices and nowhere else: on `real-film-cuts` the
four labelled pairs measure 83.7 / 76.1 / 60.5 / 67.2 mean |ΔY| against a
largest-non-cut 11.0.

**The capture contains no dissolve**: all 678 frames were differenced and every
transition in it is a single-frame jump. `real-dissolve` therefore has real material
and a synthesised transition, and says so in its notes. Its fade is in `soft_cuts`,
never in `cuts`.

## Commands

```
# corpus, once, in the gate tree (13 clips, 82-96 s)
python tools/benchmark/corpus.py --corpus <gate>/build-upscaling/benchmark-corpus

# renders: identical corpus path on both sides, so the two trees render the same bytes
cd <gate>/tools/benchmark
python run.py --corpus <gate>/build-upscaling/benchmark-corpus \
  --clips real-film-cuts real-game-cuts real-game-motion real-dissolve \
  --profiles baseline --repeats 2
python run.py ... --profiles intensity-0 --repeats 2          # scale control, gate tree only
cd <pre-gate>/tools/benchmark
python run.py --corpus <gate>/build-upscaling/benchmark-corpus \
  --clips real-film-cuts real-game-cuts real-game-motion real-dissolve \
  --profiles baseline --repeats 2

# scoring: the gate tree's analyze.py for both sides, so the metric definitions match
python <gate>/tools/benchmark/analyze.py --corpus <gate>/build-upscaling/benchmark-corpus \
  --runs <tree>/build-upscaling/benchmark-work/runs --no-ocr --no-faces --force
```

40 runs, 0 failed (16 `baseline`, 16 `shipped-defaults`, 8 `intensity-0`).
`analyze.py` derives its output directory from its own location, so
the pre-gate aggregate was scored first and copied aside to `analysis-pregate/` before
the gate aggregate was written; per-run `metrics.json` lives in each tree's own run
directory and was never crossed.

## Determinism

Every clip rendered twice per tree. Within each tree the two repeats produced
**identical rgb24 frame-sequence digests**, and every metric in the tables below is
identical between repeat 1 and repeat 2 to full float precision — maximum spread
across `temporal_sigma_added`, `psnr_mean`, `ssim_mean`, `delta_e_mean` and
`false_motion_rate` is exactly `0.0000000000` on all four clips on both sides. There
is no noise floor for a sub-point delta to hide in.

The corpus itself is reproducible on the same footing. It was built from an empty
directory, then rebuilt whole, and `corpus.py --check` re-hashed it: all thirteen
clips, including the four real ones, produced identical frame-sequence digests every
time, and the four real clips also rebuild identically into a separate empty
directory. The nine synthetic clips' digests are unchanged by this slice's edits —
four of them (`cuts-motion`, `cuts-similar`, `pan-fast`, `flash-exposure`) were
compared directly against the corpus the main checkout had already built and match
bit for bit.

| clip | pre-gate digest | gate digest |
|---|---|---|
| `real-film-cuts` | `fdc66f15ab5851b9` | `bb429113e99cf159` |
| `real-game-cuts` | `83a003e328f8a540` | `1082deb0a2b0aeab` |
| `real-game-motion` | `b96bea78f282c856` | `f1449aa12f3a901c` |
| `real-dissolve` | `09f197cb86b39288` | `377ce328943d14de` |

The digests differ between trees on all four clips, so the gate changes the picture;
they match between repeats within a tree, so the change is reproducible.

## The A/B

Median of 2 repeats per cell (the two repeats are identical, so the median is the
value). Lower is better for false motion, flips, added σ and ΔE; higher is better for
PSNR and SSIM. Δ is gate − pre-gate, so **negative is an improvement** in the first
four columns.

| clip | metric | pre-gate `1c41427` | gate `0556eb0` | Δ | relative |
|---|---|---:|---:|---:|---:|
| `real-film-cuts` | false_motion_rate | 0.01339 | **0.01146** | −0.00193 | −14.4 % |
| | flip_rate_output | 0.12277 | **0.11677** | −0.00600 | −4.9 % |
| | cell_flip_added | −0.00843 | **−0.01443** | −0.00600 | — |
| | temporal_sigma_added | +0.0001 | **−0.0681** | −0.0682 | — |
| | PSNR / SSIM | 33.282 / 0.97275 | 33.286 / 0.97167 | +0.004 / −0.00107 | — |
| | cut P/R (output) | 1.00 / 0.75 | 1.00 / 0.75 | 0 | — |
| `real-game-cuts` | false_motion_rate | 0.16252 | **0.14277** | −0.01975 | −12.2 % |
| | flip_rate_output | 0.26988 | **0.26726** | −0.00262 | −1.0 % |
| | cell_flip_added | −0.00419 | **−0.00681** | −0.00262 | — |
| | temporal_sigma_added | −0.9805 | **−1.0027** | −0.0223 | — |
| | PSNR / SSIM | 31.741 / 0.96275 | **31.872 / 0.96331** | +0.131 / +0.00056 | — |
| | cut P/R (output) | 1.00 / 1.00 | 1.00 / 1.00 | 0 | — |
| `real-game-motion` | false_motion_rate | 0.07736 | **0.07000** | −0.00736 | −9.5 % |
| | flip_rate_output | 0.19277 | **0.18779** | −0.00498 | −2.6 % |
| | cell_flip_added | −0.00847 | **−0.01345** | −0.00498 | — |
| | temporal_sigma_added | **+1.1850** | +1.2809 | +0.0959 | worse |
| | PSNR / SSIM | **29.841 / 0.96888** | 29.500 / 0.96734 | −0.341 / −0.00154 | worse |
| | cut P/R (output) | no cuts, 0 false positives | no cuts, 0 false positives | 0 | — |
| `real-dissolve` | false_motion_rate | 0.13743 | **0.13114** | −0.00629 | −4.6 % |
| | flip_rate_output | **0.25648** | 0.25802 | +0.00154 | +0.6 %, worse |
| | cell_flip_added | **+0.00740** | +0.00894 | +0.00154 | worse |
| | temporal_sigma_added | +0.3401 | **+0.2959** | −0.0442 | — |
| | PSNR / SSIM | **29.174 / 0.96617** | 28.919 / 0.96570 | −0.255 / −0.00047 | worse |
| | soft cut 15–37 | 0 resets inside the fade | 0 resets inside the fade | 0 | — |

Added flicker, for completeness: −0.0457, −0.0995, +0.0115, −0.0906 (gate − pre-gate,
film-cuts / game-cuts / game-motion / dissolve), so three of four improve.

Reset decisions are identical on both sides — 4, 2, 1, 1 accepted history resets per
clip including the priming reset — so the gate changed the flow field and not the cut
decisions, which is why the cut P/R column is flat. The one missed cut on
`real-film-cuts` is the criterion's own behaviour, present identically on both trees:
local 87 fires the weak arm (residual 0.2711, overlap 0.5294) and is then suppressed
because it is 17 frames after the accepted cut at 70, inside the 0.6 s debounce. No
synthetic clip could show that — their shots are 1.0 s or more apart. Precision is
1.00 everywhere, including through the dissolve.

## The scale to read those deltas against

Same construction as the earlier record: the `intensity-0` control is the same tree
and the same gate with the model's relighting at zero, so decode, guides, the
feature-18 pass and the NVENC re-encode all still happen. What it reports is the
plumbing's own share; the difference between it and `baseline` is the share
attributable to the neural pass.

| clip | plumbing (`intensity-0`) | gate `baseline` | NR's share | gate Δ | gate Δ as share of NR |
|---|---:|---:|---:|---:|---:|
| `real-film-cuts` | 0.00887 | 0.01146 | +0.00259 | −0.00193 | **74.4 %** |
| `real-game-cuts` | 0.11120 | 0.14277 | +0.03157 | −0.01975 | **62.6 %** |
| `real-game-motion` | 0.04912 | 0.07000 | +0.02088 | −0.00736 | **35.3 %** |
| `real-dissolve` | 0.07771 | 0.13114 | +0.05343 | −0.00629 | **11.8 %** |

The control is deterministic too (both repeats bit-identical on all four clips), and
its PSNR of 37.5–41.5 against `baseline`'s 28.9–33.3 confirms it is the same pipeline
with the relighting off rather than a different pipeline. So the gate's false-motion
improvement on real footage is between an eighth and three quarters of everything the
neural pass contributes to that metric. On the synthetic `cuts-motion` the same
construction put the gate's *penalty* at 13.5 % of NR's share. The effect is not
noise on either set; it changes sign with the material.

## Repeated at the shipped mask state

The `baseline` profile writes no `NR*` key at all, so it inherits the RenoDX add-on's
own defaults — and `ArtDefaults` measured those to have the auto mask **off**, while
the shipped player always writes `NRAutoMask=1`
(`docs/measurements/art-defaults-20260914/REPORT.md`, Table D; `NeuralSettings.h`
`autoMask{true}` through `NeuralAddonOverridesFor`). That matters here because the
same report shows the mask moving synthetic false motion by −0.0093 to +0.0059
*without keeping its sign*, which is the size of three of the four gate deltas above.
A mask-off measurement therefore could not be assumed to predict the mask-on one, and
the shift cannot be borrowed arithmetically from another slice's clips — the gate
changes the rendered output, so the reference has to be re-rendered on this footage.

So it was. Both trees re-rendered all four clips at the shipped mask state, two
repeats each, using that slice's committed profile file verbatim:

```
python run.py --corpus <gate>/build-upscaling/benchmark-corpus \
  --profile-file <art>/docs/measurements/art-defaults-20260914/shipped-defaults.profile.json \
  --profiles shipped-defaults --clips real-film-cuts real-game-cuts real-game-motion real-dissolve \
  --repeats 2
```

| clip | false motion pre → gate | Δ (relative) | flips added Δ | σ added Δ | PSNR Δ |
|---|---:|---:|---:|---:|---:|
| `real-film-cuts` | 0.01334 → **0.01148** | −0.00186 (−13.9 %) | −0.00621 | −0.0717 | −0.024 |
| `real-game-cuts` | 0.16123 → **0.14280** | −0.01843 (−11.4 %) | −0.00258 | −0.0161 | +0.141 |
| `real-game-motion` | 0.07660 → **0.06942** | −0.00718 (−9.4 %) | −0.00469 | +0.0873 | −0.338 |
| `real-dissolve` | 0.13680 → **0.13006** | −0.00674 (−4.9 %) | +0.00121 | −0.0346 | −0.283 |

**Every conclusion survives, and the numbers barely move.** False motion improves on
all four clips again (−13.9 / −11.4 / −9.4 / −4.9 % against −14.4 / −12.2 / −9.5 /
−4.6 % at mask-off); `real-film-cuts` and `real-game-cuts` again improve all three of
false motion, flips and σ; the two exceptions are again the free-fall shot's σ and the
dissolve's flips. All 16 mask-on runs are bit-identical between repeats with metric
spread exactly `0.0000000000`, and the eight digests differ between trees.

Worth recording, because it is the reason the caveat was worth testing rather than
worth assuming: on *these* clips the mask barely moves the reference at all. Gate-tree
false motion shifts by +0.00002, +0.00004, −0.00058 and −0.00108 between `baseline`
and `shipped-defaults`, one to two orders of magnitude smaller than the ±0.009 the
synthetic clips showed. Real content and synthetic patterns do not respond to the mask
the same way either.

## Verdict

1. **Real footage reverses the synthetic false-motion finding.** Four clips, four
   improvements, −4.6 % to −14.4 % relative, 11.8 % to 74.4 % of the neural pass's own
   share of the metric, and the same four improvements (−4.9 % to −13.9 %) when the
   whole A/B is repeated at the shipped mask state. The synthetic result was one
   improvement in four clips and a penalty on the clip the gate was argued for.
2. **"No clip improves on all three" no longer holds.** `real-film-cuts` and
   `real-game-cuts` improve false motion, cell flips and added σ simultaneously. The
   two clips where something gets worse are the free-fall shot (σ +0.096, PSNR −0.34)
   and the dissolve (flips +0.0015, PSNR −0.26), and in both the false-motion
   improvement survives.
3. **Fidelity is a wash, and slightly negative on two clips.** PSNR moves +0.004,
   +0.131, −0.341, −0.255. The gate is a refusal — a cell the engine contradicts
   itself about emits no motion rather than a confident wrong one — so a small PSNR
   cost on content with heavy real motion is the expected shape, not a surprise.
4. **The gate stays, and the case for removing it is now weaker than it was.** The
   earlier record refused to call the gate a quality win and said settling it needed
   real footage. On this real footage it is a win on the metric it was argued for, on
   every clip. That is the answer to the standing question; per this slice's scope the
   gate itself was not touched.

## What this does not settle (UNEXERCISED)

- **A real dissolve.** The demo capture has none, so `real-dissolve`'s transition is
  synthesised from real material. The soft-cut arm therefore remains untested on a
  real fade. Both trees put zero resets inside the fade, which is within tolerance but
  is also the easy answer.
- **Timing.** The GPU was shared by four concurrent slices for most of this session.
  Every number above is a deterministic quality metric and is unaffected, but
  `wall_s`, `e2e_fps`, `proc_fps` and `gpu_ms_p50` from these runs are CONTENDED and
  not citable. No timing claim is made here.
- **Breadth.** One machine, one driver, one GPU generation (Ada), two mask states of
  one guide configuration (`mv=1,depth=1`), 312 source frames, five labelled hard cuts.
  Two of the four clips are game footage rather than camera footage; only
  `real-film-cuts` carries real film grain.
- **The scale control at mask-on.** The `intensity-0` share-of-NR table was measured
  at mask-off only; the mask-on A/B above was not given its own control, so the
  "11.8–74.4 % of NR's share" framing is a mask-off number. The mask moves the
  reference on these clips by at most 0.00108, so the framing is unlikely to change,
  but that is an argument and not a measurement.
- **The upscale.** Clips are the player surface resampled 1362×766 → 1920×1080 with
  lanczos. Grain and h264 texture survive softened. This is real footage, not pristine
  footage, and a native-resolution real clip could read differently.
- **The gate as a toggle.** Nothing here isolates the gate from the other three
  render-path commits between `1c41427` and `0556eb0`; the attribution is by reading
  those commits and by the NVOFA log lines above. A runtime toggle would be stronger
  and does not exist.
- **Faces.** `faces` still needs the absent `mafia-60s.mkv` fixture and was skipped;
  no face-identity metric was computed on the new clips.
