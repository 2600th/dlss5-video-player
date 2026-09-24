# DLSS 5 Video Player — technical overview

_Verified against main (9d3e6cc) on 2026-09-25._

The player renders a whole video while you watch it, keeps every rendered
region seekable, reuses the validated result, and compares it with the original
at the same timestamp. Start with the
[usage guide](docs/USAGE.md) for controls, cache locations, preferences and export.

This is an unofficial RenoDX/ReShade experiment. The source calls NVIDIA NGX
Super Resolution; a separate runtime add-on observes those calls to perform
experimental neural rendering. It is not an official NVIDIA DLSS 5 SDK
integration. Video-derived motion and depth are estimates, and successful NGX
evaluation alone does not prove correct neural output.

## Run the player

Extract the entire Windows package into a new writable folder, keep
`neural-runtime/` intact, and launch `DLSSVideoPlayer.exe`. Use Windows x64,
a D3D12-capable NVIDIA RTX GPU and a suitable NVIDIA driver. Hardware verification
used an RTX 5090 and an RTX 4080 SUPER; other neural configurations require
their own validation.

Open a local photo, GIF or video, paste a public YouTube URL, or choose **File >
Game trailers**. Rendering can also run behind playback: a session collects a
four-second lead and then plays the rendered frames while the render continues
ahead of the playhead. Press `D` to turn a session on, or to compare original
and neural views once a render is cached. Recent history reuses valid downloads
and renders. **DLSS > Convert & export > Save converted video** writes PNG/JPEG
photos, animated GIFs, or MP4/MKV videos. MKV preserves compatible source
streams. The cache prefers
`cache/v1` beside the EXE and falls back to LocalAppData when unwritable.
Fullscreen hides controls until the mouse moves; see [usage](docs/USAGE.md).

Fresh-install defaults are Neural Rendering on, playback DLSS Upscaling off,
upscaling output on Auto (the largest rung the monitor can show), and YouTube Auto taking the tallest rung up to
1440p at the highest bitrate, or up to 2160p when a source offers nothing lower.
Player preferences persist after that. Manual YouTube choices are 1080p,
1440p and 2160p; each selects the highest advertised video bitrate at that
resolution across codecs. Source selection and playback upscaling are separate.

## Two runtime layouts

| Layout | Capabilities |
| --- | --- |
| Complete experimental package | Neural worker and locked runtime, FFmpeg/FFprobe, YouTube helpers, synchronized comparison and cached export |
| Public core package | Player and official SDK DLSS runtime; local Media Foundation playback and native NGX, without the experimental neural runtime or media helpers |

The experimental proxy belongs in `neural-runtime/`, beside `NeuralWorker.exe`.
The main player must not have a root-level `dxgi.dll`. Do not mix components
from different packages. See [runtime setup](docs/DLSS5_SETUP.md).

## Processing and validation

1. **Acquire the source.** YouTube downloads use stable video/format IDs for
   cache identity. Video duration is checked against metadata so a short audio
   stream or interrupted download cannot publish an incomplete source.
2. **Render in the helper.** Frames are decoded in order. Compact motion and
   depth guides feed a native-resolution DLAA carrier; motion is accepted only
   where the match beats standing still by a margin. The
   experimental add-on observes NGX calls in a persistent feature-18 session.
3. **Validate and cache.** Every source frame must have a captured native
   evaluation. Runtime receipts, dimensions, timing, final-frame decode and
   content hashes must pass before atomic publication. Neural settings
   snapshots and their hashes are part of the render identity.
4. **Play and compare.** Audio drives timestamp-matched original/neural frame
   pairs. Optional runtime Super Resolution runs independently in the player.
   Image adjustments are applied during presentation.
5. **Export.** The validated neural video is stream-copied with available source
   audio, compatible subtitles, attachments, metadata and chapters. Export never
   overwrites an existing destination. **Export with DLSS stages** is the other
   route: it writes one file with any combination of Super Resolution, neural
   rendering and frame generation, in NVIDIA's own order, without touching the
   cache.

Neural cache output preserves source resolution. Playback upscaling and image
adjustments do not change the cache or export. The Standard cache is 8-bit
(High and Lossless are 10-bit), and HDR sources are tone mapped to SDR before
the model. A render queue and resuming a render after a restart are not
implemented.
See [architecture and remaining work](docs/ARCHITECTURE.md).

## Build, diagnose and verify

[Building and testing](docs/BUILDING.md) is the canonical source-build guide.
It pins the NVIDIA SDK, Tabler assets, YouTube helpers and FFmpeg/FFprobe, and
uses `build-upscaling` for configure, build, CTest and packaging. Source builds
do not supply the separately obtained experimental neural runtime.

Player diagnostics are in `DLSSVideoPlayer.log` beside the executable. Neural
diagnostics are in `neural-runtime/NeuralWorker.log` and
`neural-runtime/ReShade.log`. **Advanced > Restart in DLSS SR safe mode** skips
the neural helper for that launch. See [troubleshooting](docs/TROUBLESHOOTING.md).

Thirteen portable CTest suites cover cache/history/settings, export, worker
protocols, runtime policy, cached comparison playback through the real decoders,
and native UI regressions; sixteen GPU smokes and an audio-clock smoke run on an
RTX card with an audio endpoint. Real-media
GPU checks and their limits are recorded per machine and date in the
repository's [hardware records](https://github.com/2600th/dlss5-video-player/blob/main/README.md#building-and-contributing).
These checks are not visual-quality benchmarks or proof of compatibility with
every RTX GPU.

## Runtime notices

The optional neural DLL is a community modification with its NVIDIA signature
removed; ReShade/RenoDX components are unsigned. Runtime locks verify expected
bytes and signature
states, not trust or redistribution permission. Keep the full notices supplied
with a package and review [security](SECURITY.md) and
[third-party terms](THIRD_PARTY.md) before distributing it.

Project source is [MIT-licensed](LICENSE). Rights to NVIDIA components belong
to NVIDIA. Other third-party components, game footage and trademarks belong
to their respective owners. This project does not imply their endorsement.
