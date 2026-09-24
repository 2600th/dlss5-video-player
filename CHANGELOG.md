# Changelog

What changed for someone installing or using the player, per release. Until
0.25.0 each entry also carried the engineering detail: what was measured, why a
fix took the shape it did, and which claims were checked and refuted. That full
text is in git history (this file at tag `dlss5-video-player-v0.25.0`), and the
decisions that still shape the code are in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Unreleased

Renders made before this release are not reused: the cache key changed with the
new capture defaults, so the first conversion of each video renders it again.
**Clear Neural Cache** frees what the old ones hold.

### New

- **Subtitles** (**Playback > Subtitles**): the source's text and picture tracks
  or a loaded SRT, ASS, VTT, PGS or VobSub file; a same-named file beside the
  video loads by itself. `V` switches track, `H`/`J` shift it 0.1 s, and both
  are remembered per video. They are drawn over the render, so the model never
  warps them, and never enter the cache or an export.
- **HDR sources** (HDR10, HLG) are tone mapped to SDR for the model, and the
  status line says so. On a display in Windows HDR mode the original shows in
  HDR; **Video > Compare > Compare HDR at SDR** shows what the model saw.
- **DLSS > Processing scale** runs the model at 75% or 50% of the source: 17.3
  and 22.0 fps against 14.8 on a 4K clip (RTX 4080 SUPER). 100% stays default.
- **Audio passthrough** (**Playback > Audio**, off by default) sends AC-3,
  E-AC-3 and DTS to a receiver, falling back to PCM with the reason shown.
- **`DLSSVideoPlayer.exe --render <input>`** writes what **Export with DLSS
  stages** would, with no window. `Ctrl+C` cancels.
- **Advanced > Render report** shows the flicker, grain and colour shift a
  render added.
- **Windows media controls** play, pause, stop and seek, and the taskbar
  thumbnail has play/pause, Neural Rendering and side-by-side buttons.

### Picture quality

- **The window scales and dithers the picture itself.** Windows used to shrink
  a 4K frame with no filtering, which looked jagged, and dark gradients the
  model lifts showed flat bands.
- **The Standard cache is constant quality.** It was held near 11 Mbit/s at
  1080p whatever its CQ; now VMAF averages 96.9 (worst 93.9, was 80.3) at about
  twice the size. **Encoder settings > Cache quality** adds **High** (10-bit
  HEVC) and **Lossless** (FFV1). Cached frames are now dithered by default:
  45-71% less banding where there was any, VMAF unchanged.
- **Renders are reproducible.** Three renders of one clip could differ in half
  their bytes where lighting changed; the same settings now give the same file.
- **A held frame stays still.** False motion from the GPU's flow engine made a
  still shot creep under the model and wore down Super Resolution (a demo clip's
  VMAF went from 63.9 to 75.2 with the fix).
- **DLSS > Upscaling history** defaults to **Per-frame**, which scored higher
  than Temporal on 5 of 6 clips. The menu says plainly that Super Resolution
  still scores below bicubic on video.
- **Neural settings** gains **Temporal stability** (off by default) and a
  **Scene cuts** sensitivity choice. **Color strength** is back, measured to
  matter on the current runtime, and **Skin structure** offers only the values
  that change the picture. Encoder settings gains deband and supplied exposure,
  both off.
- **Untagged HD video decodes as BT.709**, as other players assume; frame
  generation used to shift its colours.

### Playback and rendering

- **The first `D` after opening a video starts in about half the time**: 7.9
  to 9.0 s instead of 17.9 to 21.9 s. While it starts, the status line names
  the step instead of showing a frozen ETA.
- **Live sessions stop dropping frames at segment boundaries** (0, 0 and 4 in
  three runs, against 14, 10 and 78) **and when a render is published** (8 to
  0), and keep one copy of the render on disk instead of two.
- **High renders encode straight from the GPU**: 3-7% faster, 5-19% less CPU
  and about 250 MiB less GPU memory at 4K. HDR tone mapping moved to the GPU
  too: 84.7 fps at 4K instead of 30.7.
- **Video keeps playing through open menus and window drags**, and closing the
  player no longer waits up to 8 s for the update check.
- **Audio.** A stop or seek right after starting takes about 35 ms, not 600.
  Sound returns when a device is plugged in after none, and a pause no longer
  drops a slice of it.

### Comparison and export

- **A compare bar** switches DLSS 5, Original, Split, Wipe and the new views
  (`C`, `Shift+C`; `X` swaps). Hold the left button on the picture to see the
  original. One **Mix** slider replaces Neural strength and Blend.
- **Difference, Side by side and 2 x 2 views.** Difference shows where the
  model changed the frame, amplified 1x to 32x (`Shift+[`, `Shift+]`).
- **Zoom Fit to 8x** at the pointer (`Z`, `Ctrl`+wheel) with panning, a loupe
  on both pictures (`L`), and **Video > 1:1 pixels**.
- **Video > Compare > Load mask image** limits DLSS 5 to the mask's white
  areas, with feather and invert, remembered per video.
- **File > Save comparison image** (`Ctrl+Shift+S`) saves the view as shown,
  with a footer recording the version, video, timecode, view and settings.
- **Super Resolution alone can be exported**, and the export's Super
  Resolution stage is real DLSS SR; it had been a plain resize.
- **Exports keep their audio.** Without frame generation they came out
  silent. A `.mp4` is now an MP4, not renamed Matroska. Ranged exports keep
  chapters, audio and subtitles on time. The export uses your Neural settings.
  **Save converted video** after a range render no longer writes an empty
  video track.

### Start screen and interface

- **A start screen** checks your GPU, driver and runtime and forecasts live
  rendering, and shows recent videos with a frame of their render.
- **Seven new game trailers**, open to 2160p with no age limit, with thumbnails.
- **The timeline is a render map** with chapters and hover thumbnails. Status
  chips show render progress, frame rate and drops, and brief confirmations
  appear in the status row.
- **Help > Keyboard shortcuts** (`?`, `F1`) lists every key. Toolbar tooltips
  name each control's key.
- **Dialogs and menus are dark and scale to the monitor.** Neural settings
  fits a 1080p screen at 175%.
- **The window opens at 80% of the screen**, reopens where you left it, and is
  titled **DLSS 5 Video Player**.
- **`Esc` leaves fullscreen first**; it used to stop the render.

### Fixes

- **A finished render plays back clean in the DLSS 5 view.** Replaying one
  could draw fine vertical stripes over a yellow cast, because the render was
  decoded in a different pixel format from the one the picture expected.
- **The cache keeps what it should.** An NVIDIA App model update no longer
  orphans every render, a render made while NVIDIA rewrote its files is no
  longer lost or evicted, and leftovers of driver updates and crashes are
  reclaimed.
- **A failed neural check gives its real reason.** A stray DLL or add-on beside
  the runtime is named and refused, and the refusal clears once it is removed.
- Renders that fell back to the software encoder no longer fail, and a yt-dlp
  warning no longer stops a YouTube video opening.
- A mid-file decode error says so, and Play resumes at that frame. Dropping
  several files opens the first and says how many were skipped.
- Frame generation reads frame spacing correctly on most films and phone video.
- **Clear Neural Cache** no longer empties the live files of another running
  player, and seeking works in WebM files that don't record their length.

### Privacy and network

- **The start screen fetches trailer thumbnails from i.ytimg.com** (HTTPS, at
  most monthly, no cookies). `[Start] ThumbnailFetch=0` turns this off, and
  **Clear Neural Cache** deletes the saved images.

### New media for the README and site

- **A new demonstration video at default settings.** 24.8 seconds of *007 First
  Light* and *GTA VI* Trailer 2 from the built-in trailer list, rendered on an
  RTX 4080 SUPER: a face split between source and render from the first frame,
  then the player's own Difference, Side by side and loupe views, and a render
  filling the timeline (shown at a labelled 2x). A new poster and a 2.4 MB
  README loop go with it.
- **Five comparison stills with provenance.** Unscaled crops of one source
  frame beside the same frame of the render, each with a `.provenance.json`
  naming the source, frame, settings and runtime. One, from *Assassin's Creed
  Shadows*, shows a face the model makes worse, and says so.
- **Screenshots of the new player** at 150% scaling: Wipe, Difference, 2 x 2
  with a toast, a saved comparison image, subtitles and Neural settings; the
  start screen and Image adjustments are retaken.
- **A link-preview card and a square clip** (1200x630 and 1080x1080) for
  sharing.
- **The benchmark still reproduces.** Its `real-*` clips are cut from an older
  video, so that file moved unchanged to `tools/benchmark/fixtures/`.

### Documentation

- **The README is shorter and reorganised**, from 593 to about 250 lines.
  Release history lives only in this file, and the frame-generation and quality
  details only in `docs/USAGE.md`.
- **Corrected cache claims.** The cache removes the least recently used renders
  when the drive falls below 20 GB free; it does not keep everything. A session
  started partway into a video leaves partial renders that are not joined yet,
  so reopening that video renders it again.
- `docs/USAGE.md` gives the NVENC preset default as p5, not p7, and
  `docs/RELATED_PROJECTS.md` is rewritten.
- Removed stale files: the design spec for the already-shipped website, and a
  screenshot provenance history for images that no longer exist.

### For builders and contributors

- The packaged `verify_package.ps1` runs from an extracted download. Releases
  publish debug symbols, and the core zip is reproducible.

## 0.25.0 - 2026-09-22

### New

- **Export with DLSS stages** (**DLSS > Convert & export**, `Ctrl+S`) writes one
  file with any combination of Super Resolution, neural rendering and frame
  generation. Pick the stages, an output height and a frame-rate multiple; a
  summary line reads back the size and frame rate the file will have before it
  starts. The stages always run in NVIDIA's order (Super Resolution, then
  neural, then frame generation), and the dialog says so. Refusals name
  themselves: a height at or below the source, a multiple the runtime will not
  admit. The export does not touch the neural cache; **Save converted video** is
  still how you keep the render you are watching.
- **Export progress.** The export shows its pass ("Pass 1 of 2 - Super
  Resolution and neural rendering"), a percentage, frames done, elapsed time and
  an estimate of the time left.
- **Neural passes, 1 to 4**, in Neural settings, run the model more than once
  per frame. Two passes took 9.81 s against 8.01 s on a test clip. **Keep
  temporal history per pass** sits beside it.
- **Playback > Audio track.** The player skips commentary, audio-description
  and hearing-impaired tracks when it picks one, prefers the container's
  default, and lists every track with enough detail to tell two English tracks
  apart. The choice survives a seek.
- **Toolbar hover text** explains each control, and a pill can show a busy
  state (teal, like rendered coverage on the timeline) while a conversion runs.
- **The neural presets say what they cost**: all four render in the same time
  (within 0.6%) and differ in look only.

### Changed

- **Neural runtime moved to RenoDX DLSS 5 6.5.3** (from 4.70) and DLSS Super
  Resolution to **310.9.1** (from 310.8.0). Streamline stays at 2.13.0.0,
  DLSS NR at 310.8.SF-v2 and ReShade at 6.8.0.2155.
- **Menus are grouped one subject per block.** The DLSS menu follows the order
  the stages run in: neural, then Super Resolution, then frame generation.
  Playback keeps the audio track with the transport; Advanced has two groups.
- "Generate frames..." becomes "Cancel frame generation" while a conversion
  runs, and the export row does the same, instead of a permanently greyed
  cancel row.
- "Neural settings" and "Encoder settings" lost their ellipses; the settings
  dialogs are grouped under headings.
- The three feature pills have three different icons, so they stay
  distinguishable when the toolbar is too narrow for labels.
- **Export moved to `Ctrl+S`.** It had advertised `Ctrl+E`, which opens Image
  adjustments.
- **Super Resolution alone is not offered in the export.** The runtime always
  applies the neural pass, so the two options wrote byte-identical files.
- **Frame generation refuses variable-rate sources it used to accept.** The
  frame spacing is now measured from the packets instead of trusted from the
  container, so a screen recording that dropped a few frames no longer converts
  with lurching motion, and a constant-rate film with slightly long duration
  metadata no longer loses the feature.
- `verify_package.ps1` ships inside the package it checks.

### Fixed

- **Neural rendering no longer switches itself off at startup.** The startup
  check stopped before the new runtime had armed, recorded a failure, and
  disabled neural rendering for the session with a message blaming other DLSS
  injectors.
- **Seeking, pausing and resuming no longer click.** The audio now ramps down
  and up over 4 ms.
- **Unplugging headphones no longer leaves the film playing in silence.**
  Playback moves to the new default device in about 56 ms at the same position.
  Taking a call (the communications device) does not move a film's audio.
- A cancelled export no longer leaves its progress panel over the video, and
  the panel animates while an export runs.
- Neural passes and Keep temporal history per pass did nothing when changed in
  the dialog; they apply now.
- **A YouTube failure says which failure it was**, instead of five causes
  sharing one message, two of them wrongly saying helpers were missing.
- **Starting a render on a large file no longer stalls first.** The source is
  hashed once per loaded file instead of once per job (3-5 s on a 5 GB file).

## 0.24.0 - 2026-09-20

### New

- **Frame generation, as a conversion.** **DLSS > Generate frames** writes a
  copy of the video at 2x to 5x its frame rate, shows the percentage and time
  left, then switches playback to the result where you were watching, if you
  are still watching that video. It is not part of live playback, and the
  confirmation defaults to No.
  - **DLSS > Generated frames** sets the multiple: 2x (default), 3x, 4x, 5x, or
    as many as the display allows (`[Playback] FrameGenerationGenerated`). 5x
    is the ceiling; 6x was measured to land unevenly and is refused.
  - A multiple is offered when it makes motion smoother on your display, even
    if the result does not divide the refresh evenly: 24 fps film on a 60 Hz
    panel now converts to 48 fps. **DLSS > Generated frames > Even cadence
    only** (`[Playback] EvenCadenceOnly`, off by default) restores the stricter
    rule.
  - When a refusal is about the display, the dialog can switch the monitor to a
    refresh rate where the result lands exactly (for example 48 Hz for 24 fps
    film at 2x). The change is not written to the registry.
  - Every refusal names its reason, including when your multiple setting is the
    cause ("set to 2x, needs 5x").
  - Converted files keep the original's audio, subtitles and chapters, are
    named from the video title plus a short hash, and go into the neural cache;
    **DLSS > Show converted file** opens the folder. They are not added to
    Recent videos. **Clear neural cache** removes them too.
  - Works on YouTube videos: the pill offers to keep a local copy ("Get a
    copy", "Copying", "Generate") and converts when it lands.
  - The conversion reads the neural render only when that is what you are
    watching and it covers the whole video with the current settings;
    otherwise it reads the original. The confirmation says which, and why.
  - Frame generation does not interpolate across scene cuts.
  - `Esc` or the pill's **Cancel** stops a conversion. A conversion cannot run
    at the same time as an export or a range render, and the neural cache
    cannot be cleared during one.
- **A project website** at <https://2600th.github.io/dlss5-video-player/>,
  whose download always points at the current release.
- **Upscaling output Auto** (the new default) picks the largest rung your
  monitor can show, and a **1080p** rung was added. The pill says why upscaling
  is unavailable (`Meets output`, `Panel too small`, `No DLSS`, `No frame yet`,
  `Starting up`). Existing installs move to Auto; a harness that pins a rung
  must now write `[Playback] UpscaleAuto=0` beside `UpscaleHeight`.

### Changed

- **The NVENC preset defaults to p5** (was p7). p7 took twice as long and was
  worth 0.12-0.53 VMAF, far below a visible difference. **DLSS > Encoder
  settings** still offers p1-p7.
- **Recent videos no longer deletes renders.** Opening a sixth video used to
  throw away the first one's render and downloaded copy. Now only **Clear
  neural cache** (and the low-disk rule) removes them.
- The status line is shorter: it prints only what differs, so the frame rate
  and feature states are no longer cut off. The three DLSS features are named
  the same way everywhere.
- The first window fits on smaller displays instead of placing the seek bar
  off screen.
- Frame-generation strings are in `Localization.h`, and dialogs no longer show
  raw NGX result codes.
- Starting a neural render on a frame-generated file says that this is the
  pipeline backwards; it is not refused.

### Fixed

- **High frame-rate and frame-generated videos play in real time with neural
  rendering on.** Playback now decodes the pair as NV12, and a late pair is
  handled by presenting fewer frames or re-seeking instead of discarding its
  way further behind. A 2560x1440 59.94 fps session went from 12.3 fps
  presented to 59.94.
- **The live buffer is sized from how fast your GPU renders**, so a card below
  real time (for example 0.81x on a 119.88 fps file) buffers about once a
  minute instead of every few seconds.
- **60 fps and 24 fps sources can be neural rendered.** Half the segments were
  dropped when joining, and the render failed its final check.
- A neural pair that stops assembling no longer freezes the picture silently:
  after 3 s the original comes back at that position. A pause of a few seconds
  is no longer mistaken for that.
- A failed cached seek says so instead of leaving the old frame.
- Converting a YouTube video no longer fails at the last step when carrying the
  audio across.
- A finished YouTube download is noticed immediately, by the toolbar too,
  instead of after a restart.
- A stream whose cache folder was deleted no longer writes a log line per
  paint (14,000 lines a minute).
- Failed acquisitions and every refusal inside a neural session say why in the
  log.
- **Display changes under Auto upscaling** are logged when the running output
  no longer matches what Auto would pick.

### For builders and contributors

- `nvngx_dlssg.dll` ships beside the player and is checked against the pinned
  SDK by `tools/verify_package.ps1`.
- `tools/verification/capture-window.ps1` captures the whole window, status
  line and seek bar included.

## 0.23.0 - 2026-09-16

A reliability release from a full audit. No cache entry is retired.

- **Save converted video** is offered after a live session only when one
  render covered the whole video. It used to save just the last filled hole
  under the film's name.
- **When the graphics device is removed or stops responding**, the player
  stops, says so and rebuilds its renderer once, instead of freezing the
  picture with the audio running. If the rebuild fails it unloads with a
  message.
- A late cancel no longer cancels the next neural job before its first frame.
- A render is refused, and its entry never served, when the neural backend
  evaluated fewer frames than were captured.
- A YouTube quality change no longer leaves the previous stream's colour and
  decode settings in place.
- An out-of-memory frame buffer ends that playback with an error instead of
  closing the player.
- Every `ffmpeg`/`ffprobe` child has a time limit; a video whose file name
  starts with a dash opens.
- YouTube streams are fetched with TLS verification on and only
  `https`/`tls`/`tcp` allowed.
- Abandoned partial renders under `staging/` are cleaned up a few per launch.
- Publishing a finished render hashes it once instead of repeatedly.
- Cancelling a settings preview, opening a file during one, a folder that
  cannot be created, an out-of-sync live render, and seeking a YouTube video
  during a session all end cleanly with a status-line reason instead of a
  stuck panel or a dialog. A session that gives up hands the original back
  playing rather than paused, a render that stops reporting progress says so,
  and closing the player stops a background download and update check.
  Rendered segments are removed when a file is closed.
- The recent-videos file refuses UNC paths, and one bad record no longer
  discards the whole list.
- Both executables are built with Control Flow Guard and CET shadow-stack
  compatibility, and restrict DLL search to the application and system
  directories.
- **Releases are drafts until the complete package is attached**, every CI
  action is pinned, and the core zip carries a build provenance attestation
  (`gh attestation verify`).
- Every guide carries a `_Verified against <version> (<sha>) on <date>._` line.

## 0.22.0 - 2026-09-16

Renders made before this release are retired on purpose: the cache key now
includes the driver version, the driver-store model contents and
`NeuralWorker.exe`, so a render cannot survive a driver or model update. The
first render of each video after upgrading re-renders it; no manual wipe is
needed. A `neural-runtime/NeuralWorker.exe` left over from an older build is
refused on the version check: re-stage the runtime from the release package
instead of copying an exe over an old install.

### New

- **A neural session renders the whole video and stays seekable everywhere.**
  It fills the video hole by hole, nearest the playhead first, and the status
  line says how much is done. Seeking into rendered frames plays them; seeking
  elsewhere plays the original at once and moves the render there, without
  discarding anything. The seek bar draws every rendered region.
- **The neural helper stays resident** between jobs, so turning neural
  rendering on a second time reaches a picture in about 2.5 s instead of
  5.3 s. It exits after 30 s idle. `[NeuralHelper] IdleVramPolicy=free` returns
  361 MiB of idle feature memory at +0.6 s per reuse (default `keep`).
- A helper that dies mid-job, or a device removed under it, costs one restart
  instead of the render.
- A neural-settings change during playback says "this is the previous render"
  until the settings match again.
- Known limitation: each filled hole publishes its own cache entry, so a
  session that filled several holes does not make the whole video a cache hit
  next time.

### Fixed

- **Seeking backwards on a YouTube stream no longer turns neural rendering
  off.**
- **Streams switch to their local copy** once it exists, so seeks take about
  0.2-0.35 s instead of 1.2-1.9 s.
- **The gap between finished holes dropped from 18 s to about 1 s.**
- The render waits for the playhead to settle for a second before moving, so
  tapping the seek key keeps rendering instead of restarting five times.
- A running render is not moved to chase a hole narrower than about 7 s,
  unless playback is paused.
- The speed on the status line is the render's, not the download's.
- **Playback no longer collapses to about 1 fps** once a stream's copy is in
  the cache.
- **1440p YouTube sources play at their own frame rate** (28.85 to 29.93 fps
  on a 30 fps source).
- **The Motion vectors switch changes the picture again**; it had no effect
  since 0.20.0.
- **Neural renders are tagged and converted as BT.709.** They were written
  untagged with BT.601 colour, which players that assume BT.709 for HD showed
  shifted.
- `GpuSourceConversion` is part of the cache key, and reads the source's colour
  description instead of assuming BT.709; anything it does not implement falls
  back to ffmpeg's CPU conversion.
- A live session whose render was already cached now plays it instead of
  waiting behind the buffering panel.
- One session measured under load no longer makes the player warn that the GPU
  cannot keep up: the pace forecast uses the median of the last five sessions.
- Scene cuts less than 0.6 s apart are detected (the minimum is now 0.3 s).
- The render cache no longer serves a render with no receipt.
- `ctest` no longer erases `DLSSVideoPlayer.log`.
- The NGX log no longer reports its routine core probe as an error.
- Hardware optical flow now runs with playback Super Resolution on.
- Rendering is reported unavailable, instead of failing twice and stopping the
  session, while playback is on a downloaded copy whose cache key is unknown.

### For builders and contributors

- The benchmark corpus gained two long continuous-motion clips, and
  `blind.py` no longer mixes clips, repeats frames, keeps stale ballot pairs or
  scores an unfilled ballot as a tie.
- `player_session.ps1` keeps each session's helper log.

## 0.21.2 - 2026-09-12

- A live session whose first rendered frame is just after the playhead attaches
  instead of restarting forever with the picture stuck.
- **Video > Compare > Wipe**'s divider stays visible on bright content.
- A play press during buffering shows on the button, and the status line says
  whether playback will start when the buffer fills.
- A neural toggle pressed during a seek is applied when the seek lands, and the
  toolbar says `Queued for the seek`.
- Refused comparison modes are logged with the reason.

## 0.21.1 - 2026-09-12

- **A finished render is no longer lost when an antivirus scanner holds the
  file.** Publishing retries for up to 3 s, and a refusal says which step
  failed.
- New README video and screenshots from *Grand Theft Auto VI* and *The
  Godfather*.

## 0.21.0 - 2026-09-12

- **New Neural strength dial** in Image adjustments (`Ctrl+E`), 0 to 200 %. It
  blends the frame already on screen, so it never re-renders, and it does not
  affect exports or the cache.
- **YouTube Auto quality** takes the tallest stream up to 1440p at the highest
  bitrate, about twice the bitrate of the old 1080p pick. 2160p remains an
  explicit choice.
- The player says when YouTube served a degraded stream (below 720p), and why
  when the video is age-restricted.
- A completed render no longer loses its cache entry after a session re-attached.
- Re-toggling at full coverage no longer starts a job that fails at once.
- A run that produced frames without the neural pass is refused.
- A recent entry whose cached source was removed is re-acquired instead of
  failing the render.
- The pinned runtime was re-verified on driver 616.64 in six sessions; the
  reported faults on that driver did not reproduce.

## 0.20.1 - 2026-09-12

- **Live neural playback no longer drops half its frames** on machines whose
  antivirus scans each process start (`dropped=1110` to `dropped=1`).
- **Turning neural rendering on is faster**: 14.7 s to 9.2 s on a scanned
  install, 11.4 s to 5.8 s otherwise. A passed startup check is remembered per
  GPU, driver and runtime, and the first segment is half a second.
- The Optical Flow SDK licence notice ships in both packages. The published
  0.20.0 core zip is missing it and was left as published.

## 0.20.0 - 2026-09-11

- **Motion vectors come from the GPU's optical flow engine** where there is
  one, much finer than the CPU estimator (about 1.7 ms/frame at 1080p).
- **Less shimmer on still content**: sampling jitter was removed.
- Playback no longer requests tearing.
- Opening a YouTube video during a live session no longer leaves the previous
  session attached.
- Turning neural rendering off and on for a YouTube video resumes instead of
  re-rendering.
- A refused neural render shows its reason on the status bar, and the driver
  notice once per session.

## 0.19.0 - 2026-09-10

- **Faster neural export** (7.82 to 4.16 ms/frame measured on an RTX 5070 Ti):
  hardware decode, NV12 over the pipe, and an encoder that is ready sooner.
  Contributed by ctype-lab.
- **New DLSS > Encoder settings** (`[Encoding]` in the ini): NVENC preset
  p1-p7, GPU capture conversion and GPU source conversion (both off). The three
  settings dialogs are resizable and size correctly on scaled monitors.
- HEVC export no longer silently falls back to H.264 on GPUs without split
  encoding.
- Fixed a crash when a second settings dialog was opened.
- `build_windows.bat` finds Visual Studio 2026.

## 0.18.0 - 2026-09-10

- **The driver is checked first.** Neural rendering needs driver 610.47 or
  newer; below that the player says so with the detected, minimum and verified
  versions instead of failing a probe.
- The failure message explains the cause (driver, VRAM, unsupported GPU) rather
  than "did not arm feature 18", and a failed check is not repeated on every
  play and seek. The preflight receipt (schema 2) gained a `diagnosis` field.
- **Update notice.** At most once a day the player checks GitHub and shows
  `↑ Update <version>` in the menu bar. **Advanced > Check for updates** checks
  now; `[Updates] Enabled=0` turns it off.
- A damaged bundled helper no longer hangs the player on a Windows error
  dialog.

## 0.17.2 - 2026-09-10

- **Fixed the neural render stopping at the 60th frame** (issues #3, #4).
- **DLSS Super Resolution accepts small sources**, reducing the output to what
  the runtime admits instead of refusing (#1, #2).
- Downloading a YouTube source shows megabytes and a percentage.
- A stage that goes silent (60 s while downloading, 120 s otherwise) fails the
  job instead of leaving a progress bar running.
- **Photos can start a neural render from the toolbar.**
- On a GPU with no measured pace, the session says the pace is unmeasured.
- Scene-cut detection no longer fires several times on one transition.
- Releases carry a `.sha256` for the core package.

## 0.17.1 - 2026-09-10

- Fixed live playback stopping at a segment seam with an "out of sync" warning.
- Out-of-sync and decode stops say why in the log.
- Speed claims corrected: RTX 5090 1080p30 8.4 ms/frame, 1440p30 15.4, 4K30
  42.0; the 4K30 keep-up prompt now appears before the session.

## 0.17.0 - 2026-09-10

- **Neural export is about twice as fast** (48.6 to 109.4 frames/s at 1080p on
  an RTX 4080 SUPER) with bit-identical output. Contributed by ctype-lab (PR #5)
  with fixes.
- Faster guide generation on multi-core CPUs.
- Each stage of the export is timed in the log.

## 0.16.0 - 2026-09-09

- **Neural rendering on every RTX generation.** The runtime is ShortFuse's
  universal `310.8.SF-v2` build. Verified on RTX 40 and 50; RTX 20 and 30 are
  untested.
- Fixed the render stopping right after the start on an RTX 4080.
- Slower GPUs get a 20 s budget per frame instead of 2 s.
- **The keep-up forecast learns your GPU**, per source geometry
  (`[NeuralPace]` in the ini), and warns before a session it expects to fall
  behind.
- NGX's own errors appear in `DLSSVideoPlayer.log`.
- `tools/fetch_neural_runtime.ps1` fetches and verifies the whole locked
  runtime, and `build_windows.bat` runs it.

## 0.15.0 - 2026-09-09

- **Turn neural rendering on while watching.** Playback follows the render
  after a short buffer, the timeline shows rendered coverage, and a finished
  session becomes a cache entry. Turning it off and on resumes.
- **The player warns before a session your GPU cannot keep up with.**
- **Preview neural settings on the paused frame**, re-rendered 700 ms after
  the sliders settle.
- **Convert & save**: convert the marked clip or the whole video, save the
  converted video, cancel saving. **Apply** restarts a session or re-previews
  the paused frame.
- **Open media without rendering first.** YouTube plays as soon as it
  resolves; a local file replays a cached render if there is one. A streamed
  source is downloaded once into the cache, in the background while playback
  continues, and then seeks locally.
- **In/Out markers**, exact timecode entry, single-frame and 4 s previews, and
  range renders and exports that follow the range. The marked range is a solid
  violet block on a taller timeline.
- **Comparison modes**: Blend, Split, Wipe and Zoom.
- **A Neural settings dialog** with Intensity, Structure, Tone, Skin, Style,
  Auto-mask and guide controls. Color strength and the render preset were
  measured to change nothing and are not shown; the mask guide was removed.
- Faster decoding (12.5 ms/frame at 1080p instead of 15.6), faster short
  seeks, scrubbing with a live picture, and seeks in about half the time.
- Better motion vectors (false motion on cuts from 60.8 % to 3.7 %), and fast
  pans no longer reset the temporal history; real cuts still do.
- In/Out markers stay inside the source, so the last frame is renderable.
- Only one render at a time can use the runtime; a second player instance is
  refused with a message.
- Each cache entry carries a `receipt.json` describing how it was made.
- Failed frames are retried, a crashed helper is relaunched once, and a render
  can be paused with Space.
- A repeatable quality benchmark in `tools/benchmark` (`docs/BENCHMARK.md`).

## 0.14.1 - 2026-09-03

- Test fix for narrow and high-DPI desktops.

## 0.14.0 - 2026-09-03

- Six official trailers with human characters, each under three minutes, under
  **File > Game trailers**.
- **Photos and GIFs**: open PNG, JPEG, BMP, TIFF, static WebP and animated
  GIF; export to PNG, JPEG, GIF, MP4 or MKV. Odd photo sizes are kept.
- The cache prefers a writable `cache/v1` folder beside the exe, falling back
  to LocalAppData.
- Fullscreen hides the menu and controls until the mouse moves.
- Fewer HTTP 403 failures on freshly resolved YouTube videos.
- New screenshots from *The Witcher IV*.

## 0.13.0 - 2026-09-03

- Five official upcoming-game videos as examples.
- **Recent videos**, reusing their cached sources and renders.
- Neural settings are part of the cache identity.
- Playback, comparison, upscaling and YouTube-quality preferences persist.
- YouTube picks the highest bitrate at the chosen resolution.
- **Stream-copy MKV export** with source audio, subtitles, attachments,
  metadata and chapters.
- Fixed truncated YouTube downloads.
- Loading and progress indicators.
- The neural renderer runs in its own helper, `neural-runtime/NeuralWorker.exe`,
  so playback can use DLSS Super Resolution (off by default; 1440p and 2160p).
- Neural Rendering on by default; Frame Generation unavailable.
- Legacy quality options removed. The cache location is remembered, including
  under Windows package redirection.
- Extract each build into a new folder; old root-level layouts are refused.

## 0.12.0 - 2026-09-01

- Separate Neural Rendering, DLSS Upscaling and Frame Generation controls.
- Cached videos are reused after a content-hash check.
- Fixed cached-video seeks and garbled punctuation in status text.
- **YouTube playback** of public, non-DRM videos through pinned yt-dlp and
  Deno, and six curated examples.
- The interface is English-only.
- Open, YouTube and Examples menus come back after a render ends, however it
  ended.
- The experimental RenoDX neural-rendering path, on by default, with a safe
  mode, and pinned runtime hashes.
- The modified neural DLL has an invalid Authenticode signature, and
  redistribution permission for the runtime set is unresolved.

## 0.11.0 - 2026-08-29

- **Image adjustments**: brightness, contrast, saturation, gamma, temperature
  and tint, in their own window (`Ctrl+Alt+C` works over the overlay).
- Adjustments update while paused and are saved in `DLSSVideoPlayer.ini`.
- Less flashing over the video surface.

## 0.10.0 - 2026-08-29

- Transport hotkeys that keep working while ReShade captures input.
- Smoother D3D12 submission and frame-drop recovery for high-resolution video.

## 0.9.0 - 2026-08-29

- Start window, drag and drop, modern file picker, and language packs (since
  removed).
- Fixed seek-time races; black letterbox around the video.

## 0.8.0 - 2026-08-29

- First raw D3D12 NGX evaluation path, with reconstructed motion, depth and
  temporal mask inputs.
