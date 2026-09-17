# Using DLSS 5 Video Player

_Verified against 0.23.0 (cc423d1) on 2026-09-16._

This guide covers whole-video session coverage and seeking,
recent history, settings-aware cache identity, media export and
highest-bitrate YouTube selection.

The interface is English-only. It does not load external language packs;
legacy language settings in the INI are ignored.

## Open, render and compare

Open a local photo, GIF or video with `Ctrl+O`, paste a public YouTube URL with `Ctrl+L`, or
select a trailer under **File > Game trailers**. Opening media never starts a
whole-video render. A YouTube URL plays from its stream as soon as it resolves;
a local file is identified against the cache first, and a validated entry opens
straight away as synchronized neural playback. Otherwise the **original starts
playing** and the status line leads with
`Mark I/O, then Ctrl+R renders the marked range`.

A streamed source is downloaded into the cache once, and that starts as soon as
you mark a range rather than when you open the video, so the render begins on a
local file. Later renders and reopens of the same source and quality reuse that
copy, and a source played from the cache seeks locally instead of re-opening the
network stream.

Press `D` or use **Neural Rendering** to switch views at the same timestamp.
Pause with `Space` and press `.` to step a cached frame. Dragging the timeline
shows the frame under the cursor while you drag and keeps playing afterwards if
it was playing; mouse-wheel volume works anywhere in the window. An incomplete
render is never reused.

### Preview first: markers, timecodes and ranges

Press `I` and `O` to mark In and Out at the current frame; `Shift+I` or
`Shift+O` clears both. Markers snap to the source frame grid, draw as green and
orange ticks on the timeline, and the status line names them as `hh:mm:ss:ff`.
`Ctrl+G` opens **Playback > Go to timecode**, which accepts `hh:mm:ss:ff`,
`h:mm:ss.mmm` or `f<frame>` and can seek or set either marker.

Both markers stay inside the source. In always names a frame that exists, and
Out is the exclusive end, so marking Out on the last frame means "to the end"
and renders it. A range that names no frame is refused before a render starts.

Neural rendering has two shapes. **Turn it on while watching** with the
toolbar's Neural Rendering button, the DLSS menu entry or `D`: rendering starts
at the playhead and a panel over the current frame collects a lead of four
seconds before playback resumes on the rendered frames. Rendering keeps running
behind playback; `Space` pauses playback rather than the render.

The session's job is the whole video — or the marked range, when the playhead
sits inside one — not just the part after the playhead. It renders the stretch
you are watching first and then fills what is left, nearest to the playhead
first, until every frame of the range is rendered; the status line reports how
much of it is done. **Seeking is not limited to what has been rendered.** Seek
into rendered frames and playback continues on them, wherever they are on the
timeline. Seek into frames nobody has rendered yet and the original plays there
while the render moves to that part of the video, and playback switches over
once it has coverage. Nothing already rendered is discarded when this happens,
so seeking back and forth costs no repeated work. While you are still seeking
the render stays where it is and follows a second after you stop: starting on a
position you are about to leave would cost the next press a fresh render start.

The one place playback still waits is the stretch the render is working on right
now: catching up with the render head inside it brings the panel back until
enough is buffered, because the frames you want are seconds away. A hole the
render is not working on plays the original instead of waiting.

The frames are dropped when they can no longer apply - a different video,
changed neural settings or guides, or a YouTube quality reload, which renders at
a different resolution.

Turning the button off stops the session and hands the same frame back to the
original, but keeps the frames it already rendered: turning it back on resumes
on them instead of redoing that work, so playback starts again in well under a
second. On an RTX 5090 the render sustains about 120 frames per second at 1080p
and 65 at 1440p; a 6.3 Mbit/s 4K30 re-encode measured 24 and could not keep up.
If the source is heavier than the GPU can follow the player says so with the
predicted rate and asks before starting. Once a session has been running for a
few seconds the status line reports the rate it is actually achieving whenever
that falls behind.

**Convert to a file** with the DLSS menu's **Convert & save** submenu:
`Ctrl+R` converts the marked clip, **Convert whole video** the whole source,
and **Save converted video** writes the result out. `F` and `Shift+F` still
render just the current frame or a four-second clip as a quick look. Each
result opens as cached playback of that range, seeking stays inside it and the
status line shows `Range hh:mm:ss:ff–hh:mm:ss:ff` plus a short
`NR intensity/struct/tone` summary of the settings it was rendered with.
**Advanced > Open render receipt** opens the entry's `receipt.json` with the
full record. The recent-video history keeps one render per source, so a new
preview or range render for the same file displaces the previous entry.

Neural settings preview themselves. With playback paused, changing a slider in
**Neural settings** re-renders that one frame 700 ms after the sliders settle
and shows the result in place of it, so settings can be compared on the actual
picture; the toggle reads `Neural Rendering · Settings preview` while such a
frame is displayed. **Apply** applies the settings to what is on screen — it
restarts an active session at the playhead or re-previews the paused frame —
and never starts a whole-video render. Saving a converted video refuses an
entry that was rendered with settings you have since changed, and offers to
convert that range again.

The timeline shows both states at once: the marked range is a solid violet block
between a green In tick and an orange Out tick, played progress is blue, and
teal stripes along the bottom name the parts of the source that already have
neural frames. A session that has been seeked around shows several of them, with
the gaps between them being what it has still to render.

Marking a range on a YouTube stream, or turning neural rendering on, starts
downloading that source in the background, because a render always works from a
local copy. Playback continues while it runs and the status line says so; the
render then starts on the file instead of waiting for the whole download, and
later renders of the same source and quality reuse it.

Playback itself moves onto that copy the first time a seek would otherwise have
gone back to the network. Seeking a stream means asking YouTube for a fresh URL
and rebuilding the decoder around it - a second or two, and it can fail - so
once a copy of those exact frames is on disk, seeks use it instead. It is the
same video either way; what changes is that seeking stops depending on the
network.

### Compare the neural result

**Video > Compare** works during cached playback on the neural view. **Blend**
mixes the original into the neural frame (`[` and `]` step the amount by 0.1);
**Split** and **Wipe** show the original left of a divider you drag in the
image, Wipe adding a white line. `Z` zooms 2x around the mouse position in the
image. Pause and step with `.` to judge a single frame; `D` still switches the
whole view between original and neural. The modes gray out on the original
view or outside cached playback and are remembered in `[Comparison]`.

**DLSS > Neural settings** (`Ctrl+N`) exposes the neural model's intensity,
local structure, local tone, skin structure, style and automatic mask, plus the
motion-vector and depth guide switches. These change
the render identity: **Apply** restarts an active session at the playhead, or
re-previews the paused frame, while playback image adjustments remain instant.
Writing a file is a separate action under **Convert & save**. The guide
switches also drive the live debug views immediately. Hovering any control shows
what it does, including which effects were measured on this runtime and what a
change costs.

Color strength and the render preset are deliberately not in that dialog. Each
was measured against the pinned runtime and changes nothing - the add-on echoes
the value back and the output is byte-identical - while a change still costs a
full re-render. They remain in `DLSSVideoPlayer.ini` as `[NeuralSettings]
ColorStrength` and `Preset` so runtime-comparison work can still drive them, and
they remain part of the render identity so a runtime that does honour them
cannot be served a stale cache entry. See [Benchmark](BENCHMARK.md).

Photos support PNG, JPEG, BMP, TIFF and static WebP. They remain paused on the
single processed frame; the cache uses a one-second carrier without adding
frames to photo exports. GIF animation is decoded once, preserving its delays
on a centisecond timeline for processing. The normal neural-runtime and
source-dimension requirements still apply. Animated WebP and camera RAW are
not included in the supported photo formats.

Press `F11` or double-click the image to enter fullscreen. The menu and controls
hide immediately. Move the mouse to reveal them; they hide again after 2.5
seconds idle. Dragging, an open menu/dialog or keyboard control focus keeps
them visible. `Tab` reveals controls for keyboard access; `Esc`/`F11` restores
the window (an active render or download consumes `Esc` to cancel first).

**DLSS Upscaling** is independent and starts off on a fresh installation. Its
output is **Auto** by default: the player takes the largest rung the monitor's
current mode can scan out - 1080p, 1440p or 2160p - and never one above it,
because the surplus is scaled away when the frame is presented while the DLSS
evaluate is charged per output pixel. **DLSS > Upscaling output** offers each
rung explicitly if you want to pin one; a pinned rung is kept between launches.
It runs during playback on either view,
preserves aspect ratio and does not downsample a source already at or above the
target: a 4K source reports "source meets output" and stays off, and so does a
1080p source on a 1080p panel. A display shorter than 1080 lines reports
"display below 1080 lines", which is a different refusal from the first and says
so. Neural rendering itself preserves source resolution.

**Video > YouTube source quality** selects 1080p, 1440p or 2160p. At the selected
resolution, the player chooses the highest advertised video bitrate across
available codecs and containers, with the highest-bitrate separate audio stream
when needed. Auto takes the tallest rung up to 1440p, then the highest bitrate
inside it, and falls back to the tallest rung up to 2160p only when a source
offers nothing at 1440p or below.
An unavailable manual resolution reports an error instead of silently changing
resolution. Source streams are copied into the cache without re-encoding.

This ranks the streams YouTube exposes; it cannot request an arbitrary bitrate.
Streams with unknown bitrate rank below known rates. A larger bitrate can mean
larger downloads and cache files, and does not guarantee better quality across
different codecs. The neural output encoder and intensity are independent.

## Recent videos and cache retention

**File > Recent videos** stores the last five distinct successfully opened
videos, newest first. Reopening an entry moves it to the top. Local entries point
to the original file; YouTube entries retain the page identity and selected quality.
Validated cached YouTube sources can reopen without resolving or downloading again,
including when selected from Game trailers or pasted again at the same quality.
New downloads must match the duration reported by YouTube using decoded video
timestamps. Downloads made before the highest-bitrate selection policy require
one replacement download on reopening. Successful downloads under the new policy
are reused normally; existing files are retained until replacement succeeds
and tracked-cache cleanup runs.

The history tracks one current source/render pair per video. Adding a sixth
video, or replacing a tracked render with new settings, makes old unreferenced
cache entries eligible for removal after active work finishes. Local originals
and exported files are never deleted by this policy. Older untracked cache data
is not swept by the five-entry history. Work that was abandoned or refused -
a cancelled download, a render that failed its checks, a folder left by a
player that was killed - is parked under `staging/` and reaped a few entries
per launch, oldest first; a folder whose owning player is still running is
never touched.

Cache data prefers `cache\v1` beside `DLSSVideoPlayer.exe`, independent of the
working directory. The player verifies it can write there and falls back to
`%LOCALAPPDATA%\DLSSVideoPlayer\NeuralCache\v1` if that folder cannot be used.
Windows package virtualization may redirect the fallback into private LocalCache.
The physical path is recorded under `[Storage] CacheDirectory` and in the startup
log. `CacheDirectoryAutomatic=1` reselects storage at each launch so moving a
portable installation works. For an explicit custom absolute path, set
`CacheDirectoryAutomatic=0`; an invalid custom path does not silently fall back.
Old automatic LocalAppData settings migrate to the new preference. Existing
cache files at the old location remain untouched; copy or clear them separately
if desired. Ownership checks still apply.

Within that directory, `sources/<key>/source.mkv` contains the original YouTube
download, including acquired audio. `renders/<key>/neural.mkv` contains the
processed video; playback uses the original source for audio. `recent-videos.dat`
tracks history, and `staging/` holds work in progress. Local input videos remain
at their original paths.

**Advanced > Clear Neural Cache** shows its size and asks for confirmation. It
closes current playback and removes owned cache data, keeping recent titles and
original-source references. Clearing is blocked during acquisition, rendering or
export. This is a count-based retention policy, not a byte quota or a backup.

## Saved settings and reproducibility

`DLSSVideoPlayer.ini` beside the executable stores volume, mute, fit/fill,
original/neural view, upscaling preference and output size, YouTube quality,
image adjustments, comparison mode, neural settings and guide switches. It
also keeps `[NeuralPace]`: one measured steady-state render pace per source
size (`Samples=WxH:ms;...`) for the detected GPU, which the keep-up forecast
predicts from - exactly at a measured size, along this GPU's own fitted line
between sizes, and conservatively beyond a single sample. Delete the section
to fall back to the generation's prior. Keep the player in a writable folder
to persist preferences.

**DLSS > Encoder settings** holds the three `[Encoding]` keys, kept apart from
the model settings because they apply to the next render. Two of them never
invalidate a cache entry; `GpuSourceConversion` does, because it changes what
the model is shown rather than how the result is written:

- `NvencPreset` (1-7, default 7). p7 is the slowest and best; drop it if NVENC
  is the bottleneck on your card.
- `GpuColorConversion` (default off). Converts the rendered frame to NV12 on the
  GPU instead of letting ffmpeg do it on the CPU. Off because with the neural
  pass running the GPU is the scarce resource: 8.35 ms/frame against 8.66 on an
  RTX 5070 Ti.
- `GpuSourceConversion` (default off). Decodes the source to NV12 and converts
  it on the GPU, which saves 2.6x on pipe traffic. It is part of the render
  identity - the key carries `nv12-source-v1` when it is on - so a render made
  with it on is not served for a request with it off, and changing it re-renders.
  Turning it on no longer risks the source's colour: the open probe reads
  `color_space`,
  `color_range`, `color_primaries` and `color_transfer` off the stream, and the
  GPU path is taken only for a source that declares a matrix and a range the
  conversion implements - BT.709 or BT.601 (`bt470bg`/`smpte170m`), limited or
  full - with the shader compiled for exactly the pair the source declared.
  Anything else, including a stream that declares nothing, falls back to
  ffmpeg's CPU conversion and says so in the log: grep
  `GPU source conversion` and every render answers with `accepted:`, `refused:`
  and the four tags it read. Falling back costs pipe bandwidth, never colour,
  and never fails a render.

  Two things are still assumed rather than checked. Primaries and transfer are
  read and logged but do not decide: the conversion produces R'G'B' from
  Y'CbCr, which is a matrix and a range and nothing else, and the CPU fallback
  does no better with a BT.2020-primaries or PQ source than the GPU path would.
  And the flag remains off by default because **nobody has made the call yet**, not
  because of a measured reason to keep it off: the colour hazard was the reason, and
  this probe removed it. On the numbers in
  `docs/measurements/gpu-readback-20260914/` the decoder side is quality-free on
  tagged input (+0.067 dB) and NV12 buys about 7 % throughput at 4K, so the next
  wave should decide the default rather than inherit it.

  Turning it on retires nothing. The render key's pipeline term carries
  `nv12-source-v1` only while the flag is on, so entries rendered with it off keep
  the key they were published under, and a render made with it on gets an entry of
  its own instead of being served for a request that wanted the other input.

  One assumption this does not change, on either path: ffmpeg's own conversion
  resolves an *unspecified* matrix to BT.601 whatever the resolution, so an untagged
  HD source decodes as BT.601 on the CPU fallback exactly as it did before. Refusing
  the GPU path keeps that behaviour identical rather than improving it - "unspecified
  plus HD implies BT.709" would be a product decision for both paths, and nobody has
  made it.

Each new neural render has a canonical `neural-settings.ini` snapshot and its
SHA-256 in the manifest. The cache key covers that snapshot, source content,
runtime binaries, application version, GPU path and source dimensions. Changes
to neural settings trigger a new render. A settings change detected between
render start and completion prevents publication of that result.

The snapshot includes add-on enable state and the neural add-on's settings;
overlay appearance is excluded. Playback adjustments and runtime upscaling do
not affect the offline render. This adds no unverified sliders or presets.

Launch `DLSSVideoPlayer.exe` directly. Select optional 2160p playback upscaling
in the player; the old quality arguments and 4K launch scripts are retired.

## Generate frames for a higher frame rate

**DLSS > Generate frames (higher frame rate)** converts what you are watching to
a higher frame rate. It is an offline conversion rather than a live mode: the
file is converted once, written into the neural cache under `frame-generation/`,
and playback switches to that file when the conversion finishes. Nothing is
interleaved into the playback clock, so seeking, audio sync and dropped-frame
accounting behave as they do on any other file. While it runs the status line
carries the multiple, the target rate and the frames written so far, and names
the way out; **DLSS > Cancel frame generation** stops it and leaves the video you
were watching untouched.

The multiple comes from the display rather than the source alone, and it is
judged on two quantities: how far the picture moves between presented frames,
and how evenly those frames land on the refresh. A frame is held for a whole
number of scan-outs, so an uneven landing is uneven by one refresh period
whatever the rate - which is why a multiple is taken when it shortens the step
without widening that spread, and why a rate that divides the refresh beats a
higher one that does not. 30 fps becomes 60 on a 60 Hz panel and 120 at 4x on a
120 Hz panel; 24 fps film becomes 120 at 5x on a 120 Hz panel, and 48 on a 60 Hz
one - held for 1 or 2 refreshes, which is the cadence the film was already shown
with, at half the step. The confirmation says so when it applies.

**Even cadence only** generates nothing unless the rate divides the refresh. A
source already at the refresh, a panel that cannot double the source, a still
image, a source that reports no frame rate and a source that already lands evenly
with no higher multiple that does are each refused with their own reason instead
of a generic one - and where the monitor has a mode that would divide the rate,
the refusal offers to switch the display to it.

Watching the neural view converts the neural render rather than the original,
but only when that render covers the whole source; a render covering one marked
range is not a stand-in for the film, so the original is read instead. The
confirmation names which of the two files it will read, the rate it is going from
and to, and where the result will be written, before anything starts.

The converted file carries the source's audio, subtitle and chapter streams by
copy. The conversion adds frames without changing the length, so those streams
still line up with no retime; a source that had audio and came back silent is
reported as a failure rather than opened. **DLSS > Convert & save > Save
converted video** then writes a copy wherever you want one.

## Save a converted video

1. Open a photo, GIF or video and wait for validated cached playback, or
   convert a clip or the whole video first.
2. Choose **DLSS > Convert & save > Save converted video**.
3. Choose a format and a new filename. Existing files are not overwritten.
4. Continue playback while saving runs, or use **DLSS > Convert & save > Cancel
   saving**.

After a live session, the item is offered only once a single render covered the
whole video; while coverage is still a set of regions filled by separate jobs
it stays greyed out, because the file that would be saved is one region, not
the film. **Convert whole video** produces an entry the item accepts.

PNG is the default for photos, GIF for animation, and MKV for video. PNG and
JPEG export the first processed frame. GIF exports animation with a generated
palette at 50 fps and loops continuously; delays are rounded to 20 ms so common
viewers do not slow down very short frame delays. MP4 transcodes video to H.264,
audio to AAC, and compatible text subtitles to MP4 text. MP4 pads odd dimensions
by at most one pixel for codec compatibility; photo exports retain source size.
GIFs and photos have no audio or subtitle tracks.

MKV stream-copies the cached neural video and available source audio tracks,
compatible subtitle tracks, font attachments, metadata and chapters. Subtitles
remain separate; they are not enhanced or burned into the image. Export does not
add subtitle display or track selection to the player itself.

The output uses the cached video, even when the original view is selected.
Playback image adjustments and runtime upscaling are not baked in. The current
cache is 8-bit; another container or bit-depth label cannot recover lost precision.

MKV must support every selected codec. Incompatible subtitles such as mov_text
produce an explicit error instead of being silently discarded or converted.
Failure or cancellation removes the exporter-owned temporary output and leaves
the inputs and any existing destination intact.

See [troubleshooting](TROUBLESHOOTING.md) for diagnostics and the dated
[hardware records](https://github.com/2600th/dlss5-video-player/blob/main/README.md#building-and-contributing)
for tested boundaries.
