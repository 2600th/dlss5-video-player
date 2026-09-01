# Neural pre-render cache and synchronized comparison design

**Date:** 2026-09-02  
**Status:** Approved  
**Target release:** v0.12.0

## Objective

Make DLSS 5 neural rendering useful on RTX 40- and RTX 50-series machines even when neural processing is slower than real-time playback.

The player will render a complete neural version to a persistent, verified disk cache before playback. Playback will then use synchronized original and neural streams so the existing DLSS on/off control provides an immediate same-timestamp comparison. Spatial DLSS upscaling remains disabled by default.

The release must also correct cramped toolbar buttons whose icons or labels currently touch their outlines.

## User-visible requirements

- Default neural source and output resolution is 1080p.
- Remove 480p and 720p from the manual quality selector.
- Keep 1080p, 1440p, and 4K selectable when the source offers them.
- If 1080p is unavailable, automatically use the highest supported source resolution up to 4K, including a sub-1080p source as a compatibility fallback. Sub-1080p choices are not exposed manually.
- Neural rendering is native-resolution: selected source dimensions equal neural output dimensions. DLSS spatial upscaling is off unless a future, explicit feature changes that policy.
- Opening media while an authorized neural runtime is active automatically starts pre-rendering, unless a compatible completed cache already exists.
- Playback begins only after the neural render has completed and passed validation.
- Playback initially shows the original stream. DLSS on selects the cached neural stream; DLSS off restores the original at the same presentation timestamp.
- A pre-render screen shows resolution, completed frames, percentage, elapsed time, estimated remaining time, cache size, and Cancel.
- Cancel or failure offers original-only playback and never publishes a partial neural file.
- A Clear Neural Cache action reports the recoverable size and removes only validated application-cache paths.

## Why full pre-rendering is selected

The measured neural renderer is slower than real-time at 1080p on the development RTX 5090 path. A finite rolling buffer can absorb startup variance but cannot compensate when average production rate remains below consumption rate. It would eventually stall.

The selected approach is a persistent encoded cache:

1. Materialize a stable source for network media.
2. Decode every source frame without a presentation deadline.
3. Evaluate the neural renderer for every frame.
4. Encode the evaluated frames into a temporary cache artifact.
5. Validate the artifact and neural-execution evidence.
6. Atomically promote it to a completed cache entry.
7. Play the original and completed neural streams on one shared timeline.

A raw-frame cache was rejected because its storage cost is excessive. A rolling buffer was rejected because it cannot solve sustained below-real-time neural throughput.

## Source and resolution selection

### YouTube

The resolver requests formats in this order:

1. Exact 1080p video, with the best compatible audio.
2. If exact 1080p is unavailable, the highest available supported video resolution up to 4K.
3. A user-selected 1440p or 4K format when that exact choice is available.

The source video and audio may be separate. For network media, the app materializes a stable local source-cache entry before neural processing; an expiring direct URL is not used as the long-lived comparison source. Only public, non-DRM media supported by the existing resolver is in scope. The cached source is private application data, is never presented as a user export, and is removed by Clear Neural Cache with its derived files.

### Local files

The original local file remains the comparison source and is never copied or modified merely to create a neural cache. If its dimensions are below 1080p, those native dimensions are used automatically. Selecting 1440p or 4K does not synthesize a resolution that the file does not contain.

### Manual quality menu

The menu contains Auto (1080p preferred), 1080p, 1440p, and 4K. Unavailable choices are disabled. There are no 480p or 720p commands.

## Cache model

### Location and ownership

Cache data lives under an application-owned per-user cache directory, not beside the source or executable. The cache manager resolves and verifies its absolute root before creating, renaming, or deleting entries. It never recursively deletes an unresolved path, drive root, profile root, repository, or source-media directory.

Each cache entry contains:

- a stable original source artifact for network media, or a reference fingerprint for a local file;
- the completed neural video;
- a JSON manifest; and
- bounded diagnostic evidence needed to verify the render.

Temporary work is created under an entry-specific staging directory. Completed artifacts are moved into place only after validation. An existing valid cache is never overwritten by a failed attempt.

### Cache identity

The cache key is a versioned digest over:

- normalized source identity;
- a local source content digest or network video ID plus selected format identity and materialized-source digest;
- selected dimensions and frame-rate/timestamp policy;
- neural feature settings, including upscaling disabled;
- application cache schema and renderer version;
- GPU generation/path classification; and
- SHA-256 hashes of the neural, NGX, Streamline, ReShade, and RenoDX runtime components actually used.

Changing any material input creates a different entry rather than reusing questionable output.

### Manifest states and validation

The manifest has explicit `staging`, `complete`, and `invalid` states. Only a `complete` manifest may be opened as a cache hit.

Promotion requires all of the following:

- renderer logs prove neural feature creation and at least one successful neural evaluation;
- every decoded source frame has a corresponding evaluated output frame;
- encoded dimensions and selected dimensions match;
- output duration matches the source timeline within one presentation-frame tolerance;
- output timestamps are monotonic;
- the encoded file can be opened and decoded through its final frame; and
- the cache manifest hashes match the completed artifacts.

An interrupted render cannot safely restart from an arbitrary frame because the neural renderer carries temporal state. For v0.12.0, incomplete staging output is retained only for diagnostics and then discarded or restarted from frame zero; it is not advertised as resumable. This favors correct temporal history over a visually discontinuous shortcut.

## Offline neural pipeline

Introduce a dedicated offline render controller around the existing decoder, D3D12 renderer, and FFmpeg process integration.

- Decode frames in presentation order without wall-clock playback deadlines.
- Feed the same native-resolution color and motion/depth inputs used by the current neural path.
- Keep DLSS Super Resolution scale at 1:1 and record `upscaling=OFF` in render evidence.
- Evaluate neural feature 18 for every frame.
- Use GPU fences before readback or encoder handoff so an incomplete surface is never encoded.
- Keep a small bounded queue to overlap decode, GPU evaluation, readback, and encode. The queue improves throughput but is not treated as a real-time guarantee.
- Preserve the source presentation timeline. Audio is not neural-processed and remains the shared playback master.

The preferred cache encoder is high-quality HEVC through NVIDIA NVENC on supported RTX 40/50 systems. If hardware encode initialization or operation fails, restart the encode with a high-quality software H.264 fallback. The manifest records the chosen encoder and settings. Encoding falls back; neural evaluation does not silently fall back to a non-neural image while calling the result complete.

## Playback and original-frame comparison

After cache completion, create synchronized original and neural video decoders under one playback coordinator:

- Audio and the logical media clock come from the original source timeline.
- Both video queues are advanced toward the same target presentation timestamp.
- The hidden stream remains warm within a bounded queue so switching does not trigger a new neural render or a new network request.
- DLSS off displays the original decoded frame.
- DLSS on displays the cached neural frame with the matching presentation timestamp.
- Pause, seek, frame-step, stop, and end-of-stream operations apply to both video streams as one transaction.
- After a seek, comparison switching is enabled only when both streams have reached the same target timestamp.
- If synchronization exceeds one source-frame tolerance, keep the current visible stream and show a concise synchronization warning instead of displaying a mismatched comparison.

In this mode, the DLSS control is a comparison selector, not a live NGX enable switch. Its accessible name and status text state `Original` or `Neural rendered` while preserving the familiar DLSS on/off interaction requested by the user.

## Progress, cancellation, and errors

The pre-render view remains responsive and provides:

- source and target resolution;
- processed and total frames when total is known;
- percentage and an indeterminate fallback;
- elapsed time and a smoothed ETA after enough samples exist;
- temporary encoded size;
- Cancel; and
- a concise current phase: acquiring, decoding, neural rendering, encoding, validating, or ready.

Cancellation signals only child processes and GPU work owned by this render job, waits for bounded shutdown, marks staging invalid, and offers original-only playback. Opening a different source cancels the current job before starting another. Closing the app follows the same owned-resource cleanup path.

User-facing errors identify the failed phase and recovery action. Technical details, runtime hashes, NGX result codes, encoder output, and validation findings go to the application log without recording expiring media URLs or credentials.

## Runtime and distribution boundary

- The repository and public release must not contain proprietary neural-rendering DLLs copied from leaked games or modified unsigned binaries whose redistribution is unauthorized.
- The public core package contains only redistributable project files and the permitted official DLSS component already covered by the packaging policy.
- Experimental neural rendering activates only when the complete external runtime layout is separately supplied and passes the strict local layout/hash policy.
- A partial, unknown, or mixed runtime fails closed and remains original-playback capable.
- The UI and README clearly distinguish public-core capability, experimental locally supplied runtime capability, intended RTX 40/50 compatibility, and the hardware generations actually tested.
- No release claim may describe an unsigned modified runtime as NVIDIA-signed, official, or universally compatible.

## Button spacing and accessibility

All toolbar and dialog buttons use DPI-scaled content insets instead of positioning icons/text against fixed outlines:

- at least 10 logical pixels left and right;
- at least 6 logical pixels top and bottom;
- at least 7 logical pixels between icon and label;
- at least 36 logical pixels total hit-target height; and
- centered combined icon-and-label content after scaling and localization measurement.

Hover, pressed, focused, default, and disabled states retain the same insets. Text clipping or outline contact at 100%, 125%, 150%, and 200% Windows scaling is a release failure. Existing double-buffered painting and targeted invalidation remain in place.

## Code boundaries

Keep policy and side effects separated:

- `RuntimePolicy`: automatic/manual resolution choices and neural prerequisites.
- `NeuralCachePolicy`: cache-key inputs, paths, manifest state transitions, and validation decisions.
- `NeuralRenderController`: job lifecycle, cancellation, progress, and phase orchestration.
- `OfflineNeuralRenderer`: frame-ordered GPU evaluation and encoder handoff.
- `SynchronizedPlayback`: shared clock, dual video queues, seeking, and comparison switching.
- `PlayerApp`: UI state, commands, progress presentation, and high-level orchestration.

Pure policies remain independently testable. FFmpeg, D3D12, NGX, filesystem, and process ownership stay behind narrow interfaces. Do not add a general database, background service, updater, browser, or new UI framework.

## Verification and release gate

### Automated tests

- Auto selects exact 1080p when available.
- Auto selects the highest available resolution when 1080p is absent.
- Manual choices expose only 1080p, 1440p, and 4K and disable unavailable entries.
- Neural output remains 1:1 and spatial upscaling is off by default.
- Cache keys change for every material source, settings, application, GPU-path, or runtime-hash change.
- Only a complete, hash-valid, decodable manifest is a cache hit.
- Interrupted, partial, mismatched, or temporally invalid output cannot be promoted.
- Cancellation terminates only owned work and preserves an older valid entry.
- Hardware encoder failure uses the declared software fallback without bypassing neural evaluation.
- Toggle, seek, pause, frame-step, and end-of-stream maintain one-frame synchronization tolerance.
- Public package allowlists exclude experimental proprietary runtime files.
- DPI-aware button layout meets inset and hit-target rules at the tested scale factors.

### Build and package

- Clean Release configure and full compile with the repository warning policy.
- Run all CTest targets from the fresh Release build.
- Assemble and independently verify the public-core stage and ZIP.
- Verify exact package contents, manifest hashes, official NVIDIA component signature policy, and absence of private experimental runtime files.
- Do not perform the skipped antivirus scan step.

### Resident Evil acceptance test

Use the established Resident Evil Requiem YouTube example for the final end-to-end test:

1. Resolve and materialize the preferred 1080p source.
2. Render the full test video through neural feature 18 with spatial upscaling off.
3. Verify every source frame is represented, dimensions are 1920x1080, timestamps are monotonic, and duration is within one-frame tolerance.
4. Confirm logs show successful feature creation and evaluation through the final frame.
5. Play the completed cache from start to finish with audio.
6. Exercise pause, seek, frame-step, and repeated DLSS off/on comparison switches.
7. Confirm original and neural frames remain within one source frame and audio remains synchronized.
8. Require no sustained playback drops and no more than 0.5% total dropped frames after the initial two-second decoder warm-up.
9. Reopen the same source and prove the compatible cache is reused without neural re-rendering.

The local release tag is created only after the automated suite, package verification, and this end-to-end acceptance test pass. RTX 40 execution cannot be called hardware-verified unless an RTX 40 machine is actually tested; policy tests and community compatibility evidence are reported separately.

## Documentation updates

Update README, architecture, building, setup, troubleshooting, related-project, security, third-party, and public-release notices to describe:

- pre-render-first behavior and why buffering alone is insufficient;
- 1080p default/highest-available fallback;
- removed 480p/720p manual options;
- persistent cache management;
- original/neural comparison semantics;
- DLSS spatial upscaling off by default;
- public-versus-experimental runtime boundaries;
- measured Resident Evil results; and
- honest RTX 40/50 test coverage.

## Non-goals for v0.12.0

- Real-time 1080p neural rendering guarantees.
- Neural upscaling from a lower source resolution to 1080p or higher.
- Manual 480p or 720p quality selection.
- Arbitrary-frame resume of an interrupted temporal neural render.
- User-export workflow or saving the rendered file beside the source.
- DRM, authentication, cookie extraction, or protected-video circumvention.
- Redistributing leaked, patched, or otherwise unauthorized proprietary DLLs.
- Claiming RTX 40 hardware verification without an RTX 40 test system.
