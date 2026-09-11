# Architecture

## High-level pipeline

```text
Local file                        YouTube URL
  |                                 |
  |                                 +-> resolved stream -> playback now
  v
Source + runtime lock + settings cache lookup
  |
  +-> hit:  validated neural cache -> synchronized playback
  |
  +-> miss: the original plays; the user chooses what to render
            (one frame / 4 s clip / marked range / whole video)
  |
  v
Render request
  |
  +-> network source without a cached copy: acquire it once
  |     (re-resolves the page URL once when the stream URLs expired)
  |
  +-> NeuralWorker.exe --neural-preflight (Feature 18 probe receipt)
      NeuralWorker.exe --neural-worker
        sequential decode from range start - preroll -> temporal guides
        -> GPU guide expansion -> native DLAA carrier + RenoDX feature 18
        -> capture -> encode -> independent validation -> receipt -> cache
  v
Original + validated neural cache -> synchronized decoded frame pairs
  |
  +-> chosen view -> optional playback SR -> image adjustments -> D3D12 display
  |
  +-> cached neural frames -> PNG/JPEG/GIF or MP4/MKV export
```

Opening media never starts a whole-video render and never waits for a download.
A local open is identified against the cache and replays a validated entry; a
YouTube open plays the resolved stream. Acquisition belongs to the first render
of a streamed source, which stages one complete copy in the cache. A source
played from that copy keeps its YouTube identity for cache and history while
decode, seek and audio are ordinary local-file work; only a live stream uses the
non-blocking read and the re-resolving seek path.

The main player has no ReShade proxy. The worker hosts the experimental runtime
in `neural-runtime/`. Source/core builds without that runtime use the native
playback path.

## Decoder

`VideoDecoder` uses FFmpeg as the primary decoder by launching `ffmpeg.exe`/`ffprobe.exe` as helper processes. Media Foundation is kept as a fallback path.

Playback preserves the decoded source dimensions. Selecting an SR output never
downsamples a source to fit a nominal DLSS quality ratio.

## Timing

Audio is the preferred master clock. The video side checks decoded timestamps against that clock. Frames that are too late are discarded and temporal history is reset rather than slowing playback.

## Temporal guides

A normal movie does not contain engine motion vectors or depth. Motion comes from
the GPU's optical flow engine where there is one, and from image analysis where
there is not; depth and scene cuts are always derived from the image.

`OpticalFlowNvof` drives NVOFA, the dedicated flow engine on Turing and later
cards, through the Optical Flow SDK's D3D12 interface. It compares the decoded
frame against the previous one on a 2x2 pixel grid and returns S10.5 fixed point,
so one vector covers four pixels and resolves a thirty-second of one. The engine
is asynchronous: uploads and the flow capture are recorded into their own command
list, the queue is signalled past it, and the engine waits on that signal and
signals back when the field is ready. The frame's own list waits on the GPU for
that second fence, so nothing stalls the CPU. Temporal hints are off, which keeps
each pair independent of whatever preceded it - the engine's own hints survive a
scene cut. Vectors are read with a nearest fetch rather than a filtered one:
across a disocclusion the neighbouring cells describe different surfaces and
interpolating them invents a vector no cell measured.

`TemporalGuideGenerator` still runs. It owns the depth proxy and the scene-cut
decision, and it owns motion too on any machine without the engine - block
matching on a compact grid, gated on how much the winning displacement beats
standing still and verified by a reverse search where that margin is ambiguous,
so cells whose match is not trustworthy carry no motion at all. That estimator
analyses a 160x90 grid, which is one vector per 24x24 source pixels at 1080p with
a finest step of three pixels; measured against a synthetic pan it accepts none
of its cells at half a pixel per frame and two per cent at one pixel, which is
why the engine is preferred wherever it exists. D3D12 expands whichever field was
produced to the exact DLSS render dimensions.

The engine is used only when the frame handed to the renderer is already the DLSS
input size. That holds on every neural path; the runtime Super Resolution toggle,
where the two differ, keeps the estimator. A machine without `nvofapi64.dll`, an
older card or a build configured without the SDK headers all fall back the same
way, and the log says which backend came up.

Every decoded frame carries a `FrameIdentity` (frame number on the CFR
timeline, timestamp, source generation, history generation, job id, reset
reason). `Generate` classifies each reset (first frame, seek, drop, cut,
source change, retry, preroll) and stamps the identity on the guide; the
renderer refuses a guide built for a different source frame, derives the NGX
reset from the guide's reason and logs it; the offline job rejects captured
output whose identity does not match the submitted frame. A cut requires both
a high post-alignment residual and a low luma-histogram overlap, so fast pans
keep history and real cuts never do. Guide controls (motion, depth) can each be
neutralized for ablation; the choice is part of the cache identity. There was a
third guide, a correspondence-failure mask bound to NGX's bias/disocclusion
parameters; it was deleted after measurement showed neural rendering and the
upscaler both ignore it (see `docs/BENCHMARK.md`).

## D3D12 renderer

`D3D12Renderer` owns:

- device / queue / swapchain;
- six frame slots, each with two command allocators and lists: one for uploads and
  the optical-flow capture, one for the frame itself;
- per-frame video/guide upload resources;
- linear FP16 DLSS color input;
- typeless depth resource with DSV/SRV views;
- motion-vector resource;
- DLSS output UAV;
- final presentation/debug pipelines.

Normal playback does not flush the GPU every frame. Fence waits happen only when a frame slot is reused before completion or during operations that require a hard synchronization point such as seek/reinitialization.

During neural pre-render, `RenderFrameForCache` copies the evaluated output to a
dedicated readback resource and emits tightly packed BGRA frames to a bounded
FFmpeg encoder process. The same persistent NGX/feature-18 session is retained
across the sequence; an add-on-requested feature recreation does not break the
job's monotonic successful-submission count.

The renderer also holds a source-size reference texture (the original member
of a synchronized pair) so the presentation shader can show Blend, Split, Wipe
and Zoom comparisons instantly without re-rendering; cache capture always
samples the neural output with identity constants. Timestamp queries around
the DLSS evaluation and a per-frame local VRAM sample feed the render receipt.

## Offline neural job and cache

`OfflineNeuralRenderer` validates the requested range, primes feature 18,
reopens the source at `start - preroll`, evaluates the preroll frames without
capturing, captures `[start, end)`, rejects non-monotonic timestamps or
non-consecutive frame numbers, and finishes the encoder. Encoded timestamps
start at zero; the absolute start is recorded in the manifest and the
receipt so synchronized playback and export realign to the source.
If NVENC cannot start or write, the entire sequence restarts from zero with
software H.264 rather than splicing incompatible temporal histories.
A frame whose evaluation fails (or stalls) is retried a bounded number of
times with the same identity; exhaustion fails the job as retry-exhausted
and never skips the frame. Device removal fails the job immediately and the
launcher relaunches the helper from zero at most once. The helper checks an
inherited pause event between frames and resumes without a temporal reset.

Before every render the player verifies the staged runtime against the
embedded `packaging/runtime-lock.json` (size, SHA-256, file version) and
refuses drift instead of adopting a newer stack. On a cache miss the helper
first runs `--neural-preflight`: a synthetic Feature 18 probe whose JSON
receipt records GPU, driver, ReShade/RenoDX/DLSS-NR banner versions, every
locked module's hash and every feature-18 creation/evaluation observation.
After the render, `receipt.json` (preflight, lock checks, request, result,
timing, digests) is written beside `neural.mkv`, hashed into the schema-4
manifest and summarized in one log line.

The runtime directory has exactly one writer at a time. A job holds a
session-scoped lease (a named mutex derived from that directory) from the
settings write until the helper exits, so a second player instance cannot
interleave its neural settings or its proxy log with this render; it is
refused with a distinct preflight failure instead. The helper still selects
its log by session, because a crashed holder can leave a file that Windows
will not let the next launch delete.

`NeuralCacheManager` stages source and render artifacts under LocalAppData.
Source, application version, GPU path, runtime digest, native dimensions,
quality, upscaling state, and a canonical neural-settings digest form the render identity.
The settings snapshot is saved beside the video and its hash is checked on reuse.
Settings are checked again after rendering before publication. Network source entries
use the canonical YouTube video ID plus stable selected-format `itag` values,
not expiring signed stream URLs. Staging entries become reusable only after
independent probing and atomic promotion. Schema 4 requires
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
compatible subtitles, attachments, metadata and chapters into a new MKV. It also
encodes PNG/JPEG single frames, palette GIFs at 50 fps, and H.264/AAC MP4 with
compatible text subtitles. An owned,
cancellable FFmpeg process writes a unique sibling stage, published without
overwriting an existing destination. Export has no render or subtitle-composition
pass. Range renders trim the exported source audio, subtitles and chapters to
the rendered range. Preferences use the existing executable-adjacent INI.

Still-image demuxers produce one frame at 1 fps with a one-second cache carrier.
Neural feature warm-up can reuse that frame up to 120 times; capture reopens the
source and encodes exactly one frame. GIF decoding uses a 100 fps carrier so
variable centisecond delays survive synchronized processing. Photos bypass
hardware video decoding; odd dimensions use 4:4:4 software cache encoding.
Each encoder attempt captures its first source frame until a fresh runtime
receipt arrives, retaining only the latest pixels. This is bounded to 120 captures
and does not extend the exported timeline; unchanged or failed runtime evidence
still rejects the render. JPEG EXIF rotation is included in decoded dimensions.
Automatic cache selection probes `<exe>/cache/v1` before the LocalAppData fallback.

`SynchronizedPlayback` opens the original and neural files together, validates
their geometry/rate/duration, and publishes timestamp-matched frame pairs.
Neural Rendering defaults on for a fresh install and then follows the saved
view preference. The selected member is chosen after the first pair is ready
and before it is presented. Toggling Neural Rendering changes the visible member of the
last-presented pair, so comparison never advances ahead of the audio clock.
Seeking waits for both restarted decoders to produce a pair; temporary
`NotReady` results do not unload playback. Tail seeks account for container
duration padding with a bounded earlier-frame retry.

## Active neural session

A job can also run behind live playback. `NeuralRenderRequest::segmentFrames`
makes the helper rotate its encoder every N captured frames: the next segment's
encoder starts before the current one is finished, finalization runs on a
private FIFO thread, and each finished file is announced over the metadata pipe
as a protocol v3 `Segment` message (index, absolute first pts and frame number,
frame count, frame duration, file name). Temporal history, priming and preroll
are untouched — only the encoder rotates.

The player collects those messages into a `NeuralSegmentIndex` and hands it to
`SynchronizedPlayback::OpenLive`, which pairs the original against the growing
set, rebasing each segment with its own first pts and opening the next segment
before the current one runs out. Reading past the render head returns
`WaitingForRender`, which the player treats as "buffer", not "stop". When the
job ends the segments are concatenated (`ConcatenateMedia`) into the single
`neural.mkv` the cache promotes, so the next open is an ordinary cache hit.

Both the coverage test and the lookup that picks a segment run on frame numbers,
which are exact on the CFR grid, because a seeked FFmpeg source stamps its
timestamps a few ticks below it. The index also closes the sub-frame hole each
seam would otherwise carry: a segment's exclusive end is rebuilt from an integer
frame duration, so at 30000/1001-style rates it lands a couple of ticks under
the next segment's own first pts, and a playhead inside that hole used to be
reported as a producer contract break.

Sizing follows measurement rather than preference, and the measurements moved a
long way during the work described below. `playback_timing::ForecastLiveRender`
is seeded with **12.50 ms per 1080p frame, 16.60 ms at 1440p and 28.07 ms at
4K**, measured on an RTX 5090 from the spacing of segment arrivals so job
startup is excluded. Fitting those three points gives **7.35 ms of fixed cost
per frame plus 2.50 ms per megapixel**, reproducing each to within 0.06 ms: the
fixed part is the guide pass, the DLSS evaluate and the capture's fence wait,
the proportional part is the readback and the pixel work.

Those constants are only the seed for a machine that has never run a session.
0.17.0's pipelined capture and parallel guides moved the same GPU and the same
clips to **8.4 ms at 1080p, 15.4 at 1440p and 42.0 at 4K** (medians; see the
[0.17.0 RTX 5090 record](VERIFICATION-2026-09-10-RTX5090.md)), which no longer
fit one line: 1080p and 1440p came down 29% and 10% while the 4K figure did not
move, because that clip is a 6.3 Mbit/s re-encode whose decode and encode, not
the neural pass, set the pace. The player therefore keeps one measured pace per
source geometry per GPU and predicts from those, falling back to the seed only
until the first session has measured the machine itself.

Per *job* there is also about 7 s of fixed cost — the preflight process, ReShade
stabilization, up to 120 priming frames, the reopen and seek, and 60 preroll
frames — which is why a session is one long job rather than a chunk per few
seconds, with a 4 s lead-in and a 2 s resume threshold.

Inside a live session the rate holds up: on a 40 s native 4K30 source the median
over nine segment intervals was **1.165x real time** against the forecast's
1.188x, so the player's own decoding and presenting costs about 2%. A heavier
file at the same geometry is a different answer - the 4K re-encode above runs at
0.78x and buffers continuously - so the forecast asks before starting whatever
it expects to fall behind, and buffering remains the release valve rather than
an edge case.

A settings change while the player is paused runs the same machinery for one
frame (`NeuralJobKind::Preview`): the frame is rendered, decoded and presented
in place of the paused picture, and because it is an ordinary range render the
repeat of a setting is a cache hit.

## NGX integration

`DLSSBackend` initializes NGX, queries DLSS settings, creates the Super Sampling feature and evaluates it through the D3D12 `_C` entry point used by NVIDIA's helper path.

The default performance/quality value is DLAA. That keeps input and output at
native 1:1 resolution while preserving a real NGX feature creation/evaluation
sequence for the optional interception layer in `neural-runtime/NeuralWorker.exe`.
The main player does not load that proxy. Its independent runtime SR toggle
defaults off, selects a supported NGX input range without resizing the source,
and targets a 2560x1440 or 3840x2160 bounding box. It validates a candidate
renderer on a separate child window before swapping; failure preserves playback.
No path samples with jitter. A decoded frame is already a fixed sample grid, so a
sub-pixel offset cannot reveal new detail; it only convolves the frame with a
per-frame bilinear tent, and the neural-rendering feature does not read a jitter
offset at all. `Jitter_Offset_X/Y` are pinned to zero for every evaluation.
Frame Generation is unavailable.
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
2. Diagnostic DLSS input/motion/depth views remain unmodified.

When video is paused, adjustment changes re-present the existing DLSS output instead of decoding or reevaluating the movie frame.

## Paused-frame presentation

A frozen video frame can be re-presented while paused without advancing decoding
or neural temporal history. Cached comparison and image-adjustment changes can
therefore update the displayed frame without starting a new neural render.

## Remaining work

The shipped cache/settings/history/export work is described in [Usage](USAGE.md);
the prioritized plan is [the roadmap](DLSS5_VIDEO_ROADMAP.md). Its P0 items
(runtime preflight, benchmark, guide ablation, frame identity, stall
recovery, range preview, comparison controls) are implemented; the
measured guide ablation lives in [Benchmark](BENCHMARK.md). Of the P1 items,
confidence-aware optical flow is implemented, buffered viewing shipped as the
active session, and protection masks were measured and abandoned because the
NGX mask inputs are inert on both features. The remaining P1 work is
source-color/HDR preservation, RTX Video modes and the rest of GPU-resident
processing.

Durable mid-job resume is deliberately a from-zero relaunch: a validated
segment checkpoint would have to carry the temporal neural state at the
boundary (a preroll re-evaluation, not just frame indices and encoded
segments), and the relaunch bound already covers the observed failure
modes. Compose subtitles after enhancement, with burn-in only as an explicit
export choice. These are pending ideas, not current features or release
commitments.
