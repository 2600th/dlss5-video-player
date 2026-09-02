# dlss5-video-player v0.12.0.1

Experimental prerelease, 2026-09-02. Tag: `dlss5-video-player-v0.12.0.1`.
The application version remains 0.12.0; this release preserves the exact ZIP
contents that passed local hardware and package validation. Only the release,
tag and download names changed; the internal folder and binaries are unchanged.

## Included

- Verified offline neural rendering and reusable cached playback, with
  synchronized original/neural comparison. Neural Rendering is on by default.
- Separate runtime DLSS Upscaling, off by default, with 1440p and 2160p output.
  Frame Generation remains unavailable.
- Animated loading/cache-check indicators, real neural frame progress and ETA.
- Fixes for seek startup, example selection after completion, Unicode text,
  independent feature controls and button spacing.
- Required runtime and YouTube helper files in the complete Windows x64
  ZIP, including the isolated `neural-runtime` helper folder.

## Installation

Download `dlss5-video-player-v0.12.0.1-win64.zip`, extract the entire
archive into a new folder and launch `DLSSVideoPlayer.exe` from File Explorer.
Do not extract over an older build or move DLLs out of `neural-runtime`.

## Verification and limitations

- The Release build and all six CTest suites passed before the original
  publication. No application code changed for this rename.
- ZIP verification passed again with exactly 40 allowlisted files. The archive
  SHA-256 is unchanged from the original hardware-tested build.
- Previous local RTX 5090 validation confirmed Resident Evil neural rendering,
  animated progress, subsequent cached playback and runtime upscaling.
- RTX 40 hardware was not tested. Its neural compatibility is an unofficial
  community modification, not a guarantee of support or quality.
- This bundle contains modified/unsigned experimental third-party components.
  Rights to NVIDIA components belong to NVIDIA. Other third-party components
  belong to their respective owners and retain their own license terms. Read
  `EXPERIMENTAL_RUNTIME_NOTICE.txt` and `THIRD_PARTY.md` in the archive.
- No antivirus scan was performed, as requested. Hash checks establish file
  identity, not safety or legitimacy.

## Artifact integrity

- Size: 318,361,195 bytes.
- SHA-256: `CDD20C74B91551DFAE7C40627BB1BF6CADFDBECE12FC2E0EC7D711B9A3FAEDE2`.
- The `dlss5-video-player-` tag prefix leaves the existing public-core release automation
  unchanged. The binary ZIP is attached to the private GitHub release, not
  committed to source control.
