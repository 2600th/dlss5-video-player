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

## NGX integration

`DLSSBackend` initializes NGX, queries DLSS settings, creates the Super Sampling feature and evaluates it through the D3D12 `_C` entry point used by NVIDIA's helper path.

The integration intentionally leaves the raw NGX symbols visible to make interception by ReShade/RenoDX possible.

## Final image adjustments

Brightness, contrast, saturation, gamma, temperature and tint are applied in the final presentation shader after DLSS. This has two useful properties:

1. Changing display appearance does not invalidate temporal guides or require DLSS history resets.
2. Diagnostic DLSS input/motion/depth/mask views remain unmodified.

When video is paused, adjustment changes re-present the existing DLSS output instead of decoding or reevaluating the movie frame.

## Paused-frame presentation

A frozen video frame is re-presented at a lightweight cadence while paused. This is intentionally separate from video decoding and NGX evaluation. It keeps ReShade's overlay/render loop responsive without advancing the movie or DLSS temporal history.
