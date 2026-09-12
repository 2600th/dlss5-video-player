# Screenshot provenance

## September 3, 2026 refresh

The four media-bearing JPEGs in `current/` are authentic Windows captures of
DLSS 5 Video Player v0.13.0 on an NVIDIA RTX 5090. Each captures the complete
1442 × 932 player window, including the title, menus, controls and status.
FFmpeg captured the visible desktop region established by computer-use window
inspection. The files use one high-quality JPEG encoding; no UI replacement,
face retouching, sharpening or color adjustment was applied. The captured
computer-use pointer indicator is retained.

The footage is [The Witcher IV — Cinematic Reveal Trailer](https://www.youtube.com/watch?v=54dabgZJ5YA),
published by The Witcher / CD PROJEKT RED. The daylight village close-up of Ciri
is at **01:52.611** (the player's counter rounds this to 01:53). Contact sheets and consecutive frames were inspected for
gaze, expression, open eyes, blur and occlusion. The selected scene contains
fully clothed characters and no sexual content. The original/neural pair was
captured while paused, switching only Neural Rendering. DLSS Upscaling was off,
fit mode was active, audio was muted, and playback image adjustments were neutral.

The complete official H.264 video-only format 137 was downloaded with the
bundled yt-dlp, then opened as a local file through the player's normal render
and cache-validation path. The source is **1920 × 1080, 30000/1001 fps,
10,868 frames**. No source rescaling, retiming or image adjustment was applied.
The completed render manifest reports 10,868 native evaluations and 10,868
verified neural frames, with neural upscaling disabled.

| Provenance field | Value |
| --- | --- |
| Source SHA-256 | `ef4926c0da4451ff879b7b0e5e32d3f3b35fb06a542b179b0689735094fbefdf` |
| Render cache key | `f7ae512a647acee09aba1a38da5e269ce1cf6ae13c239ef95fa853ea9491f609` |
| Neural SHA-256 | `7a21fc81cc95c2aded299c960f19531ae8974c810347069b8c1a43fdfdea502a` |
| Neural settings | `EnableHooks=2`, `NREnableUpscaling=0`, `NeuralUplift=1` |

`current/face-comparison.png` uses zero-based frame **3375 (112.6125 seconds)**
from this source and its synchronized neural cache. Both 700 × 880 source-pixel
crops use x=670, y=0, with labels outside the image. There is no scaling,
retouching or color correction. [Reproduction script](../../tools/demo-video/make-face-comparison.py).

| File | State |
| --- | --- |
| `current/neural-playback.jpg` | Was this session's paused cached neural view; **replaced on 12 September 2026** by the GTA VI capture recorded below, so the Witcher IV pair is no longer in the tree |
| `current/original-comparison.jpg` | Was the original at the identical paused moment; **replaced on 12 September 2026** the same way |
| `current/upcoming-games.jpg` | Five official examples in the expanded File submenu |
| `current/recent-videos.jpg` | File menu with Recent videos and Export cached video; history submenu closed to keep local paths out of the capture |
| `current/player-start.jpg` | Retained September 3 start-screen capture from the v0.13.0 feature implementation before its version bump |
| `current/face-comparison.png` | Unscaled matched crops from the source and render |

The [30-second demonstration](../media/README.md) uses the same Witcher IV
source and actual application recordings. The toggle compares original and
prepared cached neural video; it does not execute live neural rendering.

## September 9, 2026: faces from three more trailers

`current/face-hellblade.png`, `current/face-cyberpunk.png` and
`current/face-mafia.png` were produced the same way as the Witcher IV figure and
under the same rules: both halves are the identical source pixels of the identical
frame, with no scaling, retouching or tonal adjustment, and the labels sit outside
the image. The right half of each is a real render from the shipping worker
(`NeuralWorkerTests --real-worker`, `mv=1,depth=1`, neural settings at their
defaults), not a mock-up or a re-encode of the original.

Each source is the official 1080p video-only format fetched with the bundled
yt-dlp. Two seconds around the chosen moment were rendered, so the captured frame
carries a full temporal history rather than being the first frame of a job. The
crop is centred on the face and constrained to the picture area, which is why the
Hellblade crop is 804 px tall: that trailer is letterboxed to 1920x804 and a taller
crop would have included the black bars.

| File | Trailer | Source | Frame | Crop | Source SHA-256 |
| --- | --- | --- | --- | --- | --- |
| `current/face-hellblade.png` | Hellblade II - Launch Trailer (XBOX) | https://www.youtube.com/watch?v=PRbOmIcVXak | 1665 (55.50 s) | 700x804 at x=821, y=138 | `63e3f42d89294071c254472a4ddf6213…` |
| `current/face-cyberpunk.png` | Cyberpunk 2077: Phantom Liberty - Launch Trailer (Cyberpunk 2077) | https://www.youtube.com/watch?v=kfX9n_G0N2Y | 2516 (83.87 s) | 700x880 at x=523, y=0 | `916b29df704c4065d2c818279c2605d6…` |
| `current/face-mafia.png` | Mafia: The Old Country - Family Takes Sacrifice (Mafia Game) | https://www.youtube.com/watch?v=EAEYZDgHNv8 | 1506 (50.20 s) | 700x880 at x=1104, y=200 | `86525500732b579e79e46f000a4aaae1…` |

Frames were chosen by inspecting contact sheets for a large, lit, unoccluded face
with open eyes; the selected scenes contain fully clothed characters and no sexual
content. Death Stranding 2's accolades trailer was checked and dropped - it has no
usable face close-up. All three are reproducible with
[the same script](../../tools/demo-video/make-face-comparison.py), which now takes
`--frame`, `--neural-frame`, `--crop` and `--caption`.

Footage is credited to Ninja Theory/Xbox Game Studios, CD PROJEKT RED and Hangar
13/2K respectively. Including it here documents a feature; it is not an
endorsement, and the source-code license does not relicense it.

## September 12, 2026: GTA VI and The Godfather, live 1440p sessions

Everything in this section was captured from the shipping **v0.21.0** player on
an RTX 5090 (driver `616.64`, DXGI `32.0.16.1664`, ReShade 6.8.0.2155, RenoDX
4.7, DLSS-NR 310.8.0). Two sources, both fetched with the bundled yt-dlp at the
rung this version's YouTube **Auto** now selects - the tallest up to 1440p:

| Source | Video | Stream | Frames | SHA-256 |
| --- | --- | --- | --- | --- |
| [Grand Theft Auto VI: An Extended Look - Now Playing](https://www.youtube.com/watch?v=uphThaa97ig) (Netflix, age limit 0) | 2560x1440 VP9, 30 fps, 26.0 s, 5.26 Mbps | `bv*[height<=1440]` | 780 | `2b43b5ce5865b396db03a008bdbaeb078f1799d3f89a37bbb8e2657a916a854d` |
| [THE GODFATHER 50th Anniversary Trailer](https://www.youtube.com/watch?v=UaVTIH8mujA) (Paramount Pictures, age limit 0) | 2560x1440 VP9, 23.976 fps, 120.119 s, 3.92 Mbps | `bv*[height<=1440]` | 2880 | `036afadb30d470ae575270c6f10de307cde43c0196df8ae4a350bb2c626d59c8` |

Rockstar's own 26-minute *An Extended Look* (`tJbzMqJGH4k`) is age-restricted:
every anonymous client is refused outright, and so is the GameSpot mirror, so no
unauthenticated session can fetch it at any resolution. Netflix's *Now Playing*
cut is the same footage without the gate. That is the defect this version added a
notice for, met in its strongest form.

### The two face figures

`current/face-gta6.png` and `current/face-godfather.png` were produced exactly
like the September 9 set: both halves are the identical source pixels of the
identical frame, no scaling, retouching or tonal adjustment, labels outside the
image, and the right half is a real render from the shipping worker
(`NeuralWorkerTests --real-worker`, `mv=1,depth=1`, neural settings at their
defaults). Candidate frames were proposed by OpenCV's frontal-face cascade over
every third frame and then chosen by eye for a large, lit, unoccluded face with
open eyes; the selected scenes show fully clothed characters and no sexual
content.

Each render covers a window that **starts before the cut into the shot**, so the
captured frame carries the temporal history a real session would have at that
moment rather than being the first frame of a job. The neural index is the frame's
position inside that render; it was confirmed by anchoring on the cut, which
appears at the same place in both sequences.

| Figure | Frame | Crop | Render window | Neural index | Worker result |
| --- | --- | --- | --- | --- | --- |
| `current/face-gta6.png` | 410 (13.67 s) | 700x880 at x=950, y=60 | 12.60-13.80 s, first frame 379 | 31 | 36/36 frames verified, neural GPU p50 5.15 ms/frame |
| `current/face-godfather.png` | 1781 (74.28 s) | 700x880 at x=1177, y=183 | 72.30-74.40 s, first frame 1734, cut at 1757 | 47 | 51/51 frames verified, neural GPU p50 5.11 ms/frame |

Reproduce with [the same script](../../tools/demo-video/make-face-comparison.py):

```
python tools/demo-video/make-face-comparison.py gta6-netflix.mp4 gta6-neural.mkv \
    --frame 410 --neural-frame 31 --crop 950,60,700,880 --output docs/screenshots/current/face-gta6.png
python tools/demo-video/make-face-comparison.py godfather-source.mp4 godfather-neural.mkv \
    --frame 1781 --neural-frame 47 --crop 1177,183,700,880 --output docs/screenshots/current/face-godfather.png
```

### The player captures

Each pair is one paused frame with only the view switched: the clip was rendered
once by a live session, the toggle was turned off so the frames stayed retained,
the timeline was pressed to land on the frame, and `Ctrl+Alt+D` then switched
between the retained neural frame and the original. No seek happens between the
two captures.

The captured rectangle is the window's **visible** frame from
`DWMWA_EXTENDED_FRAME_BOUNDS` - 1442 x 932 - not `GetWindowRect`, which includes
an invisible resize border and drags a strip of the desktop into the shot. Each
file is one JPEG encoding at quality 95 with no chroma subsampling; no UI
replacement, face retouching, sharpening or colour adjustment was applied.

| File | State |
| --- | --- |
| `current/neural-playback.jpg` | GTA VI paused at 13.17 s with the neural view attached; also the demo poster |
| `current/original-comparison.jpg` | The same paused frame with Neural Rendering off |
| `current/godfather-neural.jpg` | The Godfather paused at 73.98 s with the neural view attached |
| `current/godfather-original.jpg` | The same paused frame with Neural Rendering off |
| `current/neural-strength.jpg` | The image adjustments window over that frame, showing this version's new **Neural strength** dial at its 1.00 default |

The sessions behind them, from the player's own log: 702/702 and 2875/2875 frames
verified, `failure=none`, `lock=ok`, both published their cache entry. The
Godfather session is also the one that proved this version's publish fix - it
published *after 20 rename attempts* while a file handle was deliberately held on
the finished entry.

### The demonstration takes

The five takes behind [the 22-second video](../media/README.md) were recorded the
same day from the same two sources, on the build that carries this version's
attach fix. Each comparison take is one paused frame inspected with the player's
own controls - `Z` for 2x magnification centred on the pointer, then
**Video ▸ Compare ▸ Wipe** for the divider, then a drag to put that divider down
the middle of the face - so both halves are the same source pixels at the same
instant. The playback takes are continuous: playback was confirmed to be
advancing by measuring the picture, not by trusting the toolbar.

| Take | Source | Frame or window | What it shows |
| --- | --- | --- | --- |
| Title shot | The Godfather | from 72.06 s | 3.4 s of playback with the render attached |
| Comparison 1 | The Godfather | paused at 74.02 s | zoom at 1.71 s, divider at 3.48 s into the take |
| Comparison 2 | GTA VI | paused at 13.17 s | zoom at 1.57 s, divider at 3.33 s into the take |
| Playback | GTA VI | from 11.30 s | 5 s continuous, neural on throughout |

The zoom and divider instants are the capture driver's own report of when it
pressed each control, measured from the first recorded byte, so the labels in the
edit sit on the frames where the picture actually changed.

`current/upcoming-games.jpg`, `current/recent-videos.jpg` and
`current/player-start.jpg` are unchanged September 3 captures of menus and the
start screen: they carry no video footage, and the commands they show are the
same. The September 3 and September 9 figures above are left exactly as they were
recorded.

Footage is credited to Rockstar Games (via Netflix's *Now Playing* upload) and
Paramount Pictures. Including it here documents a feature; it is not an
endorsement, and the source-code licence does not relicense it.

These images document feature states, not an image-quality benchmark or an
official NVIDIA integration. A fixed example selection is not a popularity
ranking. See [example provenance](../EXAMPLE_VIDEOS.md),
[functional verification](../VERIFICATION-2026-09-02.md) and the
[12 September RTX 5090 record](../VERIFICATION-2026-09-12-RTX5090.md).

Rights to NVIDIA components belong to NVIDIA. Other third-party components, game
and film footage and trademarks belong to their respective owners: The Witcher IV
to CD PROJEKT RED, Hellblade II to Ninja Theory/Xbox Game Studios, Cyberpunk 2077
to CD PROJEKT RED, Mafia: The Old Country to Hangar 13/2K, Grand Theft Auto VI to
Rockstar Games and The Godfather to Paramount Pictures. The source-code licence
does not relicense this media or imply endorsement.
