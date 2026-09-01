# Third-party components

This repository contains project source code under the MIT License, but it interoperates with third-party software that has separate licenses and terms.

## NVIDIA DLSS / NGX

The one-click build clones the official NVIDIA DLSS repository into `external/DLSS`. NVIDIA files are not relicensed by this project. Review NVIDIA's license in that checkout before redistributing NVIDIA binaries.

## FFmpeg

The one-click build reuses an installed FFmpeg or downloads a Windows FFmpeg build for local use. FFmpeg and distributed builds are governed by their own licenses/configuration.

## ReShade

ReShade is optional for native DLSS SR but required for the experimental RenoDX DLSS 5 workflow described in the documentation. ReShade is a separate project.

## Tabler Icons

The embedded UI font and application icon are derived from Tabler Icons 3.46.0, copyright (c) 2020-2026 Paweł Kuna, and distributed under the MIT License. The pinned source metadata and complete license text are in `assets/tabler/SOURCE.txt` and `assets/tabler/LICENSE`.

## yt-dlp

The optional YouTube resolver uses the official yt-dlp 2026.08.19 Windows executable, distributed under the Unlicense. That official executable embeds its EJS support. The executable is fetched from its pinned upstream release and is not committed to this repository.

## Deno

Deno 2.9.5 supplies the JavaScript runtime used by yt-dlp's EJS support. Deno is distributed under the MIT License. The Windows executable is extracted from its pinned official archive and is not committed to this repository.

## RenoDX / experimental DLSS 5 runtime

`renodx-dlss5.addon64`, `nvngx_dlssnr.dll` and related experimental runtime files are not included in this repository. Users obtain and use them separately under the terms applicable to those files.
