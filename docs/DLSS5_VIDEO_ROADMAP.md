# DLSS 5 Video Player — Updated Roadmap

_Current as of September 9, 2026._

## Quick reality check

- DLSS 5 Neural Rendering is officially available on RTX 50-series GPUs, but NVIDIA's public DLSS repository still lists SDK 310.7. Video processing through Feature 18 remains an experimental community workflow. [NVIDIA announcement](https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/) · [DLSS 5 research](https://research.nvidia.com/labs/adlr/DLSS5/) · [Public SDK releases](https://github.com/NVIDIA/DLSS/releases)
- Keep the player's currently tested stack pinned: **driver 616.64 + ReShade 6.8 + RenoDX 4.70 + NR/SR 310.8**. [Runtime lock](../packaging/runtime-lock.json) · [Measured report](measurements/runtime-comparison-20260907/REPORT.md)

## What “2× / 3×” can mean

| Feature | Possible? | Meaning |
|---|---:|---|
| **2–3 Neural Rendering passes** | Yes | Feed each pass's output into another NR evaluation |
| **2–3× resolution** | Yes | DLSS Super Resolution enlarges the frame |
| **2–3× FPS** | Experimental | Frame Generation inserts new frames |
| **Intensity = 2.0** | Yes | Stronger model parameter—not two passes |

## P0 — Build first

> Status (2026-09-08): items 1–7 are implemented in this tree. See
> [Architecture](ARCHITECTURE.md), [Usage](USAGE.md) and the measured guide
> ablation in [Benchmark](BENCHMARK.md). Item 5's "validated segment
> checkpoints" are realized as bounded from-zero relaunches plus exact-frame
> retries; mid-job resume that preserves temporal state remains future work.
>
> Item 6 is now the **open default**: opening a local file or a YouTube URL
> acquires and identifies the source, replays a validated cache entry when one
> exists, and otherwise plays the original. Rendering is always an explicit
> choice (frame, 4 s clip, marked range, whole video).

### 1. Runtime preflight and exact version locking

Before every render, run a short Feature 18 probe and record:

- GPU and driver
- DLL/runtime hashes
- Consumer and ReShade versions
- Every Feature 18 creation result
- Effective settings and output receipt

Do not automatically replace the proven RenoDX stack because a newer community build exists.

**Links:** [RuntimePolicy.cpp](../src/RuntimePolicy.cpp) · [runtime-lock.json](../packaging/runtime-lock.json) · [NVIDIA releases](https://github.com/NVIDIA/DLSS/releases) · [DLSS5 Bridge releases](https://github.com/NIGos/dlss5-bridge/releases) · [DLSS5 Feeder releases](https://github.com/jlrouzies-fr/DLSS5-Feeder/releases)

> Current community status: Bridge **1.4.12** is stable; 1.4.13 builds are prereleases. Feeder's newest listed build is **0.14.0-beta.5**.

### 2. Strong automated quality benchmark

Create a repeatable test set covering:

- Front-facing and profile faces
- Skin and hair
- Small text and subtitles
- Fine moving detail
- Highlights and gradients
- Camera cuts and fast motion

Measure neural time, total FPS, VRAM, temporal flicker, OCR accuracy, face consistency, color shift and deterministic rerenders. Include blind **one-pass versus two-pass** comparisons.

**Links:** [Benchmark script](measurements/runtime-comparison-20260907/benchmark.py) · [Face comparison](measurements/runtime-comparison-20260907/face-comparison.png) · [NVIDIA DLSS 5 research](https://research.nvidia.com/labs/adlr/DLSS5/)

### 3. Prove every guide and control

Ablate these independently:

- Motion vectors
- Depth
- RenoDX automatic mask (the custom mask guide was ablated, proven inert and removed)
- Structure intensity
- Tone intensity
- Render preset and style

Use identical warm-up and history conditions. Do not add expensive guide models until output differences prove they help.

**Links:** [DLSSBackend.cpp](../src/DLSSBackend.cpp) · [TemporalGuides.cpp](../src/TemporalGuides.cpp) · [HECer ComfyUI-DLSS5](https://github.com/HECer/ComfyUI-DLSS5)

### 4. Correct frame identity and temporal resets

Attach these identifiers to every frame, guide and neural result:

- Frame number
- Presentation timestamp
- History-generation ID
- Source/job ID

Reject mismatches. Reset every neural history after cuts, seeks, retries, source changes and worker restarts.

**Links:** [SynchronizedPlayback.cpp](../src/SynchronizedPlayback.cpp) · [TemporalGuides.cpp](../src/TemporalGuides.cpp) · [Neural Coprocessor](https://github.com/maohgad-web/Neural-coprocessor)

### 5. Stall recovery and resumable rendering

Implement explicit states for:

- Temporary GPU stall
- Worker crash
- Device removal
- Retry exhaustion
- User pause/resume

Use exact-frame retries, validated segment checkpoints and bounded recovery. Never silently omit a failed frame from final output.

**Links:** [NeuralWorker.cpp](../src/NeuralWorker.cpp) · [NeuralCache.cpp](../src/NeuralCache.cpp) · [DLSS5 Bridge stall/resume work](https://github.com/NIGos/dlss5-bridge/releases)

### 6. Range selection and preview-first workflow

Add:

- In/Out markers
- Exact timecode entry
- One-frame preview
- 3–5 second preview
- Required temporal pre-roll
- Accurate audio and variable-frame-rate handling

**Links:** [OfflineNeuralRenderer.cpp](../src/OfflineNeuralRenderer.cpp) · [VideoDecoder.cpp](../src/VideoDecoder.cpp) · [FFmpeg seeking documentation](https://ffmpeg.org/ffmpeg-all.html#Main-options) · [Visual Enhancer v7](https://github.com/Merserk/dlss5-visual-enhancer/releases/tag/v7.0)

### 7. Clear strength and comparison controls

Provide:

- Original/Neural blend
- Freeze-frame comparison
- Split view and wipe
- Face/text zoom
- Separate Structure, Tone, Detail and Color controls
- Saved effective settings

The visual blend can be instant, but changing native model parameters may require rerendering.

**Links:** [ReShadeConfig.cpp](../src/ReShadeConfig.cpp) · [D3D12Renderer.cpp](../src/D3D12Renderer.cpp) · [video2dlssnr controls](https://github.com/DaniilSokolyuk/video2dlssnr)

## P1 — High-value quality and performance

> **Status (September 9, 2026).** Item 9 is implemented: acceptance by evidence
> margin, a banded reverse check and a confidence-weighted vector median cut
> false motion on the cuts clip from 60.8 % to 3.7 % and made the guide pass
> cheaper (6.66 → 3.09 ms). Item 10 was measured and abandoned on the NGX path —
> the mask is inert for neural rendering and for DLSS-SR alike, and two of its
> three parameter names belong to Ray Reconstruction ([Benchmark](BENCHMARK.md));
> compositing outside NGX remains open. Item 12's render-ahead half shipped as
> the active session, and its premise changed: the capture no longer swizzles on
> the CPU, and the residual readback is the proportional term of the measured
> 7.35 ms + 2.50 ms/megapixel cost rather than a dominant one. Item 13 was
> measured and rejected (two-pass costs 2.8 dB PSNR and 11 OCR points). Items 8,
> 11 and 14–18 are open; item 15's own precondition has now half-fired, since
> depth measurably changes the output by less than 0.02 dB.

### 8. Source-color and HDR preservation

Preserve:

- Primaries and transfer functions
- Full/limited range
- 10-bit precision
- HDR metadata
- Original chroma when requested

Expose native **Tone Intensity**, including zero, which NVIDIA says preserves the rendered frame's exact colors. Add clipping and color-shift warnings.

**Links:** [NVIDIA DLSS 5 controls](https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/) · [neural-upstream](https://github.com/matiasLombo/neural-upstream) · [MediaPipeline.cpp](../src/MediaPipeline.cpp)

### 9. Confidence-aware optical flow

Compare current motion against:

- NVIDIA Optical Flow Accelerator
- RAFT
- Cheap Lucas–Kanade fallback

Calculate forward/backward disagreement and flow cost. Reject unreliable motion around cuts, occlusions and transparent objects.

**Links:** [NVIDIA Optical Flow SDK](https://github.com/NVIDIA/NVIDIAOpticalFlowSDK) · [video2dlssnr NVOFA pipeline](https://github.com/DaniilSokolyuk/video2dlssnr) · [HECer ComfyUI-DLSS5](https://github.com/HECer/ComfyUI-DLSS5)

### 10. Stable protection masks

**Measured September 8, 2026: the NGX mask inputs are inert.** A mask with
5–24 % non-zero cells and a forced all-ones mask both produced pixel-identical
output on feature 18, preset K and preset Default alike, and mask on versus off
is 0 differing bytes on the DLSS-SR path while motion vectors change 6.85 % of
bytes. `DLSS_Input_Bias_Current_Color_Mask`, `DLSS_DisocclusionMask` and
`DLSS_ResponsivityMask` are Ray Reconstruction inputs. The mask guide was
deleted; any future protection work must composite outside NGX rather than bind
a mask to it.

Support temporally tracked masks for:

- Faces and skin
- People
- Text and subtitles
- Imported user masks
- Regions that must remain unchanged

Feather masks and preferably composite subtitles after neural processing.

**Links:** [MediaPipe Face Landmarker](https://developers.google.com/mediapipe/solutions/vision/face_landmarker) · [NVIDIA semantic/engine masks](https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/) · [TemporalGuides.cpp](../src/TemporalGuides.cpp)

### 11. RTX Video enhancement modes

Expose these as separate operations:

1. **Artifact Reduction** — deblocking/banding cleanup without resizing
2. **Video Super Resolution** — spatial upscale
3. **RTX Video HDR** — SDR-to-HDR conversion

Benchmark:

- Artifact Reduction → Neural Rendering → SR
- Neural Rendering → VSR
- NR only
- RTX Video only

**Links:** [RTX Video SDK 1.1](https://developer.nvidia.com/rtx-video-sdk/getting-started) · [Visual Enhancer v7](https://github.com/Merserk/dlss5-visual-enhancer/releases/tag/v7.0)

### 12. GPU-resident processing and buffered viewing

Reduce CPU readbacks and use bounded GPU queues for decode, neural processing and encode.

Offer two distinct modes:

- **Render-ahead playback:** watch completed segments
- **Experimental live mode:** bounded latency with visible dropped-frame counters

Dropped frames may be acceptable for live preview, never final export.

**Links:** [MediaPipeline.cpp](../src/MediaPipeline.cpp) · [D3D12Renderer.cpp](../src/D3D12Renderer.cpp) · [NVIDIA Video SDK samples](https://github.com/NVIDIA/video-sdk-samples) · [video2dlssnr](https://github.com/DaniilSokolyuk/video2dlssnr)

## P2 — Experiments that must prove value

### 13. Two-pass and three-pass Neural Rendering

Yes, this is possible on video.

Recommended implementation:

```text
Source + guides
  → NR pass 1, independent history
  → NR pass 2, independent history
  → optional NR pass 3, independent history
  → one guarded composition against the original
```

Rules:

- **1 pass:** default
- **2 passes:** experimental preset
- **3 passes:** advanced option
- **More than 3:** research only
- Reduce strength on later passes
- Reset every pass's history together
- Include pass count and per-pass settings in cache keys

Community experiments show extra structure from two passes, but also stronger brightness/color shifts, halos and diminishing returns.

**Links:** [Pre-SR Multipass, 1–3 passes](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) · [THERMOTRON Multipass, 1–4 passes](https://github.com/edgarbatjr/OptiScaler_DLSSNR-THERMOTRON-multipass) · [Two-pass live video demonstration](https://github.com/jpneagle/dlss5-webcam-demo)

### 14. Neural-before-upscale and reduced working resolution

Test this full matrix:

- NR at native resolution
- SR → NR
- Lower-resolution NR → upscale
- Low-resolution neural residual transferred to the original
- Working scale: 50%, 75%, 100%
- Passes: 1, 2, 3

Judge faces, text, shimmer, highlights, speed and VRAM separately.

**Links:** [neural-upstream](https://github.com/matiasLombo/neural-upstream) · [Pre-SR Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) · [OptiScaler DLSS-NR](https://github.com/Dagherbou/OptiScaler_DLSSNR)

### 15. Learned depth and normals

Only implement after guide ablations prove depth affects the active consumer.

Suggested order:

1. Video Depth Anything Small FP16
2. FlashDepth for lower-latency experiments
3. Derived normals
4. DSINE only if normals demonstrably matter

These are estimated guides, not ground-truth scene geometry.

**Links:** [Video Depth Anything](https://github.com/DepthAnything/Video-Depth-Anything) · [FlashDepth](https://github.com/Eyeline-Labs/FlashDepth) · [DSINE](https://github.com/baegwangbin/DSINE) · [HECer depth backend](https://github.com/HECer/ComfyUI-DLSS5)

## P3 — Later expansion

### 16. Frame Generation: 2× and 3× FPS

Treat Frame Generation as a separate pipeline from Neural Rendering.

Validate:

- Scene cuts
- First and last frames
- Generated-frame timestamps
- Audio duration and synchronization
- Long sequences
- One versus two inserted frames

HECer's worker currently provides an experimental **2× smoke test**, not a complete quality validation. A 3× mode requires producing two correctly timed intermediate frames.

**Links:** [DLSSG Stream Worker](https://github.com/HECer/DLSSG-Stream-Worker) · [Worker protocol](https://github.com/HECer/DLSSG-Stream-Worker/blob/main/docs/PROTOCOL.md) · [ComfyUI DLSS Frame Interpolation](https://github.com/Konohamaru04/ComfyUI-NVIDIA-DLSS-Frame-Interpolation)

### 17. Editor, CLI and ComfyUI interoperability

Study or optionally support compatible workflows rather than rebuilding every interface immediately.

**Relevant projects:**

- [video2dlssnr](https://github.com/DaniilSokolyuk/video2dlssnr) — D3D12 CLI/UI/ComfyUI pipeline with NVOFA, scene resets, 10-bit output and color tagging
- [DaVinci Resolve DLSS5](https://github.com/SAOG0721/DaVinci-Resolve-DLSS5) — experimental OpenFX filter
- [ComfyUI-DLSS5-Enhancer](https://github.com/Blueforcer/ComfyUI-DLSS5-Enhancer) — image/video NR nodes
- [DLSS5 Visual Enhancer](https://github.com/Merserk/dlss5-visual-enhancer) — NR, Frame Interpolation and RTX Video workflows
- [HECer ComfyUI-DLSS5](https://github.com/HECer/ComfyUI-DLSS5) — persistent processing, guides and depth

### 18. Alternative adapters, multi-GPU and cross-platform research

Keep outside the critical path until single-GPU processing is reliable.

- Evaluate only one neural consumer at a time
- Isolate RenoDX, Deep Fried Chicken and ShortFuse adapters
- Add explicit dual-consumer detection
- Consider multi-GPU transport later
- Treat model extraction and redistribution as a licensing boundary

**Links:** [DLSS5 Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) · [DLSS5 Bridge](https://github.com/NIGos/dlss5-bridge) · [Neural Coprocessor](https://github.com/maohgad-web/Neural-coprocessor) · [MLX-DLSS research](https://github.com/iamwavecut/MLX-DLSS) · [OptiScaler DLSS-NR](https://github.com/Dagherbou/OptiScaler_DLSSNR)

## Recommended implementation order

**1–7 reliability → 8–12 quality/performance → 13 multipass → 14 processing order → 15 learned guides → 16 Frame Generation → 17–18 ecosystem expansion**

The highest-value additions are **runtime preflight, stall recovery, preview-first rendering, source-color protection, RTX artifact reduction, and a carefully isolated two-pass backend**.
