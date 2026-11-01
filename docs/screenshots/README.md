# Screenshot provenance

Where every image in this folder came from. All of them are real output of the
player: nothing is mocked up, retouched, sharpened or colour-corrected. Records
for images that have since been replaced are in the git history of this file.

## Current images

| File | What it shows | Made |
| --- | --- | --- |
| `current/neural-playback.jpg`, `original-comparison.jpg` | *Grand Theft Auto VI: An Extended Look* paused on frame 40 of a 3 s cut (source frame 400) at default settings, DLSS 5 on, then off: the toolbar's **DLSS 5** pill and the pipeline chip, first of the status row's chips | 1 Nov 2026, `66dae69` |
| `current/compare-wipe.jpg` | The same frame in Wipe, the divider down the face, with the compare bar and the toolbar | 31 Oct 2026, `66dae69` |
| `current/compare-difference.jpg` | The same frame in Difference (x4, brightness only), with the compare bar's **×4** and **Luma \| Color** controls | 31 Oct 2026, `66dae69` |
| `current/compare-2x2-toast.jpg` | The same frame in 2 x 2 (original, DLSS 5, Difference, RTX VSR at High), with the toast in the status row that confirms **Save comparison image** | 31 Oct 2026, `66dae69` |
| `current/saved-comparison-2x2.png` | A file that save wrote: the player's own PNG of a 2 x 2 view of 007 First Light, frame 1122, with its provenance footer, as saved | 2 Oct 2026, `9d0f790` |
| `current/vsr-wipe.jpg` | A 960x540 copy of the cut paused on frame 40 in Wipe, compared against RTX VSR: the player's plain (bilinear) scale of the original left of the divider, RTX VSR at High right of it, the picture at 1.51x the video | 1 Nov 2026, `66dae69` |
| `current/vsr-wipe-crop.png` | The picture area of the 2 October `vsr-wipe.jpg` (a 960x540 copy of 007 First Light, frame 1122), 1:1, with both tags | 2 Oct 2026, `9d0f790` |
| `current/export-stages-vsr.jpg` | **File > Export...** (the **Export with DLSS stages** window) with Super Resolution alone, 2160p, Upscaler **RTX VSR (recommended)**, and its summary line | 1 Nov 2026, `66dae69` |
| `current/player-start.jpg` | The start screen after two renders and a restart: the capability check with the driver 621.01 notice and the RTX VSR line, two Recent tiles at Rendered 100%, and the Game trailers row | 1 Nov 2026, `66dae69` |
| `current/subtitles.jpg` | A test subtitle file drawn over the DLSS 5 frame | 1 Nov 2026, `66dae69` |
| `current/neural-strength.jpg` | **View > Viewing adjustments...**: the Picture panel in its real place over the DLSS 5 frame, Mix (DLSS 5 amount) at 100% in its viewing zone | 1 Nov 2026, `66dae69` |
| `current/neural-settings.jpg` | **Enhance > Picture...** at its defaults, the render zone turned to **Render pipeline**, with the **Network** row (A, the 0.28 look) | 1 Nov 2026, `66dae69` |
| `current/picture-menu.jpg` | The right-click menu on the picture, over the DLSS 5 frame | 1 Nov 2026, `66dae69` |
| `current/photo-wipe.jpg` | The source's frame 400 saved as a PNG and opened as a photo, in Wipe, the divider down the face | 1 Nov 2026, `66dae69` |

## 31 October 2026 images

Every image above made on 31 October or 1 November: retaken because 0.30.0
restructured the menus, the toolbar, the compare bar and the dialogs, plus one
new image, `picture-menu.jpg`. The session was interrupted after the three
compare views on the evening of 31 October; the rest were taken the next
morning from the same player folder, cache and clips, with the player started
again.

**Hardware and runtime.** NVIDIA GeForce RTX 5090, driver 621.01
(`32.0.16.2101`). Every render's receipt records ReShade 6.8.0.2155, RenoDX
10.8.0 and DLSS-NR 310.9.16.0, the current runtime on RTX 50, with network A
and the lock ok. On this driver the start screen and the log carry the
player's notice that 621.01 freezes video on some RTX 50 cards (NVIDIA bug
6979343). The notice is real; no capture showed a frozen or black picture.

**The player.** This repository at `66dae69` (0.30.0), the tested
`build-upscaling` Release build, copied with its staged neural runtime
(`ReShade.ini` and `ReShadePreset.ini` included), ffmpeg, ffprobe, yt-dlp and
deno into a new folder, `%TEMP%\dlss5-shots\player`, with no settings file,
cache or logs. The path is short enough for the portable cache beside the exe,
which it used. Every setting started at its default: Picture at Standard,
network A, one pass, RTX VSR the upscaler at High. The build has no `lang\`
folder; every launch passed `--lang en`.

**Display.** The machine's two 1920x1080 displays at 100% scaling (96 dpi
each), unchanged; the player on the second (`\\.\DISPLAY12`, SDR). Each image
is the window's visible frame (DWM's extended frame bounds), copied from the
screen after the player's windows were raised above the desktop
(`SetWindowPos` topmost without activation, then back); a panel or menu over
the player is in its real place. Saved once as JPEG at quality 95 with no
chroma subsampling. Nothing else was changed.

**Source.** The 2 October captures used a 007 First Light frame that is not
available locally, so every new image is of *Grand Theft Auto VI: An Extended
Look*: the camera-original `gtavi-extended.webm` (2560x1440 VP9, 30 fps, 26 s,
no audio), cut with the bundled ffmpeg as the 4 October round did:

```
ffmpeg -ss 12 -i gtavi-extended.webm -t 3 -map 0:v:0
  -c:v libx264 -preset slow -crf 8 -pix_fmt yuv420p
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv
  "GTA VI - An Extended Look.mkv"
```

90 frames, SHA-256
`47e618a9036bf1948b50853bf111d05067e95544b70d77d7e694d0f8d07d2339`. Frames 24
to 59 are one shot, a woman on a weight bench seen from above; frame 40
(`00:00:01:10`, the source's frame 400 at 0:13.33) is the frame in every
image. For the RTX VSR image, a 960x540 copy of the cut made with the 2 October
command (Lanczos, CRF 12, below): `GTA VI - An Extended Look 540p.mkv`, 90
frames, SHA-256
`fbcf4d78d0f673d7dd077f7563918fb9087453bbb82ea9c4d9377c1d516f34eb`.

**Renders.** Each clip was opened as a local file on the player's command line
and **Enhance > Render whole video** posted:

| Clip | Verified | Render key |
| --- | --- | --- |
| The cut | 90 / 90, failure none, lock ok | `18ce6ddc8e5e64e3…` |
| The 540p copy | 90 / 90, failure none, lock ok | `005e1c03b4106ae6…` |
| The PNG, as a photo | 1 / 1 | `55f4b47706afb2b0…` |

**How the player was driven.** By posted window messages only: `WM_COMMAND`
with the menu ids in `src/AppMenu.h`, `SetWindowPos` (no activation) for the
window's size, `WM_SETTEXT`, `BM_SETCHECK` and `CB_SETCURSEL` on dialog
controls followed by the notification each dialog acts on, and posted button
presses on the picture. No pointer or keyboard input. Frame 40 was reached with
**Playback > Go to timecode** (`00:00:01:10`) while paused. As on 2 October,
each compare mode was posted twice, so the picture's tags were presented.

**Frame checks.** The on and off pair against frames 39, 40 and 41 of the
render and of the cut, area-scaled to the picture (mean absolute difference in
levels). This shot moves little, so neighbours are close, but frame 40 is
nearest in both; the saved comparison's footer says frame 40 as well.

| Capture | Against | 39 | 40 | 41 |
| --- | --- | --- | --- | --- |
| DLSS 5 on | render | 3.56 | **2.05** | 3.69 |
| DLSS 5 off | the cut | 3.59 | **2.09** | 3.71 |

### The cut, frame 40

`neural-playback.jpg` and `original-comparison.jpg`: the window set to a
1440x930 visible frame, the size of the pair they replace. The cut reopened
with its render, in the DLSS 5 view; paused on frame 40, captured; then
**Enhance > DLSS 5 neural rendering** off, which leaves the frame where it is,
captured again. The pipeline chip reads **DLSS 5** in both.

`compare-wipe.jpg`, `compare-difference.jpg`, `compare-2x2-toast.jpg`,
`subtitles.jpg` and `picture-menu.jpg`: the window set to a 1902x1023 visible
frame, as on 2 October; the picture is 1449x815, 0.57x the video. **Wipe**
with its divider placed by a posted left-button press on the picture at 50.8%
of its width, down the face; **Difference** at its default x4, brightness
only; **2 x 2**, whose fourth pane is **RTX VSR · High**, made at the source
size since the picture is smaller than the video. **File > Save comparison
image** opened its save dialog; its file name box was given a scratch folder
and Save was posted, and the toast was captured 0.6 s later. That file is not
committed: `saved-comparison-2x2.png` stays the 2 October one. The new
file's footer reads `DLSS 5 Video Player 0.30.0 · GTA VI - An Extended Look`,
`00:00:01:10 · frame 40 · 2 × 2 (fourth pane RTX VSR High (default)) · Mix 100% · Zoom Fit`,
`Settings sha256:18848b0a97296edb · Runtime 310.9.16 · network A · Saved 2026-10-31 20:10:54`.

The subtitle is a two-line SubRip file written for this shot, loaded with
**Playback > Subtitles > Load subtitle file**. It says what it is; it is not
the trailer's dialogue.

`picture-menu.jpg`: `WM_CONTEXTMENU` posted to the picture at (1010, 120) of
its client area, in the DLSS 5 view; the menu opened there, was captured with
the player, and closed without a choice.

### The Picture panel

`neural-strength.jpg`: the window set to a 1920x1032 visible frame, the size
of the image it replaces, in the DLSS 5 view on frame 40; **View > Viewing
adjustments** opened the Picture panel, 942x582, centred over the player,
where it stays in the capture. `neural-settings.jpg`: **Enhance > Picture**
and the panel's **Render pipeline ›** button, the panel captured on its own.
Everything in it is at its default; **Network** reads
**A · the 0.28 look (recommended)**.

### RTX VSR against plain scaling

`vsr-wipe.jpg`: the 540p copy opened on the command line in the 1902x1023
window and rendered whole, because the compare views need a render. Paused on
frame 40: **Wipe**, **Compare > Compare against RTX VSR**, the divider at
50.8%. The picture is 1449x815, **1.51x** the video, and RTX VSR runs at High,
its default (`960x540 -> 1449x815` in the log). Left of the divider is the
compositor's bilinear scale of the original.

### Export

`export-stages-vsr.jpg`: with the 540p copy open, **File > Export...** opened
the **Export with DLSS stages** window, a tool window of its own, 472x486 at
100%, captured on its own. **Super Resolution** was ticked, **Neural
rendering** and **Frame generation** unticked, **Output height** set to 2160p
and **Upscaler** to **RTX VSR (recommended)**. The summary reads
`3840 × 2160 at 30 fps · 1 pass`. Nothing was exported. **History** is greyed,
since it applies to DLSS Super Resolution only.

### The start screen

`player-start.jpg`: after both clips had rendered, the player was started
again with nothing to open, and its window set to a 1116x819 visible frame,
the size of the image it replaces (it opens at 1088x819 here). The capability
check reads GPU, driver 621.01 with its notice, the neural runtime matching the
lock (310.9.16, network A), **RTX VSR · Upscales playback shown larger than the
video**, and the render-speed estimate. **Recent** has the two clips, each
**Rendered 100%** and titled with its file name; **Game trailers** shows five
of the seven, with thumbnails the player fetched from `i.ytimg.com` itself, as
it does by default.

### The photo

`photo-wipe.jpg`: the source's frame 400 decoded by the bundled ffmpeg to
`gta-vi-extended-look-frame-400.png` (2560x1440, SHA-256
`17069a6fc477ba841af5b59f3f15ea9dd2e997335b58822c1813bf873f8356bc`; the cut's
frame 40 is 50 dB PSNR from it), opened on the command line in the 1902x1023
window. It opens with DLSS 5 off; **Enhance > DLSS 5 neural rendering**
rendered it, then **Wipe** with the divider at 50.8%. The same PNG through
`DLSSVideoPlayer.exe --render <png> --out <png>` took 12.1 to 12.2 s, launch
to exit, three times.

## 2 October 2026 images

`saved-comparison-2x2.png` and `vsr-wipe-crop.png` are still from this round;
its other images were replaced on 31 October and 1 November.

**Hardware and runtime.** NVIDIA GeForce RTX 5090, driver 617.14
(`32.0.16.1714`), runtime lock `310.8.SF-v2` (ReShade 6.8.0.2155, RenoDX
6.5.3, DLSS-NR 310.8.0, as each render's receipt records them).

**The player.** This repository at `9d0f790` (the RTX VSR playback branch,
which still reports itself as 0.27.2), the tested `build-upscaling` Release
build, copied with its neural runtime, ffmpeg, ffprobe, yt-dlp and deno into a
new folder with no settings file, cache or logs, and an empty cache folder of
its own. Every setting started at its default: neural settings at 1.0 with
Skin structure Off and one pass, RTX VSR Upscaling on, RTX VSR quality High,
DLSS Upscaling off.

**Display.** The machine's own two 1920x1080 displays at 100% scaling (96 dpi),
unchanged. Each window was captured by handle with
`tools/verification/capture-window.ps1` and cut to DWM's extended frame
bounds; the PNGs are lossless.

**Source.** [007 First Light - Story Trailer](https://www.youtube.com/watch?v=trvIyyFt_MM)
(PlayStation) through **File > Game trailers** at the YouTube **Auto** rung
(1440p): 2560x1440 VP9, 59.94 fps, 96.495 s, SHA-256
`bd5f702f4087de7f7a2fa8e7c5b36436c9d66c38e4ac54b32c656c4cb97213ae`, rendered
whole (5,784 / 5,784 verified, failure none, lock ok, render key
`2d6217f001aede27…`). Driven by posted window messages only; frame 1122 was
reached with **Playback > Go to timecode** (`00:00:18:42`) while paused.

**The saved comparison.** In a 1902x1023 window, paused on frame 1122, in
**2 x 2** (fourth pane **RTX VSR · High**), **File > Save comparison image**
wrote `saved-comparison-2x2.png`, byte for byte: 1449x906, the 1449x815 view
plus a 91 px footer reading
`DLSS 5 Video Player 0.27.2 · 007 First Light`,
`00:00:18:42 · frame 1122 · 2 × 2 (fourth pane RTX VSR High (default)) · Mix 100% · Zoom Fit`,
`Settings sha256:ee4d77128a1605d2 · Runtime 310.8.SF-v2 · Saved 2026-10-02 21:51:09`.

**The RTX VSR crop.** A 960x540 copy of the cached 007 source, made with the
bundled ffmpeg:

```
ffmpeg -i source.mkv -map 0:v:0 -map 0:a:0
  -vf scale=960:540:flags=lanczos+accurate_rnd+full_chroma_int -fps_mode passthrough
  -c:v libx264 -preset slow -crf 12 -pix_fmt yuv420p
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -c:a copy
  "007 First Light 540p.mkv"
```

5,784 frames, SHA-256
`27407aef60bd59dd0368094a1cccabaeed4333cb2754d1dc178a0b65b5673e9a`, rendered
whole (render key `fad0ab4556db0e4d…`). Paused on frame 1122 in the 1902x1023
window: **Wipe** and **Compare > Compare against RTX VSR**, the divider at
60.8%. The picture is 1449x815, **1.51x** the video, RTX VSR at High. A check
on that capture: the left side differs from a bilinear scale of the copy's
frame 1122 by 1.04 levels (1.51 and 1.49 for frames 1121 and 1123), the right
side by 1.48; the right side's mean Laplacian, a measure of fine detail, is
2.57 against the left's 1.82 beside the divider. `vsr-wipe-crop.png` is the
render window's area of that capture, x 226-1675 and y 55-790 of the visible
frame: 1449x735, unscaled and lossless, both tags in it. It is one frame of a
game trailer on one display; the measured comparison is the
[RTX VSR quality report](../measurements/vsr-quality-20261002/REPORT.md), not
this picture.

## 24 September 2026 images

No image in `current/` is from this round any more (`subtitles.jpg`, the last,
was retaken on 1 November). The demonstration video's player scenes and the
social square's saved 2 x 2 PNG ([docs/media/README.md](../media/README.md))
are, so the round's record stays here; the records of the replaced files are
in this file's history.

**Hardware and runtime.** RTX 4080 SUPER, driver 610.47 (`32.0.16.1047`),
runtime lock `310.8.SF-v2`. The player is this repository at `dced888`, plus
the fix `f2ae230` (cached playback decoded the render in the wrong pixel
layout and could show a striped picture), applied before any of these shots. Every shot was checked for that artifact; none has it.

**Display.** The player sat on a 1920x1080 display set to 150% scaling for
these captures (the machine's own setting is 100%; it was put back
afterwards). Each is the window's visible frame, 1902x1023, captured by handle
with `tools/verification/capture-window.ps1` and cut to DWM's extended frame
bounds, and saved once as JPEG at quality 95 with no chroma subsampling.

**Settings.** A fresh profile, so neural settings at their defaults. The render is a whole-video render
of *007 First Light - Story Trailer* made through the player's YouTube Auto path
(see [docs/media/README.md](../media/README.md#comparison-stills)); for these
shots the cached source was opened as a local file, which found the same render
in the cache. Frame 1122 (0:18.7), paused with **Go to timecode**. Everything
was driven by posted window messages; the loupe in the demonstration video and
the wipe divider needed the real pointer over the picture for a moment.

## Rights

These images document the software. They are not an image-quality benchmark,
an official NVIDIA integration, or an endorsement by anyone shown. Footage
belongs to its owners: Grand Theft Auto VI to Rockstar Games, 007 First Light
to IO Interactive and Mafia: The Old Country to 2K and Hangar 13. NVIDIA
components belong to NVIDIA. The source-code licence doesn't relicense any of
it.
