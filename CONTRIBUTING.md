# Contributing

_Verified against 0.27.2 (ecd7a10) on 2026-10-02._

Start with [Building and testing](docs/BUILDING.md) for the pinned dependencies
and the canonical `build-upscaling` commands.

## Development guidelines

- Keep Windows x64 / D3D12 behavior working.
- Build with Visual Studio 2022 or newer. Every target compiles at `/W4 /WX`, so a new
  warning fails the build.
- Do not commit NVIDIA SDK checkouts, FFmpeg binaries, ReShade binaries, experimental DLSS 5 DLLs or other third-party runtime packages.
- Keep temporal-resource state transitions explicit and documented.
- Avoid adding a per-frame `WaitGPU()` to the normal playback path.
- When changing seek/audio lifetime code, test repeated forward/backward seeking.
- When changing UI strings, add or update the built-in English defaults.

## Before opening a pull request

For code changes, build Release x64 and run the device-free suites. The quick
tier pull requests run is `cmake --preset ci`, `cmake --build --preset ci
--parallel` and `ctest --preset quick`; before a release, or when touching the
decoder, audio or YouTube child-process code, run every device-free suite
(`ctest --preset portable`, which adds the `slow` tier). FFmpeg and FFprobe must
be staged so the real-media suite runs rather than reporting itself skipped. For rendering, timing or decoding changes
also run the hardware smokes on an RTX card with an audio endpoint
(`ctest -L "gpu|audio"`); for
renderer, swapchain or helper changes `NeuralRangeRenderSmoke` is the one that
catches a dead neural path. The nightly workflow, and every release on its
tag, also run the MSVC code analyzer on the shipped targets and the portable
suites under AddressSanitizer; `cmake --preset analyze` and
`cmake --preset asan` reproduce those builds (see
[Building and testing](docs/BUILDING.md)). Keep automated results
separate from GPU and visual-quality claims.

Check the behavior affected by the change: MP4/MKV playback, play/pause and
seeks, fit/fill, adjustments, debug views, original/neural comparison, cache
reuse or export. Renderer and timing changes also need applicable GPU/media
smoke checks. Verify native evaluations and feature-18 evidence separately;
neither alone establishes visual quality.

For documentation cleanup, check local links, retained screenshot attribution,
and package inputs. Avoid adding another copy of an existing guide or keeping
completed task plans in the maintained documentation; Git history retains
superseded material.

Do not include copyrighted/proprietary runtime packages in pull requests.
