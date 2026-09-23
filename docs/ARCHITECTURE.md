# Architecture

_Verified against 0.25.0 (1988cac) on 2026-09-22._

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
  |
  +-> stage export: Super Resolution -> neural -> frame generation -> one file
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

That rule assumes a late frame is cheap to throw away, which holds while playback advances by decoding ONE stream. A live neural session advances by decoding a PAIR - the original and the rendered segment, presented together - so a discard costs exactly what a present costs, and a catch-up loop that discards is a race it doubles the length of: once behind, every tick spends its budget discarding, presents at most one frame and ends further behind than it started. `src/PlaybackCadence.h` decides what a behind-schedule pair does instead. STRIDE presents one pair in N and skips the presentation work for the rest, which is the difference between 120 fps that cannot be shown and 60 fps that can; RE-ANCHOR stops walking and seeks, because a seek costs about half a second whatever the distance while walking costs a pair decode for every frame in between. Re-anchoring is bounded at three, after which the session says it cannot follow rather than hitching indefinitely. A playback-health line every two seconds records presented rate against source rate, drops, the frame budget, the cadence in force and the split between guide generation and the present - the measurement that disproved two plausible theories about where the time was going.

Throughput is what makes that budget reachable at all. A pair is two decoders feeding one renderer, and both hand over NV12 rather than BGRA where the source's colour description allows it: 5.5 MB per 2560x1440 frame instead of 14.7 MB, with the BT.709 limited conversion done on the GPU. `SourceNv12ConversionFor` refuses any description that conversion does not implement, and a playback open takes a narrower gate than an export one because the comparison reference is a BGRA texture uploaded from CPU bytes.

## Audio

The output is WASAPI shared mode, event driven, at the endpoint's own mix
format. `AudioPlayer` opens the endpoint first and then tells FFmpeg to
produce exactly that format, which removes the two conversions the old
`waveOut` path paid: it was opened at a fixed 16-bit 48 kHz and Windows
converted again to whatever the device actually wanted. The clock the video
side reads is `IAudioClock`'s played-frame count, not an estimate of what was
submitted.

Which track plays is a decision, not the first stream. `AudioTrackPolicy.h`
reads the four dispositions a container carries - commentary, visual
impaired, descriptions, hearing impaired - and picks the container's default
among the tracks that carry none of them. Titles are never parsed: they are
free text in an arbitrary language. A source with fewer than two tracks
builds no list, so nothing downstream changes for the common case.

Every stop is ramped. `AudioFadePolicy.h` decays the last frame the endpoint
was handed to silence over four milliseconds and opens the next stream with
the matching ramp, because a seek otherwise cuts mid-waveform and the step is
a click. The tail has to wait for room - the reader fills whatever the
endpoint asks for, so at the moment a seek arrives the buffer is typically
full - and the stop has to wait one buffer beyond what `IAudioClock` reports,
because that clock leads the speaker.

Device changes are noticed rather than tripped over. Polling a call's return
value only ever catches an endpoint that disappears; a default-device change
leaves the old one working, so nothing fails and playback continues on the
device the viewer stopped using. `IMMNotificationClient` and
`IAudioSessionEvents` both run, `AudioEndpointPolicy.h` decides which
notifications are about this stream, and a watchdog covers drivers that stop
requesting data without erroring. The player follows the console and
multimedia roles and not communications, so a call does not move a film's
audio. Recovery reopens and restarts the source rather than splicing into the
new device, because its mix format may differ and the decoder has to be told.

Passthrough is the one exclusive stream. With the toggle on and an AC-3,
E-AC-3 or DTS track at a rate IEC 61937 carries (`AudioPassthroughPolicy.h`),
the renderer asks the endpoint - `IsFormatSupported` in exclusive mode, first
as `WAVEFORMATEXTENSIBLE_IEC61937`, then as the plain extensible form some
drivers want - and FFmpeg stream-copies the track through its `spdif` muxer
into 16-bit stereo frames at the link rate: the track's own rate for AC-3 and
DTS, four times it for E-AC-3. A refusal, or an exclusive stream that will not
start, closes that client and opens the ordinary shared PCM stream, and the
status line says which happened; nothing is ever silent because passthrough
was asked for. The clock is the same `IAudioClock` count divided by the link
rate. Two details keep it honest: the exclusive stream is primed with its
first buffer before it starts, because a device started empty runs its clock
through the silence ahead of the first burst; and an event the source had
nothing ready for is answered with null data rather than left to replay the
previous buffer, with that null data kept out of the played count. Two known
failures are designed against. mpv #1773, `IAudioClient::Release` hanging after
a format change: every open activates a new client, an exclusive one is stopped
and reset before its last reference goes, and that release runs on its own
thread with a two-second bound. Kodi #18453, a display-mode change dropping the
HDMI sink: an active passthrough stream is reopened 1.5 s after the last
`WM_DISPLAYCHANGE`, once the link has retrained.

### Why there is no drift correction

A player drifts A/V when two clocks each pace one half of the film. mpv
resamples audio (`swr_set_compensation`) only in `video-sync=display-resample`,
where the display refresh paces the pictures and the audio has to be stretched
to follow them. This player has one clock. `Position()` in `main.cpp` returns
`AudioPlayer::PositionSeconds()` whenever audio answers, every presentation
decision in `Tick` - ordinary playback, cached and live neural pairs, stride
and re-anchor in `PlaybackCadence.h`, frame generation's interleave - is taken
against that one value, and the value is `IAudioClock`'s played-frame count
divided by the rate FFmpeg produced the samples at. An endpoint crystal that
runs 50 ppm fast therefore plays the whole film 50 ppm fast, pictures included:
it is 180 ms short over an hour, and nothing comes apart. Correcting it would
mean resampling the audio to match a wall clock nobody is watching.

The paths that are not paced by audio have no audio to drift from: a silent
source runs on the steady clock, and so does the short bridge
`AudioClockPolicy.h` carries across a stalled clock before slewing back to the
audio at no more than 5 %. The YouTube path is a second `AudioPlayer`, the same
clock. Export muxes by timestamp and has no clock at all. A bitstream sent to a
receiver could not be corrected even if it needed to be - it cannot be
resampled - which is one more reason the audio has to stay the master. If a
display-locked presentation mode is ever added, it is the first thing that
will need compensation, and it will need it on this path.

What that argument assumes is that the clock reports what is audible rather
than something that slides away from it over a film. `AudioClockSmoke` checks
the clock against wall time for five seconds, which cannot see a slow slide, so
`tools/verification/av-drift-probe.cpp` measures it: it plays a generated clip
through the real `AudioPlayer` whose audio is a timecode (a 20 ms burst starting
on every whole second, FLAC so the edge is exact), records the endpoint through
WASAPI loopback with QPC timestamps, samples the clock on the same timeline, and
reports what the clock read as each second became audible. On the RTX 5090
machine's default endpoint (48 kHz shared mode), a 600-second run on
2026-09-23:

| Measure | Result |
|---|---|
| Bursts matched | 599 of 599 |
| Endpoint clock against QPC | -42.9 ppm (the film runs 155 ms/hour slow, sound and picture together) |
| Clock minus audible second | mean +2.146 ms, min +2.095, max +2.194: spread 0.100 ms |
| First minute against last minute | +2.128 ms against +2.163 ms |
| Trend | +0.35 ms per hour of film |

The offset between the clock and the sound is a constant 2 ms - the engine's
own latency between the position it reports and the mix loopback sees - and it
moved by a tenth of a millisecond in ten minutes, against a frame interval of
42 ms at 24 fps. The picture follows the clock by construction, so it is within
one frame interval of the sound for the whole run. Nothing here needs
`swr_set_compensation`, and P3.3's drift half was closed on that measurement.

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

The pass also applies a zero-motion test: a vector is kept only where it matches
the engine's current input to its previous one (3x3 luma SAD, both copies bound at
t3/t4) better than no motion does. The engine does not answer zero for identical
frames. On a held 960x540 frame it returned a fixed field on 47 % of pixels (up to
0.35 px), and SR, which re-samples its history by that field every frame, lost 17
VMAF in 60 frames (`docs/measurements/sr-quality-20260924/REPORT.md`). Every Super
Resolution session takes the test. The reduced processing-scale rungs run on an SR
carrier, so their cache term went to `-v2`. A neural render at source size (the DLAA
carrier) takes it too (`NeuralMotionPolicy.h`). The same field reached the model
there: with the test, a held frame renders byte for byte as it does with the motion
guide off, and its temporal sigma p99 falls from 2.15 to 1.41
(`docs/measurements/neural-mv-gate-20260924/REPORT.md`). That changes the picture,
so the render key carries `|mv-zero-test-v1`. The benchmark's `--zero-motion-test 0|1`
forces either arm of that A/B, and only the helper's own command line accepts it.

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

Those thresholds are the **Default** rung of the **Scene cuts** setting
(`scene_cut::Sensitivity` in `SceneCut.h`). **More sensitive** lowers the strong
arm to 0.18, which catches cuts between shots that share a histogram;
**Less sensitive** raises it to 0.40, which moves every real cut onto the
debounced weak arm; **Off** takes no cut from image evidence at all while
declared resets still apply. The rung travels as `TemporalSettings` - one
canonical `--temporal` argument, a receipt field and a render-key term that is
empty at the default, so every render published before the setting keeps its key
- and `cutmirror.LADDER` mirrors it for `cutlab.py --ladder`.

Frame generation reads the same header for one more test. With **Hold repeated
frames** on, a decoded pair that `IsDuplicateDecodedPair` calls the same picture
twice - mean luma change under half a code and almost no sample moved past 32
codes - is held instead of generated between; the evaluates still run, so the
runtime's history sees every frame. `tools/benchmark/duplab.py` measures that test
on corpus clips re-timed onto twos.

**Temporal stability** (`TemporalStabilityPolicy.h`, `TemporalStabilityShader.h`)
is one pass in the helper's capture path, recorded on the capture's command list
after the add-on has written the neural frame and before the cache capture reads
it, so what it produces is what is cached. It warps the previous stabilized output
by the motion NGX was given, measures along the same vector how the decoded source
changed - a brightness gain, and whatever structure the gain does not explain -
and blends the warped history in only where that structure agrees, carried
through the gain. Two history slots, each with the source frame it was made from,
let a re-submitted frame blend against the same history every time. At **Off** the
pass is not recorded and the capture reads the neural output exactly as it always
did; the capture's `PSPresent` is untouched, since the pass is its own program with
its own root signature. The rung is a `TemporalSettings` term like the scene-cut
rung, versioned (`stability-<rung>-v1`) so a change to the pass can retire what the
previous one cached.

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

Normal playback does not flush the GPU every frame. Fence waits happen only when a frame slot is reused before completion or during operations that require a hard synchronization point such as seek/reinitialization, or a window resize.

The player's renderer presents at its window's size (`SetPresentFollowsWindow`):
before each frame and each re-present, a client area that no longer matches
the backbuffers drains the queue and resizes them, and `PSPresentScaled`
scales the output into them - bilinear when magnifying, a box average over
each pixel's footprint when minifying - instead of DWM stretching an
output-sized backbuffer bilinearly. `PSPresent` is unchanged byte for byte:
the cache capture runs it, and the offline carrier never follows its hidden
window, so captures stay at the output's size.

`PSPresentScaled` is also the player's comparison compositor. What it draws
beyond `PSPresent` - the ORIGINAL / DLSS 5 tags, the swapped split - comes
from a second cbuffer (`Compose`, b1) and a texture table (t3-t4) that only it
declares, so fxc strips them from `PSPresent` and the capture program keeps
its bindings (a PolicyTests case reflects both programs). The player's
renderer takes the compositor even when the window is exactly the output's
size, because it also dithers the window's 8-bit store (below); there, one
bilinear tap at each texel's centre is the picture `PSPresent` would have
drawn. The tags are drawn by GDI
at the window's DPI into a premultiplied atlas and uploaded once per DPI; the
spatial mask on the Mix is an R8 texture at t3, read through WIC, shrunk to at
most 4096 on a side and feathered on the CPU (three box passes) before its one
upload. Neither reaches the capture: the export draws `PSPresent` with default
comparison constants, which is also why there is no "export with mask" - the
export path cannot apply the Mix at all.

Subtitles are the compositor's last layer (t5, `Compose.Subs`): a
premultiplied BGRA texture at the backbuffer's size, blended over whatever the
compositor drew, in the sRGB-encoded values subtitles are authored in. Like the
tags they need the compositor even at 1:1, and fxc strips them from
`PSPresent`, so they cannot reach the capture, the neural input or an export.
The texture is updated the way the comparison reference is - copied into one
of three upload buffers and onto the GPU by the next frame or present - and only
a new canvas size drains the queue. The pixels come from
`SubtitleOverlay`: one worker thread that probes files with ffprobe and runs an
ffmpeg child drawing the chosen stream at the picture's size. Text goes
through FFmpeg's libass `subtitles` filter (DirectWrite fonts, the MKV's font
attachments, `original_size` as libass's storage size) onto a transparent
canvas stamped with the subtitle clock; each canvas frame is first drawn onto
an opaque odd-coloured canvas and compared with the last one kept
(`mpdecimate` at zero thresholds), and only a changed frame is drawn again
with `alpha=1`, which leaves premultiplied colour and coverage - about 1.3 ms
per unchanged 1920x1080 canvas frame. Picture subtitles (PGS, VobSub) are the
stream's own frames, premultiplied and scaled. Frames arrive only on change,
each paired with its time from a `-stats_mux_pre` line on stderr; the worker
keeps two changes ahead of the clock, so the child blocks on its pipe rather
than drawing the film into memory. A seek the frames already read cannot answer
restarts the child at the target (a picture-subtitle child starts 30 s early so
a picture already up is found); a text stream inside a video is copied out
once into a small MKS first, because the `subtitles` filter reads its whole
input at every start. The decisions - track choice, sidecars, text encoding,
filtergraph escaping, timing and delay - are `SubtitlePolicy.h`.

`PSPresentScaled` dithers the backbuffer's store, on both return paths and
after the tags and the loupe, against a static 64x64 blue-noise map
(`DitherPolicy.h`), so a dark gradient reaches the screen as a fine mix of two
codes rather than flat bands: +-half a code on the 8-bit SDR backbuffer, and
+-half a 10-bit code on the HDR one (R10G10B10A2, after the PQ encode). The
offset is scaled by one minus the subtitles' coverage, which is dithering the
picture before the subtitle layer goes over it, so the video around the text is
dithered and the glyphs are not speckled. It is presentation-only and always on. The cache capture can dither its
own 8-bit store (`SetCaptureDither`, Encoder settings > Dither the cached
frames), but against an 8x8 ordered (Bayer) map rather than blue noise: its
frames go on to HEVC, whose quantiser removes exactly the high frequencies blue
noise lives in, while a period-8 tile survives it (the measurement is in
`DitherPolicy.h`). That path compiles separate entry points
(`PSCaptureDithered`, `PSCaptureLumaDithered`, `PSCaptureChromaDithered`) so
the undithered programs keep their bytecode, and it is a cache-key term
(`dither-bayer8-v1`).

During neural pre-render, `RenderFrameForCache` copies the evaluated output to a
dedicated readback resource and emits tightly packed BGRA frames to a bounded
FFmpeg encoder process. The same persistent NGX/feature-18 session is retained
across the sequence; an add-on-requested feature recreation does not break the
job's monotonic successful-submission count.

The renderer also holds a source-size reference texture (the original member
of a synchronized pair), allocated on the first upload with three upload
buffers, so the presentation shader can show the Mix, Original, Split, Wipe
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

And it records what the render did to motion (`TemporalMetrics.h`), which is what
**Advanced > Render report** reads back. The job reduces each captured frame and
its source to the guide generator's analysis grid - the source when it is
submitted, the capture when it comes back - and accumulates, in 8-bit codes of
full-range BT.709 Y'CbCr whatever layout either side arrived in: the warping
error of both after moving the previous frame by the guide generator's own flow,
per-sample temporal sigma inside each shot, and the mean colour distance and
luma shift. That is `analyze.py`'s flicker and sigma on a grid rather than every
pixel, computed where the pixels already are, at a cost below a millisecond a
frame. The helper sends them as one `Metrics` message ahead of the result; they
are receipt fields and never key terms.

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
only when the portable directory is not writable, or sits so deep that a staging
path under it would pass the 260-character limit (the root is probed with the
deepest path the cache writes; an explicit root that fails the probe is refused). Source, application version,
GPU path, driver version, runtime digest, model-store digest, native
dimensions, quality, upscaling state, the rendered range, the guide
description, and a canonical neural-settings digest form the render identity -
thirteen terms. A structured binding beside `BuildNeuralCacheKey`
destructures all thirteen, so a fourteenth field cannot be added to
`NeuralCacheIdentity` without the compiler objecting; this list is checkable
against that one.

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
contents, so it cannot cross a model refresh on one driver either. It covers
only the features the pass evaluates: the ProgramData walk skips the feature
directories and selector sections of frame generation, ray reconstruction and
the other features the pass never loads (`OutsideNeuralPass`), and a file small
enough to hash is identified by its content alone, so a refresh of unrelated
weights or an in-place rewrite of an unchanged config no longer moves every
key. When a root
cannot be enumerated the digest falls back to the driver version alone, and
`ResolveNeuralModelStore` records which of the two it got in the preflight
receipt rather than degrading silently. The manifest schema moved 4 → 5 in the
same change, which retires every entry written under the old identity through the
schema gate rather than incidentally through a missing field.

**A read of the store is trusted only once it has been quiet.** NGX rewrites
`config/versions/<n>/files` (the server config, the mapping and the deny list)
in place on every initialisation, the player's own included. It truncates each
file to 0 bytes and writes it back between 35 ms and 1.2 s later, depending on
load. A digest taken inside that window hashes an empty file. The file count,
the hashed count and the unreadable count all stay the same, so the read passed
as settled. On the development machine the render key then carried `a69cdc79…`,
which is the store with `nvngx_server_config.txt` empty, instead of `59792fa7…`.
A render keyed that way could never be looked up again, and the next start's
eviction deleted it as "retired by a changed model store". Now a file written
within `kModelStoreQuietPeriod` (2 s) of the read makes the read unsettled.
`ResolveNeuralModelStore` sleeps the rest of that period and reads again, for up
to `kModelStoreSettlePatience` (10 s), before it hands the render key a digest.
The preflight receipt takes a single read, and its `settled` field says which
kind it got. Eviction judges by the model-store term only when two resolves
`kEvictionModelStoreGap` (5 s) apart are both settled and agree
(`NeuralModelStoresAgree`). The two ways of being wrong cost very different
amounts. If eviction trusts a wrong digest, it deletes renders that still work,
which can mean hours of GPU time or a stream that has since gone away. If it
passes on a correct one, the orphans stay until the next start or the
free-space floor.

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
`verifiedNeuralFrames == frameCount` and `nativeEvaluations >= frameCount`,
where `nativeEvaluations` is the neural backend's own evaluation tally rather
than a copy of the frame count - a backend that evaluated fewer frames than were
captured is refused at render time and its entry is never reusable - plus the
NGX-only inline interception contract armed before frame capture, a feature-18
success checkpoint that advances after the captured sequence, and no feature-18
failure, skip, or pass-through marker in the stabilized job log segment.
Sequential offline decoding requests CUDA, the same as playback. It used
software FFmpeg, on the reasoning that it would otherwise compete with the
D3D12 neural and NVENC workloads; decode and encode are separate engines and
the GPU sits idle during export, so that was burning CPU time for nothing.
The existing CUDA to D3D11VA to software fallback downgrades per codec. Cache
hits retain full content-hash verification and use header-only metadata probes;
the one exception is a payload this process published itself, whose promotion
digest is reused while the file's size, write and change times and file id
are those it had when it was hashed. The payload hash takes a stop token.
Before promotion the joined entry is counted by demuxing it - one packet per
coded frame, so the count and the video span come out of the container rather
than a full decode, 0.05 s against 23.8 s on a 1440 p render - and its final
frame is still decoded. A join that lost frames demuxes short, which is what
the publish gate compares against the render's own frame count.
Invalid metadata is quarantined. Cancellation and failed
validation can never publish a partial render.

`RecentMediaHistory` atomically persists five distinct sources and their current
cache keys. Local originals are never removal targets.

Reclamation happens at startup, on its own thread, in two steps
(`NeuralCacheManager::Evict`, rules in `CacheEvictionPolicy.h`). Render entries
whose manifest this build can never serve again — a retired schema, an
unparsable manifest — are removed unconditionally: the lookup gate already
refuses them, so keeping them costs space and buys nothing. So are entries
whose recorded key environment (the manifest's optional `environment` object:
application version, installation, driver, model-store digest, beside the
existing `runtimeDigest`) no process sharing the root can rebuild: a different
driver or model store, or an older version or runtime of this same
installation. Entries that recorded nothing, and every case where this
process's own terms cannot be resolved cleanly, are left alone. Everything else
is removed only when the volume has less than `kDefaultFreeFloorBytes` (20 GiB)
free, least recently used first, and only until the floor is met. Last use is
the entry directory's write time, which every lookup stamps. An entry an
active job owns is never a target, and an entry is removed by rename, so one
another process has open stays whole.

Evict, Clear, promotion and a lookup's last-use stamp take a named mutex per
cache root (`NeuralCacheRootLockName`), shared by every player instance, with
bounded waits. Eviction re-reads an entry's stamp under the lock before it
removes it, so an entry being looked up is skipped. Clear keeps another
running instance's `live/pid<N>` and staging entries, and startup removes
`live/pid<N>` directories whose process is gone.

The trigger is free space rather than a size cap because the key retires
entries wholesale — `applicationVersion`, `driverVersion`, `modelStoreDigest`,
`runtimeDigest` and the manifest schema are all key terms, so one driver update
changes every key at once — while re-rendering a film costs minutes to hours.
A cap would have to be either small enough to delete renders on a half-empty
disk or large enough never to fire on the machine that needed it.
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
not by position - are concatenated (`ConcatenateMedia`) into the cache entry for
**that job's sub-range**. A session that filled several holes therefore leaves
several partial entries rather than one entry for the video, so reopening the
whole source later is not a cache hit; only a session that rendered its range in
one job produces that. Joining the union once coverage reaches the whole range is
not implemented.

Once that entry is published the run is served from it:
`NeuralSegmentIndex::ReplaceRun` swaps the run's records for one record over the
same window that points at the entry's payload, and retires the segment files.
The player deletes each retired file once `SynchronizedPlayback::HoldsFile` says
no decoder has it open; the one being played goes at the next boundary, where
playback crosses into the entry through the same background open and warm-up as
any other boundary, entering it mid-way at the playing file's end. The render is
still written twice - the segments, then the join - but it no longer stays on
disk twice for the life of the session, or across toggles while retained.

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

Entering a segment part-way is a seek inside that file, and the frames it hands
back afterwards can be behind the playhead - one measured session answered a
seek to 5.5 s with the frame at 5.0 s, the start of the segment that covers it.
In live mode the pair builder walks over them; in cached playback a numbered
disagreement stays the hard identity failure it is meant to catch. A file that
ends inside its own declared window is entered the same way, from a successor
that legitimately begins after the playhead.

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

The paces are also kept per processing-scale rung. They used to be filed under
the source geometry whatever rung rendered them, so the first 50% session was
forecast at the 100% pace and its own measurement then pulled the 100% forecast
toward a pace 100% never reaches. `live_session::ForecastAtProcessingScale`
reads a rung's own profile when it has one and otherwise scales the 100%
forecast by `ProcessingScaleCostFactor`: only the model follows the reduced
pixel count, so the cost is `1 - 0.25 * (1 - pixelRatio)`, a model share fitted
to the ladder's measured rungs so that none forecasts more than 3% faster than
it ran.

Per *job* there is also about 7 s of fixed cost — the preflight process, ReShade
stabilization, up to 120 priming frames, the reopen and seek, and 60 preroll
frames — which is why a session is one long job rather than a chunk per few
seconds. The lead it waits for before attaching, and the lead it waits for
before ending a rebuffer, are sized from the render's pace rather than fixed.
`live_session::StartLead` and `ResumeLead` keep 4 s and 2 s for a render at real
time and shrink them for a card well above it, where the buffer refills faster
than playback drains it and the cushion is only a wait. Below real time they
grow: the lead falls by (1 - ratio) every second played, so no cushion prevents
a rebuffer and the question is only how often one happens. `SustainedLead` takes
whichever is smaller of what buys a minute of uninterrupted playback and what
can be refilled inside a 20 s wait, floored at the fixed cushion and capped at
30 s. It is sized against the pace the session is measuring once there is enough
of it to report one, and the forecast until then, because `LiveRealtimeRatio`
says nothing for the first eight seconds - which is when the first attach is
decided.

Inside a live session the rate holds up: on a 40 s native 4K30 source the median
over nine segment intervals was **1.165x real time** against the forecast's
1.188x, so the player's own decoding and presenting costs about 2%. A heavier
file at the same geometry is a different answer - the 4K re-encode above runs at
0.78x and buffers continuously - so the forecast asks before starting whatever
it expects to fall behind, and buffering remains the release valve rather than
an edge case. That 0.78x case, and a frame-generated source at 119.88 fps which
measures 0.814x, are what the grown cushion is for: neither can be made to keep
up, only to stop and start less often.

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
and targets a 1920x1080, 2560x1440 or 3840x2160 bounding box. Which box is
chosen is a display decision, not a stored preference: Auto takes the largest
rung the monitor's current mode can scan out, because a rung above the panel is
scaled away at present time while the evaluate is charged per output pixel. The
source only decides whether that rung is an upscale at all, which is the same
`grows` guard as before. It validates a candidate
renderer on a separate child window before swapping; failure preserves playback.
No path samples with jitter. A decoded frame is already a fixed sample grid, so a
sub-pixel offset cannot reveal new detail; it only convolves the frame with a
per-frame bilinear tent, and the neural-rendering feature does not read a jitter
offset at all. `Jitter_Offset_X/Y` are pinned to zero for every evaluation.
These controls do not alter the offline DLAA carrier or cache identity.

Frame Generation is a conversion, not a presentation mode, and it is the one NGX
feature the player creates besides Super Resolution. `FrameGenerationPass`
decodes a file, evaluates `NVSDK_NGX_Feature_FrameGeneration` between each pair
of source frames and encodes the result at the planned multiple of the source
rate; the player then loads that file. Live pacing is deliberately absent:
interleaving generated frames into the playback clock also means interleaving
them with audio sync, dropped-frame accounting and seeking. `nvngx_dlssg.dll` is
staged beside the player, not in `neural-runtime/` - NGX resolves a feature
snippet from the directory of the process that creates the feature, and the
render helper never creates this one.

The order is deliberately the opposite of NVIDIA's. NVIDIA's own pipeline
upscales first and generates frames on the upscaled result; here the pass
generates at the source's own resolution and the player's live Super Resolution
then runs on the converted file. Two reasons, neither of them NVIDIA's
ordering: an evaluate is charged per output pixel, so generating a 1280x720
source at 1280x720 is cheaper than generating it at a 4K output rung; and
runtime SR already applies to any file the player opens, so baking it into this
one buys nothing and costs a second, larger neural pass.

`frame_rate_policy::PlanFrameGeneration` decides the multiple from the monitor's
refresh rather than the source alone, and the bound handed to it is
`min(DLSSG.MultiFrameCountMax, frame_rate_policy::kPhaseVerifiedMultiFrameCount)`
- the runtime's own answer held down to what has been measured to land on its
intended phase, with the runtime's number logged unchanged so the gap stays
visible. `kPhaseVerifiedMultiFrameCount` is 4 generated frames per source frame
- 5x - while this RTX 5090 on driver 616.64 reports `MultiFrameCountMax` = 5
(6x), so the project's own bound is the limiting term and stays a real ceiling.
The bound is a cadence one, stated rather than eyeballed: a multiplier is
admitted while the worst ratio between adjacent gaps in the emitted sequence
stays under 2.0, because a constant offset from the ideal phase is invisible
while one short gap beside one long one is judder. Measured worst gap ratio:
1.13 at 2x, 1.69 at 3x, 1.71 at 4x, 1.69 at 5x, 2.70 at 6x - 6x's shortest gap
of 0.088 of an interval is a near-duplicate frame followed by a jump, so it is
refused. What is admitted still lands the common cadences on a 120 Hz panel
exactly: 24 fps at 5x is 120, 30 fps at 4x is 120.

The cadence rule the multiples are drawn against changed, and the earlier one is
recorded here because it shipped. It refused any rate that did not divide the
refresh, on the claim that generating 48 fps for a 24 fps film on a 60 Hz panel
"would trade one uneven cadence for another". A frame is held for a whole number
of scan-outs, so the two hold lengths differ by exactly one refresh period and
nothing else is reachable: 24 fps is held 33/50 ms there and 48 fps is held
17/33 ms - the same spread - while the step between presented frames halves,
41.7 ms to 20.8 ms. The refusal was giving that up for nothing. `PlanFrameGeneration`
now admits a multiple when it shortens the step without widening the spread,
which keeps two refusals that matter: a source already landing evenly is never
made uneven (30 fps on 120 Hz takes 2x or 4x, never 3x), and a panel that cannot
double the source has nothing to offer. `BetterRefreshForSource` answers the rest
from the display side - a monitor advertising 48, 72 or 120 Hz can land 24 fps
film exactly - and `DLSS > Generated frames > Even cadence only` restores the old
behaviour as a setting.

The phase evidence is a per-frame brightness centroid taken off a raw decode,
and the instrument matters more than the multiplier does. Hashing the converted
file's frames cannot answer the question at all: any such checksum runs on the
decode of a lossy encode, so two byte-identical inputs still hash differently
and every output frame looks distinct whether it is a repeat or not. The
measurement that replaced it moves a 200x200 `testsrc2` patch over black across
a 1280x720 30 fps clip, exactly 40 px per source frame, encoded FFV1 lossless;
the output is decoded to raw gray and each frame's brightness centroid becomes a
phase, `(centroid - previousSource) / (nextSource - previousSource)`, so index
k of multiplier m should read k/m. Four consecutive intervals per multiplier,
including the first interval after a reset, put every multiplier from 2x to 6x
monotonic, strictly inside the pair and evenly spaced to within 0.11 of one
interval, with a systematic slight-early bias rather than clustering: 2x reads
0.474 against an ideal 0.500, and 6x reads 0.157 / 0.281 / 0.474 / 0.628 / 0.809
against 0.167 / 0.333 / 0.500 / 0.667 / 0.833. The patch's interior texture is
the whole trick. A flat white box has no detail to localise, so a generated
frame is close to a blend of the pair and its centroid sits near the temporal
midpoint; the same 4x conversion of a flat box reads 0.482 / 0.552 / 0.735, off
by 0.232, and it was that probe rather than the runtime that produced the
earlier claim that the generated indices compact toward the middle.

Two constraints on how the feature may be driven, both measured here rather than
inferred from the headers:

- The RESET evaluate declares `multiFrameCount = 1` and `multiFrameIndex = 1`
  whatever the conversion's multiplier is. A reset states that there is no
  previous frame - the first frame of the file, or a decoder discontinuity - so
  it generates nothing and has no pair to subdivide. Driving it with the pair's
  own count instead is not a cosmetic mismatch: on a 4x conversion the first
  interval after every reset came back as three byte-identical copies of the
  newer source frame (phases 1.002 / 1.002 / 1.002), and the second interval came
  back on a poisoned history at phases -0.634 / -0.107 / 0.565, outside the pair
  entirely, recovering only from the third interval onward.
- `DLSSG.BackbufferFrameID` is declared, one id per decoded source frame. The
  runtime documents it as a counter that increments once per fully rendered
  backbuffer frame; a decoded source frame is this pass's equivalent of one, so
  every index generated inside a pair carries the id of the newer frame of that
  pair rather than an id of its own.

What the conversion reads is not a free choice either. The pass reads the neural
render only when the neural view is on screen and that render covers the whole
source; anything else - the original view, or a render covering only a marked
range - reads the original file, and the confirmation names which of the two it
will read before the conversion starts. A partial render is not an
interchangeable input: converting it would hand back a file shorter than the
source it claims to be.

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

RenoDX 6.5.3 can create and evaluate feature 18 inline after observing the
player's DLSS/DLAA contract. Bootstrap explicitly enables its hooks and neural
uplift while holding the neural working resolution at the source's own
(`NRFollowInputRes=0`, `NRResolutionScale=1`). The player does not also instantiate
a direct feature-18 bridge: that would duplicate the neural pass and require an
additional undocumented NGX/caller-shim lifetime beside the existing add-on.

## Stage export

`DLSS > Convert & export > Export with DLSS stages` writes a single file with
any combination of Super Resolution, neural rendering and frame generation. It
is separate from the cache: a cache entry is a playback carrier keyed on the
source and its settings, while this is a one-off at a size and a rate the viewer
picked, so it publishes nothing and evicts nothing.

The plan is pure policy in `ExportPipeline.h`. `PlanExport` takes the selection,
the source geometry and frame rate, the runtime's maximum multiplier and whether
the source is a still, and returns either a plan or one named refusal. Nothing in
it touches a GPU, a file or a window, so every refusal is covered by
`PolicyTests` and the dialog's own regression case set drives the controls that
produce them - including turning stages back off, because a checkbox that only
latches on is an inert control wearing a hat.

A plan is at most two passes. The first is the neural worker, which carries
Super Resolution and the neural pass together: `NeuralRenderRequest` takes a
source size and an output size separately, and a larger output makes the
carrier DLSS Super Resolution from the source size (`SuperResolutionCarrier`,
the renderer's preserve-source mode, the same feature the player's playback
upscaling creates). Until 2026-09-23 it stayed DLAA at the output size over a
source the renderer had already resampled to it, which is a bilinear upscale
that DLSS then anti-aliased. A rung the runtime cannot reach from the source
fails the job by name rather than being encoded at a size nobody asked for.
RenoDX's `NRPreUpscale` defaults to 0 -
neural after the upscale - so the model then runs on the upscaled frame with no
further plumbing. The second pass is frame generation over the file the first
one wrote. `ExportStageCount` is what the progress panel divides by.

Every pass writes Matroska, so the last step is not a rename: `MuxStageExport`
writes the container the chosen name's extension asks for
(`ExportContainerFor`), stream-copying the video into MKV or MP4 (`hvc1` for
HEVC in MP4, `+faststart`) and encoding GIF, PNG or JPEG exactly as "Save
converted video" does. It stages beside the output and replaces it only once
the file is complete. `ExportContainerChoices` is what the dialog and `--render`
offer for a video, an animation and a photo; the rename it replaced wrote
Matroska under an `.mp4` name. The same step carries the original's audio,
subtitles and chapters, trimmed to a range, onto every combination. The range
is cut out of the source's own streams into a staging file first
(`CutSourceStreamsToRange`, shared with "Save converted video"), and the mux
never trims: an output `-ss` on a stream-copied render with B-frames drops every
frame of it. The worker's carrier is video-only, so an
export without frame generation used to be silent, and frame generation now
runs with `carryStreams=false` rather than muxing streams the last step would
discard. `ExportStreamActionFor` decides per stream what each container can
hold, and the step reads the audio count back off the staged file before it
publishes it.

The order is fixed and the dialog exposes no way to change it. It is NVIDIA's:
DLSS 5 neural rendering runs on the fully upscaled frame, and Streamline hands
DLSS-G the final post-processed buffer. The community Neural Upstream mod moves
the neural pass earlier because doing so is faster, which makes early the
deviation rather than the reference - and an export has no frame budget to
defend, so it takes the reference order.

Super Resolution alone is a carrier-only job. It used to be refused: the
helper enabled the add-on for every job, so an upscale-only and an
upscale-plus-neural export of one clip came back byte-identical at 9,548,373
bytes each. Now `requireNeural=false` reaches `ConfigureNeuralAddon` in the
helper, which writes the add-on disabled and takes the same
configuration-changed exit a repair does, so the parent relaunches it once per
flip (ReShade reads the INI when its proxy loads). The job still primes until
the NGX carrier exists, then skips the feature-18 arming check, the receipt gate
and the four verdicts after capture. It is held to the opposite claim instead:
a session log that shows feature 18 evaluating fails the job. The result says
what ran rather than leaving the fields blank: `neural=false` in the wire
result's first formerly reserved byte, `verifiedNeuralFrames=0`,
`feature18ArmedBeforeCapture=false`. The parent refuses a result whose `neural`
does not match what the job asked for. A resident helper keeps its add-on
loaded, so it refuses carrier-only jobs and they run single-shot.

`ExportMatrixSmoke` renders all seven combinations through a 3.5 s 720p30 clip
and checks geometry, frame count and bytes. It earned that last check twice: the
output size was once plumbed through the request, the IPC, the encoder and the
byte accounting but not into the renderer's feature create, and a comparison of
geometry alone stayed green through both that and the upscale-only defect above.

## Final image adjustments

Brightness, contrast, saturation, gamma, temperature and tint are applied in the final presentation shader after DLSS. This has two useful properties:

1. Changing display appearance does not invalidate temporal guides or require DLSS history resets.
2. Diagnostic DLSS input/motion/depth views remain unmodified.

When video is paused, adjustment changes re-present the existing DLSS output instead of decoding or reevaluating the movie frame.

## Paused-frame presentation

A frozen video frame can be re-presented while paused without advancing decoding
or neural temporal history. Cached comparison and image-adjustment changes can
therefore update the displayed frame without starting a new neural render.

## Decisions and the measurements behind them

These were recorded in the changelog of the release that made them. They moved
here when the changelog was cut back to user-visible changes, because each one
answers a question someone will ask again. Figures are from the machine and
build named; the full entries are in `CHANGELOG.md` at tag
`dlss5-video-player-v0.25.0`.

**Runtime and NGX**

- **The NGX feature is created once and kept** (0.17.2). The renderer used to
  release and re-create it at the 60th present, which collided with the
  receipt gate re-presenting one frame up to 120 times: the release tore down
  the add-on's inline worksets and the job aborted ("A frame was not produced by
  feature 18") on an RTX 4070 Ti and an RTX PRO 6000. NVIDIA's DLSS Programming
  Guide 310.6.0 limits re-creation to display-resolution, RTX and buffer-format
  changes (S3.2) and requires that no command list referencing the feature is
  in flight when it is released (S5.5). A re-hook is now explicit and happens
  before capture starts. 0.16.0 had already made the renderer drain the queue
  before releasing any feature.
- **The preflight probe renders its whole 120-frame budget** (0.25.0). RenoDX
  6.x installs a compute-state shadow on the first NGX evaluate and injects
  only after that shadow has seen a command-list Reset ("injection admitted
  after 2 incomplete-target decline(s)"). A probe that stopped once the
  player's own DLSS/DLAA carrier existed stopped submitting the only thing that
  could arm feature 18, and latched neural rendering off for the session. The
  probe polls the add-on's log every fourth frame and takes its verdict from
  the settled read after the loop, so the two cannot disagree.
- **Evidence is what the add-on reports building and evaluating** (0.25.0):
  inline NR resources at `(native 1:1)` and an evaluate line ending `[native]`.
  6.x deleted `NREnableUpscaling` and the startup architecture banners, the two
  things the old proof read. The working resolution is pinned by
  `NRFollowInputRes=0` and `NRResolutionScale=1`; the scale is a multiplier,
  not a percentage (a written `100` came back from the add-on as `1`). A
  leftover `NREnableUpscaling` is left alone, because writing it makes 6.x
  re-run a pre-v4 migration that backs up `ReShade.ini` each time. The
  parenthesised `NR skipped (after-upscale): ... incomplete` line is a startup
  notice on a healthy run; the bare `NR skipped:` form is still a failure.
- **Streamline stays at 2.13.0.0** (0.25.0). 2.14.1.0 drops `sl.dlss_nr.dll`
  and buys nothing back: at `EnableHooks=2` Streamline is never patched and no
  `sl.*` module appears in a render log.
- **A neural render must cost real GPU time** (0.21.0). A DLAA-only run
  satisfied every other check (`frames=900/900 verified=900`) at 0.46 ms of
  neural GPU time per frame against a healthy 5.7 ms.
  `NeuralTimingClearsFloor` requires 0.59 ms per output megapixel, the
  geometric midpoint of that failure and the lowest healthy median on record
  (3.26 ms at 1080p). Per-pixel cost only rises on slower hardware, so the risk
  is one-sided.
- **`nvngx_dlssg.dll` is not in `packaging/runtime-lock.json`** (0.24.0). That
  lock is the helper's runtime set and feeds `runtimeDigest`; adding a file the
  helper never loads retired every cached render and refused neural rendering
  on existing installs until they were re-staged. It was tried and reverted.
  `tools/verify_package.ps1` holds it to the pinned SDK's bytes instead.
- **Frame Generation admission is probed inside the click that needs it**
  (0.24.0). Probing at load - a second device and a second NGX create 0.2 s
  after the renderer's own deferred create - froze presentation on the 19th
  frame of a 600-frame clip. From a settled playing session the probe, the
  conversion and a live SR feature coexisted.
- **`DLSSG.MVecs` do not drive the generated frame on this runtime** (0.24.0).
  The same pair evaluated with motion in pixels, in normalised units and zeroed
  gave the same frame (centroids 812.73 / 812.73 / 812.79), so a better motion
  estimate buys frame generation nothing. `DlssgEvaluateSmoke` is the
  experiment, zero-motion control included.

**Encode and colour**

- **NVENC p5 is the default** (0.24.0). `hevc_nvenc`, 120 frames of 2560x1440 on
  an RTX 5090: p7 took 1.51 s against p5's 0.75 s for 0.12 VMAF on ordinary
  content and 0.53 on noise-heavy content, at 95-98 VMAF, far below the ~6 VMAF
  usually quoted as just noticeable. The encoder is the long pole of a
  frame-generation conversion (9.86 ms per output frame on p7, 5.08 on p5,
  matching the standalone encode), so the preset is the one knob on that path
  worth turning. A non-default preset is a cache-key term.
- **Renders are written as BT.709** (0.22.0). The encoder stated colorimetry
  only for the GPU conversion, so a BT.709 source became an untagged file of
  BT.601 pixels (a pure-red frame came back Y=81 U=90 V=240). Both paths now
  tag `bt709`/`tv`, the CPU path converts with `out_color_matrix=bt709`, and
  `setparams` keeps the primaries and transfer tags this FFmpeg otherwise drops.
  With all four tags stamped the GPU-converted capture matches the CPU path
  exactly (30.10 dB either way; it had been 0.64 dB behind). The pipeline term
  moved to `bt709-export-v1`.
- **The model-store digest is not memoised** (0.22.0). The memo used elsewhere
  keys on path, size and write time, and Windows write times move in ~15 ms
  ticks, enough for a selector file rewritten in place at the same size to
  reuse a stale digest.

**Session and publish**

- **A retarget waits for the playhead to settle for a second** (0.22.0). Six
  back-seeks 900 ms apart: without the wait, 5 retargets, 5 job restarts and 0
  segments rendered during the burst; with it, 1, 1 and 6. The cost is that
  frames at the destination arrive about 2.5 s after the last press instead of
  0.5 s; the original plays there meanwhile. A held key auto-repeats at ~30 Hz
  and was already coalesced by the seek-in-flight rule.
- **A running job is not moved for a hole narrower than about 7.3 s** (0.22.0),
  which is what moving a job costs twice (there and back); the original covers
  such a hole in the second or two it takes to cross. A paused viewer is the
  exception.
- **Publishing retries sharing violations for up to 3 s** (0.21.1), 24 attempts
  125 ms apart. Publication is a directory rename, and Windows refuses it while
  any file inside is open, which is what an antivirus scanner does to a freshly
  closed 186 MB file. A 2871/2871-verified render was lost to this before.
  `promoteStage=` names the step that refused.
- **The join cost is per part, not per megabyte** (0.22.0): about 9.8 ms per
  segment file (the same 15 minutes of 1440p joined 3.35 s faster in 30 files
  than in 450). An hour-long render is ~1800 parts, about 18 s of join; the fix,
  not yet made, is to stop gating the next hole on the publish.
- **Segments after the first are opened without a probe** (0.20.1).
  `ffprobe.exe` is 98 MB, and an antivirus that scans process starts made each
  probe 684 ms against 32 ms inside an exclusion; paid every two seconds, that
  dropped 1110 of 2525 frames. `VideoDecoder::OpenKnown` reuses the first
  segment's parameters, on a worker thread.
- **The decode pipe holds two frames of the largest source** (0.22.0), 2160p
  BGRA at 63.28 MiB. Its old 16 MiB ceiling was two 1080p frames but 1.14 of a
  1440p one, which held a 1440p30 YouTube source to 28.85 fps.
- **An idle helper keeps its feature memory by default** (0.22.0).
  `IdleVramPolicy=free` returns 361 MiB of the 1061 MiB it holds and costs
  +0.604 s on every reuse (2.400 s against 3.004 s median, three sessions per
  arm), and reuse latency is the point of keeping a helper.

**Claims checked and refuted**, so nobody chases them again. Helpers are not
orphaned by a force-kill: a live session's worker and six `ffmpeg` children
were reaped within four seconds of `Stop-Process -Force` (0.23.0). The pinned
`ffmpeg` 9.0.1 verifies TLS certificates by default; the flag is passed
explicitly anyway because the pin will move (0.23.0). The render preset
changes no pixels, on RenoDX 4.70 (0.15.0) and again on 6.5.3; colour strength,
inert on 4.70, moves 68-89 % of bytes on 6.5.3 and is back in the dialog
(docs/measurements/knobs-653-20260924).

## Remaining work

The shipped cache/settings/history/export work is described in [Usage](USAGE.md).
Runtime preflight, the benchmark, the guide ablation, frame identity, stall
recovery, range preview, comparison controls and confidence-aware optical flow
are implemented; the measured guide ablation lives in [Benchmark](BENCHMARK.md).
Buffered viewing shipped as the active session, and protection masks were
measured and abandoned because the NGX mask inputs are inert on both features.
HDR sources are tone mapped to SDR on decode and an HDR display shows their
original in HDR (`HdrPolicy.h`); what remains is an HDR cache and export, RTX
Video modes and the rest of GPU-resident processing; the changelog's `Unreleased` section carries anything
in flight.

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
