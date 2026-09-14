## Summary

Describe the change and why it is needed.

## Validation

- [ ] Release x64 builds successfully
- [ ] CTest suites pass with the required media helpers staged
- [ ] MP4/MKV playback tested as applicable
- [ ] Seek/play/pause tested as applicable
- [ ] No per-frame GPU flush added to normal playback
- [ ] Neural changes name the GPU generation(s) the claim was measured on, say Turing/Ampere were at least not crashed when reachable, and acknowledge `pace unmeasured` where the prior is still 0
- [ ] Quality claims cite source resolution and bitrate (>=1440p high-bitrate master, not a thin 1080p AVC or a 360p stream)
- [ ] Language strings updated when UI text changed
- [ ] No third-party/proprietary runtime binaries included
