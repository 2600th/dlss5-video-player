# Experimental DLSS 5 neural-rendering mode

_Verified against 0.25.0 (1988cac) on 2026-09-22._

This is a community experiment built around a separately supplied RenoDX /
ReShade add-on and modified neural runtime. It is not NVIDIA's official
production DLSS 5 integration or a claim that the public NVIDIA SDK checkout
contains DLSS 5. The source build calls the official NGX DLSS Super Resolution
interface; the add-on observes those raw D3D12 feature calls at runtime.

A video does not provide a game engine's authoritative motion vectors,
geometry, materials, or masks. This player reconstructs approximate guides
from decoded frame history. Treat the result as experimental and verify actual
module loading, add-on status, and visual output separately.

The private build isolates the experimental runtime in `neural-runtime/`,
alongside `NeuralWorker.exe`. The player root must not contain `dxgi.dll`.
The player treats the experimental runtime as an atomic layout. If all four
of `ReShade.ini`, `dxgi.dll`, `renodx-dlss5.addon64`, and
`nvngx_dlssnr.dll` are absent, a source build uses native playback without the
neural runtime. If only part of that layout is present, startup fails closed
instead of loading a mixed runtime. Do not replace individual DLLs with files
from another pack.

On any detected NVIDIA RTX GPU - GeForce RTX 20, 30, 40 and 50, and RTX-branded
workstation or laptop parts - a complete experimental layout enables
`renodx-dlss5.addon64` by default. The locked neural runtime is ShortFuse's
community-modified universal 310.8 build (`310.8.SF-v2`), which extends the
leaked 310.8.0 runtime from Blackwell to Turing, Ampere and Ada. None of this
is official NVIDIA support: NVIDIA ships DLSS 5 for RTX 50 and has announced
RTX 40 for later. The product name only selects the cache label and the pace
prior; whether feature 18 actually runs is decided by the runtime's own
capability check and the strict evidence chain, which refuse the render rather
than publish an unverified one. Turing and Ampere lack native FP8 tensor
math, so the same network runs several times slower there; the offline cache
still completes, and a live session simply buffers when it cannot keep up.

The runtime lock currently selects RenoDX DLSS 5 add-on 6.5.3. Normal-mode
helper bootstrap atomically enforces only these managed values in
`neural-runtime/ReShade.ini`:

```ini
[RenoDX.DLSS5]
EnableHooks=2
NeuralUplift=1
NRFollowInputRes=0
NRResolutionScale=1
```

`EnableHooks=2` selects RenoDX's raw-NGX-only path. The player calls NGX
directly and does not use Streamline, so this avoids installing an unnecessary
Streamline hook.

`NRFollowInputRes=0` with `NRResolutionScale=1` holds the neural pass at the
source's own resolution. These replace 4.70's single `NREnableUpscaling=0`,
which 6.x removed when it split the working resolution into a mode and a
scale. The scale is a multiplier, not a percentage - the add-on's overlay
merely renders it as a percentage - and writing `100` there is silently
normalised to `1` with nothing logged. A leftover `NREnableUpscaling` in an
existing `ReShade.ini` is left alone at whatever value it holds; 6.x ignores
it, and rewriting a key it classifies as pre-v4 would re-run its config
migration, which backs the file up beside the add-on every time it fires.

One further key is written only when **DLSS > Processing scale** asks for it:
`NRPreUpscale=1` for the 75% and 50% rungs, which put the model on the reduced
picture ahead of DLSS Super Resolution, and `NRPreUpscale=0` when a render at
100% finds a 1 left behind. A render at 100% on a file that holds 0, or no such
key, writes nothing.

Other RenoDX controls—including preset, style, intensity, automatic mask, and
guide overrides—are preserved. Safe mode skips the neural helper entirely and
does not change those user settings.

The selected neural runtime is modified and unsigned: its author removed the
embedded NVIDIA signature (the previous RTX 40 lock reported Authenticode
`HashMismatch` instead). The ReShade proxy and RenoDX add-on are unsigned.
These signature states do not establish malware or safety, and matching a
SHA-256 lock proves only that a file is the expected byte stream.
See [third-party notices](../THIRD_PARTY.md) and the packaged
`EXPERIMENTAL_RUNTIME_NOTICE.txt`.

The offline neural helper uses a native 1:1 DLAA carrier, with upscaling off.
The player's independent runtime SR toggle starts off on a fresh install. Its output target
is Auto: the largest of the 1080p, 1440p and 2160p rungs that the monitor's
current mode can scan out, never one above it. Any rung can be pinned from
**DLSS > Upscaling output**. The backend selects a supported NGX
quality range without resizing or downsampling the decoded source.

## Verified pre-render and playback profile

When the complete experimental layout is active, the current release takes the
tallest rung up to 1440p for YouTube Auto, then the highest bitrate inside it,
falling back to a rung up to 2160p only when nothing at 1440p or below exists,
and uses native-resolution DLAA. Manual source
choices are 1080p, 1440p, and 2160p; 480p and 720p are automatic fallbacks only.
Within the selected resolution, acquisition prefers the highest advertised
video bitrate across codecs. Offline DLSS upscaling and RenoDX neural upscaling
remain off; playback SR follows the separately saved player preference.

Neural rendering either completes into a cache entry before playback or runs
behind live playback, publishing finalized segments that playback follows once
a four-second lead exists. The player materializes a private
local source when needed, evaluates every frame in timestamp order, reads the
neural output back from D3D12, encodes with NVENC (or restarts from frame zero
with software H.264), and probes the completed video. Only a complete schema-5
manifest with matching hashes, dimensions, frame count, monotonic source
timing, video duration, final-frame decode, runtime digest, one captured native
submission per source frame, the NGX-only inline interception contract armed
before capture, and a stabilized feature-18 receipt that advances after the
captured sequence is promoted. A feature-18 failure, skip, or pass-through
marker rejects the complete job. Offline decoding uses the software FFmpeg path
so CUDA resources remain available to feature 18 and NVENC; normal playback
continues to prefer hardware decoding.

On the tested RTX 5090 (v0.15.0, previous RTX 40-targeted runtime), the
complete GTA VI Trailer 2 run produced all 5,002 frames at 1920 x 1080 / 30
fps, with default neural intensity 1.00 and no upscaling. Reopening the same
example reused both source and neural caches. See the
[verification record](https://github.com/2600th/dlss5-video-player/blob/main/docs/VERIFICATION-2026-09-02.md)
for measurements and their limits. On an RTX 4080 SUPER (driver 610.47) the
universal runtime passed the strict GPU smoke and a full 1080p30 live session
at 15.31 ms/frame; see the
[RTX 4080 record](https://github.com/2600th/dlss5-video-player/blob/main/docs/VERIFICATION-2026-09-09-RTX4080.md).
An RTX 5090 (driver 616.64) then confirmed the universal runtime at 1080p
(11.89 ms/frame), 1440p and 4K; see the
[RTX 5090 record](https://github.com/2600th/dlss5-video-player/blob/main/docs/VERIFICATION-2026-09-09-RTX5090.md).
The same machine re-ran all three geometries on 0.17.0 at 8.4, 15.4 and
42.0 ms/frame; see the
[0.17.0 RTX 5090 record](https://github.com/2600th/dlss5-video-player/blob/main/docs/VERIFICATION-2026-09-10-RTX5090.md).
Later records - the 0.20.0 RTX 5090 session, the 0.21.x RTX 4080 gate and
cold-stack record, and the driven-session matrix - are listed with dates under
the README's [hardware records](https://github.com/2600th/dlss5-video-player/blob/main/README.md#building-and-contributing).
Turing and Ampere have no hardware verification in this project yet.

Feature 18 is created by the driver's NGX core, so the driver is a hard
requirement independent of the GPU: the player refuses a render below
**610.47**, the lowest driver this project has rendered on, and names 616.64 -
the driver both verification records used - in the message. The floor the wider
community publishes for the same runtime is 616.56. An older driver answers
`CreateFeature` with `0xbad00002`
(`NVSDK_NGX_Result_FAIL_PlatformError`); an architecture the runtime itself
refuses answers `0xbad00001` instead.

The architecture follows lessons from [Zonnery's offline
converter](https://github.com/Zonnery/dlss5-nr-player) and the verification
approach documented by [Merserk's visual
enhancer](https://github.com/Merserk/dlss5-visual-enhancer). It does not import
or redistribute runtime binaries from those repositories.

The cache prefers `cache\v1` beside `DLSSVideoPlayer.exe` and falls back to
`%LOCALAPPDATA%\DLSSVideoPlayer\NeuralCache\v1` when that folder cannot be
written, or is nested so deep that the cache's own paths would pass Windows'
260-character limit. **Advanced > Clear Neural
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
official NGX path available. A later normal launch on an RTX GPU enables
neural pre-rendering again. Playback SR starts off on a fresh install
and subsequently follows the saved preference.
