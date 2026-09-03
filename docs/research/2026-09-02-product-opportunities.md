# Product opportunities: DLSS 5 Video Player

Research date: 2 September 2026. Source baseline: `main`, commit `7d37ff3`.

Update: the [attached-review verification and revised roadmap](2026-09-02-attached-review-verification.md) checks all 22 follow-up recommendations and supersedes the priority order below. It promotes settings/presets, separates NVOFA and official-runtime investigations from release dependencies, and corrects several competitor and SDK claims.

Recommendation: develop the project into a local video enhancement player with fast previews, trustworthy comparisons, and reusable outputs. Keep experimental neural rendering as a distinct capability. The largest near-term gains are reducing the wait before seeing useful results and making those results easy to judge and save.

This assumes the primary audience is Windows/RTX enthusiasts working with local videos and public examples. Priorities are product judgments, not user-research measurements. Assessment used current source, repository documentation, existing release screenshots, and primary external sources. No application code was changed, fresh GPU benchmark run, or live native-app usability test performed.

## What the project already has

The current implementation includes verified whole-video neural preparation, reusable disk caches, synchronized original/neural toggling, optional playback-time DLSS SR at 1440p/2160p, YouTube acquisition, loading/progress feedback, seeking, audio, fullscreen, image adjustments, and diagnostic guide views. These should not be presented as new feature proposals. Frame generation is explicitly unavailable.

The architecture also already isolates the neural runtime in a helper and validates output before publishing a cache entry. Preserve those boundaries while expanding the experience. See [README](../../README.md) and [architecture](../ARCHITECTURE.md).

## What external research changes

| Reference | Verified capability | Implication for this project |
| --- | --- | --- |
| [NVIDIA RTX Video SDK](https://developer.nvidia.com/rtx-video-sdk/getting-started) | Video super resolution, compression-artifact reduction, SDR-to-HDR; SDK 1.1 adds 10-bit SR, CUDA and Blackwell support. The page lists DX12 and RTX 20-series-or-newer support. | Evaluate a video-specific enhancement backend. Its published hardware coverage could expand this player's audience, but the complete player must be tested on each target GPU. |
| [Topaz Video workflow](https://docs.topazlabs.com/topaz-video/reference-guide/importing-previewing-and-exporting) | Short range previews, synchronized split comparisons, presets and export queues. | Users need to judge a result before paying the processing cost of a full video. |
| [MPC-HC](https://github.com/clsid2/mpc-hc/blob/develop/Readme.md) | Resume position, subtitles, speed control, folder navigation, segment looping and online video opening. | Everyday playback conveniences influence whether this becomes a regular player. |
| [SVP RIFE integration](https://www.svp-team.com/docs/rife-ai/) | Neural interpolation for playback and transcoding, including a TensorRT route. | Frame interpolation is feasible as a separate project; competitors already offer it. Local performance must be established. |
| [NVIDIA DLSS 5 description](https://www.nvidia.com/en-eu/geforce/news/dlss-5-3d-guided-neural-rendering/) | Game-oriented neural lighting/material enhancement using color and motion inputs. | Distinguish creative neural changes from video restoration and spatial upscaling in the product language. |

Existing [MPC Video Renderer work](https://github.com/emoose/VideoRenderer) also demonstrates an RTX HDR integration in another player ecosystem. A new enhancement toggle alone is unlikely to be a durable differentiator; the preview, comparison and reuse workflow is the stronger opportunity.

## Ranked additions

Effort is relative: S = a focused change, M = several components or flows, L = substantial subsystem work. These are not delivery estimates. Ranking balances usefulness, differentiation and fit with the current architecture.

| Rank | Addition | User benefit | Effort |
| --- | --- | --- | --- |
| 1 | Watch immediately; render a short preview on demand | Users can inspect the source and test an enhancement before processing the entire video. | M-L |
| 2 | Comparison workspace: wipe, side by side, linked zoom and loop | Makes improvement, distortion and temporal artifacts much easier to judge. | M |
| 3 | RTX Video SR backend | Adds enhancement designed for ordinary compressed video and a potential broader RTX audience. | L |
| 4 | Resume, subtitles, audio-track selection and recent files | Removes common reasons to return to another player. | M-L overall |
| 5 | Export the verified enhanced result with audio | Turns a viewing experiment into a reusable creator utility. | M for native cache export; L for new processing modes |
| 6 | Cache browser with quota, location and selective cleanup | Makes repeated use practical without opaque disk growth or all-or-nothing deletion. | M |
| 7 | Render presets and saved per-video settings | Makes good results reproducible and discoverable. | M, after cache identity work |
| 8 | Persistent render queue | Supports preparing several videos unattended while exposing failures and progress. | M-L |
| 9 | End-to-end color management and HDR | Makes higher-quality movie and camera sources viable without discarding their color precision. | L |
| 10 | Opt-in frame interpolation | Adds a meaningful smooth-motion mode for suitable content. | L |
| 11 | Benchmark and diagnostics page | Helps users choose workable settings and produce useful bug reports. | M |

### 1. Immediate playback and bounded previews

Offer clear actions: **Watch original**, **Preview enhancement**, and **Render full video**. Reuse a valid complete neural cache immediately when present. A selected preview should normally be 5-10 seconds, with an explicit range and a loop once ready. Show estimated time and disk requirements before a full job, using a measured sample and expressing the estimate as approximate.

The current `StartNeuralJob` unloads playback and the offline renderer starts at frame zero. Introduce separate preview artifacts with their own range, processing settings, timeline and validation. A preview must never satisfy the existing complete-video cache contract. For previews later in a clip, validate temporal warm-up or start from an appropriate preceding cut; do not assume a cold start yields the same result as processing the full sequence.

Initially, immediate original playback and explicit preview rendering can be separate actions. Concurrent original playback and neural processing needs a GPU scheduling budget and is a later refinement.

### 2. Comparison that makes the result observable

Add a draggable original/processed wipe, synchronized side-by-side view, linked 100%/200% zoom, hold-to-show-original, A-B loop, backward frame stepping, and paired PNG capture with timestamp/settings metadata.

`SynchronizedPlayback::CurrentPair()` already provides paired frames, so this is a strong reuse opportunity. Start with paused comparisons; two simultaneous live upscalers add performance cost. Keep geometry, display color treatment and optional SR equivalent when comparing neural on/off, so the comparison isolates the intended change. Difference views reveal change, not automatically better quality.

### 3. A video-specific enhancement option

Prototype RTX Video SR against current DLSS SR on the same clips and output sizes. Expose the selected backend and actual active state. Include a conventional scaler as a baseline. Start with one selected spatial upscaler per view; stacking SR passes should require an explicit advanced choice and evidence that it helps.

The SDK's documented artifact reduction is relevant to compressed footage. This does not establish that it beats the current player on every source. Validate the SDK's access, runtime packaging, supported formats and capability queries before committing to a release architecture.

Implement HDR conversion only after the color pipeline work below. A new SDK call cannot recover precision already discarded upstream.

### 4. Playback essentials in a useful order

Start with recent files and resume position, then external SRT/WebVTT subtitles and audio-track selection. Follow with ASS support, chapters, folder playlists, playback speed with pitch correction, and configurable shortcuts. Keep watch history local and provide a clear-history option.

Current audio selects the first stream and converts to stereo 48 kHz PCM (`AudioPlayer.cpp:96`). A track selector therefore needs backend support. Subtitle rendering should occur after enhancement at presentation time so text remains crisp and subtitle changes do not trigger neural re-renders. Existing forward frame stepping should be retained, not rebuilt.

### 5. Export that reuses completed work

First ship **Save enhanced video with original audio** for a verified native-resolution cache. The cached encode is video-only (`MediaPipeline.cpp:349`); saving it directly would omit audio. Mux the chosen source audio, preserve timing, and validate the resulting file. Copy the cached video stream where container compatibility permits, avoiding another lossy encode.

Then add range export, MP4/MKV selection and paired comparison clips. Clearly distinguish exporting cached neural pixels from baking playback-time SR or color adjustments into a new render. A 4K playback setting does not make the existing 1080p cache a 4K export. Frame-accurate trims may require boundary re-encoding; a stream-copy trim is not always exact.

### 6-8. Reuse, presets and queues

Cache UI should show source/derived sizes, resolution, settings, last use, validation status and a way to open or remove a selected item. Add a configurable location, storage cap, free-space checks, pinned entries and cleanup of abandoned staging. Eviction must respect source/derived dependencies and active readers/jobs.

Offer a few presets tied to supported behavior, such as Conservative, Compressed video, Animation and Experimental neural. Test those labels against a diverse clip set before claiming they are optimal. Display-time color changes should stay separate from render-time changes.

A first queue can run one neural job at a time, persist pending/completed jobs, and offer retry and cancellation. Pause between jobs is substantially simpler than durable pause within a temporal render. Cross-restart mid-job resume requires validated segments, warm-up and seam checks; preserve the existing all-or-nothing output validation until that work is complete.

## Foundations to address before expanding

### Cache identity must include effective render settings

`main.cpp:1529` hashes twelve runtime binaries; `main.cpp:1531` constructs a cache identity with fixed quality text. `NeuralCache.cpp:325` hashes those supplied fields. Meanwhile `ReShadeConfig.cpp:456` deliberately preserves user-owned style, intensity and guide overrides. The inspected identity has no digest of those effective overrides.

This creates a plausible stale-result path when render-affecting configuration changes without binary changes. It is a code-derived finding, not a reproduced runtime failure. Before shipping presets, snapshot and canonicalize the effective render configuration, pass that immutable configuration to the worker, and include it in both key and manifest. Do not hash unrelated window placement or playback volume. A semantic pipeline revision can also avoid invalidating every cache for unrelated application-version changes.

### Preserve source timing

`VideoDecoder.cpp:379` forces `-fps_mode cfr`, and line 543 reconstructs timestamps from frame index and average frame rate. [FFmpeg documents](https://ffmpeg.org/ffmpeg.html#Advanced-Video-options) that CFR duplicates/drops frames to meet the selected rate. This is deliberate normalization, not evidence that every source currently desynchronizes.

For faithful variable-frame-rate support, carry original presentation timestamps and frame durations through decoding, cache encoding, seeking and synchronized comparison. Simply replacing a CLI flag cannot preserve timestamps in the current unframed raw-pixel pipe. Test phone recordings, screen captures, nonzero start timestamps and fractional rates.

### Build a complete color pipeline

The decoder exposes 8-bit BGRA, the cache encoder emits `yuv420p`, and the display swapchain is `R8G8B8A8_UNORM` (`VideoDecoder.h:24`, `MediaPipeline.cpp:355`, `D3D12Renderer.cpp:98`). FP16 intermediates do not restore the lost source precision.

Carry matrix, range, primaries, transfer function and HDR metadata. Add 10-bit decode/cache formats where supported, explicit HDR-to-SDR tone mapping, and an HDR-capable presentation path. Probe each enhancement backend's color contract; if the experimental neural path only supports SDR, label and isolate that conversion. Treat SDR-to-HDR enhancement as separate from correct native HDR playback. [mpv's manual](https://mpv.io/manual/master/) provides a useful reference for presentation behavior and tone-mapping choices.

### Reduce frame transfers after profiling

Playback uses FFmpeg hardware decoding but downloads frames into BGRA CPU buffers before D3D12 upload. Offline neural rendering reads GPU output back to CPU before feeding the encoder. Those are concrete transfer boundaries, not proof of the dominant bottleneck.

Measure decode, guide generation, upload, neural evaluation, readback, encode and disk time separately. Then prototype GPU-resident surfaces and direct D3D12 NVENC input. NVIDIA documents [D3D12 resource registration and fence synchronization](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html). Retain CPU fallback while validating ownership and synchronization. Avoid rewriting the entire pipeline before measurements justify it.

### Improve motion guidance and interpolation as separate experiments

The existing compact CPU motion matcher and depth proxy are approximate (`TemporalGuides.cpp`). [NVIDIA Optical Flow](https://developer.nvidia.com/optical-flow-sdk) provides a DX12 interface and forward/backward flow. It is a candidate for better motion estimates and uncertainty masks, but does not produce authoritative scene depth or guarantee better neural output.

For smooth-motion playback, evaluate [RIFE](https://github.com/hzwer/ECCV2022-RIFE) or a documented [FRUC interface](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvfruc-programming-guide/index.html). Do not assume the FRUC library uses the same DX12 interface as the optical-flow API. Use the label **Frame interpolation**, with explicit 2x/target-rate controls, cut handling and original-cadence mode. Establish offline correctness before claiming real-time 4K performance.

## Focused UI changes

The existing screenshots show a useful large video viewport, clear original/neural state and understandable preparation status. Preserve that focus.

- Replace the large permanently unavailable Frame Generation toolbar control with a capability entry under Advanced until an implementation exists.
- Label the comparison state **Original / Neural result**, with processing and playback upscaling controls visually separated.
- Put source/output resolution and active enhancement in a compact status row; move detailed frame/drop diagnostics into an expandable panel.
- Add a first-run capability view explaining which features are available on the detected GPU and why others are unavailable. Offer a short local test.
- Give recoverable errors actions such as Watch original, Retry and Open diagnostics, preserving source context.
- Verify keyboard focus, screen-reader names, high-contrast support and larger text with the native app. Screenshot review cannot establish accessibility behavior.
- Recapture the start-screen documentation image before wider sharing: its current screenshot contains a screen-sharing banner and an account identifier despite the provenance description claiming no external overlay. Do not alter the original capture in place.

## Recommended sequence and evidence

1. **Make results easy to evaluate:** cache-settings identity, paused wipe/zoom/loop, native cache export with audio, recent/resume and toolbar cleanup.
2. **Reduce the cost of trying the tool:** immediate-original flow, bounded neural preview, cache manager and a persistent single-job queue. Run the RTX Video SR prototype as a separately measured experiment.
3. **Expand media fidelity:** original timestamps, track/subtitle support, color management and HDR, followed by measured transfer optimization.
4. **Add advanced enhancement:** validated presets, better motion guides and optional interpolation.

For a creator-first audience, promote export and batch jobs. For a movie-player audience, promote subtitles, audio tracks and HDR. The sequence above assumes the current experimental comparison use case remains central.

Measure time to first original frame, time to first usable enhanced preview, cache-hit startup time, render throughput, frame-time percentiles, dropped frames, A/V skew and storage consumption. Use a fixed set covering live action, faces, small text/HUDs, animation, grain, fast motion, cuts, low-bitrate video, VFR and HDR. Assess subjective quality through equal-size, equal-color-treatment comparisons over motion, not just screenshots. Repository benchmark reports are historical evidence; none were rerun for this research.

Defer broad cloud accounts, social features, streaming-service integrations and an entire editing timeline until this local workflow is convincing. Preserve the distinction between requested capability, active processing and verified output throughout.
