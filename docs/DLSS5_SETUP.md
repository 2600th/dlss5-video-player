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

The player now treats the experimental runtime as an atomic layout. If all four
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
bootstrap atomically enforces only these managed values in `ReShade.ini`:

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
guide overrides—are preserved. Safe mode disables the add-on through ReShade's
canonical `DisabledAddons` token and does not destroy those user settings.

The selected neural runtime is modified and reports Authenticode `HashMismatch`.
Its embedded NVIDIA signature no longer validates. The ReShade proxy and RenoDX
add-on are unsigned. These signature states do not establish malware or safety,
and matching a SHA-256 lock proves only that a file is the expected byte stream.
See `EXPERIMENTAL_RUNTIME_NOTICE.txt`.

DLAA is the default NGX carrier at native 1:1 resolution, so spatial DLSS
upscaling is off by default. Quality, Balanced, Performance, Ultra Performance,
and Auto remain explicit user selections.

## Verified pre-render and playback profile

When the complete experimental layout is active, version 0.12.0 defaults to
exact 1080p for YouTube Auto and native-resolution DLAA. If exact 1080p is not
available, Auto uses the highest compatible source up to 4K. Manual source
choices are 1080p, 1440p, and 2160p; 480p and 720p are automatic fallbacks only.
Spatial DLSS upscaling and RenoDX neural upscaling both remain off.

Neural rendering finishes before playback. The player materializes a private
local source when needed, evaluates every frame in timestamp order, reads the
neural output back from D3D12, encodes with NVENC (or restarts from frame zero
with software H.264), and probes the completed video. Only a complete manifest
with matching hashes, dimensions, frame count, video duration, final-frame
decode, runtime digest, and feature-18 evidence is promoted into the cache.

On the tested RTX 5090, the 30.03-second Resident Evil Requiem example produced
all 1,800 frames at 1920×1080/59.94 fps with `upscaling=false`. Cached playback
dropped 5 frames (0.28%), stayed responsive, and reopened the unchanged cache
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
while a neural job or cached playback owns an entry.

The player intentionally retains the add-on interception backend instead of
also loading feature 18 directly. Running both would duplicate neural passes
and introduce a second undocumented NGX session. See
[Related implementations](RELATED_PROJECTS.md) for the comparison.

## Checking observed status

The player status reports the selected configuration, not proof that a neural
workload evaluated successfully. Press **Home**, open ReShade's **Add-ons** page,
and inspect the RenoDX add-on's observed status. Native NGX status and evaluation
counters are shown in the player.

## Safe mode

If the experimental path is unstable, choose **Advanced > Restart in DLSS SR
safe mode**. Safe mode disables only the RenoDX neural add-on for that launch and
keeps the official NGX path available. A later normal launch on an RTX 40/50
policy target restores the add-on setting with at most one bootstrap relaunch;
it does not opt into spatial upscaling.
