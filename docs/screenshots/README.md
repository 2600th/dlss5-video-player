# Screenshot provenance

Where every image in this folder came from. All of them are real output of the
player: nothing is mocked up, retouched, sharpened or colour-corrected. Records
for images that have since been replaced are in the git history of this file.

## Current images

| File | What it shows | Made |
| --- | --- | --- |
| `current/face-comparison.png` | The Matrix, source frame 2116, beside the same frame from the player's render. Identical unscaled 700x880 crops | 22 Sep 2026, v0.25.0 |
| `current/face-comparison-gta6.png` | GTA VI Trailer 2, source frame 1940, the same way | 22 Sep 2026, v0.25.0 |
| `current/gta6-lucia-original.jpg`, `gta6-lucia-neural.jpg` | That GTA VI frame, whole (2560x1440). The site hero and link card are cut from these | 22 Sep 2026, v0.25.0 |
| `current/matrix-neural.jpg`, `matrix-original.jpg` | The player paused on Trinity at 1:28, neural view on, then off | 22 Sep 2026, v0.25.0 |
| `current/neural-playback.jpg`, `original-comparison.jpg` | The player paused on GTA VI at 1:04, neural view on, then off | 22 Sep 2026, v0.25.0 |
| `current/neural-strength.jpg` | The image adjustments window with the DLSS 5 mix slider at 1.00, over a paused neural frame of the Mafia: The Old Country trailer | 24 Sep 2026, v0.25.0+ |
| `current/recent-videos.jpg` | The File menu (Game trailers, Recent videos, Save comparison image) over the same trailer, paused on a neural frame with the compare bar. The history submenu is closed, which keeps local paths out of frame | 24 Sep 2026, v0.25.0+ |
| `current/player-start.jpg` | The start screen on a fresh profile: the capability check and the game trailers | 24 Sep 2026, v0.25.0+ |
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

**Face comparisons.** Made with `tools/demo-video/make-face-comparison.py`:

```powershell
python tools/demo-video/make-face-comparison.py matrix.mp4 <matrix render>/neural.mkv `
    --frame 2116 --neural-frame 2056 --crop 960,186,700,880 --output docs/screenshots/current/face-comparison.png
python tools/demo-video/make-face-comparison.py gta6.mp4 <gta6 render>/neural.mkv `
    --frame 1940 --neural-frame 1886 --crop 740,144,700,880 --output docs/screenshots/current/face-comparison-gta6.png
```

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

## 24 September 2026 images

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
