# Security

_Verified against 0.28.0 (4f3150a) on 2026-10-02._

The player launches its package-local `ffmpeg.exe`, `ffprobe.exe`,
`yt-dlp.exe`, and `deno.exe` helpers without a command shell. Playback falls
back to an FFmpeg in an absolute `PATH` directory when the package has none,
never the current directory or a relative `PATH` entry; rendering and export
never do. YouTube support is limited to validated public HTTPS video URLs.
Besides YouTube, the player contacts only `api.github.com` for the update check
and `i.ytimg.com` for trailer thumbnails, both over HTTPS; `[Updates] Enabled=0`
and `[Start] ThumbnailFetch=0` in `DLSSVideoPlayer.ini` turn them off. On an
RTX 20/30 it can also fetch the community dlssg_sm86 Frame Generation add-on
from `raw.githubusercontent.com`, only when the user accepts the offer, from one
pinned commit, with every file checked against its SHA-256 before it is written.
The player loads that add-on's proxy (`version.dll`, `winmm.dll`, `dbghelp.dll`
or `dinput8.dll`) from beside itself, by full path, only when the add-on's
`dlssg_sm86.ini` is there too, and never in safe mode. A proxy put there by hand
loads whether or not it is the pinned build; the log records its SHA-256 and
says which, and so does the start screen.

Offline neural jobs run in the package-local
`neural-runtime/NeuralWorker.exe`. The experimental proxy/add-on belongs in
that subdirectory; the player rejects the old root-level proxy layout.

The experimental package intentionally contains a modified neural DLL whose
NVIDIA signature was removed, plus unsigned ReShade/RenoDX files. Those signature
states are disclosed separately from malware-scan results. The exact release
inputs are pinned by size and SHA-256 and the package verifier rejects drift or
unexpected files.

The neural DLL's embedded NVIDIA signature is invalidated by the community
modification that extends it to every RTX generation (the universal SF-v2
build that `packaging/runtime-lock.json` locks). Do not describe it as
legitimately signed. A hash
match proves reproducibility only; scan and isolate untrusted binaries before
execution.

The combined experimental runtime set is published with the releases as a
community project. No permission from the upstream owners to redistribute it
is claimed, and their terms still govern it (THIRD_PARTY.md). Report
security-sensitive issues privately to the maintainer.
