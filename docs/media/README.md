# Player demonstration

[Watch the MP4](neural-comparison-demo.mp4) ·
[Looping preview](neural-comparison-preview.webp) ·
[Poster](neural-comparison-poster.jpg) ·
[How to rebuild it](../../tools/demo-video/README.md)

[![Player demonstration poster](neural-comparison-poster.jpg)](neural-comparison-demo.mp4)

A 19.7-second, silent video: 1920x1080, 30 fps, 591 frames, H.264. GitHub
strips `<video>` tags from Markdown, so the README shows
`neural-comparison-preview.webp` instead. It's the same cut at 880 px and 10 fps.

## What's in it

| Time | Scene |
| --- | --- |
| 0:00–0:04.5 | *The Matrix*, one paused frame of Trinity. The divider sweeps across her face, revealing the render, while the shot slowly pushes in |
| 0:04.5–0:06.9 | The one-line pitch, over that frame's render |
| 0:06.9–0:09.7 | *GTA VI* Trailer 2: Lucia walking toward the camera, playing at full speed with the divider down her face |
| 0:09.7–0:13.3 | The same shot paused and magnified about 2x |
| 0:13.3–0:16.3 | GTA VI in hard daylight, a man mid-sentence |
| 0:16.3–0:19.7 | Download card, including the settings used |

## How it was made

Nothing in the video is a screen recording or a mock-up. Every picture is one
of two things:

- **Left of the divider:** the source file, decoded.
- **Right of the divider:** the same frame from the render the v0.25.0 player
  saved for that file. Each trailer was opened in the player, neural rendering
  was switched on, and the render was left to finish.

Both halves use the same frame and the same crop, so the divider is the only
difference. Zooms apply to both halves equally. Nothing was retouched,
sharpened or colour-corrected, and no clip was slowed, looped or frozen.

Getting the frames to line up is simple: a render starts at its range's first
frame, so source frame *n* is render frame *n* − round(start × fps). That's 60
frames for The Matrix and 54 for GTA VI. `prepare-inputs.py` also checks this
by picture and stops if the pair isn't closest at that offset.

| Source | Video | Render |
| --- | --- | --- |
| [The Matrix \| 4K Trailer](https://www.youtube.com/watch?v=nUEQNVV3Gfs), Warner Bros. | 2560x1440 VP9, 23.976 fps, 147 s | 3,472 of 3,472 frames verified |
| [Grand Theft Auto VI Trailer 2](https://www.youtube.com/watch?v=VQRLujxTm3c), Rockstar Games | 2560x1440 VP9, 30 fps, 167 s | 4,948 of 4,948 frames verified |

Both renders ran on an RTX 4080 SUPER (driver 610.47, RenoDX 6.5.3,
DLSS-NR 310.8.0), with the model taking about 11 ms of GPU time per frame. Intensity, Local tone and Local
structure were set to 2.0 instead of the default 1.0, because that's how this
machine is set up. At the defaults the change is subtler. Full digests are in
[screenshot provenance](../screenshots/README.md).

## Choosing the frames

Both trailers were scanned second by second, then frame by frame around each
candidate, for open eyes, a gaze into the lens, focus and motion blur.

| Scene | Source frames | Why this one |
| --- | --- | --- |
| Trinity | Matrix 2116 | Eyes on the lens, even light, a wall of gun racks behind her |
| Lucia, walking | GTA VI 1896–1979 | Golden hour, walking toward camera, chain-link mesh behind |
| Lucia, 2x | GTA VI 1940 | The moment in that walk she looks straight at the camera |
| Daylight | GTA VI 420–509 | Harsh midday light on a face mid-sentence |

Some candidates were left out. Morpheus's close-up at 1:12 is the hardest shot
in either trailer, but the render darkens the shadow side of his face, so it
doesn't make a good opener. Shirtless, club and bedroom shots in GTA VI
(1:11–1:18, 1:34–1:41, 2:03–2:04) fall outside this repository's content
standard. Everyone on screen is fully clothed.

## Rights

This is an unofficial RenoDX/ReShade experiment, not an NVIDIA product. *The
Matrix* footage is © Warner Bros. and *Grand Theft Auto VI* footage is ©
Rockstar Games. They're used to document the software, and the source-code
licence doesn't relicense them. Only the finished video, preview, poster and
edit source are in Git. The sources and renders are not.

The previous cut (12 September, *The Godfather* and GTA VI on the v0.21 player)
is kept unchanged at
[`tools/benchmark/fixtures/demo-capture-20260912.mp4`](../../tools/benchmark/fixtures/README.md),
because the benchmark corpus is cut from it.
