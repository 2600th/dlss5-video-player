# DLSS 5 Video Player — Experimental Neural Rendering for Windows

[![Windows build](https://github.com/2600th/dlss5-video-player/actions/workflows/build.yml/badge.svg)](https://github.com/2600th/dlss5-video-player/actions/workflows/build.yml)
[![Version](https://img.shields.io/badge/version-0.12.0-76b900)](CHANGELOG.md)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078d4)](docs/BUILDING.md)
[![C++](https://img.shields.io/badge/C%2B%2B-20-00599c)](CMakeLists.txt)
[![License](https://img.shields.io/badge/source%20license-MIT-blue)](LICENSE)

**DLSS 5 Video Player** is an experimental, open-source Windows project that
adapts ordinary video for **DLSS 5-style neural rendering** on NVIDIA RTX GPUs.
It reconstructs motion, depth, and uncertainty guides from local videos or
public YouTube streams, evaluates native NVIDIA NGX DLSS Super Resolution, and
exposes the real D3D12 feature calls to an optional RenoDX/ReShade DLSS 5 neural
rendering add-on.

> [!IMPORTANT]
> Version 0.12.0 is a community experiment, **not NVIDIA's official production
> DLSS 5 integration or SDK release**. Its optional neural runtime contains a
> modified `nvngx_dlssnr.dll` with Authenticode `HashMismatch`, while the
> ReShade/RenoDX components are unsigned. Read [What “DLSS 5” means
> here](#what-dlss-5-means-here) and [Security and runtime
> disclosure](#security-and-runtime-disclosure) before running or distributing
> a package.

## What “DLSS 5” means here

NVIDIA describes DLSS 5 as 3D-Guided Neural Rendering that consumes a frame's
color and motion vectors and generates temporally consistent lighting and
material detail. NVIDIA currently says the official feature debuts in NBA 2K27
on September 3, 2026 for GeForce RTX 50-series GPUs; see [NVIDIA's DLSS 5
launch article](https://www.nvidia.com/en-eu/geforce/news/dlss-5-3d-guided-neural-rendering/).

This player explores whether a similar input contract can be adapted to video.
Unlike a game engine, a movie does not provide geometry, authoritative motion
vectors, depth, material IDs, or artist masks. The player therefore estimates
compact temporal guides from consecutive decoded frames, expands them on the
GPU, and keeps the underlying NGX calls visible so the experimental add-on can
observe them.

That makes this a **research and testing tool**, not proof of NVIDIA-equivalent
DLSS 5 output. Guide reconstruction, a configured add-on, and successful native
DLSS SR evaluation do not by themselves prove that the neural runtime loaded,
evaluated successfully, or produced visually correct results.

The project is useful for:

- testing native-resolution DLAA and opt-in DLSS video upscaling on NVIDIA RTX
  hardware;
- watching local FFmpeg-supported media through a native D3D12 pipeline;
- comparing the source, motion, depth, mask, and reconstructed output views;
- experimenting with public, non-DRM YouTube playback and source-quality
  selection; and
- studying an open C++ implementation of video decoding, temporal analysis,
  DLSS integration, audio-clock synchronization, and GPU presentation.

## Highlights

- **Experimental DLSS 5 neural rendering path** through a RenoDX/ReShade add-on
  that can observe the player's real D3D12 NGX feature creation and evaluation.
- **Native-resolution DLAA carrier by default**, keeping spatial DLSS
  upscaling off while preserving the NGX evaluation contract the add-on
  observes. Super Resolution modes remain explicit opt-ins.
- **RenoDX 4.70 runtime contract** that explicitly sets neural uplift on and
  both RenoDX and NGX spatial upscaling off, while preserving user-owned style,
  intensity, mask, and guide overrides.
- **Local media playback** for MP4, MKV, MOV, WebM, AVI, and other formats
  supported by the supplied FFmpeg build.
- **Verified pre-render before playback**: every source frame must advance the
  native submission counter, produce a captured output, and remain inside a
  feature-18 session with no failure or pass-through marker before the encoded
  video is independently probed and atomically cached.
- **Public YouTube playback** with Auto (exact 1080p preferred, otherwise the
  highest available resolution up to 4K) plus manual 2160p, 1440p, and 1080p
  choices. Sub-1080p fallbacks remain automatic and are not exposed as modes.
- **Temporal guide reconstruction** for motion vectors, depth, uncertainty,
  and scene-cut resets when engine data is unavailable.
- **Hardware-aware decoding** with lower-bandwidth policies for high-resolution
  material and a Media Foundation fallback for local files.
- **Post-DLSS image controls** for brightness, contrast, saturation, gamma,
  temperature, and tint.
- **Playback essentials** including drag and drop, timeline seeking, audio,
  aspect modes, fullscreen, frame-drop recovery, and overlay-safe shortcuts.
- **Debug views** for inspecting the DLSS inputs and reconstructed output.
- **Neural-mode policy for RTX 40/50 systems**, plus a one-click native NGX
  safe mode. Official DLSS 5 support is RTX 50-only at launch; this project's
  RTX 40 path relies on a community-modified runtime and is not an NVIDIA
  support claim.
- **Automated policy tests** covering playback timing, UI behavior, GPU
  lifetimes, network-media transactions, helper-process safety, and runtime
  configuration.

## Requirements

### To run a packaged build

- 64-bit Windows with D3D12 support;
- an NVIDIA RTX GPU and a current NVIDIA graphics driver for DLSS;
- the **entire extracted release folder**—do not launch the executable from
  inside the ZIP or mix DLLs from different packages; and
- internet access only when using public YouTube playback.

The publishable `core-win64` release contains the player plus the official,
NVIDIA-signed DLSS runtime from the pinned SDK. It intentionally excludes the
modified neural runtime, RenoDX/ReShade, Streamline, FFmpeg, and YouTube
helpers. The public core therefore supports local Media Foundation playback
and native NGX/DLAA, but not the experimental neural interception path or
YouTube. Those optional capabilities require a separately assembled,
user-authorized experimental layout whose redistribution rights are not
established by this project.

The application policy recognizes RTX 40 and RTX 50 systems. Official DLSS 5
launch support is limited to RTX 50-series GPUs; RTX 40 compatibility here
depends on a community-modified binary with an invalidated signature. This
repository does not treat either policy selection or a loaded add-on as proof
of correct neural output.

### To build from source

- Visual Studio 2022 with the Desktop development with C++ workload;
- CMake 3.24 or newer;
- Windows SDK and a 64-bit MSVC toolchain;
- Git; and
- the NVIDIA DLSS SDK pinned to commit
  `a291cc7d2cc642a51566f3dfd5376f635cd1b284`.

FFmpeg/FFprobe are optional at compile time but required for the primary local
and network media path at runtime. The public source build provides the native
NGX/DLSS SR backbone; enabling the experimental DLSS 5 path also requires the
separately supplied, private, hash-locked neural runtime inputs described in the
project documentation.

## Quick start

1. Download a trusted ZIP from the project's
   [Releases](https://github.com/2600th/dlss5-video-player/releases) page.
2. Extract the **whole archive** to a normal folder.
3. Run `DLSSVideoPlayer.exe`.
4. Drag in a locally supported video or use **File > Open** (`Ctrl+O`). An
   authorized experimental layout can also expose **File > Open YouTube URL**
   (`Ctrl+L`).
5. In an authorized neural layout, wait for source acquisition, neural
   rendering, encoding, and validation to finish. Playback begins only after a
   complete cache entry is reopened successfully.
6. Use **DLSS off · Original** / **DLSS on · Neural rendered** to compare the
   same timestamp. Spatial upscaling remains off during neural pre-rendering.
7. Press `Home` and check ReShade's
   **Add-ons** page to verify the observed `DLSS 5 Neural Rendering` state.

The neural default is 1920×1080 native/DLAA. On the tested RTX 5090, the
30.03-second Resident Evil Requiem example produced and independently decoded
all 1,800 neural frames at 1920×1080/59.94 fps. Its hardened manifest recorded
`nativeEvaluations=1800`, `verifiedNeuralFrames=1800`, the inline interception
contract armed before capture, a feature-18 receipt that advanced after the
captured sequence, and
`upscaling=false`. Isolated cached playback dropped 1 frame
(0.056%) and reused the unchanged artifact without re-rendering. RTX 40
hardware remains unmeasured on this system.

If the optional neural path is unstable, choose **Advanced > Restart in DLSS
SR safe mode**. Safe mode disables the RenoDX add-on for that launch while
retaining the official NGX path. DLAA remains the default unless the user
selects a spatial upscaling mode.

## Common controls

| Action | Shortcut |
| --- | --- |
| Open a local video | `Ctrl+O` |
| Open a YouTube URL | `Ctrl+L` |
| Play or pause | `Space` or `Ctrl+Alt+Space` |
| Step one cached frame while paused | `.` |
| Seek backward / forward 10 seconds | `Left` / `Right` |
| Mute | `M` |
| Toggle Original / Neural rendered | `D` |
| Recreate the NGX feature | `F6` |
| Open image adjustments | `Ctrl+E` or `Ctrl+Alt+C` |
| Enter or leave fullscreen | `F11` |

Source quality under **Video > YouTube source quality** controls the locally
materialized YouTube source. Neural rendering uses native 1:1 DLAA and keeps
both NGX and RenoDX upscaling off. In cached playback, the DLSS toggle switches
between synchronized original and neural-rendered frames rather than running a
second real-time upscale.

## Supported YouTube playback

The player accepts supported public YouTube video URLs and resolves them with
package-local, pinned yt-dlp and Deno helpers. Auto requests exact 1080p first;
when unavailable it selects the highest compatible source up to 4K, including
a lower-resolution source as a compatibility fallback. Manual choices are
exact 1080p, 1440p, and 2160p.

Private, login-required, paid, age-gated, cookie-dependent, and DRM-protected
videos are not supported. YouTube formats, regional availability, and upstream
behavior can change independently of this project.

Neural source and render files are private cache data under
`%LOCALAPPDATA%\DLSSVideoPlayer\NeuralCache\v1`. Use **Advanced > Clear Neural
Cache** when no neural job or cached playback is active to review the cache size
and remove it. Cancelled or failed jobs are never promoted as reusable entries.

## How it works

```text
FFmpeg source acquisition / decoding
          │
          ├── audio ──> waveOut ──> playback clock
          │
          └── BGRA video frame
                    │
             temporal analysis
                    │
          motion + depth + mask guides
                    │
             D3D12 GPU expansion
                    │
          NVIDIA NGX DLSS Super Resolution
                    │
       optional RenoDX DLSS 5 interception
                    │
          GPU readback + verified encoding
                    │
            atomic disk cache promotion
                    │
       synchronized Original / Neural playback
                    │
             image adjustments / display
```

The neural job is intentionally offline: a finite buffer cannot compensate
when average neural production is slower than consumption. Playback starts
only after frame count, dimensions, duration, final-frame decoding, runtime
evidence, and cache hashes pass. Audio then drives two synchronized local
decoders, and late display frames may be dropped without losing either stream's
timestamp alignment.

For a deeper technical explanation, see
[Architecture](docs/ARCHITECTURE.md).

## Build and test on Windows

Clone the repository and fetch the exact NVIDIA SDK revision used by CI:

```bat
git clone https://github.com/2600th/dlss5-video-player.git
cd dlss5-video-player
git init external\DLSS
git -C external\DLSS remote add origin https://github.com/NVIDIA/DLSS.git
git -C external\DLSS fetch --depth 1 origin a291cc7d2cc642a51566f3dfd5376f635cd1b284
git -C external\DLSS checkout --detach FETCH_HEAD
```

Fetch the pinned UI and YouTube helper assets, configure, build, and test:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_ui_assets.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_youtube_helpers.ps1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The developer executable is written to
`build\Release\DLSSVideoPlayer.exe`. It can test the native NGX/DLSS SR path,
but a source build alone does not contain the experimental DLSS 5 neural
runtime. A developer build is not a distributable release folder. Authorized
release maintainers must assemble and verify the explicit package allowlist
separately.

Create the publishable core ZIP with:

```bat
package_public_release.bat
```

`package_release.bat` is the private experimental assembler. It requires the
complete hash-locked runtime and helper set and must not be used to publish
those files without independent redistribution authorization.

For environment overrides, FFmpeg staging, and the one-click Windows build, see
[Building from source](docs/BUILDING.md).

## Troubleshooting

- **The player starts but a video does not open:** confirm that `ffmpeg.exe` and
  `ffprobe.exe` are beside the application and that the source is supported.
- **DLSS is unavailable:** update the NVIDIA driver, confirm the system has an
  RTX GPU, and inspect the player log for NGX initialization details.
- **The experimental neural path is unstable:** restart in **DLSS SR safe mode**.
- **YouTube playback fails:** try another public, non-DRM video and confirm that
  the package-local yt-dlp and Deno helpers are present.
- **The overlay or add-on state is unclear:** press `Home`, open ReShade's
  **Add-ons** page, and inspect the observed RenoDX status. A configured mode is
  not proof that a neural workload evaluated successfully.

See the full [Troubleshooting guide](docs/TROUBLESHOOTING.md) and
[experimental neural-mode setup](docs/DLSS5_SETUP.md).

## Security and runtime disclosure

The optional 0.12.0 DLSS 5 experiment uses a community-modified RTX 40-targeted
neural DLL whose Authenticode result is `HashMismatch`, plus unsigned ReShade
and RenoDX files. The embedded NVIDIA signature was invalidated by the
modification and must not be presented as valid proof of origin.
The neural DLL and add-on are not part of the pinned official NVIDIA DLSS SDK
checkout used to compile this repository. Those signature states are disclosed
facts and are not, by themselves, a malware determination or a clean scan.
Release inputs are locked by size and SHA-256, and the package verifier rejects
missing, changed, or unexpected files. Hash pinning provides reproducibility,
not trust.

Redistribution permission for the combined experimental runtime set remains
unresolved. **Do not publish a package until the applicable upstream terms have
been reviewed.** Do not replace individual runtime files with versions from
another package.

Read [SECURITY.md](SECURITY.md), [THIRD_PARTY.md](THIRD_PARTY.md), and the files
under [`THIRD_PARTY_LICENSES/`](THIRD_PARTY_LICENSES/) before distributing a
build. Report security-sensitive issues privately to the maintainer.

## Documentation

- [Building from source](docs/BUILDING.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Experimental neural-rendering setup](docs/DLSS5_SETUP.md)
- [Related implementations and performance decisions](docs/RELATED_PROJECTS.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Contributing](CONTRIBUTING.md)
- [Changelog](CHANGELOG.md)

## Contributing

Issues, reproducible bug reports, documentation improvements, and pull requests
are welcome. Before opening a pull request, build the Release configuration, run
the tests, and follow the hands-on playback checklist in
[CONTRIBUTING.md](CONTRIBUTING.md).

Do not commit or redistribute NVIDIA SDK files, FFmpeg binaries, ReShade
binaries, experimental DLSS runtime DLLs, or other third-party packages unless
their terms explicitly allow it.

## License and trademarks

The project source is available under the [MIT License](LICENSE). Packaged
third-party components retain their own licenses and terms.

This independent project is not affiliated with, sponsored by, or endorsed by
NVIDIA, ReShade, RenoDX, FFmpeg, yt-dlp, Deno, or YouTube. NVIDIA, GeForce, RTX,
and DLSS are trademarks or registered trademarks of NVIDIA Corporation. YouTube
is a trademark of Google LLC.
