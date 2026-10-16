# The NV12 pipe transport share of a 4K frame (P2.57) — 16 October 2026

**Verdict: small; leave the Parked zero-copy line parked.** Moving one 3840x2160 NV12 frame
through the decoder pipe costs about **2 ms** of copying:

- **2.1 ms** on its own (in-process writer to reader, 5.7-6.0 GB/s)
- **3.2-3.3 ms** with ffmpeg's rawvideo writer in front
- **+1.6 ms** over the same decode into `-f null`

In the real worker it runs on the decoder thread: **pipeRead 1.9-3.2 ms per frame**, and
that thread then sits **20-25 ms per frame waiting for queue space**. The render loop's
read stage is **0.004 ms** (p50), and the 4K frame is **19.2 ms** (p50, against the 22.57
ms `docs/ARCHITECTURE.md` quotes for 0.26.2). The transport is about a tenth of a frame of
CPU on another thread and none of the critical path; an in-process decode-only path
would not make 4K faster on this machine.

BGRA, the player's default source layout, costs 2.7x the transport (5.6-5.8 ms isolated,
+7.0 ms over decode). In the worker it leaves the decoder short rather than ahead
(blockedWait 16-21 ms per frame; the render loop's read stage averages 2.9-11.8 ms). The
4K frame is still 19.6-20.5 ms p50, because the evaluate gate is the limit on an RTX 5090.

## Method

- **Worker renders.** A 4K clip: `real-game-motion` looped three times (228 frames),
  lanczos-scaled to 3840x2160, libx264 CRF 14, untagged. Rendered at Standard
  (`run.py --profile-file build-upscaling/benchmark-work/m1-4k.profile.json`) with the
  player's keys, three times each through the NV12 source pipe
  (`--gpu-source-conversion 1`) and the BGRA one (default). Per-stage numbers come from
  `NeuralWorker.log` ("Decoder frame queue" and "Render stage" lines; `worker-4k-stages.txt`).
- **Isolated transport.** `tools/benchmark/pipe_transport.py` reproduces the decoder's pipe:
  an anonymous pipe sized at two frames, clamped to 4-64 MiB (23.7 MiB for NV12, 63 MiB
  for BGRA), read in whole-frame `readinto` calls of at most 16 MiB into one preallocated
  buffer. Three producers:
  - ffmpeg rewriting a pre-decoded raw file from page cache
  - an in-process writer thread
  - ffmpeg decoding the H.264 clip into the pipe, against the same decode into `-f null`

  300 or 228 frames, three repeats each.

## Environment

RTX 5090, driver 617.14; worker 0.28.2; FFmpeg n9.0.2; the worker's decoder chose CUDA
decode + GPU scale ("FFmpeg raw NV12 process started with CUDA decode + GPU scale").

## Results

Isolated, ms per frame (3 repeats; `pipe_transport.csv`):

| producer | NV12 (12.4 MB) | BGRA (33.2 MB) |
|---|---|---|
| in-process writer, kernel copy only | 2.08-2.19 (5.7-6.0 GB/s) | 5.55-5.78 (5.7-6.0 GB/s) |
| ffmpeg rawvideo from page cache | 3.17-3.32 (3.8-3.9 GB/s) | 9.1-16.3 |
| H.264 decode into pipe vs into null, whole run | 1.17-1.23 s vs 0.80-0.85 s over 228 frames: +1.6 ms/frame | 2.56-2.76 s vs 0.98-1.16 s: +7.0 ms/frame |

Worker, 4K Standard rung (`worker-4k-runs.csv`):

| | NV12 pipe | BGRA pipe |
|---|---|---|
| render-loop throughput p50 | 19.2 / 18.9 / 19.3 ms | 20.5 / 19.6 / 19.7 ms |
| processing fps | 50.6 / 39.5 / 50.7 | 48.4 / 50.1 / 40.2 |
| decoder pipeRead per frame | 1.9 / 3.2 / 2.1 ms | 3.7 / 4.0 / 3.6 ms |
| decoder waiting for queue space | 20.5 / 24.7 / 20.2 ms | 3.5 / 4.2 / 3.6 ms |
| decoder waiting for ffmpeg | 1.1 / 1.1 / 1.0 ms | 17.7 / 15.8 / 21.3 ms |
| render-loop read stage, p50 / mean | 0.004 / 0.9-1.0 ms | 0.005-12.4 / 2.9-11.8 ms |
| model GPU ms p50 | 9.28-9.40 | 8.80-9.27 |
| worker peak local VRAM | 2803 MiB | 2910 MiB |

One run of each layout was slow (about 25 ms per frame), with the time in the guide or
read stage, not in transport.

## Caveats

- One GPU (RTX 5090) and one CPU. On a slower GPU the loop is longer and the transport's
  share smaller; on a slower CPU the copy is longer but still on the decoder thread.
- The 4K source is an upscale, so its decode cost is that of a typical H.264 4K stream,
  not of an HEVC 10-bit one.
- Python's `readinto` stands in for the worker's `ReadFile`; both are one kernel copy
  per call.
