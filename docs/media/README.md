# Player demonstration

[Watch the 21-second MP4](neural-comparison-demo.mp4) ·
[Looping preview](neural-comparison-preview.webp) ·
[Full-size poster](neural-comparison-poster.jpg) ·
[Remotion source](../../tools/demo-video/README.md)

[![Player demonstration poster](neural-comparison-poster.jpg)](neural-comparison-demo.mp4)

The video is **1920 × 1080, 30 fps, 678 frames / 22.6 seconds**, encoded as H.264
with `yuv420p` pixels. It is intentionally silent: the application was muted and
no music, narration or substitute audio was added. The README embeds
`neural-comparison-preview.webp`, the same cut at 880 px and 11 fps, because
GitHub strips `<video>` elements from Markdown - it is a looping copy of this
file, not separate footage.

| Time | Content |
| --- | --- |
| 00:00–00:03 | Title over 3.4 s of The Godfather playing with the render attached |
| 00:03–00:09 | Paused frame of The Godfather at 01:14: the player's own `Z` magnifies it 2x, then **Video ▸ Compare ▸ Wipe** splits it, and the divider is dragged onto the face |
| 00:09–00:15 | The same three controls on GTA VI paused at 00:13 |
| 00:15–00:19 | 4.8 s of uninterrupted GTA VI playback with Neural Rendering on throughout |
| 00:19–00:23 | Download card |

Every shot is a screen recording of the shipping player on an RTX 5090, recorded
on 12 September 2026 at 30 fps with FFmpeg's `gdigrab` over the window's visible
frame (1442 × 932). The recordings keep that captured size: only the surrounding
titles, the ORIGINAL / NEURAL RENDERED labels, the border and the progress line
are added in Remotion. There are straight cuts between takes, with no speed
changes and no fabricated intermediate states.

Both comparison shots are **live** sessions rendering ahead of playback, which is
what the status line reports ("Neural rendering from here · … buffered · Neural
rendered"). The clips were rendered once by the player first, so the toggle
attaches from the published entry instead of waiting for a fresh render. The
white divider, the 2x magnification and the drag are the player's own controls,
so both halves of each face are the same source pixels at the same instant.
Playback upscaling was off, playback adjustments were neutral, and both sources
keep their native 2560 × 1440 resolution.

The labels are placed on measured instants, not guesses: the capture driver
reports the wall-clock offset of each keypress from the first recorded byte
(`zoom_at`, `wipe_at`), and the edit reads those numbers.

| Source | Video | Used for |
| --- | --- | --- |
| [Grand Theft Auto VI: An Extended Look — Now Playing](https://www.youtube.com/watch?v=uphThaa97ig) (Netflix) | 2560 × 1440 VP9, 30 fps, 26 s | Comparison 2, the playback take |
| [THE GODFATHER 50th Anniversary Trailer](https://www.youtube.com/watch?v=UaVTIH8mujA) (Paramount Pictures) | 2560 × 1440 VP9, 23.976 fps, 120 s | Title shot, comparison 1 |

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

Only the compact delivery MP4, the looping preview, the poster and the reusable
edit source are maintained in Git. Untrimmed recordings, contact sheets,
extracted source frames and dependencies stay in ignored working directories.
