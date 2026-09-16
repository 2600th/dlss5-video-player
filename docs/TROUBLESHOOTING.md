# Troubleshooting

_Verified against 0.22.0 (da8871b) on 2026-09-16._

For setup and everyday use, see [Building](BUILDING.md) and [Using the player](USAGE.md).

## The player cannot find the neural runtime

Extract the complete experimental package into a new folder. Keep the locked
runtime in `neural-runtime/` beside the player. A root-level `dxgi.dll` belongs to
an older layout; do not mix files from different packages. A source/core build
does not supply the experimental runtime automatically.

Choose **Advanced > Restart in DLSS SR safe mode** to skip the neural helper
for that launch while retaining optional runtime Super Resolution.

## Neural rendering is refused because the driver is too old

Feature 18 is created by the NVIDIA driver's own NGX core, so a driver older
than the core that knows the feature refuses the create before the player is
involved. The refusal appears in `neural-runtime/ReShade.log` as
`feature 18 create failed with 0xbad00002`
(`NVSDK_NGX_Result_FAIL_PlatformError`).

The player reads the driver from DXGI and compares it against a **610.47**
floor, the lowest driver this project has rendered on; **616.64** is the driver
the verification records used, and 616.56 is the floor the wider community
publishes for the same runtime. Below the floor the render is refused up front
with the detected, minimum and verified numbers, and the startup log carries the
same verdict. DXGI reports the driver as `32.0.15.6614`-style; that is
566.14, and `32.0.16.1664` is 616.64.

Update the driver, then start the render again. A GPU whose architecture the
runtime itself refuses fails differently, with `0xbad00001`, and no driver
update changes that. `0xbad0000d` is GPU memory: choose a lower source
resolution or close other GPU applications.

One failing probe is remembered per GPU, driver and runtime, so a doomed
preflight is not re-run on every play and seek. Updating the driver or the
runtime clears it.

## The render ran on the wrong GPU on a hybrid laptop

A laptop with both an integrated GPU and a discrete NVIDIA one lets the display
driver decide which of the two each process runs on, and it decides before the
player or the helper has executed anything of its own. Both executables export
the documented Optimus and PowerXpress hints that ask for the discrete GPU, but
those are a request: Windows' own Graphics settings and the NVIDIA Control
Panel override them, and a machine whose discrete GPU is disabled in firmware
or muxed away from the panel ignores them entirely.

`DLSSVideoPlayer.log` beside the EXE, and `neural-runtime/DLSSVideoPlayer.log`
for the helper, carry one line per device creation naming the adapter that
actually rendered:

`D3D12 device adapter "NVIDIA GeForce RTX 4080 SUPER" luid=0x1600b vendor=0x10de vram=16047MiB is the high-performance adapter "NVIDIA GeForce RTX 4080 SUPER" luid=0x1600b that the cache identity, the receipt GPU label and the pace prior describe`

Read the vendor first: `vendor=0x10de` is NVIDIA and anything else cannot run
DLSS at all, so `vendor=0x8086` with a small `vram` on a laptop that has an RTX
card is placement having landed on the integrated GPU. Then compare the two
LUIDs, which identify physical parts rather than models - two identical cards
share a description. `is` means the adapter the device was created on and the
adapter the player classified are the same part, so the cache identity, the
receipt's GPU label and the render-pace forecast all describe what rendered.
`is NOT` means they are two different parts: the frames are real, but those
three describe the other one, so quote the whole line in a report and treat
that receipt's GPU and any pace figure from the session as unreliable.
`cannot be compared with` means DXGI offered no high-performance adapter to
this process, which on a laptop usually means the discrete GPU was powered
down or hidden from it.

To place both processes on the discrete GPU, open **Settings > System > Display
> Graphics**, add `DLSSVideoPlayer.exe` and `neural-runtime\NeuralWorker.exe`
as separate entries and set each to **High performance**. Setting only the
player leaves the failure in place: the helper is the process that loads the
neural stack and renders every cached frame. In **NVIDIA Control Panel > Manage
3D settings > Program Settings**, add the same two executables and select the
high-performance NVIDIA processor for each. No reboot is needed, but the player
and the helper must be started again afterwards, because placement is decided
at launch and cannot be changed by a running process.

## Update notice in the menu bar

`↑ Update <version>` right-justified in the menu bar means a newer stable
release exists; opening it goes to the GitHub releases page and retires that
version until the next one ships. **Advanced > Check for updates** asks
immediately and answers either way. The check runs at most once a day, ignores
drafts (every published release is a pre-release, so those count), and stores its state in `[Updates]` in
`DLSSVideoPlayer.ini` beside the executable; `Enabled=0` turns it off. When
GitHub cannot be reached the player says so and keeps playing.

## A render fails or starts again

The cache is reused only after source/runtime/settings hashes, dimensions,
timing and neural evidence pass validation. Changed settings or binaries trigger
a new render. An incomplete, modified or invalid cache is not reusable.

Keep neural settings unchanged while rendering. Inspect
`neural-runtime/DLSSVideoPlayer.log` and `neural-runtime/ReShade.log` for worker
diagnostics; player-side failures are in `DLSSVideoPlayer.log` beside the EXE.
Successful NGX initialization alone does not prove neural output was captured.

## The cache directory is unavailable

Use a writable player/data location and check free space. Cache data prefers
`cache\v1` beside the EXE, falling back to `%LOCALAPPDATA%\DLSSVideoPlayer\NeuralCache\v1`;
Windows may redirect the fallback into the launching package's private LocalCache. The player
resolves that physical location before creating source/render buckets and saves
it as `[Storage] CacheDirectory` in the INI beside the EXE. Check that value or
the startup log's `Neural cache directory:` line for the exact location.
Original YouTube downloads are in `sources/<key>/source.mkv`; processed videos
are in `renders/<key>/neural.mkv`.

Use **Advanced > Clear Neural Cache** to see its size and remove owned data after
confirmation. It closes playback first and is blocked during acquisition,
rendering or export. Local originals and exported files are preserved. The last
five videos remain in the menu, but their cleared caches must be rebuilt.

## Neural cache staging could not be created

The render never started because the cache could not make its own working
directory, which is separate from neural runtime availability: the add-on can
report as enabled while this fails. The status line names the cache root and
the cause, and `DLSSVideoPlayer.log` carries one line per refusal with the
exact directory, the cause, the filesystem error number and whether the
ownership check rejected it, as
`Neural render staging refused: cause=create-failed path=... error=5 ownershipRejected=0`.

Extract the player to a folder you can write to; `%ProgramFiles%` and
`%ProgramFiles(x86)%` are not writable without elevation, and a cache beside
the EXE there falls back to `%LOCALAPPDATA%\DLSSVideoPlayer\NeuralCache\v1`
only when that fallback is itself writable. Controlled Folder Access, and
third-party antivirus with the same feature, deny directory creation under
Documents, Desktop and similar protected locations: add the player executable
as an allowed app or choose a cache location outside them. `error=112` or
`error=39` is a full disk; a render writes its whole encoded result into the
cache before it is published, and an active session holds its segments there
as well. `cause=create-failed` with `error=5` is a permission or policy refusal
rather than a full disk, and `cause=already-exists` means a directory with this
render's unique name was already there, which is refused rather than reused.

## A recent video is missing or needs a download

Only the five most recent distinct videos are retained. Local files must still
exist at their saved location. YouTube entries need a valid acquired source to
skip downloading; otherwise they resolve the original public page again. An
older untracked cache is not automatically imported into recent history.

Selecting the same game trailer or pasting the same URL checks recent
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

**DLSS > Convert & save > Save converted video** becomes available after validated neural playback
opens. Select a new `.mkv` filename: existing destinations are never overwritten.
Export needs the cached neural file, its original source and FFmpeg.

Unsupported MKV subtitle codecs produce an error. The exporter does not silently
drop tracks, transcode them, or burn subtitles into the image. Check the reported
FFmpeg diagnostic and use compatible source tracks. **DLSS > Convert & save >
Cancel saving** stops the job and removes its temporary output.

The export contains cached neural video, even if the original view is selected.
Playback adjustments and runtime SR are not baked in. See [export details](USAGE.md#save-a-converted-video).

## YouTube playback fails or looks too low resolution

Only public, completed non-DRM videos without login and with a known duration
are supported. Live/upcoming streams, private, paid, age-gated and
cookie-dependent media are outside the resolver contract.
Availability and regional access can change; local playback remains available.

Choose **Video > YouTube source quality**. Auto takes the tallest rung up to
1440p, then the highest bitrate inside it, falling back to a rung up to 2160p
only when the source offers nothing lower. Manual choices are 1080p,
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
