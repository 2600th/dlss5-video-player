# Security

The player launches only its package-local `ffmpeg.exe`, `ffprobe.exe`,
`yt-dlp.exe`, and `deno.exe` helpers without a command shell. YouTube support is
limited to validated public HTTPS video URLs.

The experimental package intentionally contains a modified neural DLL with
Authenticode `HashMismatch`, plus unsigned ReShade/RenoDX files. Those signature
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
