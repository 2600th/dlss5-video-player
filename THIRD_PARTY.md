# Third-party components

_Verified against 0.25.0 (1988cac) on 2026-09-22._

The project source is MIT-licensed, but the release interoperates with and may
redistribute components under separate terms. No upstream endorsement is
claimed. Exact packaged binaries are pinned in `packaging/runtime-lock.json` and
`packaging/tool-lock.json`.

## NVIDIA DLSS / NGX

Source and terms: https://github.com/NVIDIA/DLSS

The package uses NVIDIA-signed DLSS SR 310.9.1 (`nvngx_dlss.dll`) and the
NVIDIA-signed DLSS Frame Generation snippet `nvngx_dlssg.dll` (`310.7.0.0`,
taken unmodified from the pinned official NVIDIA/DLSS SDK checkout `a291cc7`
and used by the offline frame-generation conversion pass), together with
ShortFuse's community-modified universal neural-rendering DLL
(`310.8.SF-v2`, from the `RankFTW/rhi-repo` release mirror), which extends the
leaked 310.8.0 runtime to Turing, Ampere, Ada and Blackwell. The modification
removed the embedded NVIDIA signature, so that file is unsigned and must not be
represented as authentic; both NVIDIA snippets above keep NVIDIA's own
signature. `nvngx_dlss.dll` is pinned byte for byte in
`packaging/runtime-lock.json`, which is the render helper's runtime set;
`nvngx_dlssg.dll` is not in that lock because the helper never loads it - the
player creates the Frame Generation feature - and `tools/verify_package.ps1`
holds the packaged copy to the pinned SDK's own bytes instead. NVIDIA files are not
relicensed by this project.

## NVIDIA Streamline

Source and terms: https://github.com/NVIDIA-RTX/Streamline

Streamline is pinned at 2.13.0.0 rather than the newer 2.14.1.0, and the
reason is a removal rather than a risk: 2.14.x no longer ships
`sl.dlss_nr.dll`, the Streamline plugin for the one NGX feature this player
exists to drive. Nothing is lost by staying: the add-on runs at
`EnableHooks=2`, which leaves Streamline unpatched, and no `sl.*` module is
mapped in any render log - the upgrade would be new version numbers on files
that are never loaded, bought by dropping the one that would matter if the
`EnableHooks=1` fallback were ever needed. DLSS SR moved to 310.9.1 on its own
because that module *is* loaded: RenoDX detours it to observe the player's
DLSS/DLAA create before building feature 18.

The matching package includes the exact NVIDIA-signed Streamline 2.13 files in
the runtime lock. NVIDIA files remain subject to NVIDIA's applicable terms.

## NVIDIA Optical Flow SDK

Source and terms: https://developer.nvidia.com/opticalflow-sdk

Motion vectors are estimated on NVOFA through the SDK's D3D12 interface. Two
headers are vendored in `external/nvof`. NVIDIA licenses each of them under MIT
in its own copyright block, explicitly scoped - "this copyright notice applies
to this header file only" - which is the same grant FFmpeg relies on to ship
`nv-codec-headers`. Both notices are reproduced in
`THIRD_PARTY_LICENSES/nvidia-optical-flow-MIT.txt`.

Nothing else from the SDK is included. Its samples, helper classes and binaries
are covered by NVIDIA's separate licence agreement, and `nvofapi64.dll` is a
driver component loaded by name at run time, never redistributed here.

## NVIDIA Video Codec SDK (NVENC API header)

Source: https://github.com/FFmpeg/nv-codec-headers, tag `n13.1.15.0` (commit
`0a6fba9a2820628b8103464f4c8753ee05838baa`)

The neural render's cache capture can be encoded straight from the GPU through
NVENC's D3D12 interface. The one header that needs, `nvEncodeAPI.h` (NVENC API
13.1), is vendored unmodified in `external/nvenc` from FFmpeg's copy of it, which
is the file FFmpeg itself builds `hevc_nvenc` against. NVIDIA licenses it under
MIT in its own copyright block, scoped to that file; the notice, the pinned tag
and the file's SHA-256
(`8776fddcb8febc6aec4d73989b1f21831eb30306bc583da55b4bf0c14a1dc228`) are in
`THIRD_PARTY_LICENSES/nvidia-video-codec-MIT.txt`.

Nothing else from the Video Codec SDK is included and no NVIDIA import library is
linked. `nvEncodeAPI64.dll` is a driver component, loaded from the system
directory at run time after its catalog signature is verified, and never
redistributed here.

## ReShade

Source and license: https://github.com/crosire/reshade

The packaged `dxgi.dll` is ReShade 6.8.0 and is unsigned.

## RenoDX

Source and license information: https://github.com/clshortfuse/renodx

The selected `renodx-dlss5.addon64` 6.5.3 asset comes from the
`RankFTW/rhi-repo` release mirror. It is unsigned and enabled by default on
every detected NVIDIA RTX GPU. Redistribution permission for the combined
experimental runtime set remains unresolved.

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
