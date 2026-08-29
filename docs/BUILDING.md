# Building

## Supported build environment

The project currently targets Windows x64 and Direct3D 12.

Required tools:

- Visual Studio 2022 with the **Desktop development with C++** workload
- Windows 10/11 SDK
- Git for Windows
- CMake (the copy bundled with Visual Studio is supported)

An internet connection is required for the first one-click build because the script clones the official NVIDIA DLSS SDK and obtains FFmpeg when no local FFmpeg installation is available.

## One-click build

From a normal Command Prompt:

```bat
build_windows.bat
```

The script searches for Git, Visual Studio 2022 and CMake, then prepares:

```text
external/DLSS/
external/ffmpeg/bin/
build/
```

The Release executable is written to:

```text
build\Release\DLSSVideoPlayer.exe
```

## Manual CMake build

First make sure the NVIDIA DLSS repository is available at `external/DLSS`:

```bat
git clone --depth 1 https://github.com/NVIDIA/DLSS.git external\DLSS
```

Then configure/build:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

FFmpeg is not linked into the executable. `ffmpeg.exe` and `ffprobe.exe` are runtime helpers and should be placed beside the player executable. The one-click build does this automatically.

## Experimental DLSS 5 files

The source project compiles without the experimental DLSS 5 Neural Rendering package. Those files are runtime-only and should not be committed to the repository.

If you place the experimental files beside `build_windows.bat` or in a local `streamline` folder, the build script can stage recognized runtime files into `build\Release` for local testing.

## Compiler settings

The project uses:

- C++20
- `/W4`
- `/permissive-`
- `/EHsc`
- `/Zc:__cplusplus`

Warnings should be treated as bugs during development even though the project does not globally enable `/WX`.
