# Benchmark fixtures

`demo-capture-20260912.mp4` is the old README demonstration from 12 September
2026. It's a 22.6-second screen recording (1920x1080, 30 fps, 678 frames) of the
v0.21 player running live neural sessions on an RTX 5090, over *The Godfather
50th Anniversary Trailer* and a Netflix cut of *Grand Theft Auto VI*.

It lives here because [`corpus.py`](../corpus.py) cuts its four `real-*`
benchmark clips from it at fixed frame numbers. When the README got a new video
on 22 September, this one moved here unchanged. It's the same git blob
(`dc9527b7`, SHA-256 `ac46f10a…031411`), so older reports that mention
`docs/media/neural-comparison-demo.mp4` mean this file.

Rebuilt from it after the move, all four clips matched the digests in the
[14 September report](../../../docs/measurements/gate-real-footage-20260914/REPORT.md):

| Clip | Digest |
| --- | --- |
| `real-film-cuts` | `8cfd7d5666bcd057` |
| `real-game-cuts` | `59626fdfd98e0c1d` |
| `real-game-motion` | `bd5c372bbf162c8b` |
| `real-dissolve` | `2f9c40a41a9ea53b` |

The footage belongs to Paramount Pictures and Rockstar Games. It's kept only so
measurements can be reproduced.
