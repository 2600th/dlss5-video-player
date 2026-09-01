# DLSS Video Player 0.12.0

Extract the whole ZIP, then run **`DLSSVideoPlayer.exe`**. Open a local video
with **Open** or paste a public YouTube URL with **File > Open YouTube URL**.
If the experimental neural path misbehaves, use **Advanced > Restart in DLSS SR
safe mode**.

This Windows x64 player uses D3D12, FFmpeg, NVIDIA NGX/DLSS Super Resolution,
and an experimental RenoDX neural-rendering add-on. The packaged default enables
the add-on on detected RTX 40 and RTX 50 cards. It was tested locally on an RTX 4080.
RTX 40 and RTX 50 are intended policy targets; RTX 50 was not hardware-tested here.

The included `nvngx_dlssnr.dll` is modified and its Authenticode status is
`HashMismatch`. ReShade/RenoDX files are unsigned. Read
`EXPERIMENTAL_RUNTIME_NOTICE.txt` and `THIRD_PARTY.md` before sharing the ZIP.

## Playback

- Local MP4, MKV, MOV, WebM, AVI, and other FFmpeg-supported media.
- Public, non-DRM YouTube videos that do not require login, payment, or cookies.
- Drag/drop, play/pause, ±10-second seek, timeline seek, volume/mute, aspect,
  fullscreen, debug views, and post-DLSS image adjustments.
- `D` toggles native DLSS; `F6` recreates the NGX feature.

YouTube availability, regional access, and upstream formats can change. Private,
login-required, paid, age-gated, and DRM-protected videos are not supported.

## Neural mode and safe mode

Normal mode leaves `renodx-dlss5.addon64` enabled by default on RTX 40/50 policy
targets. Safe mode disables that add-on for the launch while retaining native
DLSS Super Resolution. A configured mode does not prove a successful neural
evaluation; use ReShade's Add-ons panel for observed add-on status.

## Examples

**File > Examples** contains six fixed public links:

- GTA VI Trailer 2 — Rockstar Games
- Resident Evil Requiem - Launch Trailer — Resident Evil
- Battlefield 6 Season 3 Official Gameplay Trailer — Battlefield
- 2026 Summer Anime Season Trailer — Crunchyroll
- Spring 2026 Season Official Trailer — Crunchyroll
- My Hero Academia FINAL SEASON "More" Official Trailer — Crunchyroll

These are examples, not a live trending feed, and may become unavailable.

## Common controls

| Action | Shortcut |
| --- | --- |
| Open local video | `Ctrl+O` |
| Open YouTube URL | `Ctrl+L` |
| Play / pause | `Space` or `Ctrl+Alt+Space` |
| Seek ±10 seconds | `Left` / `Right` |
| Mute | `M` |
| Toggle DLSS | `D` |
| Image adjustments | `Ctrl+E` or `Ctrl+Alt+C` |
| Fullscreen | `F11` |

## Source build

Developers should follow [docs/BUILDING.md](docs/BUILDING.md). Building and
release assembly are separate operations. End users launch only
`DLSSVideoPlayer.exe` from the extracted ZIP.

The project source is MIT-licensed. Packaged third-party components have their
own terms; see `THIRD_PARTY.md` and `THIRD_PARTY_LICENSES/`. This project is not
affiliated with or endorsed by NVIDIA, ReShade, RenoDX, FFmpeg, yt-dlp, or Deno.
