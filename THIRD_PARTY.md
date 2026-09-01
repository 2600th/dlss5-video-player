# Third-party components

This repository contains project source code under the MIT License, but it interoperates with third-party software that has separate licenses and terms.

## NVIDIA DLSS / NGX

The one-click build clones the official NVIDIA DLSS repository into `external/DLSS`. NVIDIA files are not relicensed by this project. Review NVIDIA's license in that checkout before redistributing NVIDIA binaries.

The experimental package uses the exact NVIDIA-signed DLSS SR 310.8 runtime
listed in `packaging/runtime-lock.json`, paired with a user-supplied modified
DLSS neural-rendering DLL whose Authenticode status is `HashMismatch`.

## NVIDIA Streamline

The experimental package may include the exact NVIDIA-signed Streamline 2.13
runtime files pinned in `packaging/runtime-lock.json`. NVIDIA files remain under
NVIDIA's applicable terms and are not relicensed by this project.

## FFmpeg

The one-click build reuses an installed FFmpeg or downloads a Windows FFmpeg build for local use. FFmpeg and distributed builds are governed by their own licenses/configuration.

## ReShade

ReShade is optional for native DLSS SR but required for the experimental RenoDX DLSS 5 workflow described in the documentation. ReShade is a separate project.

## RenoDX / experimental DLSS 5 runtime

The package configuration enables the separately supplied
`renodx-dlss5.addon64` by default. The add-on and matching runtime files are
experimental, not committed to Git, and pinned by exact hash. Redistribution
permission for the supplied combined binary set remains unresolved; consult the
upstream projects and `EXPERIMENTAL_RUNTIME_NOTICE.txt` before sharing it.

## Tabler Icons

The embedded UI font and application icon are derived from Tabler Icons 3.46.0, copyright (c) 2020-2026 Paweł Kuna, and distributed under the MIT License. The pinned source metadata and complete license text are in `assets/tabler/SOURCE.txt` and `assets/tabler/LICENSE`.

## yt-dlp

The optional YouTube resolver uses the official yt-dlp 2026.08.19 Windows executable. The yt-dlp source project is offered under the Unlicense, but the official PyInstaller executable bundles GPLv3+ components and the combined executable is GPLv3+. See the upstream tag's [Licensing section](https://github.com/yt-dlp/yt-dlp/blob/2026.08.19/README.md#licensing) and [`THIRD_PARTY_LICENSES.txt`](https://github.com/yt-dlp/yt-dlp/blob/2026.08.19/THIRD_PARTY_LICENSES.txt). The executable embeds EJS support, is fetched from its pinned upstream release, and is not committed to this repository.

Any package that redistributes this exact executable must include a complete copy of the tag's upstream `THIRD_PARTY_LICENSES.txt` as `THIRD_PARTY_LICENSES/yt-dlp-2026.08.19.txt`. The pinned Windows executable does not expose a `--license` option, so packaging must use the tagged notice file and must not present the executable as Unlicense-only.

## Deno

Deno 2.9.5 supplies the JavaScript runtime used by yt-dlp's EJS support. Deno is distributed under the MIT License. The Windows executable is extracted from its pinned official archive and is not committed to this repository.
