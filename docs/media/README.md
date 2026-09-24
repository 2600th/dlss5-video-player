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

## Comparison stills

Five figures in [`stills/`](stills/), each the source frame beside the same
frame of the player's render, as identical unscaled 700x880 crops. Each has a
`.provenance.json` beside it with the source URL and SHA-256, the frame and its
timestamp, the crop, the render's cache key, SHA-256 and verified frame count,
the neural settings and their digest, the runtime, the GPU and driver, and the
player commit.

| Figure | Trailer, frame | What it shows |
| --- | --- | --- |
| [`007-first-light-bond.png`](stills/007-first-light-bond.png) | 007 First Light, Story Trailer, 1122 (0:18.7) | Skin: pores, the scar, the lips, under lamp light |
| [`resident-evil-requiem-flashlight.png`](stills/resident-evil-requiem-flashlight.png) | Resident Evil Requiem, 2nd Trailer, 4125 (1:08.8) | Night lighting: a face lit from below by a torch |
| [`gta6-lucia.png`](stills/gta6-lucia.png) | GTA VI Trailer 2, 1940 (1:04.7) | Hair and skin at golden hour |
| [`007-first-light-suit.png`](stills/007-first-light-suit.png) | 007 First Light, Story Trailer, 2928 (0:48.8) | Grey hair, a lined face and a wool suit |
| [`ac-shadows-low-key-limit.png`](stills/ac-shadows-low-key-limit.png) | Assassin's Creed Shadows, Story Trailer, 1987 (1:06.2) | **Where it does not help.** A face in deep shadow under a helmet: the render crushes it towards black and adds speckle, while the lit armour barely changes |

The first four are the strongest frames found in those trailers, not typical
ones. The last is there so they are not the only evidence: low-key shots like
it are common in these trailers, and the model makes them worse. The render also
has flaws in the good figures: in the GTA VI one the sweater's left edge breaks
into blotches, and in the Resident Evil one the background above the hair picks
up a smudge.

**How they were made.** Each trailer was opened from **File > Game trailers**
in a fresh profile at the player's YouTube **Auto** quality (2560x1440, the
highest bitrate at that height), and rendered whole with **DLSS > Convert &
export > Convert whole video to neural video** at default settings: Intensity,
Local tone, Local structure and Color strength 1.0, Skin structure Off, one
pass. Every render verified all of its
frames. The source and render were then copied out of the cache, and each figure
was made with
[`tools/demo-video/make-face-comparison.py`](../../tools/demo-video/make-face-comparison.py),
which checks by picture that the render frame is the source frame (it must
differ least at the manifest's offset, 0 for a whole-video render, than one
frame either side), for example:

```powershell
python tools/demo-video/make-face-comparison.py <007>/source.mkv <007 render folder> `
    --frame 1122 --crop 1170,185,700,880 --output docs/media/stills/007-first-light-bond.png `
    --provenance docs/media/stills/007-first-light-bond.provenance.json `
    --source-url https://www.youtube.com/watch?v=trvIyyFt_MM --title "007 First Light - Story Trailer (PlayStation)" `
    --gpu "NVIDIA GeForce RTX 4080 SUPER" --cache-key <render folder name> --player-commit dced888...
```

The other four differ only in trailer, frame and crop, which their records
give. On the two slow 007 shots that check has little margin (for example
4.62 against 4.70 levels at frame 1122), because the source barely changes
from frame to frame; a first choice, frame 1104, failed it and was dropped. The
offset of 0 was confirmed by picture on every trailer, decisively on shots
with motion.

Renders: RTX 4080 SUPER, driver 610.47 (`32.0.16.1047`), runtime lock
`310.8.SF-v2`, the player at `dced888` (0.25.0 plus the unreleased work since).

## Rights

This is an unofficial RenoDX/ReShade experiment, not an NVIDIA product. *The
Matrix* footage is © Warner Bros., *Grand Theft Auto VI* © Rockstar Games,
*007 First Light* © IO Interactive, *Resident Evil Requiem* © Capcom and
*Assassin's Creed Shadows* © Ubisoft. They're used to document the software, and the source-code
licence doesn't relicense them. Only the finished video, preview, poster and
edit source are in Git. The sources and renders are not.

The previous cut (12 September, *The Godfather* and GTA VI on the v0.21 player)
is kept unchanged at
[`tools/benchmark/fixtures/demo-capture-20260912.mp4`](../../tools/benchmark/fixtures/README.md),
because the benchmark corpus is cut from it.
