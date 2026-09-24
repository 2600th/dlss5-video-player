# Using DLSS 5 Video Player

_Verified against 0.25.0 (1988cac) on 2026-09-22._

This guide covers whole-video session coverage and seeking,
recent history, settings-aware cache identity, media export and
highest-bitrate YouTube selection.

The interface is English-only. It does not load external language packs;
legacy language settings in the INI are ignored.

## Open, render and compare

With nothing open, the window is a start screen. Under the two open buttons it
checks the machine: the GPU, the driver against the 610.47 minimum that
neural rendering needs, whether the neural runtime is installed and matches
its lock, and - once this machine has measured a render, or for a GPU with a
measured prior - the frame rate the live neural render is expected to manage
at 1080p and 1440p. A failed check is marked and offers **Restart in DLSS SR
safe mode**. Below that are your recent videos, each with a frame of its
neural render and a badge for how much of it is rendered, and the game
trailers (offered only while YouTube playback is available); a click opens
one, exactly as the File menu does. In a short window the tiles get smaller
before a row is left out, and the trailers are left out first. The checks that
touch the disk run in the background, so the screen fills in a moment after
the window appears.

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
it was playing; mouse-wheel volume works anywhere in the window (`Ctrl`+wheel over
the picture zooms instead). An incomplete
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
first, until every frame of the range is rendered; the **Render** chip at the
right of the status line reports how much of it is done and, once the pace is
known, how long the rest will take. **Seeking is not limited to what has been rendered.** Seek
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

**Convert to a file** with the DLSS menu's **Convert & export** submenu:
`Ctrl+R` converts the marked clip, **Convert whole video** the whole source,
and **Save converted video** writes the result out. `F` and `Shift+F` still
render just the current frame or a four-second clip as a quick look. Each
result opens as cached playback of that range, seeking stays inside it and the
status line shows `Range hh:mm:ss:ff–hh:mm:ss:ff` plus a short
`NR intensity/struct/tone` summary of the settings it was rendered with.
**Advanced > Open render receipt** opens the entry's `receipt.json` with the
full record. **Advanced > Render report** reads the part of it that says what
the render did to motion, measured while it rendered, against the source and in
8-bit codes: **flicker added** - how much more each frame changes from the one
before, once both are moved along the video's own motion, than the source does;
**temporal sigma added** - how much more each pixel wanders inside a shot; and
how far the colour and brightness sit from the source on average. Negative
flicker and sigma mean the render is steadier than its source. A render made
before the player measured its renders says so instead of printing zeros. The
recent-video history keeps one render per source, so a new
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

The three feature buttons in the bottom bar each carry their own icon and
colour, so the bar still tells them apart when it narrows. A narrower window
first shrinks the three buttons, and one too narrow for its whole label keeps
the state (`On`, `Generate`, `Panel too small`) and drops the feature name,
which the icon and the tooltip still give; only below 850 dip do they go to
icons with a short label underneath. A button is grey when the feature is unavailable, plain when it
is off, highlighted when it is on, and teal while it is working - the same teal
the timeline uses for rendered coverage, because both mean "this is being made
right now". Hovering one says what it does and, when it is unavailable, why: a
source that already fills the panel has nothing to upscale, and the button says
so instead of only greying out.

Three chips at the right end of the status line keep the numbers you watch
during playback in fixed places: **Render** (how much of the session's range is
rendered, with an ETA), the **frame rate** (presented / source) and
**Dropped** frames. Each flashes briefly when the fact it reports changes - a
render starting or passing another tenth, playback falling behind the source
or catching up, a newly dropped frame - rather than on every repaint.

The timeline shows both states at once: the marked range is a solid violet block
between a green In tick and an orange Out tick, played progress is blue, and
teal stripes along the bottom name the parts of the source that already have
neural frames. A session that has been seeked around shows several of them, with
the gaps between them being what it has still to render, and a hatched teal
stretch marks where the render is working right now. Chapters, when the file
has them, cut thin gaps through the bar.

Hovering the timeline shows a preview above it: the time, the chapter, whether
that moment is already rendered and, while a session runs, how long until all
of it is. A local file (or a stream's local copy) also shows a thumbnail there -
the finished neural render's frame where one exists, the original otherwise -
decoded in the background by the bundled FFmpeg, so it appears a fraction of a
second after the text. A video that reports no length, such as a
browser-recorded WebM, greys the bar out: it takes no clicks, and `Left` and
`Right` still seek 10 seconds.

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

The **compare bar** sits above the toolbar whenever a video is loaded, and works
during cached playback on the neural view (it grays out otherwise; `D` turns the
neural view on). It holds the mode, the Mix, the zoom and the swap, which are
also under **Video > Compare**. In a narrower window it shortens the mode
names and shows Swap and Loupe as icons (hover for their names); narrower
still, the modes fold into one button that opens a menu of them:

- **DLSS 5** shows the neural frame, **Original** the source frame. `C` steps
  through the modes and `Shift+C` steps back.
- **Split** and **Wipe** show the original left of a divider; click or drag in
  the picture to move it. Wipe adds a white line. Both are tagged **ORIGINAL**
  and **DLSS 5** in the picture's top corners. **Swap** (`X`) puts the original
  on the right.
- **Difference** shows where the model changed the picture: the absolute
  difference between DLSS 5 (at the current Mix) and the original, computed in
  linear light and amplified 4x, as brightness. `Shift+]` and `Shift+[` step the
  amplification through 1x, 2x, 4x, 8x, 16x and 32x, and **Video > Compare >
  Difference as brightness only** switches between one grey value and a
  difference per color channel. The image adjustments do not apply to it, and
  its tag names the amplification and the channels, e.g. `DIFFERENCE x4 · LUMA`.
- **Side by side** shows the original and DLSS 5 as two panes of the same
  frame - one pair, so always one timestamp - each fitted whole into its half.
  **2 × 2** adds Difference and DLSS 5 at a second Mix below them; pick that
  Mix under **Video > Compare > 2 × 2 fourth pane Mix** (25% to 200%, 50% by
  default). Zoom, pan and the loupe move every pane together, each pane is
  tagged, and Swap swaps the first two.
- **Mix** is how much of the neural result you see: 100% is the render
  untouched, lower mixes back toward the original, higher extends the model's
  own change. Drag the slider or press `[` and `]` (a tenth per press). It
  costs a present, not a re-render, and it is the same value as **DLSS 5 mix**
  in Image adjustments. It replaces both the old Blend mode and the Neural
  strength slider: a saved Blend opens as the neural view at the same Mix.
- **A mask** limits where DLSS 5 shows. **Video > Compare > Load mask
  image...** takes any PNG, BMP, JPEG, TIFF or GIF and uses its brightness,
  stretched over the frame: white keeps DLSS 5 at the Mix, black shows the
  original, grey blends. It applies wherever DLSS 5 is shown - the neural view,
  its side of Split and Wipe, Difference and the loupe - and, like the Mix, only
  to what you watch: exports and the cache are unchanged. **Mask feather**
  softens its edge by 4 to 64 rendered pixels (8 by default), **Invert mask**
  swaps white and black, and **Clear mask** removes it. The mask, feather and
  invert are remembered per video in `[ComparisonMasks]` and come back when the
  video is opened again; the bar shows the mask's file name while one is on.
- **Hold the left mouse button still on the picture** for 150 ms to see the
  original in place of any mode; release to go back. Moving the pointer while
  holding turns the hold into the drag it would have been.

To look closely:

- **Zoom** steps through Fit, **1:1**, **2x**, **4x** and **8x**, measured in
  screen pixels per rendered pixel (a step that would not magnify in the
  current window is skipped). `Z` zooms in at the mouse and wraps back to Fit
  after 8x, `Shift+Z` zooms out, `Ctrl`+wheel over the picture zooms at the
  pointer, and the bar's `-` and `+` zoom at the centre. **Video > Compare >
  Zoom to fit** returns to Fit. Zoom works in every view, not only while
  comparing.
- **Pan** a zoomed picture by dragging it with the left button, or with the
  middle button in any mode (in Split and Wipe the left drag moves the divider).
- **Loupe** (`L`, or the bar) shows two magnified circles beside the pointer:
  the original on the left and DLSS 5 on the right, both at the same spot, at
  4x the rendered pixels (twice the view's own zoom when that is higher) and
  without smoothing, so single pixels are visible. With Swap on, the sides
  swap.
- **Video > 1:1 pixels** sizes the picture to exactly the render's resolution,
  so one rendered pixel is one screen pixel with no scaling at all; a render
  larger than the window is cropped around the centre. `A` and the toolbar's
  Fit/Fill button go back to Fit or Fill.

**File > Save comparison image...** (`Ctrl+Shift+S`) saves exactly what the
picture shows - the mode, Mix, zoom, tags, mask and loupe - as a PNG at the
window's resolution, with a footer that records the player version, the video,
the timecode and frame number, the view settings, a digest of the render's
neural settings, the neural runtime's version and when it was saved. It is
meant as evidence you can share: the footer travels with the pixels.

Pause and step with `.` to judge a single frame; `D` still switches the whole
view between original and neural. The mode, Mix, split position, swap, zoom step, difference settings and
the fourth pane's Mix are remembered in `[Comparison]`.

### HDR sources

The neural model renders in SDR, so an HDR10 (PQ) or HLG video is tone mapped
to SDR BT.709 as it is decoded, for playback, for rendering and for export
alike. The curve is Hable, mapped to the brightest the file says it gets: its
MaxCLL, else its mastering display's peak, else 1000 nits. That peak is read
once per video, never measured frame by frame, which would make the picture
pump. The status line says **HDR tone-mapped to SDR** while one is
loaded. Renders of HDR sources made before this saw the untone-mapped picture,
flat and grey, and are not served again: the tone map and its peak are part of
the render's identity.

On a display Windows has in HDR mode (**Settings > Display > Use HDR**), the
player presents in HDR. DLSS 5, SDR videos, subtitles and the tags and loupe on
the picture sit at Windows' **SDR content brightness**, like every other SDR
window on that display, and the original of an HDR video is shown in HDR, as it
was graded: in the original view, in Split, Wipe, Side by side and the loupe, and while
you hold the picture. The player follows the display it is on - drag it to an
SDR monitor and it presents SDR again - and never turns Windows HDR on or off
itself. **Video > Compare > Compare HDR at SDR** shows the original tone mapped
instead, exactly as the model saw it, for a like-for-like comparison. A Mix
other than 100%, a mask, Difference and 2 × 2 always compare at SDR:
they compute with both pictures, and mixing HDR light with SDR light would
measure the tone map rather than the model. **Save comparison image**, exports
and the cache stay SDR.

**DLSS > Neural settings** (`Ctrl+N`) exposes the neural model's intensity,
local structure, local tone, skin structure, color strength, style and automatic mask, the
number of neural passes and whether temporal history carries between them, the
motion-vector and depth guide switches, how readily a scene cut resets the
render's temporal history, and how much temporal stability the render gets. The controls are grouped under **Look**, **Quality and
render time**, **Guides sent to the model** and **Across frames**, so which
ones answer the same question is visible before you read their labels. These change
the render identity: **Apply** restarts an active session at the playhead, or
re-previews the paused frame, while playback image adjustments remain instant.
Writing a file is a separate action under **Convert & export**. The guide
switches also drive the live debug views immediately. Hovering any control shows
what it does, including which effects were measured on this runtime and what a
change costs.

**Neural passes** runs the model over each frame more than once, 1 to 4. The
add-on's own overlay warns games away from it because every extra pass is
another full neural evaluate against a frame budget - but a conversion has no
frame budget, so the cost lands on render time, which the pace forecast already
measures. Measured over a 72-frame range: two passes took 9.81 s against 8.01 s
and wrote 932,019 bytes against 780,048. **Keep temporal history per pass** sits
beside it and greys out at a single pass, where it has nothing to govern.

**DLSS > Processing scale** sets the resolution the model runs at. **Source,
100%** is the default and the recommended rung: the model sees the decoded
frame. **75%** and **50%** show the model an area-reduced picture and let DLSS
Super Resolution bring the result back to the source size, which renders faster
and looks softer - the add-on's `NRPreUpscale=1` is what puts the model on the
smaller picture, so the player writes it for those rungs and puts it back to 0
when you return to 100%. Each rung prints the rate it measured beside it: a
whole 4K render on an RTX 4080 SUPER went 14.8, 17.3 and 22.0 frames per second.
A rung is part of the render identity, so a render made at 50% is never served
for 100%. It applies to renders at the source size: live and cached playback,
and an export with Neural rendering but not Super Resolution. An export that
upscales runs the model on the upscaled frame whatever this says.

**Scene cuts** is a ladder of four measured points rather than a slider. **Default
(recommended)** is the criterion the player has always used: over the twenty-clip
benchmark corpus it found all 22 labelled cuts in real footage with no false
alarm. **More sensitive** also catches a cut between two shots that share a
brightness distribution, which Default cannot see, at the price of one more reset
on a camera flash. **Less sensitive** resets less often - it removes a double
reset the corpus shows one frame after a cut - and can miss a second cut that
follows the first within 0.3 s. **Off** never resets on the picture; seeks,
dropped frames and a new source still do. Each rung renders its own cache entry.
`python tools/benchmark/cutlab.py --ladder` prints what each rung does on the
labelled corpus.

**Temporal stability** steadies shimmer the model adds from one frame to the
next. The previous rendered frame is moved along the video's own motion and
blended into the new one, but only where the source itself lines up after that
move - so a moving edge, something coming out from behind something else, or a
cut takes the new frame as it is, and a fade or a lighting change is followed
rather than lagged. **Low**, **Medium** and **High** keep 30, 50 and 70 % of that
history. **Off** is the default and the render as the model produced it. Measured
on the benchmark's four real captures, High lowers the temporal sigma the render
adds on every one of them (by 0.02 to 0.42 of an 8-bit code) with PSNR against
the source unchanged within 0.2 dB; on a synthetic noise texture it costs some
fine detail, which is why it is a choice. Each rung renders its own cache entry.

The render preset is deliberately not in that dialog. Measured against the
pinned RenoDX 6.5.3 it changes nothing - the output is byte-identical - while a
change still costs a full re-render. It remains in `DLSSVideoPlayer.ini` as
`[NeuralSettings] Preset` so runtime-comparison work can still drive it, and it
remains part of the render identity so a runtime that does honour it cannot be
served a stale cache entry. Color strength was hidden for the same reason on
RenoDX 4.70 and is back, because 6.5.3 honours it. Skin structure only acts
between 0.00 and 0.99 and only with Automatic mask on: every negative value and
+1.00 render the default picture, which is why the default -1.00 means off. See
[Benchmark](BENCHMARK.md).

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

Press `?` or `F1` (or **Help > Keyboard shortcuts**) for a sheet of every
shortcut, grouped by the menu it belongs to, plus the keys no menu names
(`Tab`, `Enter`, `.`, `Esc`, the wheel and the `Ctrl+Alt` overlay keys). It is
read from the menus themselves, so it always matches them. The keys keep
working while it is up; `?`, `F1`, `Esc` or a click on it closes it.

The player also answers Windows' own media controls: the player card in the
volume flyout and on the lock screen shows the video's title and position and
its play, pause, stop and seek work, as do a headset's buttons. Hovering the
taskbar button shows three buttons under the thumbnail - play/pause, Neural
Rendering on/off and side-by-side compare (**Side by side**, and back to the
neural picture) - which do exactly what the matching menu commands do.

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

**Upscaling is off by default because, on video, it does not beat a plain
scaler.** DLSS Super Resolution is built for rendered games: jittered, aliased,
noise-free samples. Decoded video is none of those. Measured 960x540 to 1080p on
six clips, a bicubic upscale scored higher VMAF than DLSS on every one
(`docs/measurements/sr-quality-20260924` and `sr-history-20260924`). Turn it on if
you prefer its look, not because it recovers detail.

**DLSS > Upscaling history** chooses how it uses earlier frames. **Temporal
(steadier)**, the default, accumulates them. Its picture changes less from frame to
frame than the source, which calms grain and also trails motion. **Per-frame
(sharper on some clips)** upscales every frame on its own. It scored 0.8 to 19 VMAF
above Temporal on five of the six clips and 1.3 below on a held frame. The greyed
line at the top of the submenu and each item's right-hand column give the measured
range. The same choice is the **History** row of **Export with DLSS stages**.

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

**File > Recent videos** lists the last five distinct successfully opened
videos, newest first. The list is a menu, not the cache: renders and downloads
outlive it, and a video dropping off the end keeps everything rendered for it. Reopening an entry moves it to the top. Local entries point
to the original file; YouTube entries retain the page identity and selected quality.
Validated cached YouTube sources can reopen without resolving or downloading again,
including when selected from Game trailers or pasted again at the same quality.
New downloads must match the duration reported by YouTube using decoded video
timestamps. Downloads made before the highest-bitrate selection policy require
one replacement download on reopening. Successful downloads under the new policy
are reused normally; existing files are retained until replacement succeeds
and tracked-cache cleanup runs.

The history tracks one current source/render pair per video, and nothing
evicts a render on the list's behalf. A sixth video pushes the first out of the
menu and leaves its render on disk, so reopening that video attaches to it
again instead of rendering the same seconds a second time; replacing a tracked
render with new settings leaves the old one there too, since it is keyed by
those settings and a later session may ask for them again. At startup the
player removes published renders it can prove nothing will ask for again - ones
made under a different NVIDIA driver or model store, or by an older version of
this same installation - and, only when the disk has less than 20 GiB free,
the least recently watched renders until it does. Otherwise **Advanced > Clear
Neural Cache** is the only thing that removes a published entry. Local
originals and exported files are never deleted by any of this. Work that was
abandoned or refused -
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
original-source references. Work belonging to another player that is still
running - its live session and its unfinished renders - is kept, and so is any
entry that player has open. Clearing is blocked during acquisition, rendering or
export. Retention is unbounded and manual: not a count, not a byte quota, and
not a backup.

## Saved settings and reproducibility

`DLSSVideoPlayer.ini` beside the executable stores volume, mute, fit/fill,
original/neural view, upscaling preference and output size, YouTube quality,
image adjustments, comparison mode and Mix, neural settings, processing scale and
guide switches. It
also keeps `[NeuralPace]`: one measured steady-state render pace per source
size (`Samples=WxH:ms;...`) for the detected GPU, which the keep-up forecast
predicts from - exactly at a measured size, along this GPU's own fitted line
between sizes, and conservatively beyond a single sample. Paces measured at a
reduced processing scale are kept apart under `Samples75` and `Samples50`;
until a rung has one, its forecast is the source-scale one made cheaper by the
pixels the smaller model no longer processes. Delete the section
to fall back to the generation's prior. Keep the player in a writable folder
to persist preferences.

**DLSS > Encoder settings** holds the three `[Encoding]` keys, kept apart from
the model settings because they apply to the next render. All three are part of
the render identity, so changing one re-renders instead of serving a cached
range made under the other value. At their defaults they add nothing to the
key, so a default render keeps the cache entries it already has:

- `NvencPreset` (1-7, default 5). p7 is the slowest and best; drop it if NVENC
  is the bottleneck on your card. A non-default preset adds `nvenc-p<N>` to the
  key.
- `GpuColorConversion` (default off). Converts the rendered frame to NV12 on the
  GPU instead of letting ffmpeg do it on the CPU. Off because with the neural
  pass running the GPU is the scarce resource: 8.35 ms/frame against 8.66 on an
  RTX 5070 Ti. On, it adds `nv12-output-v1` to the key: the GPU's 2x2 box
  chroma downsample and ffmpeg's conversion are different filters.
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

  A source that declares no matrix is decoded the way players decode one: BT.709
  when it is HD (wider than 1279 or taller than 576 pixels), BT.601 below that.
  ffmpeg's own conversion would pick BT.601 at every size, and every export is
  converted back with BT.709, so an untagged HD source used to come out of an export
  with its colours moved - the cyan bar of an untagged 720p test pattern went from
  Y 133 to 155 through frame generation alone. Such a source still refuses the GPU
  path and decodes on the CPU, now with BT.709; its renders carry
  `untagged-hd-bt709-v1` in the key, so none made under the old reading is served.
  A declared matrix is always used as declared, and photos and GIFs are left as
  they are.

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

## Choosing the audio track

**Playback > Audio** lists the source's audio streams. Most files have
one and the menu says so; the entry exists for the ones that do not.

The player does not simply take the first stream. A disc rip often lists the
director's commentary first, and a release with a dub often lists the dub
ahead of the original, so "first" is regularly the wrong answer. Containers
mark the tracks that are not the feature - commentary, audio description,
descriptions, and tracks for the hard of hearing - and those are skipped when
the player picks an opening track. Among what is left it prefers the one the
container marks as its default. If every track is flagged, one of those is
used anyway: a film has to have sound.

Nothing is inferred from the track's title. Titles are free text written by
whoever made the file, in whatever language they chose, so they are shown to
you and not used to decide.

Each entry names the language, the title if there is one, the codec and the
channel layout, which is what it takes to tell two English tracks apart:

```
1. English - Director's Commentary - AC3 5.1 (commentary)
2. English - AC3 5.1
3. French - AC3 5.1
```

Switching tracks restarts the audio at the position you are at, so it costs
the same as a seek. The choice stays until you load something else.

## Send Dolby Digital or DTS to a receiver

**Playback > Audio > Passthrough to receiver (AC-3/E-AC-3/DTS)** sends an
AC-3, E-AC-3 or DTS track to an AV receiver or soundbar undecoded, so the
receiver decodes the surround mix itself. It is off by default and the choice
is remembered.

Passthrough needs the default playback device to be an HDMI or S/PDIF output
with a receiver behind it that decodes the codec. The player opens that device
exclusively while the film plays, so other applications cannot play sound on
it meanwhile, and Windows' volume slider does not apply: the receiver owns the
level. Mute still works, by sending silence in the film's place. DTS-HD tracks
send their DTS core; TrueHD, AAC and every other codec are decoded and played
as PCM as usual.

When the track or the device cannot do it, the film plays as ordinary PCM and
the status line says why, for example
`Passthrough: the audio device does not take AC-3; playing PCM`. Nothing goes
silent because passthrough was refused. Each seek restarts the bitstream, and
most receivers take a moment to lock onto it again, so the first fraction of a
second after a seek can be quiet on the receiver.

## Subtitles

**Playback > Subtitles** lists the source's subtitle streams under **Off**,
and a subtitle file once one is loaded. Text subtitles (SRT, ASS/SSA, WebVTT,
MP4 timed text) are drawn by libass with their own styling and positioning,
using the fonts an MKV carries as attachments; picture subtitles (PGS from a
Blu-ray, VobSub from a DVD) are scaled with the picture.

- **What opens with the video.** A subtitle file beside the video with the same
  name (`Film.srt`, or `Film.en.srt` when there is no plain one; `.ass`, `.ssa`,
  `.srt`, `.vtt`, `.sup`, `.idx`) is loaded by itself. Otherwise a stream the
  container marks as default, or failing that one marked forced, is shown;
  otherwise subtitles start off.
- **Load subtitle file…** takes a file from anywhere. A text file that is not
  UTF-8 is handled: UTF-16 (with or without a byte-order mark) is recognised,
  and anything else that is not valid UTF-8 is read in the system's ANSI code
  page (Windows-1252 on a Western European install). A file saved in some other
  legacy code page shows the wrong accents - convert it to UTF-8.
- `V` steps through Off, each stream and the loaded file. `H` shows subtitles
  0.1 s earlier and `J` 0.1 s later; **Reset subtitle delay** shows the current
  delay and puts it back to zero.
- Your choice for a source - the stream or file, Off, and the delay - is
  remembered with it, like its mask (the 200 most recent sources).

Subtitles are drawn over the picture after the neural render, at the size the
picture has on screen, so text is never warped by the model, never enters the
render cache and is never in an export (MKV export still carries the source's
subtitle streams as separate tracks, see below). They follow the playback
clock through pause, seeks and frame steps; after a seek the line on screen
appears once the subtitle helper has caught up, usually within a fraction of a
second. A stream inside a large file is read out of it once, in the background,
the first time it is shown. Subtitles are not shown in the Motion vectors and
Depth views.

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

**Hold repeated frames (animation on twos)**, in the same submenu, is for
animation drawn on twos or threes, where every drawing is shown for two or three
frames. With it on, a pair of source frames that is the same picture twice - up
to codec noise - is not generated between: the frame is held for those slots,
which is exact, instead of whatever the runtime makes between two copies of one
image. It is off by default: measured on an RTX 4080 SUPER, the frame the runtime
generates between two copies of one frame is already that frame to within the
encoder's own noise, so holding changes nothing visible there, while the detector
(`tools/benchmark/duplab.py`) can still hold a pair a small object moves in.

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
reported as a failure rather than opened. **DLSS > Convert & export > Save
converted video** then writes a copy wherever you want one.

## Export with the DLSS stages combined

**DLSS > Convert & export > Export with DLSS stages** (`Ctrl+S`) writes one file
with any combination of Super Resolution, neural rendering and frame
generation. Tick the stages you want, pick an output height and a frame rate,
and the summary line reads back the geometry and frame rate the file will
actually have before anything starts.

The order is fixed - Super Resolution, then neural rendering on the upscaled
frame, then frame generation on the result - and the dialog offers no way to
change it. That is NVIDIA's own arrangement rather than a preference: DLSS 5
neural rendering runs on the upscaled frame, and DLSS-G consumes the finished
picture. The first two stages are one pass over the video; frame generation is a
second pass over what that pass wrote.

Refusals are named rather than generic, and they appear as you tick:

- *Choose at least one stage* - nothing is selected.
- *Already at or above that height* - the rung does not grow the source, and
  DLSS will not run an output that does not grow.
- *This GPU's runtime does not admit that frame rate* - the multiple is beyond
  what the runtime reports, said before the render rather than minutes into it.
- *A still image has no successor frame to generate toward.*
- *Open a local video first* - a stream has to finish copying before it can be
  exported.

**Super Resolution on its own runs without the neural model.** Tick it with
Neural rendering unticked and the helper starts with the neural add-on
disabled, so the file is DLSS Super Resolution alone. The render is refused
rather than written if the add-on turns out to have run anyway. With Neural
rendering ticked, the pass uses the look set in **Neural settings**.

**History** (under the output height) is the same choice as **DLSS > Upscaling
history**: Temporal or Per-frame, with the measured VMAF and steadiness in its
tooltip. It applies only to Super Resolution on its own. With Neural rendering
ticked it is greyed and the pass keeps Temporal, because the model runs on the same
frames. Cached renders are therefore never affected. `--render` uses the saved
choice.

The file is written in the container its name asks for. The Save dialog
offers MKV (the default) and MP4 for a video, GIF (the default), MP4 and MKV
for an animated GIF, and PNG (the default) or JPEG for a photo; a name typed
with any other extension gets the selected type's extension added. MKV and MP4
keep the video exactly as the passes encoded it - only the container changes -
and an MP4 of HEVC is tagged `hvc1`, which Apple's players need.

Whatever the stages, the file carries the source's audio, subtitles and
chapters (only the part a range covers, on the range's own clock: a subtitle
already showing when the range starts is kept for the rest of its time, and
"Save converted video" cuts a range the same way). MKV copies them as they are, turning
MP4 timed text into SubRip. MP4 copies the audio it can hold and encodes the
rest to AAC, turns text subtitles into MP4 timed text, and leaves out picture
subtitles and font attachments, which it has no place for. An export whose
audio did not all arrive is reported as failed rather than written. The dialog
asks before replacing an existing file, and the replacement happens only once
the new file is complete.

While it runs, the panel over the video names the pass, the percentage, frames
done of total, elapsed time and an ETA once enough frames have gone through for
one to mean anything. Two passes are reported as two rather than one bar that
jumps backwards. The export row becomes **Cancel export** while it runs, and so
does the toolbar pill.

This does not touch the neural cache. A cache entry is a playback carrier keyed
on the source and its settings; this is a one-off at a size and a rate you
picked. **Save converted video** is still how you keep the render you are
already watching.

### From the command line

The same export runs without opening the player:

```
DLSSVideoPlayer.exe --render <input> [--stages sr,nr,fg] [--height 1080|1440|2160]
                    [--multiplier 2-5] [--preset NAME] [--processing-scale 100|75|50]
                    [--range START-END] [--out FILE] [--quiet]
```

- `--stages` picks the stages as the dialog's ticks do: `sr` (Super
  Resolution), `nr` (neural rendering), `fg` (frame generation). They run in
  the fixed order above whatever order they are listed in. Without it, `nr`.
- `--height` is the Super Resolution rung and `--multiplier` the frame
  generation rate, 1440 and 2 by default. Each needs its stage.
- `--preset` is one of `natural`, `detail-only`, `gentle` or `strong`, the
  presets in **Neural settings**. Without it the render uses the Neural
  settings the player saved.
- `--processing-scale` is `100`, `75` or `50`, the rungs of **DLSS >
  Processing scale**, for `nr` without `sr`. Without it, the saved rung.
- `--range` renders part of the source, in the timecode forms **Go to
  timecode** accepts, for example `0:10-0:25` or `f0-f300`. It needs `sr` or
  `nr`: frame generation then converts that pass's result rather than the whole
  film, and the file carries the source's audio, subtitles and chapters for
  that range.
- `--out` names the file to write and replaces an existing one. Its extension
  picks the container, from the ones the dialog offers that source: `.mkv` or
  `.mp4` for a video, also `.gif` for an animated GIF, `.png` or `.jpg` for a
  photo; any other is refused as a bad argument. Without it the file is
  `<input>-dlss.mkv` beside the input (`.gif` for an animation, `.png` for a
  photo), and an existing one is refused rather than overwritten.
- `--quiet` prints only the last line; `--help` prints the options.

It prints the plan, a progress line per pass at most once a second, and a final
`done:`, `refused:`, `failed:` or `cancelled` line. The exit code says which:
0 done, 2 bad arguments, 3 refused (the reason is printed - the same refusals
the dialog names, plus a busy neural runtime or a missing one), 4 failed, 5
cancelled with Ctrl+C.

The player is a Windows program rather than a console one, so `cmd.exe` does
not wait for it: use `start /wait "" DLSSVideoPlayer.exe --render ...` and read
`%ERRORLEVEL%`, or in PowerShell
`$p = Start-Process DLSSVideoPlayer.exe -ArgumentList '--render','clip.mp4' -Wait -PassThru -NoNewWindow; $p.ExitCode`.
Output redirected to a file or a pipe is written there as UTF-8. It shares the
neural runtime with a running player, so it waits for a render the player is
doing, and refuses after five seconds rather than interleaving with it.

## Save a converted video

1. Open a photo, GIF or video and wait for validated cached playback, or
   convert a clip or the whole video first.
2. Choose **DLSS > Convert & export > Save converted video**.
3. Choose a format and a new filename. Existing files are not overwritten.
4. Continue playback while saving runs. While it runs, the export row reads
   **Cancel export** - one row cancels whichever write is in flight.

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
remain separate; they are not enhanced or burned into the image, and the
subtitles the player shows (see "Subtitles") never reach an export.

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
