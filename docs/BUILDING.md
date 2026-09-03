# Building and testing

Use Windows x64, Visual Studio 2022 with the **Desktop development with C++**
workload and Windows SDK, CMake 3.24 or newer, Git and PowerShell. Run the commands
below from the repository root in a Developer PowerShell for VS 2022.

The source builds native NVIDIA NGX / DLSS Super Resolution and the offline
worker. Experimental neural rendering additionally needs the separately
supplied, hash-locked runtime; that runtime is not part of the NVIDIA SDK checkout.

## Fetch the pinned inputs

For a new checkout:

```powershell
git clone https://github.com/2600th/dlss5-video-player.git
cd dlss5-video-player
git init external/DLSS
git -C external/DLSS remote add origin https://github.com/NVIDIA/DLSS.git
git -C external/DLSS fetch --depth 1 origin a291cc7d2cc642a51566f3dfd5376f635cd1b284
git -C external/DLSS checkout --detach FETCH_HEAD
./tools/fetch_ui_assets.ps1
./tools/fetch_youtube_helpers.ps1
./tools/fetch_ffmpeg_helpers.ps1
```

If the repository and SDK already exist, keep them and fetch/checkout the pinned
SDK revision without repeating initialization. The scripts validate pinned
downloads before staging them. FFmpeg and FFprobe go in `external/ffmpeg/bin`;
both are required by the real-media export tests. YouTube tests use the staged
yt-dlp helper. CI fetches all three sets of assets before building.

## Build and run the tests

Use one `build-upscaling` directory throughout:

```powershell
cmake -S . -B build-upscaling -G 'Visual Studio 17 2022' -A x64 -DBUILD_TESTING=ON
cmake --build build-upscaling --config Release --parallel
ctest --test-dir build-upscaling -C Release --output-on-failure
```

If `cmake` or `ctest` is not on PATH, use the CMake `bin` directory in your
Visual Studio installation under
`Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin`.

Launch `build-upscaling/Release/DLSSVideoPlayer.exe`. Its neural worker is built
as `build-upscaling/Release/neural-runtime/NeuralWorker.exe`. Without the
experimental runtime, a source build uses the native playback path.

The nine suites cover recent history, settings/cache integrity, real-media
export, worker protocols, runtime policy, playback and native UI regressions.
CTest does not establish GPU compatibility or visual quality. For changes to
rendering, timing or decoding, also run applicable GPU/media smoke checks from
the [verification record](https://github.com/2600th/dlss5-video-player/blob/main/docs/VERIFICATION-2026-09-02.md).

CMake accepts absolute `DLSS_SDK`, `FFMPEG_STAGED_DIR` and `YOUTUBE_STAGED_DIR`
paths when verified inputs live elsewhere. Keep downloaded binaries out of Git.

## Add the experimental runtime

With the complete locked input set already in `external/runtime`, stage it
only in the worker's subdirectory:

```powershell
./tools/stage_runtime.ps1 -InputDirectory external/runtime -Destination build-upscaling/Release/neural-runtime
Copy-Item packaging/ReShade.ini,packaging/ReShadePreset.ini build-upscaling/Release/neural-runtime
```

The configuration copy is for initial setup; preserve existing neural settings
before replacing it. Never place the neural `dxgi.dll` beside the main player.
See [runtime setup](DLSS5_SETUP.md) for loading and validation contracts.

`build_windows.bat` combines validation, build, tests and runtime staging for
maintainers who already have the complete locked runtime and FFmpeg inputs.
It also refreshes pinned UI/YouTube helpers. Set `DLSS_SDK_DIR` or `FFMPEG_BIN_DIR`
before running it to override their paths. Use the manual CMake route above
for a source build without the experimental runtime.

## Assemble a package

Developer output is not a distributable folder. The assembler performs a
fresh clean build and verifies an explicit file allowlist and manifest:

```powershell
./tools/package_release.ps1 -BuildDirectory build-upscaling -PackageSuffix ''
./tools/verify_package.ps1 -Zip dist/DLSSVideoPlayer-v0.13.0-win64.zip -PackageSuffix ''
```

This complete experimental package requires the locked runtime and helpers.
The assembler refuses to replace an existing output; select a new suffix for
another local candidate. The published download uses the
`dlss5-video-player-v0.13.0-win64.zip` name.

`package_release.bat` wraps the complete package with the default `-upscaling`
suffix. `package_public_release.bat` creates the smaller core package:
application, official SDK DLSS runtime, notices and documentation, without
the neural runtime, FFmpeg or YouTube helpers. The `v*` tag workflow publishes
this core variant; the complete experimental release is assembled and uploaded
separately.

Review the applicable third-party terms before distributing any package.
End users run the extracted player, not these build scripts.
