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
D3D12 swapchain -> display (player has no ReShade proxy)
```

## Decoder

`VideoDecoder` uses FFmpeg as the primary decoder by launching `ffmpeg.exe`/`ffprobe.exe` as helper processes. Media Foundation is kept as a fallback path.

Playback preserves the decoded source dimensions. Selecting an SR output never
downsamples a source to fit a nominal DLSS quality ratio.

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
quality, upscaling state, and a canonical neural-settings digest form the render identity.
The settings snapshot is saved beside the video and its hash is checked on reuse.
Settings are checked again after rendering before publication. Network source entries
use the canonical YouTube video ID plus stable selected-format `itag` values,
not expiring signed stream URLs. Staging entries become reusable only after
independent probing and atomic promotion. Schema 3 requires
`nativeEvaluations == verifiedNeuralFrames == frameCount`, the NGX-only inline
interception contract armed before frame capture, a feature-18 success
checkpoint that advances after the captured sequence, and no feature-18
failure, skip, or pass-through marker in the stabilized job log segment.
Sequential offline decoding uses software FFmpeg to avoid competing with the
D3D12 neural and NVENC workloads; playback still prefers hardware decode. Cache
hits retain full content-hash verification and use header-only metadata probes;
frame counting and final-frame decoding run once before promotion, not on every
replay. Invalid metadata is quarantined. Cancellation and failed
validation can never publish a partial render.

`RecentMediaHistory` atomically persists five distinct sources and their current
cache keys. Displaced keys are removed only when unreferenced by that history and
no active job/export can own them. Local originals are never removal targets.
The cache root is resolved through a temporary delete-on-close file before bucket
creation, so inherited Windows package redirection cannot split the ownership root
from newly written children. Descendant and reparse-point checks remain in force.

`CachedVideoExporter` stream-copies the validated neural video and source audio,
compatible subtitles, attachments, metadata and chapters into a new MKV. An owned,
cancellable FFmpeg process writes a unique sibling stage, published without
overwriting an existing destination. Export has no render or subtitle-composition
pass. Preferences use the existing executable-adjacent INI.

`SynchronizedPlayback` opens the original and neural files together, validates
their geometry/rate/duration, and publishes timestamp-matched frame pairs.
Neural Rendering is requested on for a new session, so a valid cached replay
selects the neural member after the first pair is ready and before it is
presented. Toggling Neural Rendering changes the visible member of the
last-presented pair, so comparison never advances ahead of the audio clock.
Seeking waits for both restarted decoders to produce a pair; temporary
`NotReady` results do not unload playback. Tail seeks account for container
duration padding with a bounded earlier-frame retry.

## NGX integration

`DLSSBackend` initializes NGX, queries DLSS settings, creates the Super Sampling feature and evaluates it through the D3D12 `_C` entry point used by NVIDIA's helper path.

The default performance/quality value is DLAA. That keeps input and output at
native 1:1 resolution while preserving a real NGX feature creation/evaluation
sequence for the optional interception layer in `neural-runtime/NeuralWorker.exe`.
The main player does not load that proxy. Its independent runtime SR toggle
defaults off, selects a supported NGX input range without resizing the source,
and targets a 2560x1440 or 3840x2160 bounding box. It validates a candidate
renderer on a separate child window before swapping; failure preserves playback.
Ordinary playback disables sampling jitter. Frame Generation is unavailable.
These controls do not alter the offline DLAA carrier or cache identity.

Cache misses invoke a hidden, job-owned helper through a versioned metadata pipe.
Only paths and progress/results cross processes; encoded videos remain in the
existing cache. The helper enters DXGI on its main thread before Media Foundation
and decoder startup, then maintains a hidden window/message pump during rendering.
The cache manager still checks hashes, geometry, timeline and feature-18 evidence
before promotion. Closing/cancelling the job terminates the helper process tree.

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
