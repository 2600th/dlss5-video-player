# Using DLSS 5 Video Player

This guide covers v0.13.0, including recent history, settings-aware cache identity,
cached-video export and highest-bitrate YouTube selection.

## Open, render and compare

Open a local video with `Ctrl+O`, paste a public YouTube URL with `Ctrl+L`, or
select a trailer under **File > Upcoming games**. With the experimental runtime
available, the player acquires the source, checks its cache, renders on a miss,
and validates the complete result before opening synchronized playback.

Press `D` or use **Neural Rendering** to switch views at the same timestamp.
Pause with `Space` and press `.` to step a cached frame. Timeline seeking and
mouse-wheel volume are already supported. Cancellation can fall back to the
original when a local source has been acquired; an incomplete render is never reused.

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
including when selected from Upcoming games or pasted again at the same quality.
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

Cache data normally lives in `%LOCALAPPDATA%\DLSSVideoPlayer\NeuralCache\v1`.
Windows package virtualization may redirect it into the launching app's private
LocalCache directory. The player resolves the writable physical root, saves it
under `[Storage] CacheDirectory` in `DLSSVideoPlayer.ini` beside the executable,
and reuses that location on subsequent launches. The startup log also prints
`Neural cache directory:` with the actual path. Ownership checks still apply.

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

The legacy `--quality` argument is no longer supported. Existing
`run_4k_auto.bat` and `run_4k_quality.bat` names are compatibility launchers;
select optional 2160p playback upscaling in the player.

## Export a cached video

1. Open a video and wait for validated cached playback.
2. Choose **File > Export cached video**.
3. Select a new `.mkv` filename. Existing files are not overwritten.
4. Continue playback while export runs, or use **File > Cancel export**.

Export stream-copies the cached neural video and available source audio tracks,
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
[verification](VERIFICATION-2026-09-02.md) for tested boundaries.
