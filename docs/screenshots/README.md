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
| `current/neural-playback.jpg` | Paused cached neural view; also used in the demo poster |
| `current/original-comparison.jpg` | Original at the identical paused moment |
| `current/upcoming-games.jpg` | Five official examples in the expanded File submenu |
| `current/recent-videos.jpg` | File menu with Recent videos and Export cached video; history submenu closed to keep local paths out of the capture |
| `current/player-start.jpg` | Retained September 3 start-screen capture from the v0.13.0 feature implementation before its version bump |
| `current/face-comparison.png` | Unscaled matched crops from the source and render |

The [30-second demonstration](../media/README.md) uses the same Witcher IV
source and actual application recordings. The toggle compares original and
prepared cached neural video; it does not execute live neural rendering.

These images document feature states, not an image-quality benchmark or an
official NVIDIA integration. A fixed five-game selection is not a popularity
ranking. See [example provenance](../EXAMPLE_VIDEOS.md) and
[functional verification](../VERIFICATION-2026-09-02.md).

Rights to NVIDIA components belong to NVIDIA. Other third-party components,
game footage and trademarks belong to their respective owners. The Witcher IV
trailer footage is credited to CD PROJEKT RED. The source-code license does not
relicense this media or imply endorsement.
