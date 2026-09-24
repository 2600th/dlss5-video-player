# Direct NVENC encoding from D3D12 (P3.7) - 24 September 2026, RTX 4080 SUPER

**What shipped.** A neural render whose capture is NV12 or P010 - the High rung,
and the Standard rung with GPU colour conversion on - is now encoded by NVENC
straight from the D3D12 capture. The capture copies its two planes into an
NV12/P010 surface that NVENC's D3D12 interface reads, fenced on the capture
itself; nothing is read back and nothing goes through a pipe. The packets go to
a minimal Matroska hand-off file that ffmpeg stream-copies into the cache file,
so FFmpeg's own muxer still lays out, indexes and tags it. The ffmpeg child
remains for everything else and as the fallback.

**The output is bit-identical to the ffmpeg `hevc_nvenc` path**, packet for
packet with the same timestamps and the same codec private data, so the direct
path shares the child's cache key and needed no VMAF proof. Identity was checked
at the encoder at five geometries, five frame rates and four presets, and inside
real feature-18 renders on both rungs; the last two are registered GPU tests.

**Throughput moved a little, CPU moved more.** The render loop is 3-4 % faster
at 4K and 6-7 % at 1080p on the rungs the path serves, and the helper with its
children uses 5-19 % less CPU per render. 4K30 is still not real time: at 4K the
loop is GPU-bound on the neural pass and the carrier (neural GPU time alone is
18.5 ms of a 37.6 ms frame), which the encode path was no longer the long pole
of after P2.7. Standard's default BGRA capture keeps the child and is unchanged.

**The render report (P2.11) is kept, and High renders gain one.** A direct
capture reads back only the rows the report's sample grid touches (about 0.7 MB
a frame at 1080p NV12, against 3.1 MB for the frame) and the report is computed
from them by the same sampler, with the same numbers as the encoder child's
render of the same frames. The sampler now reads P010 as well, so a High render,
which used to report no metrics at all, is measured on both paths.

## Environment

| Item | Value |
|---|---|
| GPU / driver | NVIDIA GeForce RTX 4080 SUPER, driver 610.47 (`32.0.16.1047`), NVENC API 13.1 |
| CPU / OS | Intel Core i7-14700, Windows 11 Pro 10.0.26200 |
| FFmpeg | 9.0.1 essentials (gyan.dev), `Loaded Nvenc version 13.1` |
| Header | `external/nvenc/nvEncodeAPI.h`, nv-codec-headers `n13.1.15.0` (`0a6fba9`), SHA-256 `8776fddcb8febc6aec4d73989b1f21831eb30306bc583da55b4bf0c14a1dc228` |
| Tree | `wip/w5-nvenc` at the commit that adds this report |

The machine was shared with other implementers' builds while these ran, so every
throughput figure is a median of three and single outliers are left visible in
the raw files.

## Identity

The contract: a file the direct path writes carries exactly the packets the
encoder child writes for the same frames and settings. The test is `framemd5`
of a stream copy of each file - one line per packet with its dts, pts, duration,
size and MD5, plus the codec private data's size and MD5 - minus only the line
naming the muxing library.

How it is reached (`src/NvencDirectPolicy.h`): the session is configured as
FFmpeg's `nvenc.c` configures `hevc_nvenc` for `BuildEncoderArguments`' options,
field by field and in its order - the preset's configuration for pN/hq, then
g=250 with the preset's B-frames kept, multipass off, VBR with the initial QPs
26/21/34 FFmpeg derives from the codec's default quant factors, constant quality
in 8.8 fixed point with the average rate and buffer discarded and the 800M
ceiling kept, one slice, BT.709 limited-range VUI, picture timing SEI, parameter
sets out of band (Matroska's global header), auto split-frame encoding - and at
the frame rate FFmpeg itself parses out of the `-framerate` text the child is
given. That last one is not the fraction it looks like: the shipped ffmpeg reads
`29.97003` as 979001/32666 and `23.976024` as 991001/41333, and frame 12 of the
latter is stamped 500 ms where 24000/1001 would say 501, so the direct path runs
the same `av_d2q` rather than trusting a rational of its own. Both are pinned by
`nvenc_direct_rates_are_the_ones_the_encoder_child_parses_test`.

**At the encoder** (`MediaGpuSmoke --nvenc-direct-identity`, registered as
`NvencDirectIdentitySmoke` at 1280x720): a `testsrc2` clip as raw NV12 and raw
P010 fed once through `RawVideoEncoder` - the child and arguments a render
uses - and once uploaded into the direct path's surfaces.

| Geometry | Rate | Preset | Frames | Standard, NV12 | High, P010 |
|---|---|---|---:|---|---|
| 1280x720 | 30 | p5 | 60 | identical | identical |
| 3840x2160 | 23.976 | p5 | 40 | identical | identical |
| 1920x1080 | 29.97 | p7 | 90 | identical | identical |
| 1920x1080 | 60 | p1 | 60 | identical | identical |
| 2560x1440 | 24 | p6 | 50 | identical | identical |

**In a render** (`NeuralRangeRenderSmoke --direct-encode-identity`, registered
as `NeuralDirectEncodeSmoke`): a 2 s 1280x720 clip rendered through feature 18
with `--encoder-path direct` and with `--encoder-path ffmpeg`, on each rung: 60
verified neural frames each way and identical packets on both rungs. Neural
renders are bit-identical run to run, so the encoder is the only thing the two
files could differ in. The same test holds the two renders' reports to each
other: on this run both paths measured all 60 frames and agreed on every value
(Standard-NV12: output warping error 1.930967, output sigma 7.445548, luma shift
0.859313, colour delta 8.673564; High-P010: 1.920468, 7.444235, 0.901375,
8.667617).

## Throughput, CPU and memory

`NeuralRangeRenderSmoke --encoder-path-cost WxH 10 3`: a 10 s `testsrc2` clip
at 30 fps rendered whole, three times, in five arms, on the final code (report
rows read back, High measured). **Before** is the encoder
child, which is the pre-change path unchanged (`--encoder-path ffmpeg`);
**after** is the direct path (`--encoder-path direct`). *Loop* is the render
loop's own steady-state rate from the helper's stage line (`measured loop`),
which is the throughput; *wall* is frames over the whole helper run, with
start-up, NGX, feature arming and the mux included; *CPU* is the helper and
every child it started, through a job object. Raw lines:
[`runs-1080p.txt`](runs-1080p.txt), [`runs-2160p.txt`](runs-2160p.txt); an
earlier session before the report rows were added
([`runs-1080p-session1.txt`](runs-1080p-session1.txt),
[`runs-2160p-session1.txt`](runs-2160p-session1.txt)) measured the same shape,
a little more in the direct path's favour at 4K (39.3 -> 37.3 ms NV12, 39.6 ->
37.4 ms P010), which is within the machine's session-to-session spread.

1920x1080, 300 frames, medians of three:

| Arm | Loop ms (fps) | Wall fps | CPU s per render |
|---|---:|---:|---:|
| Standard, BGRA capture (default; always the child) | 12.15 (82.3) | 33.3 | 14.5 |
| Standard, NV12 capture - before | 12.15 (82.3) | 34.0 | 10.7 |
| Standard, NV12 capture - **after** | **11.32 (88.3)** | 34.5 | **9.8** |
| High, P010 capture - before | 12.17 (82.2) | 33.7 | 12.5 |
| High, P010 capture - **after** | **11.43 (87.5)** | 34.2 | **10.5** |

3840x2160, 300 frames, medians of three:

| Arm | Loop ms (fps) | Wall fps | CPU s per render |
|---|---:|---:|---:|
| Standard, BGRA capture (default; always the child) | 40.28 (24.8) | 16.9 | 46.7 |
| Standard, NV12 capture - before | 38.69 (25.8) | 17.4 | 31.1 |
| Standard, NV12 capture - **after** | **37.60 (26.6)** | 17.7 | **25.2** |
| High, P010 capture - before | 39.28 (25.5) | 17.2 | 31.7 |
| High, P010 capture - **after** | **37.58 (26.6)** | 17.6 | **30.1** |

Single runs sat far off the other two (1080p Standard-NV12-before repeat 1 at
14.65 ms, 4K BGRA repeat 2 at 47.25 ms, and in session 1 a 4K direct run at
62.5 ms); each coincides with other work on the shared machine, the medians are
unaffected, and the CPU column, which that work also inflates, is the noisiest.

Where the 4K frame goes (High, before and after, from the stage lines): the loop
is GPU-bound - the render thread waits 17-27 ms a frame for its frame slot, and
the neural evaluate alone takes 18.5 ms of GPU time - and the capture's readback
copy on the direct queue, about 2 ms of it, is what the direct path removed. The
readback's CPU side (the copy worker waited 28-30 ms a frame behind the fence,
overlapped) and the child's pipe and upload are what the CPU column lost.

**Memory** ([`vram-2160p.txt`](vram-2160p.txt), whole-GPU `memory.used` sampled
every 250 ms across one 4K render per arm, session 1): the direct path holds 11 encode
surfaces (274 MB at 4K P010) on the helper's device and drops the encoder child's
CUDA context and its own input surfaces. Peak over idle: High 4025 MiB before,
3723 after; Standard-NV12 3689 before, 3449 after. The helper alone reports
315 MiB more (2495 against 2810 MiB post-job), because the child's share is no
longer in another process.

## What the direct path does not cover, and why

- **Standard's default capture, BGRA.** The child converts it to 4:2:0 with
  swscale on the CPU, a filter NVENC's internal RGB conversion does not
  reproduce, so the direct path could not write the same pixels; the GPU
  conversion that would feed it NV12 is off by default on a measured quality
  trade (`docs/measurements/gpu-readback-20260914/`). If that default is ever
  flipped, the Standard rung joins the direct path with no further change.
- **Segmented live output.** Live sessions write one file per 2 s segment
  through `SegmentWriter`, whose warm-start machinery exists to hide one ffmpeg
  spawn per file; direct sessions per segment are a separate change. Every cache
  render and export is single-file and takes the direct path.
- **FFV1 (Lossless) and libx264**, which are CPU encoders.

## The render report

The report's metrics (`TemporalMetrics.h`) average four point samples per cell of
the guide generator's grid, so a frame contributes only the rows those samples
sit on - two per grid row, a few hundred of the frame's rows. A direct capture
copies exactly those rows of its luma and chroma planes into its readback slot,
one row per `CopyTextureRegion`, at the places a whole-frame copy would have put
them; the copy worker waits on the capture fence, as it does for a readback, and
appends them to the token. The job reads them through `SampleRowPayload`, which
is the same `SampleRows` loop the whole-frame `Sample` runs, over row accessors,
so the definition cannot drift between the paths; a row list that does not match
the grid is refused rather than misread. `FrameIdentityTests` holds the rows and
P010 to the whole NV12 frame's plane exactly, and `NeuralDirectEncodeSmoke` holds
a direct render's report to the child's (tolerance 1e-6, both measured).

P010 is read on the NV12 scale: the 10-bit code over four is the 8-bit code it
refines, so the report stays "in 8-bit codes" and an 8-bit value carried at 10
bits reports exactly what it did at 8. Until now a High render's capture was
handed to the sampler as BGRA, failed its size check, and reported `null`.

## Fallback

`NeuralWorker.log` says which encoder each render used
(`Neural render encoder: NVENC direct from the D3D12 capture.` or `the ffmpeg
child (<reason>)`). The child takes over automatically when the driver's NVENC
API is older than 13.1, when `nvEncodeAPI64.dll` is missing or fails its
catalog-signature check, when the direct session does not start, and -
rendering the attempt again from its preroll - when it fails mid-render; only a
child failure after that falls back further, to libx264, as before.
`--encoder-path direct|ffmpeg` on a helper's command line pins one path for the
benchmark and the tests: `direct` fails the job rather than fall back.

## Reproduce

```
MediaGpuSmoke --nvenc-direct-identity <ffmpeg-dir> <out-dir> [WxH] [frames] [fps] [preset]
NeuralRangeRenderSmoke <ffmpeg-dir> <NeuralWorker.exe> <out-dir> --direct-encode-identity
NeuralRangeRenderSmoke <ffmpeg-dir> <NeuralWorker.exe> <out-dir> --encoder-path-cost 3840x2160 10 3
ctest -C Release -R "NvencDirectIdentitySmoke|NeuralDirectEncodeSmoke"
```
