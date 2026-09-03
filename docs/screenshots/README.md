# Screenshot provenance

## Current development captures

The `current/` gallery is captured on 2026-09-03 from the local
`codex/value-improvements` development build on Windows with an NVIDIA RTX 5090.
It is separate from the earlier release images below. The five JPEG window captures retain the actual
window, menus, video and status text; there is no retouching, synthetic UI or
image enhancement. Source screenshots are saved directly without recompression.

The gallery uses the configured [GTA VI - Trailer 2 example](https://www.youtube.com/watch?v=VQRLujxTm3c)
from Rockstar Games. Neural playback was validated at 1920 x 1080 / 30 fps.
Original and neural comparison images use the same paused timestamp at
approximately 01:05 on the complete 2:47 trailer, with
runtime upscaling and playback image adjustments off/default.
The neural/original JPEG pair was refreshed after the highest-bitrate download
change using the complete H.264 source (2.434 Mbps measured average video bitrate).

The separate `current/face-comparison.png` is a diagnostic figure extracted from
frame 1950 (zero-based, 01:05 at 30 fps) in the original and saved neural videos.
This figure retains the earlier AV1-source verification; it is not a bitrate comparison.
It uses identical 400 x 460 crops at x=560, y=100, with labels above them.
There is no scaling, retouching or color adjustment. The render log reports
intensity 1.00, global tone 1.00, preset/style 0, and neural upscaling off.

| File | State |
| --- | --- |
| `current/neural-playback.jpg` | Paused cached neural view; README lead image |
| `current/original-comparison.jpg` | Original view at the same timestamp |
| `current/upcoming-games.jpg` | Five official examples in the File menu |
| `current/recent-videos.jpg` | Persistent recent history and export command |
| `current/player-start.jpg` | Local file and YouTube entry points |

Images are documentation of feature states, not a benchmark or evidence of
NVIDIA-equivalent quality. A fixed five-game editorial selection is not a
popularity ranking. See [example provenance](../EXAMPLE_VIDEOS.md) and
[functional verification](../VERIFICATION-2026-09-02.md).

For future updates, capture the actual current build at one consistent window
size, use a clear paused frame, keep compared timestamps/settings identical,
and include complete relevant controls. Check for clipped menus, tooltips,
personal paths and desktop overlays before saving. Preserve older release images
when release documentation references them.

## Earlier v0.12.0.1 captures

Captured on 2026-09-02 from the locally built dlss5-video-player-v0.12.0.1 player, running
on Windows with an NVIDIA RTX 5090. Files are unedited JPEG window captures;
no mockups, AI-generated frames or external desktop overlays are included.

- `player-start.jpg`: idle player and source-opening actions.
- `cache-loading.jpg`: saved 1920x1080 video verification; no new render.
- `neural-playback.jpg`: paused cached neural view at approximately 00:06.
- `original-comparison.jpg`: original view at the same paused timestamp.
- `dlss-controls.jpg`: independent feature menu and upscaling output choices.

Playback and comparison captures use the built-in
[Resident Evil Requiem - Launch Trailer example](https://www.youtube.com/watch?v=9lrThxCoznw).
Runtime upscaling is off in all captures. The screenshots demonstrate interface
states, not an image-quality or cross-GPU performance benchmark.

Game footage and trademarks belong to their respective owners. The source-code
license does not relicense this third-party media or imply endorsement. Keep
the repository and experimental package private as requested.
