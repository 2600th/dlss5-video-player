# DLSS Video Player

A Windows x64 video player built around Direct3D 12, FFmpeg and NVIDIA NGX/DLSS. It can play normal video files while exposing a game-like temporal DLSS pipeline with reconstructed motion vectors, depth and temporal masks, making it useful for DLSS Super Resolution testing and for experimental RenoDX DLSS 5 Neural Rendering setups.

> **Status:** experimental. Native DLSS Super Resolution is integrated directly through NGX. DLSS 5 Neural Rendering currently depends on external experimental runtime files and a RenoDX add-on.

## Features

- Direct3D 12 renderer with a three-frame submission ring.
- NVIDIA NGX DLSS Super Resolution with `CreateFeature` + `EvaluateFeature_C`.
- FFmpeg primary decoder with Media Foundation fallback.
- MP4, MKV, MOV, WebM, AVI and most formats supported by FFmpeg.
- H.264, HEVC, AV1, VP9 and other FFmpeg-supported codecs.
- Audio playback with audio-clock synchronization.
- Realtime frame dropping when rendering falls behind instead of slowing the movie.
- Reconstructed temporal motion vectors, depth and uncertainty masks for video input.
- Original display aspect ratio by default, with optional fill/crop.
- Drag-and-drop, seek bar, volume, mute, fullscreen and common media controls.
- ReShade-safe global hotkeys for play/pause and comparison workflows.
- English-only user interface.
- Image adjustments applied after DLSS: brightness, contrast, saturation, gamma, temperature and tint.
- Debug views for DLSS input, motion vectors, depth and temporal mask.
- Paused-frame presentation heartbeat so ReShade remains responsive while the video is paused.

## Download / Releases

The executable published in **Releases** does **not** include the experimental DLSS 5 Neural Rendering files.

For DLSS 5 Neural Rendering, download the required DLSS 5 runtime files and `renodx-dlss5.addon64` from:

https://app.mediafire.com/folder/sa9zioqbixj7e

Place the required files in the **same folder as `DLSSVideoPlayer.exe`**. In particular, the setup expects the DLSS 5 Neural Rendering DLL (`nvngx_dlssnr.dll`) and `renodx-dlss5.addon64`. If the package provides a matching `nvngx_dlss.dll`, keep the matching pair together.

You also need **ReShade with add-on support** installed for `DLSSVideoPlayer.exe` using DirectX 10/11/12 mode.

At startup, the player updates only ReShade's `[ADDON] DisabledAddons` setting. It enables `renodx-dlss5.addon64` by default on detected RTX 40 and RTX 50 GPUs, and disables it on other or unknown GPUs. If that setting needs correction, the player relaunches itself once before renderer creation. A malformed or read-only `ReShade.ini` fails closed without replacing the file; check `DLSSVideoPlayer.log` for the specific error.

The status row reports the selected configuration as **Neural addon enabled (experimental)** or **DLSS SR safe mode**. It does not prove that the add-on completed neural evaluation; press **Home** and use ReShade's **Add-ons** page for the add-on's own runtime status. RTX 50 behavior is a best-effort target covered by policy tests, not local hardware verification; this project has exercised the supplied runtime on an RTX 4080 SUPER.

A typical DLSS 5 folder looks like this:

```text
DLSSVideoPlayer.exe
ffmpeg.exe
ffprobe.exe
nvngx_dlss.dll
nvngx_dlssnr.dll
renodx-dlss5.addon64
dxgi.dll                 <- ReShade, depending on installation choice
ReShade.ini
```

Additional `sl.*.dll` files may also be present when using a Streamline-based experimental package. Keep files from the same package/version together.

## Quick start

1. Launch `DLSSVideoPlayer.exe`.
2. Drop a video onto the idle window or click **Open**.
3. Leave **DLSS Auto** enabled for the realtime-oriented default.
4. Open ReShade with **Home** when testing the RenoDX DLSS 5 add-on.
5. Use **Ctrl+Alt+Space** to pause/resume even while ReShade is capturing normal keyboard/mouse input.
6. Use **Ctrl+Alt+C** to open Image Adjustments even while the ReShade overlay is open.

If the experimental neural path is unstable, choose **Advanced > Restart in DLSS SR safe mode** or launch with `--safe-mode`. Safe mode disables only `renodx-dlss5.addon64` for that launch and keeps native DLSS Super Resolution available. A later normal launch on an RTX 40/50 GPU restores the default add-on setting, with at most one bootstrap relaunch for either correction.

The player does not force a file picker at startup. It opens in an idle state and waits for drag-and-drop, **Open**, `Ctrl+O`, or a file path passed on the command line.

## Fixed YouTube examples

When the packaged YouTube helpers are available, **File > Examples** offers this fixed list. It does not fetch trending videos at runtime.

| Category | Video | Official channel | URL |
| --- | --- | --- | --- |
| Games | GTA VI Trailer 2 | Rockstar Games | https://www.youtube.com/watch?v=VQRLujxTm3c |
| Games | Resident Evil Requiem - Launch Trailer | Resident Evil | https://www.youtube.com/watch?v=9lrThxCoznw |
| Games | Battlefield 6 Season 3 Official Gameplay Trailer | Battlefield | https://www.youtube.com/watch?v=XCMr55EjFew |
| Anime | 2026 Summer Anime Season Trailer | Crunchyroll | https://www.youtube.com/watch?v=DWM2IfkzLHo |
| Anime | Spring 2026 Season Official Trailer | Crunchyroll | https://www.youtube.com/watch?v=7Wc6ugY3meg |
| Anime | My Hero Academia FINAL SEASON "More" Official Trailer | Crunchyroll | https://www.youtube.com/watch?v=pxbEWUjh6E4 |

Availability and region access can change. These entries play through the same public, non-DRM YouTube resolver flow as a pasted URL.

## Controls

| Action | Shortcut |
| --- | --- |
| Open video | `Ctrl+O` |
| Play / Pause | `Space` |
| ReShade-safe Play / Pause | `Ctrl+Alt+Space` |
| Back 10 seconds | `Left` / `Ctrl+Alt+Left` |
| Forward 10 seconds | `Right` / `Ctrl+Alt+Right` |
| Mute | `M` / `Ctrl+Alt+M` |
| Toggle DLSS | `D` / `Ctrl+Alt+D` |
| Image adjustments | `Ctrl+E` / `Ctrl+Alt+C` |
| Recreate NGX / re-hook | `F6` |
| Fullscreen | `F11` or double-click video |
| Final image | `1` |
| DLSS color input | `2` |
| Motion vectors | `3` |
| Depth | `4` |
| Temporal mask | `5` |
| Estimated / flat depth proxy | `G` |

The **Color** button and **Video > Image adjustments...** open live controls for:

- Brightness (`-2` to `+2` EV-like stops)
- Contrast
- Saturation
- Gamma
- Temperature
- Tint (green ↔ magenta)

Image adjustments are applied in the final presentation pass after DLSS. DLSS input/debug guide views remain unchanged, which makes before/after comparisons easier.

## ReShade comparison workflow

When ReShade owns the normal input path, clicking the player UI can be unreliable. The player therefore registers Windows-level global hotkeys. A useful comparison workflow is:

```text
Ctrl+Alt+Space     pause video
Home               open ReShade
change DLSS 5 / RenoDX settings
Ctrl+Alt+C         optionally adjust image controls
Ctrl+Alt+Space     resume video
```

While paused, the player continues to present the frozen frame at a lightweight cadence without decoding new frames or reevaluating DLSS. This keeps the ReShade overlay responsive for side-by-side tuning.

## DLSS temporal inputs

Normal encoded video does not contain the original game engine Z-buffer or object motion vectors. Those buffers are discarded when the game is rendered into a 2D movie, so the player reconstructs temporal guides from consecutive frames.

The current pipeline provides:

- **Color:** linear `R16G16B16A16_FLOAT`.
- **Output:** `R16G16B16A16_FLOAT` UAV.
- **Motion vectors:** `R16G16_FLOAT`, current frame → previous frame, in input-pixel units.
- **Depth:** one `R32_TYPELESS` resource viewed as `D32_FLOAT` for depth writes and `R32_FLOAT` for sampling/debugging. The same resource is passed to NGX.
- **Temporal mask:** `R8_UNORM` used for current-color bias / disocclusion-style hints.
- **Jitter:** Halton subpixel jitter shared by the generated guides and the NGX evaluation parameters.
- **Reset handling:** first frame, seeks, decoder discontinuities, scene cuts, dropped-frame recovery and NGX recreation.
- **Frame timing:** actual frame delta when available.

These are reconstructed video guides, not the original engine buffers. Their quality depends on the source material.

## Realtime behavior

The player is designed to preserve video speed rather than slow the whole movie when DLSS becomes expensive.

- Audio is the master playback clock when audio is available.
- D3D12 uses three frames in flight instead of flushing the GPU every frame.
- High-resolution sources can be decoded directly closer to the DLSS input resolution.
- If the renderer falls too far behind, stale frames are dropped and temporal history is reset.
- The status bar shows rendered FPS, source FPS and dropped frames.

For example:

```text
fps 60/60 | drop 0
```

means the player is keeping full source speed without dropping frames.

## Build from source

### Requirements

- Windows 10/11 x64
- Visual Studio 2022
- **Desktop development with C++** workload
- Windows SDK
- Git for Windows
- NVIDIA RTX GPU for native DLSS execution
- Internet connection on the first build

Run:

```bat
build_windows.bat
```

The build script automatically:

1. Locates Visual Studio and CMake.
2. Clones the official NVIDIA DLSS SDK into `external/DLSS`.
3. Reuses or downloads FFmpeg/FFprobe.
4. Configures a Visual Studio 2022 x64 build.
5. Builds Release.
6. Stages the runtime files beside the executable.

Output:

```text
build\Release\DLSSVideoPlayer.exe
build\Release\nvngx_dlss.dll
build\Release\ffmpeg.exe
build\Release\ffprobe.exe
```

The experimental DLSS 5 / RenoDX files are intentionally not part of this repository and are not required to compile the player.

For more detail, see [Building](docs/BUILDING.md), [DLSS 5 setup](docs/DLSS5_SETUP.md), [Architecture](docs/ARCHITECTURE.md), [Troubleshooting](docs/TROUBLESHOOTING.md), and [Localization](docs/LOCALIZATION.md).

## Command line

```text
DLSSVideoPlayer.exe [video-file] [--output WIDTHxHEIGHT] [--quality MODE]
```

Quality modes:

```text
auto
quality
balanced
performance
ultra-performance
dlaa
```

Example:

```bat
DLSSVideoPlayer.exe "D:\Videos\sample.mkv" --output 3840x2160 --quality auto
```

## Project layout

```text
src/                    player, decoder, D3D12/NGX and temporal-guide code
docs/                    user/developer documentation
.github/                 GitHub Actions and contribution templates
build_windows.bat        one-click local Windows build
prepare_dlss5_test.bat   runtime-file diagnostics for experimental DLSS 5
inspect_dlssnr.ps1       DLSSNR version/hash/signature inspection
```

## License

The project source code is licensed under the **MIT License**. See [LICENSE](LICENSE).

Third-party components such as NVIDIA DLSS/NGX, FFmpeg, ReShade and RenoDX are separate projects and remain subject to their own licenses and terms. Experimental DLSS 5 runtime files are not distributed by this repository.

DLSS and NVIDIA are trademarks of NVIDIA Corporation. This project is not affiliated with or endorsed by NVIDIA, ReShade, RenoDX or FFmpeg.
