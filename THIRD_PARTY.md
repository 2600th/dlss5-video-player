# Third-party components

The project source is MIT-licensed, but the release interoperates with and may
redistribute components under separate terms. No upstream endorsement is
claimed. Exact packaged binaries are pinned in `packaging/runtime-lock.json` and
`packaging/tool-lock.json`.

## NVIDIA DLSS / NGX

Source and terms: https://github.com/NVIDIA/DLSS

The package uses NVIDIA-signed DLSS SR 310.8 together with a community-modified
RTX 40-targeted neural-rendering DLL. The latter reports Authenticode
`HashMismatch`; its embedded NVIDIA signature is invalid and must not be
represented as authentic. NVIDIA files are not relicensed by this project.

## NVIDIA Streamline

Source and terms: https://github.com/NVIDIA-RTX/Streamline

The matching package includes the exact NVIDIA-signed Streamline 2.13 files in
the runtime lock. NVIDIA files remain subject to NVIDIA's applicable terms.

## ReShade

Source and license: https://github.com/crosire/reshade

The packaged `dxgi.dll` is ReShade 6.8.0 and is unsigned.

## RenoDX

Source and license information: https://github.com/clshortfuse/renodx

The selected `renodx-dlss5.addon64` 4.70 asset comes from the
`RankFTW/rhi-repo` release mirror. It is unsigned and enabled by default on RTX
40/50 policy targets. Redistribution permission for the combined experimental
runtime set remains unresolved.

The player's `[RenoDX.DLSS5]` configuration contract was adapted from the
MIT-licensed `jlrouzies-fr/DLSS5-Feeder` project. Its copyright and license are
included in `THIRD_PARTY_LICENSES/dlss5-feeder-MIT.txt`.

## FFmpeg

Source and licensing: https://ffmpeg.org/legal.html

The package uses FFmpeg 9.0.1 Essentials from gyan.dev. Its reported build
configuration enables GPLv3 components; see `THIRD_PARTY_LICENSES/ffmpeg.txt`.

## yt-dlp

Source and tag: https://github.com/yt-dlp/yt-dlp/tree/2026.08.19

The official Windows executable is a PyInstaller bundle containing GPLv3+
components. Its complete tagged bundled notices are included as
`THIRD_PARTY_LICENSES/yt-dlp-2026.08.19.txt`; it is not described as
Unlicense-only.

## Deno

Source and license: https://github.com/denoland/deno/tree/v2.9.5

Deno 2.9.5 provides yt-dlp's JavaScript runtime and is MIT-licensed. See
`THIRD_PARTY_LICENSES/deno-2.9.5.txt`.

## Tabler Icons

Source: https://github.com/tabler/tabler-icons

The embedded font and application icon derive from Tabler Icons 3.46.0,
copyright (c) 2020-2026 Pawel Kuna, under MIT. The package includes
`THIRD_PARTY_LICENSES/tabler-MIT.txt`.

## Redistribution warning

The package's combined ReShade/RenoDX/NVIDIA/Streamline/patched-neural binary
set has unresolved redistribution permission. Review each upstream's current
terms before sharing or publishing the ZIP; this project does not invent or
grant permission.
