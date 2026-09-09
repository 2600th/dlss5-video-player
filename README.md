# DLSS 5 Video Player

Process a photo, GIF or video, replay its cached neural result, and compare it with the
original at the same timestamp. A native Windows player for local media and
public YouTube videos, with optional DLSS Super Resolution during playback.

[Get started](#get-started) · [Usage guide](docs/USAGE.md) · [Build from source](docs/BUILDING.md) · [Troubleshooting](docs/TROUBLESHOOTING.md)

[![Watch the 30-second DLSS 5 Video Player demonstration](docs/media/neural-comparison-poster.jpg)](docs/media/neural-comparison-demo.mp4)

**[Watch the 30-second demo](docs/media/neural-comparison-demo.mp4)** — actual player
footage from The Witcher IV. Compare the same paused frame with Neural Rendering
Off and On, then watch uninterrupted playback with it left On. 1080p H.264 MP4;
intentionally silent.
[Capture details and edit source](docs/media/README.md).

**v0.16.0 experimental release.** [Download the Windows build](https://github.com/2600th/dlss5-video-player/releases/tag/dlss5-video-player-v0.16.0)
with neural rendering on every RTX generation, a keep-up forecast measured per
GPU and source size, and a one-command runtime fetch. See the [changelog](CHANGELOG.md).

> [!IMPORTANT]
> This is an experimental community project, not an official NVIDIA DLSS 5
> integration. The optional neural runtime uses modified/unsigned third-party
> components. Hardware verification for v0.16.0 used an RTX 4080 SUPER and an
> RTX 5090 on the universal runtime; Turing and Ampere are enabled but unverified.
> See [runtime details and notices](THIRD_PARTY.md).

## Why use it?

- **Compare the same moment.** Switch between original and cached neural video
  without changing the playback timestamp.
- **Replay recent videos.** The last five distinct videos persist across
  launches. Acquired YouTube sources and neural renders are reused after validation.
- **Keep experiments consistent.** Neural settings are saved with each render
  and included in its cache identity. With playback paused, a settings change
  re-renders that frame so the choice is made on the picture itself.
- **Take the result with you.** Export processed photos as PNG/JPEG, animations
  as GIF, or videos as MP4/MKV. MKV preserves available source audio, compatible
  subtitles and chapters without re-encoding.
- **Keep playback clear.** Fullscreen hides the menu and controls until the
  mouse moves. Writable portable installs keep their cache beside the EXE.
- **Choose playback upscaling separately.** Apply optional DLSS Super Resolution
  to either view at 1440p or 2160p. The neural cache retains source resolution.
- **Start with a game trailer.** Six official trailers featuring human characters,
  each under three minutes, are available under **File > Game trailers**.

## Get started

1. Use Windows x64 with an NVIDIA RTX GPU (GeForce RTX 20 through 50, or an
   RTX-branded workstation/laptop part) and a suitable NVIDIA driver. The
   experimental neural layout requires the separately supplied runtime
   described in [setup](docs/DLSS5_SETUP.md).
2. [Build the current source](docs/BUILDING.md), or download the
   [v0.16.0 package](https://github.com/2600th/dlss5-video-player/releases/tag/dlss5-video-player-v0.16.0)
   if you have repository access. GitHub's source ZIP is not a runnable package.
3. Extract a packaged build into a **new folder** and keep all helpers and
   `neural-runtime/` intact. Launch `DLSSVideoPlayer.exe`.
4. Open a local file (`Ctrl+O`), paste a public YouTube URL (`Ctrl+L`), or choose
   **File > Game trailers**.
5. Press `D` to turn neural rendering on from the playhead: the picture waits a
   few seconds for its buffer, then plays rendered. Use **Video > Compare** for
   blend, split or wipe; enable **DLSS Upscaling** separately if desired.
6. Reopen through **File > Recent videos**, or use **DLSS > Convert & save** to
   convert a clip or the whole video and write it to a file.

The publishable core package has fewer capabilities than the complete experimental
layout. Build inputs and package contents are explained in [Building](docs/BUILDING.md).

## Everyday controls

| Action | Control |
| --- | --- |
| Open a local file / YouTube URL | `Ctrl+O` / `Ctrl+L` |
| Play or pause | `Space` |
| Compare original and neural views | **Video > Compare** for blend, split, wipe (`[` / `]`, drag) and `Z` zoom |
| Seek / step a paused cached frame | Timeline, or `Left` / `Right` for ten seconds; `.` to step one frame |
| Mark In / Out, exact timecode | `I` / `O`, `Shift+I` to clear; `Ctrl+G` |
| Turn neural rendering on while watching | `D` or the Neural Rendering button; it renders from the playhead and buffers |
| Convert part or all of a video to a file | `F` one frame, `Shift+F` four seconds, `Ctrl+R` the marked clip, **DLSS > Convert & save** |
| Neural model and guide settings | `Ctrl+N` |
| Volume / mute | Volume control or mouse wheel; `M` to mute |
| Fit or fill / fullscreen | `A` / `F11` |
| Image adjustments | `Ctrl+E` |
| Stop playback | `S` |
| Debug views (final, DLSS input, motion vectors, depth) | `1` / `2` / `3` / `4`, or **Video** |
| Re-hook the runtime | `F6` |
| Replay / save | **File > Recent videos** / **DLSS > Convert & save** |

Volume, mute, fit/fill, comparison view and mode, upscaling preference/output,
YouTube quality, image adjustments, neural settings and guide switches are saved
across launches.

| Setting | Fresh-install default |
| --- | --- |
| Neural Rendering | On; prepare or reuse a validated cache |
| DLSS Upscaling | Off; 1440p output selected, with 2160p available |
| YouTube source quality | Auto: prefer exact 1080p, otherwise highest available up to 4K |
| Frame Generation | Unavailable; no backend is implemented |

Manual YouTube choices are 1080p, 1440p and 2160p. Each selects the highest
advertised video bitrate at that resolution, across available codecs and containers.
Source quality and playback upscaling are separate.
See [cache, settings and export details](docs/USAGE.md).

See the [trailer list and runtimes](docs/EXAMPLE_VIDEOS.md) for the released and
upcoming AAA games in **File > Game trailers**.

## Screenshots

Actual Windows captures of the v0.13.0 feature implementation on an RTX 5090.
Images show interface states, not image-quality benchmarks. Open an image to
inspect it at full size.

### Recent videos and export

![File menu with Recent videos and the cached-video export command](docs/screenshots/current/recent-videos.jpg)

<details>
<summary>Matched original and neural face views</summary>

![Cached neural view of Ciri in The Witcher IV daylight village sequence](docs/screenshots/current/neural-playback.jpg)

![Original video at the same paused timestamp as the neural view](docs/screenshots/current/original-comparison.jpg)

Runtime upscaling is off in both comparison captures. These document a synchronized
toggle, not a claim that every source gains visible detail.

[Inspect the unscaled same-frame face crops](docs/screenshots/current/face-comparison.png).

</details>

### Faces from three more trailers

Same source pixels either side, no scaling or retouching, neural settings at
their defaults. The right half of each figure is a real render from the shipping
worker, not a mock-up.

![Hellblade II close-up, original beside the neural render](docs/screenshots/current/face-hellblade.png)

![Cyberpunk 2077 Phantom Liberty close-up, original beside the neural render](docs/screenshots/current/face-cyberpunk.png)

![Mafia The Old Country close-up, original beside the neural render](docs/screenshots/current/face-mafia.png)

The differences are subtle and content-dependent: skin shading and fine texture
move, silhouettes and framing do not. Judge a source on its own preview rather
than on these.

<details>
<summary>Start screen</summary>

![Start screen with local file and YouTube URL actions](docs/screenshots/current/player-start.jpg)

</details>

[Capture details and footage attribution](docs/screenshots/README.md).

## Limits to know

- Neural rendering runs either as a cached render you play beside the original,
  or behind live playback from the playhead. Measured 1080p30 cost: 11.9
  ms/frame on an RTX 5090, 15.3 on an RTX 4080 SUPER. Whether 4K30 keeps up
  depends on the source: the 5090 measured 1.165x real time on one 4K30 file
  and 0.78x on a 6.3 Mbit/s re-encode. The player measures its own GPU at each
  source size after the first session, warns with the predicted rate before a
  session it expects to fall behind, and buffers when a running one does.
- Motion and depth guides are estimated from video. Artifacts are possible.
  Turing (RTX 20) and Ampere (RTX 30) run the universal runtime without native
  FP8 and are several times slower; they have not been hardware-verified in
  this project.
- Export copies the cached 8-bit video. Playback adjustments and runtime upscaling
  are not baked in; export does not restore HDR or lost source precision.
- Compatible source subtitles remain separate in export. In-player subtitle
  display, burn-in, queues, HDR processing and durable render resume are not included.
- YouTube supports public, non-DRM videos without login. Availability and regional
  access can change. Local playback remains available.
- Recent history is limited to five videos, not a disk-size quota. Large videos
  can consume substantial space; use **Advanced > Clear Neural Cache** when needed.

## Development and help

The stack is C++20, Win32, Direct3D 12, FFmpeg and NVIDIA NGX. An isolated helper
hosts the experimental neural runtime; playback upscaling runs in the player.

- [Build and test](docs/BUILDING.md)
- [Architecture](docs/ARCHITECTURE.md) and [technical overview](TECHNICAL_OVERVIEW.md)
- [Runtime setup](docs/DLSS5_SETUP.md)
- [Verification results](docs/VERIFICATION-2026-09-02.md), the [RTX 4080 SUPER record](docs/VERIFICATION-2026-09-09-RTX4080.md) and the [RTX 5090 record](docs/VERIFICATION-2026-09-09-RTX5090.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md) and [report an issue](https://github.com/2600th/dlss5-video-player/issues)
- [Example video sources](docs/EXAMPLE_VIDEOS.md)

For a bug report, include the build/revision, GPU, driver, source dimensions and
frame rate, reproduction steps and relevant log excerpts. Remove private paths
and signed media URLs before sharing logs. Changes should preserve the source
resolution and neural-validation contracts; run CTest and relevant hardware
smoke checks before proposing a renderer change.

## Acknowledgments

Built upon [DLSS 5 Video Player by Jessica Natalia Mods](https://gitlab.com/JessicaNataliaMods/dlss-5-video-player/).
Credit to the original project and its contributors for the foundation this
project builds on.

Project source is [MIT-licensed](LICENSE). Third-party binaries, game footage and
trademarks retain their own terms; see [third-party notices](THIRD_PARTY.md).
