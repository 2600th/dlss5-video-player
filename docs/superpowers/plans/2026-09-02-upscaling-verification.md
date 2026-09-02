# Runtime upscaling verification

Local validation, 2026-09-02. Hardware: NVIDIA RTX 5090 (PCI device 2b85).
RTX 40 was not tested. No tag, commit, push or public release was created.

## Evidence

- Six CTest suites passed after the final startup fix: worker protocol, upscaling
  policy, policy/lifetime, release API, neural cache and player UI (18.12 seconds
  in the final post-package repeat).
  GPU checks run separately because policy tests count system FFmpeg processes.
- Opt-in GPU smoke: 1080p source to 1440p and 2160p, 120/120 native SR
  evaluations each. Measured wall throughput approximately 58.4 and 61.3 fps,
  including first-frame initialization and presentation; not isolated GPU timings.
- Fresh isolated helper job: a two-second Resident Evil excerpt produced 120/120
  captured, verified neural frames at 1920x1080 with upscaling disabled. Independent
  ffprobe frame counting confirmed 120 frames. Inline feature 18 was armed before
  capture, advanced afterward, and contained no later failure markers.
- Live player loaded only system DXGI, driver NGX and root nvngx_dlss.dll, with
  no RenoDX/neural DLL loaded in the player process.
- Paused timestamp 00:21 stayed fixed through SR off/on, original/neural toggles,
  and a 1440p-to-2160p target change. Each feature's UI stayed independent.
- Seek to approximately 4.94 seconds completed with 4K SR active and remained
  paused; resume played to EOF. Run reported 2,734 presented frames and one drop,
  including replay/toggle interactions (not a fresh full-clip benchmark).
- Resident Evil cache SHA-256 remained
  `ABC7F0C6F5BEA151C78BF6A534537775F8E7C4105E27F930B9939A49FA87BB6E`.
- Re-selected the Resident Evil example after EOF: source and neural cache hits,
  no re-render, 4K SR preference restored. Fresh full-clip replay reported 1,799
  presented frames and one drop out of 1,800 (approximately 0.056%).
- Reviewed paused SR-on/off output for visible corruption; this is a qualitative
  sanity check, not proof that approximate video guides improve every scene.

## Fixes discovered during validation

- Hook warm-up delayed native SR until frame two, rejecting candidate frame one.
  Native source-preserving SR now creates immediately; offline NR retains warm-up.
- Helper must enter DXGI on its main thread before Media Foundation/decoder
  startup. Without that ordering the proxy loaded but interception stayed inactive.
- Fixed restart argument bounds, failed-job-assignment cleanup, test exit status,
  and new-layout package allowlists/root checks found by independent code review.
- The first extracted-package check exposed overlapping restart processes:
  ReShade retained the first process's log and wrote actual neural evidence to
  `ReShade.log1`. The helper now returns a configuration-changed exit code;
  its hook-free parent waits for full exit and retries at most once. A focused
  RED/GREEN regression covers retry success and the retry limit. Resetting the
  extracted helper to its shipped INI then passed all 120 verified neural frames.

## Final private package

- `dist/DLSSVideoPlayer-v0.12.0-upscaling-win64.zip`: 318,357,643 bytes.
- SHA-256: `6E2D33A3E954FD83363070ECAC64F10A688D70FE48C52990D3B47ECFCA668D52`.
- Stage and ZIP checks passed with exactly 40 allowlisted files.
- A new extraction of this final ZIP completed its first-run configuration and
  returned 120/120 captured and verified neural frames, with upscaling off,
  feature 18 armed, and no later runtime failure. The rendering job took 7.63
  seconds, excluding the initial process/configuration bootstrap.
- The previous `DLSSVideoPlayer-v0.12.0-win64.zip` remains byte-identical.
  The rejected first candidate was retained under `build-upscaling/rejected-package`
  for diagnostics, outside `dist`.
- `git diff --check` passed; existing unrelated edits remain untouched.

## Boundaries

Frame generation remains unavailable. Motion/depth guides remain approximate;
SR is optional and off by default. Existing third-party runtime redistribution
and signature disclosures remain applicable. No malware scan or security-setting
change was performed. This is a private experimental build.
