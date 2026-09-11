# DLSS 5 Video Player — Updated Roadmap

_Current as of September 11, 2026._

## Quick reality check

- DLSS 5 Neural Rendering is officially available on RTX 50-series GPUs, but NVIDIA's public DLSS repository still lists SDK 310.7. Video processing through Feature 18 remains an experimental community workflow. [NVIDIA announcement](https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/) · [DLSS 5 research](https://research.nvidia.com/labs/adlr/DLSS5/) · [Public SDK releases](https://github.com/NVIDIA/DLSS/releases)
- Keep the player's currently tested stack pinned: **driver 616.64 + ReShade 6.8 + RenoDX 4.70 + NR/SR 310.8**. [Runtime lock](../packaging/runtime-lock.json) · [Measured report](measurements/runtime-comparison-20260907/REPORT.md)
- **Unverified risk against that pin:** DLSS5-Autopilot's field data reports renodx-dlss5 4.6/4.7 faulting on every evaluate from driver 616.64, which is the driver we recommend. Untested here. See item 0 below.

## Next session — ordered, from the 2026-09-11 survey

Written after v0.20.0 shipped hardware optical flow. Everything here was checked
against the source or an external document on that date; the ordering is by value
over cost, not by ambition. Items 1–4 are together about four flags and thirty
lines, and they aim at the symptom the whole 0.20.0 cycle was chasing: motion that
does not feel attached to the picture.

**0. Check the add-on against driver 616.64+ before anything else.** DLSS5-Autopilot's
aggregated field data says "from 616.64 the driver routes neural rendering through
its own runtime, and the renodx-dlss5 add-on the feeder route loads faults there -
4.6 and 4.7 on every evaluate, 4.55 in some games". We pin renodx-dlss5 4.70 and we
recommend 616.64, which is exactly the reported combination. Their workaround was a
standalone route that never loads the add-on. One session on a current driver
settles whether this reaches us.
[Autopilot v1.8.1](https://github.com/Kizzuwatnaa/DLSS5-Autopilot/releases/tag/v1.8.1)

**1. `NV_OF_PRED_DIRECTION_BOTH`.** `src/OpticalFlowNvof.cpp` asks for forward flow
only. The NVOFA guide: "When `NV_OF_INIT_PARAMS::predDirection` is set to
`NV_OF_PRED_DIRECTION_BOTH`, forward and backward flow will be generated in a single
`NvOFExecute`/`NvOFExecuteD3D12`/`NvOFExecuteVk` API call", with `bwdOutputBuffer`
and `bwdOutputCostBuffer` alongside. A forward/backward disagreement test is
measured in pixels, so unlike the cost gate it needs no invented constant; the
standard criterion is scale-free. This closes two open items at once and does not
need a second session or a second Execute.
[NVOFA guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html)

**2. `enableGlobalFlow`.** Also off today, also computed inside the Execute we
already issue: "a global flow vector is estimated from forward flow in the same
`NvOFExecute` ... API call". A slow pan is a large, coherent global vector the
per-cell field agrees with; a cut is one it does not. Our cut test currently has no
motion evidence in it at all.

**3. The zero-motion SAD we already discard.** `EstimateFlow` records the
zero-displacement cost and drops it - only `bestGlobal` escapes as `globalCost`.
x264/x265 do not threshold an absolute residual, they compare inter cost against a
no-prediction baseline, which is scale-free where our fixed 0.10 band is not. Our
own note records the softest observed real cut at 0.108, sitting on that threshold.
mvtools adds the second half of the same idea: decide on the *fraction* of blocks
that failed, not the mean. Per-cell confidence already exists; the fraction is one
accumulate.
[x265 scenecut-bias](https://x265.readthedocs.io/en/stable/cli.html) ·
[mvtools thSCD2](https://avisynth.org.ru/mvtools/mvtools2.html)

**4. Temporal metrics in the harness.** `tools/benchmark/analyze.py` has no
per-pixel temporal variance, no false-motion rate and no cut precision/recall. The
60.8 % -> 3.7 % false-motion figure and 0.20.0's 2.23 -> 1.41 temporal sigma are both
prose from ad-hoc runs that no script reproduces. The ground truth for cuts is
already written: `tools/benchmark/corpus.py` records hard-cut frame indices in the
manifest and `analyze.py` only uses them to exclude frames. Until this exists, every
item below is unfalsifiable, which is also why the reference table being stale
matters more than it looks.

**5. Then, now that they can be measured:** re-run the reference table; A/B `IsHDR`
on the linear FP16 input; A/B a supplied 1x1 exposure texture against auto-exposure;
A/B constant depth against the proxy; calibrate the NVOFA cost gate against the
forward/backward mask from item 1 and keep it as the cheap one-pass runtime proxy if
agreement is high.

### Corrections this survey produced

- **Temporal hints.** We disable them unconditionally. NVOFA guideline 3: "Disable
  the temporal hints only if there is a-priori knowledge of no temporal correlation
  (e.g. a scene change, independent successive frame pairs)." The shape NVIDIA
  describes is on within a shot, off at a discontinuity - which we already detect.
  The real cost of changing it is cache determinism, since output would then depend
  on where the segment started. A design decision, not a bug.
- **Auto-exposure is not a defect.** Streamline: "If `sl::kBufferTypeExposure` is NOT
  provided or `dlssOptions.useAutoExposure` is set to be true then DLSS will be in
  auto-exposure mode". Our null texture is the sanctioned path. Re-scope the item
  from "fix" to "never measured against a supplied exposure".
- **Depth is not in any official description of the model's inputs.** NVIDIA
  Research: "the model is conditioned on the current rendered frame, engine motion
  vectors, carried temporal state, and artistic-direction values." Depth appears
  nowhere. Our SR carrier is a real SR feature and does consume depth, so it cannot
  simply be deleted - but the useful experiment is constant-depth versus the proxy,
  not more tuning of the per-frame normalizer.
- **NVOFA cost has no published scale.** Three official sources say only that a
  higher cost means a less accurate vector. No range, no normalization, no
  recommended cutoff. The gate cannot be closed by reading; it has to be measured or
  replaced by item 1.
- **Hardware flow does not cover upscaling sessions.** `D3D12Renderer.cpp` only
  offers the engine when the decoded frame is already the DLSS input size, so the
  runtime SR toggle silently keeps the CPU estimator. True and deliberate, but only
  ARCHITECTURE.md says so.
- **The driver floor has an official citation now.** The 616.64 release notes name
  it for DLSS 5 Neural Rendering, so `src/RuntimePolicy.h` can stop citing a
  community figure in text that reaches a dialog.
- **Frame generation for video is now an NVIDIA product**, in the Video Effects SDK
  rather than DLSS, and it ships automatic shot-change detection with a hard bypass
  on by default. That both decides the Frame Generation stub (point at VFG or delete
  it) and independently confirms that a cut detector plus a bypass is the accepted
  answer to our problem.
  [VFG filter](https://docs.nvidia.com/maxine/vfx/latest/Filters/VideoFrameGeneration.html)

### Where the ecosystem actually is

Nobody has solved temporal coherence for video neural rendering. dlss5-bridge says
of its own substitute contract that "text softens and dense foliage smears";
DLSS5-Feeder tells users to "expect the temporal quality of estimated motion vectors
(some ghosting in fast motion, softness on thin moving geometry)". video2dlssnr is in
the identical state as us on the confidence gate, with the comment "Confidence gate
disabled (costLo==costHi==0 -> shader keeps every vector)". No project in the survey
implements a forward/backward check, and no one has published cost thresholds - that
ground is unclaimed.

NVIDIA has still published no neural-rendering API: the public NGX header names
feature 18 `NVSDK_NGX_Feature_Reserved18`, and Streamline 2.14.1 shipped five days
after DLSS 5 launched with no NR plugin or guide. Nothing states that video is or is
not a supported use. Every ordering and threshold decision here is ours to measure;
there is no spec to defer to.

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
>
> Open gap in item 1 (found 2026-09-10 while porting PR #5): the evidence chain
> accepts a run in which feature 18 was created and evaluated but the neural
> pass did not execute. A worker that stopped presenting after the feature
> recreate reported `frames=900/900 verified=900` for output that was DLAA
> only (0.46 ms neural GPU time against 5.7 ms; 34.6 dB from the source
> instead of 31.7). The receipt should carry the per-frame neural GPU time and
> refuse a run whose median falls below a per-geometry floor, or compare a
> captured frame against the DLAA-only path directly.

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

> Scene-cut false positives (found 2026-09-10): the cut detector was stateless,
> so a single transition fired it repeatedly — 6 fires in 12 frames
> (#19119-19130) and 11 in 15 frames on a second clip. Every fire zeroes the
> motion vectors, drops the depth-proxy EMA, bumps the history generation and
> reaches NGX as `NVSDK_NGX_Parameter_Reset=1`, i.e. it throws away the
> accumulated history the reconstruction is built on; the DLSS Programming
> Guide 310.6.0 S3.13 asks for a reset only on the first frame after a major
> transition and notes that "improper use of this can result in temporal
> flickering, heavy aliasing or other visual artifacts". Shipped: the decision
> is split by strength, and the weak arm (moderate residual plus collapsed luma
> histogram) is now suppressed unless 0.6 s has passed since the last accepted
> cut — PySceneDetect's `min_scene_len` default, applied as the same hard
> minimum-interval filter. A strong residual still cuts immediately, which is
> how x264/x265 treat a decisive scenecut inside `min-keyint`. Every frame now
> carries its own verdict (strength, suppressed flag, residual, histogram
> overlap) so a live-playback decision is no longer invisible. Still open: the
> 0.30/0.10/0.85 thresholds are corpus-tuned, not validated against labelled
> cuts, and the accepted/suppressed counts do not yet appear in the render
> receipt next to the neural timings.

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

**NVOFA is in.** Motion comes from the engine on a 2x2 grid in S10.5, with the
CPU estimator kept as the fallback for cards and builds without it. What that
leaves open is the confidence half: the engine's cost surface is produced and
bound, but the gate is off because its thresholds have not been measured, and
nothing yet uses forward/backward disagreement.

The 2026-09-11 survey found the cheap route to both, and it is items 1 and 2 of
the plan at the top of this file: `NV_OF_PRED_DIRECTION_BOTH` returns backward
flow and its cost from the same Execute, which makes the disagreement test a
measurement in pixels rather than a guess at a cost threshold, and
`enableGlobalFlow` returns the per-frame global vector the cut detector is
missing. Neither needs a second session or a second Execute. NVIDIA publishes no
numeric meaning for the cost values in any document, so calibrating the cheap
gate against the forward/backward mask is the only honest way to keep it.

Still worth comparing the engine against RAFT and a Lucas-Kanade fallback on
the corpus before assuming it wins everywhere.

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
