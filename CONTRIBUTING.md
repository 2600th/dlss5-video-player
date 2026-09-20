# Contributing

_Verified against 0.24.0 (918c0b0) on 2026-09-20._

Start with [Building and testing](docs/BUILDING.md) for the pinned dependencies
and the canonical `build-upscaling` commands.

## Development guidelines

- Keep Windows x64 / D3D12 behavior working.
- Build with Visual Studio 2022 or newer and keep `/W4` output clean when possible.
- Do not commit NVIDIA SDK checkouts, FFmpeg binaries, ReShade binaries, experimental DLSS 5 DLLs or other third-party runtime packages.
- Keep temporal-resource state transitions explicit and documented.
- Avoid adding a per-frame `WaitGPU()` to the normal playback path.
- When changing seek/audio lifetime code, test repeated forward/backward seeking.
- When changing UI strings, add or update the built-in English defaults.

## Before opening a pull request

For code changes, build Release x64 and run the thirteen portable CTest suites
(`ctest -LE gpu`). FFmpeg and FFprobe must be staged so the real-media suite runs
rather than reporting itself skipped. For rendering, timing or decoding changes
also run the `gpu`-labelled smokes on an RTX card (`ctest -L gpu`); for
renderer, swapchain or helper changes `NeuralRangeRenderSmoke` is the one that
catches a dead neural path. Keep automated results
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
