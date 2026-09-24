# The flow zero-motion test on the neural path — 24 September 2026, RTX 4080 SUPER

**Verdict: adopted, on by default and a cache-key term (`|mv-zero-test-v1`).** The
fixed motion field the flow engine (NVOFA) reports for two *identical* frames
([sr-quality-20260924](../sr-quality-20260924/REPORT.md)) reaches the neural render
too. On a held frame, the render without the test differs from the render with the
motion guide switched off. With the test on, the render is **byte-identical** to it,
which is the exact answer for content that does not move. Without the test the
model's output creeps over the held frame: its per-pixel temporal sigma p99 is 2.15
luma levels, against 1.41 with the test. Its sharpness against the source climbs
from 1.03x to 1.12x, and its PSNR falls twice as fast. On every NR-processed real
clip and on a 0.5 px/frame pan, the test lowers sigma, flicker or the renderer's own
warp error. PSNR stays within 0.1 dB, colour within 0.02 ΔE, and sharpness is equal
or higher. The one cost is on the synthetic near/far pan: +0.24 ΔE and −0.007 SSIM,
with a picture that changes less per frame than its source.

## What was built

| Piece | Where |
|---|---|
| `D3D12Renderer::SetZeroMotionTest`: the resolve pass's eighth root constant is now `preserveSource \|\| zeroMotionTest`, so SR sessions are unchanged and a DLAA carrier can ask for it | `src/D3D12Renderer.*` |
| The neural evaluator sets it per job from `NeuralZeroMotionTest(override)`; one log line per job says which | `src/OfflineNeuralRenderer.cpp` |
| `kNeuralZeroMotionTest = true` and the key term `\|mv-zero-test-v1`, appended to the render identity in the player | `src/NeuralMotionPolicy.h`, `src/main.cpp` |
| `--zero-motion-test 0\|1`: benchmark-only, taken off the helper's line with the guide-file flags before the shared parser, so the player cannot send it and no cached render is made with an override | `src/GuideFiles.h` |
| Clips `still-hold` (a demo film frame held 60 frames), `pan-slow` (a demo frame panned at exactly 0.5 px/frame) and `pan-slow-fractal` | `tools/benchmark/corpus.py` |
| The A/B profiles and the extra scoring (sharpness over time, luma PSNR at both ends, the worker's warp error, the two arms' byte difference) | `gate.profile.json`, `score.py` in this folder |

## Environment

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 4080 SUPER, driver 32.0.16.1047 (worker preflight) |
| Runtime | ReShade 6.8.0.2155, RenoDX 6.5.3 (add-on 0.2026.917.1432), DLSS-NR 310.8.0, the locked files of the main checkout's staged runtime |
| Worker | 0.25.0, built from this branch; NVOFA hardware flow at 1920x1080 on every render (log: "Motion guide backend: NVOFA hardware flow") |
| Corpus | `corpus.py --clips still-hold pan-slow pan-slow-fractal real-film-cuts real-game-cuts real-game-motion depth-pan depth-subject`, FFmpeg 9.0.1-essentials, FFV1 1920x1080 30 fps |
| Settings | the ten keys the player writes. The `gate-*` arms add `NRNormGovernor=0`, under which every repeat was byte-identical. The `shipped-*` arms leave the add-on's default governor, which is what the player renders |

Arms (`gate.profile.json`): `gate-off` (`--zero-motion-test 0`, what every build before
this one rendered), `gate-on` (`--zero-motion-test 1`, what this build ships), `mv-zero`
(`mv=0`, the motion guide switched off), and `shipped-gate-off`/`shipped-gate-on`
(the same two arms at the default governor). Two repeats of each arm, 80 renders, all
`ok`, every frame neural.

**Determinism.** At `NRNormGovernor=0` every arm repeated byte for byte on every clip.
At the default governor the arms repeated byte for byte on the six real, still and pan
clips, where they also matched their governor-0 twins exactly. They did not repeat on
`depth-pan` and `depth-subject`, as the settings report found. Those two clips are
scored at both governors below, both repeats; the repeats agree to the third decimal.

## Results

`flicker+`, `sigma+`, `false mv`, ΔE, PSNR and SSIM are `analyze.py` (`--sample-every 3`)
against the lossless source: flicker and sigma are output minus source, and negative
means smoother than the source. `sharp` is the output's mean absolute Laplacian over
the source's, every frame. `warp+` is the renderer's own motion-compensated warp error,
output minus source (`TemporalMetrics.h`). "Differ" is the share of output bytes the
two arms disagree on.

| clip | differ | sharp off → on | flicker+ off → on | sigma+ off → on | false mv off → on | warp+ off → on | ΔE off → on | PSNR off → on | SSIM off → on |
|---|---|---|---|---|---|---|---|---|---|
| still-hold | 40.0 % | 1.078 → 1.061 | 0.077 → 0.071 | **0.439 → 0.366** | 0 → 0 | 0.069 → 0.071 | 2.10 → 2.09 | **36.34 → 36.50** | 0.9784 → 0.9795 |
| pan-slow | 54.6 % | 1.086 → 1.093 | 0.080 → 0.063 | 0.416 → 0.376 | 0.0166 → 0.0168 | 0.040 → 0.032 | 2.83 → 2.85 | 32.31 → 32.23 | 0.9755 → 0.9746 |
| pan-slow-fractal | 50.0 % | 0.731 → 0.732 | −0.393 → −0.396 | 0.218 → 0.205 | 0.0207 → 0.0206 | −0.199 → −0.203 | 15.67 → 15.67 | 22.94 → 22.94 | 0.9575 → 0.9574 |
| real-film-cuts | 38.4 % | 1.062 → 1.069 | −0.015 → −0.034 | 0.059 → 0.039 | 0.0111 → 0.0096 | −0.031 → −0.044 | 2.26 → 2.26 | 35.47 → 35.57 | 0.9708 → 0.9710 |
| real-game-cuts | 68.2 % | 0.959 → 0.977 | −0.363 → −0.437 | −0.339 → −0.329 | 0.1363 → 0.1228 | −0.161 → −0.198 | 3.16 → 3.16 | 32.15 → 32.21 | 0.9629 → 0.9624 |
| real-game-motion | 63.5 % | 1.031 → 1.043 | 0.306 → 0.243 | 1.271 → 1.275 | 0.0647 → 0.0590 | 0.087 → 0.064 | 3.09 → 3.11 | 31.24 → 31.26 | 0.9697 → 0.9691 |
| depth-pan | 63.4 % | 0.608 → 0.625 | −1.891 → −2.677 | −5.030 → −5.284 | 0.1773 → 0.1609 | 0.198 → 0.095 | **10.83 → 11.07** | 19.32 → 19.45 | **0.9277 → 0.9211** |
| depth-subject | 45.9 % | 0.801 → 0.803 | −0.809 → −0.799 | −1.665 → −1.647 | 0.0605 → 0.0636 | −0.003 → 0.009 | 13.10 → 13.11 | 24.03 → 24.04 | 0.9630 → 0.9630 |
| depth-pan, shipped governor | 62.4 % | 0.609 → 0.627 | −1.894 → −2.701 | −4.898 → −5.094 | 0.1559 → 0.1279 | 0.139 → 0.013 | 10.83 → 11.09 | 19.34 → 19.47 | 0.9281 → 0.9218 |
| depth-subject, shipped governor | 46.6 % | 0.801 → 0.802 | −0.879 → −0.891 | −1.692 → −1.663 | 0.0383 → 0.0352 | −0.080 → −0.091 | 13.13 → 13.05 | 24.04 → 24.08 | 0.9630 → 0.9632 |

The held frame, over time (luma, every frame; first ten → last ten frames):

| arm | sharp first10 → last10 | PSNR-Y first10 → last10 | sigma p99 |
|---|---|---|---|
| gate-off | 1.027 → **1.117** | 36.93 → **36.20** | **2.15** |
| gate-on | 1.030 → 1.071 | 36.92 → 36.56 | 1.41 |
| mv-zero | 1.030 → 1.071 | 36.92 → 36.56 | 1.41 |

What it says:

1. **The field reaches the model, and the test removes it exactly.** `still-hold` with
   the test is byte-identical to `mv-zero`, whose motion guide is all zero. Without the
   test, 40 % of the output bytes differ from that answer. The model does what SR did
   with the same field, more mildly: it re-reads its history through a sub-pixel shift
   every frame. Here that shows up as sharpening that grows over the clip (1.03 → 1.12
   against the source), a PSNR that falls 0.73 dB in two seconds instead of 0.36, and
   localized shimmer, with sigma p99 up by half. The extra "sharpness" of the off arm
   is drift away from the source, not detail.
2. **Real footage: stability up, fidelity unchanged.** On the three NR-processed
   captures, false motion falls (0.0111 → 0.0096, 0.136 → 0.123, 0.065 → 0.059), and
   warp error falls on all three. Flicker falls on the moving clip (0.306 → 0.243).
   PSNR moves by +0.02 to +0.10 dB, ΔE by at most 0.02, and sharpness rises slightly
   (0.959 → 0.977 on the game cuts).
3. **Slow pans: steadier, marginal on fidelity.** On the 0.5 px/frame real pan, sigma
   (0.416 → 0.376), flicker and warp error all fall. PSNR is 0.08 dB lower and ΔE 0.02
   higher. The fractal pan barely moves: detail at every scale gives the SAD test a
   clear answer, so few vectors tie.
4. **The synthetic near/far pan is the one cost.** On `depth-pan`, ΔE rises 0.24
   (10.83 → 11.07), SSIM falls 0.007 and flicker+ drops further below the source
   (−1.89 → −2.68), while PSNR rises 0.13 dB and warp error halves (0.198 → 0.095).
   Both governors agree. On that clip the model already relights heavily (ΔE ≈ 11 in
   both arms) over flat sky and hills, where zero motion and the engine's vector
   explain the pair equally and the test gives the tie to zero. The interior clip is
   neutral. The ruling weighs the real footage and the held frame, which is the case
   the test exists for, above one synthetic layer pan. The change is +2 % of a ΔE that
   is mostly the model's own.

**Cost.** The neural evaluate's GPU p50 was 5.62-5.65 ms with the test off and
5.68-5.83 ms with it on (+0.06-0.18 ms, 1080p). The test adds 27 texture reads per
pixel to the resolve pass. The guide stage is unchanged (0.83-1.22 ms in both arms).

## Why it is a cache-key term

The test changes the output bytes on every clip (38-68 % of bytes). A render cached
without it must never be served for one made with it, so the render identity gains
`|mv-zero-test-v1`. It is appended after the temporal term, the same way the other
pipeline terms are. The benchmark override never reaches a cached render: it exists
only on the helper's own command line, and the helper refuses it on a resident or
preflight line. `tests/PolicyTests.cpp` pins the shipped value and the term.
`tests/NeuralWorkerTests.cpp` pins the flag's extraction. `UpscalingTests` already
pins the shader's constant slot and input bindings.

## Caveats

- The real clips are NR-processed captures (see `docs/BENCHMARK.md`), not camera
  originals: those need a download this machine may not make. The held frame and the
  slow pan are built from the same capture.
- One render per arm and governor. Every repeat that could be byte-identical was.
- PSNR/SSIM/ΔE are against the source, which a relighting model is expected to move.
  Read them as change magnitudes between the arms, not as quality.

## Reproduce

```
set DLSS_BENCHMARK_BUILD=<build dir with Release\neural-runtime staged>
set DLSS_BENCHMARK_FFMPEG=<dir with ffmpeg.exe, ffprobe.exe>
python tools/benchmark/corpus.py --clips still-hold pan-slow pan-slow-fractal real-film-cuts real-game-cuts real-game-motion depth-pan depth-subject
python tools/benchmark/run.py --profile-file docs/measurements/neural-mv-gate-20260924/gate.profile.json ^
    --profiles gate-off gate-on mv-zero shipped-gate-off shipped-gate-on --repeats 2
python tools/benchmark/analyze.py --no-ocr --no-faces --sample-every 3
python docs/measurements/neural-mv-gate-20260924/score.py
```
