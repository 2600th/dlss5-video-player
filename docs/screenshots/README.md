# Screenshot provenance

Where every image in this folder came from. All of them are real output of the
player: nothing is mocked up, retouched, sharpened or colour-corrected. Records
for images that have since been replaced are in the git history of this file.

## Current images

| File | What it shows | Made |
| --- | --- | --- |
| `current/vsr-wipe.jpg` | A 1280x720 copy of 007 First Light paused on frame 1122 in Wipe, compared against RTX VSR: the player's plain (bilinear) scale of the original left of the divider, RTX VSR at High right of it, the picture at 1.13x the video | 2 Oct 2026, `9d0f790` |
| `current/vsr-wipe-crop.png` | The picture area of `vsr-wipe.jpg`, 1:1, with both tags | 2 Oct 2026, `9d0f790` |
| `current/export-stages-vsr.jpg` | **Export with DLSS stages** with Super Resolution alone, 2160p, Upscaler **RTX VSR (recommended)**, and its summary line | 2 Oct 2026, `9d0f790` |
| `current/player-start.jpg` | The start screen after two renders and a restart: the capability check with its RTX VSR line, two Recent tiles at Rendered 100%, and the Game trailers row | 2 Oct 2026, `9d0f790` |
| `current/compare-wipe.jpg` | 007 First Light paused on frame 1122 in Wipe, the divider down the face, with the compare bar | 2 Oct 2026, `9d0f790` |
| `current/compare-difference.jpg` | The same frame in Difference (x4, brightness only): where the model changed the picture | 2 Oct 2026, `9d0f790` |
| `current/compare-2x2-toast.jpg` | The same frame in 2 x 2 (original, DLSS 5, Difference, RTX VSR at High), with the toast that confirms **Save comparison image** | 2 Oct 2026, `9d0f790` |
| `current/saved-comparison-2x2.png` | The file that save wrote: the player's own PNG of the 2 x 2 view with its provenance footer, as saved | 2 Oct 2026, `9d0f790` |
| `current/neural-playback.jpg`, `original-comparison.jpg` | The player paused on GTA VI Trailer 2 at frame 1940 (1:04.7) at default settings, the DLSS 5 view, then neural rendering off | 2 Oct 2026, `9d0f790` |
| `current/photo-wipe.jpg` | A *Mafia: The Old Country* trailer frame saved as a PNG and opened as a photo, in Wipe, the divider down the man's nose | 25 Sep 2026, 0.26.0 |
| `current/subtitles.jpg` | A test subtitle file drawn over the DLSS 5 frame | 24 Sep 2026, `dced888` + `f2ae230` |
| `current/neural-strength.jpg` | Image adjustments, with the DLSS 5 mix slider at 1.00, over the DLSS 5 frame | 24 Sep 2026, `dced888` + `f2ae230` |
| `current/neural-settings.jpg` | The Neural settings dialog at its defaults | 24 Sep 2026, `dced888` + `f2ae230` |

## 2 October 2026 images

**Hardware and runtime.** NVIDIA GeForce RTX 5090, driver 617.14
(`32.0.16.1714`), runtime lock `310.8.SF-v2` (ReShade 6.8.0.2155, RenoDX
6.5.3, DLSS-NR 310.8.0, as each render's receipt records them).

**The player.** This repository at `9d0f790` (the RTX VSR playback branch,
which still reports itself as 0.27.2), the tested `build-upscaling` Release
build, copied with its neural runtime, ffmpeg, ffprobe, yt-dlp and deno into a
new folder with no settings file, cache or logs. That folder was too deep for
the portable cache beside the exe (the player refuses a root whose staging
paths would overflow, and falls back to LocalAppData, which held another
install's cache and history), so before anything was opened the copy's
settings file named an empty cache folder of its own (`[Storage]
CacheDirectory`, `CacheDirectoryAutomatic=0`). Every other setting started at
its default: neural settings at 1.0 with Skin structure Off and one pass, RTX
VSR Upscaling on, RTX VSR quality High, DLSS Upscaling off.

**Display.** The machine's own two 1920x1080 displays at 100% scaling (96 dpi),
unchanged; the player on the primary one. At 100% the player's chrome is a
smaller share of the window than in the 150% captures these replace, so the
picture is larger and the text smaller.

**Sources.** Both through **File > Game trailers**, at the YouTube **Auto**
rung (1440p), so the player titles them with the trailer list's names:

| Source | Video | SHA-256 |
| --- | --- | --- |
| [007 First Light - Story Trailer](https://www.youtube.com/watch?v=trvIyyFt_MM) (PlayStation) | 2560x1440 VP9, 59.94 fps, 96.495 s | `bd5f702f4087de7f7a2fa8e7c5b36436c9d66c38e4ac54b32c656c4cb97213ae` |
| [Grand Theft Auto VI Trailer 2](https://www.youtube.com/watch?v=VQRLujxTm3c) (Rockstar Games) | 2560x1440 VP9, 30 fps, 166.789 s | `19416a84caaaaa214077cb52c70b3db9df4ac6243d620f1cc537dea1acc1298d` |

**Renders.** Each was opened and **DLSS > Convert & export > Convert whole
video to neural video** posted, which copied the source into the cache and
rendered all of it (72 s for 007, 55 s for GTA VI):

| Source | Verified | Render key | Neural SHA-256 |
| --- | --- | --- | --- |
| 007 First Light | 5,784 / 5,784, failure none, lock ok | `2d6217f001aede27…` | `e730f75d71422f846f1af95920939d92566230a8b265f7d1999b9c16cd700711` |
| GTA VI | 5,002 / 5,002, failure none, lock ok | `2161305902a7f543…` | `24af6909095911ee71cd573bd776360fc08eb5ea2be09a876e3a6e15a93090f1` |

A whole-video render's frame *n* is source frame *n*.

**How the player was driven.** By posted window messages only: `WM_COMMAND`
with the menu ids in `src/AppMenu.h`, `SetWindowPos` (no activation, no
z-order change) for the window's size, and for the dialogs `WM_SETTEXT`,
`BM_SETCHECK` and `CB_SETCURSEL` on their controls followed by the
notification each dialog acts on. No pointer or keyboard input, so the person
using the machine kept both. Frames were reached with **Playback > Go to
timecode** (`00:00:18:42` is frame 1122 of 007, `00:01:04:20` frame 1940 of GTA
VI) while paused.

**Capture.** Each window was captured by handle with
`tools/verification/capture-window.ps1` (a dialog, which is a window of its
own, through the same capture function given its handle), cut to DWM's
extended frame bounds, and saved once as JPEG at quality 95 with no chroma
subsampling; the two PNGs are lossless. Nothing else was changed.

**One thing posted twice.** The picture's tags fade in on a timer after a
compare mode changes. In this unattended session the fade's last frame was not
presented, and the tags stayed invisible on a paused frame; posting the same
mode command a second time, which changes nothing, presented them. Every
compare capture below was taken after that second command.

**Frame checks.** Each capture was compared with frames *n* − 1, *n* and *n* +
1 of the source and of the render, area-scaled to the picture (mean absolute
difference in levels):

| Capture | Against | *n* − 1 | *n* | *n* + 1 |
| --- | --- | --- | --- | --- |
| GTA VI, neural rendering off | source | 7.03 | **0.97** | 7.71 |
| GTA VI, DLSS 5 view | render | 7.52 | **1.00** | 8.23 |
| 007 First Light, DLSS 5 view (before Wipe) | render | 1.24 | **0.72** | 1.20 |

The saved comparison's footer says frame 1122 as well.

### 007 First Light, frame 1122

`compare-wipe.jpg`, `compare-difference.jpg` and `compare-2x2-toast.jpg`: the
window set to a 1902x1023 visible frame (client 1900x971), the size of the
captures they replace; the picture is 1449x815, 0.57x the video. The 007 First
Light tile in **File > Game trailers** reopened the cached source with its
render. Paused, frame 1122, then **Wipe** with its divider placed by a posted
left-button press on the picture at 60.8% of its width (where the previous
round had it), **Difference** at its default x4 brightness, and **2 x 2**.

On this build the 2 x 2 view's fourth pane is **RTX VSR · High**, not DLSS 5 at
a second Mix: where RTX VSR can run, it takes that pane. Here the picture is
shown smaller than the video, so it is made at the source size (the log says
`input=2560x1440 output=2560x1440 quality=3`) and the pane shows what VSR does
to the original without enlarging it.

**File > Save comparison image** opened its save dialog; its file name box was
given a scratch folder plus the name the player suggested, and Save was
posted. The toast was captured 1.2 s later. `saved-comparison-2x2.png` is that
file, byte for byte: 1449x906, the 1449x815 view plus a 91 px footer reading
`DLSS 5 Video Player 0.27.2 · 007 First Light`,
`00:00:18:42 · frame 1122 · 2 × 2 (fourth pane RTX VSR High (default)) · Mix 100% · Zoom Fit`,
`Settings sha256:ee4d77128a1605d2 · Runtime 310.8.SF-v2 · Saved 2026-10-02 21:51:09`.
The settings digest is the one the 24 September footer gave for the defaults.

### RTX VSR against plain scaling

`vsr-wipe.jpg` and `vsr-wipe-crop.png`. RTX VSR only works where the picture is
shown larger than the video, which a 2560x1440 trailer never is on a 1080p
display, so the source is a 1280x720 copy of the cached 007 source, made with
the bundled ffmpeg:

```
ffmpeg -i source.mkv -map 0:v:0 -map 0:a:0
  -vf scale=1280:720:flags=lanczos+accurate_rnd+full_chroma_int -fps_mode passthrough
  -c:v libx264 -preset slow -crf 12 -pix_fmt yuv420p
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -c:a copy
  "007 First Light 720p.mkv"
```

5,784 frames, SHA-256
`3ee4dd70c80f55720b86892c98edab40c3a82e39d939931f4ba7a578baaddff4`. It was
opened as a local file (on the player's command line, after the start screen
had been captured, so no local name reached a Recent tile) in the 1902x1023
window and rendered whole, because the compare views need a render (render key
`118cb8468703a30b…`, 5,784 / 5,784 verified). Then, paused on frame 1122:
**Wipe** and **Video > Compare > Compare against RTX VSR** (`Shift+R`'s
command), the divider still at 60.8%.

The picture is 1449x815: **1.13x** the video, the largest Fit makes on this
display, because the menu, compare bar, controls and status line take 156 of
the client's 971 rows. **Fill** reaches 1.48x but pushes the tags out of view,
so it was not used. RTX VSR runs at High, its default (`1280x720 -> 1449x815`
in the log). Left of the divider is the compositor's bilinear scale of the
original. A check on the capture: the left side differs from a bilinear scale
of the copy's frame 1122 by 1.04 levels (1.52 and 1.50 for frames 1121 and
1123), the right side by 1.42; the right side's mean Laplacian, a measure of
fine detail, is 2.72 against the left's 2.09 beside the divider. The status
line reads `DLSS Upscaling off`, which is right: playback VSR stands aside
while a compare view draws both members.

`vsr-wipe-crop.png` is the render window's area of that capture, x 226-1675 and
y 55-790 of the visible frame: 1449x735, unscaled and lossless, both tags in
it. It is one frame of a game trailer on one display; the measured comparison
is the [RTX VSR quality report](../measurements/vsr-quality-20261002/REPORT.md),
not this picture.

A 960x540 copy made the same way showed at 1.51x in the same window and was
captured too; it is not used here.

### GTA VI on and off, frame 1940

`neural-playback.jpg` and `original-comparison.jpg`: the window set to a
1440x930 visible frame (client 1438x878), the size of the pair they replace.
The GTA VI tile reopened the cached source with its render, in the DLSS 5 view;
paused, frame 1940, captured; then **Neural Rendering** off, which leaves the
frame where it is, captured again. Default settings, unlike the 22 September
pair, which was taken with Intensity, Local tone and Local structure at 2.0.

### Export with DLSS stages

`export-stages-vsr.jpg`, with GTA VI loaded: **DLSS > Convert & export > Export
with DLSS stages** opened its window, which is a tool window of its own, and
was captured by its own handle: 472x468 at 100%. **Super Resolution** was
ticked, **Output height** set to 2160p and **Upscaler** to **RTX VSR
(recommended)**; Neural rendering and Frame generation stayed unticked, as they
open. The summary reads `3840 × 2160 at 30 fps · 1 pass`. Nothing was
exported, so no save dialog or output path exists. **History** is greyed, since
it applies to DLSS Super Resolution only, and at this size its text is cut to
"Per-frame (recommendec".

### The start screen

`player-start.jpg`: after both renders the player was closed and started
again with nothing to open, at the size it opens at (80% of the work area,
a 1116x819 visible frame, client 1114x767). The capability check reads GPU,
driver 617.14 against the 610.47 minimum, the neural runtime matching the lock,
**RTX VSR · Upscales playback shown larger than the video**, and the player's
render-speed estimate. **Recent** has GTA VI - Trailer 2 and 007 First Light,
each **Rendered 100%**, with the frame each was left on; **Game trailers** has
the other five, with thumbnails from `i.ytimg.com`. Taken before any local file
was opened.

**Not retaken.** `subtitles.jpg`, `neural-strength.jpg` and
`neural-settings.jpg` (24 September, below) and `photo-wipe.jpg` (25
September). `recent-videos.jpg`, the File menu over a paused frame, is retired:
a menu needs real input to open, and the start screen above now shows Recent
videos.

## 25 September 2026 image

**What it is.** `photo-wipe.jpg` shows the player's photo path: one frame of a
trailer, written out as a PNG and opened with the player the way you would open
a game screenshot. The frame is 864 (0:28.8) of *Mafia: The Old Country -
Family Takes Sacrifice* (Mafia Game), the official upload in the player's
trailer list, from a copy the player had cached (2560x1440, 30 fps, SHA-256
`68d791b6b674a88f34a42b830b59e9dfa12cfaed7d3cd6f6c52a62765fea24fc`). ffmpeg
decoded that frame to `mafia-the-old-country.png` (SHA-256
`c75f7df5d3ae1f4c06d8b21100aeb69698de4cec3ac1dd29e9a94349cecf8c81`). It was
picked from four candidate frames rendered the same way, as the one where the
change was plainest.

**Hardware, runtime and settings.** RTX 4080 SUPER, driver 610.47
(`32.0.16.1047`), runtime lock `310.8.SF-v2`. The player is 0.26.0: the build
tree's player source is the release's (`a4a3411`), on a fresh profile, so the
neural settings are at their defaults (digest `96bf471a…`, the same as the
comparison stills). The render is cache entry `36fc8c03597e3080…`, one frame,
verified.

**Timing.** `DLSSVideoPlayer.exe --render <png> --out <png>` took 7.1 to 8.1 s
per 2560x1440 frame, launch to exit, across the four candidates.

**Capture.** Opened in the player on the 3840x2160 display at its own 175%
scaling, the window set to 2240x1204. Neural rendering, then Wipe, were sent as
`WM_COMMAND`, and the divider was placed by a left-button press posted to the
picture at 30.5% of its width. The window was captured by handle with
`tools/verification/capture-window.ps1`, then area-downscaled once to
1902x1023, the size of the 150% captures beside it, and saved as JPEG at quality
95 with no chroma subsampling. Nothing else was changed.

## 24 September 2026 images

`subtitles.jpg`, `neural-strength.jpg` and `neural-settings.jpg` are left from
this round. Its Wipe, Difference, 2 x 2, saved-comparison and start-screen
images were replaced on 2 October; the demonstration video's player scenes and
the social square's saved 2 x 2 PNG
([docs/media/README.md](../media/README.md)) are from this round too, and the
records of the replaced files are in this file's history.

**Hardware and runtime.** RTX 4080 SUPER, driver 610.47 (`32.0.16.1047`),
runtime lock `310.8.SF-v2`. The player is this repository at `dced888`, plus
the fix `f2ae230` (cached playback decoded the render in the wrong pixel
layout and could show a striped picture), applied before any of these shots. Every shot was checked for that artifact; none has it.

**Display.** The player sat on a 1920x1080 display set to 150% scaling for
these captures (the machine's own setting is 100%; it was put back
afterwards). Each is the window's visible frame, 1902x1023, captured by handle
with `tools/verification/capture-window.ps1` and cut to DWM's extended frame
bounds. `neural-strength.jpg` is two such captures, the
player and the Image adjustments window placed at its real on-screen offset
from it (599, 152). Saved once as JPEG at quality 95 with no chroma
subsampling. `neural-settings.jpg` is the dialog's own capture: the player
behind a modal dialog came back black.

**Settings.** A fresh profile, so neural settings at their defaults (the
dialog in `neural-settings.jpg` shows them). The render is a whole-video render
of *007 First Light - Story Trailer* made through the player's YouTube Auto path
(see [docs/media/README.md](../media/README.md#comparison-stills)); for these
shots the cached source was opened as a local file, which found the same render
in the cache. Frame 1122 (0:18.7), paused with **Go to timecode**. Everything
was driven by posted window messages; the loupe in the demonstration video and
the wipe divider needed the real pointer over the picture for a moment.

**The subtitle** is a two-line test file written for this shot, loaded with
**Load subtitle file**. It says what it is; it is not the trailer's dialogue.

## Rights

These images document the software. They are not an image-quality benchmark,
an official NVIDIA integration, or an endorsement by anyone shown. Footage
belongs to its owners: Grand Theft Auto VI to Rockstar Games, 007 First Light
to IO Interactive and Mafia: The Old Country to 2K and Hangar 13. NVIDIA
components belong to NVIDIA. The source-code licence doesn't relicense any of
it.
