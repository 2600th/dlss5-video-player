# Contributing

Contributions are welcome.

## Development guidelines

- Keep Windows x64 / D3D12 behavior working.
- Build with Visual Studio 2022 and keep `/W4` output clean when possible.
- Do not commit NVIDIA SDK checkouts, FFmpeg binaries, ReShade binaries, experimental DLSS 5 DLLs or other third-party runtime packages.
- Keep temporal-resource state transitions explicit and documented.
- Avoid adding a per-frame `WaitGPU()` to the normal playback path.
- When changing seek/audio lifetime code, test repeated forward/backward seeking.
- When changing UI strings, add or update the built-in English defaults.

## Before opening a pull request

1. Build Release x64.
2. Open at least MP4 and MKV samples.
3. Test play/pause and repeated seeks.
4. Test Fit and Crop aspect modes.
5. Test Image Adjustments while playing and paused.
6. Test debug views `1` through `5`.
7. If you have an RTX GPU, verify native NGX evaluation succeeds.

Do not include copyrighted/proprietary runtime packages in pull requests.
