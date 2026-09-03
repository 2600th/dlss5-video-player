# Verification of the attached recommendations

Checked 2 September 2026 against local commit `7d37ff3`, the supplied 22-point review, current primary-source documentation and selected competitor source files. This is a research update, not approval or implementation of the proposed features. It supersedes the priority order in the initial [product-opportunities report](2026-09-02-product-opportunities.md).

The attachment identifies useful missing capabilities, especially persistent settings, render controls, previews, export and hardware optical flow. Its proposed first release is too dependent on an unverified official-runtime migration and unmeasured performance/quality assumptions. Ship improvements with known dependencies while investigating those two areas separately.

## Corrections that materially change the recommendation

### Official launch does not establish a drop-in runtime migration

[NVIDIA's announcement](https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/) specifies 3 September at 9 p.m. Pacific for the game feature and driver. That is **4 September 2026 at 9:30 a.m. IST**. Its announcement establishes an RTX 50 game launch; it does not establish an independently usable signed neural DLL in DriverStore, compatibility with this player's interception path, or permission to redistribute that DLL.

A read-only scan of this machine's 16 `nv*.inf_*` DriverStore package directories found `_nvngx.dll` and `nvofapi64.dll`, but no `nvngx_dlssnr.dll`. This is a bounded observation of the installed driver packages, not a prediction about the forthcoming driver or every possible installation location.

[ComfyUI's README](https://github.com/lisitskyaa/ComfyUI-DLSS5-NR#_nvngxdll) distinguishes discovery of the NGX core `_nvngx.dll` from the separately supplied neural runtime. The official-runtime proposal should therefore become a capability/compatibility investigation. Report file signature, supported integration route and successful evaluation as separate facts. Replacing one DLL also does not supply FFmpeg/YouTube helpers or resolve the rest of the experimental package's dependencies.

### The competitors are useful references, with narrower evidence than claimed

- [Zonnery](https://github.com/Zonnery/dlss5-nr-player) advertises live neural comparison, conversion and parameter controls. Its inspected [player source](https://github.com/Zonnery/dlss5-nr-player/blob/e9c37bec513991bf25a204e3e6dbfe4b8a7dffb2/nr_player.cpp) invokes FFmpeg, reads raw NV12 through a CPU pipe, copies it into upload staging, then converts to FP16 on the GPU. This reduces color-conversion work and bytes compared with BGRA piping; it is not a zero-copy NVDEC-to-NR pipeline. Real-time performance was not reproduced here.
- [Merserk](https://github.com/Merserk/dlss5-visual-enhancer) documents batch rendering, previews, saved controls and output verification. Its [interpolation code](https://github.com/Merserk/dlss5-visual-enhancer/tree/e22f822496377757b678822aab8a417478071771/src/frame_interpolation) contains a separate DLSSG worker interface and explicit frame-rate choices. This supports the feature-existence claim, not a performance/quality comparison. Native worker implementation was not among the public source files inspected. Star counts do not establish correctness or adoption.
- Merserk's [video code](https://github.com/Merserk/dlss5-visual-enhancer/blob/e22f822496377757b678822aab8a417478071771/src/video.py) rejects **HDR preservation**, while describing HDR-input conversion to SDR. “Rejects HDR” is too broad. True HDR output remains an opportunity, but ordinary HDR-input acceptance is not established as unique.
- ComfyUI v0.3.0 documents NVOFA temporal guidance and still uses CPU staging for tensor transfer. Its implementation is a useful reference, not proof that this player's output will improve automatically.

Atomic reusable caches, independent playback SR, timestamp-matched viewing, process isolation and source access form a useful combined product. Feature-18 verification and helper isolation should not be advertised as individually unique without a broader comparison; Merserk also describes a worker and output verification.

### Motion-vector units differ between integrations

ComfyUI documents normalized UV vectors with a width/height scale. This player currently stores motion in render-pixel units (`TemporalGuides.cpp:302`) and uses `MV_Scale_X/Y = 1` (`DLSSBackend.cpp:247`). Copying the normalized-vector contract without adapting the consumer would produce incorrect magnitudes.

The [NVOFA guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html) describes fixed-point flow and DirectX interfaces. A prototype must adapt direction, fixed-point conversion, grid reconstruction and scale to the active backend. Test positive/negative translation, occlusion, cuts and resize resets. Hardware optical flow estimates motion; it does not replace the depth proxy. NVOFA is not a prerequisite for exposing existing neural parameters.

### Two technical assertions need direct replacement

At 3840 x 2160 x 4 bytes x 60 fps, one tight BGRA8 stream contains **1,990,656,000 bytes/s**, approximately **1.99 GB/s** or **1.85 GiB/s**. Approximately 500 MB/s instead corresponds to 1080p60 BGRA8. These are payload calculations for one stream/pass, not measured PCIe bandwidth or total pipeline traffic.

The pinned [NVIDIA SDK header](https://github.com/NVIDIA/DLSS/blob/a291cc7d2cc642a51566f3dfd5376f635cd1b284/include/nvsdk_ngx_defs.h) explicitly marks DLSS sharpening unsupported. Exposing the current `Sharpness` parameter as a working feature would be misleading. If desired, implement a separately identified, measured post-process sharpening filter. DLSS SR model hints and neural style presets must also remain distinct controls.

## Disposition of every numbered recommendation

| Attachment item | Verification and updated recommendation |
| --- | --- |
| **1. Official runtime** | **Research track.** Keep the objective, remove the promise of a launch-day drop-in replacement. First establish a supported runtime/API combination, signature/provenance and an isolated successful render. Update runtime locks only after that evidence. The installed DriverStore scan did not find the neural DLL. |
| **2. NVOFA guides** | **Promote to an early measured prototype.** The compact SAD matcher and heuristic depth are confirmed in `TemporalGuides.cpp`; the depth proxy also uses gradients, motion and temporal smoothing, not just row position. “CPU-bound at 60 fps” and “weakest link” are not demonstrated. Compare guide time and temporal artifacts against the current path. Retain fallback. |
| **3. Parameters/presets** | **High priority, with a prerequisite.** Add a canonical immutable render-settings snapshot to the helper request, cache key and manifest. Start with a few controls whose applied effect can be verified. Preserving unknown INI values does not itself prove those values affect the captured output. Avoid promising content-specific preset quality before testing. |
| **4. Three-second preview** | **High value; medium-to-large work.** Call this a bounded rendered preview. `NeuralRenderRequest` has no start-range field and the worker starts from frame zero. Add range/timeline metadata, temporal warm-up, preview validation and comparison support. Reuse the renderer, but do not treat the entire feature as a trivial frame-range argument. It is not evidence of real-time processing. |
| **5. Export/transcode** | **High priority; split into two deliveries.** First remux verified native cached video with available source audio, subtitles and chapters. Explicit stream mapping and container compatibility are required. Then add codec/transcode options. Encoding an 8-bit cached image into 10-bit does not recover source precision or create correct HDR. The relevant FFmpeg format names are `yuv420p10le`/`p010le`, selected according to the encoder's actual accepted input. |
| **6. Persist settings** | **Confirmed and promoted.** `main.cpp:778`/`:787` save only video adjustments. Persist volume, aspect, output target, source quality, window position, recent directory and opt-in history. Validate restored values against current capabilities; a saved requested neural state must not become a false active-state claim. |
| **7. Pacing and PTS** | **Valid objectives; separate diagnoses.** Both presentation sites use `Present(0, ...)`; flags conditionally allow tearing. A waitable swapchain is worth testing, but `Present(0)` is not alone proof of broken pacing. The loop yields with `Sleep(0)` while playing and sleeps 8 ms when paused. CFR normalization may duplicate/drop frames; it does not prove universal A/V drift. Preserve source timestamps independently of improving presentation scheduling. |
| **8. Subtitles** | **Confirmed gap, revised implementation.** Prefer subtitle composition after enhancement. [libass](https://github.com/libass/libass) is relevant for ASS/SSA styling; plain D2D text is not a full ASS renderer. Keep burn-in an explicit export choice. Subtitle rendering, stream preservation and timestamp handling are related but distinct work. |
| **9. HDR/10-bit** | **Confirmed pipeline gap.** First carry color metadata and perform correct HDR-to-SDR conversion. Then implement native high-bit-depth output and cache formats for supported backends. Merserk's refusal to preserve HDR should not be described as refusal to accept HDR input. |
| **10. Playlist/queue** | **Confirmed gap.** Drop handling opens the first file only. Add multiple-file/folder queue and auto-advance. A separate neural process is not a resource scheduler: start with one render job at a time and explicit playback priority before concurrent preparation. |
| **11. Split/wipe/OSD** | **Promote.** Reuse the synchronized frame pairs for paused wipe/linked zoom, then evaluate live comparison cost. Difference imagery measures change, not quality. Make diagnostic OSD optional; retain a concise, readable status area. |
| **12. In-process decoding** | **Reframe as a staged media-pipeline improvement.** Correct the payload arithmetic above. The current raw pipe discards per-frame metadata; process isolation itself does not prevent PTS or HDR support. A typed frame protocol carrying timestamps, color metadata and NV12/P010 is an incremental option. GPU-resident libavcodec/NVDEC integration is a larger alternative requiring measured benefit. |
| **13. Quality dead code** | **Cleanup justified; restoring modes is a separate design.** Command cases are empty at `main.cpp:1900`; legacy CLI quality arguments are parsed and forwarded into paths that ignore them. Remove/deprecate misleading options coherently, including scripts/docs. Do not restore game-style quality ratios by silently reducing source resolution; the current approved SR design explicitly preserves it. |
| **14. Sharpness/model presets** | **Correct the proposal.** Legacy DLSS sharpness is unsupported in the pinned SDK. Default/automatic model selection and capability-checked J/K/L/M hints can be investigated, but are advanced controls, not guaranteed improvement. They must not be conflated with neural style, intensity or output size. |
| **15. Cache quota/LRU** | **Promote from polish.** Cache sizing and Clear exist, while quota and selective eviction are missing. Add a cache browser, free-space checks, configurable limits, pinning and source/derived dependency-aware eviction. Active files and jobs must remain valid. |
| **16. Resumable jobs** | **Defer arbitrary mid-job resume.** Frame index plus an encoder segment cannot restore neural history by itself. Use pending-job persistence first; later evaluate scene/segment checkpoints with reproducible warm-up and seam validation. Preserve complete-output verification. |
| **17. Audio/WASAPI** | **Partly confirmed, sequence by benefit.** The player uses first-track stereo 48 kHz PCM through waveOut. Prioritize track selection and explicit downmix behavior. Device selection/WASAPI can follow measured synchronization, output-format or device-switching needs. API replacement alone is not a user feature. |
| **18. Playback controls** | **Mixed.** Non-cached frame stepping, speed, zoom and configurable volume keys are useful additions. Timeline drag seeking and mouse-wheel volume already exist (`main.cpp:1871`, `:1880`); exclude them from the missing-feature list unless a different gesture is intended. |
| **19. Broader online sources** | **Later scope expansion.** Playlist opening is useful after the local queue. Live/HLS and arbitrary yt-dlp sites require different buffering, duration, seeking and source-validation behavior. Do not put them behind one supposedly simple toggle. |
| **20. Localization** | **Reject as a defect claim.** `docs/LOCALIZATION.md` and the newer changelog explicitly establish an English-only UI. Old 0.9.0 history does not override that decision. Centralizing hardcoded strings can improve maintenance; restoring packs requires a new product decision. |
| **21. Documentation** | **Useful with corrected claims.** Add a dated competitor matrix with columns for documented, source-inspected and locally tested behavior. Hash-versus-signature limitations are already explained in README/security/technical docs; a concise download-verification guide may improve discoverability. Do not claim that verification or helper isolation is unique. |
| **22. Tests/GPU CI** | **Strengthen the existing gates.** Dedicated quantitative motion-guide tests and VFR/HDR fixtures are worthwhile. “No real decode coverage” is too broad: `UpscalingGpuSmoke.cpp` opens actual media, generates guides and evaluates SR, and local verification reports document runs. It is opt-in, not portable CTest. Add trusted/manual GPU CI and enforce the expected fixture frame count; currently the smoke exit condition accepts any positive count with matching evaluations. |

## Revised roadmap

### First: reproducible settings and useful outputs

1. **Immutable render configuration and cache identity.** Include actual NR settings and the processing contract in the identity; keep display-only settings separate. Record requested/applied settings in the manifest.
2. **Persistent preferences and focused neural controls.** Expose only verified controls; restore playback preferences and offer per-video settings. Start with conservative defaults.
3. **Paused wipe/zoom/loop comparison and native cache export with audio.** These build directly on existing synchronized pairs and validated encoded output. Preserve compatible source streams with explicit mapping and output validation.
4. **Basic cache management.** Show reusable results and storage use, support selective deletion and a configurable cap.

### Next: reduce waiting and support ordinary viewing

5. **Watch-original-first and bounded 3-10 second previews.** Create a proper preview artifact with range, warm-up and validation. Keep preview completion distinct from full-video completion.
6. **Persistent sequential job queue and folder playback.** Resume pending work after restart; add concurrent rendering only after a playback resource budget exists.
7. **Source PTS, subtitles/audio tracks and measured frame pacing.** Add fixtures for VFR, fractional frame rates, seeking and long A/V synchronization. Evaluate a [DXGI waitable swapchain](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject) without blocking the window message loop.

### Research alongside those deliveries

- **NVOFA:** compare CPU guide time, flow accuracy, ghosting and cut recovery; verify vector units per backend. Promote only on measured benefit.
- **Official NR runtime:** after its actual availability, confirm distribution/API contract and compatibility in an isolated helper. A signed file is one part of the evidence.
- **RTX Video SR:** retain the video-specific alternative from the first report as a separate experiment. The attachment does not invalidate its usefulness, but it need not delay the existing NR workflow improvements.

### Later, after those results

Deliver source-aware HDR/high-bit-depth processing and reduce CPU transfers where profiling identifies value. Then evaluate interpolation, durable segment resume and wider online-source support. Performance predictions should include source resolution/rate, output resolution, GPU/runtime versions and measured percentile timings.

## Verification boundary

Local application code and SDK headers were inspected; selected competitor source was read without executing it. Zonnery was inspected at `e9c37bec513991bf25a204e3e6dbfe4b8a7dffb2`; Merserk at `e22f822496377757b678822aab8a417478071771`. ComfyUI documentation describes v0.3.0. Current official documentation was checked on the research date. No drivers, DLLs or application settings were changed. No fresh GPU renders, compatibility tests, frame-pacing benchmarks or full regression suite were run. Historical repository test reports remain historical evidence.
