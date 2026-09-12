# Player demonstration

[Watch the 30-second MP4](neural-comparison-demo.mp4) ·
[Full-size poster](neural-comparison-poster.jpg) ·
[Remotion source](../../tools/demo-video/README.md)

[![Player demonstration poster](neural-comparison-poster.jpg)](neural-comparison-demo.mp4)

The video is **1920 × 1080, 30 fps, exactly 900 frames / 30 seconds**, encoded as
H.264 with `yuv420p` pixels. It is intentionally silent: the application was
muted and no music, narration or substitute audio was added.

| Time | Content |
| --- | --- |
| 00:00–00:03 | Title and an actual player screenshot |
| 00:03–00:13 | Paused frame of The Godfather at 01:14; the real `Ctrl+Alt+D` toggle switches Neural Rendering from Off to On |
| 00:13–00:28 | 15 seconds of uninterrupted GTA VI playback with Neural Rendering On throughout |
| 00:28–00:30 | Closing card and an actual player screenshot |

Both middle chapters are screen recordings of the shipping v0.21.0 player on an
RTX 5090, recorded on 12 September 2026 at 30 fps with FFmpeg's `gdigrab` over
the window's visible frame (1442 × 932). The recordings keep that captured size:
only the surrounding titles, explanatory text, border and progress line are added
in Remotion. There are straight cuts between takes, with no speed changes and no
fabricated intermediate states.

Unlike the September 3 edit, this one shows **live** neural rendering: each take
is a session rendering ahead of playback, which is what the status line reports
("Neural rendering from here · … buffered · Neural rendered"). Nothing was
pre-baked for the camera; the clips were rendered once by the player first, so
the toggle attaches from retained segments instead of waiting for a fresh render.
Playback upscaling was off, playback adjustments were neutral, and both sources
keep their native 2560 × 1440 resolution.

| Source | Video | Used for |
| --- | --- | --- |
| [Grand Theft Auto VI: An Extended Look — Now Playing](https://www.youtube.com/watch?v=uphThaa97ig) (Netflix) | 2560 × 1440 VP9, 30 fps, 26 s | Title card, the 15-second playback take |
| [THE GODFATHER 50th Anniversary Trailer](https://www.youtube.com/watch?v=UaVTIH8mujA) (Paramount Pictures) | 2560 × 1440 VP9, 23.976 fps, 120 s | The paused toggle, the closing card |

Rockstar's own 26-minute *An Extended Look* upload is age-restricted, so an
anonymous session cannot fetch it at all; Netflix's *Now Playing* cut of the same
footage is not. [Screenshot and render provenance](../screenshots/README.md)
records the exact frames, digests and receipts.

Every included shot was inspected for nudity, sexual activity and sexualized
imagery, and the selected scenes show fully clothed characters. The video
demonstrates this unofficial RenoDX/ReShade experiment, not an official NVIDIA
DLSS 5 integration. Rights to NVIDIA components belong to NVIDIA. Grand Theft
Auto VI footage is © Rockstar Games; The Godfather footage is © Paramount
Pictures. Game and film footage, third-party components and trademarks retain
their owners' rights, and the source-code licence does not relicense them.

Only the compact delivery MP4, the poster and the reusable edit source are
maintained in Git. Untrimmed recordings, contact sheets, extracted source frames
and dependencies stay in ignored working directories. The composition can recover
its two app recordings from the delivered MP4; the local originals avoid an extra
lossy encoding generation.
