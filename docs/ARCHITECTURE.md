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

`VideoDecoder` and `AudioPlayer` take an optional `Settings` on construction.
Default-constructed is production - helpers are resolved relative to the module,
then PATH - and the shipping player always default-constructs; `Settings` exists
so tests compile the same code the release does instead of a second program
behind an `#ifdef`. `Settings::helperDirectory` overrides that search and
`Settings::faults` reaches Win32 failures no test can provoke, so both are public
in a release build with no production caller. `YouTubeResolver` is deliberately
different: it canonicalizes its helper directory, refuses reparse points and
holds `yt-dlp.exe`/`deno.exe` open before spawning them, so its injecting
constructor stays behind `YOUTUBE_RESOLVER_TESTING` and two configure-time
compile checks plus `ReleaseApiCompileTests` assert a release build cannot reach
it.

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

The engine is asked for both prediction directions and for its global flow
estimate, and capability is given up one rung at a time - both directions with
global flow, both alone, forward with global flow, forward alone - so a refusal
costs one feature rather than the engine; the mode that came up is in the `NVOFA
ready` line, and each rung gets a fresh session because a refused `nvOFInit`
leaves one in a state the SDK offers no way to reset. The reverse field is what
the resolve pass gates on: `src/FlowGate.h` holds the scale-free forward/backward
criterion as one `constexpr` function (Sundaram/Brox, alpha 0.01, beta 0.5 px²,
literature defaults that nothing here has measured yet), and
`src/NvofResolveShader.h` holds the pass's HLSL so those two numbers reach the
shader by stringification instead of a second copy - and so `UpscalingTests` can
compile the pass with `D3DCompile` on a machine with no flow engine to run it on.
A cell the engine contradicts itself about, an occlusion or a repeating pattern,
emits no motion instead of a confident wrong one. A device that offered only
forward flow is passed a zero cell pitch, which takes the gate out of the shader
rather than neutralising it, so it behaves exactly as before. The global vector
arrives as a four-byte readback latched behind the fence the next submit signals,
a frame or two behind the field and never stalling; nothing consumes it yet. The
cost surface is still ungated: its scale is unpublished, and the round-trip mask
is what it will be calibrated against.

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

The criterion is validated rather than asserted. `tools/benchmark/cutmirror.py`
mirrors the cut path in `TemporalGuides.cpp` - the analysis grid, the stratified
cell luma, the global search with its distance penalty and its refusal to prefer
a marginal shift, the histogram intersection, `ClassifySceneCut` and the 0.3 s
weak-arm debounce - and `tools/benchmark/cutlab.py` replays it over a corpus
whose cuts are labelled in the manifest, so the thresholds can be swept without a
GPU. Measured 2026-09-14 over nine clips and 1212 consecutive pairs, the shipped
0.30 / 0.10 / 0.85 criterion finds every cut in `cuts-motion` and takes one reset
too many on it, misses cuts between shots that share a luma histogram, and takes
a flash for a cut. The scale-free alternative - compare the winner against the
zero-displacement cost `EstimateFlow` already discards and decide on the fraction
of cells whose match failed - was implemented in the mirror and swept beside the
shipped shape; the two reach indistinguishable best operating points, so the
thresholds stand and the fraction lives in the harness as the measurement that
justified leaving them alone. The consequence when changing the generator:
`AnalysisGrid`, `DownsampleLuma`, `ClassifySceneCut` and `MinFramesBetweenCuts`
have a second reader, and it is Python.

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

GPU calls a test cannot make - the fence wait and signal, the device-removed
reason, the capture readback - are reachable through one nullable
`D3D12RendererTestHooks` pointer, null in every production renderer, so the
class has the same size in every translation unit and each site falls through
to the real call.

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

The job reaches the decoder, the evaluator and the encoder through three
duck-typed adapters. A second constructor injects `IFrameSource`,
`INeuralFrameEvaluator` and `IFrameEncoder` instead, plus the evidence
provider, clock and pause predicate the job would otherwise take from the
ReShade log, the steady clock and `NeuralRenderRequest::pauseEvent`;
`Run` picks between the two sets at runtime by whether anything was injected,
and a partial injection is a Protocol failure rather than a silent fall back
to the real decoder. The production set is not rebuilt per call: it lives in
the renderer's retained state, so a resident helper keeps the device, the NGX
instance and the feature-18 workset across jobs - see *The helper is resident*
below. An injected set is per-call and never builds one, which is also why an
injected run reports no residency.
Both sets compile in every build. `OFFLINE_NEURAL_RENDERER_TESTING` used to cut
the production half out of this translation unit, and it was a link-closure tool
rather than a testing policy: when it arrived `NeuralPrerenderTests` compiled
three sources and linked `bcrypt shell32`, so the production adapters' references
to `D3D12Renderer`, `DLSSBackend`, `TemporalGuides`, `OpticalFlowNvof` and `Log`
had nowhere to resolve. Removing the macro means paying that closure instead: the
test target compiles those four sources plus `RuntimePolicy`, links `DLSS_LIB_DIR`
and the player's full library set, and takes the NVOF helper. That is a slower
test build which now depends on the DLSS libraries being staged.

Two things pay for it. A change to `D3D12Renderer`, `VideoDecoder` or
`DLSSBackend` that breaks the job's use of them now breaks the test build too,
which the `#else` hid by compiling no production adapter anywhere. And residency
becomes assertable at all: under the macro `Retained` had no members,
`ReusableForAnotherJob` returned a constant `true`, and both footprint calls
reported zero, so no test could observe a resident helper. What the macro did
guarantee, and the runtime choice does not, is that production code could not run
inside a test binary; the Protocol failure above is what replaces it. Injecting
fakes still does not exercise the production adapters.

Before every render the player verifies the staged runtime against the
embedded `packaging/runtime-lock.json` (size, SHA-256, file version) and
refuses drift instead of adopting a newer stack. On a cache miss the helper
first runs `--neural-preflight`: a synthetic Feature 18 probe whose JSON
receipt records GPU, driver, ReShade/RenoDX/DLSS-NR banner versions, every
locked module's hash and every feature-18 creation/evaluation observation.
After the render, `receipt.json` (preflight, lock checks, request, result,
timing, digests) is written beside `neural.mkv`, hashed into the schema-5
manifest and summarized in one log line.

The receipt also records what the scene-cut classifier did over the job: cuts
accepted on the strong arm, cuts accepted on the weak arm, and weak cuts the
minimum-interval debounce withheld. A suppression is either a flicker avoided or
a cut missed, so these are the numbers a sweep over labelled clips scores itself
against - and labelled real footage has now bracketed the window from both
sides. The only transient in the corpus returns 4 frames after the cut that
opened it, and the shortest genuine shot in it is 17 frames, so the window must
exceed 4 and must not exceed 17 - suppression is `since_cut < min_frames`, so a
17-frame window still accepts a cut 17 frames out; the shipped 0.3 s sits
between them. It was
0.6 s, which is 18 frames at 30 fps, and discarded a hard cut 17 frames after
its predecessor - the neural pass then kept accumulated history across a genuine
discontinuity. The counters count the job's guide generator over its whole life -
preroll and every encoder attempt included - and a re-evaluated frame counts
once, so they are not bounded by `historyResets`.

The runtime directory has exactly one writer at a time. A job holds a
session-scoped lease (a named mutex derived from that directory) from the
settings write until the helper exits, so a second player instance cannot
interleave its neural settings or its proxy log with this render; it is
refused with a distinct preflight failure instead. The helper still selects
its log by session, because a crashed holder can leave a file that Windows
will not let the next launch delete.

Which machines reach any of this is two independent decisions, and they are kept
independent. `ClassifyGpu` answers what the part is, which picks the cache
identity, the receipt label and the render-pace prior; `NeuralAddonDesired`
follows from RTX-ness alone, so a generation is never the reason the addon is
withheld. The driver, classified against `kNeuralDriverFloor`, is the axis that
refuses. `RenderPacePrior` returns zero for every generation nobody has timed,
and zero means unmeasured, not unsupported: a forecast built from it reports no
verdict and keeps the full start cushion, where a prior-backed forecast reports
one and shortens it. Both splits are pinned in `tests/PolicyTests.cpp` against
the adapter and driver strings the machines in the field notes reported.

Which adapter the device actually got is logged once per creation - description,
LUID, vendor, dedicated memory - beside the adapter `DetectHighPerformanceGpu`
picked, compared by LUID rather than by model name, because on a hybrid laptop
the two can differ and then the cache identity, the receipt's GPU label and the
pace prior all describe a part that did not render. Both binaries also export
`NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance` from
`src/GpuPreference.cpp`: the driver reads them from the main module at process
launch, so there is nothing to call and the worker - the process that loads the
neural stack - needs its own copy.

Every render also reports what its cold start cost, phase by phase, because the
persistent-helper work is judged in wall-clock and nothing measured it before:
the player's request, the preflight probe, the launch, then the helper's own
boundaries (process creation to entry point, entry to runtime ready, source open
through NGX init, feature 18 armed, first output) and finally the attach. They
travel as a protocol v6 `Timeline` message, land in the receipt beside `timing`
and in one log line, and a phase that did not happen is absent rather than zero -
a cache hit, a single-file job and a refused request each report less than a
segmented render, and that difference is information. The helper's five phases
reach that line because the reader raises them the moment the helper reports
them, not when the job returns: the job returns seconds after the attach, so a
line written at first picture used to carry five dashes while the receipt for
the same render carried all five numbers. A session that never started a helper
says so - `helper=none(cache-hit)` - because five dashes beside a real total
read as a broken instrument rather than as a render that never happened.

Measured on an RTX 4080 SUPER, the helper side is 2.1-2.6 s, of which NGX init
and feature arm are 95 %. From a driven player session the whole toggle costs
**8.39-9.18 s on the first toggle with the preflight verdict and the cache
cleared** and **4.88-5.16 s on every later one** over ten sessions; the 3.8 s
difference is the feature-18 preflight probe, a second helper process whose
verdict is cached per runtime identity. Of the warm 5 s, `neuralInit` plus
`featureArm` is 2.10 s and per-process, so that is what a resident helper
removes. `firstOutput` and the attach are not removable that way, which put the
estimated floor near 2.9 s - arithmetic on measured phases, and reuse later beat
it by also shortening `firstOutput`. See
`docs/VERIFICATION-2026-09-14-RTX4080.md` and `docs/VERIFICATION-matrix.md`.

**The helper is resident, and the protocol runs both ways to make that possible.**
Until v6 the metadata pipe was one-way and a job could only arrive as argv, so a
process served exactly one render. v6 adds a command channel - `Hello`, `Job`,
`Cancel`, `Shutdown`, answered by a new outbound `Ready` - on a second inherited
pipe passed as `--command-handle`. A `Job` carries the argument vector the helper
already accepted on its command line, so `ParseWorkerArguments` remains the single
definition and single validator of what a job is; residency changed how a job
arrives, not what one means. Without `--command-handle` the helper behaves exactly
as before, one job then exit, which is the path the benchmark harness and the
preflight probe drive.

Five decisions shape it. The helper is reused only while `(runtime directory,
runtime digest, neural-settings digest)` matches, because ReShade and RenoDX read
their INI at process start and there is no way to re-read it in place - a settings
change must relaunch. The runtime lease is held only while a job runs, so an idle
resident helper never locks a second player instance out of the runtime directory,
and every job re-verifies the runtime lock under that lease rather than trusting
what it checked at startup. The helper exits itself after 30 s idle, which bounds
the ~1 GiB of feature memory DLSS deliberately does not free on
`ReleaseFeature`; keeping that memory is the trade, and the idle timeout is what
makes it survivable. If the next job's geometry matches, the NGX feature is kept
and the arm is skipped too. And orphan safety is two mechanisms, not one: the job
object still carries `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, and the helper also
waits on the parent's process handle, because the failure being prevented is a
process holding the GPU with nobody to reap it.

Re-entering a render was where the work was. State that had never outlived a job
had to be found and reset per job: the evaluator's successful-evaluation count
(which becomes `nativeEvaluations`, so a carried value inflates every later job),
the guide generator's scene-cut tally and history generation - whose `Reset()`
deliberately preserves the tally because it is evidence about a whole job, so the
generator is replaced rather than reset - a posted readback copy still holding a
capture slot, and `D3D12Renderer`'s peak-VRAM high-water mark, which was a running
maximum with no reset because nothing had ever needed one. A reused job reports no
`neuralInit` and no `featureArm` in its timeline, because it did not pay them, and
a session answered from the cache without any helper says `helper=none(cache-hit)`.

**Measured, and it pays.** The acceptance criterion is a warm toggle under 3 s in
a driven player session. A reused helper puts a neural frame on screen in
**2.44-2.52 s over four sessions**, median 2.47 s, every one reporting
`plan=reuse`, against 5.23-5.41 s for the first toggle in the same process. The
reused job's timeline carries no `helperStart`, `runtimeReady`, `neuralInit` or
`featureArm` - 2.19 s it did not pay because no process started - leaving
`firstOutput` at 1.06 s and the attach at 1.29 s, which are exactly the two the
arithmetic said residency cannot remove. Residency is reached only when the
second job's range is not already covered by the first one's published entry; a
toggle inside that coverage is answered from the cache in about 0.8 s with no
helper job at all, which is correct and is not this measurement. See
`docs/VERIFICATION-matrix.md`.

`NeuralCacheManager` stages source and render artifacts in `cache/v1` beside the
executable, which is the default root; LocalAppData is the legacy fallback used
only when the portable directory is not writable. Source, application version,
GPU path, runtime digest, native dimensions, quality, upscaling state, and a
canonical neural-settings digest form the render identity.

**The identity covers the driver and the weights, as of 2026-09-14.** It did not,
and the gap was a correctness defect rather than a performance rider: `gpuPath` is
a generation label, so every Ada card on every driver shared one value, and the
lookup's validity check tests the same terms - so a render produced on
32.0.16.1047 was served *and* validated on any later driver. The gap was wider
than the missing version string. `runtimeDigest` hashes staged files, but every
run resolves its models out of the driver store (`NGXGetPathUsingQAI` →
`...\DriverStore\FileRepository\nv_dispsi.inf_...`) and
`%ProgramData%\NVIDIA\NGX\models`, neither of which was in that set and both of
which a driver update or a model refresh can replace with the digest unchanged.

Two terms close it. `driverVersion` enters the key directly, so a render cannot
cross a driver change. `modelStoreDigest` covers the resolved model-path
contents, so it cannot cross a model refresh on one driver either; when a root
cannot be enumerated the digest falls back to the driver version alone, and
`ResolveNeuralModelStore` records which of the two it got in the preflight
receipt rather than degrading silently. The manifest schema moved 4 → 5 in the
same change, which retires every entry written under the old identity through the
schema gate rather than incidentally through a missing field.

**Two runtime file sets exist, and they are deliberately different sizes.**
`LockedRuntimeFileNames()` is the thirteen files hashed into `runtimeDigest` - the
twelve vendor modules plus `NeuralWorker.exe` - and it is the set a preflight
failure quotes. `LockPinnedRuntimeFileNames()` is the twelve that
`packaging/runtime-lock.json` pins and `VerifyRuntimeLock` checks; the worker is
never pinned, because every build of the player changes it. The worker joined the
hashed set because rebuilding it with different guide or cut logic used to leave
`runtimeDigest` unchanged, so only an `applicationVersion` bump retired the
entries it produced, and between bumps a stale hit masked exactly the change a
developer was trying to see. `-DropRenderCache` in the session harness remains
the way to force the issue during a live session.

The settings snapshot is saved beside the video and its hash is checked on reuse.
Settings are checked again after rendering before publication. Network source entries
use the canonical YouTube video ID plus stable selected-format `itag` values,
not expiring signed stream URLs. Staging entries become reusable only after
independent probing and atomic promotion. Schema 5 requires
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

A refused staging directory is no longer an unexplained `nullopt`. The manager
keeps the cause, the filesystem error and the directory it attempted; the
constructor's verdict survives on an invalid manager because no attempt can get
past it, and each refusal writes one log line with the path, the cause, the error
number and whether the ownership check rejected it. The player reads that record
for its message, so an unwritable install directory, a rejected key, a failed
create and a directory that resolved outside the root are four different
sentences instead of one, and a root that cannot be created at all is said at
startup rather than at the first render.

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
as a protocol v6 `Segment` message (index, absolute first pts and frame number,
frame count, frame duration, file name). Temporal history, priming and preroll
are untouched — only the encoder rotates.

The parent refuses any header whose version is not exactly `kVersion`, so a
helper left in `neural-runtime/` by an older build fails closed instead of
having its shorter result read as a longer one.

The player collects those messages into a `NeuralSegmentIndex` and hands it to
`SynchronizedPlayback::OpenLive`, which pairs the original against the growing
set, rebasing each segment with its own first pts and opening the next segment
before the current one runs out. Reading video nobody has rendered returns
`WaitingForRender`, which the player treats as "buffer" or "play the original",
never as "stop". When a job ends the segments IT published - selected by run id,
not by position - are concatenated (`ConcatenateMedia`) into the single
`neural.mkv` the cache promotes, so the next open is an ordinary cache hit.

Coverage is a **set of rendered regions**, not a head. A session renders the
whole video (or the marked range) hole by hole, nearest the playhead first, and
a viewer who seeks backwards makes the next job start behind an earlier one. So
the index is sorted by timestamp rather than by arrival, each segment carries the
`runId` of the job that published it, and `NeuralCoverage.h` holds the timeline
algebra over those regions: `MergeSpans`, `UncoveredSpans`, `NextRenderTarget`
(the hole under the playhead, else the nearest ahead, else the earliest behind),
`SpanContaining` and `CoveredFraction`. `live_session::ShouldRetarget` decides
when to move a running job, and it compares regions rather than endpoints -
a frame-snap residual of five ticks between a hole's start and a job's range
once read as different work and relaunched the helper on every tick.

What replaced what: the session used to render one forward run from the playhead
and "rebase" on a seek out of it, which stopped the session and deleted every
rendered segment the new playhead was not inside. Retargeting keeps them. The
index's `Finished()` flag is gone with it: whether a session has more to do is a
question about coverage against its range, and one job ending answers only for
its own hole.

Both the coverage test and the lookup that picks a segment run on frame numbers,
which are exact on the CFR grid, because a seeked FFmpeg source stamps its
timestamps a few ticks below it. The index also closes the sub-frame hole each
seam would otherwise carry: a segment's exclusive end is rebuilt from an integer
frame duration, so at 30000/1001-style rates it lands a couple of ticks under
the next segment's own first pts, and a playhead inside that hole used to be
reported as a producer contract break.

Entering a segment part-way is a seek inside that file, and a segment holds one
keyframe at its own start, so the first frames it hands back are behind the
playhead. In live mode the pair builder walks over them; in cached playback a
numbered disagreement stays the hard identity failure it is meant to catch.

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
the neural pass, set the pace. The player therefore keeps the last five measured
paces per source geometry per GPU and predicts from their median, falling back
to the seed only until the first session has measured the machine itself. The
median is what makes the record survive one bad sample: contention inflates a
measurement and never deflates it, so a single session measured under load used
to persist as the machine's pace and make the forecast refuse work the card does
comfortably. Five samples and a median let the measurements outvote the outlier,
and the minimum is deliberately not used - this forecast exists to refuse
sessions that cannot keep up, so erasing slow evidence is the wrong failure.

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

The harness under `tools/benchmark/` is deliberately not a second implementation
of what it scores. Its cell grid, cell luma, scene-cut thresholds and cut
debounce are read off `src/TemporalGuides.cpp`, so per-pixel temporal sigma, the
false-motion rate, the cell flip rate and cut precision/recall describe the field
the guide generator actually solves on, and a threshold swept in Python transfers
to the runtime without a second calibration. The manifest's hard-cut indices are
the ground truth for the cut score, which is why `corpus.py` records them. The
consequence when changing the generator: `AnalysisGrid`, `DownsampleLuma`,
`ClassifySceneCut` and `MinFramesBetweenCuts` have a second reader, and it is
`analyze.py`.

Durable mid-job resume is deliberately a from-zero relaunch: a validated
segment checkpoint would have to carry the temporal neural state at the
boundary (a preroll re-evaluation, not just frame indices and encoded
segments), and the relaunch bound already covers the observed failure
modes. Compose subtitles after enhancement, with burn-in only as an explicit
export choice. These are pending ideas, not current features or release
commitments.
