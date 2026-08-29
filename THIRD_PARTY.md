# Third-party components

This repository contains project source code under the MIT License, but it interoperates with third-party software that has separate licenses and terms.

## NVIDIA DLSS / NGX

The one-click build clones the official NVIDIA DLSS repository into `external/DLSS`. NVIDIA files are not relicensed by this project. Review NVIDIA's license in that checkout before redistributing NVIDIA binaries.

## FFmpeg

The one-click build reuses an installed FFmpeg or downloads a Windows FFmpeg build for local use. FFmpeg and distributed builds are governed by their own licenses/configuration.

## ReShade

ReShade is optional for native DLSS SR but required for the experimental RenoDX DLSS 5 workflow described in the documentation. ReShade is a separate project.

## RenoDX / experimental DLSS 5 runtime

`renodx-dlss5.addon64`, `nvngx_dlssnr.dll` and related experimental runtime files are not included in this repository. Users obtain and use them separately under the terms applicable to those files.
