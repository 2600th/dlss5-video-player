# Architecture

## High-level pipeline

```text
Video file
  |
  v
FFmpeg / Media Foundation fallback
  |
  +---------------------> audio -> waveOut -> playback clock
  |
  v
BGRA decoded video frame
  |
  +-> temporal analysis (current + previous frame)
  |      |
  |      +-> compact motion/depth/uncertainty guide grid
  |              |
  |              v
  |          GPU expansion
  |              |
  |              +-> RG16F motion vectors
  |              +-> D32/R32 depth
  |              +-> R8 temporal mask
  |
  v
sRGB -> linear FP16 color
  |
  v
NVIDIA NGX DLSS Super Resolution
  |
  v
FP16 reconstructed output
  |
  +-> final image adjustments
  |
  v
D3D12 swapchain -> ReShade -> display
```

## Decoder

`VideoDecoder` uses FFmpeg as the primary decoder by launching `ffmpeg.exe`/`ffprobe.exe` as helper processes. Media Foundation is kept as a fallback path.

The decoder can request a lower decode size for high-resolution material so the CPU does not always move native 4K BGRA frames when DLSS is rendering from a smaller input resolution.

## Timing

Audio is the preferred master clock. The video side checks decoded timestamps against that clock. Frames that are too late are discarded and temporal history is reset rather than slowing playback.

## Temporal guides

A normal movie does not contain engine motion vectors or depth. `TemporalGuideGenerator` reconstructs approximate guides from image history:

- block/optical-flow-style temporal matching for current-to-previous motion;
- image/motion cues for a stabilized depth proxy;
- correspondence uncertainty for the temporal mask;
- scene-cut detection for history resets.

CPU analysis is performed on a compact grid. D3D12 expands the result to the exact DLSS render dimensions.

## D3D12 renderer

`D3D12Renderer` owns:

- device / queue / swapchain;
- three command allocators and command lists;
- per-frame video/guide upload resources;
- linear FP16 DLSS color input;
- typeless depth resource with DSV/SRV views;
- motion-vector and mask resources;
- DLSS output UAV;
- final presentation/debug pipelines.

Normal playback does not flush the GPU every frame. Fence waits happen only when a frame slot is reused before completion or during operations that require a hard synchronization point such as seek/reinitialization.

During neural pre-render, `RenderFrameForCache` copies the evaluated output to a
dedicated readback resource and emits tightly packed BGRA frames to a bounded
FFmpeg encoder process. The same persistent NGX/feature-18 session is retained
across the sequence; an add-on-requested feature recreation does not break the
job's monotonic successful-submission count.

## Offline neural job and cache

`OfflineNeuralRenderer` decodes sequentially from frame zero, primes feature
18, restarts the source from zero, rejects non-monotonic timestamps, evaluates
and captures every frame, and finishes the encoder.
If NVENC cannot start or write, the entire sequence restarts from zero with
software H.264 rather than splicing incompatible temporal histories.

`NeuralCacheManager` stages source and render artifacts under LocalAppData.
Source, application version, GPU path, runtime digest, native dimensions,
quality, and upscaling state form the render identity. Network source entries
use the canonical YouTube video ID plus stable selected-format `itag` values,
not expiring signed stream URLs. Staging entries become reusable only after
independent probing and atomic promotion. Schema 3 requires
`nativeEvaluations == verifiedNeuralFrames == frameCount`, the NGX-only inline
interception contract armed before frame capture, a feature-18 success
checkpoint that advances after the captured sequence, and no feature-18
failure, skip, or pass-through marker in the stabilized job log segment.
Sequential offline decoding uses software FFmpeg to avoid competing with the
D3D12 neural and NVENC workloads; playback still prefers hardware decode. Cache
hits are re-probed through the final frame and
quarantined if decoding or metadata validation fails. Cancellation and failed
validation can never publish a partial render.

`SynchronizedPlayback` opens the original and neural files together, validates
their geometry/rate/duration, and publishes timestamp-matched frame pairs.
Original is the initial view. The DLSS toggle changes the visible member of the
last-presented pair, so comparison never advances ahead of the audio clock.

## NGX integration

`DLSSBackend` initializes NGX, queries DLSS settings, creates the Super Sampling feature and evaluates it through the D3D12 `_C` entry point used by NVIDIA's helper path.

The default performance/quality value is DLAA. That keeps input and output at
native 1:1 resolution while preserving a real NGX feature creation/evaluation
sequence for the optional interception layer. Spatial DLSS Super Resolution is
enabled only when the user explicitly selects Auto, Quality, Balanced,
Performance, or Ultra Performance.

The source tree directly implements native DLSS Super Resolution, not an
official public DLSS 5 API. It intentionally leaves the raw NGX symbols visible
so the separately supplied experimental RenoDX/ReShade DLSS 5 add-on can
intercept real feature creation and evaluation calls. Successful native NGX
evaluation therefore does not prove that the neural add-on loaded or evaluated.

RenoDX 4.70 can create and evaluate feature 18 inline after observing the
player's DLSS/DLAA contract. Bootstrap explicitly enables its hooks and neural
uplift while leaving `NREnableUpscaling=0`. The player does not also instantiate
a direct feature-18 bridge: that would duplicate the neural pass and require an
additional undocumented NGX/caller-shim lifetime beside the existing add-on.

## Final image adjustments

Brightness, contrast, saturation, gamma, temperature and tint are applied in the final presentation shader after DLSS. This has two useful properties:

1. Changing display appearance does not invalidate temporal guides or require DLSS history resets.
2. Diagnostic DLSS input/motion/depth/mask views remain unmodified.

When video is paused, adjustment changes re-present the existing DLSS output instead of decoding or reevaluating the movie frame.

## Paused-frame presentation

A frozen video frame is re-presented at a lightweight cadence while paused. This is intentionally separate from video decoding and NGX evaluation. It keeps ReShade's overlay/render loop responsive without advancing the movie or DLSS temporal history.
