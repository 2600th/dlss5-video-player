# Troubleshooting

_Verified against main (9d3e6cc) on 2026-09-25._

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

`DLSSVideoPlayer.log` beside the EXE, and `neural-runtime/NeuralWorker.log`
for the helper, carry one line per device creation naming the adapter that
actually rendered:

`D3D12 device adapter "NVIDIA GeForce RTX 4080 SUPER" luid=0x000000000001600b vendor=0x000010de vram=16047MiB is the high-performance adapter "NVIDIA GeForce RTX 4080 SUPER" luid=0x000000000001600b that the cache identity, the receipt GPU label and the pace prior describe`

Read the vendor first: `vendor=0x000010de` is NVIDIA and anything else cannot run
DLSS at all, so `vendor=0x00008086` with a small `vram` on a laptop that has an RTX
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

## Blank trailer tiles on the start screen

The trailer tiles' pictures come from `i.ytimg.com`, YouTube's image server,
fetched once and kept under `thumbs\` in the cache folder for 30 days (see
[usage](USAGE.md)). A tile shows its plain placeholder when that request
failed, timed out, or is turned off with `[Start] ThumbnailFetch=0`; the log
says which, on a `Trailer thumbnail` line. Nothing else waits on it, and the
trailer still opens.

## A render fails or starts again

The cache is reused only after source/runtime/settings hashes, dimensions,
timing and neural evidence pass validation. Changed settings or binaries trigger
a new render. An incomplete, modified or invalid cache is not reusable.

Keep neural settings unchanged while rendering. Inspect
`neural-runtime/NeuralWorker.log` and `neural-runtime/ReShade.log` for worker
diagnostics; player-side failures are in `DLSSVideoPlayer.log` beside the EXE.
Successful NGX initialization alone does not prove neural output was captured.

`Neural render encoder:` in `NeuralWorker.log` says whether a render was encoded
by NVENC straight from the GPU or by the ffmpeg encoder, and why. Lines starting
`NVENC direct:` name what stopped the direct encoder; the render then continued
through ffmpeg, which writes the same file, so they explain a slower render
rather than a failed one.

## The cache directory is unavailable

Use a writable player/data location and check free space. Cache data prefers
`cache\v1` beside the EXE, falling back to `%LOCALAPPDATA%\DLSSVideoPlayer\NeuralCache\v1`;
Windows may redirect the fallback into the launching package's private LocalCache. The player
resolves that physical location before creating source/render buckets and saves
it as `[Storage] CacheDirectory` in the INI beside the EXE. Check that value or
the startup log's `Neural cache directory:` line for the exact location. A cache
folder whose path is longer than about 140 characters is refused, because the files
the cache writes inside it would pass Windows' 260-character path limit; choose a
shorter folder.
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

The **menu** lists five videos; the cache behind it is not limited by the list
(it evicts only when the drive falls below 20 GiB free). A video that
has dropped off the list still has its render and its download, and reopening it
by any route attaches to them. Local files must still exist at their saved
location. YouTube entries need a valid acquired source to
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

**DLSS > Convert & export > Save converted video** becomes available after validated neural playback
opens. Select a new filename: existing destinations are never overwritten.
Export needs the cached neural file, its original source and FFmpeg.

Unsupported MKV subtitle codecs produce an error. The exporter does not silently
drop tracks, transcode them, or burn subtitles into the image. Check the reported
FFmpeg diagnostic and use compatible source tracks. While a write is in flight
the export row reads **Cancel export**; choosing it stops the job and removes
its temporary output.

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

If the helpers themselves are the problem, the message now names which one it
is rather than always reporting them missing. "yt-dlp.exe is not beside the
app" is a broken install; "is there but could not be opened" is usually
antivirus or file permissions; "is a link rather than a file, so it was
refused" and "resolves to somewhere outside the app's folder" are the
junction checks doing their job and mean the install has been tampered with.
"The YouTube helpers are present, but their cache folder beside the app could
not be created" is a read-only install folder, which used to be reported as
the files being missing.

Every refusal is also written to the log now, with the cause named, which is
what to attach to a bug report when YouTube changes something upstream.

## Upscaling is off or playback drops frames

DLSS Upscaling starts off on a fresh installation and then follows the saved
preference. Enable it from the DLSS menu or bottom bar. The output rung is Auto
by default and follows the monitor's current mode; **DLSS > Upscaling output**
pins 1080p, 1440p or 2160p instead. Two different refusals are reported and are
not the same problem: "source meets output" means the source already meets or
exceeds the rung, which a 4K source does everywhere and a 1080p source does on a
1080p panel, and "display below 1080 lines" means the panel is shorter than the
smallest rung, so there is nowhere to put the extra pixels.

Pin a lower rung, or turn runtime upscaling off, to isolate its cost. For YouTube, choosing
1080p reduces source load. The player drops late frames to preserve playback time.
Check the player log for SR startup/evaluation failures; ordinary playback remains available.

## RTX VSR is grey

The **RTX VSR** compare view needs an NVIDIA RTX GPU, driver 550 or newer, the
`nvngx_vsr.dll` feature DLL beside `DLSSVideoPlayer.exe`, and a player built with
the NVIDIA RTX Video SDK ([Building](BUILDING.md)). The grey segment's tooltip,
and the notice `R` shows, name the one that is missing:

- **not in this build** - it was built without `-DRTX_VIDEO_SDK`, as the public
  CI build is;
- **needs an NVIDIA RTX GPU** - NGX did not start on this adapter;
- **needs NVIDIA driver ... or newer** - the driver's NGX reported
  `VSR.NeedsUpdatedDriver`; update the driver;
- **nvngx_vsr.dll is missing** - restore it from the package or the SDK's
  `bin/Windows/x64/rel` folder;
- **not supported on this GPU or driver**, or **could not start (NGX 0x...)** -
  the runtime refused it; `DLSSVideoPlayer.log` has the `RTX VSR capability:`
  line with what NGX answered, and the create's result.

Like every compare mode, RTX VSR is live only during cached playback on the
neural view (`D`). Its cost is logged every 300 frames as `RTX VSR GPU:`; if
playback drops frames with it on, pick a lower rung under **Video > Compare >
RTX VSR quality**.

## Generate frames is unavailable, or refuses

**DLSS > Generate frames** converts a video to a higher frame rate and writes a
new file; it does not run during playback. The pill and the status line always
name the reason rather than only saying "Unavailable", and the reasons fall into
four groups.

*Nothing to generate between.* "no frame rate", "still image" and "variable
frame rate" are properties of the source: generated frames need a constant
interval to land inside.

*Nowhere to show them.* "no display refresh" means Windows did not report a
refresh rate. "already matches the display" means the video already runs at or
above what the panel can show, so generated frames would never be presented.
"display cannot double it" means the refresh is below twice the source rate.
"already lands evenly" means every source frame is already scanned out the same
number of times and no higher multiple divides this refresh.

*A setting, not the display.* "set to 2x, needs 5x" is **DLSS > Generated
frames** being the binding limit - 24 fps film on a 120 Hz panel needs 5x to
land exactly - and raising that setting converts it. "even cadence only" is the
**Even cadence only** option refusing an uneven landing; turning it off converts
anyway, with the generated frames in the same uneven grid the video already
plays in. Where the monitor has another mode that would divide exactly, the
dialog offers to switch to it and converts nothing until you ask again.

*A streaming source.* "needs a local copy" is a YouTube video: the conversion
reads a file. Accepting the prompt downloads one into the cache while playback
continues, and Generate frames becomes available when it finishes. A copy
already in the cache from an earlier render is used as-is.

"This GPU and driver admit no generated frames" is the runtime refusing the
feature outright, which is a driver and hardware question rather than a setting.

## A neural session keeps stopping to buffer

A live session plays rendered frames while the rest is still being rendered, so
it can only keep up if the render produces video at least as fast as you watch
it. Where it cannot, the buffer drains while playback runs and a rebuffer is
arithmetic rather than a fault - the player forecasts this before starting and
asks whether to go ahead.

Frame generation is the usual way to get there: it doubles the frames the render
must produce without changing the clock they have to arrive by. A 2560x1440
source at 119.88 fps measures about 0.81x real time on an RTX 5090.

The player sizes its buffer from that measured pace, so a sub-real-time render
waits longer before starting and longer after each rebuffer, in exchange for
playing about a minute at a time instead of a few seconds. What it cannot do is
remove the stalls, and the total time is set by the pace whatever the cushion
is. To watch without them, either render the video fully first and play the
result, or generate frames at a lower multiple so there is less to render.

## There is no sound, or it is the wrong track

*The wrong track.* If you are hearing the director's commentary or a dub,
open **Playback > Audio** and pick another. The player skips tracks the
container marks as commentary, audio description or hard-of-hearing when it
chooses an opening track, but a file whose tracks carry no such marking gives
it nothing to go on.

*A silent film that should not be.* The log distinguishes the cases:

```
Audio: the source has no audio track (ffmpeg mapped no stream); playing silent.
```

is a video-only file and not a fault. Any other non-zero exit is reported with
its code, and

```
Audio: the endpoint's mix format is not 32-bit float (16 bits); refusing rather than guessing.
```

means the endpoint reported a shared-mode format the player will not write
blind. Windows mixes in float in shared mode, so this indicates something
unusual about the device rather than an ordinary configuration.

*Sound stopped after changing devices.* Unplugging headphones, switching
default device, or an audio service restart are all handled: the player moves
to the new default and resumes where it was, and says so.

```
Audio: the default playback endpoint changed; the owner will move onto it.
Audio: the render endpoint went away; restarting on the current default at 8.004 s.
```

If neither line appears and the film has gone quiet, look for

```
Audio: the endpoint has not asked for data in over 1.1 s while playing; treating the sink as dead and reopening.
```

which is the guard for drivers that stop asking for data without reporting an
error. Note that the player follows the *console* and *multimedia* default
device and deliberately not the *communications* one, so starting a call does
not move a film's audio to your headset.

*Passthrough to a receiver plays PCM instead.* With **Playback > Audio >
Passthrough to receiver** on, the status line says what happened to each
track. "The audio device does not take AC-3" means the default playback device
refused the IEC 61937 format: it is not an HDMI or S/PDIF output with a
receiver behind it that decodes that codec, or the receiver is off or on
another input. Windows' own **Sound > Playback > Properties > Supported
Formats** tab lists what the device claims to decode. "In use or refuses
exclusive mode" means the format was accepted but the stream could not be
opened exclusively: another application holds the device, or **Allow
applications to take exclusive control of this device** is off under
**Properties > Advanced**. Either way the film plays as PCM, and the log has
the HRESULT:

```
Audio: the endpoint does not take AC-3 as IEC 61937 at 48000 Hz (hr=0x88890008, answered in 3.0 ms); nothing on it decodes this. Playing PCM instead.
```

If a receiver goes quiet after the display changes refresh rate - frame
generation offers such a switch - the player reopens the passthrough stream
once the display has settled, about a second and a half later, and logs
`the display changed mode; reopening the passthrough stream`.

## Preferences do not persist

The existing `DLSSVideoPlayer.ini` is stored beside the executable. Run from a
writable extracted folder and close the player normally to save preferences.

## Where the logs are, and what survives a crash

`DLSSVideoPlayer.log` sits beside the EXE, and the helper writes
`neural-runtime/NeuralWorker.log`. Each file is named after the executable that
writes it, so running a test binary no longer overwrites the player's.

Both are **appended**, not truncated, and each run starts with a
`===== session started <date> pid=N =====` banner. Relaunching after a crash to
collect the log no longer destroys the evidence of it. They roll at 8 MB,
keeping one previous generation as `<name>.log.1`.

If the folder holding the EXE cannot be written - `%ProgramFiles%`, or a
protected location under Controlled Folder Access - the log falls back to
`%LOCALAPPDATA%\DLSSVideoPlayer\`. Look there before concluding nothing was
logged.

An unhandled crash writes a minidump beside the log, named
`DLSSVideoPlayer-crash-<date>-<pid>.dmp`, and records the exception code and
address in the log itself. Attach both.

## Reporting an issue

Include the build/revision, GPU, driver, source resolution/frame rate, steps and
relevant log excerpts. Remove private file paths and signed media URLs before
sharing. Note whether the problem occurs during preparation, cached playback,
comparison, runtime upscaling or export. Hardware and visual-quality evidence
should be distinguished from configuration and test results.
