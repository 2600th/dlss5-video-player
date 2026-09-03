# Troubleshooting

For setup and everyday use, see [Building](BUILDING.md) and [Using the player](USAGE.md).
These instructions describe v0.13.0.

## The player cannot find the neural runtime

Extract the complete experimental package into a new folder. Keep the locked
runtime in `neural-runtime/` beside the player. A root-level `dxgi.dll` belongs to
an older layout; do not mix files from different packages. A source/core build
does not supply the experimental runtime automatically.

Choose **Advanced > Restart in DLSS SR safe mode** to skip the neural helper
for that launch while retaining optional runtime Super Resolution.

## A render fails or starts again

The cache is reused only after source/runtime/settings hashes, dimensions,
timing and neural evidence pass validation. Changed settings or binaries trigger
a new render. An incomplete, modified or invalid cache is not reusable.

Keep neural settings unchanged while rendering. Inspect
`neural-runtime/DLSSVideoPlayer.log` and `neural-runtime/ReShade.log` for worker
diagnostics; player-side failures are in `DLSSVideoPlayer.log` beside the EXE.
Successful NGX initialization alone does not prove neural output was captured.

## The cache directory is unavailable

Use a writable player/data location and check free space. Cache data normally
lives under `%LOCALAPPDATA%\DLSSVideoPlayer\NeuralCache\v1`; Windows may redirect
it into the launching package's private LocalCache. The player
resolves that physical location before creating source/render buckets and saves
it as `[Storage] CacheDirectory` in the INI beside the EXE. Check that value or
the startup log's `Neural cache directory:` line for the exact location.
Original YouTube downloads are in `sources/<key>/source.mkv`; processed videos
are in `renders/<key>/neural.mkv`.

Use **Advanced > Clear Neural Cache** to see its size and remove owned data after
confirmation. It closes playback first and is blocked during acquisition,
rendering or export. Local originals and exported files are preserved. The last
five videos remain in the menu, but their cleared caches must be rebuilt.

## A recent video is missing or needs a download

Only the five most recent distinct videos are retained. Local files must still
exist at their saved location. YouTube entries need a valid acquired source to
skip downloading; otherwise they resolve the original public page again. An
older untracked cache is not automatically imported into recent history.

Selecting the same upcoming-game example or pasting the same URL checks recent
history too. Keep the same source-quality setting to reuse its tracked download.
The completeness and highest-bitrate selection policies replace older source
caches once: earlier builds could accept a prematurely ended download or a
lower-bitrate format. Current acquisition validates decoded video duration
against YouTube metadata before starting neural rendering;
a short audio stream no longer cuts off the video. Interrupted HTTP reads use
bounded retries, and an incomplete result fails instead of becoming a cache hit.
For acquired YouTube sources, neural timing is checked against the video track;
a slightly longer audio tail does not count as missing neural video.

## Export is unavailable or fails

**File > Export cached video** becomes available after validated neural playback
opens. Select a new `.mkv` filename: existing destinations are never overwritten.
Export needs the cached neural file, its original source and FFmpeg.

Unsupported MKV subtitle codecs produce an error. The exporter does not silently
drop tracks, transcode them, or burn subtitles into the image. Check the reported
FFmpeg diagnostic and use compatible source tracks. **File > Cancel export**
stops the job and removes its temporary output.

The export contains cached neural video, even if the original view is selected.
Playback adjustments and runtime SR are not baked in. See [export details](USAGE.md#export-a-cached-video).

## YouTube playback fails or looks too low resolution

Only public, completed non-DRM videos without login and with a known duration
are supported. Live/upcoming streams, private, paid, age-gated and
cookie-dependent media are outside the resolver contract.
Availability and regional access can change; local playback remains available.

Choose **Video > YouTube source quality**. Auto prefers exact 1080p, otherwise
selecting the highest available resolution up to 4K. Manual choices are 1080p,
1440p and 2160p. Source quality is separate from **DLSS > Upscaling output**.
At the chosen resolution, the resolver prefers the highest advertised video
bitrate across codecs. That estimate can differ from the downloaded file's
average bitrate; bitrate alone is not a cross-codec quality score.

## Upscaling is off or playback drops frames

DLSS Upscaling starts off on a fresh installation and then follows the saved
preference. Enable it from the DLSS menu or bottom bar. Choose 1440p or 2160p;
a source that already meets/exceeds the target stays native.

Try 1440p or turn runtime upscaling off to isolate its cost. For YouTube, choosing
1080p reduces source load. The player drops late frames to preserve playback time.
There are no legacy Auto/Balanced/Performance render-quality modes. Check the
player log for SR startup/evaluation failures; ordinary playback remains available.

## Preferences do not persist

The existing `DLSSVideoPlayer.ini` is stored beside the executable. Run from a
writable extracted folder and close the player normally to save preferences.

## Reporting an issue

Include the build/revision, GPU, driver, source resolution/frame rate, steps and
relevant log excerpts. Remove private file paths and signed media URLs before
sharing. Note whether the problem occurs during preparation, cached playback,
comparison, runtime upscaling or export. Hardware and visual-quality evidence
should be distinguished from configuration and test results.
