# Using DLSS 5 Video Player

This guide covers v0.14.1, including recent history, settings-aware cache identity,
media export and highest-bitrate YouTube selection.

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
toolbar's Neural Rendering button, the DLSS menu entry or `D`: the render
starts at the playhead — to the Out marker when the playhead sits inside a
marked range, otherwise to the end of the source — and a panel over the
current frame collects a lead of four seconds before playback resumes on the
rendered frames. Rendering keeps running behind playback; if the playhead
reaches the render head the panel returns until the buffer refills, and `Space`
pauses playback rather than the render. Turning the button off stops the
session and hands the same frame back to the original. On an RTX 5090 the
render sustains about 80 frames per second at 1080p, 60 at 1440p and 36 at 4K,
so the lead grows on any source up to 4K30; if the source is heavier than the
GPU can follow (4K60, 8K) the player says so with the predicted rate and asks
before starting. Once a session has been running for a few seconds the status
line reports the rate it is actually achieving whenever that falls behind.

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
between a green In tick and an orange Out tick, played progress is blue, and a
teal stripe along the bottom names the part of the source that already has
neural frames — during an active session it grows with the render head, which
is also how far ahead you can seek.

Marking a range on a YouTube stream starts downloading that source in the
background, because a render always works from a local copy. Playback continues
while it runs and the status line says so; the render then starts on the file
instead of waiting for the whole download, and later renders of the same source
and quality reuse it.

### Compare the neural result

**Video > Compare** works during cached playback on the neural view. **Blend**
mixes the original into the neural frame (`[` and `]` step the amount by 0.1);
**Split** and **Wipe** show the original left of a divider you drag in the
image, Wipe adding a white line. `Z` zooms 2x around the mouse position in the
image. Pause and step with `.` to judge a single frame; `D` still switches the
whole view between original and neural. The modes gray out on the original
view or outside cached playback and are remembered in `[Comparison]`.

**DLSS > Neural settings** (`Ctrl+N`) exposes the neural model's intensity,
local structure, local tone, skin structure, color strength, preset, style and
automatic mask, plus the motion-vector and depth guide switches. These change
the render identity: **Apply** restarts an active session at the playhead, or
re-previews the paused frame, while playback image adjustments remain instant.
Writing a file is a separate action under **Convert & save**. The guide
switches also drive the live debug views immediately.

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

**DLSS Upscaling** is independent and starts off on a fresh installation. Select
1440p or 2160p output in the DLSS menu. It runs during playback on either view,
preserves aspect ratio and does not downsample a source already at or above the
target. Neural rendering itself preserves source resolution.

**Video > YouTube source quality** selects 1080p, 1440p or 2160p. At the selected
resolution, the player chooses the highest advertised video bitrate across
available codecs and containers, with the highest-bitrate separate audio stream
when needed. Auto prefers exact 1080p; if unavailable, it chooses the highest
available resolution up to 2160p, then the highest bitrate at that resolution.
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
and abandoned staging data are not swept by the five-entry history.

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
image adjustments, comparison mode, neural settings and guide switches. Keep
the player in a writable folder to persist preferences.

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

## Save a converted video

1. Open a photo, GIF or video and wait for validated cached playback, or
   convert a clip or the whole video first.
2. Choose **DLSS > Convert & save > Save converted video**.
3. Choose a format and a new filename. Existing files are not overwritten.
4. Continue playback while saving runs, or use **Cancel saving**.

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

See [troubleshooting](TROUBLESHOOTING.md) for diagnostics and
[verification](https://github.com/2600th/dlss5-video-player/blob/main/docs/VERIFICATION-2026-09-02.md)
for tested boundaries.
