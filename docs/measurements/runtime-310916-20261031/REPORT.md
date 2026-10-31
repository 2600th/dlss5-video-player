# M1.2 — DLSS-NR 310.9.16, its two networks, RenoDX 10.8.0 (P3.14, P3.5 RTX 50 half)

Branch `release-0.30/m1.2` (merged with release/0.30.0 at d753bd7). Machine: one RTX 5090,
driver 621.01, Windows 11. Other agents ran ci/quick builds during the session (GPU 9-16 % busy);
timings are 3 interleaved repetitions, spread reported, processing fps provisional.

## Inputs

Four approved archives from github.com/RankFTW/rhi-repo releases, SHA-256 verified before
extraction, each in its own empty directory, nothing run from them:
`dlssnr-310.9.16`, `renodx-dlss5-10.8.0`, `streamline-2.14.16.0-v2`, `dlss-310.9.16`.

| File | Version | Signature |
|---|---|---|
| nvngx_dlssnr.dll (mirror) | 310.9.16.0 | NotSigned — modified (`.nrcubin` section, sm_75/86/89 kernels) |
| nvngx_dlssnr.dll (previous) | 310.8.2.0 (310.8.SF-v2) | NotSigned |
| nvngx_dlss.dll | 310.9.16 | Valid (NVIDIA) |
| sl.* (7 files) | 2.14.16 | Valid (NVIDIA) |
| sl.dlss_nr.dll | 2.13 (kept; absent from 2.14.16 zip) | Valid (NVIDIA) |
| RenoDX DLSS5 add-on | 10.8.0 | unsigned community build |

Both NR DLLs declare a minimum driver of 615.00; the docs still say 610.47 (not changed here).

## What a preset does

- 310.9.16: preset 1 = network A, byte-identical to 310.8.SF-v2 on all 5 clips and every look
  (digests.csv; looks.csv ident=5). Presets 0, 2, 3 are identical to each other = network B.
- 310.8.SF-v2: every preset value renders the same.
- The player sends 1 (A) or 2 (B), never 0.

## RenoDX 10.8.0 against 6.5.3 (both on 310.8.SF-v2)

| Config | Distance from 6.5.3 |
|---|---|
| 10.8.0, NRDetailStability=1, NRSkinIndependent=0 | 0.000-0.101 levels; byte-identical on 1 of 5 clips |
| 10.8.0 as written (unpinned) | 0.188-0.390 levels |

Colour strength 0 is refused by 10.8.0 ("every pass at zero strength"); the player writes at
least 0.01. On 6.5.3, 0.01 vs 0: 0.036-0.084 levels, distance share 1.000-1.007.
10.8.0 writes `RenoDX-DLSS5-crash.log` (a phase journal) beside itself every session.

## B against A

B vs A: 1.29-2.63 levels; knobs distance 76-93 % of A's (0.757-0.934 share).

| Profile | VMAF v1 | VMAF v1 p5 | ColorVideoVDP JOD | added | lost share | lost mv |
|---|---|---|---|---|---|---|
| base (6.5.3) | 78.77 | 71.51 | 8.735 | 0.760 | 0.140 | 0.205 |
| A | 78.76 | 71.52 | 8.735 | 0.759 | 0.140 | 0.206 |
| B | 82.06 | 74.85 | 8.897 | 0.708 | 0.130 | 0.198 |

B is a milder picture nearer the source. A stays the default (quality first); B is an opt-in rung.

## Pace (RTX 5090, NVENC rung, 3 interleaved reps)

| Config | 1080p model ms p50 (rep means) | 4K model ms p50 (rep means) | Peak VRAM 1080p / 4K MiB |
|---|---|---|---|
| A (310.9.16) | 3.78 (3.75/3.77/3.82) | 9.63 (9.57-9.72) | 1100 / 2645 |
| B (310.9.16) | 2.42 (2.40-2.43) | 6.04 (5.89-6.13) | 1034 / 2578 |
| 310.8.SF-v2 on 10.8.0 | 3.67 | 10.07 | 1162 / 2847 |
| base 6.5.3 | 3.62 | 9.82 | 1178 / 2911 |

Processing fps 113-155 (noisy, shared GPU): provisional.

## Fade

No darkening on any runtime: render-vs-control luma follows the source's own change
(e.g. B −1.24 vs source −1.24, frames 0-29). No SceneCut / history-reset change made.

## Player flow (`--render`, starting from a 0.28-stamped [RenoDX.DLSS5] section)

- Section restamped ConfigVersion=10 / ConfigRevision=1; no `v10-backup` written.
- A digest 5d6d2e70 = previous runtime's digest; B digest c6c30af9; both match the benchmark.
- Export log: `nvngx_dlssnr.dll (DLSS-NR 310.9.16.0, NotSigned)` and
  `310.8.SF-v2/nvngx_dlssnr.dll (DLSS-NR 310.8.2.0, NotSigned)`.
- Preflight JSON carries `dlssnrFile` and `dlssnrSignature`.

## Decisions

1. Default network A (identical to 0.28); B opt-in, labelled with its measured cost and look.
2. 310.9.16 only on RTX 50 (the only generation measured); other GPUs keep 310.8.SF-v2.
3. Pin NRDetailStability=1, NRSkinIndependent=0 (keeps 10.8.0 within 0.1 level of 6.5.3).
4. Floor written colour strength at 0.01 (10.8.0 refuses 0).
5. Restamp the player's own 0.28 section rather than let the add-on migrate it.
6. No fade reset (does not reproduce).
7. sl.dlss_nr stays 2.13 (2.14.16 ships none; no sl.* module loads in an NR render).

## Cache

Earlier renders render again once: the runtime digest covers both NR DLLs, and NRRuntimePath /
NRPreset enter the settings digest; the canonical string adds `network=a|b` and ` runtime=previous`.
Known inefficiency: previous runtime + network=b keys differently from network=a with identical pixels.

## Gaps and untested

- NVIDIA's signed DLSS-NR 310.9.16 not available from the approved sources (P3.5 gap).
- RTX 20/30/40 never run with 10.8.0 + 310.8.SF-v2 (no such card here).
- The playback-path preflight refusal of a wrong NR file was not exercised (no DLL that fails to load).
- Windows 10 not tested. Processing fps provisional.
- Package grows roughly 200 MB (new NR zip 192 MB compressed); the "377 MB" figure needs
  the real number at release.

Data: docs/measurements/runtime-310916-20261031/ (timing, fade, yardstick, looks, digests,
config-contract). Harness: tools/benchmark/m12.py.
