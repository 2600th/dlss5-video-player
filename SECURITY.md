# Security

_Verified against 0.26.0 (c81ccd8) on 2026-09-25._

The player launches its package-local `ffmpeg.exe`, `ffprobe.exe`,
`yt-dlp.exe`, and `deno.exe` helpers without a command shell. Playback falls
back to an FFmpeg in an absolute `PATH` directory when the package has none,
never the current directory or a relative `PATH` entry; rendering and export
never do. YouTube support is limited to validated public HTTPS video URLs.
Besides YouTube, the player contacts only `api.github.com` for the update check
and `i.ytimg.com` for trailer thumbnails, both over HTTPS; `[Updates] Enabled=0`
and `[Start] ThumbnailFetch=0` in `DLSSVideoPlayer.ini` turn them off.

Offline neural jobs run in the package-local
`neural-runtime/NeuralWorker.exe`. The experimental proxy/add-on belongs in
that subdirectory; the player rejects the old root-level proxy layout.

The experimental package intentionally contains a modified neural DLL whose
NVIDIA signature was removed, plus unsigned ReShade/RenoDX files. Those signature
states are disclosed separately from malware-scan results. The exact release
inputs are pinned by size and SHA-256 and the package verifier rejects drift or
unexpected files.

The neural DLL's embedded NVIDIA signature is invalidated by the RTX 40
compatibility modification. Do not describe it as legitimately signed. A hash
match proves reproducibility only; scan and isolate untrusted binaries before
execution, and do not upload or redistribute them without authorization.

Redistribution permission for the supplied combined experimental runtime set is
unresolved. Do not publish a package until the applicable upstream terms have
been reviewed. Report security-sensitive issues privately to the maintainer.
