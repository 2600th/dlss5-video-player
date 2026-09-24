# Screenshot provenance

Where every image in this folder came from. All of them are real output of the
player: nothing is mocked up, retouched, sharpened or colour-corrected. Records
for images that have since been replaced are in the git history of this file.

## Current images

| File | What it shows | Made |
| --- | --- | --- |
| `current/gta6-lucia-original.jpg`, `gta6-lucia-neural.jpg` | GTA VI Trailer 2, source frame 1940 and the same frame of the v0.25.0 render (Intensity, Local tone and Local structure 2.0), whole (2560x1440). The site hero and link card are cut from these | 22 Sep 2026, v0.25.0 |
| `current/matrix-neural.jpg`, `matrix-original.jpg` | The player paused on Trinity at 1:28, neural view on, then off | 22 Sep 2026, v0.25.0 |
| `current/neural-playback.jpg`, `original-comparison.jpg` | The player paused on GTA VI at 1:04, neural view on, then off | 22 Sep 2026, v0.25.0 |
| `current/player-start.jpg` | The start screen at 150% on a fresh profile: the capability check and the seven game trailers with their YouTube thumbnails | 24 Sep 2026, `dced888` |
| `current/compare-wipe.jpg` | 007 First Light paused on frame 1122 in Wipe, the divider down the face, with the compare bar | 24 Sep 2026, `dced888` + `f2ae230` |
| `current/compare-difference.jpg` | The same frame in Difference (x4, brightness only): where the model changed the picture | 24 Sep 2026, `dced888` + `f2ae230` |
| `current/compare-2x2-toast.jpg` | The same frame in 2 x 2 (original, DLSS 5, Difference, DLSS 5 at Mix 50%), with the toast that confirms **Save comparison image** | 24 Sep 2026, `dced888` + `f2ae230` |
| `current/saved-comparison-2x2.png` | The file that save wrote: the player's own PNG of the 2 x 2 view with its provenance footer, as saved | 24 Sep 2026, `dced888` + `f2ae230` |
| `current/subtitles.jpg` | A test subtitle file drawn over the DLSS 5 frame | 24 Sep 2026, `dced888` + `f2ae230` |
| `current/neural-strength.jpg` | Image adjustments, with the DLSS 5 mix slider at 1.00, over the DLSS 5 frame | 24 Sep 2026, `dced888` + `f2ae230` |
| `current/neural-settings.jpg` | The Neural settings dialog at its defaults | 24 Sep 2026, `dced888` + `f2ae230` |
| `current/recent-videos.jpg` | The File menu (Game trailers, Recent videos, Save comparison image) over the Mafia: The Old Country trailer, paused on a neural frame with the compare bar. The history submenu is closed, which keeps local paths out of frame. From the earlier round, at 175%, before the sliders were redrawn | 24 Sep 2026, v0.25.0+ |
| `2026-09-03/face-comparison.png` | The Witcher IV, frame 3375, source beside render. Kept because the 2 September verification report shows it | 3 Sep 2026, v0.13.0 |

## 22 September 2026 images

**Hardware and runtime.** RTX 4080 SUPER, driver 610.47 (`32.0.16.1047`),
ReShade 6.8.0.2155, RenoDX 6.5.3, DLSS-NR 310.8.0.

**Settings.** Intensity, Local tone and Local structure at 2.0 (the defaults are
1.0). Everything else at its default, upscaling off.

**Sources.** Two official uploads, both with age limit 0, fetched at the
player's YouTube **Auto** rung:

| Source | Video | SHA-256 |
| --- | --- | --- |
| [The Matrix \| 4K Trailer](https://www.youtube.com/watch?v=nUEQNVV3Gfs) (Warner Bros.) | 2560x1440 VP9, 23.976 fps, 147.271 s | `559ad1772b90adeedad54cd9257abffeddb34d91a2f90d8d8204669fbffc531e` |
| [Grand Theft Auto VI Trailer 2](https://www.youtube.com/watch?v=VQRLujxTm3c) (Rockstar Games) | 2560x1440 VP9, 30 fps, 166.733 s | `269585e3a8ed7d2b77b2869ee7fb575a8be2266f7e96bcd07298be6260307680` |

**Renders.** Each source was opened in the player with neural rendering on and
left to finish. The cache entries:

| Source | Range | Verified | Render key | Neural SHA-256 |
| --- | --- | --- | --- | --- |
| The Matrix | 2.502 s to end | 3,472 / 3,472, failure none, lock ok | `cd868340cb59c649…` | `537dc9aa996283b1025442b174c49f858e0a395fd32ab1900be61824a25b4798` |
| GTA VI | 1.800 s to end | 4,948 / 4,948, failure none, lock ok | `edc64443cf32a761…` | `bc99b9528ba2824b97f23cab42cc2fb48eb9ae8f231e012664125a2850234680` |

A render starts at its range's first frame, so source frame *n* is render frame
*n* − 60 for The Matrix and *n* − 54 for GTA VI. A picture check confirmed both
offsets: each pair differs least at that offset.

**Face comparisons.** The two unscaled face figures that used to sit here
(`face-comparison.png`, The Matrix, and `face-comparison-gta6.png`) were
replaced on 24 September 2026 by the comparison stills in
[`docs/media/stills/`](../media/stills/), each with its own provenance record;
see [docs/media/README.md](../media/README.md#comparison-stills).

**Player captures.** Each pair is one paused frame in a live session, with only
the view switched:

1. Send the player to 0:00 and step forward 10 s at a time with its own command.
2. Turn neural rendering on, so the session renders ahead of the paused playhead.
3. Play onto rendered frames and pause. Capture the neural view.
4. Turn neural rendering off (the frame stays where it is) and capture the original.

Commands went to the window as `WM_COMMAND`, as
`tools/verification/drive-neural-toggle.ps1` does. The window was captured by
handle with `tools/verification/capture-window.ps1` and cropped to its visible
frame (1440x930), then saved once as JPEG at quality 95 with no chroma
subsampling.

The pairs really are one frame each. Across the video area, the GTA VI pair
differs by a mean of 9.5 levels and the Matrix pair by 5.2. A wipe taken in the
same session matched the original left of its divider (0.16) and the render to
the right (0.0).

## 24 September 2026 images, second round

**Hardware and runtime.** RTX 4080 SUPER, driver 610.47 (`32.0.16.1047`),
runtime lock `310.8.SF-v2`. The player is this repository at `dced888`, plus
the fix `f2ae230` (cached playback decoded the render in the wrong pixel
layout and could show a striped picture) for every shot after the start
screen. Every shot was checked for that artifact; none has it.

**Display.** The player sat on a 1920x1080 display set to 150% scaling for
these captures (the machine's own setting is 100%; it was put back
afterwards). Each is the window's visible frame, 1902x1023, captured by handle
with `tools/verification/capture-window.ps1` and cut to DWM's extended frame
bounds. The tool now asks for per-monitor DPI awareness: before that, a window
on a monitor scaled unlike the primary (175% here) came back 2240x1204 with the
player in its top-left corner. `neural-strength.jpg` is two such captures, the
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

**The saved comparison** is exactly what **File > Save comparison image**
wrote. Its footer reads: `DLSS 5 Video Player 0.25.0 · 007-First-Light-Story-Trailer`,
`00:00:18:42 · frame 1122 · 2 × 2 (fourth pane Mix 50%) · Mix 100% · Zoom Fit`,
`Settings sha256:ee4d77128a1605d2 · Runtime 310.8.SF-v2 · Saved 2026-09-24 16:44:22`.
The settings digest there is the player's own (canonical settings and guides);
the render's cache manifest records `96bf471a…` for the same settings under its
own hashing.

**Not retaken.** `recent-videos.jpg` needs an open menu, which a posted message
did not open; real input would have taken the pointer and focus from the person
using the machine. It is from the earlier round and has no playback artifact
(checked). The GTA VI and Matrix pairs and the full GTA VI frames are v0.25.0
captures the site still uses.

## 24 September 2026 images, first round

`recent-videos.jpg` is still from this round; the `neural-strength.jpg` and
`player-start.jpg` described here have since been replaced (above).

**Hardware and runtime.** The 22 September machine and runtime (RTX 4080
SUPER, driver 610.47, RenoDX 6.5.3, DLSS-NR 310.8.0), at 175% display scaling.
The build is the w3-ui branch after v0.25.0: Common Controls 6, dark popup
menus, the compare bar and the start screen as they ship next.

**Source.** *Mafia: The Old Country - Family Takes Sacrifice* (Mafia Game), the
official upload in the player's trailer list, 2560x1440 at 30 fps, opened as a
local copy of the player's cached source. Neural rendering was turned on at
0:00, the video played and was paused at 0:21 (the menu) and 0:41 (the
adjustments window), both inside the rendered range with the neural view on.
Default neural settings.

**Captures.** Every window was captured by handle with `PrintWindow` and cut to
its visible frame (DWM's extended frame bounds), as
`tools/verification/capture-window.ps1` does, because the workstation was
locked and the screen itself could not be read. A popup is its own window, so
`recent-videos.jpg` and `neural-strength.jpg` are each two such captures - the
player, and the open File menu or the adjustments window - with the second
placed at its real on-screen offset from the first. Nothing else is added or
changed. Saved once as JPEG at quality 95 with no chroma subsampling.

**One correction.** Those captures drew into a GDI+ bitmap's HDC, and GDI+ turns
every pixel of exactly RGB(13,11,12) in that HDC into (0,0,0) with alpha 0. The
neural frame at 0:21 has a flat shadow that decodes to exactly that colour, so
the first `recent-videos.jpg` showed 798 pixels of black patches on the vest and
trousers that the player never drew. The window capture was the same, except for
those pixels. `recent-videos.jpg` was rebuilt from it with them set back to
(13,11,12): a pixel with alpha 0 inside a `PrintWindow` capture can only be that
colour. Everywhere else it matches the first JPEG to a mean of 0.01 levels.
`neural-strength.jpg` had no such pixel. `capture-window.ps1` now captures into
a GDI bitmap of its own, and `-SelfTest` checks that colour.

**Size.** 1493x932 (`recent-videos.jpg`, `player-start.jpg`) and 1493x1100
(`neural-strength.jpg`). The older shots were 1442x932 at 100%; at 175% the
player's minimum width is 1493 visible pixels, and the adjustments window
(933 pixels tall at that scale) needed a taller player to sit over.

## Earlier images

**12 September 2026** (the previous `neural-strength.jpg`, in this file's
history). v0.21.0 on an RTX 5090, driver 616.64, RenoDX 4.7. The frame is
*The Godfather 50th Anniversary Trailer* (Paramount Pictures) paused at
73.98 s, from a live session that verified 2,875 of 2,875 frames. The window's
visible frame (1442x932), JPEG quality 95, no chroma subsampling.

**3 September 2026** (`2026-09-03/face-comparison.png`, and the previous
`recent-videos.jpg` and `player-start.jpg`). v0.13.0 on an RTX 5090, full
1442x932 windows captured with FFmpeg. The face comparison uses *The Witcher IV* Cinematic
Reveal Trailer (CD PROJEKT RED), 1920x1080 at 30000/1001 fps, zero-based frame
3375 (112.6 s). Both 700x880 crops are at x=670, y=0 from the source and its
fully verified render (10,868 of 10,868 frames).

## Rights

These images document the software. They are not an image-quality benchmark,
an official NVIDIA integration, or an endorsement by anyone shown. Footage
belongs to its owners: The Matrix to Warner Bros., Grand Theft Auto VI to
Rockstar Games, The Godfather to Paramount Pictures, The Witcher IV to CD
PROJEKT RED and Mafia: The Old Country to 2K and Hangar 13. NVIDIA components belong to NVIDIA. The source-code licence
doesn't relicense any of it.
