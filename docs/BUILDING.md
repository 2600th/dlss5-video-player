# Building and testing

The project targets Windows x64 with Visual Studio 2022, CMake, the Windows SDK,
and C++20. Build inputs are deliberately separate from release packaging.

The public source build compiles the native NVIDIA NGX / DLSS Super Resolution
path. The optional experimental DLSS 5 neural-rendering path is a separate
runtime interception layer and is not contained in the public NVIDIA SDK
checkout or produced by this build.

## Pinned local inputs

- NVIDIA/DLSS checkout at commit
  `a291cc7d2cc642a51566f3dfd5376f635cd1b284`.
- A verified FFmpeg/FFprobe pair in `external/ffmpeg/bin`.
- Pinned Tabler assets and YouTube helpers fetched by the scripts in `tools/`.
- For the experimental runtime and complete build script, the hash-locked files
  in `external/runtime`.

Set `DLSS_SDK_DIR` or `FFMPEG_BIN_DIR` before running `build_windows.bat` when
those verified inputs live elsewhere. The build script does not search nearby
folders for alternate DLLs and does not silently replace `nvngx_dlss.dll`.

## Build and test

For the current development layout, use a fresh `build-upscaling` directory.
CMake builds the hidden
`Release/neural-runtime/NeuralWorker.exe` alongside the hook-free player.
```bat
build_windows.bat
```

For a manual build:

```bat
cmake -S . -B build-upscaling -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON -DDLSS_SDK=external/DLSS -DFFMPEG_STAGED_DIR=external/ffmpeg/bin
cmake --build build-upscaling --config Release --parallel
ctest --test-dir build-upscaling -C Release --output-on-failure
```

The current CTest gate has nine suites, including recent history, neural settings
and cache integrity, stream-copy export, and native UI regressions. Supply the
verified FFmpeg/FFprobe pair to exercise real export fixtures. CTest success does
not establish GPU compatibility or visual quality; the [verification record](VERIFICATION-2026-09-02.md)
separates automated checks from hardware and UI observations.

The build script also validates/stages the private runtime; use the manual CMake
commands for a source-only build. Absolute SDK/helper paths are supported when
building from an isolated worktree. Keep private runtime files out of Git.

## Optional experimental runtime and packaging

After building, a private NR test can stage the existing locked runtime into that subdirectory:

```powershell
./tools/stage_runtime.ps1 -InputDirectory external/runtime -Destination build-upscaling/Release/neural-runtime
Copy-Item packaging/ReShade.ini,packaging/ReShadePreset.ini build-upscaling/Release/neural-runtime
./tools/package_release.ps1 -BuildDirectory build-upscaling -PackageSuffix '-upscaling'
```

The package assembler refuses to replace an existing package; choose a new suffix
for another candidate. Never copy the neural `dxgi.dll` into the player root.
Private runtime notices and redistribution restrictions are unchanged.

`build-upscaling/Release` is developer output, not a distributable folder. Authorized
release maintainers assemble and verify the explicit allowlist separately with
`package_public_release.bat`. That command creates the publishable core ZIP
containing the application, the official NVIDIA-signed SDK runtime, notices,
and documentation. It excludes the modified neural DLL, RenoDX/ReShade,
Streamline, FFmpeg, and YouTube helpers.

`package_release.bat` is reserved for a private, complete, hash-locked
experimental layout. Its output is not publishable unless the maintainer has
independently established redistribution rights for every bundled component.
End users do not run build or helper scripts.
