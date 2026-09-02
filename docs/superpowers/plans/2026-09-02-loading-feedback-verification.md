# Loading feedback verification

Approved bounded change: animate existing loading and neural-processing UI;
preserve playback, cache policy, renderer defaults and existing packages.

## Implementation

- Blue native GDI spinner and moving indeterminate progress segment during
  source loading, cache checks, initialization and final validation.
- During neural frame processing, show the actual completed-frame percentage,
  frame count, elapsed time and supplied ETA. No time-based fake percentage.
- Window-owned timer: 50 ms while visible and busy; 1 second for elapsed text
  when Windows client-area animations are disabled. No timer during normal
  playback or idle; hidden/minimized windows stop timer-driven repainting.
- Loaded YouTube source changes use a small status-area spinner, without
  replacing the playing video. Cancel hit regions are cleared outside the
  full processing surface.

## Verification

- TDD: new animation/layout/timer tests failed before implementation, then passed.
  A stale Cancel hit-area regression was separately observed failing and fixed.
- Six CTest suites passed before final packaging (18.71 seconds); the targeted
  UI suite passed again after the Cancel fix. The final clean-build repeat
  passed all six suites in 18.59 seconds.
- Real RTX 5090 neural job on a 1080p Resident Evil excerpt: captured successive
  changing spinner and indeterminate-bar states during startup, then actual
  progress at 87%, 557/639 frames, elapsed 00:42, ETA 00:02. Verified neural
  playback opened afterward and the loading indicators disappeared.
- Independent code review found no remaining scoped issues.
- Test-launcher caveat: launching directly from Codex inherited MSIX AppData
  redirection; new staging paths resolved outside the cache's canonical root
  and were correctly rejected. Normal Explorer desktop launch completed the
  job. No cache containment checks or security settings were weakened.

## Private package

- `dist/DLSSVideoPlayer-v0.12.0-upscaling-loading-win64.zip`
- Size: 318,361,195 bytes.
- SHA-256: `CDD20C74B91551DFAE7C40627BB1BF6CADFDBECE12FC2E0EC7D711B9A3FAEDE2`.
- Clean Release rebuild succeeded. Both staged-folder and extracted-ZIP
  verification passed with exactly 40 allowlisted files.
- Previous upscaling ZIP remains unchanged. `git diff --check` passed.

## References

- [Win32 timers](https://learn.microsoft.com/en-us/windows/win32/winmsg/using-timers)
- [Client-area animation preference](https://learn.microsoft.com/en-us/windows/win32/winauto/client-area-animation)

No tag, commit, push, antivirus scan, runtime download or public release.
