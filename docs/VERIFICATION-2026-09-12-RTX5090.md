# RTX 5090 on 0.20.0: the segment boundary that dropped 44 % of playback

Verified 12 September 2026 on the same machine as the
[0.17.0 RTX 5090 record](VERIFICATION-2026-09-10-RTX5090.md), from a user report
of stutter while playing a YouTube trailer with neural rendering on. The render
was healthy in every session; playback was not.

## Environment

| Item | Value | Source |
| --- | --- | --- |
| OS | `Microsoft Windows 11 Pro` / `10.0.26200` | `Win32_OperatingSystem` |
| GPU | `NVIDIA GeForce RTX 5090`, driver `616.64`, `0x2B8510DE` | `nvidia-smi --query-gpu=name,driver_version,pci.device_id` |
| GPU (DXGI) | DXGI driver `32.0.16.1664` | render receipt |
| Repo commit | `36a4872` plus the fix below | `git rev-parse HEAD` |
| Build | Release, `cmake --build --parallel`, zero warnings | build log |
| Runtime | ReShade 6.8.0.2155, RenoDX 4.7, DLSS-NR 310.8.0 | render receipt |
| Antivirus | Bitdefender Antivirus active, Defender real-time protection off | `root/SecurityCenter2 AntiVirusProduct`, `Get-MpComputerStatus` |

Source: the packaged `DLSSVideoPlayer-v0.20.0-win64` release on the desktop,
opened on `https://www.youtube.com/watch?v=kfX9n_G0N2Y` (Cyberpunk 2077: Phantom
Liberty launch trailer, 1920x1080 30 fps, 94.7 s) from Video > Game trailers.
Every session was driven the same way: launch on the URL, post `D` fourteen
seconds later, hold for five minutes, read the log. The fix was measured from a
copy of that same package directory with only the two executables replaced, so
both runs saw the same folder, the same ffmpeg copies and the same scanner.

## Gates

- `ctest --test-dir build-upscaling -C Release`: **13/13 passed**, 49.6 s.
- `MediaGpuSmoke.exe external/ffmpeg/bin …/NeuralWorker.exe smoke-verify`:
  exit 0, `overall=PASS`, `neural_ok=1` on photo, GIF and video with
  `verified_frames` equal to `frames` in each. This gate had failed on stock
  `36a4872` as well, with ten `Export contains an incomplete decoded frame`
  lines: it asserted four bytes per pixel against the NV12 frames 0.19.0's
  sequential decode produces. Fixed alongside, since it is the gate this change
  had to pass.

## The defect

A live session plays the render as two-second segment files. `BuildLivePair`
commits a frame and then calls `PrefetchNextSegment`, which opened the next
segment file inline: `VideoDecoder::Open` starts `ffprobe` as a child process
and waits for its answer, then starts `ffmpeg`. Both ran on the thread that
decodes and presents frames.

`ffprobe.exe` is 98 MB. A process start that an antivirus inspects is not free:

| ffprobe invocation (5 runs, median) | Cost |
| --- | --- |
| desktop package binary on a desktop package segment | **684.2 ms** |
| desktop package binary on a segment under `D:\Github` | 31.8 ms |
| repo binary on a desktop package segment | 44.5 ms |
| repo binary on a repo segment | 32.5 ms |

Only the pair that the scanner watches is slow, which is also the pair a user
has. Playback therefore froze for ~0.7 s every 2 s. Every frame that came due
inside the freeze was more than `LateFrameThreshold` (1.5 frame intervals) late,
so `Tick` dropped it - and each drop also reset the guides and the neural
history.

## Measured

Both rows are the same clip in the same directory with the same antivirus; the
first is the published 0.20.0 binary, the second the same tree plus the fix.

| | 0.20.0 | with the fix |
| --- | --- | --- |
| `Cached playback completed` | `presented=1415 dropped=1110` | **`presented=2838 dropped=1`** |
| Frames dropped | 43.9 % | 0.04 % |
| Segment opens that probed | 47 of 47 | **4 of 51** |
| Open -> ffmpeg started, median | 732 ms | **9 ms** |
| Opens over 50 ms | 47 | 4 |
| Time inside segment opens | 36.5 s | 4.5 s, none of it on the present thread |
| Measured render pace | `6.98285 ms/frame` | `7.18315 ms/frame` |

The four remaining probes are the network source, its acquired local copy and
the first segment - all of them at attach time, behind the buffering panel,
where a probe costs nothing that is visible.

The user's own log of the reported session agrees with the baseline row:
`Active neural session stopped at 64.8667 s; presented=1042 dropped=868` with a
median open of 740.5 ms over 50 opens.

## The fix

`src/VideoDecoder.{h,cpp}`: `OpenKnown` takes the geometry, frame rate, duration
and acceleration-memo key of a sibling file and skips the probe entirely. All
segments of one render come from one encoder at one geometry, so the first one
establishes them for the rest of the session.

`src/SynchronizedPlayback.cpp`: `PrefetchNextSegment` hands the open to a worker
thread (`std::async`, its own `stop_source`) and adopts it when it lands;
`AdoptSegment` waits for that open only if the one-second prefetch lead was not
enough, and cancels a stale one instead of leaving it to block later prefetches.
The boundary itself now moves a ready decoder into place.

`tests/NeuralPrerenderTests.cpp`:
`live_playback_crosses_a_segment_boundary_without_a_gap_or_stall_test` now also
asserts that the second segment was opened on another thread and with the first
segment's parameters. Both assertions fail on the previous behaviour
(`std::launch::deferred` plus an empty `KnownMedia` reproduces it).

## Second defect: sixteen seconds before the picture

The same sessions showed the toggle itself taking that long. Measured on a
local 1080p30 copy of the same clip, from `Neural buffer empty` (the key press)
to `Active neural playback attached` (the picture):

| | 0.20.0 | with the fix, cold | with the fix, warm |
| --- | --- | --- | --- |
| Packaged install, scanned | `14.71 s` | `14.49 s` | **`9.24 s`** |
| Repo build, excluded | `11.43 s` | `10.38 s` | **`5.80 s`** |
| Render start after the press | `+1.40 s` | `+0.81 s` | `+0.79 s` |
| First segment published | `+11.51 s` | `+11.01 s` | `+6.71 s` |
| Dropped frames over the clip | - | `0` | `0` |

"Cold" is the first session on a machine, which still pays for the probe once;
"warm" is every session after it, including after a relaunch. Where it went,
from the worker and ReShade logs of one session (`01:13:16.730` render start,
`01:13:27.421` attached):

| Stage | Cost | Evidence |
| --- | --- | --- |
| Source, runtime and settings digests | 0.63 s | `16.101` -> `16.729`, 226 MB hashed three times |
| Preflight helper: spawn, ReShade, NGX, probe, exit | 4.37 s | render start `16.730` -> ReShade init of the render worker `21.097` |
| Render worker: ReShade, D3D12, add-on | 0.41 s | `ReShade.log 21:097` -> `21:503` |
| NGX init (model cache misses against a `versions\0` that does not exist) | 1.51 s | `21.503` -> `23.015` |
| CreateFeature and first evaluate | 0.63 s | `23.015` -> `23.638` |
| Preroll, 60 frames, encode, mux | 1.78 s | `23.638` -> `25.421` |
| Second segment for the 4 s lead, then attach | 2.00 s | `25.421` -> `27.421` |

The render itself is 60 frames at 7.0 ms = 0.42 s of that. Ten child processes
start before the first picture; on the scanned install each costs ~0.7 s.

Fix, in four parts: the preflight latch remembers a pass and its receipt and
persists it beside the render cache (`NeuralPreflightLatch::LatchedSuccessJson`,
`StoreNeuralPreflightReceipt`, keyed on GPU, driver and runtime digest, with the
reused receipt stamped `reusedVerdict`); `Sha256FileCached` memoises the locked
runtime instead of hashing it three times; the first segment is half a second
(`--first-segment-frames`) and the attach lead scales with the measured pace
(`live_session::StartLead`, 1 s at this GPU's 4.8x); and the two redundant
probes are gone (`VideoDecoder::OpenMetadata` for the job's metadata read, the
player's own `KnownMedia` for the live attach's original).

Tests: `segmented_offline_job_makes_only_the_first_file_short_test` pins 1/2/2
frames for `firstSegmentFrames=1, segmentFrames=2`; the live-session policy test
pins `StartLead`'s thresholds and its ceiling; the latch test pins that a pass
is remembered with its receipt, that a pass without one is not reusable, and
that a different driver, GPU or runtime digest does not match.

## Not the runtime

The pinned neural stack was checked against the report that renodx-dlss5 4.6/4.7
faults on every evaluate from driver 616.64 (roadmap item 0). It does not
explain this symptom and did not occur here: the render receipts of the two
sessions above read `frames=2805/2805 verified=2805 retries=0` and
`2779/2779`, `failure=none`, `lock=ok`, at ~7.0 ms/frame. The player process
loads none of that stack - only `NeuralWorker.exe` does - and the drops
continued for 36 s after the helper had exited and published its cache entry.

## Limits

- One machine, one clip, one geometry, one antivirus product. The 684 ms probe
  is that scanner's cost; the defect is the synchronous open, whose cost on an
  unscanned install is the ~44 ms process start alone, which still lands a
  dropped frame on most boundaries at 60 fps.
- Two defects seen in these sessions are untouched and not diagnosed here:
  a completed render whose publish failed with `The neural video failed final
  cache validation`, and a re-toggle after full coverage that starts a
  zero-length session and reports `The requested render range lies outside the
  source`.
- Only the live-session path on a YouTube source was exercised. Export, local
  files, packaging and visual quality were not.
