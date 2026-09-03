# Using DLSS 5 Video Player

This guide covers v0.14.1, including recent history, settings-aware cache identity,
media export and highest-bitrate YouTube selection.

The interface is English-only. It does not load external language packs;
legacy language settings in the INI are ignored.

## Open, render and compare

Open a local photo, GIF or video with `Ctrl+O`, paste a public YouTube URL with `Ctrl+L`, or
select a trailer under **File > Game trailers**. With the experimental runtime
available, the player acquires the source, checks its cache, renders on a miss,
and validates the complete result before opening synchronized playback.

Press `D` or use **Neural Rendering** to switch views at the same timestamp.
Pause with `Space` and press `.` to step a cached frame. Timeline seeking and
mouse-wheel volume are already supported. Cancellation can fall back to the
original when a local source has been acquired; an incomplete render is never reused.

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
original/neural view, upscaling preference and output size, YouTube quality and
image adjustments. Keep the player in a writable folder to persist preferences.

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

## Export processed media

1. Open a photo, GIF or video and wait for validated cached playback.
2. Choose **File > Export processed media**.
3. Choose a format and a new filename. Existing files are not overwritten.
4. Continue playback while export runs, or use **File > Cancel export**.

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
