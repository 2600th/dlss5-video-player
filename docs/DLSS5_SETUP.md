# Experimental DLSS 5 neural-rendering mode

This is a community experiment built around a separately supplied RenoDX /
ReShade add-on and modified neural runtime. It is not NVIDIA's official
production DLSS 5 integration or a claim that the public NVIDIA SDK checkout
contains DLSS 5. The source build calls the official NGX DLSS Super Resolution
interface; the add-on observes those raw D3D12 feature calls at runtime.

NVIDIA describes DLSS 5 as consuming frame color and motion vectors to generate
temporally consistent lighting and material detail. Its official September 3,
2026 debut targets RTX 50-series GPUs. A video does not provide a game engine's
authoritative motion vectors, geometry, materials, or masks, so this player
reconstructs approximate guides from decoded frame history. Treat the result as
experimental and verify actual module loading, add-on status, and visual output
separately.

The private build isolates the experimental runtime in `neural-runtime/`,
alongside `NeuralWorker.exe`. The player root must not contain `dxgi.dll`.
The player treats the experimental runtime as an atomic layout. If all four
of `ReShade.ini`, `dxgi.dll`, `renodx-dlss5.addon64`, and
`nvngx_dlssnr.dll` are absent, a normal source build starts in native DLAA
developer mode. If only part of that layout is present, startup fails closed
instead of loading a mixed runtime. Do not replace individual DLLs with files
from another pack.

On detected RTX 40 and RTX 50 GPUs, a complete experimental layout enables
`renodx-dlss5.addon64` by default. RTX 50 is the official DLSS 5 hardware
generation. RTX 40 support in this project relies on a community-modified
310.8.0 runtime and must not be described as official NVIDIA support.

The runtime lock currently selects RenoDX DLSS 5 add-on 4.70. Normal-mode
helper bootstrap atomically enforces only these managed values in
`neural-runtime/ReShade.ini`:

```ini
[RenoDX.DLSS5]
EnableHooks=2
NeuralUplift=1
NREnableUpscaling=0
```

`EnableHooks=2` selects RenoDX's raw-NGX-only path. The player calls NGX
directly and does not use Streamline, so this avoids installing an unnecessary
Streamline hook.

Other RenoDX controls—including preset, style, intensity, automatic mask, and
guide overrides—are preserved. Safe mode skips the neural helper entirely and
does not change those user settings.

The selected neural runtime is modified and reports Authenticode `HashMismatch`.
Its embedded NVIDIA signature no longer validates. The ReShade proxy and RenoDX
add-on are unsigned. These signature states do not establish malware or safety,
and matching a SHA-256 lock proves only that a file is the expected byte stream.
See `EXPERIMENTAL_RUNTIME_NOTICE.txt`.

The offline neural helper uses a native 1:1 DLAA carrier, with upscaling off.
The player's independent runtime SR toggle also starts off. Its output target
defaults to 1440p, with 2160p selectable; the backend selects a supported NGX
quality range without resizing or downsampling the decoded source.

## Verified pre-render and playback profile

When the complete experimental layout is active, version 0.12.0 defaults to
exact 1080p for YouTube Auto and native-resolution DLAA. If exact 1080p is not
available, Auto uses the highest compatible source up to 4K. Manual source
choices are 1080p, 1440p, and 2160p; 480p and 720p are automatic fallbacks only.
Spatial DLSS upscaling and RenoDX neural upscaling both remain off.

Neural rendering finishes before playback. The player materializes a private
local source when needed, evaluates every frame in timestamp order, reads the
neural output back from D3D12, encodes with NVENC (or restarts from frame zero
with software H.264), and probes the completed video. Only a complete schema-3
manifest with matching hashes, dimensions, frame count, monotonic source
timing, video duration, final-frame decode, runtime digest, one captured native
submission per source frame, the NGX-only inline interception contract armed
before capture, and a stabilized feature-18 receipt that advances after the
captured sequence is promoted. A feature-18 failure, skip, or pass-through
marker rejects the complete job. Offline decoding uses the software FFmpeg path
so CUDA resources remain available to feature 18 and NVENC; normal playback
continues to prefer hardware decoding.

On the tested RTX 5090, the 30.03-second Resident Evil Requiem example produced
all 1,800 frames at 1920×1080/59.94 fps with `upscaling=false`. Isolated cached
playback dropped 1 frame (0.056%) and reopened the unchanged cache
entry without neural re-evaluation. RTX 40 policy and runtime loading are
covered by automated tests, but physical RTX 40 neural output remains
unverified on this system.

The architecture follows lessons from [Zonnery's offline
converter](https://github.com/Zonnery/dlss5-nr-player) and the verification
approach documented by [Merserk's visual
enhancer](https://github.com/Merserk/dlss5-visual-enhancer). It does not import
or redistribute runtime binaries from those repositories.

The cache is stored under
`%LOCALAPPDATA%\DLSSVideoPlayer\NeuralCache\v1`. **Advanced > Clear Neural
Cache** reports its current size and requires confirmation. Clearing is blocked
while acquisition, a neural job or export is active. Confirmed clearing closes
playback first. Windows package virtualization may redirect the physical cache
under the launching app's LocalCache. Recent-five retention and settings-aware
cache identity are described in the [usage guide](USAGE.md).

The player intentionally retains the add-on interception backend instead of
also loading feature 18 directly. Running both would duplicate neural passes
and introduce a second undocumented NGX session. See
[Related implementations](RELATED_PROJECTS.md) for the comparison.

## Checking observed status

The player status reports the selected configuration. The hidden helper has no
interactive overlay: inspect `neural-runtime/ReShade.log` for feature-18 creation,
inline evaluation and no later failure. Successful cache promotion additionally
requires captured-frame counts, hashes and independent media validation. Native
runtime SR evaluations appear in the player's separate `DLSSVideoPlayer.log`.

## Safe mode

If the experimental path is unstable, choose **Advanced > Restart in DLSS SR
safe mode**. Safe mode skips the neural helper for that launch and keeps the
official NGX path available. A later normal launch on an RTX 40/50 policy target
enables neural pre-rendering again. SR remains off by default on each launch.
