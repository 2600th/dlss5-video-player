# Q7: are the shipped art defaults the right ones? (2026-09-14, RTX 4080 SUPER)

**Verdict.** Keep every shipped art default, including `NRLocalTone`. Of the nine alternative
settings the harness can express at the shipped state, three are **bit-identical** to the
shipped picture (`NRPreset` 1, 2, 3), one is the carrier-floor control rather than an art
option (`NRIntensity=0`), four are worse or a wash (`NRAutoMask=0`, `NRLocalStructure=0`,
`NRStyle=1`, `NRStyle=2`), and one looked like a candidate on synthetic clips: `NRLocalTone`,
where lowering the shipped `1.0` cuts the departure from the source by 36-68% in dE (4.6-7.7
dB PSNR) on fractals at no measured cost in temporal stability.

**That candidacy is withdrawn, on real footage.** Repeated on a real graded film clip (Table
F), the knob's entire range is dE 2.48 -> 1.71, a span of **0.77 dE** against 4.02-8.11 on
the synthetic clips - and the NVENC carrier alone accounts for 1.26 dE of the 2.48, so the
whole model contributes 1.22 dE above the floor and the tone term is 0.77 of that. There is
no default change worth making for a sub-dE effect that no metric here distinguishes from the
carrier, which is exactly the magnitude-non-transfer this report warns about twice over. A
human blind A/B remains the only instrument that could overturn a *look* decision, and none
was run.

Two findings that matter more than any knob value:

1. **The harness's `baseline` is not the shipped configuration.** `baseline` writes no `NR*`
   key at all, and the add-on's own default for the automatic mask is *off*, while the
   shipped player writes `NRAutoMask=1`. `baseline` is therefore the mask-**off** picture.
   All four clips: `shipped-automask-off` (eight keys, mask 0) is **bit-identical** to
   `baseline`, and `explicit-automask` (one key, mask 1) is **bit-identical** to
   `shipped-defaults`. The ABLATION row named `automask-off` is consequently a no-op against
   `baseline` - it restates the add-on default - and every earlier "baseline" number in
   `docs/BENCHMARK.md` describes a configuration the player never ships.
2. **`NRPreset` cannot matter on this runtime.** `0/1/2/3` are bit-identical on all four
   clips at both mask states (8 profiles x 4 clips x 2 repeats, one digest per clip). It is
   still written into the runtime INI and still enters the render cache identity, so a stale
   value in `DLSSVideoPlayer.ini` costs a full re-render for byte-identical output.

## Environment

| item | value |
|---|---|
| machine | Windows 11 Pro 10.0.26200, Intel Core i7-14700 (20 threads) |
| GPU | NVIDIA GeForce RTX 4080 SUPER (Ada), 16047 MiB, DXGI driver 32.0.16.1047 (= NVIDIA 610.47) |
| second adapter | Intel UHD Graphics 770 (not used for rendering) |
| worktree | `../dlss5-art`, branch `slice/art`, from `main` at `0556eb0` |
| toolchain | MSVC 19.44.35222.0, CMake VS17 2022 x64, Release, `-DBUILD_TESTING=ON` |
| worker | NeuralWorker 0.21.2, protocol v5 |
| runtime | ReShade 6.8.0.2155, RenoDX 4.7 add-on 0.2026.828.517 (API 18), DLSS-NR 310.8.0 (`nvngx_dlssnr.dll` 310.8.2.0), `nvngx_dlss.dll` 310.8.0.0, Streamline 2.13.0.0 |
| NGX | feature 18 created, evaluated, armed, upscaling off, inline interception - every run |
| corpus | 4 labelled synthetic clips, FFV1 1920x1080 30 fps, built 2026-09-14T11:02:25Z with FFmpeg 9.0.1-essentials (gyan) |
| analysis | Python 3.12.10, `analyze.py --no-ocr --no-faces`, `--sample-every 15` (default) |
| carrier | `hevc_nvenc` on every one of the 204 runs |

Corpus digests (`corpus.py` frame-hash sequence, first 16 hex): `cuts-motion` 225 frames
`d93955e9b6f0aa32` cuts at 45/90/135/180, `cuts-similar` 120 frames `6877df2526da0863`
cuts at 30/60/90, `pan-fast` 30 frames `85f8eb18e9a5901b` no cuts, `flash-exposure` 90
frames `96eac8ef816f9b42` no cuts.

## What the shipped defaults actually are

`packaging/ReShade.ini` ships **no** `[RenoDX.DLSS5]` section, so the shipped art state is
whatever the player writes at runtime. `src/main.cpp:3533` calls
`NeuralAddonOverridesFor(settings)` on every render and `ConfigureNeuralAddon` writes all
eight keys; `settings` comes from `NeuralSettings{}` (`src/NeuralSettings.h:14-22`) unless
`DLSSVideoPlayer.ini [NeuralSettings]` overrides it. The shipped default state is therefore
exactly:

| key | shipped value | source | in add-on preflight receipt? |
|---|---|---|---|
| `NRIntensity` | `1.000000` | `NeuralSettings::intensity` | yes (`intensity=`) |
| `NRLocalTone` | `1.000000` | `NeuralSettings::localTone` | **no** |
| `NRLocalStructure` | `1.000000` | `NeuralSettings::localStructure` | **no** |
| `NRSkinStructure` | `-1.000000` | `NeuralSettings::skinStructure` | **no** |
| `NRColorStrength` | `1.000000` | `NeuralSettings::colorStrength` | **no** |
| `NRPreset` | `0` | `NeuralSettings::preset` | yes (`preset=`) |
| `NRStyle` | `0` | `NeuralSettings::style` | yes (`style=`) |
| `NRAutoMask` | `1` | `NeuralSettings::autoMask` | **no** |

The harness writes only `EnableHooks=2`, `NeuralUplift=1`, `NREnableUpscaling=0` plus a
profile's overrides (`run.py:199-201`), so `ABLATION` measures around the *add-on's*
defaults. Which ABLATION entries restate a shipped value and which change one:

| ABLATION profile | vs shipped defaults |
|---|---|
| `baseline` | **differs** - no `NRAutoMask=1`, i.e. mask off (proved below) |
| `automask-off` | restates the add-on default; no-op vs `baseline` |
| `intensity-0` | changes `NRIntensity` 1.0 -> 0.0 |
| `structure-0` | changes `NRLocalStructure` 1.0 -> 0.0 |
| `tone-0` | changes `NRLocalTone` 1.0 -> 0.0 |
| `preset-1/2/3` | changes `NRPreset` 0 -> 1/2/3 |
| `style-natural`/`style-cinematic` | changes `NRStyle` 0 -> 1/2 |
| `mv-off`/`depth-off`/`two-pass` | not art knobs; out of scope here (`DepthAB` owns the guide A/B) |

Because `ABLATION` has no row for `NRSkinStructure` or `NRColorStrength`, and because this
corpus has no faces, both are listed UNEXERCISED below.

The add-on's preflight receipt (`preflight.runtime.activeSettings`) is **not** a complete
echo of the art state: it reports `upscaling`, `intensity`, `global_tone`,
`diffuse_white_nits`, `preset`, `style`, `enabled` only. Five of the eight keys never appear
in it (`NRLocalTone`, `NRLocalStructure`, `NRSkinStructure`, `NRColorStrength`,
`NRAutoMask`), and three of those five are proved below to change pixels:
`NRLocalTone=0.000000` moves dE by up to 8.1 while leaving the receipt string byte-identical
(`global_tone` stays `1.000000`, so it is a different parameter), and the mask - the one knob
whose shipped value differs from the add-on default - is invisible in it. For those keys the
INI is the only record of what was rendered.

## Commands (reproducible, run from the worktree)

```
git worktree add -b slice/art ../dlss5-art main
cmake -S . -B build-upscaling -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON ^
  -DDLSS_SDK=<main>/external/DLSS -DFFMPEG_STAGED_DIR=<main>/external/ffmpeg/bin ^
  -DNVOF_SDK=<main>/external/nvof -DYOUTUBE_STAGED_DIR=<main>/external/youtube
cmake --build build-upscaling --config Release --parallel
powershell -NoProfile -ExecutionPolicy Bypass -File tools/stage_runtime.ps1 ^
  -InputDirectory <main>/external/runtime -Destination build-upscaling/Release/neural-runtime
copy packaging\ReShade.ini packaging\ReShadePreset.ini build-upscaling\Release\neural-runtime\

python tools/benchmark/corpus.py --corpus <tree>/build-upscaling/benchmark-corpus ^
  --clips cuts-motion cuts-similar pan-fast flash-exposure

cd tools/benchmark
python run.py --corpus <corpus> --clips cuts-motion cuts-similar flash-exposure pan-fast ^
  --profiles baseline automask-off structure-0 tone-0 intensity-0 preset-1 preset-2 preset-3 ^
             style-natural style-cinematic --repeats 2
python run.py --corpus <corpus> --clips cuts-motion cuts-similar flash-exposure pan-fast ^
  --profile-file <docs>/shipped-defaults.profile.json --profiles shipped-defaults --repeats 2
python run.py --corpus <corpus> --clips cuts-motion pan-fast ^
  --profile-file <docs>/explicit-keys.profile.json --profiles explicit-intensity explicit-localtone ^
             explicit-localstructure explicit-skinstructure explicit-colorstrength explicit-automask ^
             explicit-preset-style --repeats 2
python run.py --corpus <corpus> --clips cuts-motion cuts-similar flash-exposure pan-fast ^
  --profile-file <docs>/shipped-state.profile.json --profiles shipped-automask-off shipped-structure-0 ^
             shipped-tone-0 shipped-tone-050 shipped-tone-075 shipped-intensity-0 shipped-preset-1 ^
             shipped-preset-2 shipped-preset-3 shipped-style-natural shipped-style-cinematic --repeats 2
python analyze.py --corpus <corpus> --runs <tree>/build-upscaling/benchmark-work/runs --no-ocr --no-faces --force
python blind.py --single baseline --double shipped-defaults --pairs-per-clip 2 --seconds 2 --seed 7

:: the real-footage follow-up (Table F). corpus.py here is RealCorpus's committed version,
:: `git show slice/corpus:tools/benchmark/corpus.py`, written to a scratch name inside
:: tools/benchmark so its REPO-relative demo path resolves, then deleted. Their file is
:: never modified and nothing of theirs is committed on this branch.
python <scratch corpus.py> --corpus <tree>/build-upscaling/benchmark-corpus-real --clips real-film-cuts
python run.py --corpus <real corpus> --clips real-film-cuts ^
  --profile-file <docs>/shipped-defaults.profile.json --profiles shipped-defaults --repeats 2
python run.py --corpus <real corpus> --clips real-film-cuts ^
  --profile-file <docs>/shipped-state.profile.json --profiles shipped-tone-075 shipped-tone-050 ^
             shipped-tone-0 shipped-intensity-0 --repeats 2
:: real-footage runs must be scored against their own manifest, in their own runs directory
move <tree>\build-upscaling\benchmark-work\runs\real-film-cuts__* ^
     <tree>\build-upscaling\benchmark-work\runs-real\
python analyze.py --corpus <real corpus> --runs <tree>/build-upscaling/benchmark-work/runs-real ^
  --no-ocr --no-faces
:: then copy benchmark-work/analysis -> benchmark-work/analysis-real and re-run the synthetic
:: pass, because the analysis output directory is a fixed constant in common.py
```

`run.py` was not modified. The three `--profile-file` JSONs are committed next to this
report: `shipped-defaults.profile.json` (the eight keys at their shipped values),
`explicit-keys.profile.json` (one shipped key each, to find which explicit write matters)
and `shipped-state.profile.json` (shipped defaults with exactly one knob changed).

## Determinism

214 runs in total: 204 synthetic (102 clip/profile groups x 2 repeats) and 10 on real footage
(5 groups x 2 repeats). `analyze.py` reports `deterministic = true` for **all 107** groups -
the two repeats of every group produce the same `output_digest` (SHA-256 over the rgb24
per-frame MD5 sequence). Zero failed runs, zero frame-count mismatches against the source,
zero withheld temporal metrics, in both sets. Every number below is a median over two
bit-identical repeats, which means it is a single deterministic value rather than an average
of two different ones; the repeats prove reproducibility, not variance. `DepthAB`
independently established this wave that these renders are also bit-identical between an idle
and a loaded box, so contention does not enter the quality numbers.

The two sets are scored separately because `analyze.py` takes one manifest: the synthetic
runs live in `benchmark-work/runs` and score against `benchmark-corpus`, the real-footage runs
in `benchmark-work/runs-real` against `benchmark-corpus-real`, and because the analysis output
directory is a fixed constant the real-footage `analysis.json`/`report.md` were copied to
`benchmark-work/analysis-real/` before the synthetic pass was re-run. Pointing one pass at a
mixed runs directory raises `KeyError` on the clip lookup rather than silently mis-scoring.

Container bytes are *not* stable (the muxer stamps per-run metadata), so file hashes differ
between repeats while the decoded frames do not. Identity claims here always use
`output_digest`.

Scoring was parallelised for throughput, not changed: `analyze.py` caches `metrics.json` per
run, so the runs were split across six copies of the runs directory (each holding only
`result.json`, which carries absolute source/output paths), scored concurrently, and the
`metrics.json` files copied back; the canonical `analysis.json` and `report.md` then come
from one `analyze.py` pass over the whole directory. The single-command form in the block
above reproduces it identically, just serially.

## Table A - ablation around the shipped defaults

One knob changed from the shipped eight per row. `flicker+` and `sigma+` are output minus
source (negative = the pass is smoother than the source), `false mv` is the fraction of
source-static cells the output moved anyway. `digest` is the first 8 hex of `output_digest`,
identical across both repeats.

| clip | profile | n | det | digest | PSNR dB | SSIM | dE | flicker+ | sigma+ | false mv |
|---|---|---|---|---|---|---|---|---|---|---|
| cuts-motion | shipped-defaults | 2 | yes | 6fd7c4ea | 23.02 | 0.8657 | 11.29 | -2.744 | -4.355 | 0.1598 |
| cuts-motion | shipped-automask-off | 2 | yes | 027bb9cc | 22.89 | 0.8673 | 11.25 | -2.853 | -4.603 | 0.1600 |
| cuts-motion | shipped-structure-0 | 2 | yes | 7df912e3 | 23.49 | 0.8738 | 11.22 | -2.462 | -3.672 | 0.1577 |
| cuts-motion | shipped-tone-0 | 2 | yes | fde2a9cd | 27.85 | 0.8677 | 7.27 | -2.749 | -4.195 | 0.1274 |
| cuts-motion | shipped-style-natural | 2 | yes | b6273c1c | 17.20 | 0.8341 | 21.10 | -4.699 | -11.383 | 0.1939 |
| cuts-motion | shipped-style-cinematic | 2 | yes | 689693da | 18.75 | 0.8473 | 18.89 | -3.999 | -8.491 | 0.1754 |
| cuts-motion | shipped-preset-1 | 2 | yes | 6fd7c4ea | 23.02 | 0.8657 | 11.29 | -2.744 | -4.355 | 0.1598 |
| cuts-motion | shipped-preset-2 | 2 | yes | 6fd7c4ea | 23.02 | 0.8657 | 11.29 | -2.744 | -4.355 | 0.1598 |
| cuts-motion | shipped-preset-3 | 2 | yes | 6fd7c4ea | 23.02 | 0.8657 | 11.29 | -2.744 | -4.355 | 0.1598 |
| cuts-motion | shipped-intensity-0 | 2 | yes | 58bcc57c | 32.41 | 0.8710 | 6.06 | -2.615 | -3.412 | 0.0915 |
| cuts-similar | shipped-defaults | 2 | yes | 8d6398bb | 20.87 | 0.9838 | 11.51 | -0.555 | -0.451 | 0.0983 |
| cuts-similar | shipped-automask-off | 2 | yes | 37e5a65b | 20.06 | 0.9829 | 13.31 | -0.512 | -0.371 | 0.1076 |
| cuts-similar | shipped-structure-0 | 2 | yes | 17ba3b24 | 19.97 | 0.9854 | 14.45 | -0.223 | -0.122 | 0.0848 |
| cuts-similar | shipped-tone-0 | 2 | yes | 3900b48e | 28.53 | 0.9840 | 3.66 | -0.812 | -0.771 | 0.0626 |
| cuts-similar | shipped-style-natural | 2 | yes | c06c32aa | 17.04 | 0.9801 | 26.34 | -0.868 | -1.730 | 0.1472 |
| cuts-similar | shipped-style-cinematic | 2 | yes | 2f487161 | 15.27 | 0.9813 | 29.72 | -0.087 | 0.358 | 0.1034 |
| cuts-similar | shipped-preset-1 | 2 | yes | 8d6398bb | 20.87 | 0.9838 | 11.51 | -0.555 | -0.451 | 0.0983 |
| cuts-similar | shipped-preset-2 | 2 | yes | 8d6398bb | 20.87 | 0.9838 | 11.51 | -0.555 | -0.451 | 0.0983 |
| cuts-similar | shipped-preset-3 | 2 | yes | 8d6398bb | 20.87 | 0.9838 | 11.51 | -0.555 | -0.451 | 0.0983 |
| cuts-similar | shipped-intensity-0 | 2 | yes | 59714008 | 32.71 | 0.9877 | 1.81 | -0.553 | -0.512 | 0.0451 |
| flash-exposure | shipped-defaults | 2 | yes | ad3585c8 | 23.42 | 0.9794 | 13.17 | -1.027 | -0.788 | 0.1057 |
| flash-exposure | shipped-automask-off | 2 | yes | ae54d7a3 | 23.16 | 0.9788 | 13.58 | -1.062 | -0.808 | 0.1073 |
| flash-exposure | shipped-structure-0 | 2 | yes | 107311b5 | 24.09 | 0.9828 | 13.92 | -0.495 | -0.304 | 0.1055 |
| flash-exposure | shipped-tone-0 | 2 | yes | 496fda97 | 27.99 | 0.9789 | 5.07 | -1.121 | -0.886 | 0.0936 |
| flash-exposure | shipped-style-natural | 2 | yes | 61765fa7 | 19.30 | 0.9720 | 22.39 | -2.030 | -5.719 | 0.1015 |
| flash-exposure | shipped-style-cinematic | 2 | yes | 9666ecfd | 18.64 | 0.9734 | 24.29 | -1.158 | -2.659 | 0.1317 |
| flash-exposure | shipped-preset-1 | 2 | yes | ad3585c8 | 23.42 | 0.9794 | 13.17 | -1.027 | -0.788 | 0.1057 |
| flash-exposure | shipped-preset-2 | 2 | yes | ad3585c8 | 23.42 | 0.9794 | 13.17 | -1.027 | -0.788 | 0.1057 |
| flash-exposure | shipped-preset-3 | 2 | yes | ad3585c8 | 23.42 | 0.9794 | 13.17 | -1.027 | -0.788 | 0.1057 |
| flash-exposure | shipped-intensity-0 | 2 | yes | 177a4baf | 33.14 | 0.9848 | 2.26 | -0.622 | -0.530 | 0.0787 |
| pan-fast | shipped-defaults | 2 | yes | da62fd4c | 24.27 | 0.9886 | 11.28 | -0.813 | -1.089 | 0.1826 |
| pan-fast | shipped-automask-off | 2 | yes | cfbe0716 | 24.34 | 0.9883 | 11.01 | -0.916 | -1.281 | 0.1767 |
| pan-fast | shipped-structure-0 | 2 | yes | f74d9b7f | 24.67 | 0.9905 | 12.61 | 0.052 | -0.505 | 0.1889 |
| pan-fast | shipped-tone-0 | 2 | yes | 90c70131 | 29.56 | 0.9880 | 3.90 | -1.298 | -1.065 | 0.1640 |
| pan-fast | shipped-style-natural | 2 | yes | fc575ba9 | 20.94 | 0.9837 | 15.67 | -2.641 | -5.327 | 0.1759 |
| pan-fast | shipped-style-cinematic | 2 | yes | b5b99fff | 20.37 | 0.9853 | 19.67 | -0.157 | -0.368 | 0.2157 |
| pan-fast | shipped-preset-1 | 2 | yes | da62fd4c | 24.27 | 0.9886 | 11.28 | -0.813 | -1.089 | 0.1826 |
| pan-fast | shipped-preset-2 | 2 | yes | da62fd4c | 24.27 | 0.9886 | 11.28 | -0.813 | -1.089 | 0.1826 |
| pan-fast | shipped-preset-3 | 2 | yes | da62fd4c | 24.27 | 0.9886 | 11.28 | -0.813 | -1.089 | 0.1826 |
| pan-fast | shipped-intensity-0 | 2 | yes | ed181750 | 38.53 | 0.9924 | 1.35 | -0.293 | -0.348 | 0.1119 |

## Table B - the same rows as deltas against `shipped-defaults`

`cut F1` is the generator's own cut test on the output (absolute, not a delta; `-` where the
clip has no hard cuts, or where nothing was detected so precision is undefined). `resets` is
the worker's own history-reset count: 6 / 1 / 3 / 1 on `cuts-motion` / `cuts-similar` /
`flash-exposure` / `pan-fast` for **every** profile measured, art knobs included - the
worker's cut detector reads the guide grid, not the relit picture.

Downstream cut detectability, on the other hand, is not invariant. On `cuts-similar` the
source-side detector finds 0 of the 3 ground-truth cuts for every profile (that is the point
of the clip), while the rendered output surfaces the cut at frame 60 under `shipped-defaults`,
`baseline` and `shipped-tone-050`, both frames 60 and 90 under `shipped-style-natural`, and
nothing at all under `shipped-tone-0` and `shipped-intensity-0`. On `cuts-motion` output F1
is 0.67 at the shipped defaults, 0.80 under `shipped-tone-0` and `shipped-intensity-0`, and
0.60 under either style. A relight can therefore make an edit more or less visible to a
downstream detector; the player's own resets never move.

| clip | profile | d PSNR | d SSIM | d dE | d flicker+ | d sigma+ | d false mv | d flips+ | cut F1 | resets |
|---|---|---|---|---|---|---|---|---|---|---|
| cuts-motion | shipped-automask-off | -0.13 | +0.0016 | -0.04 | -0.109 | -0.248 | +0.0003 | +0.0003 | 0.67 | 6 |
| cuts-motion | shipped-structure-0 | +0.47 | +0.0081 | -0.08 | +0.282 | +0.682 | -0.0020 | -0.0012 | 0.67 | 6 |
| cuts-motion | shipped-tone-0 | +4.83 | +0.0020 | -4.02 | -0.005 | +0.159 | -0.0324 | -0.0056 | 0.80 | 6 |
| cuts-motion | shipped-style-natural | -5.82 | -0.0315 | +9.81 | -1.955 | -7.029 | +0.0341 | +0.0314 | 0.60 | 6 |
| cuts-motion | shipped-style-cinematic | -4.27 | -0.0184 | +7.60 | -1.255 | -4.136 | +0.0156 | +0.0146 | 0.60 | 6 |
| cuts-motion | shipped-intensity-0 | +9.39 | +0.0053 | -5.23 | +0.129 | +0.943 | -0.0682 | -0.0179 | 0.80 | 6 |
| cuts-similar | shipped-automask-off | -0.81 | -0.0008 | +1.80 | +0.043 | +0.080 | +0.0093 | +0.0041 | 0.50 | 1 |
| cuts-similar | shipped-structure-0 | -0.90 | +0.0016 | +2.94 | +0.332 | +0.330 | -0.0136 | -0.0128 | 0.50 | 1 |
| cuts-similar | shipped-tone-0 | +7.66 | +0.0003 | -7.85 | -0.257 | -0.319 | -0.0357 | -0.0269 | - | 1 |
| cuts-similar | shipped-style-natural | -3.83 | -0.0037 | +14.83 | -0.313 | -1.279 | +0.0489 | +0.0354 | 0.80 | 1 |
| cuts-similar | shipped-style-cinematic | -5.60 | -0.0025 | +18.21 | +0.468 | +0.810 | +0.0051 | -0.0067 | 0.50 | 1 |
| cuts-similar | shipped-intensity-0 | +11.84 | +0.0039 | -9.70 | +0.002 | -0.061 | -0.0532 | -0.0424 | - | 1 |
| flash-exposure | shipped-automask-off | -0.27 | -0.0006 | +0.41 | -0.035 | -0.020 | +0.0016 | +0.0005 | - | 3 |
| flash-exposure | shipped-structure-0 | +0.67 | +0.0035 | +0.75 | +0.532 | +0.484 | -0.0001 | +0.0020 | - | 3 |
| flash-exposure | shipped-tone-0 | +4.57 | -0.0005 | -8.11 | -0.094 | -0.098 | -0.0121 | -0.0029 | - | 3 |
| flash-exposure | shipped-style-natural | -4.12 | -0.0074 | +9.21 | -1.003 | -4.931 | -0.0042 | +0.0020 | - | 3 |
| flash-exposure | shipped-style-cinematic | -4.78 | -0.0060 | +11.12 | -0.131 | -1.871 | +0.0260 | +0.0098 | - | 3 |
| flash-exposure | shipped-intensity-0 | +9.71 | +0.0055 | -10.91 | +0.405 | +0.258 | -0.0270 | -0.0113 | - | 3 |
| pan-fast | shipped-automask-off | +0.08 | -0.0003 | -0.27 | -0.103 | -0.192 | -0.0059 | +0.0006 | - | 1 |
| pan-fast | shipped-structure-0 | +0.40 | +0.0019 | +1.34 | +0.865 | +0.584 | +0.0063 | -0.0022 | - | 1 |
| pan-fast | shipped-tone-0 | +5.30 | -0.0006 | -7.37 | -0.485 | +0.024 | -0.0186 | +0.0030 | - | 1 |
| pan-fast | shipped-style-natural | -3.32 | -0.0049 | +4.39 | -1.828 | -4.238 | -0.0067 | +0.0145 | - | 1 |
| pan-fast | shipped-style-cinematic | -3.90 | -0.0033 | +8.40 | +0.657 | +0.721 | +0.0331 | +0.0100 | - | 1 |
| pan-fast | shipped-intensity-0 | +14.26 | +0.0038 | -9.93 | +0.520 | +0.741 | -0.0707 | -0.0022 | - | 1 |

## Table C - `NRLocalTone` sweep at the shipped state

| clip | profile | n | det | digest | PSNR dB | SSIM | dE | flicker+ | sigma+ | false mv |
|---|---|---|---|---|---|---|---|---|---|---|
| cuts-motion | shipped-defaults | 2 | yes | 6fd7c4ea | 23.02 | 0.8657 | 11.29 | -2.744 | -4.355 | 0.1598 |
| cuts-motion | shipped-tone-075 | 2 | yes | 380d2e11 | 24.07 | 0.8690 | 9.82 | -2.748 | -4.232 | 0.1408 |
| cuts-motion | shipped-tone-050 | 2 | yes | 1076acae | 25.07 | 0.8615 | 8.70 | -2.760 | -4.163 | 0.1309 |
| cuts-motion | shipped-tone-0 | 2 | yes | fde2a9cd | 27.85 | 0.8677 | 7.27 | -2.749 | -4.195 | 0.1274 |
| cuts-similar | shipped-defaults | 2 | yes | 8d6398bb | 20.87 | 0.9838 | 11.51 | -0.555 | -0.451 | 0.0983 |
| cuts-similar | shipped-tone-075 | 2 | yes | 357c9954 | 22.38 | 0.9841 | 8.99 | -0.614 | -0.562 | 0.0843 |
| cuts-similar | shipped-tone-050 | 2 | yes | e13a7a1b | 24.80 | 0.9844 | 6.18 | -0.726 | -0.668 | 0.0689 |
| cuts-similar | shipped-tone-0 | 2 | yes | 3900b48e | 28.53 | 0.9840 | 3.66 | -0.812 | -0.771 | 0.0626 |
| flash-exposure | shipped-defaults | 2 | yes | ad3585c8 | 23.42 | 0.9794 | 13.17 | -1.027 | -0.788 | 0.1057 |
| flash-exposure | shipped-tone-075 | 2 | yes | a305a7b2 | 24.54 | 0.9795 | 10.25 | -1.037 | -0.749 | 0.1008 |
| flash-exposure | shipped-tone-050 | 2 | yes | 1461eec0 | 26.05 | 0.9795 | 7.44 | -1.090 | -0.839 | 0.0928 |
| flash-exposure | shipped-tone-0 | 2 | yes | 496fda97 | 27.99 | 0.9789 | 5.07 | -1.121 | -0.886 | 0.0936 |
| pan-fast | shipped-defaults | 2 | yes | da62fd4c | 24.27 | 0.9886 | 11.28 | -0.813 | -1.089 | 0.1826 |
| pan-fast | shipped-tone-075 | 2 | yes | fbb7bcf2 | 25.64 | 0.9886 | 8.62 | -0.832 | -0.780 | 0.1762 |
| pan-fast | shipped-tone-050 | 2 | yes | df80db21 | 27.65 | 0.9886 | 6.03 | -1.025 | -0.788 | 0.1749 |
| pan-fast | shipped-tone-0 | 2 | yes | 90c70131 | 29.56 | 0.9880 | 3.90 | -1.298 | -1.065 | 0.1640 |

Monotone in the knob on all four clips: dE 11.29 -> 9.82 -> 8.70 -> 7.27 (cuts-motion),
11.51 -> 8.99 -> 6.18 -> 3.66 (cuts-similar), 13.17 -> 10.25 -> 7.44 -> 5.07
(flash-exposure), 11.28 -> 8.62 -> 6.03 -> 3.90 (pan-fast), with PSNR rising 4.6-7.7 dB from
1.0 to 0.0 and SSIM staying within 0.005 either way. Across the whole sweep `flicker+` moves
by at most 0.49 (pan-fast) and `sigma+` by at most 0.32 (cuts-similar), i.e. the local tone
term buys nothing measurable in temporal stability on this corpus; what it does is move
colour.

## Table D - which keys move a pixel

Bit-identity classes over `output_digest`. `explicit-*` profiles write exactly one shipped
key and were rendered on `cuts-motion` and `pan-fast`; every other profile covers all four
clips. Both repeats agree in every class.

| clip | profile | n | det | digest | PSNR dB | SSIM | dE | flicker+ | sigma+ | false mv |
|---|---|---|---|---|---|---|---|---|---|---|
| cuts-motion | baseline | 2 | yes | 027bb9cc | 22.89 | 0.8673 | 11.25 | -2.853 | -4.603 | 0.1600 |
| cuts-motion | shipped-defaults | 2 | yes | 6fd7c4ea | 23.02 | 0.8657 | 11.29 | -2.744 | -4.355 | 0.1598 |
| cuts-motion | explicit-automask | 2 | yes | 6fd7c4ea | 23.02 | 0.8657 | 11.29 | -2.744 | -4.355 | 0.1598 |
| cuts-motion | explicit-intensity | 2 | yes | 027bb9cc | 22.89 | 0.8673 | 11.25 | -2.853 | -4.603 | 0.1600 |
| cuts-motion | explicit-localtone | 2 | yes | 027bb9cc | 22.89 | 0.8673 | 11.25 | -2.853 | -4.603 | 0.1600 |
| cuts-motion | explicit-localstructure | 2 | yes | 027bb9cc | 22.89 | 0.8673 | 11.25 | -2.853 | -4.603 | 0.1600 |
| cuts-motion | explicit-skinstructure | 2 | yes | 027bb9cc | 22.89 | 0.8673 | 11.25 | -2.853 | -4.603 | 0.1600 |
| cuts-motion | explicit-colorstrength | 2 | yes | 027bb9cc | 22.89 | 0.8673 | 11.25 | -2.853 | -4.603 | 0.1600 |
| cuts-motion | explicit-preset-style | 2 | yes | 027bb9cc | 22.89 | 0.8673 | 11.25 | -2.853 | -4.603 | 0.1600 |
| pan-fast | baseline | 2 | yes | cfbe0716 | 24.34 | 0.9883 | 11.01 | -0.916 | -1.281 | 0.1767 |
| pan-fast | shipped-defaults | 2 | yes | da62fd4c | 24.27 | 0.9886 | 11.28 | -0.813 | -1.089 | 0.1826 |
| pan-fast | explicit-automask | 2 | yes | da62fd4c | 24.27 | 0.9886 | 11.28 | -0.813 | -1.089 | 0.1826 |
| pan-fast | explicit-intensity | 2 | yes | cfbe0716 | 24.34 | 0.9883 | 11.01 | -0.916 | -1.281 | 0.1767 |
| pan-fast | explicit-localtone | 2 | yes | cfbe0716 | 24.34 | 0.9883 | 11.01 | -0.916 | -1.281 | 0.1767 |
| pan-fast | explicit-localstructure | 2 | yes | cfbe0716 | 24.34 | 0.9883 | 11.01 | -0.916 | -1.281 | 0.1767 |
| pan-fast | explicit-skinstructure | 2 | yes | cfbe0716 | 24.34 | 0.9883 | 11.01 | -0.916 | -1.281 | 0.1767 |
| pan-fast | explicit-colorstrength | 2 | yes | cfbe0716 | 24.34 | 0.9883 | 11.01 | -0.916 | -1.281 | 0.1767 |
| pan-fast | explicit-preset-style | 2 | yes | cfbe0716 | 24.34 | 0.9883 | 11.01 | -0.916 | -1.281 | 0.1767 |

- `baseline` == `automask-off` == `preset-1` == `preset-2` == `preset-3` ==
  `shipped-automask-off` on **all four** clips, and additionally == `explicit-intensity` ==
  `explicit-localtone` == `explicit-localstructure` == `explicit-skinstructure` ==
  `explicit-colorstrength` == `explicit-preset-style` on the two clips those probes were
  rendered on. The seven single-key probes cover `NRIntensity`, `NRLocalTone`,
  `NRLocalStructure`, `NRSkinStructure`, `NRColorStrength`, `NRPreset` and `NRStyle`:
  writing any of them at its shipped value is indistinguishable from writing nothing, and
  the three presets are indistinguishable from each other.
- `shipped-defaults` == `explicit-automask` (2 clips, single key) == `shipped-preset-1/2/3`
  (4 clips). The entire difference between "no keys written" and "the shipped eight written"
  is `NRAutoMask=1`, and the four-clip proof of that is `shipped-automask-off` == `baseline`.
- Live knobs (output differs from both reference states): `NRIntensity`, `NRLocalTone`,
  `NRLocalStructure`, `NRStyle`, `NRAutoMask`. `NRSkinStructure` and `NRColorStrength` were
  only ever written at their shipped values, so nothing here says whether they are live.

## Table E - ablation around the harness `baseline` (mask off)

This is what `run.py --ablation` measures today, and it is reported separately because the
mask changes the *sign* of one result: `structure-0` improves dE by 2.46 against `baseline`
on `cuts-similar` (13.31 -> 10.85) but worsens it by 2.94 against `shipped-defaults`
(11.51 -> 14.45). Art-knob ablations therefore have to be run at the shipped mask state, or
they can invert.

| clip | profile | n | det | digest | PSNR dB | SSIM | dE | flicker+ | sigma+ | false mv |
|---|---|---|---|---|---|---|---|---|---|---|
| cuts-motion | baseline | 2 | yes | 027bb9cc | 22.89 | 0.8673 | 11.25 | -2.853 | -4.603 | 0.1600 |
| cuts-motion | automask-off | 2 | yes | 027bb9cc | 22.89 | 0.8673 | 11.25 | -2.853 | -4.603 | 0.1600 |
| cuts-motion | structure-0 | 2 | yes | 434d03b9 | 23.73 | 0.8712 | 10.86 | -2.421 | -3.582 | 0.1603 |
| cuts-motion | tone-0 | 2 | yes | 8d473f7f | 27.41 | 0.8688 | 7.42 | -2.799 | -4.371 | 0.1346 |
| cuts-motion | style-natural | 2 | yes | 0c57f45b | 17.18 | 0.8320 | 21.00 | -4.713 | -11.335 | 0.1943 |
| cuts-motion | style-cinematic | 2 | yes | a2dcf9a9 | 18.84 | 0.8458 | 19.02 | -4.078 | -8.692 | 0.1704 |
| cuts-motion | preset-1 | 2 | yes | 027bb9cc | 22.89 | 0.8673 | 11.25 | -2.853 | -4.603 | 0.1600 |
| cuts-motion | preset-2 | 2 | yes | 027bb9cc | 22.89 | 0.8673 | 11.25 | -2.853 | -4.603 | 0.1600 |
| cuts-motion | preset-3 | 2 | yes | 027bb9cc | 22.89 | 0.8673 | 11.25 | -2.853 | -4.603 | 0.1600 |
| cuts-motion | intensity-0 | 2 | yes | 58bcc57c | 32.41 | 0.8710 | 6.06 | -2.615 | -3.412 | 0.0915 |
| cuts-similar | baseline | 2 | yes | 37e5a65b | 20.06 | 0.9829 | 13.31 | -0.512 | -0.371 | 0.1076 |
| cuts-similar | automask-off | 2 | yes | 37e5a65b | 20.06 | 0.9829 | 13.31 | -0.512 | -0.371 | 0.1076 |
| cuts-similar | structure-0 | 2 | yes | 6b74e746 | 21.70 | 0.9864 | 10.85 | -0.286 | -0.212 | 0.0741 |
| cuts-similar | tone-0 | 2 | yes | a694308d | 28.36 | 0.9838 | 3.74 | -0.841 | -0.819 | 0.0636 |
| cuts-similar | style-natural | 2 | yes | a6228758 | 16.54 | 0.9794 | 28.20 | -0.790 | -1.617 | 0.1657 |
| cuts-similar | style-cinematic | 2 | yes | c9f63bd0 | 14.94 | 0.9803 | 31.21 | -0.078 | 0.288 | 0.1139 |
| cuts-similar | preset-1 | 2 | yes | 37e5a65b | 20.06 | 0.9829 | 13.31 | -0.512 | -0.371 | 0.1076 |
| cuts-similar | preset-2 | 2 | yes | 37e5a65b | 20.06 | 0.9829 | 13.31 | -0.512 | -0.371 | 0.1076 |
| cuts-similar | preset-3 | 2 | yes | 37e5a65b | 20.06 | 0.9829 | 13.31 | -0.512 | -0.371 | 0.1076 |
| cuts-similar | intensity-0 | 2 | yes | 59714008 | 32.71 | 0.9877 | 1.81 | -0.553 | -0.512 | 0.0451 |
| flash-exposure | baseline | 2 | yes | ae54d7a3 | 23.16 | 0.9788 | 13.58 | -1.062 | -0.808 | 0.1073 |
| flash-exposure | automask-off | 2 | yes | ae54d7a3 | 23.16 | 0.9788 | 13.58 | -1.062 | -0.808 | 0.1073 |
| flash-exposure | structure-0 | 2 | yes | 73ad2a0d | 24.56 | 0.9831 | 12.94 | -0.487 | -0.290 | 0.1014 |
| flash-exposure | tone-0 | 2 | yes | e0553a2e | 27.79 | 0.9787 | 5.22 | -1.144 | -0.906 | 0.0944 |
| flash-exposure | style-natural | 2 | yes | 9201b870 | 19.16 | 0.9717 | 22.91 | -2.019 | -5.661 | 0.1084 |
| flash-exposure | style-cinematic | 2 | yes | ab0a49f2 | 18.50 | 0.9727 | 24.68 | -1.198 | -2.768 | 0.1302 |
| flash-exposure | preset-1 | 2 | yes | ae54d7a3 | 23.16 | 0.9788 | 13.58 | -1.062 | -0.808 | 0.1073 |
| flash-exposure | preset-2 | 2 | yes | ae54d7a3 | 23.16 | 0.9788 | 13.58 | -1.062 | -0.808 | 0.1073 |
| flash-exposure | preset-3 | 2 | yes | ae54d7a3 | 23.16 | 0.9788 | 13.58 | -1.062 | -0.808 | 0.1073 |
| flash-exposure | intensity-0 | 2 | yes | 177a4baf | 33.14 | 0.9848 | 2.26 | -0.622 | -0.530 | 0.0787 |
| pan-fast | baseline | 2 | yes | cfbe0716 | 24.34 | 0.9883 | 11.01 | -0.916 | -1.281 | 0.1767 |
| pan-fast | automask-off | 2 | yes | cfbe0716 | 24.34 | 0.9883 | 11.01 | -0.916 | -1.281 | 0.1767 |
| pan-fast | structure-0 | 2 | yes | 099d91b6 | 25.00 | 0.9908 | 11.95 | 0.075 | -0.522 | 0.1763 |
| pan-fast | tone-0 | 2 | yes | c11653a3 | 29.21 | 0.9876 | 4.05 | -1.387 | -1.132 | 0.1671 |
| pan-fast | style-natural | 2 | yes | 1d345ccc | 20.80 | 0.9831 | 15.84 | -2.766 | -5.604 | 0.1745 |
| pan-fast | style-cinematic | 2 | yes | 204d6d51 | 20.26 | 0.9849 | 19.97 | -0.233 | -0.541 | 0.2221 |
| pan-fast | preset-1 | 2 | yes | cfbe0716 | 24.34 | 0.9883 | 11.01 | -0.916 | -1.281 | 0.1767 |
| pan-fast | preset-2 | 2 | yes | cfbe0716 | 24.34 | 0.9883 | 11.01 | -0.916 | -1.281 | 0.1767 |
| pan-fast | preset-3 | 2 | yes | cfbe0716 | 24.34 | 0.9883 | 11.01 | -0.916 | -1.281 | 0.1767 |
| pan-fast | intensity-0 | 2 | yes | ed181750 | 38.53 | 0.9924 | 1.35 | -0.293 | -0.348 | 0.1119 |

## Cost: CONTENDED - not citable

The GPU was shared throughout: three sibling slices rendered concurrently, and a later agent
ran driven player sessions including two deliberate CPU/GPU saturation windows. No idle
window was ever claimed for these numbers - `PlayerSession` owned the timing-critical work
and had first claim, and this slice yielded the box on every request. Every figure in this
block is **CONTENDED - not citable**; it is here only to show that no art knob looks like a
cost regression worth chasing.

| profile | gpu ms p50 median/min/max over the 4 clips | proc fps median | peak local VRAM MiB |
|---|---|---|---|
| baseline | 5.76 / 5.62 / 6.04 | 85.5 | 960/993 |
| shipped-defaults | 5.94 / 5.83 / 5.97 | 93.6 | 993 |
| shipped-automask-off | 5.71 / 5.63 / 5.92 | 100.0 | 993 |
| shipped-structure-0 | 5.63 / 5.58 / 5.83 | 100.4 | 993 |
| shipped-tone-075 | 5.70 / 5.64 / 5.80 | 101.9 | 993 |
| shipped-tone-050 | 5.71 / 5.60 / 5.98 | 103.4 | 993 |
| shipped-tone-0 | 5.81 / 5.58 / 5.98 | 98.3 | 993 |
| shipped-style-natural | 5.64 / 5.62 / 5.77 | 100.7 | 1027 |
| shipped-style-cinematic | 5.65 / 5.64 / 5.95 | 101.0 | 1027 |
| shipped-preset-1 | 5.73 / 5.60 / 5.93 | 99.7 | 993 |
| shipped-preset-2 | 5.70 / 5.63 / 5.80 | 98.5 | 993 |
| shipped-preset-3 | 5.65 / 5.59 / 5.97 | 100.1 | 993 |
| shipped-intensity-0 | 5.71 / 5.67 / 5.86 | 100.2 | 1010 |

The VRAM column is the worker's own `IDXGIAdapter3` local-budget peak, which is
process-local rather than device-wide: 993 MiB at the shipped defaults, 1027 MiB for either
`NRStyle` value (+34 MiB) and 1010 MiB for `NRIntensity=0`. Those receipts are consistent
across all repeats and clips, and are the one column in this block that contention does not
plausibly move - but they were still recorded on a shared box, so treat them as indicative.

## Blind A/B: the tool works, the judgement needs a human

`tools/benchmark/blind.py` is profile-agnostic (`--single`/`--double` name any two profiles),
so it does support a blind A/B on the top candidates. Run here for the pair that actually
matters, mask off versus mask on:

```
python blind.py --single baseline --double shipped-defaults --pairs-per-clip 2 --seconds 2 --seed 7
-> 8 pairs in build-upscaling/benchmark-work/blind/pairs (40 files: 16 stills, 16 excerpts,
   8 prompts), sealed key.json, ballot.csv with 8 empty rows
python blind.py --score build-upscaling/benchmark-work/blind/ballot.csv
-> {"votes": {"single": 0, "double": 0, "tie": 0}, "confidence_weighted": {"single": 0.0, "double": 0.0}, "per_clip": {}}
```

`--score` reads a `preferred`/`confidence` column that only a human can fill, and it skips
rows with neither, which is why the scored empty ballot above returns zeros. **There is no
blind verdict in this report and none was fabricated.** The pairs and the sealed key are on
disk for whoever judges them.

One honest defect observed while running it: for clips with hard cuts the candidate-frame
filter (`blind.py:75-78`) requires a frame more than one window away from *every* cut, which
on `cuts-motion` (cuts every 45 frames, 60-frame window) admits nothing, so it falls back to
frame 0 and both pairs of that clip are the same frame. On the two cut clips the eight pairs
are really four distinct stills. Not my file to fix; reported rather than worked around.

## Table F - the tone candidate on real footage (this slice's own measurement)

`RealCorpus` named `real-film-cuts` as the right clip for this follow-up: it is the only one
of theirs with real film grain and a real grade, and the one where the mask shifts the
reference least (+0.00002 false motion), so a tone delta measured there is not competing with
mask noise. The clip is cut from `docs/media/neural-comparison-demo.mp4`, which is tracked in
the repository, so it could be rebuilt here: frames 15-116 of the demo, 102 frames of five
film shots (hands over a bedspread, a car on a road, a man in a crowd, a revolver firing, a
portrait), FFV1 1920x1080 30 fps, hard cuts at 20/47/70/87. It was built with `RealCorpus`'s
own committed `corpus.py` (`slice/corpus` 78ed5e4), extracted read-only from their branch into
a scratch file in this tree and deleted afterwards - their file was never modified and nothing
of theirs is committed here.

That rebuild is also a **cross-tree reproduction of their corpus**: the clip came out
`8cfd7d5666bcd0577f8411b8a5c504314648949fbbebe18d09762b5ca856dee0`, which `RealCorpus`
confirms is byte-for-byte their manifest's digest for the same clip. Built in a different
worktree from nothing but the tracked `neural-comparison-demo.mp4` and the pinned FFmpeg
9.0.1-essentials, it shows their builder carries no state from their tree - a stronger
statement than a same-tree rebuild can make.

Five profiles, 2 repeats each, 10 runs, 0 failed, every group bit-identical across repeats.
The `cut P/R/F1` column was measured in a worktree based on `0556eb0`, which carried the
**0.6 s** weak-arm debounce; that constant is now 0.3 s and the same clip scores
**1.00/1.00/1.00 with 5 resets** on `main` (measured by `RealCorpus` with `cutlab.py`, the
run that also confirmed the shortened window costs the synthetic set nothing). The column is
left as taken and keyed to its window - every other column here is debounce-independent and
stands as measured.

| profile | det | digest | PSNR dB | SSIM | dE | flicker+ | sigma+ | false mv | cut P/R/F1 (0.6 s window) | resets |
|---|---|---|---|---|---|---|---|---|---|---|
| shipped-defaults | yes | 446fced2 | 33.50 | 0.9721 | 2.48 | -0.036 | -0.070 | 0.0115 | 1.00/0.75/0.86 | 4 |
| shipped-tone-075 | yes | eb176ee7 | 34.47 | 0.9747 | 2.23 | -0.042 | -0.085 | 0.0115 | 1.00/0.75/0.86 | 4 |
| shipped-tone-050 | yes | 29bdc151 | 35.66 | 0.9762 | 1.98 | -0.051 | -0.111 | 0.0113 | 1.00/0.75/0.86 | 4 |
| shipped-tone-0 | yes | dea330a5 | 37.20 | 0.9781 | 1.71 | -0.055 | -0.111 | 0.0110 | 1.00/0.75/0.86 | 4 |
| shipped-intensity-0 | yes | 925e85df | 41.46 | 0.9807 | 1.26 | -0.097 | -0.156 | 0.0089 | 1.00/0.75/0.86 | 4 |

What this settles, and it is the report's most useful number:

- The tone knob's **entire** range on real footage is dE 2.48 -> 1.71 = **0.77 dE**
  (+3.70 dB PSNR). On the synthetic clips the same range was 4.02-8.11 dE. Same sign, same
  monotonicity, magnitude 5-11x smaller.
- The carrier is most of what the metric sees. `shipped-intensity-0` - the model doing
  nothing, NVENC HEVC loss alone - is already at dE 1.26, so at the shipped defaults the whole
  neural pass contributes 1.22 dE above the floor and the tone term is 0.77 of that 1.22. The
  knob cannot be worth a default change at that size: the effect being traded is comparable
  to the encoder's own error, and on this material `analyze.py` cannot tell the two apart.
- Nothing temporal moves. Across the whole sweep `flicker+` spans 0.019 and `sigma+` spans
  0.041 levels, false motion goes 0.0115 -> 0.0110, and cut precision/recall/F1 is
  1.00/0.75/0.86 with 4 history resets for all five profiles including the floor control.
- Real footage is a far easier target than the corpus overall: false motion is ~0.011 here
  against 0.098-0.183 on the synthetic clips, with a 0.0089 carrier floor. Any threshold or
  default tuned on the synthetic numbers is being tuned on a different population.

This is one real clip, no faces, no text, and no human has looked at it. It is enough to
withdraw a default-change proposal - the proposed gain is too small to defend - and not enough
to propose one.

### The preset gap on real footage, closed afterwards (measured by `RealCorpus`)

This report's preset result was synthetic-only, which the wave's own rule made a
weaker claim than it looked: the synthetic corpus preserves an effect's sign and
inflates its magnitude, so "no difference on fractals" does not settle "no
difference on film". Six further runs closed it - `shipped-preset-1/2/3`, two
repeats each, on `real-film-cuts`, with this report's committed
`shipped-state.profile.json` verbatim and the existing `shipped-defaults` pair as
the `NRPreset=0` arm from the same build and corpus.

**One digest, eight runs: `446fced262154452`.** All 102 frames, `ok=true`,
`deterministic=true` per arm, and every quality column identical to the last
decimal `analyze.py` prints (PSNR 33.497, SSIM 0.97210, dE 2.484, flicker+
-0.0364, sigma+ -0.0698, false motion 0.01148, 4 resets), as they must be given
one digest.

What makes that evidence rather than a null result: the knob was confirmed to have
been requested *and* to have reached the runtime, because "no difference" and "the
key was never set" are otherwise the same measurement. Per arm, `run.py`'s
`overrides.NRPreset`, the `NRPreset=` line in the copied `pass1-ReShade.ini`, and
the add-on's own preflight `activeSettings` string all agree - RenoDX echoes back
`preset=1`, `preset=2`, `preset=3` from its own state. So the hint was written,
read by the add-on, echoed in its receipt, and changed not one bit of output on
grained graded film.

Scope, because a zero-difference claim earns no more than it measured: four
presets, one real clip, the shipped mask state only, two repeats, DLSS-NR 310.8.0
with RenoDX 4.7 on Ada at driver 32.0.16.1047. Not measured: mask-off on real
footage, the other three real clips, any other driver or generation. This stays a
property of this runtime on this driver, not a property of the preset hint.

## Cross-check on real footage - `RealCorpus`'s measurement, not mine

This slice's one transferable worry was that the mask state shifts the reference by about as
much as a real effect: between `baseline` and `shipped-defaults` false motion moves by
-0.0093 to +0.0059 on these synthetic clips, and it does not keep the same sign across them.
`RealCorpus` re-ran their gate A/B at the shipped mask state using this report's committed
`shipped-defaults.profile.json` on both of their trees (4 real-footage clips x 2 repeats per
tree, 16 runs, 0 failed, repeats bit-identical, the eight digests differing between trees)
and reported that on real footage the mask barely moves the reference at all: gate-tree false
motion shifts by +0.00002, +0.00004, -0.00058 and -0.00108 between `baseline` and
`shipped-defaults`, one to two orders of magnitude below the synthetic figure and well under
their smallest gate delta. Their gate conclusions survive the mask unchanged (-13.9/-11.4/
-9.4/-4.9 % false motion at mask-on against -14.4/-12.2/-9.5/-4.6 % at mask-off). Source:
`docs/measurements/gate-real-footage-20260914/REPORT.md` on `slice/corpus` (78ed5e4).

I did not observe those runs; they are recorded here because they bound one of my caveats
from the outside, and the bound cuts both ways. The caveat was correct as a caveat - the mask
state has to be checked rather than assumed - and on real material the answer happens to be
benign. The broader lesson is the one this report keeps making: real content and synthetic
patterns do not respond to an art knob by the same amount, so a magnitude measured on
fractals is not a magnitude for footage even when the sign agrees.

## Recommendation

**Keep all eight shipped art defaults as they are.** Specifically:

- `NRAutoMask = 1` (**keep**). This is the only default that differs from the add-on's own,
  so it is the only one that had to be justified. The whole effect is small - at most 0.81 dB
  PSNR and 1.80 dE - and its sign is inconsistent. Stated as mask-on minus mask-off:
  `cuts-similar` +0.81 dB / -1.80 dE (mask-on better on both), `flash-exposure` +0.27 dB /
  -0.41 dE (better on both), `cuts-motion` +0.13 dB / +0.04 dE (a wash), `pan-fast`
  -0.08 dB / +0.27 dE (marginally worse). Mask-off lowers `sigma+` by a further 0.02-0.25 on
  three clips and raises it by 0.08 on `cuts-similar`. No metric here justifies flipping a
  shipped default, and the mask exists to protect content this corpus does not contain
  (faces, skin, text), so the fractal evidence cannot condemn it either.
- `NRPreset = 0` (**keep**, nothing to gain). Bit-identical across `0/1/2/3`, 4 clips, both
  mask states, both repeats. Evidence for the separate open item that the key should stop
  entering the cache identity: it cannot change a pixel here, but it does force re-renders.
- `NRStyle = 0` (**keep**). Both alternatives are far worse on every fidelity metric
  (`natural` -3.3 to -5.8 dB PSNR, +4.4 to +14.8 dE; `cinematic` -3.9 to -5.6 dB, +7.6 to
  +18.2 dE), they raise `false mv` on 6 of 8 clip/style pairs, and `natural` suppresses
  `sigma+` by 1.3-7.0 levels, i.e. it smooths the source rather than stabilising it. They
  also cost 34 MiB of local budget.
- `NRLocalStructure = 1.0` (**keep**). Turning it off buys 0.40-0.67 dB PSNR on three clips
  and loses 0.90 dB on the fourth, worsens dE on three of four (up to +2.94), and clearly
  costs temporal stability: `flicker+` rises by 0.28-0.87 and `sigma+` by 0.33-0.68, with
  `pan-fast` crossing from -0.813 (smoother than source) to +0.052 (adding flicker). The
  default is doing work a metric can see.
- `NRIntensity = 1.0` (**keep**). `intensity-0` is the carrier floor control, not a
  candidate: it is what the pipeline scores when the model does nothing (32.4-38.5 dB PSNR,
  1.35-6.06 dE, `false mv` 0.045-0.112), and it is the correct denominator for reading the
  rest of the table.
- `NRLocalTone = 1.0` (**keep** - this was the candidate, and real footage withdrew it). On
  the synthetic clips it is the only knob whose numbers point anywhere: monotone, 0.5 recovers
  2.0-3.9 dB PSNR and 2.6-5.7 dE, 0.0 recovers 4.6-7.7 dB and 4.0-8.1 dE, with SSIM within
  0.005 and both temporal metrics within 0.31 levels. On `real-film-cuts` (Table F) the whole
  1.0 -> 0.0 range is 0.77 dE and +3.70 dB, against a 1.26 dE carrier floor that accounts for
  half of the 2.48 dE the shipped defaults score - so the effect on offer is the same size as
  the encoder's own error, and nothing temporal moves at all (flicker+ spans 0.019, sigma+
  0.041, cut P/R/F1 and history resets identical across the sweep). A sub-dE, metric-
  indistinguishable-from-the-carrier gain does not justify changing a shipped default, and
  the synthetic 4-8 dE that made it look like one was an artefact of fractal content.
  Only a human blind A/B could overturn a look decision this small; the pairs exist
  (`blind.py`), nobody judged them, and I did not invent a verdict. If anyone does run it,
  `real-film-cuts` is the clip and `shipped-defaults` versus `shipped-tone-050` is the pair.

**Scope of every claim above:** four synthetic 1080p30 clips (`cuts-motion`, `cuts-similar`,
`flash-exposure`, `pan-fast`) plus one real graded film clip (`real-film-cuts`, 102 frames,
used only for the tone question), one GPU (RTX 4080 SUPER, Ada, driver 32.0.16.1047), one
runtime (DLSS-NR 310.8.0 / RenoDX 4.7), NVENC HEVC carrier, no faces, no text, no human
judgement. The bit-identity results (mask, presets, explicit writes) are properties of the
runtime and transfer as stated. The metric *rankings* hold on the material measured; the
metric *magnitudes* demonstrably do not transfer between synthetic and real content - the
tone knob moved 5-11x less on film than on fractals, and `RealCorpus` measured the mask
moving 1-2 orders of magnitude less. The shape all three of this wave's instances agree on,
in `RealCorpus`'s formulation: the synthetic corpus preserves the **sign** of an effect and
systematically **inflates its magnitude**, which is what you would expect of clips built
adversarial on purpose - their cut scores move the same way seen from the other end
(precision/recall 0.571/0.571 synthetic against 1.000/0.800 real). So synthetic patterns are
sound for deciding direction and for regression-gating, and unsound for sizing an effect or
for tuning a threshold on pooled numbers. Nobody should retune an art default on a pooled
synthetic-plus-real set.

## UNEXERCISED

- **Real footage, beyond the tone question on one clip.** `NRLocalTone` was re-measured on
  `real-film-cuts` (Table F) and the mask's own reference shift was measured on four real
  clips by `RealCorpus`. Everything else - the mask comparison itself, `NRLocalStructure`,
  both `NRStyle` values, the preset bit-identity, the carrier-floor share - rests on the four
  synthetic clips only. Given that the tone magnitude shrank 5-11x on film, the style and
  structure magnitudes should be assumed not to transfer either; their *signs* are the part
  worth carrying forward. The remaining real clips (`real-game-cuts`, `real-game-motion`,
  `real-dissolve`) were never rendered under any art profile.
- **Blind human judgement.** Pairs, sealed key and ballot exist
  (`build-upscaling/benchmark-work/blind/`); no human filled them, so there is no verdict.
  `blind.py --score` on the empty ballot returns zeros and is reported as such.
- **`NRSkinStructure` (shipped `-1.000000`) and `NRColorStrength` (shipped `1.000000`).**
  No `ABLATION` row exists for either, and writing them explicitly at their shipped values
  is bit-identical to writing nothing, which proves only that the add-on's defaults agree -
  not what a different value would do. Both are skin/colour controls and this corpus has no
  faces and no memory colours, so sweeping them here would produce numbers with no bearing
  on the decision. Needs the `faces` clip (its fixture lives only in the main checkout).
- **Intermediate values of the other knobs.** Only `NRLocalTone` was swept (1.0/0.75/0.5/0.0).
  `NRLocalStructure` and `NRIntensity` were measured at 1.0 and 0.0 only; no intermediate
  point was rendered, so "keep 1.0" for those rests on two-point evidence.
- **`NRPreset` on other runtimes.** Preset inertness is a statement about DLSS-NR 310.8.0 +
  RenoDX 4.7 + Ada on driver 32.0.16.1047. Another NR model version could give the hint
  meaning.
- **Timing.** Contended throughout; no citable cost number for any knob. The VRAM receipts
  are process-local and consistent but were also taken on a shared box.
- **OCR and face metrics.** Disabled (`--no-ocr --no-faces`); the corpus carries no text
  clip and no faces clip, so text legibility and identity preservation under these knobs are
  unmeasured. `docs/BENCHMARK.md` records an 11-point OCR loss for a second pass, which is a
  different question from art defaults.
- **Two-pass and the guide knobs.** Out of scope here by assignment (`mv-off`, `depth-off`,
  `two-pass`); `DepthAB` owns the guide A/B this wave.
