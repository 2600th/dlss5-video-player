# DLSS Video Player 0.12.0 - personal experimental build 1

Private personal-use prerelease, 2026-09-02. Tag: `personal-v0.12.0.1`.
The application version remains 0.12.0; this release preserves the exact ZIP
that passed local hardware and package validation.

## Included

- Verified offline neural rendering and reusable cached playback, with
  synchronized original/neural comparison. Neural Rendering is on by default.
- Separate runtime DLSS Upscaling, off by default, with 1440p and 2160p output.
  Frame Generation remains unavailable.
- Animated loading/cache-check indicators, real neural frame progress and ETA.
- Fixes for seek startup, example selection after completion, Unicode text,
  independent feature controls and button spacing.
- Required private runtime and YouTube helper files in the complete Windows x64
  ZIP, including the isolated `neural-runtime` helper folder.

## Installation

Download `DLSSVideoPlayer-v0.12.0-upscaling-loading-win64.zip`, extract the entire
archive into a new folder and launch `DLSSVideoPlayer.exe` from File Explorer.
Do not extract over an older build or move DLLs out of `neural-runtime`.

## Verification and limitations

- Release build and all six CTest suites passed again before publishing.
- ZIP verification passed with exactly 40 allowlisted files; both packaged
  executables match the current Release build.
- Previous local RTX 5090 validation confirmed Resident Evil neural rendering,
  animated progress, subsequent cached playback and runtime upscaling.
- RTX 40 hardware was not tested. Its neural compatibility is an unofficial
  community modification, not a guarantee of support or quality.
- This bundle contains modified/unsigned experimental third-party components.
  Third-party rights remain with their respective owners; the project's MIT
  license and a testing notice do not grant redistribution permission.
  Keep this release private while those permissions remain unresolved. Read
  `EXPERIMENTAL_RUNTIME_NOTICE.txt` and `THIRD_PARTY.md` in the archive.
- No antivirus scan was performed, as requested. Hash checks establish file
  identity, not safety or legitimacy.

## Artifact integrity

- Size: 318,361,195 bytes.
- SHA-256: `CDD20C74B91551DFAE7C40627BB1BF6CADFDBECE12FC2E0EC7D711B9A3FAEDE2`.
- The `personal-` tag prefix leaves the existing public-core release automation
  unchanged. The binary ZIP is attached to the private GitHub release, not
  committed to source control.
