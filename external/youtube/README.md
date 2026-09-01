# YouTube resolver helpers

Run `powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File tools\fetch_youtube_helpers.ps1` from the repository root to fetch the two package-local helper executables used by YouTube playback.

The script accepts only these pinned releases:

| Helper | Official source | SHA-256 of download | Required version identity |
|---|---|---|---|
| yt-dlp | `https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp.exe` | `66674953FE251B89F4D08C5F0E35E0728679BD67AB3D7D05C0562AF101DD3E7A` | `2026.08.19` |
| Deno x86_64 Windows zip | `https://github.com/denoland/deno/releases/download/v2.9.5/deno-x86_64-pc-windows-msvc.zip` | `171EFAB55AC6B9881FD53EE4C20F8BF3BB1340FFC618483746909014DB12216A` | `deno 2.9.5` parsed from the first output line |

Both downloads are verified before either installed helper is changed. The verified Deno archive is then extracted in temporary staging, both executables are version-checked, and the destination is replaced as one directory transaction with rollback on swap failure. Re-running the script refreshes both helpers to the same pinned inputs.

`yt-dlp.exe` and `deno.exe` are intentionally ignored by Git. Release packaging decides when to place the verified helpers beside the application; this directory keeps only their provenance in source control.

The yt-dlp source project is offered under the Unlicense, but that is not the complete license for the downloaded file. The official Windows executable is a PyInstaller bundle containing GPLv3+ components, so the combined executable is GPLv3+. This distinction and its bundled third-party notices are documented in the upstream [2026.08.19 licensing section](https://github.com/yt-dlp/yt-dlp/blob/2026.08.19/README.md#licensing) and tagged [`THIRD_PARTY_LICENSES.txt`](https://github.com/yt-dlp/yt-dlp/blob/2026.08.19/THIRD_PARTY_LICENSES.txt). The executable also embeds EJS support.

Any redistributable package containing this exact `yt-dlp.exe` must include a complete copy of the tag's upstream `THIRD_PARTY_LICENSES.txt` as `THIRD_PARTY_LICENSES/yt-dlp-2026.08.19.txt`; it must not describe the executable as Unlicense-only. The pinned Windows executable does not expose a `--license` option, so packaging must use that tagged notice file rather than attempting to generate license output from the binary. Deno is distributed under the MIT License and supplies the JavaScript runtime required by the embedded EJS support. See the upstream projects for complete license texts and notices.
