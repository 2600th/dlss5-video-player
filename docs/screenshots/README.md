# Screenshot provenance

## v0.13.0 feature captures

The `current/` gallery was captured on 2026-09-03 from the feature implementation
released in v0.13.0, before the release version bump, on Windows with an NVIDIA RTX 5090.
The five JPEG window captures retain the actual
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
| `current/face-comparison.png` | Same-frame face diagnostic from the earlier AV1-source run |

Images are documentation of feature states, not a benchmark or evidence of
NVIDIA-equivalent quality. A fixed five-game editorial selection is not a
popularity ranking. See [example provenance](../EXAMPLE_VIDEOS.md) and
[functional verification](../VERIFICATION-2026-09-02.md).

For future updates, capture the actual current build at one consistent window
size, use a clear paused frame, keep compared timestamps/settings identical,
and include complete relevant controls. Check for clipped menus, tooltips,
personal paths and desktop overlays before saving. Remove superseded captures
when no maintained document uses them; historical versions remain in Git.

Game footage and trademarks belong to their respective owners. The source-code
license does not relicense this third-party media or imply endorsement.
