# Building and testing

_Verified against 0.25.0 (1988cac) on 2026-09-22._

Use Windows x64, Visual Studio 2022 or newer with the **Desktop development with
C++** workload and Windows SDK, CMake 3.24 or newer, Git and PowerShell. Run the
commands below from the repository root in a Developer PowerShell for that
Visual Studio.

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

Use one `build-upscaling` directory throughout. Name the generator your install
actually provides - `Visual Studio 17 2022` or `Visual Studio 18 2026` - or omit
`-G` and let CMake pick the newest one it finds:

```powershell
cmake -S . -B build-upscaling -G 'Visual Studio 18 2026' -A x64 -DBUILD_TESTING=ON
cmake --build build-upscaling --config Release --parallel
ctest --test-dir build-upscaling -C Release -LE "gpu|audio" --output-on-failure
```

`-LE "gpu|audio"` excludes the hardware smokes, which need an RTX card with the
staged neural runtime, and `AudioClockSmoke`, which needs an audio render
endpoint. Without it a machine with neither reports skips rather than passes.
See below for running those deliberately.

Naming `Visual Studio 17 2022` on a machine that has only 2026 asks for the v143
toolset that install does not carry, and MSBuild stops with MSB8020 before
compiling anything. A configured directory keeps the generator that created it,
so delete `build-upscaling` after switching toolchains. `build_windows.bat`
detects the edition itself; 2026 installs under
`...\Microsoft Visual Studio\18\<Edition>`, not under the year.

If `cmake` or `ctest` is not on PATH, use the CMake `bin` directory in your
Visual Studio installation under
`Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin`.

Launch `build-upscaling/Release/DLSSVideoPlayer.exe`. Its neural worker is built
as `build-upscaling/Release/neural-runtime/NeuralWorker.exe`. Without the
experimental runtime, a source build uses the native playback path.

The thirteen portable suites cover recent history, settings/cache integrity,
real-media export and cached comparison playback through the real decoders,
runtime lock and worker protocols, runtime, upscaling and export policy, range selection,
frame identity, update checks, the release API surface, prerender, playback and
native UI regressions. Every test carries a time limit, and the real-media suite
reports itself skipped rather than failed when FFmpeg is not staged.

Twelve more are registered under the `gpu` label and need an RTX card with the
neural runtime staged beside the executable: `UpscalingGpuSmoke`,
`MediaGpuSmoke`, `NeuralRangeRenderSmoke`, `NeuralPreflightSmoke`,
`ExportMatrixSmoke`, `DlssgProbeSmoke`, `DlssgEvaluateSmoke`, the four
`FrameGenerationSmoke` registrations and `NetworkPreparedRendererSmoke`. That
last one is the `--gpu` case set of the `PlayerUiRegressionTests` binary rather
than a target of its own: it drives the prepared network renderer path - the one
a YouTube open commits through - which nothing else in the suite reaches.
`NeuralPreflightSmoke` is the same arrangement around `NeuralWorkerTests`, and
it is in the gate because the probe it runs can take neural rendering out for a
whole session on its own. `ExportMatrixSmoke` renders all seven combinations of
Super Resolution, neural rendering and frame generation through a 3.5 s 720p30
clip and checks the geometry, the frame count and the bytes of each.

Two of the twelve skip on hardware that is working correctly.
`DlssgEvaluateSmoke` needs three generated frames per source pair, and
multi-frame generation is Blackwell-only, so every RTX 40 and earlier reports
it skipped. `FrameGenerationSmoke` needs `external/test-media/dlaa-smoke.mp4`,
which is fetched by no script; a checkout without it skips rather than failing.
`ctest -LE "gpu|audio"` is the portable run CI performs; `ctest -L "gpu|audio"`
runs the hardware set. `AudioClockSmoke` is the `audio` one: it asserts the
audio clock every video frame's due time is computed from, against a real
render endpoint. Each opens with a no-adapter check and reports itself skipped
(exit 125) rather than failed on a machine without a GPU.

`NeuralRangeRenderSmoke` is the one to keep green when touching the renderer,
the swapchain or the helper: it renders a *range*, which is what every live
session does, and a range is the only shape of render that exercises preroll -
frames evaluated and never captured. fd9279b broke exactly that while the
whole suite stayed green.

`UpscalingGpuSmoke.exe <clip> 1440 device-loss` additionally removes the D3D12
device mid-frame and checks the renderer's recovery.
CTest still does not establish visual quality. For changes to rendering, timing
or decoding, also run the applicable GPU/media checks from the most recent of
the dated [hardware records](https://github.com/2600th/dlss5-video-player/blob/main/README.md#building-and-contributing).

CMake accepts absolute `DLSS_SDK`, `FFMPEG_STAGED_DIR` and `YOUTUBE_STAGED_DIR`
paths when verified inputs live elsewhere. Keep downloaded binaries out of Git.

## Optical flow

Motion vectors are estimated on NVOFA, the flow engine in every RTX card from
Turing on. Nothing to install: the two interface headers the build needs are in
`external/nvof`, and `nvofapi64.dll` comes from the installed driver and is
loaded by name at run time.

Those headers are committed because NVIDIA licenses each one under MIT in its
own copyright block, scoped to the file - the same grant that lets FFmpeg ship
`nv-codec-headers`. The rest of the Optical Flow SDK is under NVIDIA's licence
agreement and is not here, and is not needed. See
[third-party notices](../THIRD_PARTY.md).

CMake reports which way it went and `NVOF_SDK` overrides the location:

```
-- NVIDIA Optical Flow SDK headers found at .../external/nvof; hardware optical flow enabled.
```

If the headers are missing the build still succeeds and the player falls back
to its own CPU motion estimator, which is also what happens on a pre-Turing
card or an older driver. The `NVOFA ready:` line in `DLSSVideoPlayer.log` tells
you which backend actually came up.

## Add the experimental runtime

Every file in `packaging/runtime-lock.json` is reproducible byte-for-byte from
public releases, and one script fetches them all:

```powershell
./tools/fetch_neural_runtime.ps1
```

It downloads each source archive, checks the archive's SHA-256, extracts the
locked members, checks each against the lock's size and SHA-256, stages them
in `external/runtime`, and finishes with the full `stage_runtime.ps1`
validation (Authenticode state and numeric file version). Files that already
match the lock are not downloaded again, so re-running it costs nothing. The
largest download is the 111 MiB neural runtime archive. The lock's
`provenance` names each source; for reference:

| Locked file | Public source |
| --- | --- |
| `nvngx_dlssnr.dll` | `RankFTW/rhi-repo` release `dlssnr-310.8.SF-v2` |
| `nvngx_dlss.dll` | `RankFTW/rhi-repo` release `dlss-310.9.1` |
| `renodx-dlss5.addon64` | `RankFTW/rhi-repo` release `renodx-dlss5-6.5.3` |
| `sl.*.dll` (8 files) | `RankFTW/rhi-repo` release `streamline-2.13.0.0` (deliberately not 2.14.x - see THIRD_PARTY.md) |
| `dxgi.dll` | `ReShade64.dll` inside `ReShade_Setup_6.8.0_Addon.exe` from reshade.me; the installer is a ZIP container |

Fetching them for your own build is you obtaining the files from their
publishers. It does not resolve the redistribution question for the combined
set, which is why the public `v*` release excludes them; see
[third-party notices](../THIRD_PARTY.md).

With the locked input set in `external/runtime`, stage it only in the
worker's subdirectory:

```powershell
./tools/stage_runtime.ps1 -InputDirectory external/runtime -Destination build-upscaling/Release/neural-runtime
Copy-Item packaging/ReShade.ini,packaging/ReShadePreset.ini build-upscaling/Release/neural-runtime
```

The configuration copy is for initial setup; preserve existing neural settings
before replacing it. Never place the neural `dxgi.dll` beside the main player.
See [runtime setup](DLSS5_SETUP.md) for loading and validation contracts.

`build_windows.bat` combines fetching and validating the locked runtime,
build, tests and runtime staging for maintainers with the FFmpeg inputs in
place. It also refreshes pinned UI/YouTube helpers. Set `DLSS_SDK_DIR` or
`FFMPEG_BIN_DIR` before running it to override their paths. Use the manual
CMake route above for a source build without the experimental runtime.

## Assemble a package

Developer output is not a distributable folder. The assembler takes the build
ctest ran - it checks that the executable's version resource matches `VERSION`
rather than rebuilding - and verifies an explicit file allowlist and manifest:

```powershell
./tools/package_release.ps1 -BuildDirectory build-upscaling -PackageSuffix ''
./tools/verify_package.ps1 -Zip dist/DLSSVideoPlayer-v<version>-win64.zip -PackageSuffix ''
```

Run from the repository, the verifier compares the packaged runtime and helpers
with the pinned SDK and the locks in `packaging/`. The copy that ships in the
package has neither, so from an unpacked folder it runs in package mode: it
reads the version and variant from `PACKAGE_MANIFEST.txt` and holds every file
to the manifest and the allowlist. CI runs that copy from an extracted core zip.

This complete experimental package requires the locked runtime and helpers.
The assembler refuses to replace an existing output; select a new suffix for
another local candidate. The published download uses the
`dlss5-video-player-v<version>-win64.zip` name.

`package_release.bat` wraps the complete package with the default `-upscaling`
suffix. `package_public_release.bat` creates the smaller core package:
application, official SDK DLSS runtime, notices and documentation, without
the neural runtime, FFmpeg or YouTube helpers. CI assembles and verifies this
core variant on every push and pull request. The `v*` tag workflow publishes it
as a **draft** release with the notes and a provenance attestation; the complete
experimental package is assembled locally, attached to the draft as
`dlss5-video-player-v<version>-win64.zip` with its `.sha256`, and only then is
the release published. A publish that failed can be re-run for the same tag
from the Actions tab (`workflow_dispatch` with the tag as input).

Review the applicable third-party terms before distributing any package.
End users run the extracted player, not these build scripts.
