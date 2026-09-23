# Deband pre-pass (P2.14) - 23 September 2026, RTX 4080 SUPER

**Verdict: the neural pass does not amplify the source's banding - it removes most of it -
so a deband in front of the model has almost nothing left to do, and it ships off.** The
seven clips' sources score CAMBI 1.6-10.9 (8-bit, and the three `real-*` ones through an
h264 capture); the model's own output, written losslessly at 10 bits, scores 0.004-1.9.
The pre-pass takes that to 0.000-0.028, below anything visible either way, while moving
the picture measurably on the clips with fine texture or text (VMAF of debanded against
plain 97.3-97.7 there) through its grain and its averaging. Flicker is unchanged to within
0.02 codes. Where banding does appear in this player it is made after the model, by the
Standard rung's 8-bit encode (CAMBI 10.6 on the gradient clip against the model's 0.23),
which a pre-pass cannot reach; the High rung and the capture dither are the remedies for
that (`docs/measurements/cache-quality-20260923/`).

## Environment and instruments

| Item | Value |
|---|---|
| GPU / driver | NVIDIA GeForce RTX 4080 SUPER, driver `32.0.16.1047` |
| Runtime | DLSS-NR 310.8.0, ReShade 6.8.0.2155, RenoDX 6.5.3, worker 0.25.0 |
| Tree | branch `wip/w2-quality` (`2eb9e33` and later) |
| Renders | `tools/benchmark/run.py`, profiles `q-lossless` and `q-lossless-deband` in [`deband.profile.json`](deband.profile.json): the shipped state at the Lossless rung, so the encoder adds nothing to either arm; the second adds `--deband 1` |
| Clips | the three synthetic clips with gradients, fine detail and text, and the four `real-*` clips cut from the h264 demo capture |
| Metrics | CAMBI (FFmpeg 9.0.1 `libvmaf`, `feature=name=cambi`, both inputs at `yuv420p10le`); flicker as mean |Y(t) - Y(t-1)| in 8-bit codes with the manifest's cut frames skipped; VMAF and PSNR-Y of the debanded render against the plain one |

The pre-pass is libplacebo's deband at its defaults (1 iteration, threshold 3, radius 16,
grain 4) in front of the linearisation the model's input goes through, with a fixed
per-pixel pattern instead of libplacebo's per-frame one (`src/DebandPolicy.h`).

## Results

| clip | source CAMBI | output CAMBI plain -> deband | source flicker | output flicker plain -> deband | VMAF deband vs plain | PSNR-Y deband vs plain |
|---|---:|---:|---:|---:|---:|---:|
| highlights-gradients | 10.91 | 0.230 -> 0.010 | 1.343 | 1.224 -> 1.221 | 97.29 | 58.6 dB |
| fine-detail | 3.69 | 0.250 -> 0.028 | 11.180 | 7.136 -> 7.119 | 99.97 | 49.7 dB |
| text-subtitles | 6.74 | 0.611 -> 0.016 | 0.248 | 0.244 -> 0.245 | 97.47 | 58.0 dB |
| real-film-cuts | 8.39 | 1.902 -> 0.017 | 1.291 | 1.272 -> 1.278 | 97.73 | 56.5 dB |
| real-game-motion | 1.55 | 0.004 -> 0.000 | 5.357 | 5.609 -> 5.603 | 99.76 | 57.7 dB |
| real-game-cuts | 3.98 | 0.048 -> 0.001 | 12.135 | 11.954 -> 11.940 | 99.79 | 56.7 dB |
| real-dissolve | 3.08 | 0.027 -> 0.000 | 6.948 | 6.964 -> 6.958 | 99.80 | 55.6 dB |

For comparison, the same clips at the Standard rung (8-bit HEVC) score CAMBI 10.57, 0.87,
7.78, 8.83 and 2.04 on the first five: the banding a viewer sees is the cache encode's.

## Reproducing

```
python tools/benchmark/corpus.py
python tools/benchmark/run.py --profile-file docs/measurements/deband-20260923/deband.profile.json \
    --profiles q-lossless q-lossless-deband \
    --clips highlights-gradients fine-detail text-subtitles real-film-cuts real-game-motion real-game-cuts real-dissolve
```

then score each `output.mkv` with `ffmpeg -i <out> -i <out> -lavfi "[0:v]format=yuv420p10le[d];[1:v]format=yuv420p10le[r];[d][r]libvmaf=feature=name=cambi:log_fmt=json:log_path=c.json" -f null -`.
