# DLSS 5 Video Player

Run a video, photo or GIF through NVIDIA's DLSS 5 neural renderer, then look at
the result next to the original on the same frame. Windows only. Needs an RTX
card.

[Download](#download) · [First run](#first-run) · [Usage guide](docs/USAGE.md) · [Build it yourself](docs/BUILDING.md) · [Troubleshooting](docs/TROUBLESHOOTING.md)

[![Watch the 30-second DLSS 5 Video Player demonstration](docs/media/neural-comparison-poster.jpg)](docs/media/neural-comparison-demo.mp4)

**[30-second demo](docs/media/neural-comparison-demo.mp4)** from The Witcher IV. A paused
frame with neural rendering off, then on, then playback with it left on. 1080p
H.264, no sound. [How it was captured](docs/media/README.md).

> [!IMPORTANT]
> This is a community project, not an NVIDIA product. The neural runtime is a
> modified, unsigned community build. v0.17.1 was checked on an RTX 5090 and
> v0.17.0 on an RTX 4080 SUPER; RTX 20 and 30 are enabled but nobody has run
> them yet. Details and notices: [THIRD_PARTY.md](THIRD_PARTY.md).

## Download

**v0.17.1** (2026-09-10): [release page](https://github.com/2600th/dlss5-video-player/releases/tag/dlss5-video-player-v0.17.1)

| Package | What is in it | Size |
| --- | --- | --- |
| `dlss5-video-player-v0.17.1-win64.zip` | Player plus the pinned neural runtime. This is the one you want. | 308 MB |
| `DLSSVideoPlayer-v0.17.1-core-win64.zip` | Player only, no neural runtime. | 31 MB |

SHA-256 for both is on the release page. GitHub's "Source code" zip does not
run; it has no runtime in it.

## First run

1. Unzip into a **new, empty folder**. Keep `neural-runtime/` next to the exe.
2. Run `DLSSVideoPlayer.exe`.
3. Open a file (`Ctrl+O`), paste a public YouTube link (`Ctrl+L`), or pick
   something from **File > Game trailers**.
4. Press `D`. It buffers for a few seconds, then plays rendered.
5. **Video > Compare** shows before and after as a split, a wipe or a blend.
6. **DLSS > Convert & save** writes the rendered video to a file.

Next time, **File > Recent videos** reopens it with the render already done.

## What it does

- Renders while you watch. Press `D` at any point and playback continues on the
  rendered frames a few seconds later. Turn it off and on again and it picks up
  where it stopped instead of starting over.
- Keeps the original and the render in step. Switching views does not move the
  playhead, and you can pause and step frames on either.
- Remembers the last five videos and their renders. A render is reused only if
  the source, the runtime and the neural settings all still match.
- Exports what you rendered. PNG or JPEG for photos, GIF for animations, MP4 or
  MKV for video. MKV keeps the source audio, subtitles and chapters without
  re-encoding them.
- Optional DLSS Super Resolution on top, 1440p or 2160p, for either view. The
  render itself stays at source resolution.
- Neural settings live at `Ctrl+N`. Change one while paused and that frame is
  re-rendered so you can judge on the picture. Settings are saved with each
  render and are part of its cache identity.
- Six official game trailers under **File > Game trailers**, each under three
  minutes, for a quick first test.

## What changed

The short version. Every detail is in [CHANGELOG.md](CHANGELOG.md).

**0.17.1** (2026-09-10). Live playback no longer stops at a segment seam with an
"out of sync" warning, and a stop of any kind now says why in the log.
Re-measured on an RTX 5090: a 1080p30 session costs 8.4 ms per frame, down from
11.9 before the export loop was pipelined.

**0.17.0** (2026-09-10). The export got 2.3x faster and the pixels did not change.
1080p30 on an RTX 4080 SUPER: 48.6 to 110.7 frames per second, and the output
is bit for bit the same as 0.16.0. Guide generation went from 4.6 ms to 1.5 ms
per frame. Most of this came from ctype-lab's PR #5; the parts that broke
things were fixed or left out.

**0.16.0** (2026-09-09). Runs on every RTX generation, 20 through 50, by
switching to the universal 310.8.SF-v2 runtime. Fixed a wedge on the RTX 4080
that killed every 1080p live session two seconds after preroll. The keep-up
forecast now uses your GPU's own measured speed at each source size and warns
before a session it expects to fall behind. `build_windows.bat` fetches the
runtime by itself.

**0.15.0** (2026-09-09). Press `D` while watching instead of rendering the whole
file first. Toggling off and on resumes rather than re-rendering. 4K30 keeps up
on an RTX 5090 (it ran at 0.23x real time before). Two neural controls that the
model provably ignores were removed from the dialog.

**0.14.0** (2026-09-03). Photos and GIFs, with PNG, JPEG, GIF, MP4 and MKV
export. Six game trailers replaced the old example list. Fullscreen hides the
controls until the mouse moves.

**0.13.0** (2026-09-03). Recent-five history with cache reuse. Stream-copy MKV
export with audio, subtitles and chapters. Runtime DLSS upscaling, off by
default. The neural renderer moved into its own helper process.

**0.12.0** (2026-09-01). Public YouTube playback. Separate Neural Rendering,
DLSS Upscaling and Frame Generation controls. The RenoDX neural path, with
pinned runtime hashes and a safe-mode escape hatch.

## Controls

| What | Key |
| --- | --- |
| Open a file / a YouTube URL | `Ctrl+O` / `Ctrl+L` |
| Play or pause | `Space` |
| Neural rendering on or off | `D` |
| Compare views | **Video > Compare**; `[` and `]` move the wipe, `Z` zooms |
| Seek ten seconds / step one frame | `Left` / `Right`; `.` |
| Mark In / Out; clear; go to time | `I` / `O`; `Shift+I`; `Ctrl+G` |
| Render one frame / four seconds / the marked clip | `F` / `Shift+F` / `Ctrl+R` |
| Neural settings | `Ctrl+N` |
| Image adjustments | `Ctrl+E` |
| Volume, mute | Mouse wheel; `M` |
| Fit or fill; fullscreen | `A`; `F11` |
| Stop | `S` |
| Debug views (final, DLSS input, motion, depth) | `1` `2` `3` `4` |
| Re-hook the runtime | `F6` |

Everything you set is kept between launches: volume, view, upscaling, YouTube
quality, image adjustments, neural settings and guide switches.

Defaults on a fresh install: neural rendering on, DLSS upscaling off (1440p
selected when you turn it on), YouTube quality Auto (exact 1080p if it exists,
otherwise the best available up to 4K). Frame Generation has no backend and
stays unavailable.

More on the cache, settings and export in [USAGE.md](docs/USAGE.md). The
trailer list is in [EXAMPLE_VIDEOS.md](docs/EXAMPLE_VIDEOS.md).

## Screenshots

Real captures on an RTX 5090, v0.13.0 interface. They show the UI, not image
quality; click through for full size.

![File menu with Recent videos and the cached-video export command](docs/screenshots/current/recent-videos.jpg)

<details>
<summary>Same frame, original and neural</summary>

![Cached neural view of Ciri in The Witcher IV daylight village sequence](docs/screenshots/current/neural-playback.jpg)

![Original video at the same paused timestamp as the neural view](docs/screenshots/current/original-comparison.jpg)

Upscaling is off in both. This shows the synchronized toggle, not a claim that
every source gains detail.

[Unscaled face crops from the same frame](docs/screenshots/current/face-comparison.png).

</details>

<details>
<summary>Faces from three more trailers</summary>

Same source pixels on both sides, no scaling or retouching, default settings.
The right half of each is a real render from the shipping worker.

![Hellblade II close-up, original beside the neural render](docs/screenshots/current/face-hellblade.png)

![Cyberpunk 2077 Phantom Liberty close-up, original beside the neural render](docs/screenshots/current/face-cyberpunk.png)

![Mafia The Old Country close-up, original beside the neural render](docs/screenshots/current/face-mafia.png)

The change is subtle and depends on the source. Skin shading and fine texture
move; silhouettes and framing do not. Judge your own footage on its own preview.

</details>

<details>
<summary>Start screen</summary>

![Start screen with local file and YouTube URL actions](docs/screenshots/current/player-start.jpg)

</details>

[Capture details and footage attribution](docs/screenshots/README.md).

## Limits

- Speed. A live 1080p30 session costs about 8.4 ms per frame on an RTX 5090
  (driver 616.64, median of eight 30 s sessions; the same clip cost 11.9 ms
  before the export loop was pipelined in 0.17.0), and 15.4 ms at 1440p30. An
  RTX 4080 SUPER measured 15.3 ms at 1080p30 on 0.16.0 and has not been
  re-measured since. 4K30 depends on the file: a native 40 s 4K30 source ran at
  1.165x real time, while a 6.3 Mbit/s 4K re-encode costs 42 ms per frame, or
  0.78x, and drops nearly every present. The player measures your GPU after the
  first session and warns before one it expects to fall behind.
- RTX 20 and 30 run the universal runtime without native FP8. Expect them to be
  several times slower. Nobody has verified them in this project yet.
- Motion and depth guides are estimated from the video. Artifacts happen.
- Export copies the cached 8-bit render. Image adjustments and upscaling are not
  baked in, and HDR or lost source precision is not restored.
- Subtitles stay as separate tracks. No in-player subtitle display, no burn-in,
  no queue, no HDR, no resume of an interrupted render across restarts.
- YouTube: public, non-DRM videos only, no login. Availability can change.
- History is five videos, not a size quota. Big videos take space.
  **Advanced > Clear Neural Cache** frees it.

## Building and contributing

C++20, Win32, Direct3D 12, FFmpeg, NVIDIA NGX. The neural runtime runs in a
separate helper process; playback upscaling runs in the player.

- [Build and test](docs/BUILDING.md)
- [Architecture](docs/ARCHITECTURE.md), [technical overview](TECHNICAL_OVERVIEW.md)
- [Runtime setup](docs/DLSS5_SETUP.md)
- Hardware records: [2026-09-02](docs/VERIFICATION-2026-09-02.md), [RTX 4080 SUPER](docs/VERIFICATION-2026-09-09-RTX4080.md), [RTX 5090](docs/VERIFICATION-2026-09-09-RTX5090.md), [RTX 5090 on 0.17.0](docs/VERIFICATION-2026-09-10-RTX5090.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md), [issues](https://github.com/2600th/dlss5-video-player/issues)

Bug report: build or commit, GPU, driver, source size and frame rate, steps,
log excerpts. Strip private paths and signed media URLs from logs first.

Pull request: run `ctest` green on a clean checkout of `main` before opening
it. Renderer changes also need the GPU smoke test. Keep the source-resolution
and neural-validation contracts intact.

## Credits and license

Started from [DLSS 5 Video Player by Jessica Natalia Mods](https://gitlab.com/JessicaNataliaMods/dlss-5-video-player/).

Project source is [MIT](LICENSE). Third-party binaries, game footage and
trademarks keep their own terms; see [THIRD_PARTY.md](THIRD_PARTY.md).
